#include "windowmanager.h"
#include "backend.h"
#include "systemtheme.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QWindow>
#include <QTimer>
#include <algorithm>

struct WindowManager::Entry {
    // Reverse destruction order: QML destruction still needs its Backend.
    std::unique_ptr<Backend> backend;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QWindow *window = nullptr;
    bool hadSnapshot = false;
    bool closing = false;
};

WindowManager::WindowManager(const QUrl &qml, QObject *parent)
    : QObject(parent), m_mainQml(qml), m_theme(std::make_unique<SystemTheme>()) {}
WindowManager::~WindowManager() = default;

void WindowManager::retranslate() {
    for (const auto &entry : m_entries) {
        entry->engine->retranslate();
        entry->backend->retranslate();
    }
}

QString WindowManager::documentKey(const QUrl &url) {
    if (!url.isLocalFile() || url.toLocalFile().isEmpty()) return {};
    const QFileInfo info(url.toLocalFile());
    QString path = info.canonicalFilePath();
    if (path.isEmpty()) {
        const QString parent = info.dir().canonicalPath();
        path = parent.isEmpty() ? info.absoluteFilePath() : QDir(parent).filePath(info.fileName());
    }
    path = QDir::cleanPath(path);
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return path;
}

WindowManager::Entry *WindowManager::owner(const QUrl &url) const {
    const QString key = documentKey(url);
    if (key.isEmpty()) return nullptr;
    for (const auto &entry : m_entries) {
        if (entry->closing) continue;
        if (documentKey(entry->backend->fileUrl()) == key) return entry.get();
        // QFileInfo additionally handles platform-native filename comparison.
        const QFileInfo a(url.toLocalFile()), b(entry->backend->fileUrl().toLocalFile());
        if (a.exists() && b.exists() && a == b) return entry.get();
    }
    return nullptr;
}

WindowManager::Entry *WindowManager::create(bool recover) {
    auto entry = std::make_unique<Entry>();
    entry->backend = std::make_unique<Backend>(nullptr, recover);
    Entry *ptr = entry.get();
    if (!ptr->backend->hasRecoveryStorage()) {
        emit errorOccurred(QStringLiteral("No recovery slot is available; close an unused window first."));
        return nullptr;
    }
    ptr->hadSnapshot = ptr->backend->hasRecoverySnapshot();
    ptr->backend->setDarkMode(m_theme->darkMode());
    ptr->backend->setDesktopTextScale(m_theme->textScale());
    connect(m_theme.get(), &SystemTheme::darkModeChanged, ptr->backend.get(), &Backend::setDarkMode);
    connect(m_theme.get(), &SystemTheme::textScaleChanged, ptr->backend.get(), &Backend::setDesktopTextScale);
    ptr->backend->setOpenRouter([this](const QUrl &url) { openFile(url); });
    ptr->backend->setWriteGuard([this, ptr](const QUrl &url) {
        Entry *existing = owner(url);
        if (!existing || existing == ptr) return true;
        activate(existing);
        return false;
    });
    connect(ptr->backend.get(), &Backend::newWindowRequested, this, [this] { newWindow(); });
    ptr->engine = std::make_unique<QQmlApplicationEngine>();
    ptr->engine->rootContext()->setContextProperty(QStringLiteral("backend"), ptr->backend.get());
    ptr->engine->load(m_mainQml);
    if (ptr->engine->rootObjects().isEmpty()) {
        emit errorOccurred(tr("Could not open a new window."));
        return nullptr;
    }
    ptr->window = qobject_cast<QWindow *>(ptr->engine->rootObjects().first());
    if (!ptr->window) return nullptr;
    ptr->backend->setParentWindow(ptr->window);
    connect(ptr->window, SIGNAL(windowCloseAccepted()), this, SLOT(releaseClosedWindow()));
    // Window-local zoom must not change the font of every other window.
    const auto font = [ptr] {
        QFont value = QApplication::font();
        value.setFamily(QStringLiteral("IBM Plex Mono"));
        value.setPointSizeF(qMax(qreal(1), value.pointSizeF()) * ptr->backend->textScale());
        ptr->window->setProperty("font", value);
    };
    font();
    connect(ptr->backend.get(), &Backend::textScaleChanged, ptr->window, font);
    m_entries.push_back(std::move(entry));
    return ptr;
}

void WindowManager::activate(Entry *entry) {
    if (!entry) return;
    if (entry->window->windowStates().testFlag(Qt::WindowMinimized))
        entry->window->setWindowStates(entry->window->windowStates() & ~Qt::WindowMinimized);
    if (!entry->window->isVisible() && entry->backend->windowGeometry().value(QStringLiteral("maximized")).toBool())
        entry->window->showMaximized();
    else entry->window->show();
    entry->window->raise();
    entry->window->requestActivate();
    if (!entry->window->isActive()) entry->window->alert(0);
}

void WindowManager::start(const QStringList &files) {
    Entry *blank = nullptr;
    // Restore orphaned drafts before incoming file requests. Fresh windows never
    // claim snapshots. Duplicate recovered filenames remain separate drafts;
    // only the first owner may write that target until it closes.
    for (int i = 0; i < 100; ++i) {
        Entry *entry = create(true);
        if (!entry) break;
        if (!entry->hadSnapshot) { blank = entry; break; }
        if (entry->backend->modified()) activate(entry);
        else remove(entry); // Malformed snapshot was rejected by Backend.
    }
    m_startupBlank = blank;
    if (files.isEmpty()) {
        // Recovery restores drafts, not the user's request for a blank document.
        if (blank) activate(blank); else newWindow();
    } else {
        for (const QString &file : files) openFile(QUrl::fromLocalFile(file));
        if (m_startupBlank) remove(m_startupBlank);
    }
}

void WindowManager::openFiles(const QStringList &files) {
    if (files.isEmpty()) {
        newWindow();
        return;
    }
    for (const auto &file : files) openFile(QUrl::fromLocalFile(file));
}

QWindow *WindowManager::openFile(const QUrl &url) {
    if (documentKey(url).isEmpty()) {
        emit errorOccurred(QStringLiteral("Only local files can be opened."));
        return nullptr;
    }
    if (auto *existing = owner(url)) { activate(existing); return existing->window; }
    Entry *entry = m_startupBlank;
    if (entry && (entry->backend->modified() || !entry->backend->fileUrl().isEmpty())) entry = nullptr;
    m_startupBlank = nullptr;
    if (!entry) entry = create(false);
    if (!entry) return nullptr;
    entry->backend->openOrStartFile(url);
    activate(entry);
    return entry->window;
}

QWindow *WindowManager::newWindow() {
    auto *entry = create(false);
    activate(entry);
    return entry ? entry->window : nullptr;
}

QList<QWindow *> WindowManager::windows() const {
    QList<QWindow *> result;
    for (const auto &entry : m_entries) result.append(entry->window);
    return result;
}
Backend *WindowManager::backendFor(QWindow *window) const {
    for (const auto &entry : m_entries) if (entry->window == window) return entry->backend.get();
    return nullptr;
}
void WindowManager::remove(Entry *entry) {
    if (m_startupBlank == entry) m_startupBlank = nullptr;
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
        [entry](const auto &candidate) { return candidate.get() == entry; });
    if (it != m_entries.end()) m_entries.erase(it);
}
void WindowManager::releaseClosedWindow() {
    auto *window = qobject_cast<QWindow *>(sender());
    for (const auto &entry : m_entries) {
        if (entry->window == window && !entry->closing) {
            entry->closing = true; // Immediately release ownership; defer QML destruction.
            Entry *closed = entry.get();
            QTimer::singleShot(0, this, [this, closed] {
                remove(closed);
                if (m_entries.empty()) emit noWindowsRemaining();
            });
            break;
        }
    }
}

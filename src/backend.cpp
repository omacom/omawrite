#include "backend.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMimeData>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include "documenttab.h"

QString Backend::normalizedLinkUrl(const QString &clipboardText) {
    QString candidate = clipboardText.trimmed();
    static const QRegularExpression lineBreakRe(QStringLiteral("[\\r\\n]"));
    const int lineBreak = candidate.indexOf(lineBreakRe);
    if (lineBreak >= 0)
        candidate = candidate.left(lineBreak).trimmed();

    if (candidate.isEmpty())
        return {};

    if (candidate.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
        candidate.prepend(QStringLiteral("https://"));

    static const QRegularExpression schemeRe(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    if (!schemeRe.match(candidate).hasMatch())
        return {};

    const QUrl url(candidate);
    if (!url.isValid() || url.scheme().isEmpty())
        return {};

    const QString scheme = url.scheme().toLower();
    const bool webUrl = scheme == QStringLiteral("http")
        || scheme == QStringLiteral("https")
        || scheme == QStringLiteral("ftp");
    if (webUrl && url.host().isEmpty())
        return {};

    if (!webUrl && scheme != QStringLiteral("mailto"))
        return {};

    return url.toString();
}

Backend::Backend(QObject *parent) : QObject(parent) {
    m_sessionTimer.setSingleShot(true);
    m_sessionTimer.setInterval(750);
    connect(&m_sessionTimer, &QTimer::timeout, this, &Backend::writeSessionFile);

    claimSessionSlot();
    loadSessionOrLegacyRecovery();
    if (m_tabs.isEmpty())
        m_tabs.append(new DocumentTab(this));
    m_activeTabIndex = qBound(0, m_activeTabIndex, m_tabs.size() - 1);
    connectActiveTabSignals();

    loadOmarchyTheme();
    watchOmarchyTheme();
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
}

Backend::~Backend() = default;

DocumentTab *Backend::activeTab() const {
    return m_tabs.value(m_activeTabIndex);
}

void Backend::connectActiveTabSignals() {
    DocumentTab *tab = activeTab();
    if (!tab)
        return;
    m_activeTabConnections
        << connect(tab, &DocumentTab::fileUrlChanged, this, &Backend::fileUrlChanged)
        << connect(tab, &DocumentTab::modifiedChanged, this, &Backend::modifiedChanged)
        << connect(tab, &DocumentTab::statusChanged, this, &Backend::statusChanged)
        << connect(tab, &DocumentTab::wordCountChanged, this, &Backend::wordCountChanged)
        << connect(tab, &DocumentTab::closeAfterSave, this, &Backend::closeAfterSave)
        << connect(tab, &DocumentTab::saveDialogRequested, this, &Backend::saveDialogRequested)
        << connect(tab, &DocumentTab::saveSucceeded, this, &Backend::saveSucceeded)
        << connect(tab, &DocumentTab::externalChangeDetected, this,
                   &Backend::externalChangeDetected)
        << connect(tab, &DocumentTab::fileUrlChanged, this, &Backend::scheduleSessionWrite)
        << connect(tab, &DocumentTab::modifiedChanged, this, &Backend::scheduleSessionWrite);
}

void Backend::disconnectActiveTabSignals() {
    for (const QMetaObject::Connection &connection : std::as_const(m_activeTabConnections))
        disconnect(connection);
    m_activeTabConnections.clear();
}

void Backend::setParentWindow(QWindow *window) {
    m_parentWindow = window;
    for (DocumentTab *tab : std::as_const(m_tabs))
        tab->setParentWindow(window);
}

QUrl Backend::fileUrl() const { return activeTab() ? activeTab()->fileUrl() : QUrl(); }
QString Backend::fileName() const {
    return activeTab() ? activeTab()->fileName() : QStringLiteral("Untitled.md");
}
bool Backend::modified() const { return activeTab() && activeTab()->modified(); }
QString Backend::status() const { return activeTab() ? activeTab()->status() : QString(); }
int Backend::wordCount() const { return activeTab() ? activeTab()->wordCount() : 0; }

QVariantList Backend::tabs() const {
    QVariantList result;
    result.reserve(m_tabs.size());
    for (DocumentTab *tab : m_tabs)
        result.append(QVariant::fromValue<QObject *>(tab));
    return result;
}

void Backend::setActiveTabIndex(int index) {
    if (index < 0 || index >= m_tabs.size() || index == m_activeTabIndex)
        return;

    disconnectActiveTabSignals();
    m_activeTabIndex = index;
    connectActiveTabSignals();
    emit activeTabIndexChanged();
    emit fileUrlChanged();
    emit modifiedChanged();
    emit statusChanged();
    emit wordCountChanged();
    scheduleSessionWrite();
}

DocumentTab *Backend::appendTab() {
    disconnectActiveTabSignals();

    auto *tab = new DocumentTab(this);
    tab->setParentWindow(m_parentWindow);
    tab->applyTheme(m_darkMode, m_themeBackground, m_themeForeground, m_themeAccent);
    m_tabs.append(tab);
    m_activeTabIndex = m_tabs.size() - 1;

    connectActiveTabSignals();
    emit tabsChanged();
    emit activeTabIndexChanged();
    emit fileUrlChanged();
    emit modifiedChanged();
    emit statusChanged();
    emit wordCountChanged();
    scheduleSessionWrite();
    return tab;
}

void Backend::removeActiveTab() {
    if (m_tabs.isEmpty())
        return;

    disconnectActiveTabSignals();
    DocumentTab *tab = m_tabs.takeAt(m_activeTabIndex);
    tab->deleteLater();

    if (m_tabs.isEmpty()) {
        emit tabsChanged();
        emit lastTabClosed();
        return;
    }

    m_activeTabIndex = qBound(0, m_activeTabIndex, m_tabs.size() - 1);
    connectActiveTabSignals();
    emit tabsChanged();
    emit activeTabIndexChanged();
    emit fileUrlChanged();
    emit modifiedChanged();
    emit statusChanged();
    emit wordCountChanged();
    scheduleSessionWrite();
}

void Backend::setDarkMode(bool darkMode) {
    if (m_darkMode == darkMode)
        return;

    m_darkMode = darkMode;
    loadOmarchyTheme();
    emit darkModeChanged();
}

void Backend::setTextScale(qreal textScale) {
    if (qFuzzyCompare(m_textScale, textScale))
        return;

    m_textScale = textScale;
    emit textScaleChanged();
}

void Backend::attachDocument(QObject *textDocument) {
    if (activeTab())
        activeTab()->attachDocument(textDocument);
}

void Backend::attachTabDocument(QObject *tabHandle, QObject *textDocument) {
    if (auto *tab = qobject_cast<DocumentTab *>(tabHandle))
        tab->attachDocument(textDocument);
}

void Backend::openDialog() {
    emit openDialogRequested();
}

void Backend::open(const QUrl &url) { if (activeTab()) activeTab()->open(url); }

void Backend::activateOrOpenTab(const QUrl &url) {
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs.at(i)->fileUrl() == url) {
            setActiveTabIndex(i);
            return;
        }
    }
    appendTab()->primeNamedRestore(url, -1, -1, -1);
}

void Backend::newTab() {
    appendTab();
}

void Backend::closeActiveTab() {
    DocumentTab *tab = activeTab();
    if (!tab)
        return;

    tab->flushPendingAutosave();
    if (tab->isNamed() || !tab->modified()) {
        removeActiveTab();
        return;
    }
    emit closeActiveTabRequiresConfirmation();
}

void Backend::discardActiveTab() {
    DocumentTab *tab = activeTab();
    if (!tab)
        return;
    tab->discardRecovery();
    removeActiveTab();
}

void Backend::save() { if (activeTab()) activeTab()->save(); }
void Backend::saveForClose() { if (activeTab()) activeTab()->saveForClose(); }
void Backend::saveAsDialog() { if (activeTab()) activeTab()->saveAsDialog(); }
void Backend::saveAs(const QUrl &url) { if (activeTab()) activeTab()->saveAs(url); }
void Backend::fileDialogCanceled() { if (activeTab()) activeTab()->fileDialogCanceled(); }
void Backend::discardRecovery() { if (activeTab()) activeTab()->discardRecovery(); }
void Backend::reloadFromDisk() { if (activeTab()) activeTab()->reloadFromDisk(); }
void Backend::keepExternalVersion() { if (activeTab()) activeTab()->keepExternalVersion(); }
void Backend::printDocument() { if (activeTab()) activeTab()->printDocument(); }

void Backend::newWindow() {
    const bool started = QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                                 QStringList());
    if (!started && activeTab())
        activeTab()->reportStatus(QStringLiteral("Could not open a new window."));
}

void Backend::flushAllPendingWrites() {
    for (DocumentTab *tab : std::as_const(m_tabs))
        tab->flushPendingAutosave();
    if (m_sessionTimer.isActive()) {
        m_sessionTimer.stop();
        writeSessionFile();
    }
}

QString Backend::clipboardUrl() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    if (!mimeData)
        return {};

    if (mimeData->hasUrls()) {
        const QList<QUrl> urls = mimeData->urls();
        for (const QUrl &url : urls) {
            const QString normalized = normalizedLinkUrl(url.toString());
            if (!normalized.isEmpty())
                return normalized;
        }
    }

    if (!mimeData->hasText())
        return {};

    return normalizedLinkUrl(mimeData->text());
}

QString Backend::clipboardText() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    return mimeData && mimeData->hasText() ? mimeData->text() : QString();
}

bool Backend::editorTextChanged() {
    return activeTab() && activeTab()->editorTextChanged();
}

bool Backend::editorTextChangedForTab(QObject *tabHandle) {
    auto *tab = qobject_cast<DocumentTab *>(tabHandle);
    return tab && tab->editorTextChanged();
}

QVariantList Backend::hiddenRangesAt(int position) const {
    return activeTab() ? activeTab()->hiddenRangesAt(position) : QVariantList();
}

void Backend::setSearchHighlight(const QString &query, int currentMatchStart) {
    if (activeTab())
        activeTab()->setSearchHighlight(query, currentMatchStart);
}

void Backend::updateCursorState(int cursor, int selectionStart, int selectionEnd) {
    if (!activeTab())
        return;
    activeTab()->updateCursorState(cursor, selectionStart, selectionEnd);
    scheduleSessionWrite();
}

QVariantMap Backend::consumePendingCursorRestore() {
    return activeTab() ? activeTab()->consumePendingCursorRestore() : QVariantMap();
}

void Backend::openExternalUrl(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https")
            || scheme == QStringLiteral("mailto"))
        QDesktopServices::openUrl(url);
}

QVariantMap Backend::windowGeometry() const {
    QSettings settings;
    return {{QStringLiteral("x"), settings.value(QStringLiteral("window/x"), -1)},
            {QStringLiteral("y"), settings.value(QStringLiteral("window/y"), -1)},
            {QStringLiteral("width"), settings.value(QStringLiteral("window/width"), 1280)},
            {QStringLiteral("height"), settings.value(QStringLiteral("window/height"), 820)},
            {QStringLiteral("maximized"), settings.value(QStringLiteral("window/maximized"), false)}};
}

void Backend::saveWindowGeometry(int x, int y, int width, int height, bool maximized) {
    QSettings settings;
    if (!maximized) {
        settings.setValue(QStringLiteral("window/x"), x);
        settings.setValue(QStringLiteral("window/y"), y);
        settings.setValue(QStringLiteral("window/width"), width);
        settings.setValue(QStringLiteral("window/height"), height);
    }
    settings.setValue(QStringLiteral("window/maximized"), maximized);
}

void Backend::loadOmarchyTheme() {
    m_themeBackground = m_darkMode ? QStringLiteral("#101010") : QStringLiteral("#ffffff");
    m_themeForeground = m_darkMode ? QStringLiteral("#eeeeee") : QStringLiteral("#222324");
    m_themeAccent = m_darkMode ? QStringLiteral("#5584aa") : QStringLiteral("#2077b2");
    m_themeSelection = m_darkMode ? QStringLiteral("#186a9a") : QStringLiteral("#2077b2");

    const QString colorsPath = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");
    QString themeMode;
    QFile file(colorsPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;

            const int equals = line.indexOf(QLatin1Char('='));
            if (equals < 0)
                continue;

            const QString key = line.left(equals).trimmed();
            QString value = line.mid(equals + 1).trimmed();
            if (value.size() >= 2
                    && ((value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
                        || (value.front() == QLatin1Char('\'') && value.back() == QLatin1Char('\''))))
                value = value.mid(1, value.size() - 2);

            if (key == QStringLiteral("mode"))
                themeMode = value;
            else if (key == QStringLiteral("background"))
                m_themeBackground = value;
            else if (key == QStringLiteral("foreground"))
                m_themeForeground = value;
            else if (key == QStringLiteral("accent"))
                m_themeAccent = value;
            else if (key == QStringLiteral("selection"))
                m_themeSelection = value;
        }
    }

    bool themeModeKnown = false;
    bool themeIsDark = m_darkMode;
    if (themeMode == QStringLiteral("dark")) {
        themeIsDark = true;
        themeModeKnown = true;
    } else if (themeMode == QStringLiteral("light")) {
        themeIsDark = false;
        themeModeKnown = true;
    } else {
        const QColor background(m_themeBackground);
        if (background.isValid()) {
            const double luminance = 0.299 * background.redF()
                + 0.587 * background.greenF() + 0.114 * background.blueF();
            themeIsDark = luminance < 0.5;
            themeModeKnown = true;
        }
    }
    if (themeModeKnown && themeIsDark != m_darkMode) {
        m_darkMode = themeIsDark;
        emit darkModeChanged();
    }

    for (DocumentTab *tab : std::as_const(m_tabs))
        tab->applyTheme(m_darkMode, m_themeBackground, m_themeForeground, m_themeAccent);

    emit themeColorsChanged();
}

void Backend::watchOmarchyTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const QString currentDir = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current");
    const QString themeDir = currentDir + QStringLiteral("/theme");
    const QString colorsPath = themeDir + QStringLiteral("/colors.toml");

    if (QDir(currentDir).exists())
        m_themeWatcher.addPath(currentDir);
    if (QDir(themeDir).exists())
        m_themeWatcher.addPath(themeDir);
    if (QFile::exists(colorsPath))
        m_themeWatcher.addPath(colorsPath);
}

void Backend::claimSessionSlot() {
    const QString stateDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(stateDirectory);
    // Same two-pass, orphan-preferring claim as DocumentTab's own untitled-N
    // slots: a session left behind by a crashed process is recovered even if
    // another window/process exited normally in the meantime.
    for (int pass = 0; pass < 2 && !m_sessionLock; ++pass) {
        for (int slot = 0; slot < 100; ++slot) {
            const QString base = QDir(stateDirectory).filePath(
                QStringLiteral("session-%1").arg(slot));
            const bool snapshotExists = QFileInfo::exists(base + QStringLiteral(".json"));
            if ((pass == 0) != snapshotExists)
                continue;
            auto lock = std::make_unique<QLockFile>(base + QStringLiteral(".lock"));
            if (lock->tryLock()) {
                m_sessionPath = base + QStringLiteral(".json");
                m_sessionLock = std::move(lock);
                break;
            }
        }
    }
}

void Backend::loadSessionOrLegacyRecovery() {
    const QString stateDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    QFile sessionFile(m_sessionPath);
    if (sessionFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument json = QJsonDocument::fromJson(sessionFile.readAll());
        const QJsonObject root = json.object();
        const QJsonArray tabs = root.value(QStringLiteral("tabs")).toArray();
        for (const QJsonValue &value : tabs) {
            const QJsonObject tab = value.toObject();
            auto *documentTab = new DocumentTab(this);
            const int cursor = tab.value(QStringLiteral("cursor")).toInt(-1);
            const int selectionStart = tab.value(QStringLiteral("selectionStart")).toInt(-1);
            const int selectionEnd = tab.value(QStringLiteral("selectionEnd")).toInt(-1);
            if (tab.value(QStringLiteral("kind")).toString() == QStringLiteral("named")) {
                documentTab->primeNamedRestore(
                    QUrl(tab.value(QStringLiteral("fileUrl")).toString()), cursor,
                    selectionStart, selectionEnd);
            } else {
                const QString slot = tab.value(QStringLiteral("slot")).toString();
                QFile untitledFile(QDir(stateDirectory).filePath(slot + QStringLiteral(".json")));
                QString text;
                if (!slot.isEmpty() && untitledFile.open(QIODevice::ReadOnly)) {
                    text = QJsonDocument::fromJson(untitledFile.readAll())
                               .object().value(QStringLiteral("text")).toString();
                }
                documentTab->primeUntitledRestore(text, cursor, selectionStart, selectionEnd);
            }
            m_tabs.append(documentTab);
        }
        if (!tabs.isEmpty())
            m_activeTabIndex = root.value(QStringLiteral("activeIndex")).toInt(0);
    }

    // Import any leftover recovery-*.json from before tabs/sessions existed,
    // as additional tabs -- regardless of whether a session was also found
    // above, since a legacy file's slot number is coincidental and must not
    // cause it to be silently skipped. Only an ORPHANED snapshot (whose lock
    // nobody currently holds) is imported: one still held by a live process
    // running the pre-tabs single-slot scheme is left alone, since that
    // process is still actively managing it.
    const QDir dir(stateDirectory);
    const QStringList legacyFiles =
        dir.entryList(QStringList() << QStringLiteral("recovery-*.json"), QDir::Files);
    for (const QString &legacyName : legacyFiles) {
        const QString legacyBase = legacyName.chopped(5); // strip ".json"
        const QString legacyLockPath = dir.filePath(legacyBase + QStringLiteral(".lock"));
        QLockFile legacyLock(legacyLockPath);
        if (!legacyLock.tryLock())
            continue;

        QFile legacy(dir.filePath(legacyName));
        if (!legacy.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject legacyObject = QJsonDocument::fromJson(legacy.readAll()).object();
        legacy.close();
        QFile::remove(dir.filePath(legacyName));
        legacyLock.unlock();
        QFile::remove(legacyLockPath);

        const QUrl fileUrl(legacyObject.value(QStringLiteral("fileUrl")).toString());
        const QString text = legacyObject.value(QStringLiteral("text")).toString();
        auto *documentTab = new DocumentTab(this);
        if (fileUrl.isEmpty())
            documentTab->primeUntitledRestore(text, -1, -1, -1);
        else
            documentTab->primeNamedRestoreWithPendingEdits(fileUrl, text);
        m_tabs.append(documentTab);
    }
}

void Backend::scheduleSessionWrite() {
    m_sessionTimer.start();
}

void Backend::writeSessionFile() {
    if (m_sessionPath.isEmpty())
        return;

    QJsonArray tabs;
    for (DocumentTab *tab : std::as_const(m_tabs)) {
        QJsonObject entry;
        if (tab->isNamed()) {
            entry[QStringLiteral("kind")] = QStringLiteral("named");
            entry[QStringLiteral("fileUrl")] = tab->fileUrl().toString();
        } else {
            entry[QStringLiteral("kind")] = QStringLiteral("untitled");
            entry[QStringLiteral("slot")] = tab->untitledSlotName();
        }
        entry[QStringLiteral("cursor")] = tab->cursorPosition();
        entry[QStringLiteral("selectionStart")] = tab->selectionStart();
        entry[QStringLiteral("selectionEnd")] = tab->selectionEnd();
        tabs.append(entry);
    }

    const QJsonObject root{{QStringLiteral("version"), 1},
                           {QStringLiteral("activeIndex"), m_activeTabIndex},
                           {QStringLiteral("tabs"), tabs}};

    QDir().mkpath(QFileInfo(m_sessionPath).absolutePath());
    QSaveFile file(m_sessionPath);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

QString Backend::suggestedFileName(const QString &text) {
    QString name = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\x00-\\x1f\\x7f]")),
                 QStringLiteral("-"));
    name = name.left(120).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("Untitled");
    if (!name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        name += QStringLiteral(".md");
    return name;
}

int Backend::countWords(const QString &text) {
    static const QRegularExpression wordRe(
        QStringLiteral("[\\p{L}\\p{N}]+(?:['-][\\p{L}\\p{N}]+)*"));
    int count = 0;
    QRegularExpressionMatchIterator it = wordRe.globalMatch(text);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

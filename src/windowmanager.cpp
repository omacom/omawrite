#include "windowmanager.h"

#include <QFileOpenEvent>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWindow>
#include <utility>

#include "backend.h"
#include "systemtheme.h"

namespace {
// How far each new window steps from the one in front, so it never lands
// exactly on top of it.
constexpr int cascadeOffset = 24;
}

WindowManager::WindowManager(SystemTheme *systemTheme, QObject *parent)
    : QObject(parent),
      m_systemTheme(systemTheme),
      m_component(&m_engine, QUrl(QStringLiteral("qrc:/Main.qml"))) {
    connect(&m_engine, &QQmlApplicationEngine::warnings, this,
            [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qWarning().noquote() << warning.toString();
    });
}

WindowManager::~WindowManager() {
    // Quitting can skip the queued releases; tear down whatever is left.
    for (const Window &window : std::exchange(m_windows, {}))
        destroyWindow(window);
}

QQuickWindow *WindowManager::openWindow(const QUrl &fileUrl) {
    const Window *front = frontWindow();
    const QPoint cascadeFrom = front && front->window
        ? front->window->position() + QPoint(cascadeOffset, cascadeOffset)
        : QPoint();

    auto *backend = new Backend(this);
    backend->setDarkMode(m_systemTheme->darkMode());
    backend->setTextScale(m_systemTheme->textScale());
    connect(m_systemTheme, &SystemTheme::darkModeChanged, backend, &Backend::setDarkMode);
    connect(m_systemTheme, &SystemTheme::textScaleChanged, backend, &Backend::setTextScale);
    connect(backend, &Backend::newWindowRequested, this, [this]() { openWindow(); });

    auto *context = new QQmlContext(m_engine.rootContext(), this);
    context->setContextProperty(QStringLiteral("backend"), backend);
    auto *window = qobject_cast<QQuickWindow *>(m_component.create(context));
    if (!window) {
        qCritical() << "Could not load the Omawrite interface:" << m_component.errors();
        delete context;
        delete backend;
        return nullptr;
    }
    backend->setParentWindow(window);
    if (!cascadeFrom.isNull())
        window->setPosition(cascadeFrom);
    m_windows.append({window, context, backend});

    if (!fileUrl.isEmpty()) {
        // A window that just recovered a crashed draft keeps it, and the file
        // gets a window of its own. Each Backend claims a different recovery
        // slot, so this ends once a window comes up clean.
        if (backend->modified())
            openWindow(fileUrl);
        else
            backend->open(fileUrl);
    }

    // Files Finder handed over before the first window existed.
    for (const QUrl &pending : std::exchange(m_pendingFiles, {}))
        openFile(pending);

    return window;
}

bool WindowManager::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::FileOpen) {
        const QUrl url = static_cast<QFileOpenEvent *>(event)->url();
        if (m_windows.isEmpty())
            m_pendingFiles.append(url);
        else
            openFile(url);
        return true;
    }

    if (event->type() == QEvent::Close) {
        // onClosing in Main.qml may refuse the close for unsaved changes, and
        // only a close that went through leaves the window hidden, so look once
        // the event has run its course.
        const QPointer<QQuickWindow> window = qobject_cast<QQuickWindow *>(watched);
        if (window) {
            QMetaObject::invokeMethod(this, [this, window]() {
                if (window && !window->isVisible())
                    release(window);
            }, Qt::QueuedConnection);
        }
    }

    return QObject::eventFilter(watched, event);
}

void WindowManager::openFile(const QUrl &fileUrl) {
    if (!fileUrl.isLocalFile())
        return;

    for (const Window &window : std::as_const(m_windows)) {
        if (window.backend->fileUrl() == fileUrl && window.window) {
            window.window->raise();
            window.window->requestActivate();
            return;
        }
    }

    // Reuse the front window while it is still an untouched Untitled.md, so a
    // launch from Finder opens the file in the first window rather than beside it.
    const Window *front = frontWindow();
    if (front && front->window && front->backend->fileUrl().isEmpty()
            && !front->backend->modified()) {
        front->backend->open(fileUrl);
        front->window->raise();
        front->window->requestActivate();
        return;
    }

    openWindow(fileUrl);
}

void WindowManager::release(QQuickWindow *window) {
    for (qsizetype i = 0; i < m_windows.size(); ++i) {
        if (m_windows.at(i).window == window) {
            destroyWindow(m_windows.takeAt(i));
            return;
        }
    }
}

void WindowManager::destroyWindow(const Window &window) {
    // The window goes first so Component.onDestruction can still save its
    // geometry through the Backend; the Backend goes last and frees its
    // recovery slot.
    delete window.window.data();
    delete window.context;
    delete window.backend;
}

const WindowManager::Window *WindowManager::frontWindow() const {
    if (m_windows.isEmpty())
        return nullptr;

    const QWindow *focused = QGuiApplication::focusWindow();
    for (const Window &window : m_windows) {
        if (window.window && window.window == focused)
            return &window;
    }
    return &m_windows.constLast();
}

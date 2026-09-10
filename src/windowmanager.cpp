#include "windowmanager.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTimer>
#include <QWindow>

#include "backend.h"
#include "workspacesession.h"

WindowManager::WindowManager(WorkspaceSession *workspaceSession, QQmlEngine *engine,
                             const QUrl &qmlUrl, QObject *parent)
    : QObject(parent), m_workspaceSession(workspaceSession), m_engine(engine), m_qmlUrl(qmlUrl) {}

WindowManager::~WindowManager() {
    for (const WritingWindow &window : m_windows) {
        delete window.root;
        delete window.context;
        delete window.backend;
    }
}

Backend *WindowManager::createWindow() {
    if (!m_workspaceSession || !m_engine)
        return nullptr;

    const QString windowId = m_workspaceSession->createWindow(-1, -1, 1280, 820, false);
    m_workspaceSession->createTab(windowId, QUrl(), QString(), 0, 0, 0, false);
    Backend *backend = createWindow(windowId);
    if (!backend)
        return nullptr;

    m_workspaceSession->saveNow();
    return backend;
}

int WindowManager::restoreWindows() {
    if (!m_workspaceSession || !m_engine)
        return 0;

    for (const QVariant &value : m_workspaceSession->windows()) {
        if (!createWindow(value.toMap().value(QStringLiteral("id")).toString()))
            return 0;
    }
    return m_windows.size();
}

Backend *WindowManager::createWindow(const QString &windowId) {
    if (windowId.isEmpty())
        return nullptr;

    auto *backend = new Backend(m_workspaceSession, windowId);
    backend->setDarkMode(m_darkMode);
    backend->setTextScale(m_textScale);
    auto *context = new QQmlContext(m_engine->rootContext());
    context->setContextProperty(QStringLiteral("backend"), backend);
    QQmlComponent component(m_engine, m_qmlUrl, backend);
    QObject *window = component.create(context);
    if (!window) {
        delete context;
        delete backend;
        return nullptr;
    }
    backend->setParentWindow(qobject_cast<QWindow *>(window));
    connect(backend, &Backend::newWindowRequested, this, [this]() { createWindow(); });
    connect(backend, &Backend::openTabRequested, this, &WindowManager::activateTab);
    connect(backend, &Backend::windowEmptied, this, [this, windowId]() {
        QTimer::singleShot(0, this, [this, windowId]() { closeWindow(windowId); });
    });
    m_windows.append({windowId, backend, context, window});
    return backend;
}

int WindowManager::windowCount() const {
    return m_windows.size();
}

Backend *WindowManager::primaryBackend() const {
    return m_windows.isEmpty() ? nullptr : m_windows.constFirst().backend;
}

void WindowManager::setDarkMode(bool darkMode) {
    m_darkMode = darkMode;
    for (const WritingWindow &window : m_windows)
        window.backend->setDarkMode(darkMode);
}

void WindowManager::setTextScale(qreal textScale) {
    m_textScale = textScale;
    for (const WritingWindow &window : m_windows)
        window.backend->setTextScale(textScale);
}

void WindowManager::activateTab(const QString &tabId) {
    const QString windowId = m_workspaceSession->windowIdForTab(tabId);
    for (const WritingWindow &window : m_windows) {
        if (window.id != windowId)
            continue;
        window.backend->selectBuffer(tabId);
        auto *nativeWindow = qobject_cast<QWindow *>(window.root);
        if (nativeWindow) {
            nativeWindow->raise();
            nativeWindow->requestActivate();
        }
        return;
    }
}

void WindowManager::closeWindow(const QString &windowId) {
    for (int index = 0; index < m_windows.size(); ++index) {
        WritingWindow window = m_windows.at(index);
        if (window.id != windowId)
            continue;
        m_windows.removeAt(index);
        m_workspaceSession->removeWindow(window.id);
        m_workspaceSession->saveNow();
        window.root->deleteLater();
        window.context->deleteLater();
        window.backend->deleteLater();
        return;
    }
}

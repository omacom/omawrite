#include "windowmanager.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
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

    auto *backend = new Backend(m_workspaceSession, windowId);
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
    m_windows.append({backend, context, window});
    m_workspaceSession->saveNow();
    return backend;
}

int WindowManager::windowCount() const {
    return m_windows.size();
}

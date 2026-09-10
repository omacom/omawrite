#pragma once

#include <QObject>
#include <QList>
#include <QUrl>

class Backend;
class QQmlContext;
class QQmlEngine;
class WorkspaceSession;

class WindowManager : public QObject {
    Q_OBJECT

public:
    WindowManager(WorkspaceSession *workspaceSession, QQmlEngine *engine, const QUrl &qmlUrl,
                  QObject *parent = nullptr);
    ~WindowManager() override;

    Backend *createWindow();
    int windowCount() const;

private:
    struct WritingWindow {
        Backend *backend;
        QQmlContext *context;
        QObject *root;
    };

    WorkspaceSession *m_workspaceSession;
    QQmlEngine *m_engine;
    QUrl m_qmlUrl;
    QList<WritingWindow> m_windows;
};

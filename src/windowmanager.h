#pragma once

#include <QObject>
#include <QFileSystemWatcher>
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
    int restoreWindows();
    int windowCount() const;
    Backend *primaryBackend() const;
    void setDarkMode(bool darkMode);
    void setTextScale(qreal textScale);

private:
    struct WritingWindow {
        QString id;
        Backend *backend;
        QQmlContext *context;
        QObject *root;
    };

    Backend *createWindow(const QString &windowId);
    void activateTab(const QString &tabId);
    void closeWindow(const QString &windowId);
    void syncFileWatcher();
    void handleFileChange(const QString &path);

    WorkspaceSession *m_workspaceSession;
    QQmlEngine *m_engine;
    QUrl m_qmlUrl;
    QList<WritingWindow> m_windows;
    bool m_darkMode = true;
    qreal m_textScale = 1.0;
    QFileSystemWatcher m_fileWatcher;
};

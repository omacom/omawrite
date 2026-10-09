#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QUrl>

class Backend;
class QQmlContext;
class SystemTheme;

// macOS expects one process to hold every window of a document app, so Finder,
// the Dock and Cmd+Q all reach the same instance. Each window keeps a Backend of
// its own, so recovery, file watching and the unsaved prompt stay per document,
// just as they are per process on Linux.
class WindowManager : public QObject {
    Q_OBJECT

public:
    explicit WindowManager(SystemTheme *systemTheme, QObject *parent = nullptr);
    ~WindowManager() override;

    QQuickWindow *openWindow(const QUrl &fileUrl = QUrl());
    int windowCount() const { return m_windows.size(); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Window {
        QPointer<QQuickWindow> window;
        QQmlContext *context = nullptr;
        Backend *backend = nullptr;
    };

    void openFile(const QUrl &fileUrl);
    void release(QQuickWindow *window);
    void destroyWindow(const Window &window);
    const Window *frontWindow() const;

    SystemTheme *m_systemTheme;
    QQmlApplicationEngine m_engine;
    QQmlComponent m_component;
    QList<Window> m_windows;
    QList<QUrl> m_pendingFiles;
};

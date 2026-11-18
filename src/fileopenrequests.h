#pragma once
#include <QObject>
#include <QStringList>
#include <QTimer>

// Native launch events can arrive after the initial CLI request was forwarded.
// Keep a secondary's event loop alive through a quiet interval, forwarding
// every delivered batch; there is no resident/background service or polling.
class FileOpenRequests : public QObject {
    Q_OBJECT
public:
    explicit FileOpenRequests(QObject *application, QObject *parent = nullptr);
    QStringList takePending();
    void activate(bool forwarding);
    void requestReopen(); // A native launch request, never ordinary activation.
signals:
    void filesRequested(const QStringList &files);
    void forwardingIdle();
protected:
    bool eventFilter(QObject *, QEvent *) override;
private:
    QStringList m_pending;
    QTimer m_dispatch;
    QTimer m_idle;
    bool m_ready = false;
    int m_reopens = 0;
};

#ifdef Q_OS_MACOS
// Install only in the application, not for every FileOpenRequests test fixture.
void installMacReopenHandler(FileOpenRequests *requests);
#endif

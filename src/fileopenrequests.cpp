#include "fileopenrequests.h"
#include <QFileOpenEvent>
#include <QFileInfo>
#include <utility>

FileOpenRequests::FileOpenRequests(QObject *application, QObject *parent) : QObject(parent) {
    application->installEventFilter(this);
    m_dispatch.setSingleShot(true);
    m_dispatch.setInterval(0);
    m_idle.setSingleShot(true);
    m_idle.setInterval(400);
    connect(&m_dispatch, &QTimer::timeout, this, [this] {
        const auto files = takePending();
        const int reopens = std::exchange(m_reopens, 0);
        if (!files.isEmpty()) emit filesRequested(files);
        for (int i = 0; i < reopens; ++i) emit filesRequested({});
    });
    connect(&m_idle, &QTimer::timeout, this, &FileOpenRequests::forwardingIdle);
}
QStringList FileOpenRequests::takePending() {
    QStringList files;
    files.swap(m_pending);
    return files;
}
void FileOpenRequests::activate(bool forwarding) {
    m_ready = true;
    if (forwarding) m_idle.start(); else m_idle.stop();
    if (!m_pending.isEmpty() || m_reopens) m_dispatch.start();
}
void FileOpenRequests::requestReopen() {
    ++m_reopens;
    if (m_ready) m_dispatch.start();
    if (m_idle.isActive()) m_idle.start();
}
bool FileOpenRequests::eventFilter(QObject *, QEvent *event) {
    if (event->type() != QEvent::FileOpen) return false;
    const auto *file = static_cast<QFileOpenEvent *>(event);
    if (!file->url().isLocalFile()) return false;
    m_pending.append(QFileInfo(file->url().toLocalFile()).absoluteFilePath());
    if (m_ready) m_dispatch.start();
    if (m_idle.isActive()) m_idle.start();
    event->accept();
    return true;
}

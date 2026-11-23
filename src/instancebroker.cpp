#include "instancebroker.h"
#include "profile.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QtEndian>
#include <algorithm>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
constexpr int MaxPayload = 256 * 1024;
constexpr int MaxPaths = 64;
constexpr int MaxClients = 16;
constexpr int TimeoutMs = 3000;

bool validPaths(const QStringList &paths)
{
    if (paths.size() > MaxPaths)
        return false;
    for (const QString &path : paths) {
        if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path)
            || path.contains(QStringLiteral("://")))
            return false;
#ifdef Q_OS_WIN
        // UNC/device paths may access remote resources; only drive-local paths.
        if (path.size() < 3 || !path.at(0).isLetter() || path.at(1) != QLatin1Char(':')
            || (path.at(2) != QLatin1Char('/') && path.at(2) != QLatin1Char('\\')))
            return false;
#endif
    }
    return true;
}

bool decode(const QByteArray &payload, QStringList &paths)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const auto object = document.object();
    if (object.size() != 2 || object.value(QStringLiteral("version")) != QJsonValue(1)
        || !object.value(QStringLiteral("paths")).isArray())
        return false;
    const auto array = object.value(QStringLiteral("paths")).toArray();
    if (array.size() > MaxPaths)
        return false;
    for (const auto &value : array) {
        if (!value.isString())
            return false;
        paths.append(value.toString());
    }
    return validPaths(paths);
}

QByteArray frame(const QByteArray &payload)
{
    QByteArray result(4, '\0');
    qToBigEndian<quint32>(quint32(payload.size()), result.data());
    result.append(payload);
    return result;
}
}

QString InstanceBroker::prepareFallbackDirectory(const QString &base)
{
#ifdef Q_OS_UNIX
    // Validate the predictable parent itself, not merely its private child.
    // Never chmod/follow an attacker-created directory or symlink.
    const QByteArray path = QFile::encodeName(base);
    ::mkdir(path.constData(), 0700);
    struct stat status;
    if (::lstat(path.constData(), &status) != 0 || !S_ISDIR(status.st_mode)
        || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0)
        return {};
#else
    if (QFileInfo(base).isSymLink() || !QDir().mkpath(base)) return {};
#endif
    return QDir(base).filePath(QStringLiteral("ipc"));
}

QString InstanceBroker::defaultDirectory()
{
    const QString base = Profile::runtimeLocation();
    if (!base.isEmpty()) return QDir(base).filePath(Profile::ipcDirectoryName());
    const auto user = QCryptographicHash::hash(QDir::homePath().toUtf8(),
                                              QCryptographicHash::Sha256).toHex().left(16);
    return prepareFallbackDirectory(QDir::tempPath() + QStringLiteral("/ow-") + QString::fromLatin1(user));
}

InstanceBroker::InstanceBroker(QObject *parent)
    : InstanceBroker(defaultDirectory(), QStringLiteral("requests"), parent)
{
}

InstanceBroker::InstanceBroker(const QString &runtimeDirectory, const QString &endpointName,
                               QObject *parent)
    : QObject(parent), m_runtimeDirectory(QDir::cleanPath(runtimeDirectory)), m_endpointName(endpointName)
{
}

InstanceBroker::~InstanceBroker()
{
    // Close before unlocking, but defer object destruction: a request handler
    // may destroy the broker from inside a socket's readyRead notification.
    if (m_server) {
        m_server->close();
        m_server->setParent(nullptr);
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_lock.reset();
}

QString InstanceBroker::endpointAddress(const QString &directory, const QString &name)
{
#ifdef Q_OS_WIN
    // Named-pipe names are not filesystem paths: avoid separators, drive
    // prefixes and the pipe-name length limit even with a long user profile.
    const auto hash = QCryptographicHash::hash((QDir::cleanPath(directory).toCaseFolded()
                       + QLatin1Char('/') + name).toUtf8(), QCryptographicHash::Sha256).toHex();
    return Profile::pipePrefix() + QString::fromLatin1(hash);
#else
    return QDir(directory).filePath(name);
#endif
}

QString InstanceBroker::errorString() const
{
    return m_error;
}

InstanceBroker::StartResult InstanceBroker::start(const QStringList &absoluteFilePaths)
{
    if (m_started) {
        m_error = QStringLiteral("InstanceBroker can only be started once.");
        return Failed;
    }
    m_started = true;
    if (!validPaths(absoluteFilePaths)) {
        m_error = QStringLiteral("Expected at most 64 absolute local file paths.");
        return Failed;
    }
    QJsonArray paths;
    for (const QString &path : absoluteFilePaths)
        paths.append(path);
    const auto payload = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                  {QStringLiteral("paths"), paths}})
                             .toJson(QJsonDocument::Compact);
    if (payload.size() > MaxPayload) {
        m_error = QStringLiteral("Instance request exceeds 256 KiB.");
        return Failed;
    }
    if (!QDir::isAbsolutePath(m_runtimeDirectory) || m_runtimeDirectory.contains(QChar::Null)
        || m_endpointName.isEmpty()
        || m_endpointName == QStringLiteral(".") || m_endpointName == QStringLiteral("..")
        || m_endpointName.contains(QLatin1Char('/')) || m_endpointName.contains(QLatin1Char('\\'))
        || m_endpointName.contains(QChar::Null)) {
        m_error = QStringLiteral("Invalid instance runtime directory or endpoint name.");
        return Failed;
    }
    const QDir directory(m_runtimeDirectory);
#ifdef Q_OS_UNIX
    // Atomic private creation; never chmod or follow a pre-created symlink.
    const QByteArray nativeDirectory = QFile::encodeName(m_runtimeDirectory);
    if (!directory.exists()) {
        if (!QDir().mkpath(QFileInfo(m_runtimeDirectory).absolutePath())) {
            m_error = QStringLiteral("Cannot create instance runtime parent directory.");
            return Failed;
        }
        ::mkdir(nativeDirectory.constData(), 0700);
    }
    struct stat status;
    if (::lstat(nativeDirectory.constData(), &status) != 0 || !S_ISDIR(status.st_mode)
        || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) {
        m_error = QStringLiteral("Instance runtime directory must be private and owned by this user.");
        return Failed;
    }
#else
    if (QFileInfo(m_runtimeDirectory).isSymLink() || !QDir().mkpath(m_runtimeDirectory)) {
        m_error = QStringLiteral("Cannot create instance runtime directory.");
        return Failed;
    }
#endif
    const QString endpoint = endpointAddress(m_runtimeDirectory, m_endpointName);
    m_lock = std::make_unique<QLockFile>(directory.filePath(m_endpointName + QStringLiteral(".lock")));
    // Disable age-based expiry: a live but busy primary must never lose its lock.
    m_lock->setStaleLockTime(0);
    QElapsedTimer deadline;
    deadline.start();
    // Bounded startup grace also lets a simultaneous winner finish listen().
    if (!m_lock->tryLock(1000)) {
        if (m_lock->error() != QLockFile::LockFailedError) {
            m_error = QStringLiteral("Cannot acquire instance lock.");
            return Failed;
        }
        return forward(payload, std::max(1, TimeoutMs - int(deadline.elapsed())));
    }

    // Never unlink anything unless this process owns the authoritative lock.
    QLocalServer::removeServer(endpoint);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    m_server->setMaxPendingConnections(MaxClients);
    connect(m_server, &QLocalServer::newConnection, this, &InstanceBroker::acceptConnections);
    if (!m_server->listen(endpoint)) {
        m_error = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        m_lock.reset();
        return Failed;
    }
    emit openRequested(absoluteFilePaths);
    return Primary;
}

InstanceBroker::StartResult InstanceBroker::forward(const QByteArray &payload, int timeoutMs)
{
    QElapsedTimer deadline;
    deadline.start();
    const auto remaining = [&] { return std::max(0, timeoutMs - int(deadline.elapsed())); };
    QLocalSocket socket;
    socket.setReadBufferSize(2);
    socket.connectToServer(endpointAddress(m_runtimeDirectory, m_endpointName));
    if (!socket.waitForConnected(remaining())) {
        m_error = QStringLiteral("Primary instance is unavailable: ") + socket.errorString();
        return Failed;
    }
    const auto request = frame(payload);
    if (socket.write(request) != request.size()) {
        m_error = QStringLiteral("Cannot send instance request.");
        return Failed;
    }
    while (socket.bytesToWrite() > 0) {
        if (remaining() == 0 || !socket.waitForBytesWritten(remaining())) {
            m_error = QStringLiteral("Timed out sending instance request.");
            return Failed;
        }
    }
    while (socket.bytesAvailable() == 0) {
        if (remaining() == 0 || !socket.waitForReadyRead(remaining())) {
            m_error = QStringLiteral("Primary did not acknowledge instance request.");
            return Failed;
        }
    }
    if (socket.readAll() != QByteArrayLiteral("1")) {
        m_error = QStringLiteral("Invalid instance acknowledgement.");
        return Failed;
    }
    return Forwarded;
}

void InstanceBroker::acceptConnections()
{
    while (m_server->hasPendingConnections()) {
        auto *socket = m_server->nextPendingConnection();
        if (socket->state() != QLocalSocket::ConnectedState || m_clients >= MaxClients) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        ++m_clients;
        socket->setReadBufferSize(MaxPayload + 5);
        auto *timer = new QTimer(socket);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, socket, [socket] { socket->abort(); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            --m_clients;
            socket->deleteLater();
        });
        // The buffer is owned by the connection's readyRead functor.
        QElapsedTimer elapsed;
        elapsed.start();
        auto receive = [this, socket = QPointer<QLocalSocket>(socket), elapsed,
                        buffer = QByteArray(), done = false]() mutable {
            if (done || !socket || socket->state() != QLocalSocket::ConnectedState)
                return;
            if (elapsed.elapsed() >= TimeoutMs) {
                done = true;
                socket->abort();
                return;
            }
            buffer.append(socket->read(MaxPayload + 5 - buffer.size()));
            if (buffer.size() < 4)
                return;
            const auto length = qFromBigEndian<quint32>(buffer.constData());
            if (length == 0 || length > MaxPayload || buffer.size() > qint64(length) + 4) {
                done = true;
                socket->abort();
                return;
            }
            if (buffer.size() < qint64(length) + 4)
                return;
            done = true;
            QStringList paths;
            if (!decode(buffer.mid(4), paths)) {
                socket->abort();
                return;
            }
            QPointer<InstanceBroker> guard(this);
            QPointer<QLocalSocket> client(socket);
            emit openRequested(paths);
            if (!guard || !client)
                return;
            if (elapsed.elapsed() >= TimeoutMs || socket->state() != QLocalSocket::ConnectedState) {
                socket->abort();
                return;
            }
            socket->write("1", 1);
            // On Unix, disconnectFromServer may defer the buffered write until
            // another notifier event. Flush before queued UI work can block
            // this event loop longer than the secondary's ACK deadline.
            socket->flush();
            socket->disconnectFromServer();
        };
        // Never deliver application callbacks on Qt's native socket-notifier
        // stack: nested event loops may disconnect/delete the socket there.
        connect(socket, &QLocalSocket::readyRead, this, receive, Qt::QueuedConnection);
        timer->start(TimeoutMs);
        // A client can have sent data before newConnection was delivered.
        if (socket->bytesAvailable() > 0) {
            QPointer<InstanceBroker> guard(this);
            emit socket->readyRead();
            if (!guard)
                return;
        }
    }
}

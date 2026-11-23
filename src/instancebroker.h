#pragma once

#include <QObject>
#include <QStringList>
#include <memory>

class QLocalServer;
class QLockFile;

// Standalone, same-user request transport. Use on the QCoreApplication thread
// and connect openRequested BEFORE start(): Primary emits the initial request
// synchronously. An empty list requests a new untitled window. Paths need not
// exist, but must be absolute native local paths (not URLs).
//
// Each instance may be started once. Keep a Primary alive for the application's
// lifetime. Forwarded means accepted by the primary, not that a file was opened;
// Failed must NOT be used as permission to launch another editor or blindly
// retry: an accepted request can lose its ACK. Handlers should enqueue work
// and return promptly rather than perform blocking file/UI operations.
class InstanceBroker : public QObject
{
    Q_OBJECT
public:
    enum StartResult { Primary, Forwarded, Failed };
    Q_ENUM(StartResult)

    explicit InstanceBroker(QObject *parent = nullptr);
    // Explicit location/name isolate tests and independent application instances.
    InstanceBroker(const QString &runtimeDirectory, const QString &endpointName,
                   QObject *parent = nullptr);
    ~InstanceBroker() override;

    StartResult start(const QStringList &absoluteFilePaths);
    QString errorString() const;
    static QString endpointAddress(const QString &directory, const QString &name);

signals:
    void openRequested(const QStringList &absoluteFilePaths);

private:
    friend class tst_InstanceBroker;
    static QString defaultDirectory();
    static QString prepareFallbackDirectory(const QString &base);
    void acceptConnections();
    StartResult forward(const QByteArray &payload, int timeoutMs);

    QString m_runtimeDirectory;
    QString m_endpointName;
    QString m_error;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer *m_server = nullptr;
    int m_clients = 0;
    bool m_started = false;
};

// Wire format: one big-endian uint32 length followed by UTF-8 JSON
// {"version":1,"paths":[...]}. ACK is the single byte '1', sent only after
// validation and openRequested delivery. One request per connection. Limits:
// 64 paths, 256 KiB JSON, 16 clients, 3000 ms total per client/forward attempt.
// A contender allows up to 1000 ms for lock acquisition/startup within that
// deadline. Only the lock owner may remove a stale server endpoint.

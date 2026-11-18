#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QThread>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLocalServer>
#include <QEventLoop>
#include <QPointer>
#include <QElapsedTimer>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <vector>

#include "instancebroker.h"

namespace {

// Keep Unix-domain socket names short, including on macOS where the default
// temporary directory can already consume most of sun_path.
QString temporaryTemplate()
{
#ifdef Q_OS_UNIX
    return QStringLiteral("/tmp/ib-XXXXXX");
#else
    return QDir::tempPath() + QStringLiteral("/ib-XXXXXX");
#endif
}

QStringList samplePaths()
{
    return {QDir::temp().absoluteFilePath(QStringLiteral("a file.md")),
            QDir::temp().absoluteFilePath(QString::fromUtf8("日本語-élève.md")),
            QDir::temp().absoluteFilePath(QStringLiteral("another-file.md"))};
}

QByteArray payload(const QStringList &paths)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("paths"), QJsonArray::fromStringList(paths)}
    }).toJson(QJsonDocument::Compact);
}

QByteArray lengthPrefix(quint32 length)
{
    QByteArray result(4, '\0');
    for (int i = 0; i < 4; ++i)
        result[i] = char((length >> (24 - 8 * i)) & 0xff);
    return result;
}

QByteArray frame(const QByteArray &body)
{
    return lengthPrefix(quint32(body.size())) + body;
}

void printEvent(const QJsonObject &event)
{
    const QByteArray line = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

// A secondary must run in another process: start() can block waiting for the
// primary's acknowledgement, which requires the primary's event loop to run.
int runHelper(QCoreApplication &app)
{
    const QStringList args = app.arguments();
    if (args.size() < 5)
        return 3;
    InstanceBroker broker(args.at(2), args.at(3));
    QObject::connect(&broker, &InstanceBroker::openRequested, &app,
                     [](const QStringList &paths) {
        printEvent({{QStringLiteral("event"), QStringLiteral("request")},
                    {QStringLiteral("paths"), QJsonArray::fromStringList(paths)}});
    });
    // Encode paths in one ASCII argument: macOS normalizes Unicode argv.
    const auto document = QJsonDocument::fromJson(QByteArray::fromBase64(args.value(5).toLatin1()));
    QStringList paths;
    for (const auto &value : document.array())
        paths.append(value.toString());
    const auto result = broker.start(paths);
    const QString name = result == InstanceBroker::Primary ? QStringLiteral("primary")
        : result == InstanceBroker::Forwarded ? QStringLiteral("forwarded")
                                              : QStringLiteral("failed");
    printEvent({{QStringLiteral("event"), QStringLiteral("start")},
                {QStringLiteral("result"), name},
                {QStringLiteral("error"), broker.errorString()}});
    if (result != InstanceBroker::Primary)
        return result == InstanceBroker::Forwarded ? 0 : 2;
    QTimer::singleShot(args.at(4).toInt(), &app, &QCoreApplication::quit);
    return app.exec();
}

void launch(QProcess &process, const QString &directory, const QString &endpoint,
            const QStringList &paths, int lifetime = 15000)
{
    QStringList args{QStringLiteral("--broker-helper"), directory, endpoint,
                     QString::number(lifetime)};
    args.append(QString::fromLatin1(QJsonDocument(QJsonArray::fromStringList(paths))
                                       .toJson(QJsonDocument::Compact).toBase64()));
    process.start(QCoreApplication::applicationFilePath(), args);
}

QList<QJsonObject> events(QProcess &process, QByteArray &output)
{
    output += process.readAllStandardOutput();
    QList<QJsonObject> result;
    const auto lines = output.split('\n');
    // The final item may be an incomplete line.
    for (int i = 0; i + 1 < lines.size(); ++i) {
        const QJsonDocument document = QJsonDocument::fromJson(lines.at(i));
        if (document.isObject())
            result.append(document.object());
    }
    return result;
}

QString startResult(QProcess &process, QByteArray &output)
{
    for (const auto &event : events(process, output)) {
        if (event.value(QStringLiteral("event")) == QStringLiteral("start"))
            return event.value(QStringLiteral("result")).toString();
    }
    return {};
}

int requestCount(QProcess &process, QByteArray &output)
{
    int count = 0;
    for (const auto &event : events(process, output)) {
        if (event.value(QStringLiteral("event")) == QStringLiteral("request"))
            ++count;
    }
    return count;
}

// Also clean up children if a QVERIFY/QCOMPARE returns early.
struct Child : QProcess {
    ~Child() override
    {
        if (state() != QProcess::NotRunning) {
            kill();
            waitForFinished(5000);
        }
    }
};

} // namespace

class tst_InstanceBroker : public QObject
{
    Q_OBJECT

private slots:
    void primaryCreatesRuntimeDirectory();
    void forwards_data();
    void forwards();
    void fragmentedFrame();
    void rejectsMalformed_data();
    void rejectsMalformed();
    void disconnectAndTimeout();
    void clientLimit();
    void lockContentionPreservesEndpoint();
    void staleEndpoint();
    void concurrentStartup();
    void unsafeRuntimeDirectory();
    void fallbackParentMustBePrivate();
    void platformEndpointAddress();
    void queuedDeliveryAcknowledgesBeforeSlowOpening();
    void receiverDeletesBroker();
    void receiverEntersNestedLoop();
    void unresponsivePrimary();
    void invalidInitialRequests();
};

void tst_InstanceBroker::primaryCreatesRuntimeDirectory()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    const QString runtime = temporary.path() + QStringLiteral("/new");
    QVERIFY(!QDir(runtime).exists());
    InstanceBroker broker(runtime, QStringLiteral("s"));
    QSignalSpy requests(&broker, &InstanceBroker::openRequested);
    const QStringList paths = samplePaths();
    QCOMPARE(broker.start(paths), InstanceBroker::Primary);
    QVERIFY(QDir(runtime).exists());
    // No event-loop turn: the initial request must be synchronous.
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.at(0).at(0).toStringList(), paths);
    QCOMPARE(broker.start({}), InstanceBroker::Failed);
    QCOMPARE(requests.size(), 1);
}

void tst_InstanceBroker::forwards_data()
{
    QTest::addColumn<QStringList>("paths");
    QTest::newRow("spaces-unicode-multiple") << samplePaths();
    QTest::newRow("empty") << QStringList{};
    QTest::newRow("single") << QStringList{samplePaths().first()};
    QStringList maximum;
    for (int i = 0; i < 64; ++i)
        maximum.append(QDir::temp().absoluteFilePath(QString::number(i)));
    QTest::newRow("64-paths") << maximum;
}

void tst_InstanceBroker::forwards()
{
    QFETCH(QStringList, paths);
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    InstanceBroker broker(temporary.path(), QStringLiteral("s"));
    QSignalSpy requests(&broker, &InstanceBroker::openRequested);
    QCOMPARE(broker.start({}), InstanceBroker::Primary);
    QCOMPARE(requests.size(), 1);
    requests.clear();

    Child secondary;
    launch(secondary, temporary.path(), QStringLiteral("s"), paths);
    QTRY_VERIFY_WITH_TIMEOUT(secondary.state() == QProcess::NotRunning, 8000);
    QCOMPARE(secondary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(secondary.exitCode(), 0);
    QByteArray output;
    QCOMPARE(startResult(secondary, output), QStringLiteral("forwarded"));
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.at(0).at(0).toStringList(), paths);
}

void tst_InstanceBroker::fragmentedFrame()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    InstanceBroker broker(temporary.path(), QStringLiteral("s"));
    QSignalSpy requests(&broker, &InstanceBroker::openRequested);
    QCOMPARE(broker.start({}), InstanceBroker::Primary);
    requests.clear();
    QLocalSocket socket;
    socket.connectToServer(InstanceBroker::endpointAddress(temporary.path(), QStringLiteral("s")));
    QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
    const QByteArray wire = frame(payload(samplePaths()));
    // Split both the length prefix and the payload across separate writes.
    QCOMPARE(socket.write(wire.left(2)), qint64(2));
    socket.flush();
    QTest::qWait(30);
    QCOMPARE(requests.size(), 0);
    QCOMPARE(socket.bytesAvailable(), qint64(0));
    QCOMPARE(socket.write(wire.mid(2, wire.size() - 3)), qint64(wire.size() - 3));
    socket.flush();
    QTest::qWait(30);
    QCOMPARE(requests.size(), 0);
    QCOMPARE(socket.bytesAvailable(), qint64(0));
    QCOMPARE(socket.write(wire.right(1)), qint64(1));
    socket.flush();
    QTRY_VERIFY(socket.bytesAvailable() > 0);
    QCOMPARE(socket.readAll(), QByteArray("1"));
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.at(0).at(0).toStringList(), samplePaths());
}

void tst_InstanceBroker::rejectsMalformed_data()
{
    QTest::addColumn<QByteArray>("wire");
    QTest::newRow("zero-length") << lengthPrefix(0);
    QTest::newRow("oversize") << lengthPrefix(256 * 1024 + 1);
    QTest::newRow("huge-length") << lengthPrefix(0xffffffffu);
    QTest::newRow("invalid-json") << frame("{broken");
    QTest::newRow("array") << frame("[]");
    QTest::newRow("missing-version") << frame("{\"paths\":[]}");
    QTest::newRow("wrong-version") << frame("{\"version\":2,\"paths\":[]}");
    QTest::newRow("string-version") << frame("{\"version\":\"1\",\"paths\":[]}");
    QTest::newRow("extra-field") << frame("{\"version\":1,\"paths\":[],\"extra\":true}");
    QTest::newRow("missing-paths") << frame("{\"version\":1}");
    QTest::newRow("paths-not-array") << frame("{\"version\":1,\"paths\":\"/tmp/a\"}");
    QTest::newRow("null-path") << frame("{\"version\":1,\"paths\":[null]}");
    QTest::newRow("numeric-path") << frame("{\"version\":1,\"paths\":[42]}");
    QTest::newRow("relative") << frame(payload({QStringLiteral("relative.md")}));
    QTest::newRow("empty-path") << frame(payload({QString()}));
    QTest::newRow("url") << frame(payload({QStringLiteral("https://example.org/a.md")}));
    QTest::newRow("file-url") << frame(payload({QStringLiteral("file:///tmp/a.md")}));
    QString nul = samplePaths().first();
    nul.append(QChar(0));
    nul.append(QStringLiteral("suffix"));
    QTest::newRow("embedded-nul") << frame(payload({nul}));
    QStringList tooMany;
    for (int i = 0; i < 65; ++i)
        tooMany.append(samplePaths().first());
    QTest::newRow("65-paths") << frame(payload(tooMany));
}

void tst_InstanceBroker::rejectsMalformed()
{
    QFETCH(QByteArray, wire);
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    InstanceBroker broker(temporary.path(), QStringLiteral("s"));
    QSignalSpy requests(&broker, &InstanceBroker::openRequested);
    QCOMPARE(broker.start({}), InstanceBroker::Primary);
    requests.clear();
    const QString endpoint = InstanceBroker::endpointAddress(temporary.path(), QStringLiteral("s"));
    QLocalSocket socket;
    socket.connectToServer(endpoint);
    QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
    QCOMPARE(socket.write(wire), qint64(wire.size()));
    socket.flush();
    QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QLocalSocket::UnconnectedState, 5000);
    QVERIFY(!socket.readAll().contains('1'));
    QCOMPARE(requests.size(), 0);

    // Rejecting a bad peer must leave the listener usable.
    QLocalSocket good;
    good.connectToServer(endpoint);
    QTRY_COMPARE(good.state(), QLocalSocket::ConnectedState);
    const QByteArray valid = frame(payload(samplePaths()));
    QCOMPARE(good.write(valid), qint64(valid.size()));
    good.flush();
    QTRY_VERIFY(good.bytesAvailable() > 0);
    QCOMPARE(good.readAll(), QByteArray("1"));
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.at(0).at(0).toStringList(), samplePaths());
}

void tst_InstanceBroker::disconnectAndTimeout()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    InstanceBroker broker(temporary.path(), QStringLiteral("s"));
    QSignalSpy requests(&broker, &InstanceBroker::openRequested);
    QCOMPARE(broker.start({}), InstanceBroker::Primary);
    requests.clear();
    const QString endpoint = InstanceBroker::endpointAddress(temporary.path(), QStringLiteral("s"));
    {
        QLocalSocket partial;
        partial.connectToServer(endpoint);
        QTRY_COMPARE(partial.state(), QLocalSocket::ConnectedState);
        partial.write(lengthPrefix(100) + "{");
        partial.flush();
        QTRY_COMPARE(partial.bytesToWrite(), qint64(0));
        partial.abort();
    }
    QLocalSocket stalled;
    stalled.connectToServer(endpoint);
    QTRY_COMPARE(stalled.state(), QLocalSocket::ConnectedState);
    stalled.write(lengthPrefix(100) + "{");
    stalled.flush();
    QTRY_COMPARE_WITH_TIMEOUT(stalled.state(), QLocalSocket::UnconnectedState, 5000);
    QVERIFY(!stalled.readAll().contains('1'));
    QCOMPARE(requests.size(), 0);

    Child secondary;
    launch(secondary, temporary.path(), QStringLiteral("s"), {});
    QTRY_VERIFY_WITH_TIMEOUT(secondary.state() == QProcess::NotRunning, 8000);
    QCOMPARE(secondary.exitCode(), 0);
    QCOMPARE(requests.size(), 1);
    QVERIFY(requests.at(0).at(0).toStringList().isEmpty());
}

void tst_InstanceBroker::clientLimit()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    InstanceBroker broker(temporary.path(), QStringLiteral("s"));
    QSignalSpy requests(&broker, &InstanceBroker::openRequested);
    QCOMPARE(broker.start({}), InstanceBroker::Primary);
    requests.clear();
    const QString endpoint = InstanceBroker::endpointAddress(temporary.path(), QStringLiteral("s"));
    std::vector<std::unique_ptr<QLocalSocket>> clients;
    for (int i = 0; i < 16; ++i) {
        clients.emplace_back(new QLocalSocket);
        clients.back()->connectToServer(endpoint);
        QTRY_COMPARE(clients.back()->state(), QLocalSocket::ConnectedState);
    }
    QLocalSocket excess;
    excess.connectToServer(endpoint);
    QTRY_COMPARE_WITH_TIMEOUT(excess.state(), QLocalSocket::UnconnectedState, 2000);
    QVERIFY(!excess.readAll().contains('1'));
    QCOMPARE(requests.size(), 0);
    for (auto &client : clients)
        client->abort();

    Child secondary;
    launch(secondary, temporary.path(), QStringLiteral("s"), samplePaths());
    QTRY_VERIFY_WITH_TIMEOUT(secondary.state() == QProcess::NotRunning, 8000);
    QCOMPARE(secondary.exitCode(), 0);
    QCOMPARE(requests.size(), 1);
}

void tst_InstanceBroker::lockContentionPreservesEndpoint()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    const QString endpoint = QDir(temporary.path()).filePath(QStringLiteral("s"));
    QLockFile lock(endpoint + QStringLiteral(".lock"));
    QVERIFY(lock.tryLock());
    const QByteArray sentinel("do not remove this endpoint");
    QFile file(endpoint);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(sentinel), qint64(sentinel.size()));
    file.close();
    {
        InstanceBroker broker(temporary.path(), QStringLiteral("s"));
        QSignalSpy requests(&broker, &InstanceBroker::openRequested);
        QCOMPARE(broker.start(samplePaths()), InstanceBroker::Failed);
        QVERIFY(!broker.errorString().isEmpty());
        QCOMPARE(requests.size(), 0);
    }
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), sentinel);
    QVERIFY(QFile::exists(endpoint + QStringLiteral(".lock")));
}

void tst_InstanceBroker::staleEndpoint()
{
#ifndef Q_OS_UNIX
    QSKIP("A filesystem socket left by a killed process is Unix-specific");
#else
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    Child oldPrimary;
    launch(oldPrimary, temporary.path(), QStringLiteral("s"), {});
    QByteArray output;
    QTRY_COMPARE_WITH_TIMEOUT(startResult(oldPrimary, output), QStringLiteral("primary"), 8000);
    oldPrimary.kill();
    QTRY_VERIFY(oldPrimary.state() == QProcess::NotRunning);
    const QString endpoint = QDir(temporary.path()).filePath(QStringLiteral("s"));
    QVERIFY(QFile::exists(endpoint));
    InstanceBroker replacement(temporary.path(), QStringLiteral("s"));
    QSignalSpy requests(&replacement, &InstanceBroker::openRequested);
    QCOMPARE(replacement.start(samplePaths()), InstanceBroker::Primary);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.at(0).at(0).toStringList(), samplePaths());
#endif
}

void tst_InstanceBroker::concurrentStartup()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    Child first;
    Child second;
    const QStringList a{samplePaths().at(0)};
    const QStringList b{samplePaths().at(1)};
    launch(first, temporary.path(), QStringLiteral("s"), a);
    launch(second, temporary.path(), QStringLiteral("s"), b);
    QByteArray firstOutput;
    QByteArray secondOutput;
    QTRY_VERIFY_WITH_TIMEOUT(!startResult(first, firstOutput).isEmpty()
                            && !startResult(second, secondOutput).isEmpty(), 10000);
    const QString firstResult = startResult(first, firstOutput);
    const QString secondResult = startResult(second, secondOutput);
    QVERIFY2((firstResult == QStringLiteral("primary") && secondResult == QStringLiteral("forwarded"))
             || (secondResult == QStringLiteral("primary") && firstResult == QStringLiteral("forwarded")),
             (firstOutput + secondOutput).constData());
    QProcess &primary = firstResult == QStringLiteral("primary") ? first : second;
    QProcess &secondary = firstResult == QStringLiteral("primary") ? second : first;
    QByteArray &primaryOutput = firstResult == QStringLiteral("primary") ? firstOutput : secondOutput;
    QTRY_COMPARE(requestCount(primary, primaryOutput), 2);
    QTRY_VERIFY(secondary.state() == QProcess::NotRunning);
    QCOMPARE(secondary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(secondary.exitCode(), 0);
    QList<QStringList> received;
    for (const auto &event : events(primary, primaryOutput)) {
        if (event.value(QStringLiteral("event")) == QStringLiteral("request")) {
            QStringList paths;
            for (const auto &path : event.value(QStringLiteral("paths")).toArray())
                paths.append(path.toString());
            received.append(paths);
        }
    }
    QVERIFY(received.contains(a));
    QVERIFY(received.contains(b));
}

void tst_InstanceBroker::invalidInitialRequests()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    const QList<QStringList> invalid{
        {QStringLiteral("relative.md")},
        {QStringLiteral("file:///tmp/a.md")},
        {QDir::temp().absoluteFilePath(QString(300000, QLatin1Char('x')))},
        QStringList(65, samplePaths().first())
    };
    for (const auto &paths : invalid) {
        InstanceBroker broker(temporary.path(), QStringLiteral("s"));
        QSignalSpy requests(&broker, &InstanceBroker::openRequested);
        QCOMPARE(broker.start(paths), InstanceBroker::Failed);
        QVERIFY(!broker.errorString().isEmpty());
        QCOMPARE(requests.size(), 0);
        QVERIFY(!QFile::exists(temporary.path() + QStringLiteral("/s.lock")));
    }
}

void tst_InstanceBroker::unsafeRuntimeDirectory()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix directory permissions/symlink checks");
#else
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    QVERIFY(QFile::setPermissions(temporary.path(), QFile::ReadOwner | QFile::WriteOwner
                                 | QFile::ExeOwner | QFile::ReadOther | QFile::ExeOther));
    InstanceBroker shared(temporary.path(), QStringLiteral("s"));
    QCOMPARE(shared.start({}), InstanceBroker::Failed);
    QVERIFY(!shared.errorString().isEmpty());
    const QString link = temporary.path() + QStringLiteral("-link");
    QVERIFY(QFile::link(temporary.path(), link));
    InstanceBroker linked(link, QStringLiteral("s"));
    QCOMPARE(linked.start({}), InstanceBroker::Failed);
    QVERIFY(QFile::remove(link));
#endif
}

void tst_InstanceBroker::platformEndpointAddress()
{
#ifdef Q_OS_WIN
    const QString directory = QStringLiteral("C:/Users/") + QString(1000, QLatin1Char('x'));
    const QString address = InstanceBroker::endpointAddress(directory, QStringLiteral("s"));
    QVERIFY(address.size() < 100);
    QVERIFY(!address.contains('/') && !address.contains('\\') && !address.contains(':'));
    QCOMPARE(address, InstanceBroker::endpointAddress(directory.toUpper(), QStringLiteral("s")));
#else
    QCOMPARE(InstanceBroker::endpointAddress(QStringLiteral("/tmp/private"), QStringLiteral("s")),
             QStringLiteral("/tmp/private/s"));
#endif
}

void tst_InstanceBroker::fallbackParentMustBePrivate()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    const QString parent = temporary.path() + QStringLiteral("/fallback");
    QVERIFY(!InstanceBroker::prepareFallbackDirectory(parent).isEmpty());
#ifdef Q_OS_UNIX
    QVERIFY(QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                | QFileDevice::ExeOwner | QFileDevice::ReadOther));
    QVERIFY(InstanceBroker::prepareFallbackDirectory(parent).isEmpty());
    const QString link = temporary.path() + QStringLiteral("/link");
    QVERIFY(QFile::link(parent, link));
    QVERIFY(InstanceBroker::prepareFallbackDirectory(link).isEmpty());
#endif
}

void tst_InstanceBroker::queuedDeliveryAcknowledgesBeforeSlowOpening()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    InstanceBroker broker(temporary.path(), QStringLiteral("s"));
    QCOMPARE(broker.start({}), InstanceBroker::Primary);
    int opened = 0;
    connect(&broker, &InstanceBroker::openRequested, this, [&] {
        QThread::msleep(3300); // Simulate slow QML/disk work, outside the IPC ACK callback.
        ++opened;
    }, Qt::QueuedConnection);
    Child secondary;
    launch(secondary, temporary.path(), QStringLiteral("s"), samplePaths());
    QTRY_VERIFY_WITH_TIMEOUT(secondary.state() == QProcess::NotRunning, 7000);
    QCOMPARE(secondary.exitCode(), 0);
    QCOMPARE(opened, 1);
}

void tst_InstanceBroker::receiverDeletesBroker()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    QPointer<InstanceBroker> broker = new InstanceBroker(temporary.path(), QStringLiteral("s"));
    QCOMPARE(broker->start({}), InstanceBroker::Primary);
    connect(broker, &InstanceBroker::openRequested, this, [&] { delete broker.data(); });
    QLocalSocket socket;
    socket.connectToServer(InstanceBroker::endpointAddress(temporary.path(), QStringLiteral("s")));
    // Queue data before the server processes newConnection.
    QVERIFY(socket.waitForConnected(1000));
    socket.write(frame(payload(samplePaths())));
    socket.flush();
    QTRY_VERIFY(broker.isNull());
}

void tst_InstanceBroker::receiverEntersNestedLoop()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    InstanceBroker broker(temporary.path(), QStringLiteral("s"));
    QCOMPARE(broker.start({}), InstanceBroker::Primary);
    int received = 0;
    connect(&broker, &InstanceBroker::openRequested, this, [&] {
        ++received;
        QEventLoop loop;
        QTimer::singleShot(3300, &loop, &QEventLoop::quit);
        loop.exec();
    });
    QLocalSocket socket;
    socket.connectToServer(InstanceBroker::endpointAddress(temporary.path(), QStringLiteral("s")));
    QVERIFY(socket.waitForConnected(1000));
    socket.write(frame(payload({})));
    socket.flush();
    QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QLocalSocket::UnconnectedState, 5000);
    QCOMPARE(received, 1);
    QVERIFY(socket.readAll().isEmpty());
}

void tst_InstanceBroker::unresponsivePrimary()
{
    QTemporaryDir temporary(temporaryTemplate());
    QVERIFY(temporary.isValid());
    const QString endpoint = QDir(temporary.path()).filePath(QStringLiteral("s"));
    QLockFile lock(endpoint + QStringLiteral(".lock"));
    QVERIFY(lock.tryLock());
    QLocalServer silent;
    silent.setSocketOptions(QLocalServer::UserAccessOption);
    QVERIFY(silent.listen(InstanceBroker::endpointAddress(temporary.path(), QStringLiteral("s"))));
    Child secondary;
    QElapsedTimer elapsed;
    elapsed.start();
    launch(secondary, temporary.path(), QStringLiteral("s"), {});
    QTRY_VERIFY_WITH_TIMEOUT(secondary.state() == QProcess::NotRunning, 6000);
    QVERIFY(elapsed.elapsed() < 5500);
    QCOMPARE(secondary.exitCode(), 2);
    QByteArray output;
    QCOMPARE(startResult(secondary, output), QStringLiteral("failed"));
    QVERIFY(silent.isListening());
    QVERIFY(QFile::exists(endpoint + QStringLiteral(".lock")));
    QLocalSocket probe;
    probe.connectToServer(InstanceBroker::endpointAddress(temporary.path(), QStringLiteral("s")));
    QVERIFY(probe.waitForConnected(1000));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == QStringLiteral("--broker-helper"))
        return runHelper(app);
    tst_InstanceBroker test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_instancebroker.moc"

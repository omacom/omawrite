#include "mermaidrenderer.h"
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <QSaveFile>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class MermaidTests : public QObject {
    Q_OBJECT
    QByteArray previousPath;
    static QString fenced(const QString &body) { return "```mermaid\n" + body + "\n```"; }
private slots:
    void init() { previousPath = qgetenv("PATH"); }
    void cleanup() {
        qputenv("PATH", previousPath);
        qunsetenv("OMAWRITE_TEST_PNG");
    }
    void supportedSyntax() {
        QVERIFY(MermaidRenderer::supported("flowchart LR\nA-->B"));
        QVERIFY(MermaidRenderer::supported("sequenceDiagram\nAlice->>Bob: Hello"));
        QVERIFY(!MermaidRenderer::supported("flowchart LR\nclick A \"https://example.com\""));
        QVERIFY(!MermaidRenderer::supported("%%{init: {}}%%\nflowchart LR\nA-->B"));
        QVERIFY(!MermaidRenderer::supported("flowchart LR\nA[<img src='file:///tmp/x'>]"));
        QVERIFY(!MermaidRenderer::supported(QStringLiteral("x").repeated(32001)));
        QVERIFY(!MermaidRenderer::supported("flowchart LR\nA-->B\nclassDef default fill:red"));
        QVERIFY(!MermaidRenderer::supported("flowchart LR\nA-->B\nstyle A fill:u\\75rl(x)"));
        QVERIFY(!MermaidRenderer::supported("flowchart LR\nA[&#x3c;img]"));
        QVERIFY(!MermaidRenderer::supported("flowchart LR\nA@{ img: 'something' }"));
    }
    void guiPathFindsCliAndItsInterpreter_data() {
        QTest::addColumn<bool>("separateNode");
        QTest::addColumn<bool>("symlinkCli");
        QTest::newRow("same-bin") << false << false;
        QTest::newRow("separate-bin") << true << false;
        QTest::newRow("npm-symlink") << false << true;
    }
    void guiPathFindsCliAndItsInterpreter() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX fixture; Windows uses native helper tests.");
#else
        QFETCH(bool, separateNode);
        QFETCH(bool, symlinkCli);
        QTemporaryDir cli, node, inherited;
        QVERIFY(cli.isValid() && node.isValid() && inherited.isValid());
        QImage fixture(40, 20, QImage::Format_ARGB32); fixture.fill(Qt::green);
        const QString png = cli.filePath("original.png"); QVERIFY(fixture.save(png));
        const auto executable = [](const QString &path, const QByteArray &bytes) {
            QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
                && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        };
        const QString script = symlinkCli ? cli.filePath("lib/cli.mjs") : cli.filePath("mmdc");
        if (symlinkCli) QVERIFY(QDir().mkpath(cli.filePath("lib")));
        QVERIFY(executable(script, "#!/usr/bin/env node\nunused fake JavaScript\n"));
        if (symlinkCli) QVERIFY(QFile::link(script, cli.filePath("mmdc")));
        const QString nodeDir = separateNode ? node.path() : cli.path();
        // env passes the script name as $1, so the CLI output argument is $5.
        QVERIFY(executable(nodeDir + "/node", "#!/bin/sh\n/bin/cp \"$OMAWRITE_TEST_PNG\" \"$5\"\n"));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PATH", inherited.path()); environment.insert("OMAWRITE_TEST_PNG", png);
        const QByteArray parentPath = qgetenv("PATH");
        MermaidRenderer renderer(nullptr, environment, {cli.path(), nodeDir});
        QCOMPARE(renderer.m_executable, cli.filePath("mmdc"));
        const auto childPaths = renderer.m_process.processEnvironment().value("PATH").split(':');
        QStringList expectedPaths{cli.path(), inherited.path()};
        if (separateNode) expectedPaths.append(node.path());
        QCOMPARE(childPaths, expectedPaths); QCOMPARE(qgetenv("PATH"), parentPath);
        QSignalSpy ready(&renderer, &MermaidRenderer::ready);
        const auto code = fenced("flowchart LR\nA-->B");
        QVERIFY(renderer.request(code, false, 500).isNull()); QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(renderer.request(code, false, 500).size(), QSize(40, 20));
        QCOMPARE(qgetenv("PATH"), parentPath);
        // PATH-installed CLI wins over fallback copies; an empty PATH does not
        // silently consult the host's real PATH during injected discovery.
        QVERIFY(executable(node.filePath("mmdc"), "#!/bin/sh\nexit 7\n"));
        environment.insert("PATH", cli.path());
        MermaidRenderer preferred(nullptr, environment, {node.path()});
        QCOMPARE(preferred.m_executable, cli.filePath("mmdc"));
        environment.remove("PATH");
        MermaidRenderer unavailable(nullptr, environment, {}); QVERIFY(unavailable.m_executable.isEmpty());
#endif
    }
    void missingNodeAndNonExecutableCliRemainSource() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX fixture; Windows uses native helper tests.");
#else
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile cli(directory.filePath("mmdc")); QVERIFY(cli.open(QIODevice::WriteOnly));
        cli.write("#!/usr/bin/env node\nunused\n"); cli.close();
        QVERIFY(cli.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        auto environment = QProcessEnvironment::systemEnvironment(); environment.remove("PATH");
        MermaidRenderer notExecutable(nullptr, environment, {directory.path()}); QVERIFY(notExecutable.m_executable.isEmpty());
        QVERIFY(cli.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MermaidRenderer missingNode(nullptr, environment, {directory.path()});
        QSignalSpy ready(&missingNode, &MermaidRenderer::ready);
        const auto code = fenced("flowchart LR\nA-->B");
        QVERIFY(missingNode.request(code, false, 500).isNull()); QTRY_COMPARE(ready.count(), 1);
        QVERIFY(missingNode.request(code, false, 500).isNull()); QCOMPARE(ready.count(), 1);
#endif
    }
#ifdef Q_OS_WIN
    void windowsNpmDiscovery() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto write = [](const QString &path, const QByteArray &bytes) {
            QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        QVERIFY(write(directory.filePath("mmdc.cmd"), "@exit /b 99\r\n"));
        QVERIFY(write(directory.filePath("node.exe"), "resolver-only fixture"));
        QVERIFY(QDir().mkpath(directory.filePath("node_modules/@mermaid-js/mermaid-cli/src")));
        const QString package = directory.filePath("node_modules/@mermaid-js/mermaid-cli/");
        QVERIFY(write(package + "src/cli.js", "unused"));
        auto environment = QProcessEnvironment::systemEnvironment(); environment.insert("PATH", directory.path());
        const auto parentPath = qgetenv("PATH");
        MermaidRenderer unknown(nullptr, environment, {}); QVERIFY(unknown.m_executable.isEmpty());
        QVERIFY(write(package + "package.json", R"({"name":"@mermaid-js/mermaid-cli","bin":{"mmdc":"./src/cli.js"}})"));
        MermaidRenderer found(nullptr, environment, {});
        QCOMPARE(QFileInfo(found.m_executable).canonicalFilePath(), QFileInfo(directory.filePath("node.exe")).canonicalFilePath());
        QCOMPARE(found.m_argumentPrefix, QStringList{package + "src/cli.js"});
        QCOMPARE(qgetenv("PATH"), parentPath);
        QVERIFY(QFile::remove(directory.filePath("node.exe")));
        MermaidRenderer missingNode(nullptr, environment, {}); QVERIFY(missingNode.m_executable.isEmpty());
        environment.remove("PATH");
        MermaidRenderer noPath(nullptr, environment, {}); QVERIFY(noPath.m_executable.isEmpty());
    }
    void windowsFailedStartDrainsQueue() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile invalid(directory.filePath("mmdc.exe"));
        QVERIFY(invalid.open(QIODevice::WriteOnly)); invalid.write("not a PE executable"); invalid.close();
        auto environment = QProcessEnvironment::systemEnvironment(); environment.insert("PATH", directory.path());
        MermaidRenderer renderer(nullptr, environment, {});
        QSignalSpy ready(&renderer, &MermaidRenderer::ready);
        // Queue first, then deliberately exercise synchronous FailedToStart.
        renderer.m_running = true;
        renderer.request(fenced("flowchart LR\nA-->B"), false, 500);
        renderer.request(fenced("flowchart LR\nC-->D"), false, 500);
        renderer.m_running = false;
        renderer.startNext();
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 5000);
        QVERIFY(renderer.m_queue.isEmpty()); QVERIFY(!renderer.m_running);
        QVERIFY(!renderer.m_timeout.isActive());
        QCOMPARE(renderer.m_process.state(), QProcess::NotRunning);
        QVERIFY(renderer.m_cache.isEmpty());
    }
    void windowsJobCleansDescendants_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("normal-exit") << QString("finish");
        QTest::newRow("crashed-parent") << QString("crash");
        QTest::newRow("timeout") << QString("timeout");
        QTest::newRow("destruction") << QString("destroy");
    }
    void windowsJobCleansDescendants() {
        QFETCH(QString, action);
        // Spaces, Unicode and cmd metacharacters must remain literal path data.
        QTemporaryDir directory(QDir::tempPath() + QString::fromUtf8("/omawrite ü & % ! (test)-XXXXXX"));
        QVERIFY(directory.isValid());
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), directory.filePath("node.exe")));
        const QString package = directory.filePath("node_modules/@mermaid-js/mermaid-cli/");
        QVERIFY(QDir().mkpath(package + "src"));
        QFile shim(directory.filePath("mmdc.cmd")); QVERIFY(shim.open(QIODevice::WriteOnly));
        shim.write("@exit /b 99\r\n"); shim.close(); // Never run this batch shim.
        QFile metadata(package + "package.json"); QVERIFY(metadata.open(QIODevice::WriteOnly));
        metadata.write(R"({"name":"@mermaid-js/mermaid-cli","bin":{"mmdc":"./src/cli.js"}})"); metadata.close();
        QFile entry(package + "src/cli.js"); QVERIFY(entry.open(QIODevice::WriteOnly)); entry.close();
        QImage fixture(40, 20, QImage::Format_ARGB32); fixture.fill(Qt::green);
        QVERIFY(fixture.save(directory.filePath("fixture.png")));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PATH", directory.path() + ";" + QCoreApplication::applicationDirPath() + ";" + environment.value("PATH"));
        environment.insert("OMAWRITE_TEST_PNG", directory.filePath("fixture.png"));
        environment.insert("OMAWRITE_TEST_PID", directory.filePath("pid"));
        environment.insert("OMAWRITE_TEST_HOLD", action == "finish" || action == "crash" ? "0" : "1");
        environment.insert("OMAWRITE_TEST_CRASH", action == "crash" ? "1" : "0");
        auto renderer = std::unique_ptr<MermaidRenderer>(new MermaidRenderer(nullptr, environment, {}));
        QSignalSpy ready(renderer.get(), &MermaidRenderer::ready);
        const auto code = fenced("flowchart LR\nA-->B");
        QVERIFY(renderer->request(code, false, 500).isNull());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath("pid-leaf")), 10000);
        if (action == "timeout") { renderer->m_timeout.setInterval(1); renderer->m_timeout.start(); }
        if (action == "destroy") renderer.reset();
        else QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        if (action == "finish") QCOMPARE(renderer->request(code, false, 500).size(), QSize(40, 20));
        for (const auto &suffix : {"-child", "-leaf"}) {
            QFile pidFile(directory.filePath(QString("pid") + suffix)); QVERIFY(pidFile.open(QIODevice::ReadOnly));
            const DWORD pid = pidFile.readAll().trimmed().toULong(); QVERIFY(pid);
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (process) {
                const DWORD result = WaitForSingleObject(process, 5000); CloseHandle(process);
                QCOMPARE(result, DWORD(WAIT_OBJECT_0));
            } else {
                QCOMPARE(GetLastError(), DWORD(ERROR_INVALID_PARAMETER)); // Already reaped.
            }
        }
    }
#endif
    void platformFallbacksAreExplicit() {
#ifdef Q_OS_MACOS
        QCOMPARE(MermaidRenderer::defaultSearchDirectories(), QStringList({"/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin"}));
#else
        QVERIFY(MermaidRenderer::defaultSearchDirectories().isEmpty());
#endif
    }
    void unavailable() {
        qputenv("PATH", "/nonexistent/omawrite-mermaid-renderer");
        MermaidRenderer renderer(nullptr, QProcessEnvironment::systemEnvironment(), {});
        QSignalSpy ready(&renderer, &MermaidRenderer::ready);
        QVERIFY(renderer.request(fenced("flowchart LR\nA-->B"), false, 500).isNull());
        QCOMPARE(ready.count(), 0);
    }
    void renderAndCache() {
#ifndef Q_OS_UNIX
        QSKIP("Fake renderer uses a POSIX shell.");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage fixture(40, 20, QImage::Format_ARGB32);
        fixture.fill(Qt::red);
        const QString png = dir.filePath("fixture.png");
        QVERIFY(fixture.save(png));
        qputenv("OMAWRITE_TEST_PNG", png.toUtf8());
        const QString script = dir.filePath("mmdc");
        QFile file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        // CLI 12 rejects the old viewport-width switches. Fail rather than
        // let a permissive fake renderer conceal this compatibility bug.
        file.write("#!/bin/sh\nfor arg in \"$@\"; do\n"
                   "case \"$arg\" in -w|--width) echo 'unknown option' >&2; exit 1;; esac\n"
                   "done\ncp \"$OMAWRITE_TEST_PNG\" \"$4\"\n");
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", dir.path().toUtf8() + ":" + previousPath);
        MermaidRenderer renderer(nullptr, QProcessEnvironment::systemEnvironment(), {});
        QSignalSpy ready(&renderer, &MermaidRenderer::ready);
        const QString code = fenced("flowchart LR\nA[unicode 😀]-->B");
        QVERIFY(renderer.request(code, false, 500).isNull());
        QVERIFY(renderer.request(code, false, 500).isNull());
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(renderer.request(code, false, 500).size(), QSize(40, 20));
        QCOMPARE(ready.count(), 1);
#endif
    }
    void invalidOutput() {
#ifndef Q_OS_UNIX
        QSKIP("Fake renderer uses a POSIX shell.");
#else
        QTemporaryDir dir;
        QFile script(dir.filePath("mmdc"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nexit 1\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", dir.path().toUtf8() + ":" + previousPath);
        MermaidRenderer renderer(nullptr, QProcessEnvironment::systemEnvironment(), {});
        QSignalSpy ready(&renderer, &MermaidRenderer::ready);
        const QString code = fenced("flowchart LR\nA-->B");
        QVERIFY(renderer.request(code, false, 500).isNull());
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(renderer.request(code, false, 500).isNull());
        QCOMPARE(ready.count(), 1);
#endif
    }
};
#ifdef Q_OS_WIN
// This executable doubles as fake Node/Chromium: no browser or downloads.
static int windowsHelper(const QStringList &arguments) {
    BOOL inJob = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) || !inJob) return 86;
    const QString marker = qEnvironmentVariable("OMAWRITE_TEST_PID");
    const bool child = arguments.value(1) == "--mermaid-test-child";
    const bool leaf = arguments.value(1) == "--mermaid-test-leaf";
    if (!leaf) {
        qint64 pid = 0;
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                    {child ? "--mermaid-test-leaf" : "--mermaid-test-child"}, {}, &pid)) return 87;
        QSaveFile file(marker + (child ? "-leaf" : "-child"));
        if (!file.open(QIODevice::WriteOnly)) return 88;
        if (file.write(QByteArray::number(pid)) <= 0 || !file.commit()) return 88;
    }
    if (child || leaf || qEnvironmentVariable("OMAWRITE_TEST_HOLD") == "1") {
        QThread::sleep(60);
        return 89;
    }
    QElapsedTimer wait; wait.start();
    while (!QFileInfo::exists(marker + "-leaf") && wait.elapsed() < 5000) QThread::msleep(10);
    if (qEnvironmentVariable("OMAWRITE_TEST_CRASH") == "1") {
        // Recognized by QProcess as CrashExit, without an interactive crash dialog.
        TerminateProcess(GetCurrentProcess(), 0xC0000005);
        return 91;
    }
    const int output = arguments.indexOf("-o");
    return output > 0 && QFile::copy(qEnvironmentVariable("OMAWRITE_TEST_PNG"), arguments.value(output + 1)) ? 0 : 90;
}
#endif
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    const auto arguments = app.arguments();
    if (arguments.value(1).endsWith("/src/cli.js") || arguments.value(1).startsWith("--mermaid-test-"))
        return windowsHelper(arguments);
#endif
    MermaidTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_mermaid.moc"

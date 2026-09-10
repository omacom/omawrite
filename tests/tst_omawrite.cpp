#include <QtTest>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>

#include "backend.h"
#include "buffersession.h"
#include "markdownhighlighter.h"
#include "workspacesession.h"
#include "windowmanager.h"

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_settingsDirectory.isValid());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDirectory.path());
    }

    void countsWords() {
        QCOMPARE(Backend::countWords(QStringLiteral("one two-three don't 42")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好 世界")), 2);
        QCOMPARE(Backend::countWords(QString()), 0);
    }

    void normalizesLinks() {
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("www.example.com/path")),
                 QStringLiteral("https://www.example.com/path"));
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("mailto:writer@example.com")),
                 QStringLiteral("mailto:writer@example.com"));
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("example.com")).isEmpty());
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("file:///tmp/private")).isEmpty());
    }

    void suggestsSafeNames() {
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("My first draft\nBody")),
                 QStringLiteral("My first draft.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("A/B")), QStringLiteral("A-B.md"));
        QCOMPARE(Backend::suggestedFileName(QString()), QStringLiteral("Untitled.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("Already.md")),
                 QStringLiteral("Already.md"));
    }

    void restoresOrderedBuffersAndActiveCaret() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());

        BufferSession session(stateDirectory.path());
        const QString first = session.createBuffer();
        session.updateBuffer(first, QString(), QStringLiteral("first"), 2, 1, 2, true);
        const QString second = session.createBuffer();
        session.updateBuffer(second, QString(), QStringLiteral("second"), 4, 4, 4, true);
        session.selectBuffer(first);
        QVERIFY(session.saveNow());

        BufferSession restored(stateDirectory.path());
        QVERIFY(restored.restore());
        QCOMPARE(restored.activeBufferId(), first);
        QCOMPARE(restored.buffers().size(), 2);
        QCOMPARE(restored.buffers().at(0).toMap().value(QStringLiteral("text")),
                 QStringLiteral("first"));
        QCOMPARE(restored.buffers().at(0).toMap().value(QStringLiteral("cursorPosition")), 2);
        QCOMPARE(restored.buffers().at(0).toMap().value(QStringLiteral("selectionStart")), 1);
        QCOMPARE(restored.buffers().at(0).toMap().value(QStringLiteral("selectionEnd")), 2);
        QCOMPARE(restored.buffers().at(1).toMap().value(QStringLiteral("text")),
                 QStringLiteral("second"));
    }

    void restoresWorkspaceWindowsAndActiveCarets() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());

        WorkspaceSession session(stateDirectory.path());
        const QString firstWindow = session.createWindow(10, 20, 900, 700, false);
        const QString firstTab = session.createTab(firstWindow, QUrl(), QStringLiteral("first"),
                                                   2, 1, 2, true);
        const QString secondTab = session.createTab(firstWindow, QUrl(), QStringLiteral("second"),
                                                    4, 4, 4, true);
        QVERIFY(session.setActiveTab(firstWindow, firstTab));

        const QString secondWindow = session.createWindow(30, 40, 800, 600, true);
        const QString thirdTab = session.createTab(secondWindow, QUrl(), QStringLiteral("third"),
                                                   3, 0, 3, false);
        QVERIFY(session.setActiveTab(secondWindow, thirdTab));
        QVERIFY(session.saveNow());

        WorkspaceSession restored(stateDirectory.path());
        QVERIFY(restored.restore());
        const QVariantList windows = restored.windows();
        QCOMPARE(windows.size(), 2);

        const QVariantMap first = windows.at(0).toMap();
        QCOMPARE(first.value(QStringLiteral("x")).toInt(), 10);
        QCOMPARE(first.value(QStringLiteral("activeTabId")).toString(), firstTab);
        const QVariantList firstTabs = first.value(QStringLiteral("tabs")).toList();
        QCOMPARE(firstTabs.size(), 2);
        QCOMPARE(firstTabs.at(0).toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("first"));
        QCOMPARE(firstTabs.at(0).toMap().value(QStringLiteral("cursorPosition")).toInt(), 2);
        QCOMPARE(firstTabs.at(1).toMap().value(QStringLiteral("id")).toString(), secondTab);

        const QVariantMap second = windows.at(1).toMap();
        QCOMPARE(second.value(QStringLiteral("maximized")).toBool(), true);
        QCOMPARE(second.value(QStringLiteral("activeTabId")).toString(), thirdTab);
    }

    void keepsLocalFilesUniqueAndMovesTheActiveTab() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());

        WorkspaceSession session(stateDirectory.path());
        const QString firstWindow = session.createWindow(0, 0, 900, 700, false);
        const QString firstTab = session.createTab(firstWindow,
                                                   QUrl::fromLocalFile(QStringLiteral("/tmp/one.md")),
                                                   QStringLiteral("one"), 0, 0, 0, false);
        const QString secondTab = session.createTab(firstWindow, QUrl(), QStringLiteral("two"),
                                                    0, 0, 0, false);
        const QString secondWindow = session.createWindow(0, 0, 800, 600, false);

        QCOMPARE(session.findOpenLocalFile(QUrl::fromLocalFile(QStringLiteral("/tmp/one.md"))),
                 firstTab);
        QVERIFY(session.createTab(secondWindow,
                                  QUrl::fromLocalFile(QStringLiteral("/tmp/one.md")),
                                  QStringLiteral("other copy"), 0, 0, 0, false).isEmpty());
        QVERIFY(!session.updateTab(secondWindow, secondTab,
                                   QUrl::fromLocalFile(QStringLiteral("/tmp/one.md")),
                                   QStringLiteral("other copy"), 0, 0, 0, false));
        QCOMPARE(session.windowIdForTab(firstTab), firstWindow);

        QVERIFY(session.moveActiveTab(firstWindow, -1));
        const QVariantList tabs = session.windows().constFirst().toMap()
            .value(QStringLiteral("tabs")).toList();
        QCOMPARE(tabs.at(0).toMap().value(QStringLiteral("id")).toString(), secondTab);
        QCOMPARE(session.windows().constFirst().toMap()
                     .value(QStringLiteral("activeTabId")).toString(), secondTab);
    }

    void rejectsWorkspaceSnapshotsWithDuplicateLocalFiles() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());

        const QJsonObject tab{{QStringLiteral("id"), QStringLiteral("first-tab")},
                              {QStringLiteral("fileUrl"),
                               QUrl::fromLocalFile(QStringLiteral("/tmp/note.md")).toString()},
                              {QStringLiteral("text"), QStringLiteral("text")},
                              {QStringLiteral("cursorPosition"), 0},
                              {QStringLiteral("selectionStart"), 0},
                              {QStringLiteral("selectionEnd"), 0},
                              {QStringLiteral("modified"), false},
                              {QStringLiteral("externalChanged"), false}};
        const QJsonObject duplicateTab{{QStringLiteral("id"), QStringLiteral("second-tab")},
                                       {QStringLiteral("fileUrl"),
                                        QUrl::fromLocalFile(QStringLiteral("/tmp/note.md")).toString()},
                                       {QStringLiteral("text"), QStringLiteral("text")},
                                       {QStringLiteral("cursorPosition"), 0},
                                       {QStringLiteral("selectionStart"), 0},
                                       {QStringLiteral("selectionEnd"), 0},
                                       {QStringLiteral("modified"), false},
                                       {QStringLiteral("externalChanged"), false}};
        const QJsonObject window{{QStringLiteral("id"), QStringLiteral("window")},
                                 {QStringLiteral("x"), 0},
                                 {QStringLiteral("y"), 0},
                                 {QStringLiteral("width"), 900},
                                 {QStringLiteral("height"), 700},
                                 {QStringLiteral("maximized"), false},
                                 {QStringLiteral("activeTabId"), QStringLiteral("first-tab")},
                                 {QStringLiteral("tabs"), QJsonArray{tab, duplicateTab}}};
        QFile file(stateDirectory.filePath(QStringLiteral("session.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 2},
                                              {QStringLiteral("windows"), QJsonArray{window}}})
                       .toJson(QJsonDocument::Compact));
        file.close();

        WorkspaceSession session(stateDirectory.path());
        QVERIFY(!session.restore());
    }

    void removesTheLastTabWithoutCreatingAReplacement() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());

        WorkspaceSession session(stateDirectory.path());
        const QString window = session.createWindow(0, 0, 900, 700, false);
        const QString tab = session.createTab(window, QUrl(), QStringLiteral("draft"),
                                              0, 0, 0, true);

        QVERIFY(session.removeTab(window, tab));
        const QVariantMap restoredWindow = session.windows().constFirst().toMap();
        QVERIFY(restoredWindow.value(QStringLiteral("tabs")).toList().isEmpty());
        QVERIFY(restoredWindow.value(QStringLiteral("activeTabId")).toString().isEmpty());
    }

    void savesWorkspaceWindowGeometry() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());

        WorkspaceSession session(stateDirectory.path());
        const QString windowId = session.createWindow(10, 20, 900, 700, false);
        session.createTab(windowId, QUrl(), QString(), 0, 0, 0, false);

        QVERIFY(session.updateWindowGeometry(windowId, 30, 40, 1200, 800, true));
        const QVariantMap window = session.window(windowId);
        QCOMPARE(window.value(QStringLiteral("x")).toInt(), 30);
        QCOMPARE(window.value(QStringLiteral("y")).toInt(), 40);
        QCOMPARE(window.value(QStringLiteral("width")).toInt(), 1200);
        QCOMPARE(window.value(QStringLiteral("height")).toInt(), 800);
        QVERIFY(window.value(QStringLiteral("maximized")).toBool());
    }

    void backendReadsTabsFromItsWorkspaceWindow() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());

        WorkspaceSession session(stateDirectory.path());
        const QString firstWindow = session.createWindow(0, 0, 900, 700, false);
        const QString firstTab = session.createTab(firstWindow, QUrl(), QStringLiteral("first"),
                                                   2, 1, 2, true);
        const QString secondWindow = session.createWindow(0, 0, 800, 600, false);
        session.createTab(secondWindow, QUrl(), QStringLiteral("second"), 0, 0, 0, false);

        Backend backend(&session, firstWindow);
        QCOMPARE(backend.buffers().size(), 1);
        QCOMPARE(backend.activeBufferId(), firstTab);
        QCOMPARE(backend.activeBufferText(), QStringLiteral("first"));
    }

    void windowManagerCreatesIndependentWritingWindows() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        WorkspaceSession session(stateDirectory.path());
        QQmlEngine engine;
        WindowManager manager(&session, &engine, QUrl::fromLocalFile(mainQmlPath));

        Backend *first = manager.createWindow();
        QVERIFY(first);
        first->newWindow();

        QTRY_COMPARE(manager.windowCount(), 2);
        QCOMPARE(session.windows().size(), 2);
    }

    void windowManagerClosesTheLastTabWithItsWindow() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        WorkspaceSession session(stateDirectory.path());
        QQmlEngine engine;
        WindowManager manager(&session, &engine, QUrl::fromLocalFile(mainQmlPath));
        Backend *backend = manager.createWindow();
        QVERIFY(backend);

        QVERIFY(backend->discardActiveBuffer());
        QTRY_COMPARE(manager.windowCount(), 0);
        QVERIFY(session.windows().isEmpty());
    }

    void windowManagerActivatesAnExistingFileTab() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());
        const QString filePath = stateDirectory.filePath(QStringLiteral("note.md"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("note");
        file.close();

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        WorkspaceSession session(stateDirectory.path());
        QQmlEngine engine;
        WindowManager manager(&session, &engine, QUrl::fromLocalFile(mainQmlPath));
        Backend *first = manager.createWindow();
        Backend *second = manager.createWindow();
        QVERIFY(first);
        QVERIFY(second);

        first->open(QUrl::fromLocalFile(filePath));
        second->open(QUrl::fromLocalFile(filePath));

        QVERIFY(!session.findOpenLocalFile(QUrl::fromLocalFile(filePath)).isEmpty());
        QCOMPARE(session.windows().at(0).toMap().value(QStringLiteral("tabs")).toList().size(), 2);
        QCOMPARE(session.windows().at(1).toMap().value(QStringLiteral("tabs")).toList().size(), 1);
    }

    void windowManagerMarksBackgroundExternalChangesWithoutPrompting() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());
        const QString filePath = stateDirectory.filePath(QStringLiteral("note.md"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("first");
        file.close();

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        WorkspaceSession session(stateDirectory.path());
        QQmlEngine engine;
        WindowManager manager(&session, &engine, QUrl::fromLocalFile(mainQmlPath));
        Backend *backend = manager.createWindow();
        QVERIFY(backend);
        backend->open(QUrl::fromLocalFile(filePath));
        const QString fileTab = backend->activeBufferId();
        backend->newBuffer();
        QSignalSpy externalChangeSpy(backend, &Backend::externalChangeDetected);

        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("second");
        file.close();

        QTRY_VERIFY(session.tab(fileTab).value(QStringLiteral("externalChanged")).toBool());
        QCOMPARE(externalChangeSpy.count(), 0);
        QVERIFY(backend->selectBuffer(fileTab));
        QTRY_COMPARE(externalChangeSpy.count(), 1);
    }

    void windowManagerRestoresWritingWindows() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        WorkspaceSession session(stateDirectory.path());
        const QString firstWindow = session.createWindow(10, 20, 900, 700, false);
        session.createTab(firstWindow, QUrl(), QStringLiteral("first"), 0, 0, 0, true);
        const QString secondWindow = session.createWindow(30, 40, 1200, 800, true);
        session.createTab(secondWindow, QUrl(), QStringLiteral("second"), 0, 0, 0, true);

        QQmlEngine engine;
        WindowManager manager(&session, &engine, QUrl::fromLocalFile(mainQmlPath));

        QCOMPARE(manager.restoreWindows(), 2);
        QCOMPARE(manager.windowCount(), 2);
    }

    void windowManagerRecoversLegacySnapshotsAsUnsavedTabs() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());
        const QString recoveryPath = stateDirectory.filePath(QStringLiteral("recovery-0.json"));
        QFile recovery(recoveryPath);
        QVERIFY(recovery.open(QIODevice::WriteOnly));
        recovery.write(QJsonDocument(QJsonObject{{QStringLiteral("fileUrl"), QString()},
                                                  {QStringLiteral("text"), QStringLiteral("draft")}})
                           .toJson(QJsonDocument::Compact));
        recovery.close();

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        WorkspaceSession session(stateDirectory.path());
        QQmlEngine engine;
        WindowManager manager(&session, &engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY(manager.createWindow());

        QCOMPARE(manager.recoverLegacySnapshots(), 1);
        QCOMPARE(session.windows().constFirst().toMap().value(QStringLiteral("tabs")).toList().size(), 2);
        QVERIFY(!QFile::exists(recoveryPath));
    }

    void preservesBufferTextWhenUpdatingCaret() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());

        BufferSession session(stateDirectory.path());
        const QString id = session.createBuffer();
        QVERIFY(session.updateBuffer(id, QString(), QStringLiteral("first"), 0, 0, 0, true));
        QVERIFY(session.updateBufferCursor(id, 3, 1, 3));
        QVERIFY(session.saveNow());

        BufferSession restored(stateDirectory.path());
        QVERIFY(restored.restore());
        const QVariantMap buffer = restored.buffers().constFirst().toMap();
        QCOMPARE(buffer.value(QStringLiteral("text")), QStringLiteral("first"));
        QCOMPARE(buffer.value(QStringLiteral("cursorPosition")), 3);
        QCOMPARE(buffer.value(QStringLiteral("selectionStart")), 1);
        QCOMPARE(buffer.value(QStringLiteral("selectionEnd")), 3);
    }

    void activatesExistingBufferForSameFile() {
        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());

        BufferSession session(stateDirectory.path());
        const QUrl fileUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/note.md"));
        const QString first = session.openBuffer(fileUrl, QStringLiteral("first"));
        const QString second = session.openBuffer(fileUrl, QStringLiteral("second"));

        QCOMPARE(second, first);
        QCOMPARE(session.buffers().size(), 1);
        QCOMPARE(session.buffers().constFirst().toMap().value(QStringLiteral("text")),
                 QStringLiteral("first"));
    }

    void sessionSnapshotsDoNotModifyUserFiles() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("note.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("original"), qint64(8));
        file.close();

        BufferSession session(directory.filePath(QStringLiteral("state")));
        const QString id = session.openBuffer(QUrl::fromLocalFile(path), QStringLiteral("edited"));
        session.updateBuffer(id, QUrl::fromLocalFile(path).toString(), QStringLiteral("edited"),
                             6, 6, 6, true);
        QVERIFY(session.saveNow());

        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original"));
    }

    void backendCreatesAndSelectsBuffers() {
        Backend backend;

        QCOMPARE(backend.buffers().size(), 1);
        const QString first = backend.activeBufferId();
        const QString second = backend.newBuffer();
        QCOMPARE(backend.buffers().size(), 2);
        QCOMPARE(backend.activeBufferId(), second);
        QVERIFY(backend.selectBuffer(first));
        QCOMPARE(backend.activeBufferId(), first);
    }

    void derivesBufferTitles() {
        Backend backend;

        QCOMPARE(backend.bufferTitle({{QStringLiteral("fileUrl"), QString()},
                                     {QStringLiteral("text"), QString()}}, 0),
                 QStringLiteral("Untitled 1"));
        QCOMPARE(backend.bufferTitle({{QStringLiteral("fileUrl"), QString()},
                                     {QStringLiteral("text"), QStringLiteral("  Draft title\nBody")}}, 1),
                 QStringLiteral("Draft title"));
        QCOMPARE(backend.bufferTitle({{QStringLiteral("fileUrl"), QString()},
                                     {QStringLiteral("text"), QString(40, QChar('a'))}}, 2),
                 QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaa…"));
        QCOMPARE(backend.bufferTitle({{QStringLiteral("fileUrl"),
                                      QStringLiteral("file:///tmp/notes.md")},
                                     {QStringLiteral("text"), QStringLiteral("Ignored")}}, 3),
                 QStringLiteral("notes.md"));
    }

    void scrollsOverflowingTabs() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        for (int index = 0; index < 12; ++index)
            backend.newBuffer();

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        window->setProperty("width", 280);

        QObject *tabFlick = window->findChild<QObject *>(QStringLiteral("tabFlick"));
        QVERIFY(tabFlick);
        QTRY_VERIFY(tabFlick->property("contentWidth").toReal()
                     > tabFlick->property("width").toReal());
        QVERIFY(QMetaObject::invokeMethod(window.get(), "scrollTabs", Q_ARG(QVariant, 1)));
        QTRY_VERIFY(tabFlick->property("contentX").toReal() > 0);
    }

    void hidesTabBarForSingleBuffer() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *tabBar = window->findChild<QObject *>(QStringLiteral("tabBar"));
        QVERIFY(tabBar);
        QVERIFY(!tabBar->property("visible").toBool());
    }

    void cyclesTabsFromWindowShortcuts() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        const QString first = backend.activeBufferId();
        const QString second = backend.newBuffer();
        const QString third = backend.newBuffer();

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QVERIFY(QMetaObject::invokeMethod(window.get(), "selectAdjacentTab", Q_ARG(QVariant, 1)));
        QCOMPARE(backend.activeBufferId(), first);
        QVERIFY(QMetaObject::invokeMethod(window.get(), "selectAdjacentTab", Q_ARG(QVariant, -1)));
        QCOMPARE(backend.activeBufferId(), third);
        QVERIFY(backend.selectBuffer(second));
        QVERIFY(QMetaObject::invokeMethod(window.get(), "selectAdjacentTab", Q_ARG(QVariant, 1)));
        QCOMPARE(backend.activeBufferId(), third);
    }

    void cyclesToHiddenTabsAndScrollsThemIntoView() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        for (int index = 0; index < 12; ++index)
            backend.newBuffer();

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        window->setProperty("width", 280);

        QObject *tabFlick = window->findChild<QObject *>(QStringLiteral("tabFlick"));
        QVERIFY(tabFlick);
        QTRY_VERIFY(tabFlick->property("contentWidth").toReal()
                     > tabFlick->property("width").toReal());
        QCOMPARE(tabFlick->property("contentX").toReal(), 0.0);

        QVERIFY(QMetaObject::invokeMethod(window.get(), "selectAdjacentTab", Q_ARG(QVariant, -1)));
        QTRY_VERIFY(tabFlick->property("contentX").toReal() > 0);
    }

    void scopesDocumentShortcutsToTheirWindow() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        for (const QString &name : {QStringLiteral("newTabShortcut"),
                                    QStringLiteral("closeTabShortcut"),
                                    QStringLiteral("nextTabShortcut"),
                                    QStringLiteral("previousTabShortcut"),
                                    QStringLiteral("moveTabLeftShortcut"),
                                    QStringLiteral("moveTabRightShortcut"),
                                    QStringLiteral("newWindowShortcut")}) {
            QObject *shortcut = window->findChild<QObject *>(name);
            QVERIFY(shortcut);
            QCOMPARE(shortcut->property("context").toInt(), static_cast<int>(Qt::WindowShortcut));
        }
    }

    void restoresActiveTextAndCaretThroughQmlLifecycle() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        QTemporaryDir stateDirectory;
        QVERIFY(stateDirectory.isValid());

        {
            Backend writer(stateDirectory.path());
            QQmlEngine engine;
            engine.rootContext()->setContextProperty(QStringLiteral("backend"), &writer);
            QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
            QScopedPointer<QObject> window(component.create());
            QVERIFY2(window, qPrintable(component.errorString()));

            QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
            QVERIFY(editor);
            QTest::qWait(120);
            QTRY_VERIFY(!writer.buffers().isEmpty());
            editor->setProperty("text", QStringLiteral("First tab"));
            editor->setProperty("cursorPosition", 5);
            QTRY_COMPARE(writer.buffers().constFirst().toMap().value(QStringLiteral("text")),
                         QStringLiteral("First tab"));
            QTRY_COMPARE(writer.buffers().constFirst().toMap()
                             .value(QStringLiteral("cursorPosition")).toInt(),
                         5);
            writer.prepareForApplicationClose();
        }

        BufferSession persisted(stateDirectory.path());
        QVERIFY(persisted.restore());
        QCOMPARE(persisted.buffers().constFirst().toMap()
                     .value(QStringLiteral("cursorPosition")).toInt(),
                 5);

        Backend reader(stateDirectory.path());
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &reader);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QTRY_COMPARE(editor->property("text").toString(), QStringLiteral("First tab"));
        QTRY_COMPARE(reader.activeCursorPosition(), 5);
        QTRY_COMPARE(editor->property("cursorPosition").toInt(), 5);
    }

    void findsInlineMarkdownRanges() {
        const auto markup = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic* and [site](https://example.com)"));
        QCOMPARE(markup.size(), 3);
        QCOMPARE(markup.at(0).content.start, 2);
        QCOMPARE(markup.at(0).content.length, 4);
        QCOMPARE(markup.at(2).content.length, 4);
        QCOMPARE(markup.at(2).markers[0].length, 1);
    }

    void loadsCurrentOmarchyTheme() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());

        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        const QString themeDirectory = homeDirectory.path()
            + QStringLiteral("/.local/state/omarchy/current/theme");
        QVERIFY(QDir().mkpath(themeDirectory));

        QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
        QVERIFY(colorsFile.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray palette(
            "mode = \"light\"\n"
            "accent = \"#112233\"\n"
            "selection = \"#445566\"\n"
            "background = \"#fefefe\"\n"
            "foreground = \"#101010\"\n");
        QCOMPARE(colorsFile.write(palette), qint64(palette.size()));
        colorsFile.close();

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#fefefe"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#101010"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QVERIFY(!backend.darkMode());
    }

    void ignoresFileWatcherEventsForSavedContents() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("first-save.md"));
        Backend backend;
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        backend.saveAs(QUrl::fromLocalFile(path));
        QVERIFY(QFileInfo::exists(path));

        QFile sameContents(path);
        QVERIFY(sameContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        sameContents.close();
        QTest::qWait(100);
        QCOMPARE(externalChangeSpy.count(), 0);

        QFile changedContents(path);
        QVERIFY(changedContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(changedContents.write("changed elsewhere"), qint64(17));
        changedContents.close();
        QTRY_COMPARE(externalChangeSpy.count(), 1);
    }

    void keepsCursorAndSelectionStableAcrossInsertions() {
        const QString mutationsPath = QFINDTESTDATA("../src/EditorMutations.js");
        QVERIFY(!mutationsPath.isEmpty());

        QQmlEngine engine;
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import "EditorMutations.js" as EditorMutations

            TextEdit {
                property string insertionText
                property int insertionCursor
                property string wrappedText
                property int wrappedSelectionStart
                property int wrappedSelectionEnd

                Component.onCompleted: {
                    text = "alpha omega";
                    cursorPosition = 5;
                    EditorMutations.replaceRange(this, 5, 5, "one\r\ntwo");
                    insertionText = text;
                    insertionCursor = cursorPosition;

                    text = "alpha beta omega";
                    select(6, 10);
                    EditorMutations.replaceRange(this, selectionStart, selectionEnd,
                                                 "**beta**", 2, 6);
                    wrappedText = text;
                    wrappedSelectionStart = selectionStart;
                    wrappedSelectionEnd = selectionEnd;
                }
            }
        )QML";
        const QUrl harnessUrl = QUrl::fromLocalFile(
            QFileInfo(mutationsPath).absolutePath() + QStringLiteral("/MutationHarness.qml"));
        component.setData(harness, harnessUrl);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        QCOMPARE(editor->property("insertionText").toString(),
                 QStringLiteral("alphaone\ntwo omega"));
        QCOMPARE(editor->property("insertionCursor").toInt(), 12);
        QCOMPARE(editor->property("wrappedText").toString(),
                 QStringLiteral("alpha **beta** omega"));
        QCOMPARE(editor->property("wrappedSelectionStart").toInt(), 8);
        QCOMPARE(editor->property("wrappedSelectionEnd").toInt(), 12);
    }

    void savesAndOpensFromFooterButtons() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QVERIFY(window->findChild<QObject *>(QStringLiteral("sourceEditor")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("renderedPreview")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("modeToggle")));

        QObject *saveButton = window->findChild<QObject *>(QStringLiteral("saveButton"));
        QObject *openButton = window->findChild<QObject *>(QStringLiteral("openButton"));
        QVERIFY(saveButton);
        QVERIFY(openButton);

        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(saveButton, "clicked"));
        QCOMPARE(saveDialogSpy.count(), 1);

        QSignalSpy openDialogSpy(&backend, &Backend::openDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(openButton, "clicked"));
        QCOMPARE(openDialogSpy.count(), 1);
    }

    void scalesTextWithDesktopTextSize() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 20);

        // `omarchy display text size 16` sets the GNOME factor to 16/12.
        backend.setTextScale(16.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 27);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 27);

        backend.setTextScale(9.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 15);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 15);
    }

    void remembersLastSaveDirectory() {
        QTemporaryDir saveDirectory;
        QVERIFY(saveDirectory.isValid());

        const QString savedPath = saveDirectory.filePath(QStringLiteral("first.md"));
        Backend savedDocument;
        savedDocument.saveAs(QUrl::fromLocalFile(savedPath));

        Backend nextDocument;
        QSignalSpy saveDialogSpy(&nextDocument, &Backend::saveDialogRequested);
        nextDocument.saveAsDialog();
        QCOMPARE(saveDialogSpy.count(), 1);

        const QUrl suggestedUrl = saveDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).absolutePath(),
                 saveDirectory.path());
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).fileName(),
                 QStringLiteral("Untitled.md"));

        QSettings().setValue(QStringLiteral("file/lastSaveDirectory"),
                             saveDirectory.filePath(QStringLiteral("missing")));
        Backend fallbackDocument;
        QSignalSpy fallbackDialogSpy(&fallbackDocument, &Backend::saveDialogRequested);
        fallbackDocument.saveAsDialog();
        const QUrl fallbackUrl = fallbackDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(fallbackUrl.toLocalFile()).absolutePath(), QDir::homePath());
    }

private:
    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"

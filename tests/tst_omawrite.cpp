#include <QtTest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>

#include "backend.h"
#include "markdownhighlighter.h"

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

    void listsOnlyDocumentsAndFolders() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        QVERIFY(QDir(folder.path()).mkdir(QStringLiteral("archive")));
        for (const QString &name : {QStringLiteral("second.md"),
                                    QStringLiteral("first.markdown"),
                                    QStringLiteral("notes.txt"),
                                    QStringLiteral(".hidden.md")}) {
            QFile file(folder.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }

        Backend backend;
        backend.setFolder(QUrl::fromLocalFile(folder.path()));
        QCOMPARE(backend.folderName(), QDir(folder.path()).dirName());
        QVERIFY(backend.folderHasParent());

        // Folders first, then the documents Omawrite can open, by name.
        // Plain text and dotfiles are not writing in this app's sense.
        const QVariantList entries = backend.folderEntries();
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries.at(0).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("archive"));
        QVERIFY(entries.at(0).toMap().value(QStringLiteral("isDir")).toBool());
        QCOMPARE(entries.at(1).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("first.markdown"));
        QVERIFY(!entries.at(1).toMap().value(QStringLiteral("isDir")).toBool());
        QCOMPARE(entries.at(2).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("second.md"));
        QCOMPARE(entries.at(2).toMap().value(QStringLiteral("url")).toUrl(),
                 QUrl::fromLocalFile(folder.filePath(QStringLiteral("second.md"))));

        // An empty folder still lists, so it can be walked out of again.
        backend.setFolder(entries.at(0).toMap().value(QStringLiteral("url")).toUrl());
        QVERIFY(backend.folderEntries().isEmpty());
        backend.openParentFolder();
        QCOMPARE(backend.folderUrl(), QUrl::fromLocalFile(folder.path()));
    }

    void browsesTheOpenDocumentsFolder() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(QStringLiteral("draft.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        Backend backend;
        QSignalSpy folderSpy(&backend, &Backend::folderChanged);
        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(backend.folderUrl(), QUrl::fromLocalFile(folder.path()));
        QCOMPARE(folderSpy.count(), 1);
    }

    void noticesDocumentsWrittenElsewhere() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());

        Backend backend;
        backend.setFolder(QUrl::fromLocalFile(folder.path()));
        QVERIFY(backend.folderEntries().isEmpty());

        QSignalSpy folderSpy(&backend, &Backend::folderChanged);
        QFile file(folder.filePath(QStringLiteral("written-elsewhere.md")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        QTRY_VERIFY(folderSpy.count() > 0);
        QCOMPARE(backend.folderEntries().size(), 1);
    }


    void createsDocumentsAndFoldersWhereItBrowses() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());

        Backend backend;
        backend.setFolder(QUrl::fromLocalFile(folder.path()));

        // A typed name is cleaned the same way a saved document's is, and
        // gains the extension when it is missing.
        const QUrl created = backend.createDocument(QStringLiteral("Field notes"));
        QCOMPARE(created, QUrl::fromLocalFile(folder.filePath(QStringLiteral("Field notes.md"))));
        QVERIFY(QFileInfo::exists(created.toLocalFile()));
        QCOMPARE(Backend::sanitizedEntryName(QStringLiteral("a/b")), QStringLiteral("a-b"));

        // An existing name is reported, never overwritten.
        QVERIFY(backend.createDocument(QStringLiteral("Field notes.md")).isEmpty());
        QCOMPARE(backend.status(), QStringLiteral("Field notes.md already exists."));

        backend.createFolder(QStringLiteral("archive"));
        QVERIFY(QFileInfo(folder.filePath(QStringLiteral("archive"))).isDir());

        const QVariantList entries = backend.folderEntries();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("archive"));
        QCOMPARE(entries.at(1).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Field notes.md"));
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

    void togglesTheFileSidebar() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *sidebar = window->findChild<QObject *>(QStringLiteral("fileSidebar"));
        QObject *filesButton = window->findChild<QObject *>(QStringLiteral("filesButton"));
        QVERIFY(sidebar);
        QVERIFY(filesButton);

        // Closed on launch: the panel takes no width from the writing area.
        QVERIFY(!window->property("sidebarOpen").toBool());
        QCOMPARE(sidebar->property("width").toReal(), 0.0);

        QVERIFY(QMetaObject::invokeMethod(filesButton, "clicked"));
        QVERIFY(window->property("sidebarOpen").toBool());
        QVERIFY(sidebar->property("width").toReal() > 0.0);

        QVERIFY(QMetaObject::invokeMethod(filesButton, "clicked"));
        QCOMPARE(sidebar->property("width").toReal(), 0.0);
    }


    void closesTheSidebarRatherThanReachingIntoIt() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        for (const QString &name : {QStringLiteral("one.md"), QStringLiteral("two.md")}) {
            QFile file(folder.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }

        Backend backend;
        backend.setFolder(QUrl::fromLocalFile(folder.path()));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *sidebar = window->findChild<QObject *>(QStringLiteral("fileSidebar"));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(sidebar);
        QVERIFY(editor);

        // Closed: the key puts the panel there and the keyboard in it.
        QVERIFY(QMetaObject::invokeMethod(window.data(), "toggleSidebar"));
        QVERIFY(window->property("sidebarOpen").toBool());
        QVERIFY(sidebar->property("listHasFocus").toBool());

        // Open with the keyboard back in the text — Esc does this, and so does
        // opening a document. The key takes the panel away rather than
        // interrupting the writing to reach into it.
        QVERIFY(QMetaObject::invokeMethod(editor, "forceActiveFocus"));
        QVERIFY(editor->property("activeFocus").toBool());
        QVERIFY(QMetaObject::invokeMethod(window.data(), "toggleSidebar"));
        QVERIFY(!window->property("sidebarOpen").toBool());
        QCOMPARE(sidebar->property("width").toReal(), 0.0);
        QVERIFY(editor->property("activeFocus").toBool());

        // And from inside the panel it closes just the same.
        QVERIFY(QMetaObject::invokeMethod(window.data(), "toggleSidebar"));
        QVERIFY(sidebar->property("listHasFocus").toBool());
        QVERIFY(QMetaObject::invokeMethod(window.data(), "toggleSidebar"));
        QVERIFY(!window->property("sidebarOpen").toBool());
        QVERIFY(editor->property("activeFocus").toBool());
    }

    void putsTheCaretAtTheEndOfAnOpenedDocument() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(QStringLiteral("long.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("# Title\n\n");
        // Long enough that the end of it is well off the bottom of the window.
        for (int i = 0; i < 200; ++i)
            file.write(QStringLiteral("body line %1\n").arg(i).toUtf8());
        file.close();

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *flick = window->findChild<QObject *>(QStringLiteral("editorFlick"));
        QVERIFY(editor);
        QVERIFY(flick);

        // Writing carries on where the writing stopped, so an opened
        // document hands over its end rather than its beginning.
        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(editor->property("cursorPosition").toInt(),
                 editor->property("length").toInt());

        // And it is drawn there. The document is laid out in stages — the
        // text, then the hidden markers shrinking, then the line height —
        // so a caret measured too early sits against a layout that is no
        // longer on screen, halfway up the page.
        const QRectF caret = editor->property("cursorRectangle").toRectF();
        const qreal textHeight = editor->property("implicitHeight").toReal();
        qDebug() << "PROBE caret" << caret << "textHeight" << textHeight
                 << "contentY" << flick->property("contentY")
                 << "contentHeight" << flick->property("contentHeight");
        QVERIFY(textHeight > 0);
        QVERIFY2(caret.y() > textHeight * 0.9,
                 qPrintable(QStringLiteral("caret at %1 of %2")
                            .arg(caret.y()).arg(textHeight)));
        QVERIFY(flick->property("contentY").toReal() > 0);

        // And again switching between documents, which is how it is really
        // met: the layout in place is the previous document's.
        const QString second = folder.filePath(QStringLiteral("second.md"));
        QFile secondFile(second);
        QVERIFY(secondFile.open(QIODevice::WriteOnly));
        for (int i = 0; i < 60; ++i)
            secondFile.write(QStringLiteral("## Heading %1\n\nwith **bold** and `code` in it\n\n").arg(i).toUtf8());
        secondFile.close();
        backend.open(QUrl::fromLocalFile(second));
        const QRectF caret2 = editor->property("cursorRectangle").toRectF();
        const qreal textHeight2 = editor->property("implicitHeight").toReal();
        qDebug() << "PROBE2 caret" << caret2 << "textHeight" << textHeight2
                 << "contentY" << flick->property("contentY")
                 << "contentHeight" << flick->property("contentHeight");
        QVERIFY2(caret2.y() > textHeight2 * 0.9,
                 qPrintable(QStringLiteral("caret at %1 of %2")
                            .arg(caret2.y()).arg(textHeight2)));

        // Saving names the file but does not reload it, so writing is never
        // interrupted by the caret jumping back to the top.
        editor->setProperty("cursorPosition", 12);
        backend.saveAs(QUrl::fromLocalFile(folder.filePath(QStringLiteral("copy.md"))));
        QCOMPARE(editor->property("cursorPosition").toInt(), 12);
    }

    void walksTheSidebarWithTheKeyboard() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        QVERIFY(QDir(folder.path()).mkdir(QStringLiteral("archive")));
        for (const QString &name : {QStringLiteral("one.md"), QStringLiteral("two.md")}) {
            QFile file(folder.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }

        Backend backend;
        backend.setFolder(QUrl::fromLocalFile(folder.path()));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *sidebar = window->findChild<QObject *>(QStringLiteral("fileSidebar"));
        QVERIFY(sidebar);
        QVERIFY(QMetaObject::invokeMethod(window.data(), "setSidebarOpen",
                                          Q_ARG(QVariant, true)));

        // archive/, one.md, two.md — moving down twice lands on the last row
        // and stays there rather than wrapping.
        QVERIFY(QMetaObject::invokeMethod(sidebar, "selectNext"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "selectNext"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "selectNext"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "activateSelection"));
        QCOMPARE(backend.fileName(), QStringLiteral("two.md"));

        // Enter on a folder walks into it; going up comes back.
        QVERIFY(QMetaObject::invokeMethod(sidebar, "selectPrevious"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "selectPrevious"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "activateSelection"));
        QCOMPARE(backend.folderUrl(),
                 QUrl::fromLocalFile(folder.filePath(QStringLiteral("archive"))));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "goUp"));
        QCOMPARE(backend.folderUrl(), QUrl::fromLocalFile(folder.path()));
    }

    void handsTheKeyboardBackWhenADocumentOpens() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        QVERIFY(QDir(folder.path()).mkdir(QStringLiteral("archive")));
        for (const QString &name : {QStringLiteral("one.md"), QStringLiteral("two.md")}) {
            QFile file(folder.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }

        Backend backend;
        backend.setFolder(QUrl::fromLocalFile(folder.path()));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *sidebar = window->findChild<QObject *>(QStringLiteral("fileSidebar"));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(sidebar);
        QVERIFY(editor);

        // Browsing takes the keyboard, and the caret goes out with it.
        QVERIFY(QMetaObject::invokeMethod(window.data(), "setSidebarOpen",
                                          Q_ARG(QVariant, true)));
        QVERIFY(sidebar->property("listHasFocus").toBool());
        QVERIFY(!editor->property("activeFocus").toBool());

        // Opening a document hands it straight back, so it can be written in
        // without closing the sidebar first.
        QVERIFY(QMetaObject::invokeMethod(sidebar, "selectNext"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "activateSelection"));
        QCOMPARE(backend.fileName(), QStringLiteral("one.md"));
        QVERIFY(editor->property("activeFocus").toBool());
        QVERIFY(window->property("sidebarOpen").toBool());

        // Walking into a folder is not opening a document, so it keeps it.
        QVERIFY(QMetaObject::invokeMethod(window.data(), "setSidebarOpen",
                                          Q_ARG(QVariant, true)));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "selectPrevious"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "activateSelection"));
        QCOMPARE(backend.folderName(), QStringLiteral("archive"));
        QVERIFY(sidebar->property("listHasFocus").toBool());
        QVERIFY(!editor->property("activeFocus").toBool());

        // Coming back up, the panel starts from the document being written.
        QVERIFY(QMetaObject::invokeMethod(sidebar, "goUp"));
        QTRY_COMPARE(sidebar->property("selectedName").toString(),
                     QStringLiteral("one.md"));

        // Unsaved work asks nothing: the document being left is written out
        // on the way, and the keyboard lands in the one that opens.
        editor->setProperty("text", QStringLiteral("a draft"));
        QVERIFY(backend.modified());
        QVERIFY(QMetaObject::invokeMethod(sidebar, "selectNext"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "activateSelection"));
        QCOMPARE(backend.fileName(), QStringLiteral("two.md"));
        QVERIFY(editor->property("activeFocus").toBool());
        QVERIFY(!backend.modified());

        QFile left(folder.filePath(QStringLiteral("one.md")));
        QVERIFY(left.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(left.readAll()), QStringLiteral("a draft"));
        left.close();

        // A document that cannot be read is never opened, so the keyboard
        // stays with the browsing rather than following a document that
        // never arrived.
        QVERIFY(QMetaObject::invokeMethod(window.data(), "setSidebarOpen",
                                          Q_ARG(QVariant, true)));
        const QUrl missing = QUrl::fromLocalFile(
            folder.filePath(QStringLiteral("missing.md")));
        QVERIFY(QMetaObject::invokeMethod(window.data(), "requestOpen",
                                          Q_ARG(QVariant, QVariant(missing))));
        QCOMPARE(backend.fileName(), QStringLiteral("two.md"));
        QVERIFY(sidebar->property("listHasFocus").toBool());
        QVERIFY(!editor->property("activeFocus").toBool());
    }

    void autosavesOnceTheTypingStops() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(QStringLiteral("note.md"));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.close();

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("written and left alone"));
        QVERIFY(backend.modified());

        // A pause in the writing is the save; nothing has to be pressed.
        QTRY_VERIFY(!backend.modified());
        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(written.readAll()),
                 QStringLiteral("written and left alone"));
        written.close();

        // Closing does not wait out the pause, and does not ask either.
        editor->setProperty("text", QStringLiteral("one last thought"));
        QVERIFY(backend.modified());
        QVERIFY(QMetaObject::invokeMethod(window.data(), "close"));
        QVERIFY(!backend.modified());
        QFile onClose(path);
        QVERIFY(onClose.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(onClose.readAll()),
                 QStringLiteral("one last thought"));
    }

    void keepsTheWorkWhenTheFileCannotBeWritten() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(QStringLiteral("locked.md"));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.write("on disk");
        seed.close();
        // A real document to switch to, so the refusal is what stops the
        // switch rather than a target that was never openable.
        const QString other = folder.filePath(QStringLiteral("other.md"));
        QFile neighbour(other);
        QVERIFY(neighbour.open(QIODevice::WriteOnly));
        neighbour.write("somewhere else");
        neighbour.close();

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("work that must not vanish"));
        QVERIFY(backend.modified());

        // A directory that cannot be written to is the same to QSaveFile as any
        // other failed write.
        QVERIFY(QFile::setPermissions(folder.path(), QFileDevice::ReadOwner
                                                     | QFileDevice::ExeOwner));

        QVERIFY2(!backend.saveBeforeLeaving(), "a failed save must report itself");

        // Leaving is refused, so the writer is still looking at their own text.
        QVERIFY(QMetaObject::invokeMethod(window.data(), "requestOpen",
                                          Q_ARG(QVariant, QVariant(QUrl::fromLocalFile(other)))));
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("work that must not vanish"));
        QVERIFY(backend.modified());

        QVERIFY(QFile::setPermissions(folder.path(), QFileDevice::ReadOwner
                                                     | QFileDevice::WriteOwner
                                                     | QFileDevice::ExeOwner));
    }

    void doesNotAutosaveOverAnUnansweredExternalChange() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(QStringLiteral("shared.md"));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.write("original");
        seed.close();

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("my version"));
        QVERIFY(backend.modified());

        QSignalSpy conflict(&backend, &Backend::externalChangeDetected);
        QFile outside(path);
        QVERIFY(outside.open(QIODevice::WriteOnly));
        outside.write("their version");
        outside.close();
        QTRY_COMPARE(conflict.count(), 1);

        // The file on disk is not ours to overwrite until that is answered, so
        // the work waits in a draft rather than landing on top of it.
        QVERIFY(QMetaObject::invokeMethod(&backend, "saveNow"));
        QFile after(path);
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(after.readAll()), QStringLiteral("their version"));
        after.close();
        QVERIFY(backend.modified());

        // Pressing Ctrl+S does not pre-empt the question either: the prompt is
        // on screen asking which version to keep.
        backend.save();
        QFile stillTheirs(path);
        QVERIFY(stillTheirs.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(stillTheirs.readAll()),
                 QStringLiteral("their version"));
        stillTheirs.close();

        // Saving somewhere else is not the contested file, so it goes through.
        const QString copy = folder.filePath(QStringLiteral("copy.md"));
        backend.saveAs(QUrl::fromLocalFile(copy));
        QVERIFY(QFileInfo::exists(copy));

        // The prompt is the only way out, so it cannot be dismissed. Escape
        // would have to mean keep, or reload, or neither, and each of those
        // answers the question on the writer's behalf.
        QObject *prompt = window->findChild<QObject *>(
            QStringLiteral("externalChangeDialog"));
        QVERIFY(prompt);
        // Asserted as configuration rather than by pressing Escape: the window
        // is never shown here, so a synthetic key never reaches the popup and
        // the behavioural version of this passes whatever the policy says.
        QCOMPARE(prompt->property("closePolicy").toInt(), 0);  // Popup.NoAutoClose

        // A conflict is about one file. Opening another document ends it,
        // rather than following the writer and refusing to save that one too.
        const QString elsewhere = folder.filePath(QStringLiteral("elsewhere.md"));
        QFile other(elsewhere);
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.close();
        backend.open(QUrl::fromLocalFile(elsewhere));
        editor->setProperty("text", QStringLiteral("a different document"));
        QVERIFY(QMetaObject::invokeMethod(&backend, "saveNow"));
        QFile unrelated(elsewhere);
        QVERIFY(unrelated.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(unrelated.readAll()),
                 QStringLiteral("a different document"));
        unrelated.close();

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("my version"));

        // Once it is answered, keeping your version saves over it as asked.
        // A second outside change still gets through. This one replaces the
        // file the way another editor's atomic save does, which takes the old
        // inode — and the watched path with it — out from under the watcher.
        QSignalSpy second(&backend, &Backend::externalChangeDetected);
        QSaveFile replacement(path);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        replacement.write("changed once more");
        QVERIFY(replacement.commit());
        QTRY_COMPARE(second.count(), 1);

        backend.keepExternalVersion();
        QVERIFY(QMetaObject::invokeMethod(&backend, "saveNow"));
        QFile kept(path);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(kept.readAll()), QStringLiteral("my version"));
    }

    void keepsTheGuardWhenTheNextDocumentWillNotOpen() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(QStringLiteral("contested.md"));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.write("original");
        seed.close();

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("my version"));

        QSignalSpy conflict(&backend, &Backend::externalChangeDetected);
        QFile outside(path);
        QVERIFY(outside.open(QIODevice::WriteOnly));
        outside.write("their version");
        outside.close();
        QTRY_COMPARE(conflict.count(), 1);

        // An open that fails replaces nothing, so the document still on screen
        // is the contested one and its guard has to stand.
        backend.open(QUrl::fromLocalFile(folder.filePath(QStringLiteral("missing.md"))));
        backend.save();
        QVERIFY(QMetaObject::invokeMethod(&backend, "saveNow"));
        QFile after(path);
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(after.readAll()), QStringLiteral("their version"));
    }

    void asksAgainWhenTheReloadItselfFails() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(QStringLiteral("vanishing.md"));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.write("original");
        seed.close();

        Backend backend;
        backend.open(QUrl::fromLocalFile(path));

        QSignalSpy conflict(&backend, &Backend::externalChangeDetected);
        QSaveFile outside(path);
        QVERIFY(outside.open(QIODevice::WriteOnly));
        outside.write("theirs");
        QVERIFY(outside.commit());
        QTRY_COMPARE(conflict.count(), 1);

        // Taking their version cannot be done if there is no longer a their
        // version to take. That has answered nothing, and the prompt has
        // already closed itself, so it is raised again rather than leaving the
        // guard standing with nothing able to clear it.
        QVERIFY(QFile::remove(path));
        backend.reloadFromDisk();
        QCOMPARE(conflict.count(), 2);
        QCOMPARE(conflict.last().at(0).toBool(), true);  // reported as deleted
    }

    void asksAgainWhenTheFileIsReplacedTwice() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(QStringLiteral("contested.md"));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.write("original");
        seed.close();

        Backend backend;
        backend.open(QUrl::fromLocalFile(path));

        QSignalSpy conflict(&backend, &Backend::externalChangeDetected);

        // An atomic save from another editor replaces the file rather than
        // rewriting it, which takes the watched inode away with it.
        QSaveFile first(path);
        QVERIFY(first.open(QIODevice::WriteOnly));
        first.write("theirs");
        QVERIFY(first.commit());
        QTRY_COMPARE(conflict.count(), 1);

        // Nothing has answered the prompt, and nothing has re-opened the file.
        // A second replacement still has to reach the writer, or the one
        // chance to ask went with the first inode.
        QSaveFile again(path);
        QVERIFY(again.open(QIODevice::WriteOnly));
        again.write("theirs, again");
        QVERIFY(again.commit());
        QTRY_COMPARE(conflict.count(), 2);
    }

    void reloadingAlsoEndsTheConflict() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(QStringLiteral("shared.md"));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.write("original");
        seed.close();

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("mine"));

        QSignalSpy conflict(&backend, &Backend::externalChangeDetected);
        QFile outside(path);
        QVERIFY(outside.open(QIODevice::WriteOnly));
        outside.write("theirs");
        outside.close();
        QTRY_COMPARE(conflict.count(), 1);

        // Taking their version is the other answer, and saving resumes on it.
        backend.reloadFromDisk();
        QCOMPARE(editor->property("text").toString(), QStringLiteral("theirs"));
        editor->setProperty("text", QStringLiteral("theirs, then mine"));
        QVERIFY(QMetaObject::invokeMethod(&backend, "saveNow"));
        QFile after(path);
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(after.readAll()),
                 QStringLiteral("theirs, then mine"));
    }

    void namesAnUntitledDocumentFromItsFirstLine() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());

        Backend backend;
        backend.setFolder(QUrl::fromLocalFile(folder.path()));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // A document that was never named takes one from its first line when
        // it is left, rather than stopping the writer to ask for it.
        editor->setProperty("text", QStringLiteral("Field notes\n\nbody"));
        QVERIFY(QMetaObject::invokeMethod(&backend, "saveBeforeLeaving"));
        QCOMPARE(backend.fileName(), QStringLiteral("Field notes.md"));
        QFile named(folder.filePath(QStringLiteral("Field notes.md")));
        QVERIFY(named.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(named.readAll()),
                 QStringLiteral("Field notes\n\nbody"));
        named.close();

        // The name gives way rather than the writing: a second note opening
        // on the same line lands beside the first.
        Backend second;
        second.setFolder(QUrl::fromLocalFile(folder.path()));
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &second);
        QScopedPointer<QObject> secondWindow(component.create());
        QVERIFY2(secondWindow, qPrintable(component.errorString()));
        QObject *secondEditor =
            secondWindow->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(secondEditor);
        secondEditor->setProperty("text", QStringLiteral("Field notes\n\nagain"));
        QVERIFY(QMetaObject::invokeMethod(&second, "saveBeforeLeaving"));
        QCOMPARE(second.fileName(), QStringLiteral("Field notes 2.md"));
    }

    void doesNotSettleForAnOlderDraftWhenClosing() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        QTemporaryDir state;
        QVERIFY(state.isValid());
        const QString path = folder.filePath(QStringLiteral("draft.md"));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.close();

        qputenv("XDG_DATA_HOME", state.path().toUtf8());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));

        // A draft of an older version, made when the document itself could not
        // be written but the draft still could.
        QVERIFY(QFile::setPermissions(folder.path(), QFileDevice::ReadOwner
                                                     | QFileDevice::ExeOwner));
        editor->setProperty("text", QStringLiteral("old version"));
        QVERIFY(QMetaObject::invokeMethod(&backend, "saveNow"));

        const QString appData =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(appData.startsWith(state.path()));
        QDir drafts(appData);
        const QStringList written = drafts.entryList({QStringLiteral("*.json")}, QDir::Files);
        QCOMPARE(written.size(), 1);
        const QString draftPath = drafts.filePath(written.first());

        // Newer work, and now nowhere at all to put it.
        editor->setProperty("text", QStringLiteral("new version"));
        QVERIFY(QFile::setPermissions(appData, QFileDevice::ReadOwner
                                               | QFileDevice::ExeOwner));

        // The old draft is still sitting there, but it holds the wrong text, so
        // it is no reason to let the window take the new text with it.
        QVERIFY2(!backend.saveBeforeClosing(),
                 "an older draft does not stand in for this attempt");
        QVERIFY(QMetaObject::invokeMethod(window.data(), "close"));
        QVERIFY(window->property("visible").toBool());

        QFile stale(draftPath);
        QVERIFY(stale.open(QIODevice::ReadOnly));
        QVERIFY2(QString::fromUtf8(stale.readAll()).contains(QStringLiteral("old version")),
                 "the draft on disk is the older one, which is the point");
        stale.close();

        QVERIFY(QFile::setPermissions(appData, QFileDevice::ReadOwner
                                               | QFileDevice::WriteOwner
                                               | QFileDevice::ExeOwner));
        QVERIFY(QFile::setPermissions(folder.path(), QFileDevice::ReadOwner
                                                     | QFileDevice::WriteOwner
                                                     | QFileDevice::ExeOwner));
        qunsetenv("XDG_DATA_HOME");
    }

    void refusesTheFirstCloseWhenTheWorkFitsNowhere() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        QTemporaryDir state;
        QVERIFY(state.isValid());
        const QString path = folder.filePath(QStringLiteral("stuck.md"));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.close();

        // A recovery slot of its own, so taking it away takes away the last
        // place the work could go.
        qputenv("XDG_DATA_HOME", state.path().toUtf8());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("the only copy"));
        QVERIFY(backend.modified());

        const QString appData =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(appData.startsWith(state.path()));
        QVERIFY(QFile::setPermissions(folder.path(), QFileDevice::ReadOwner
                                                     | QFileDevice::ExeOwner));
        QVERIFY(QFile::setPermissions(appData, QFileDevice::ReadOwner
                                               | QFileDevice::ExeOwner));

        QVERIFY2(!backend.saveBeforeClosing(),
                 "with nowhere to write, the work reached nowhere");

        // The first close is refused rather than dropping the only copy.
        QVERIFY(QMetaObject::invokeMethod(window.data(), "close"));
        QVERIFY(window->property("visible").toBool());
        QVERIFY(window->property("closeAnyway").toBool());

        // That override belongs only to the text from the refused attempt.
        // Continuing to write revokes it, so a later close tries persistence
        // again instead of silently discarding the newer text.
        editor->setProperty("text", QStringLiteral("newer only copy"));
        QVERIFY(!window->property("closeAnyway").toBool());
        QVERIFY(QMetaObject::invokeMethod(window.data(), "close"));
        QVERIFY(window->property("visible").toBool());
        QVERIFY(window->property("closeAnyway").toBool());

        QVERIFY(QFile::setPermissions(appData, QFileDevice::ReadOwner
                                               | QFileDevice::WriteOwner
                                               | QFileDevice::ExeOwner));
        QVERIFY(QFile::setPermissions(folder.path(), QFileDevice::ReadOwner
                                                     | QFileDevice::WriteOwner
                                                     | QFileDevice::ExeOwner));
        qunsetenv("XDG_DATA_HOME");
    }

    void discardsAnEmptyUntitledDocument() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());

        Backend backend;
        backend.setFolder(QUrl::fromLocalFile(folder.path()));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // Nothing was written, so there is nothing to name and nothing to keep.
        editor->setProperty("text", QStringLiteral("   \n\n  "));
        QVERIFY(QMetaObject::invokeMethod(&backend, "saveBeforeLeaving"));
        QVERIFY(!backend.modified());
        QCOMPARE(QDir(folder.path())
                     .entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 0);
    }

    void keepsTheSidebarSelectionWhenTheFolderIsReRead() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        for (const QString &name : {QStringLiteral("one.md"), QStringLiteral("two.md")}) {
            QFile file(folder.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }

        Backend backend;
        backend.setFolder(QUrl::fromLocalFile(folder.path()));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *sidebar = window->findChild<QObject *>(QStringLiteral("fileSidebar"));
        QVERIFY(sidebar);
        QVERIFY(QMetaObject::invokeMethod(window.data(), "setSidebarOpen",
                                          Q_ARG(QVariant, true)));

        QVERIFY(QMetaObject::invokeMethod(sidebar, "selectNext"));
        QCOMPARE(sidebar->property("selectedName").toString(), QStringLiteral("two.md"));

        // The folder is re-read whenever anything in it changes — a save is
        // enough — and the rows can move; the keyboard should stay on the
        // row it was on rather than be thrown back to the top.
        QVERIFY(QMetaObject::invokeMethod(&backend, "createFolder",
                                          Q_ARG(QString, QStringLiteral("archive"))));
        QTRY_COMPARE(sidebar->property("selectedName").toString(),
                     QStringLiteral("two.md"));

        // Reopening the panel starts from the open document instead.
        backend.open(QUrl::fromLocalFile(folder.filePath(QStringLiteral("one.md"))));
        QVERIFY(QMetaObject::invokeMethod(window.data(), "setSidebarOpen",
                                          Q_ARG(QVariant, false)));
        QVERIFY(QMetaObject::invokeMethod(window.data(), "setSidebarOpen",
                                          Q_ARG(QVariant, true)));
        QCOMPARE(sidebar->property("selectedName").toString(), QStringLiteral("one.md"));
    }

    void createsAndOpensADocumentFromTheSidebar() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());

        Backend backend;
        backend.setFolder(QUrl::fromLocalFile(folder.path()));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *sidebar = window->findChild<QObject *>(QStringLiteral("fileSidebar"));
        QObject *nameField = window->findChild<QObject *>(QStringLiteral("newEntryField"));
        QVERIFY(sidebar);
        QVERIFY(nameField);
        QVERIFY(QMetaObject::invokeMethod(window.data(), "setSidebarOpen",
                                          Q_ARG(QVariant, true)));

        QVERIFY(QMetaObject::invokeMethod(sidebar, "beginNewDocument"));
        QVERIFY(sidebar->property("creating").toBool());
        nameField->setProperty("text", QStringLiteral("Field notes"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "commitNewEntry"));
        QVERIFY(!sidebar->property("creating").toBool());

        // The new document is created and opened, ready to be written in,
        // with the selection left on it rather than back at the top.
        QVERIFY(QFileInfo::exists(folder.filePath(QStringLiteral("Field notes.md"))));
        QCOMPARE(backend.fileName(), QStringLiteral("Field notes.md"));
        QCOMPARE(sidebar->property("selectedName").toString(),
                 QStringLiteral("Field notes.md"));

        QVERIFY(QMetaObject::invokeMethod(sidebar, "beginNewFolder"));
        nameField->setProperty("text", QStringLiteral("archive"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "commitNewEntry"));
        QVERIFY(QFileInfo(folder.filePath(QStringLiteral("archive"))).isDir());
        QCOMPARE(sidebar->property("selectedName").toString(), QStringLiteral("archive"));

        // An abandoned name creates nothing.
        QVERIFY(QMetaObject::invokeMethod(sidebar, "beginNewDocument"));
        nameField->setProperty("text", QStringLiteral("discarded"));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "cancelNewEntry"));
        QVERIFY(!QFileInfo::exists(folder.filePath(QStringLiteral("discarded.md"))));
    }

    void followsThePointerWhenTheEdgeIsDragged() {
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *sidebar = window->findChild<QObject *>(QStringLiteral("fileSidebar"));
        QVERIFY(sidebar);
        QVERIFY(QMetaObject::invokeMethod(window.data(), "setSidebarOpen",
                                          Q_ARG(QVariant, true)));
        window->setProperty("width", 1280);
        QCOMPARE(sidebar->property("width").toReal(), 240.0);

        // The pointer starts on the edge, where the handle is.
        QVERIFY(QMetaObject::invokeMethod(sidebar, "beginResize", Q_ARG(QVariant, 240.0)));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "resizeTo", Q_ARG(QVariant, 300.0)));
        QCOMPARE(window->property("sidebarLogicalWidth").toInt(), 300);

        // A pointer that has not moved asks for the width it already has. The
        // handle rides the edge it moves, so a width measured against it used
        // to come back different every time it was asked — which is what the
        // drag twitching was.
        QVERIFY(QMetaObject::invokeMethod(sidebar, "resizeTo", Q_ARG(QVariant, 300.0)));
        QCOMPARE(window->property("sidebarLogicalWidth").toInt(), 300);

        // One pixel of pointer, one pixel of panel, both ways.
        QVERIFY(QMetaObject::invokeMethod(sidebar, "resizeTo", Q_ARG(QVariant, 360.0)));
        QCOMPARE(window->property("sidebarLogicalWidth").toInt(), 360);
        QVERIFY(QMetaObject::invokeMethod(sidebar, "resizeTo", Q_ARG(QVariant, 200.0)));
        QCOMPARE(window->property("sidebarLogicalWidth").toInt(), 200);

        // Widths are kept at text scale 1, and the pointer is not: a drag to a
        // device pixel lands on the logical width under it, once divided.
        backend.setTextScale(1.25);
        QCOMPARE(sidebar->property("width").toReal(), 250.0);
        QVERIFY(QMetaObject::invokeMethod(sidebar, "beginResize", Q_ARG(QVariant, 250.0)));
        QVERIFY(QMetaObject::invokeMethod(sidebar, "resizeTo", Q_ARG(QVariant, 500.0)));
        QCOMPARE(window->property("sidebarLogicalWidth").toInt(), 400);
    }

    void keepsTheWritingColumnWhenDraggedWider() {
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *sidebar = window->findChild<QObject *>(QStringLiteral("fileSidebar"));
        QVERIFY(sidebar);
        QVERIFY(QMetaObject::invokeMethod(window.data(), "setSidebarOpen",
                                          Q_ARG(QVariant, true)));
        window->setProperty("width", 1280);

        QVERIFY(QMetaObject::invokeMethod(sidebar, "requestLogicalWidth",
                                          Q_ARG(QVariant, 380)));
        QCOMPARE(window->property("sidebarLogicalWidth").toInt(), 380);

        // Dragging past either end is held at the limit, and the wide end
        // always leaves the editor its minimum.
        QVERIFY(QMetaObject::invokeMethod(sidebar, "requestLogicalWidth",
                                          Q_ARG(QVariant, 40)));
        QCOMPARE(window->property("sidebarLogicalWidth").toInt(),
                 sidebar->property("minimumLogicalWidth").toInt());

        QVERIFY(QMetaObject::invokeMethod(sidebar, "requestLogicalWidth",
                                          Q_ARG(QVariant, 5000)));
        QCOMPARE(window->property("sidebarLogicalWidth").toInt(), 1280 - 420);

        // The drag writes the width once it is let go, not on every frame.
        QVERIFY(QMetaObject::invokeMethod(sidebar, "requestLogicalWidth",
                                          Q_ARG(QVariant, 300)));
        QCOMPARE(backend.sidebarWidth(), 240);
        QVERIFY(QMetaObject::invokeMethod(sidebar, "widthCommitted"));
        QCOMPARE(backend.sidebarWidth(), 300);
    }

private:
    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"

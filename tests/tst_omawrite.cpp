#include <QtTest>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>

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

    void breaksLinesOnReturn() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto load = [&](const QString &content, int position) {
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };
        auto returnKey = [&] { QTest::keyClick(window, Qt::Key_Return); };

        // Ending a paragraph leaves the blank line that separates it from the
        // next one.
        load(QStringLiteral("one\n\ntwo"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n\n"));

        // On a line that is already blank there is nothing to separate from,
        // so Return is worth one line, not two.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));

        // An empty document is a blank line too.
        load(QString(), 0);
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n"));

        // Whitespace left behind on a line still reads as blank.
        load(QStringLiteral("one\n  \ntwo"), 5);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n \n \ntwo"));

        // A selection dragged right to left leaves the caret on the blank line
        // it began on, but the break lands on the line it leaves behind.
        load(QStringLiteral("one\n\ntwo"), 7);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 4));
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\no"));

        // A list carries its marker down, and an item left empty drops the
        // marker to end the list.
        load(QStringLiteral("- item"), 6);
        returnKey();
        QCOMPARE(text(), QStringLiteral("- item\n- "));
        returnKey();
        QCOMPARE(text(), QStringLiteral("- item\n\n"));

        // Inside a code fence every line is its own, blank ones included.
        load(QStringLiteral("```\ncode\n```"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("```\ncode\n\n```"));
        returnKey();
        QCOMPARE(text(), QStringLiteral("```\ncode\n\n\n```"));
    }

    void closesParagraphBreaksOnBackspace() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto caret = [&] { return editor->property("cursorPosition").toInt(); };
        auto load = [&](const QString &content, int position) {
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };
        auto returnKey = [&] { QTest::keyClick(window, Qt::Key_Return); };
        auto backspaceKey = [&] { QTest::keyClick(window, Qt::Key_Backspace); };

        // Backspace at the head of a paragraph closes the whole break above
        // it, so the two paragraphs join in one press rather than two, and
        // the caret lands where they meet.
        load(QStringLiteral("one\n\ntwo"), 5);
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("onetwo"));
        QCOMPARE(caret(), 3);

        // Ending a paragraph writes both breaks at once, and one Backspace
        // takes both back. A Return on the blank line of "one\n\n\ntwo" can
        // leave this very document with the caret in this very place, so it
        // is not provenance that decides here: it is that the gap is left
        // standing either way, and this is much the commoner press.
        load(QStringLiteral("one\n\ntwo"), 3);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));
        QCOMPARE(caret(), 3);

        // Which is what a Return inside a wide gap costs: the same rule reads
        // this as the paragraph-ending press it cannot be told apart from, so
        // the writer gets back one blank line fewer than they had. The gap
        // still separates the paragraphs, and one more Return returns it.
        load(QStringLiteral("one\n\n\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        QCOMPARE(caret(), 3);

        // Return on a blank line writes a single break, so Backspace must
        // take back a single break too. Closing the pair behind the caret
        // would swallow the separator that was there beforehand and leave
        // the two paragraphs with nothing between them.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // The same holds however far the gap has grown: each Return is worth
        // one Backspace.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));
        backspaceKey();
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // A line with nothing but spaces on it is a blank line, and Return
        // above one writes a single break there too.
        load(QStringLiteral("one\n  \ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n  \ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n  \ntwo"));

        // On the blank line a document ends with, the two Returns leave the
        // same text behind and no reading of it says which was pressed.
        // Backspace takes one break, because taking two would carry off the
        // line the document already ended with.
        load(QStringLiteral("one\n"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n"));

        // Which costs the paragraph that ends a document a second press,
        // and that is the whole of what it costs: nothing is lost on the
        // way through.
        load(QStringLiteral("one\n\ntwo"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // An empty document is a blank line, so it takes two Returns to open
        // a gap and two Backspaces to close it again.
        load(QString(), 0);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("\n"));
        backspaceKey();
        QCOMPARE(text(), QString());

        // A document holding nothing but spaces is a blank line as well, and
        // the spaces are text: they must still be there at the end of it.
        load(QStringLiteral("   "), 3);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("   \n\n"));
        backspaceKey();
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("   "));

        // Return at the very head of a document writes both breaks, since
        // the paragraph below it is not blank, and one Backspace takes them.
        load(QStringLiteral("two"), 0);
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("two"));
        QCOMPARE(caret(), 0);

        // Spaces left on the line above a break do not make the break any
        // less of one: Return between them and the paragraph below wrote
        // both of these, so Backspace still takes both.
        load(QStringLiteral("one\n  \n\ntwo"), 8);
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n  two"));
        QCOMPARE(caret(), 6);
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

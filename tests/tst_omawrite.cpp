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
        QQmlEngine engine;
        QQmlComponent component(&engine);
        QScopedPointer<QObject> editor(createJsHarness(component, R"QML(
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
        )QML", QStringLiteral("MutationHarness")));
        QVERIFY2(editor, qPrintable(component.errorString()));

        QCOMPARE(editor->property("insertionText").toString(),
                 QStringLiteral("alphaone\ntwo omega"));
        QCOMPARE(editor->property("insertionCursor").toInt(), 12);
        QCOMPARE(editor->property("wrappedText").toString(),
                 QStringLiteral("alpha **beta** omega"));
        QCOMPARE(editor->property("wrappedSelectionStart").toInt(), 8);
        QCOMPARE(editor->property("wrappedSelectionEnd").toInt(), 12);
    }

    void nestsListItemsWithTab() {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        QScopedPointer<QObject> editor(createListEditor(component));
        QVERIFY2(editor, qPrintable(component.errorString()));

        const auto indent = [&](const QString &source, int from, int to, int direction) {
            QVariant handled;
            QMetaObject::invokeMethod(editor.data(), "indent", Q_RETURN_ARG(QVariant, handled),
                                      Q_ARG(QVariant, source), Q_ARG(QVariant, from),
                                      Q_ARG(QVariant, to), Q_ARG(QVariant, direction));
            return handled.toBool();
        };
        const auto result = [&] { return editor->property("resultText").toString(); };
        const auto caret = [&] { return editor->property("resultCursor").toInt(); };

        // A bullet nests under the item above it, and the caret rides along.
        QVERIFY(indent(QStringLiteral("- one\n- two"), 11, 11, 1));
        QCOMPARE(result(), QStringLiteral("- one\n  - two"));
        QCOMPARE(caret(), 13);

        // Numbers hang off the wider `1. ` marker and renumber around the move.
        QVERIFY(indent(QStringLiteral("1. one\n2. two\n3. three"), 12, 12, 1));
        QCOMPARE(result(), QStringLiteral("1. one\n   1. two\n2. three"));

        // Shift+Tab puts it back beside the item it hung under.
        QVERIFY(indent(QStringLiteral("1. one\n   1. two\n2. three"), 15, 15, -1));
        QCOMPARE(result(), QStringLiteral("1. one\n2. two\n3. three"));

        // Nesting goes as deep as the list does.
        QVERIFY(indent(QStringLiteral("- one\n  - two\n  - three\n- four"), 22, 22, 1));
        QCOMPARE(result(), QStringLiteral("- one\n  - two\n    - three\n- four"));

        // Children follow the item they hang under.
        QVERIFY(indent(QStringLiteral("- one\n  - two\n    - deep\n- four"), 12, 12, -1));
        QCOMPARE(result(), QStringLiteral("- one\n- two\n  - deep\n- four"));

        // A selection nests every item it touches, keeping their relative depth.
        QVERIFY(indent(QStringLiteral("- one\n- two\n- three"), 8, 18, 1));
        QCOMPARE(result(), QStringLiteral("- one\n  - two\n  - three"));

        // A list that deliberately starts at 3 keeps its numbering.
        QVERIFY(indent(QStringLiteral("3. a\n4. b\n5. c"), 8, 8, 1));
        QCOMPARE(result(), QStringLiteral("3. a\n   1. b\n4. c"));

        // Nothing to nest under, nothing to lift out of, and no lists inside a
        // fence: Tab is left to whatever it did before.
        QVERIFY(!indent(QStringLiteral("- one"), 5, 5, 1));
        QVERIFY(!indent(QStringLiteral("- one\n- two"), 11, 11, -1));
        QVERIFY(!indent(QStringLiteral("just text"), 4, 4, 1));
        QVERIFY(!indent(QStringLiteral("```\n- one\n- two\n"), 15, 15, 1));
    }

    void continuesListsAcrossReturn() {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        QScopedPointer<QObject> editor(createListEditor(component));
        QVERIFY2(editor, qPrintable(component.errorString()));

        const auto pressReturn = [&](const QString &source, int cursor) {
            QVariant handled;
            QMetaObject::invokeMethod(editor.data(), "pressReturn",
                                      Q_RETURN_ARG(QVariant, handled),
                                      Q_ARG(QVariant, source), Q_ARG(QVariant, cursor));
            return handled.toBool();
        };
        const auto result = [&] { return editor->property("resultText").toString(); };
        const auto caret = [&] { return editor->property("resultCursor").toInt(); };

        // A new item keeps the depth of the one it follows.
        QVERIFY(pressReturn(QStringLiteral("- one\n  - two"), 13));
        QCOMPARE(result(), QStringLiteral("- one\n  - two\n  - "));
        QCOMPARE(caret(), 18);

        // Inserting into an ordered list renumbers what follows.
        QVERIFY(pressReturn(QStringLiteral("1. one\n2. two\n3. three"), 6));
        QCOMPARE(result(), QStringLiteral("1. one\n2. \n3. two\n4. three"));
        QCOMPARE(caret(), 10);

        // Return on an empty nested item lifts it a level instead of ending the
        // list; the outermost level is left to drop out of the list entirely.
        QVERIFY(pressReturn(QStringLiteral("- one\n  - "), 10));
        QCOMPARE(result(), QStringLiteral("- one\n- "));
        QCOMPARE(caret(), 8);
        QVERIFY(pressReturn(QStringLiteral("- one\n- "), 8));
        QCOMPARE(result(), QStringLiteral("- one\n\n"));

        // Blockquotes carry their marker over too, but never renumber.
        QVERIFY(pressReturn(QStringLiteral("> quoted"), 8));
        QCOMPARE(result(), QStringLiteral("> quoted\n> "));

        // Ordinary prose has no marker to carry, so Return is left alone.
        QVERIFY(!pressReturn(QStringLiteral("just text"), 9));
    }

    void indentsListsFromTheEditorKeys() {
        Backend backend;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        QScopedPointer<QObject> created(createMainWindow(engine, component, backend));
        QVERIFY2(created, qPrintable(component.errorString()));

        QQuickWindow *window = qobject_cast<QQuickWindow *>(created.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("- one\n- two"));
        editor->setProperty("cursorPosition", 11);
        QVERIFY(QMetaObject::invokeMethod(editor, "forceActiveFocus"));

        QTest::keyClick(window, Qt::Key_Tab);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("- one\n  - two"));

        QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("- one\n- two"));
    }

    void savesAndOpensFromFooterButtons() {
        Backend backend;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        QScopedPointer<QObject> window(createMainWindow(engine, component, backend));
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
        Backend backend;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        QScopedPointer<QObject> window(createMainWindow(engine, component, backend));
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
    // Load EditorMutations.js into a throwaway TextEdit, so the library can be
    // driven the way Main.qml drives it.
    QObject *createJsHarness(QQmlComponent &component, const QByteArray &body,
                             const QString &name) {
        const QString mutationsPath = QFINDTESTDATA("../src/EditorMutations.js");
        if (mutationsPath.isEmpty())
            return nullptr;

        const QByteArray harness = "import QtQuick\n"
                                   "import \"EditorMutations.js\" as EditorMutations\n"
                                   "TextEdit {\n" + body + "\n}\n";
        component.setData(harness, QUrl::fromLocalFile(
            QFileInfo(mutationsPath).absolutePath() + "/" + name + QStringLiteral(".qml")));
        if (!component.isReady())
            return nullptr;
        return component.create();
    }

    // A TextEdit that applies plans the way Main.qml's applyPlan() does.
    QObject *createListEditor(QQmlComponent &component) {
        return createJsHarness(component, R"QML(
                property string resultText
                property int resultCursor

                function apply(plan) {
                    EditorMutations.replaceRange(this, plan.start, plan.end, plan.replacement,
                                                 plan.selectionStartOffset,
                                                 plan.selectionEndOffset);
                    resultText = text;
                    resultCursor = cursorPosition;
                }

                function edit(plan, source, from) {
                    resultText = source;
                    resultCursor = from;
                    if (!plan)
                        return false;
                    apply(plan);
                    return true;
                }

                function indent(source, from, to, direction) {
                    text = source;
                    select(from, to);
                    return edit(EditorMutations.listIndentPlan(source, from, to, direction),
                                source, from);
                }

                function pressReturn(source, cursor) {
                    text = source;
                    cursorPosition = cursor;
                    return edit(EditorMutations.returnPlan(source, cursor, cursor),
                                source, cursor);
                }
        )QML", QStringLiteral("ListHarness"));
    }

    QObject *createMainWindow(QQmlEngine &engine, QQmlComponent &component, Backend &backend) {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        if (mainQmlPath.isEmpty())
            return nullptr;

        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        component.loadUrl(QUrl::fromLocalFile(mainQmlPath));
        if (!component.isReady())
            return nullptr;
        return component.create();
    }

    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"

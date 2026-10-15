#include <QtTest>
#include <QFont>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QStandardPaths>
#include <QClipboard>
#include <QMimeData>
#include "cli.h"

#include "backend.h"
#include "markdownhighlighter.h"

// Counts QML "Binding loop detected" warnings while it is alive, forwarding
// everything to the previous handler so test output stays intact.
class BindingLoopCounter {
public:
    BindingLoopCounter() {
        s_count = 0;
        m_previous = qInstallMessageHandler(handle);
        s_previous = m_previous;
    }
    ~BindingLoopCounter() { qInstallMessageHandler(m_previous); }
    int count() const { return s_count; }

private:
    static void handle(QtMsgType type, const QMessageLogContext &context, const QString &message) {
        if (message.contains(QLatin1String("Binding loop detected")))
            ++s_count;
        if (s_previous)
            s_previous(type, context, message);
    }
    static inline int s_count = 0;
    static inline QtMessageHandler s_previous = nullptr;
    QtMessageHandler m_previous = nullptr;
};

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_settingsDirectory.isValid());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDirectory.path());
    }

    void countsWords() {
        QCOMPARE(Backend::countWords(QStringLiteral("one two-three don't 42")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好世界")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好 世界")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("中文 iPad AI")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("foo中bar")), 3);
        QCOMPARE(Backend::countWords(QStringLiteral("日本語テスト")), 6);
        QCOMPARE(Backend::countWords(QStringLiteral("你好，世界。")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("コーヒー")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("か\u3099")), 1); // が, decomposed
        QCOMPARE(Backend::countWords(QStringLiteral("𠀀𠀁 foo")), 3);
        QCOMPARE(Backend::countWords(QStringLiteral("안녕하세요 세계")), 2);
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

    void answersHelpBeforeOpeningAWindow() {
        const QString usage = Cli::usage();
        QVERIFY(usage.contains(QStringLiteral("omawrite [FILE]")));
        QVERIFY(usage.contains(QStringLiteral("-h, --help")));
        QVERIFY(usage.contains(QStringLiteral("-v, --version")));

        const QString version = QStringLiteral("1.2.3");
        const auto answer = [&](const QStringList &arguments) {
            return Cli::handleArguments(QStringList{QStringLiteral("omawrite")} + arguments,
                                        version);
        };
        QCOMPARE(answer({QStringLiteral("--help")}), std::optional<int>(0));
        QCOMPARE(answer({QStringLiteral("-h")}), std::optional<int>(0));
        QCOMPARE(answer({QStringLiteral("--version")}), std::optional<int>(0));
        QCOMPARE(answer({QStringLiteral("-v")}), std::optional<int>(0));

        // Everything else is the app's: no arguments, a file, and Qt's own
        // options, which a leading dash must not turn into an error.
        QCOMPARE(answer({}), std::nullopt);
        QCOMPARE(answer({QStringLiteral("draft.md")}), std::nullopt);
        QCOMPARE(answer({QStringLiteral("-platform"), QStringLiteral("offscreen")}),
                 std::nullopt);
        QCOMPARE(answer({QStringLiteral("-notes.md")}), std::nullopt);
    }

    void printsInPointsNotPixels() {
        QFont editorFont;
        editorFont.setPixelSize(20);
        const QFont printed = Backend::printFont(editorFont, 96.0);
        QCOMPARE(printed.pixelSize(), -1);
        QCOMPARE(printed.pointSizeF(), 15.0);
        QCOMPARE(Backend::printFont(editorFont, 0.0).pointSizeF(), 15.0);

        QFont pointFont;
        pointFont.setPointSizeF(11.0);
        QCOMPARE(Backend::printFont(pointFont, 144.0).pointSizeF(), 11.0);
    }

    void keepsNotoFromCrowdingTheFontList() {
        const QStringList fonts = Backend::selectableFontFamilies({
            QStringLiteral("Noto Sans Devanagari"), QStringLiteral("Liberation Serif"),
            QStringLiteral("Noto Serif"), QStringLiteral("Noto Sans Tamil UI"),
            QStringLiteral("adwaita Sans"), QStringLiteral("IBM Plex Mono"),
            QStringLiteral("Noto Sans"), QStringLiteral("Noto Sans Mono"),
            QStringLiteral("Liberation Serif"), QStringLiteral("Monospace"),
            QStringLiteral("Standard Symbols PS"), QStringLiteral("D050000L"),
            QStringLiteral("Noto Color Emoji"), QStringLiteral("Nimbus Sans [UKWN]"),
            QStringLiteral("Nimbus Sans [URW ]")});
        QCOMPARE(fonts, QStringList({QStringLiteral("IBM Plex Mono"),
                                     QStringLiteral("adwaita Sans"),
                                     QStringLiteral("Liberation Serif"),
                                     QStringLiteral("Nimbus Sans"),
                                     QStringLiteral("Noto Sans"),
                                     QStringLiteral("Noto Sans Mono"),
                                     QStringLiteral("Noto Serif")}));
    }

    void remembersEditorFont() {
        const QString installed = Backend().availableFonts().value(1);
        QVERIFY(!installed.isEmpty());

        {
            Backend backend;
            QCOMPARE(backend.editorFont(), Backend::defaultEditorFont());
            QSignalSpy fontSpy(&backend, &Backend::editorFontChanged);
            backend.setEditorFont(installed);
            QCOMPARE(fontSpy.count(), 1);
        }

        QCOMPARE(Backend().editorFont(), installed);

        // A remembered font that has since been uninstalled falls back.
        QSettings().setValue(QStringLiteral("editor/font"), QStringLiteral("No Such Font"));
        QCOMPARE(Backend().editorFont(), Backend::defaultEditorFont());
        QSettings().remove(QStringLiteral("editor/font"));
    }

    void findsStrikethroughOutsideCode() {
        const auto markup = MarkdownHighlighter::inlineMarkup(QStringLiteral("a ~~gone~~ b"));
        QCOMPARE(markup.size(), 1);
        QCOMPARE(markup.at(0).kind, MarkdownHighlighter::InlineKind::Strikethrough);
        QCOMPARE(markup.at(0).content.start, 4);
        QCOMPARE(markup.at(0).content.length, 4);

        // Tildes in code, beside space, or in longer runs stay literal.
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("`~~x~~`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("a ~~ b ~~ c")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("~~~x~~~")).isEmpty());
    }

    void undoesAnEditInOneStep() {
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

        editor->setProperty("text", QStringLiteral("hello world"));
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 5));
        QMetaObject::invokeMethod(editor, "wrapSelection", Q_ARG(QVariant, QStringLiteral("**")));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("**hello** world"));
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("hello world"));
    }

    void keepsLineHeightAcrossUndoAndPaste() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();
        const auto allLinesSpaced = [document] {
            for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
                if (block.blockFormat().lineHeight() != 140)
                    return false;
            }
            return true;
        };

        // Undoing a deleted line leaves redo working.
        editor->setProperty("text", QStringLiteral("a\nb\nc"));
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 2), Q_ARG(int, 4));
        QTest::keyClick(window, Qt::Key_Delete);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a\nc"));
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a\nb\nc"));
        QVERIFY(document->isRedoAvailable());

        // Lines written over others keep the line height, however many there are.
        editor->setProperty("text", QStringLiteral("aa\nbb\ncc"));
        QGuiApplication::clipboard()->setText(QStringLiteral("X\nY"));
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 5));
        QMetaObject::invokeMethod(editor, "paste");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("X\nY\ncc"));
        QVERIFY(allLinesSpaced());
    }

    void searchFollowsEditsWithoutTakingTheCaret() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *searchField = window->findChild<QObject *>(QStringLiteral("searchField"));
        QVERIFY(editor);
        QVERIFY(searchField);

        editor->setProperty("text", QStringLiteral("foo bar foo"));
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        searchField->setProperty("text", QStringLiteral("foo"));
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("foo"));

        // Back in the text, typing goes where the caret is.
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        editor->setProperty("cursorPosition", 7);
        QTest::keyClick(window, 'x');
        QTest::keyClick(window, 'y');
        QCOMPARE(editor->property("text").toString(), QStringLiteral("foo barxy foo"));
        QCOMPARE(window->property("searchMatches").toList().size(), 2);

        // The current match's highlight leaves its old block even when text
        // above it is deleted.
        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();
        const auto matchBackground = [document](int blockNumber) {
            const QTextBlock block = document->findBlockByNumber(blockNumber);
            for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
                if (range.start == 0 && range.format.hasProperty(QTextFormat::BackgroundBrush))
                    return range.format.background().color();
            }
            return QColor();
        };
        editor->setProperty("text", QStringLiteral("long text\nfoo\nfoo"));
        searchField->setProperty("text", QStringLiteral("x"));
        searchField->setProperty("text", QStringLiteral("foo"));
        QMetaObject::invokeMethod(window, "moveSearch", Q_ARG(QVariant, 1));
        const QColor current = matchBackground(2);
        const QColor other = matchBackground(1);
        QVERIFY(current.isValid() && other.isValid() && current != other);
        QMetaObject::invokeMethod(editor, "remove", Q_ARG(int, 0), Q_ARG(int, 10));
        QCOMPARE(window->property("searchMatches").toList().size(), 2);
        QCOMPARE(window->property("searchMatchIndex").toInt(), 0);
        QCOMPARE(matchBackground(0), current);
        QCOMPARE(matchBackground(1), other);

        // Nor does it stay behind when another match moves into its offset.
        editor->setProperty("text", QStringLiteral("foo\nab\nfoo"));
        QMetaObject::invokeMethod(window, "moveSearch", Q_ARG(QVariant, 1));
        QCOMPARE(matchBackground(2), current);
        QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 0), Q_ARG(QString, QStringLiteral("abcdefg")));
        QCOMPARE(window->property("searchMatches").toList().first().toInt(), 7);
        QCOMPARE(matchBackground(2), other);

        // Matches are found where the highlighter finds them, even past a
        // character whose lowercase is longer.
        editor->setProperty("text", QString::fromUtf8("\xC4\xB0 ab"));
        searchField->setProperty("text", QStringLiteral("ab"));
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("ab"));
    }

    void replacesOneMatchAfterAnother() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *searchField = window->findChild<QObject *>(QStringLiteral("searchField"));
        QObject *replaceField = window->findChild<QObject *>(QStringLiteral("replaceField"));
        QObject *replaceButton = window->findChild<QObject *>(QStringLiteral("replaceButton"));
        QObject *replaceAllButton = window->findChild<QObject *>(QStringLiteral("replaceAllButton"));
        QVERIFY(editor && searchField && replaceField && replaceButton && replaceAllButton);

        editor->setProperty("text", QStringLiteral("foo foo foo"));
        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        searchField->setProperty("text", QStringLiteral("foo"));
        replaceField->setProperty("text", QStringLiteral("Foo"));

        // A replacement the query still matches is not found again.
        QMetaObject::invokeMethod(replaceButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("Foo foo foo"));
        QMetaObject::invokeMethod(replaceButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("Foo Foo foo"));
        QMetaObject::invokeMethod(replaceButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("Foo Foo Foo"));

        // Replace All is one edit, so one undo takes it back.
        editor->setProperty("text", QStringLiteral("a foo b foo c"));
        replaceField->setProperty("text", QStringLiteral("x"));
        QMetaObject::invokeMethod(replaceAllButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a x b x c"));
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a foo b foo c"));
    }

    void reachesAndLeavesTheReplaceFieldFromTheKeyboard() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *replaceField = window->findChild<QObject *>(QStringLiteral("replaceField"));
        QVERIFY(editor && replaceField);

        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(replaceField->property("activeFocus").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("searchOpen").toBool());

        // Escape closes the bar from the Replace field itself too.
        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        QMetaObject::invokeMethod(replaceField, "forceActiveFocus");
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("searchOpen").toBool());

        // A link inserted from the find bar puts the writer back in the text.
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QVERIFY(editor->property("activeFocus").toBool());
    }

    void keepsShortcutsBehindDialogs() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *dialog = window->findChild<QObject *>(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);

        QMetaObject::invokeMethod(dialog, "open");
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(!window->property("searchOpen").toBool());
        QMetaObject::invokeMethod(dialog, "close");
    }

    void togglesTheFontPickerFromItsButton() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *button = window->findChild<QQuickItem *>(QStringLiteral("fontButton"));
        QObject *picker = window->findChild<QObject *>(QStringLiteral("fontPicker"));
        QVERIFY(button && picker);

        const QPoint center = button->mapToScene(
            QPointF(button->width() / 2, button->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
        QTRY_VERIFY(picker->property("opened").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
        QTRY_VERIFY(!picker->property("visible").toBool());
    }

    void convertsLineStartCheckboxAndUndo() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        for (const QChar key : QStringLiteral("[] "))
            QTest::keyClick(window, key.toLatin1());
        QCOMPARE(editor->property("text").toString(), QStringLiteral("- [ ] "));
        QCOMPARE(editor->property("cursorPosition").toInt(), 6);
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(editor->property("text").toString(), QString());

        editor->setProperty("text", QStringLiteral("```\n"));
        editor->setProperty("cursorPosition", 4);
        for (const QChar key : QStringLiteral("[] "))
            QTest::keyClick(window, key.toLatin1());
        QCOMPARE(editor->property("text").toString(), QStringLiteral("```\n[] "));
    }

    void slashPaletteFiltersAndInsertsMarkdown() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QTest::keyClick(window, Qt::Key_Slash);
        QObject *palette = window->findChild<QObject *>(QStringLiteral("slashPalette"));
        QVERIFY(palette);
        QVERIFY(palette->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_Q);
        QCOMPARE(editor->property("slashFilter").toString(), QStringLiteral("q"));
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("> "));
        QVERIFY(editor->property("activeFocus").toBool());

        editor->setProperty("text", QStringLiteral("hello "));
        editor->setProperty("cursorPosition", 6);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QTest::keyClick(window, Qt::Key_Slash);
        QTRY_COMPARE(editor->property("text").toString(), QStringLiteral("hello /"));
        QVERIFY(!palette->property("visible").toBool());
    }

    void breaksLinesAfterInlineBackticks() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // Backticks that open no fence leave the paragraphs below as prose.
        const QString text = QStringLiteral("```x``` inline\n\npara");
        editor->setProperty("text", text);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        editor->setProperty("cursorPosition", text.length());
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), text + QStringLiteral("\n\n"));

        // Before a closing fence the caret is still in the code; before an
        // opening one it is not yet.
        editor->setProperty("text", QStringLiteral("```\ncode\n```"));
        editor->setProperty("cursorPosition", 9);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("```\ncode\n\n```"));
        editor->setProperty("text", QStringLiteral("para\n```\ncode\n```"));
        editor->setProperty("cursorPosition", 5);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("para\n\n\n```\ncode\n```"));

        // Tilde fences hold code the same way.
        editor->setProperty("text", QStringLiteral("~~~\ncode"));
        editor->setProperty("cursorPosition", 8);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("~~~\ncode\n"));
    }

    void togglesEmphasisAtTheCaret() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");

        const auto type = [window](const QString &keys) {
            for (const QChar key : keys)
                QTest::keyClick(window, key.toLatin1());
        };
        const auto bold = [window] { QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier); };
        const auto text = [editor] { return editor->property("text").toString(); };

        // Ctrl+B, type, Ctrl+B ends the bold instead of stacking markers.
        editor->setProperty("text", QStringLiteral("a "));
        editor->setProperty("cursorPosition", 2);
        bold();
        type(QStringLiteral("hi"));
        bold();
        type(QStringLiteral(" more"));
        QCOMPARE(text(), QStringLiteral("a **hi** more"));

        // Just past the closing marker, Ctrl+B steps back into the bold.
        editor->setProperty("text", QStringLiteral("a **hi**"));
        editor->setProperty("cursorPosition", 8);
        bold();
        type(QStringLiteral("!"));
        QCOMPARE(text(), QStringLiteral("a **hi!**"));

        // Inside the bold, Ctrl+B takes it off and keeps the caret in place.
        editor->setProperty("text", QStringLiteral("a **hello** b"));
        editor->setProperty("cursorPosition", 6);
        bold();
        QCOMPARE(text(), QStringLiteral("a hello b"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 4);

        // At the end of bold italic text, Ctrl+B steps out rather than unbolding.
        editor->setProperty("text", QStringLiteral("***hello***"));
        editor->setProperty("cursorPosition", 8);
        bold();
        QCOMPARE(text(), QStringLiteral("***hello***"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 11);

        // Through several layers of nesting too.
        editor->setProperty("text", QStringLiteral("**~~*hello*~~**"));
        editor->setProperty("cursorPosition", 10);
        bold();
        QCOMPARE(text(), QStringLiteral("**~~*hello*~~**"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 15);

        // A literal underscore at the end is text, not a closing marker.
        editor->setProperty("text", QStringLiteral("**value_**"));
        editor->setProperty("cursorPosition", 7);
        bold();
        QCOMPARE(text(), QStringLiteral("value_"));

        // Italic works the same way.
        editor->setProperty("text", QStringLiteral("a "));
        editor->setProperty("cursorPosition", 2);
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        type(QStringLiteral("hi"));
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        type(QStringLiteral(" x"));
        QCOMPARE(text(), QStringLiteral("a *hi* x"));
    }

    void togglesEmphasisMarkers() {
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

        // Sets the text, selects [from, to), toggles, and returns the text.
        const auto toggle = [editor](const QString &text, int from, int to,
                                     const QString &marker) {
            editor->setProperty("text", text);
            QMetaObject::invokeMethod(editor, "select", Q_ARG(int, from), Q_ARG(int, to));
            QMetaObject::invokeMethod(editor, "wrapSelection", Q_ARG(QVariant, marker));
            return editor->property("text").toString();
        };
        const QString bold = QStringLiteral("**");
        const QString italic = QStringLiteral("*");

        QCOMPARE(toggle(QStringLiteral("a word b"), 2, 6, bold), QStringLiteral("a **word** b"));
        // Off again, whether the markers sit outside the selection or in it.
        QCOMPARE(toggle(QStringLiteral("a **word** b"), 4, 8, bold), QStringLiteral("a word b"));
        QCOMPARE(toggle(QStringLiteral("a **word** b"), 2, 10, bold), QStringLiteral("a word b"));
        QCOMPARE(toggle(QStringLiteral("a ~~word~~ b"), 4, 8, QStringLiteral("~~")),
                 QStringLiteral("a word b"));
        // An empty pair goes away too.
        QCOMPARE(toggle(QStringLiteral("Hello****."), 7, 7, bold), QStringLiteral("Hello."));

        // The "*" of bold is not italic, and "***" holds both.
        QCOMPARE(toggle(QStringLiteral("**word**"), 2, 6, italic), QStringLiteral("***word***"));
        QCOMPARE(toggle(QStringLiteral("***word***"), 3, 7, italic), QStringLiteral("**word**"));
        QCOMPARE(toggle(QStringLiteral("***word***"), 3, 7, bold), QStringLiteral("*word*"));
        // Selecting part of bold's markers adds italic rather than removing bold.
        QCOMPARE(toggle(QStringLiteral("**word**"), 1, 7, italic), QStringLiteral("***word***"));

        // Separate spans are wrapped, not broken apart.
        QCOMPARE(toggle(QStringLiteral("**a** and **b**"), 0, 15, bold),
                 QStringLiteral("****a** and **b****"));
        // Other emphasis nested inside does not stop italic from coming off.
        QCOMPARE(toggle(QStringLiteral("*a **b** c*"), 1, 10, italic),
                 QStringLiteral("a **b** c"));
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

    void findsLinkUrlAtPosition() {
        const QString text = QStringLiteral("Read [site](https://example.com/docs) now");
        const int linkStart = text.indexOf(QStringLiteral("site"));

        QCOMPARE(MarkdownHighlighter::linkUrlAt(text, linkStart),
                 QStringLiteral("https://example.com/docs"));
        QCOMPARE(MarkdownHighlighter::linkUrlAt(text, linkStart + 3),
                 QStringLiteral("https://example.com/docs"));
        QVERIFY(MarkdownHighlighter::linkUrlAt(text, linkStart - 1).isEmpty());
        QVERIFY(MarkdownHighlighter::linkUrlAt(text, text.indexOf(QStringLiteral("https"))).isEmpty());

        // Escaped punctuation, as the editor writes for parentheses, reads as itself.
        const QString escaped = QStringLiteral(R"([wiki](https://example.com/a\(b\)))");
        QCOMPARE(MarkdownHighlighter::linkUrlAt(escaped, 1),
                 QStringLiteral("https://example.com/a(b)"));
    }

    void showsLinkUrlOnlyOverTheLinkText() {
        // Probing a character past the end of the text warns from QTextCursor.
        QTest::failOnWarning(QRegularExpression(QStringLiteral("out of range")));
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("see [label](https://example.com)\nplain"));

        const auto urlUnder = [editor](qreal x, qreal y) {
            QVariant url;
            QMetaObject::invokeMethod(editor, "linkUrlUnder", Q_RETURN_ARG(QVariant, url),
                                      Q_ARG(QVariant, x), Q_ARG(QVariant, y));
            return url.toString();
        };
        const auto boxAt = [editor](int position) {
            QRectF box;
            QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, box),
                                      Q_ARG(int, position));
            return box;
        };

        // Over the label (the hidden "[" is index 4), in the right half of a
        // character too.
        const QRectF first = boxAt(5);
        const QRectF second = boxAt(6);
        const qreal middleY = first.y() + first.height() / 2;
        QCOMPARE(urlUnder(first.x() + 1, middleY), QStringLiteral("https://example.com"));
        QCOMPARE(urlUnder(second.x() - 1, middleY), QStringLiteral("https://example.com"));
        // Over the plain word before it.
        QCOMPARE(urlUnder(boxAt(0).x() + 1, middleY), QString());
        // Past the end of the line the link ends.
        QCOMPARE(urlUnder(editor->property("width").toReal() - 2, middleY), QString());
        // The right half of the label's last character still counts, though
        // the nearest caret position is past the label.
        QCOMPARE(urlUnder(boxAt(10).x() - 1, middleY), QStringLiteral("https://example.com"));

        // Below a last line that is a link, positionAt() snaps up onto it.
        editor->setProperty("text", QStringLiteral("[label](https://example.com)"));
        const QRectF only = boxAt(1);
        QCOMPARE(urlUnder(only.x() + 1, only.y() + only.height() / 2),
                 QStringLiteral("https://example.com"));
        QCOMPARE(urlUnder(only.x() + 1, only.y() + only.height() * 3), QString());

        // A wide character counts across its whole width.
        editor->setProperty("text", QStringLiteral("[\u5bbd](https://example.com)"));
        const QRectF wide = boxAt(1);
        const qreal wideRight = boxAt(2).x();
        QVERIFY(wideRight - wide.x() > editor->property("font").value<QFont>().pixelSize() * 0.7);
        QCOMPARE(urlUnder(wideRight - 1, wide.y() + wide.height() / 2),
                 QStringLiteral("https://example.com"));

        // A right-to-left label runs the other way across the screen.
        editor->setProperty("text", QStringLiteral("[\u05e9\u05dc\u05d5\u05dd](https://example.org)"));
        const QRectF rtlFirst = boxAt(1);
        const QRectF rtlSecond = boxAt(2);
        QVERIFY(rtlSecond.x() < rtlFirst.x());
        const qreal rtlY = rtlFirst.y() + rtlFirst.height() / 2;
        QCOMPARE(urlUnder((rtlFirst.x() + rtlSecond.x()) / 2, rtlY),
                 QStringLiteral("https://example.org"));
        QCOMPARE(urlUnder(rtlFirst.x() - 1, rtlY), QStringLiteral("https://example.org"));

        // Link-like text inside fenced code is not a link.
        editor->setProperty("text", QStringLiteral("```\n[label](https://example.com)\n```"));
        QCOMPARE(backend.linkUrlAt(5), QString());
    }

    void showsLinkTooltipUrlAsPlainText() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *tooltipText =
            window->findChild<QObject *>(QStringLiteral("linkTooltipText"));
        QVERIFY(tooltipText);

        // A destination Qt would mistake for HTML must still read as itself.
        const QString destination =
            QStringLiteral("<img src=\"https://example.com/pixel\">");
        QVERIFY(Qt::mightBeRichText(destination));
        tooltipText->setProperty("text", destination);
        QCOMPARE(tooltipText->property("text").toString(), destination);
        QCOMPARE(tooltipText->property("textFormat").toInt(), int(Qt::PlainText));
    }

    void leavesInlineCodeLiteral() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("see `default_line_height` and `file_name`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("a_b `c_d`")).isEmpty());
        // Word-boundary underscores inside a code span are still literal.
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("`_private_`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("`a * b * c`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("`[not](a link)`")).isEmpty());

        // Emphasis that merely wraps a code span still applies.
        const auto wrapping = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**`file_name` only**"));
        QCOMPARE(wrapping.size(), 1);
        QCOMPARE(wrapping.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(wrapping.at(0).content.start, 2);
        QCOMPARE(wrapping.at(0).content.length, 16);
        QCOMPARE(wrapping.at(0).markers[0].length, 2);
        QCOMPARE(wrapping.at(0).markers[1].start, 18);
    }

    void keepsIntrawordUnderscoresLiteral() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("snake_case_name and default_line_height")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("a__b__c")).isEmpty());

        // Emphasis still opens at a word boundary.
        const auto underscoreBold =
            MarkdownHighlighter::inlineMarkup(QStringLiteral("say __bold__ here"));
        QCOMPARE(underscoreBold.size(), 1);
        QCOMPARE(underscoreBold.at(0).content.start, 6);
        QCOMPARE(underscoreBold.at(0).content.length, 4);

        const auto emphasis = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("_italic_, **bold** and mid*word*"));
        QCOMPARE(emphasis.size(), 3);
        QCOMPARE(emphasis.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(emphasis.at(0).content.length, 4);
        QCOMPARE(emphasis.at(1).content.start, 1);
        QCOMPARE(emphasis.at(1).content.length, 6);
        QCOMPARE(emphasis.at(2).content.length, 4);
    }

    void respectsEscapedOpeningBackticks() {
        const auto escaped = MarkdownHighlighter::inlineMarkup(
            QStringLiteral(R"(\` *italic* \`)"));
        QCOMPARE(escaped.size(), 1);
        QCOMPARE(escaped.constFirst().kind, MarkdownHighlighter::InlineKind::Italic);
        QVERIFY(MarkdownHighlighter::codeSpans(QStringLiteral(R"(\` *italic* \`)")).isEmpty());

        const QString mixed = QStringLiteral(R"(\` *italic* `*literal*`)");
        QCOMPARE(MarkdownHighlighter::inlineMarkup(mixed).size(), 1);
        const auto code = MarkdownHighlighter::codeSpans(mixed);
        QCOMPARE(code.size(), 1);
        QCOMPARE(mixed.mid(code.constFirst().start, code.constFirst().length),
                 QStringLiteral("`*literal*`"));

        // An escaped backslash leaves the opening backtick active.
        QVERIFY(MarkdownHighlighter::inlineMarkup(
            QStringLiteral(R"(\\`*literal*`)")).isEmpty());
        QCOMPARE(MarkdownHighlighter::codeSpans(
            QStringLiteral(R"(\\`*literal*`)")).size(), 1);

        // Backslashes inside code do not escape its closing delimiter.
        const auto afterCode = MarkdownHighlighter::inlineMarkup(
            QStringLiteral(R"(`code\` *italic*)"));
        QCOMPARE(afterCode.size(), 1);
        QCOMPARE(afterCode.constFirst().kind, MarkdownHighlighter::InlineKind::Italic);
    }

    void keepsEnclosedCodeStyling_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("kind");
        QTest::newRow("italic") << QStringLiteral("*a `code` b*")
                                << int(MarkdownHighlighter::InlineKind::Italic);
        QTest::newRow("bold") << QStringLiteral("**a `code` b**")
                              << int(MarkdownHighlighter::InlineKind::Bold);
        QTest::newRow("link") << QStringLiteral("[a `code` b](https://example.com)")
                              << int(MarkdownHighlighter::InlineKind::Link);
    }

    void keepsEnclosedCodeStyling() {
        QFETCH(QString, text);
        QFETCH(int, kind);
        QTextDocument document;
        document.setPlainText(text);
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();

        const auto formatAt = [&document](int position) {
            for (const auto &range : document.firstBlock().layout()->formats()) {
                if (position >= range.start && position < range.start + range.length)
                    return range.format;
            }
            return QTextCharFormat();
        };
        const auto markup = MarkdownHighlighter::inlineMarkup(text);
        QCOMPARE(markup.size(), 1);
        QCOMPARE(int(markup.constFirst().kind), kind);
        const auto prose = formatAt(text.indexOf(QLatin1Char('a')));
        QCOMPARE(prose.fontItalic(), kind == int(MarkdownHighlighter::InlineKind::Italic));
        QCOMPARE(prose.fontWeight() == QFont::Bold,
                 kind == int(MarkdownHighlighter::InlineKind::Bold));
        QCOMPARE(prose.fontUnderline(), kind == int(MarkdownHighlighter::InlineKind::Link));
        QCOMPARE(prose.background().style(), Qt::NoBrush);

        const int codeStart = text.indexOf(QLatin1Char('`'));
        for (int i = codeStart; i < codeStart + 6; ++i) {
            const auto code = formatAt(i);
            QCOMPARE(code.background().color(), QColor("#23372b"));
            QVERIFY(!code.fontItalic());
            QVERIFY(code.fontWeight() != QFont::Bold);
            QVERIFY(!code.fontUnderline());
        }
        for (const auto &marker : markup.constFirst().markers)
            QCOMPARE(formatAt(marker.start).fontPointSize(), 1.0);
    }

    void leavesFencedCodeLiteral() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("prose _italic_\n"
                                            "```ruby\n"
                                            "snake_case_name = *value*\n"
                                            "```\n"
                                            "after _italic_\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();

        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        QCOMPARE(stateOf(0), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(1), int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(2), int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(3), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(4), int(MarkdownHighlighter::Prose));

        // The fenced line sits on the code panel, with nothing italic and no
        // marker hidden.
        const QList<QTextLayout::FormatRange> fenced =
            document.findBlockByNumber(2).layout()->formats();
        QCOMPARE(fenced.size(), 1);
        QCOMPARE(fenced.constFirst().length, document.findBlockByNumber(2).text().length());
        QVERIFY(fenced.constFirst().format.background().style() != Qt::NoBrush);
        for (const QTextLayout::FormatRange &range : fenced) {
            QVERIFY(!range.format.fontItalic());
            QVERIFY(range.format.foreground().color() != range.format.background().color());
        }

        const QList<QTextLayout::FormatRange> prose =
            document.findBlockByNumber(4).layout()->formats();
        QVERIFY(std::any_of(prose.cbegin(), prose.cend(),
                            [](const QTextLayout::FormatRange &range) {
                                return range.format.fontItalic();
                            }));
    }

    void opensFencesOnlyWhereMarkdownDoes() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("```inline``` and _italic_\n"
                                            "````md\n"
                                            "```\n"
                                            "still code\n"
                                            "````\n"
                                            "after _italic_\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();

        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        // Backticks that close on the same line are inline code, not a fence.
        QCOMPARE(stateOf(0), int(MarkdownHighlighter::Prose));
        // A four-backtick fence is not closed by three.
        QVERIFY(stateOf(1) >= int(MarkdownHighlighter::InsideFence));
        QVERIFY(stateOf(2) >= int(MarkdownHighlighter::InsideFence));
        QVERIFY(stateOf(3) >= int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(4), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(5), int(MarkdownHighlighter::Prose));

        QCOMPARE(MarkdownHighlighter::openingFenceLength(QStringLiteral("```ruby")), 3);
        QCOMPARE(MarkdownHighlighter::openingFenceLength(QStringLiteral("    ```")), 0);
        QVERIFY(!MarkdownHighlighter::closesFence(QStringLiteral("``` text"), 3));
    }

    void keepsCaretRangesConsistentWithCode() {
        Backend backend;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit {}", QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));
        auto *quickDocument = editor->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quickDocument);
        backend.attachDocument(quickDocument);
        auto *document = quickDocument->textDocument();
        auto *highlighter = document->findChild<MarkdownHighlighter *>();
        QVERIFY(highlighter);
        editor->setProperty("text", QStringLiteral("``` **opening**\n"
                                                   "# _literal_ [link](url)\n"
                                                   "```  \n"
                                                   "*prose*\n"
                                                   "`*literal*`\n"
                                                   "```\n"
                                                   "_unterminated_"));
        highlighter->rehighlight();
        for (int blockNumber : {0, 1, 2, 4, 5, 6}) {
            const auto block = document->findBlockByNumber(blockNumber);
            QVERIFY(backend.hiddenRangesAt(block.position()).isEmpty());
        }
        QCOMPARE(backend.hiddenRangesAt(document->findBlockByNumber(3).position()).size(), 2);

        // Removing a fence rehighlights the following blocks as prose.
        QTextCursor cursor(document);
        cursor.setPosition(document->findBlockByNumber(5).position());
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        QTRY_COMPARE(document->findBlockByNumber(6).userState(), int(MarkdownHighlighter::Prose));
        QCOMPARE(backend.hiddenRangesAt(document->findBlockByNumber(6).position()).size(), 2);
    }

    void takesTheCodePanelFromTheTheme() {
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

        Backend backend;
        const auto codePanelFor = [&themeDirectory, &backend](const QByteArray &palette) {
            QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
            if (!colorsFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                return QColor();
            colorsFile.write(palette);
            colorsFile.close();
            // Reload the same backend so a missing key cannot inherit the
            // previous theme's panel color.
            backend.setDarkMode(!backend.darkMode());
            return QColor(backend.themeCodeBackground());
        };

        // The shade the theme names for panels wins.
        QCOMPARE(codePanelFor("mode = \"dark\"\n"
                              "background = \"#111c18\"\n"
                              "foreground = \"#c1c497\"\n"
                              "lighter_background = \"#23372b\"\n"),
                 QColor(QStringLiteral("#23372b")));

        // Themes naming none leave code on a shade mixed from the page: a
        // little off the background, and nowhere near the text.
        const QColor mixedDark = codePanelFor("mode = \"dark\"\n"
                                              "background = \"#000000\"\n"
                                              "foreground = \"#ffffff\"\n");
        QVERIFY(mixedDark != QColor(Qt::black));
        QVERIFY(mixedDark.lightness() < 60);

        const QColor mixedLight = codePanelFor("mode = \"light\"\n"
                                               "background = \"#ffffff\"\n"
                                               "foreground = \"#000000\"\n");
        QVERIFY(mixedLight != QColor(Qt::white));
        QVERIFY(mixedLight.lightness() > 195);

        QCOMPARE(codePanelFor("mode = \"light\"\n"
                              "background = \"#ffffff\"\n"
                              "foreground = \"#000000\"\n"
                              "lighter_background = \"garbage\"\n"), mixedLight);

        // A theme whose lighter background is the page itself would leave code
        // with no panel at all, so it falls back to the mix as well.
        QVERIFY(codePanelFor("mode = \"dark\"\n"
                             "background = \"#0c0b0c\"\n"
                             "foreground = \"#fafcfb\"\n"
                             "lighter_background = \"#0c0b0c\"\n")
                != QColor(QStringLiteral("#0c0b0c")));
    }

    void rendersBoldItalicRuns() {
        const QString text = QStringLiteral("a ***word*** b");
        const auto markup = MarkdownHighlighter::inlineMarkup(text);
        QCOMPARE(markup.size(), 2);
        QCOMPARE(markup.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(markup.at(1).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(markup.at(1).content.start, 5);
        QCOMPARE(markup.at(1).content.length, 4);

        const auto formats = highlightedFormats(text);
        QVERIFY(formats.at(5).fontItalic());
        QCOMPARE(formats.at(5).fontWeight(), int(QFont::Bold));
        for (int i : {2, 3, 4, 9, 10, 11})
            QCOMPARE(formats.at(i).fontPointSize(), 1.0);
    }

    // Markers nested inside other markup stay hidden, as the caret skips them.
    void hidesMarkersInsideNestedMarkup() {
        const QString linked = QStringLiteral("[**a**](u)");
        const auto linkFormats = highlightedFormats(linked);
        for (int i : {0, 1, 2, 4, 5, 6})
            QCOMPARE(linkFormats.at(i).fontPointSize(), 1.0);
        QCOMPARE(linkFormats.at(3).fontWeight(), int(QFont::Bold));
        QVERIFY(linkFormats.at(3).fontUnderline());

        const QString nested = QStringLiteral("*a __b__ c*");
        const auto nestedFormats = highlightedFormats(nested);
        for (int i : {0, 3, 4, 6, 7, 10})
            QCOMPARE(nestedFormats.at(i).fontPointSize(), 1.0);
        QVERIFY(nestedFormats.at(5).fontItalic());
        QCOMPARE(nestedFormats.at(5).fontWeight(), int(QFont::Bold));
    }

    void honoursBackslashEscapes() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\*not\\* here")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\[x](y)")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\~~not~~")).isEmpty());
        // An escaped backslash leaves the marker alone.
        QCOMPARE(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\\\*yes*")).size(), 1);
    }

    void leavesThematicBreaksAlone() {
        for (const QString &rule : {QStringLiteral("*****"), QStringLiteral("___"),
                                    QStringLiteral("* * *")}) {
            QVERIFY2(MarkdownHighlighter::inlineMarkup(rule).isEmpty(), qPrintable(rule));
            const auto formats = highlightedFormats(rule);
            for (const QTextCharFormat &format : formats) {
                QVERIFY(format.fontWeight() != QFont::Bold);
                QVERIFY(format.fontPointSize() != 1.0);
            }
        }
    }

    void mergesInlineStylesWithTheBlock() {
        // A heading stays bold under italic, a link, or code.
        const auto heading = highlightedFormats(QStringLiteral("# head *it* [l](u) `c`"));
        QVERIFY(heading.at(8).fontItalic());
        QCOMPARE(heading.at(8).fontWeight(), int(QFont::Bold));
        QCOMPARE(heading.at(13).fontWeight(), int(QFont::Bold));
        QCOMPARE(heading.at(20).fontWeight(), int(QFont::Bold));

        // A quote keeps its colour under bold.
        const auto quote = highlightedFormats(QStringLiteral("> quote **b**"));
        QCOMPARE(quote.at(10).fontWeight(), int(QFont::Bold));
        QCOMPARE(quote.at(10).foreground().color(), quote.at(3).foreground().color());
    }

    void recognisesTildeFences() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("~~~js\n"
                                            "*code*\n"
                                            "```\n"
                                            "~~~\n"
                                            "after *it*\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.rehighlight();
        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        QVERIFY(stateOf(0) >= int(MarkdownHighlighter::InsideFence));
        QVERIFY(stateOf(1) >= int(MarkdownHighlighter::InsideFence));
        // Backticks do not close a tilde fence.
        QVERIFY(stateOf(2) >= int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(3), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(4), int(MarkdownHighlighter::Prose));
    }

    void readsLinkDestinationsWithParentheses() {
        QCOMPARE(MarkdownHighlighter::linkUrlAt(QStringLiteral("[a](b(c))"), 1),
                 QStringLiteral("b(c)"));
        QCOMPARE(MarkdownHighlighter::linkUrlAt(QStringLiteral("[a](b) and (c)"), 1),
                 QStringLiteral("b"));
    }

    void reportsThemeChangesOnlyWhenTheyHappen() {
        ScopedTheme theme("mode = \"dark\"\nbackground = \"#111111\"\n");
        QVERIFY(theme.ok);
        Backend backend;
        QSignalSpy colorsSpy(&backend, &Backend::themeColorsChanged);
        QSignalSpy modeSpy(&backend, &Backend::darkModeChanged);

        // The theme names its mode, so the desktop's setting changes nothing.
        backend.setDarkMode(false);
        QVERIFY(backend.darkMode());
        QCOMPARE(colorsSpy.count(), 0);
        QCOMPARE(modeSpy.count(), 0);
    }

    void ignoresInlineMarkdownInsideCodeSpans() {
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("`The_brown_fox` jumps")).size(), 0);
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("`a **b** [c](d)` and `e`")).size(), 0);

        const auto mixed = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("`code_span` then *real* emphasis"));
        QCOMPARE(mixed.size(), 1);
        QCOMPARE(mixed.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(mixed.at(0).content.start, 18);
    }

    // A code span makes its own contents literal; it does not stop markup that
    // merely encloses one. Only a delimiter landing inside the code span does.
    void keepsMarkupThatEnclosesACodeSpan() {
        const auto spanning = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("_a `b` c_"));
        QCOMPARE(spanning.size(), 1);
        QCOMPARE(spanning.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(spanning.at(0).content.start, 1);
        QCOMPARE(spanning.at(0).content.length, 7);

        const auto linked = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("[see `code`](url)"));
        QCOMPARE(linked.size(), 1);
        QCOMPARE(linked.at(0).kind, MarkdownHighlighter::InlineKind::Link);
        QCOMPARE(linked.at(0).content.start, 1);
        QCOMPARE(linked.at(0).content.length, 10);

        const auto wrapped = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("*`code`*"));
        QCOMPARE(wrapped.size(), 1);
        QCOMPARE(wrapped.at(0).kind, MarkdownHighlighter::InlineKind::Italic);

        const auto bold = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**a `b` c**"));
        QCOMPARE(bold.size(), 1);
        QCOMPARE(bold.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(bold.at(0).content.start, 2);
        QCOMPARE(bold.at(0).content.length, 7);

        // The closing underscore is inside the code span, so it is not a marker.
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("_a `b_ c` d_")).size(), 0);
    }

    // Emphasis may enclose a code span, so the code pass has to run last for the
    // code span to keep its own styling.
    void stylesAnEnclosedCodeSpanAsCode() {
        QTextDocument document;
        MarkdownHighlighter highlighter(&document);
        document.setPlainText(QStringLiteral("_a `b` c_"));
        // The code panel comes from the theme, as Backend passes it in.
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        // The constructor's own rehighlight is queued, so ask for one directly.
        highlighter.rehighlight();

        const QTextBlock block = document.firstBlock();
        QVERIFY(block.isValid());
        const auto formatAt = [&block](int index) {
            QTextCharFormat found;
            for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
                if (index >= range.start && index < range.start + range.length)
                    found = range.format;
            }
            return found;
        };

        // "a" is italic and carries no code background.
        QVERIFY(formatAt(1).fontItalic());
        QCOMPARE(formatAt(1).background().style(), Qt::NoBrush);

        // The code span keeps the code background and is not italicised.
        QVERIFY(formatAt(4).background().style() != Qt::NoBrush);
        QVERIFY(!formatAt(4).fontItalic());
    }

    // A stray asterisk next to whitespace is not an emphasis delimiter, so it
    // must not swallow a bullet or the operands of arithmetic.
    void keepsWhitespaceDelimitedAsterisksLiteral() {
        // Only `*important*` is markup; the bullet stays a bullet.
        const auto bullet = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("* first *important* point"));
        QCOMPARE(bullet.size(), 1);
        QCOMPARE(bullet.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(bullet.at(0).content.start, 9);
        QCOMPARE(bullet.at(0).content.length, 9);
        QCOMPARE(bullet.at(0).markers[0].start, 8);
        QCOMPARE(bullet.at(0).markers[1].start, 18);

        // Multiplication stays exactly as typed.
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("2 * 3 * 4")).size(), 0);

        // Real emphasis is unaffected.
        const auto shortWord = MarkdownHighlighter::inlineMarkup(QStringLiteral("*a*"));
        QCOMPARE(shortWord.size(), 1);
        QCOMPARE(shortWord.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(shortWord.at(0).content.start, 1);
        QCOMPARE(shortWord.at(0).content.length, 1);

        const auto longPhrase =
            MarkdownHighlighter::inlineMarkup(QStringLiteral("*longer text*"));
        QCOMPARE(longPhrase.size(), 1);
        QCOMPARE(longPhrase.at(0).content.length, 11);

        const auto mixed = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic*"));
        QCOMPARE(mixed.size(), 2);
        QCOMPARE(mixed.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(mixed.at(1).kind, MarkdownHighlighter::InlineKind::Italic);
    }

    void loadsCurrentOmarchyTheme() {
        ScopedTheme theme(
            "mode = \"light\"\n"
            "accent = \"#112233\"\n"
            "selection = \"#445566\"\n"
            "selection_foreground = \"#fafafa\"\n"
            "background = \"#fefefe\"\n"
            "foreground = \"#101010\"\n");
        QVERIFY(theme.ok);

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#fefefe"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#101010"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QCOMPARE(backend.themeSelectionForeground(), QStringLiteral("#fafafa"));
        QVERIFY(!backend.darkMode());

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->property("selectionColor").value<QColor>(), QColor("#445566"));
        QCOMPARE(editor->property("selectedTextColor").value<QColor>(), QColor("#fafafa"));
    }

    void fallsBackForSelectionColors_data() {
        QTest::addColumn<QByteArray>("palette");
        QTest::addColumn<QString>("selection");
        QTest::addColumn<QString>("selectionForeground");

        QTest::newRow("bright_foreground")
            << QByteArray("foreground = \"#c0c0c0\"\n"
                          "bright_foreground = \"#ffffff\"\n"
                          "selection = \"#333333\"\n")
            << QStringLiteral("#333333") << QStringLiteral("#ffffff");
        QTest::newRow("foreground")
            << QByteArray("foreground = \"#c0c0c0\"\n"
                          "selection = \"#333333\"\n")
            << QStringLiteral("#333333") << QStringLiteral("#c0c0c0");
        QTest::newRow("selection_background")
            << QByteArray("foreground = \"#c0c0c0\"\n"
                          "selection_background = \"#222222\"\n")
            << QStringLiteral("#222222") << QStringLiteral("#c0c0c0");
    }

    void fallsBackForSelectionColors() {
        QFETCH(QByteArray, palette);
        QFETCH(QString, selection);
        QFETCH(QString, selectionForeground);

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
        QCOMPARE(colorsFile.write(palette), qint64(palette.size()));
        colorsFile.close();

        Backend backend;
        QCOMPARE(backend.themeSelection(), selection);
        QCOMPARE(backend.themeSelectionForeground(), selectionForeground);
    }

    void readsTomlValuesPastInlineComments() {
        // Every colour in every theme is a quoted "#rrggbb": the hash inside the quotes stays.
        QCOMPARE(Backend::tomlValue(QStringLiteral(" \"#1a1b26\" ")), QStringLiteral("#1a1b26"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"#EDE6D6\"   # unbleached cloth")),
                 QStringLiteral("#EDE6D6"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("'#445566'# no space before the hash")),
                 QStringLiteral("#445566"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"rebecca#purple\"")),
                 QStringLiteral("rebecca#purple"));
        QCOMPARE(Backend::tomlValue(QStringLiteral(" #1a1b26 ")), QStringLiteral("#1a1b26"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"#1a1b26")), QStringLiteral("\"#1a1b26"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"\"")), QString());
        QCOMPARE(Backend::tomlValue(QString()), QString());
    }

    void keepsDefaultsForColorsItCannotParse() {
        ScopedTheme theme(
            "mode = \"dark\"          # warmed ink\r\n"
            "accent = \"#112233\"     # shell-white pigment\r\n"
            "selection = '#445566'\r\n"
            "background = \"#1a1b17\" # ink, warmed\r\n"
            "foreground = \"unbleached cloth\"\r\n");
        QVERIFY(theme.ok);

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#1a1b17"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#eeeeee"));
        QVERIFY(backend.darkMode());
    }

    void takesItsDefaultsFromTheModeTheThemeAsksFor() {
        ScopedTheme theme(
            "mode = \"light\"\n"
            "background = \"#ffffff\"\n"
            "foreground = \"unbleached cloth\"\n");
        QVERIFY(theme.ok);

        Backend backend;
        QVERIFY(!backend.darkMode());
        QCOMPARE(backend.themeForeground(), QStringLiteral("#222324"));
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
                function replaceText(start, end, replacement) {
                    remove(start, end);
                    insert(start, replacement);
                }

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

    void stepsOverWholeCharactersWhenTypingAndSaving() {
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

        const auto setText = [&](const QString &value) {
            editor->setProperty("text", value);
            editor->setProperty("cursorPosition", 0);
        };
        const auto move = [&](int direction) -> int {
            const bool invoked = QMetaObject::invokeMethod(
                editor, "moveCursorVisibly", Q_ARG(QVariant, direction));
            return invoked ? editor->property("cursorPosition").toInt() : -1;
        };

        // Right steps over the whole emoji, so typing keeps it in the file.
        setText(QStringLiteral("a😀b"));
        editor->setProperty("cursorPosition", 1);
        QCOMPARE(move(1), 3);
        QVERIFY(QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 3),
                                          Q_ARG(QString, QStringLiteral("x"))));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a😀xb"));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString savedPath = directory.filePath(QStringLiteral("emoji.md"));
        backend.saveAs(QUrl::fromLocalFile(savedPath));
        QFile saved(savedPath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(saved.readAll()), QStringLiteral("a😀xb"));

        // Left steps back over the same cluster.
        setText(QStringLiteral("a😀b"));
        editor->setProperty("cursorPosition", 3);
        QCOMPARE(move(-1), 1);

        // Hidden Markdown markers are still skipped on both sides.
        setText(QStringLiteral("**bold**"));
        editor->setProperty("cursorPosition", 0);
        QCOMPARE(move(1), 2);
        editor->setProperty("cursorPosition", 8);
        QCOMPARE(move(-1), 6);

        // Hidden markers next to an emoji combine correctly.
        setText(QStringLiteral("*😀*x"));
        editor->setProperty("cursorPosition", 0);
        QCOMPARE(move(1), 1);
        QCOMPARE(move(1), 4);

        // Paragraph separators still move one position at a time.
        setText(QStringLiteral("ab\ncd"));
        editor->setProperty("cursorPosition", 2);
        QCOMPARE(move(1), 3);
        QCOMPARE(move(-1), 2);
    }

    void stepsCursorByWholeGraphemeClusters() {
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

        const auto step = [&](const QString &text, int from, int direction) {
            editor->setProperty("text", text);
            return backend.cursorPositionStep(from, direction);
        };

        // Qt 6 treats surrogate pairs, skin tone modifiers, ZWJ sequences,
        // regional indicators and combining marks as single cursor steps.
        QCOMPARE(step(QStringLiteral("a😀b"), 1, 1), 3);
        QCOMPARE(step(QStringLiteral("a😀b"), 3, -1), 1);
        QCOMPARE(step(QStringLiteral("a👍🏽b"), 1, 1), 5);
        QCOMPARE(step(QStringLiteral("a👍🏽b"), 5, -1), 1);
        QCOMPARE(step(QStringLiteral("a👨‍👩‍👧b"), 1, 1), 9);
        QCOMPARE(step(QStringLiteral("a👨‍👩‍👧b"), 9, -1), 1);
        QCOMPARE(step(QStringLiteral("a🇺🇸b"), 1, 1), 5);
        QCOMPARE(step(QStringLiteral("a1\u20e3b"), 1, 1), 3);
        QCOMPARE(step(QStringLiteral("ae\u0301b"), 1, 1), 3);

        // Document limits and paragraph separators are handled explicitly.
        QCOMPARE(step(QStringLiteral("a😀b"), 0, -1), 0);
        QCOMPARE(step(QStringLiteral("a😀b"), 4, 1), 4);
        QCOMPARE(step(QStringLiteral("ab\ncd"), 2, 1), 3);
        QCOMPARE(step(QStringLiteral("ab\ncd"), 3, -1), 2);
        QCOMPARE(step(QStringLiteral("ab\ncd"), 5, 1), 5);
        QCOMPARE(step(QStringLiteral("😀\nb"), 2, 1), 3);
        QCOMPARE(step(QStringLiteral("😀\nb"), 3, -1), 2);
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

    void copiesRenderedHtml() {
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
        editor->setProperty("text", QStringLiteral("# Title\n\nFirst **bold**.\n\nSecond."));
        backend.copyAsHtml();

        const QMimeData *mimeData = QGuiApplication::clipboard()->mimeData();
        QVERIFY(mimeData);
        QVERIFY(mimeData->hasHtml());
        const QString html = mimeData->html();
        QVERIFY(html.contains(QStringLiteral("<h1>")));
        QVERIFY(html.contains(QStringLiteral("<p>First ")));
        QVERIFY(html.contains(QStringLiteral("<body>")));
        QVERIFY(html.contains(QStringLiteral("font-weight:700;\">bold</span>")));
        QVERIFY(!html.contains(QStringLiteral("font-family")));
        QVERIFY(!html.contains(QStringLiteral("margin")));
        QCOMPARE(mimeData->text(), QStringLiteral("Title\nFirst bold.\nSecond."));
    }

    void flashesScrollBarOnlyWhenPaging() {
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
        QObject *linger = window->findChild<QObject *>(QStringLiteral("scrollLinger"));
        QVERIFY(editor);
        QVERIFY(linger);

        QStringList lines;
        for (int i = 0; i < 300; ++i)
            lines << QStringLiteral("line %1").arg(i);
        editor->setProperty("text", lines.join(QLatin1Char('\n')));
        QVERIFY(QMetaObject::invokeMethod(editor, "forceActiveFocus"));
        QTRY_VERIFY(editor->property("activeFocus").toBool());

        // Writing past the bottom edge scrolls along, but leaves the bar hidden.
        editor->setProperty("cursorPosition", editor->property("length").toInt());
        QVERIFY(!linger->property("running").toBool());

        QTest::keyClick(window, Qt::Key_PageUp);
        QVERIFY(linger->property("running").toBool());

        QMetaObject::invokeMethod(linger, "stop");
        QTest::keyClick(window, Qt::Key_Home, Qt::ControlModifier);
        QVERIFY(linger->property("running").toBool());
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);
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

    void unsavedChangesDialogSupportsAltMnemonics() {
        const QString dialogQmlPath = QFINDTESTDATA("../src/UnsavedChangesDialog.qml");
        QVERIFY(!dialogQmlPath.isEmpty());

        QQmlEngine engine;
        engine.addImportPath(QFileInfo(dialogQmlPath).absolutePath());
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import QtQuick.Controls

            ApplicationWindow {
                width: 640
                height: 420
                visible: true

                UnsavedChangesDialog {
                    id: dialog
                    objectName: "unsavedChangesDialog"
                    containerWidth: parent.width
                    containerHeight: parent.height
                }
            }
        )QML";
        component.setData(harness, QUrl::fromLocalFile(
            QFileInfo(dialogQmlPath).absolutePath() + QStringLiteral("/ShortcutHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        QObject *dialog = root->findChild<QObject *>(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);

        QSignalSpy cancelSpy(dialog, SIGNAL(cancelRequested()));
        QSignalSpy discardSpy(dialog, SIGNAL(discardRequested()));
        QSignalSpy saveSpy(dialog, SIGNAL(saveRequested()));

        // Bare letters and Ctrl chords do nothing, so a stray keypress
        // while typing can never discard work.
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_C);
        QTest::keyClick(window, Qt::Key_D);
        QTest::keyClick(window, Qt::Key_S);
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(cancelSpy.count(), 0);
        QCOMPARE(discardSpy.count(), 0);
        QCOMPARE(saveSpy.count(), 0);
        QVERIFY(dialog->property("opened").toBool());

        QTest::keyClick(window, Qt::Key_C, Qt::AltModifier);
        QTRY_COMPARE(cancelSpy.count(), 1);
        QVERIFY(!dialog->property("opened").toBool());

        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_D, Qt::AltModifier);
        QTRY_COMPARE(discardSpy.count(), 1);
        QVERIFY(!dialog->property("opened").toBool());

        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_S, Qt::AltModifier);
        QTRY_COMPARE(saveSpy.count(), 1);
        QVERIFY(!dialog->property("opened").toBool());

        // With the dialog closed, the mnemonics are inert.
        QTest::keyClick(window, Qt::Key_C, Qt::AltModifier);
        QTest::keyClick(window, Qt::Key_D, Qt::AltModifier);
        QTest::keyClick(window, Qt::Key_S, Qt::AltModifier);
        QCOMPARE(cancelSpy.count(), 1);
        QCOMPARE(discardSpy.count(), 1);
        QCOMPARE(saveSpy.count(), 1);
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
        backend.setDesktopTextScale(16.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 27);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 27);

        backend.setDesktopTextScale(9.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 15);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 15);
    }

    void stepsTextSizeOnTopOfTheDesktopScale() {
        Backend backend;
        QSignalSpy textScaleSpy(&backend, &Backend::textScaleChanged);
        backend.setDesktopTextScale(1.5);
        QCOMPARE(backend.textScale(), 1.5);

        backend.increaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 1.1);
        backend.decreaseTextSize();
        backend.decreaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 0.9);
        QCOMPARE(backend.status(), QStringLiteral("Text size 90%"));

        // The ends of the ladder hold instead of running away.
        for (int step = 0; step < 8; ++step)
            backend.decreaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 0.67);
        const int settledCount = textScaleSpy.count();
        backend.decreaseTextSize();
        QCOMPARE(textScaleSpy.count(), settledCount);

        // A new window picks the size back up.
        Backend reopened;
        QCOMPARE(reopened.textScale(), 0.67);

        backend.resetTextSize();
        QCOMPARE(backend.textScale(), 1.5);
        QCOMPARE(Backend().textScale(), 1.0);
    }

    void banksTouchpadScrollBeforeChangingTextSize() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *scroll = window->findChild<QObject *>(QStringLiteral("editorScroll"));
        QVERIFY(scroll);

        const auto scrollBy = [scroll](int angle, int pixels) {
            const QVariant wheel = QVariantMap{
                {QStringLiteral("angleDelta"), QVariantMap{{QStringLiteral("y"), angle}}},
                {QStringLiteral("pixelDelta"), QVariantMap{{QStringLiteral("y"), pixels}}}};
            QVERIFY(QMetaObject::invokeMethod(scroll, "zoomByWheel", Q_ARG(QVariant, wheel)));
        };

        // A touchpad trickles in pixels: a nudge is not a whole step yet. Its
        // events carry a synthesized angleDelta as well, as Wayland sends them.
        scrollBy(240, 20);
        scrollBy(240, 20);
        QCOMPARE(backend.textScale(), 1.0);
        scrollBy(240, 20);
        QCOMPARE(backend.textScale(), 1.1);

        // A wheel notch is one step, and reversing drops the banked remainder.
        scrollBy(-120, 0);
        QCOMPARE(backend.textScale(), 1.0);

        backend.resetTextSize();
    }

    void growsTextFromEitherPlusKey() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QQuickWindow *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        // Qt resolves a shifted = to Ctrl++ rather than to Ctrl+=, and a layout
        // that puts + on its own key never produces Ctrl+= at all.
        QTest::keyClick(window, Qt::Key_Plus, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.1);
        QTest::keyClick(window, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.25);
        QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.1);

        backend.resetTextSize();
    }

    void keepsTextColumnInsideNarrowWindows() {
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
        QObject *viewport = editor->parent();
        QVERIFY(viewport);

        // A resize reaches the window through its platform window, so the
        // bindings only see the new width once the events are delivered.
        const int minimumWidth = window->property("minimumWidth").toInt();
        window->setProperty("width", minimumWidth);
        QCoreApplication::processEvents();
        QCOMPARE(window->property("width").toInt(), minimumWidth);
        QVERIFY(editor->property("x").toInt() > 0);

        // A tiling compositor resizes below the minimum the window asks for.
        window->setProperty("width", 394);
        QCoreApplication::processEvents();
        QCOMPARE(window->property("width").toInt(), 394);
        QVERIFY(editor->property("width").toInt()
                <= viewport->property("width").toInt());
        QVERIFY(editor->property("x").toInt() >= 0);
    }

    void shortcutsDialogHasNoBindingLoop() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        // Material's Dialog derives implicitWidth from its content item; a bare
        // Label assigned as contentItem feeds back into that binding and QML
        // reports a binding loop for the dialog on every launch.
        BindingLoopCounter bindingLoops;

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *dialog = window->findChild<QObject *>(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(dialog->property("implicitWidth").toReal() > 0, true);

        QCOMPARE(bindingLoops.count(), 0);
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

    void autoSavesTitledDocumentsAfterEdits() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("draft.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);

        // Give the document a home on disk, then edit it without saving again.
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);
        editor->setProperty("text", QStringLiteral("auto-saved body"));
        QVERIFY(backend.editorTextChanged());

        // The debounced timer should flush to disk with no manual save.
        QTRY_VERIFY(savedSpy.count() >= 1);
        QVERIFY(!backend.modified());

        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(written.readAll()),
                 QStringLiteral("auto-saved body"));
    }

    void doesNotAutoSaveUntitledDocuments() {
        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);

        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);
        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        editor->setProperty("text", QStringLiteral("nameless draft"));
        QVERIFY(backend.editorTextChanged());

        // Wait past the auto-save debounce: an untitled draft must never auto-save
        // or silently raise the save picker. Crash recovery still covers it.
        QTest::qWait(1800);
        QCOMPARE(savedSpy.count(), 0);
        QCOMPARE(saveDialogSpy.count(), 0);
        QVERIFY(backend.modified());
        // Leave no draft behind for the next test's backend to recover.
        backend.discardRecovery();
    }

    void holdsAutoSaveWhenTheFileChangesElsewhere() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("shared.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        // Another program writes while our auto-save is pending.
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());
        writeFile(path, "theirs");

        // Their version survives the debounce, and further typing.
        QTRY_VERIFY(externalChangeSpy.count() >= 1);
        editor->setProperty("text", QStringLiteral("mine, still typing"));
        QVERIFY(backend.editorTextChanged());
        QTest::qWait(1200);
        QCOMPARE(readFile(path), QByteArray("theirs"));
        QVERIFY(backend.modified());

        // Keeping our version is a choice to overwrite, so auto-save resumes.
        backend.keepExternalVersion();
        QTRY_COMPARE(readFile(path), QByteArray("mine, still typing"));
        QVERIFY(!backend.modified());
    }

    void checksTheFileBeforeEachAutoSave() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("unwatched.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        // Change the file without the watcher seeing it, then let auto-save run.
        writeFileUnwatched(backend, path, "theirs");
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());

        QTRY_VERIFY(externalChangeSpy.count() >= 1);
        QTest::qWait(700);
        QCOMPARE(readFile(path), QByteArray("theirs"));
        QVERIFY(backend.modified());
        backend.discardRecovery();
    }

    void resumesAutoSaveOnlyAfterAnExplicitSave() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("dismissed.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        writeFile(path, "theirs");
        QTRY_VERIFY(externalChangeSpy.count() >= 1);

        // The prompt is dismissed (Escape): neither reload nor keep is chosen.
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());
        QTest::qWait(1200);
        QCOMPARE(readFile(path), QByteArray("theirs"));

        backend.save();
        QCOMPARE(readFile(path), QByteArray("mine"));

        editor->setProperty("text", QStringLiteral("mine, auto-saved again"));
        QVERIFY(backend.editorTextChanged());
        QTRY_COMPARE(readFile(path), QByteArray("mine, auto-saved again"));
    }

    void resumesAutoSaveAfterReloading() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("reloaded.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        writeFile(path, "theirs");
        QTRY_VERIFY(externalChangeSpy.count() >= 1);
        backend.reloadFromDisk();
        QCOMPARE(editor->property("text").toString(), QStringLiteral("theirs"));

        editor->setProperty("text", QStringLiteral("theirs, edited"));
        QVERIFY(backend.editorTextChanged());
        QTRY_COMPARE(readFile(path), QByteArray("theirs, edited"));
    }

    // Saving writes UTF-8 with plain line endings, so a file holding anything
    // else is only rewritten by an explicit save, never by typing.
    void keepsOtherEncodingsAndLineEndingsOutOfAutoSave() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QList<QByteArray> originals{QByteArray("one\r\ntwo\r\n"),
                                          QByteArray("caf\xe9\n")};
        for (const QByteArray &original : originals) {
            const QString path = directory.filePath(QStringLiteral("kept.md"));
            writeFile(path, original);

            Backend backend;
            QScopedPointer<QObject> editor(makeEditor(backend));
            QVERIFY(editor);
            backend.open(QUrl::fromLocalFile(path));
            QVERIFY(backend.status().contains(QStringLiteral("Auto-save is off")));

            editor->setProperty("text", QStringLiteral("changed\n"));
            QVERIFY(backend.editorTextChanged());
            QVERIFY(!backend.m_autoSaveTimer.isActive());
            QTest::qWait(700);
            QCOMPARE(readFile(path), original);

            // Keeping our version over an outside edit does not lift it either.
            backend.keepExternalVersion();
            QVERIFY(!backend.m_autoSaveTimer.isActive());

            // An explicit save still writes.
            backend.save();
            QCOMPARE(readFile(path), QByteArray("changed\n"));
        }
    }

    void holdsAutoSaveWhileTheWriterDecides() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("deciding.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);

        // Closing right after typing opens the unsaved-changes prompt.
        editor->setProperty("text", QStringLiteral("undecided"));
        QVERIFY(backend.editorTextChanged());
        backend.holdAutoSave(true);
        QTest::qWait(1200);
        QCOMPARE(savedSpy.count(), 0);
        QCOMPARE(readFile(path), QByteArray());

        // Cancel lets auto-save carry on.
        backend.holdAutoSave(false);
        QTRY_COMPARE(readFile(path), QByteArray("undecided"));
    }

    void startsANamedFileThatIsNotThereYet() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("new.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);

        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(backend.fileName(), QStringLiteral("new.md"));
        QCOMPARE(backend.status(), QStringLiteral("New file new.md"));
        QVERIFY(!backend.modified());
        QVERIFY(!QFileInfo::exists(path));

        // Typing does not auto-save the file into existence.
        editor->setProperty("text", QStringLiteral("first words"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
        QTest::qWait(800);
        QVERIFY(!QFileInfo::exists(path));

        // The first save lands on the name given, without asking for one.
        QSignalSpy saveAsSpy(&backend, &Backend::saveDialogRequested);
        backend.save();
        QCOMPARE(saveAsSpy.count(), 0);
        QCOMPARE(readFile(path), QByteArray("first words"));
        QVERIFY(!backend.modified());

        // From then on it is an ordinary file, auto-saved like any other.
        editor->setProperty("text", QStringLiteral("more words"));
        QVERIFY(backend.editorTextChanged());
        QTRY_COMPARE(readFile(path), QByteArray("more words"));
        QCOMPARE(externalSpy.count(), 0);
    }

    void refusesANewFileInAMissingFolder() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("missing/new.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(backend.status(), QStringLiteral("Could not open new.md."));
        QVERIFY(backend.fileName() != QStringLiteral("new.md"));
    }

    void namesARelativeFileByItsFullPath() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(directory.path()));

        Backend backend;
        backend.openOrStartFile(QUrl::fromLocalFile(QStringLiteral("relative.md")));
        QDir::setCurrent(previous);

        // A recovery draft written later must not depend on the directory.
        QCOMPARE(backend.fileUrl().toLocalFile(),
                 QDir(directory.path()).absoluteFilePath(QStringLiteral("relative.md")));
    }

    void saysWhenARecoveredDraftKeepsAFileFromOpening() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("asked.md"));
        writeFile(path, "on disk");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        // As after restoring a draft: the window holds unsaved text.
        editor->setProperty("text", QStringLiteral("recovered"));
        QVERIFY(backend.editorTextChanged());

        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(backend.status(),
                 QStringLiteral("Recovered unsaved changes; asked.md was not opened."));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("recovered"));
        backend.discardRecovery();
    }

    void dropsAPendingCloseWhenItsSaveFails() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        // The writer chose Save in the close prompt, and the save failed.
        window->setProperty("pendingAction", QStringLiteral("close"));
        window->setProperty("awaitingPendingSave", true);
        QSignalSpy failedSpy(&backend, &Backend::saveFailed);
        backend.saveAs(QUrl::fromLocalFile(QStringLiteral("/nonexistent-dir/x.md")));
        QCOMPARE(failedSpy.count(), 1);

        // A later save, such as an auto-save, must not close the window.
        QCOMPARE(window->property("pendingAction").toString(), QString());
        QVERIFY(!window->property("awaitingPendingSave").toBool());
    }

    void savesInAFolderItCannotWriteNewFilesIn() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("shared.md"));
        writeFile(path, "before");
        QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner);

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("after"));
        QVERIFY(backend.editorTextChanged());
        backend.save();
        QFile::setPermissions(directory.path(),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        QVERIFY(!backend.modified());
        QCOMPARE(readFile(path), QByteArray("after"));
    }

    void removesARecoveryDraftItCannotRead() {
        Backend backend;
        const QString draft = backend.m_recoveryPath;
        QVERIFY(!draft.isEmpty());
        writeFile(draft, "not json");

        // Attaching the document is when a draft is restored.
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        QVERIFY(!QFileInfo::exists(draft));
        QVERIFY(!backend.modified());
    }

    void keepsWatchingAFileReplacedFromOutside() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("watched.md"));
        writeFile(path, "first");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);

        // Editors and sync tools often replace a file rather than write into it.
        const QString replacement = directory.filePath(QStringLiteral("replacement"));
        writeFile(replacement, "second");
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(replacement, path));
        QTRY_VERIFY(externalSpy.count() >= 1);
        QTRY_VERIFY(backend.m_fileWatcher.files().contains(path));
    }

    void keepsTheWritersTextWhenTheChangeDialogIsDismissed() {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Controls\nApplicationWindow {"
                          " property alias dialog: dialog\n"
                          " ExternalChangeDialog { id: dialog } }",
                          QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY(window);
        QObject *dialog = window->property("dialog").value<QObject *>();
        QVERIFY(dialog);
        QSignalSpy keepSpy(dialog, SIGNAL(keepRequested()));
        QSignalSpy reloadSpy(dialog, SIGNAL(reloadRequested()));

        // Escape closes it without a choice.
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_COMPARE(keepSpy.count(), 1);
        QCOMPARE(reloadSpy.count(), 0);
    }

    void opensAnExistingFileFromTheCommandLine() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("notes.md"));
        writeFile(path, "already here");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("already here"));
        QCOMPARE(backend.status(), QStringLiteral("Opened notes.md"));
    }

    void keepsDiscardedChangesOutOfTheFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("draft.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("original"));
        QVERIFY(backend.editorTextChanged());
        backend.saveAs(QUrl::fromLocalFile(path));

        // The writer edits, then picks Discard to open a file that fails.
        backend.holdAutoSave(true);
        editor->setProperty("text", QStringLiteral("changes to throw away"));
        QVERIFY(backend.editorTextChanged());
        backend.discardRecovery();
        backend.open(QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing.md"))));
        backend.holdAutoSave(false);

        QTest::qWait(800);
        QCOMPARE(readFile(path), QByteArray("original"));
    }

    void forgetsTheSavedTextWhenTheFileChangedUnseen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("draft.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("original"));
        QVERIFY(backend.editorTextChanged());
        backend.saveAs(QUrl::fromLocalFile(path));

        // The file changes without the watcher noticing; the pre-save check
        // catches it.
        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);
        writeFileUnwatched(backend, path, "theirs");
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());
        QTRY_VERIFY(externalSpy.count() >= 1);

        // Undoing back to the old text is not a return to what is on disk.
        editor->setProperty("text", QStringLiteral("original"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
        backend.discardRecovery();
    }

    void doesNotAutoSaveARecoveredDraftOverItsFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("recovered.md"));
        const QUrl url = QUrl::fromLocalFile(path);

        {
            // A session that ends without saving leaves a recovery draft.
            Backend crashed;
            QScopedPointer<QObject> editor(makeEditor(crashed));
            QVERIFY(editor);
            crashed.saveAs(url);
            editor->setProperty("text", QStringLiteral("draft"));
            crashed.m_autoSaveSuspended = true; // stand in for a crash before auto-save
            QVERIFY(crashed.editorTextChanged());
            QTest::qWait(1000);
        }
        QCOMPARE(readFile(path), QByteArray());

        // The file moves on before Omawrite comes back.
        writeFile(path, "newer on disk");

        // Earlier tests may have left drafts of their own; claim until ours.
        for (int attempt = 0; attempt < 100; ++attempt) {
            Backend restored;
            QScopedPointer<QObject> editor(makeEditor(restored));
            QVERIFY(editor);
            if (restored.fileUrl() != url) {
                restored.discardRecovery();
                continue;
            }

            QCOMPARE(editor->property("text").toString(), QStringLiteral("draft"));
            editor->setProperty("text", QStringLiteral("draft, edited"));
            QVERIFY(restored.editorTextChanged());
            QTest::qWait(1200);
            QCOMPARE(readFile(path), QByteArray("newer on disk"));
            QVERIFY(restored.modified());

            restored.save();
            QCOMPARE(readFile(path), QByteArray("draft, edited"));
            return;
        }
        QFAIL("The recovery draft was not restored.");
    }

    void revertsEmptyDocumentToCleanState() {
        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        QVERIFY(!backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        quickDocument->textDocument()->setPlainText(QString());
        QVERIFY(backend.editorTextChanged());
        QVERIFY(!backend.modified());
    }

    void revertsSavedDocumentToCleanState() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("doc.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(file.write("hello"), qint64(5));
        file.close();

        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        backend.open(QUrl::fromLocalFile(path));
        QVERIFY(!backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello world"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(!backend.modified());
    }

    void keepsADocumentUnsavedAfterItsFileDisappears() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("gone.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(file.write("hello"), qint64(5));
        file.close();

        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);
        backend.open(QUrl::fromLocalFile(path));
        QVERIFY(!backend.modified());

        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(externalChangeSpy.count(), 1);

        // The writer can dismiss the dialog without picking a version, so the
        // text the deleted file used to hold is not a saved state to return to.
        quickDocument->textDocument()->setPlainText(QStringLiteral("hello world"));
        QVERIFY(backend.editorTextChanged());
        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        // Nor is emptying it: an empty document only reads as clean while there
        // is no file behind it.
        quickDocument->textDocument()->setPlainText(QString());
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
    }

private:
    // The character formats the highlighter gives a single line of text.
    static QList<QTextCharFormat> highlightedFormats(const QString &text) {
        QTextDocument document;
        document.setPlainText(text);
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();
        QList<QTextCharFormat> formats(text.length());
        for (const auto &range : document.firstBlock().layout()->formats()) {
            for (int i = range.start; i < range.start + range.length; ++i)
                formats[i] = range.format;
        }
        return formats;
    }

    // Points HOME at a scratch tree holding one colors.toml, and puts it back on the way out.
    struct ScopedTheme {
        explicit ScopedTheme(const QByteArray &palette) {
            const QString themeDirectory = home.path()
                + QStringLiteral("/.local/state/omarchy/current/theme");
            QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
            ok = home.isValid() && QDir().mkpath(themeDirectory)
                && qputenv("HOME", home.path().toUtf8())
                && colorsFile.open(QIODevice::WriteOnly | QIODevice::Text)
                && colorsFile.write(palette) == qint64(palette.size());
        }
        ~ScopedTheme() { qputenv("HOME", originalHome); }

        QTemporaryDir home;
        QByteArray originalHome = qgetenv("HOME");
        bool ok = false;
    };

    // Builds a minimal TextEdit and attaches its document to the backend, so
    // editorTextChanged() sees real edits. The engine outlives the returned
    // editor because it is a data member.
    QObject *makeEditor(Backend &backend) {
        QQmlComponent component(&m_engine);
        component.setData(
            "import QtQuick\nTextEdit { property var doc: textDocument }", QUrl());
        if (!component.isReady()) {
            qWarning() << component.errorString();
            return nullptr;
        }
        QObject *editor = component.create();
        if (editor)
            backend.attachDocument(editor->property("doc").value<QObject *>());
        return editor;
    }

    static void writeFile(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(contents), qint64(contents.size()));
    }

    // Writes while the backend is not watching, as when an event is missed.
    static void writeFileUnwatched(Backend &backend, const QString &path,
                                   const QByteArray &contents) {
        backend.m_fileWatcher.removePath(path);
        writeFile(path, contents);
    }

    static QByteArray readFile(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    // Loads a bare TextEdit, attaches its document to the backend, and returns
    // the QQuickTextDocument. The engine and TextEdit are parented to the
    // backend so they outlive the helper call.
    QQuickTextDocument *attachTextEdit(Backend *backend) {
        auto *engine = new QQmlEngine(backend);
        auto *component = new QQmlComponent(engine);
        component->setData(R"QML(
            import QtQuick
            TextEdit { }
        )QML", QUrl::fromLocalFile(QStringLiteral("Harness.qml")));
        if (!component->isReady())
            return nullptr;

        QObject *editor = component->create();
        if (!editor)
            return nullptr;
        editor->setParent(backend);

        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        if (!quickDocument)
            return nullptr;

        backend->attachDocument(quickDocument);
        return quickDocument;
    }

    QQmlEngine m_engine;
    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"

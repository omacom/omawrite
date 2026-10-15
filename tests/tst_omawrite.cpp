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
#include <QQuickTextDocument>
#include <QStandardPaths>
#include <QClipboard>
#include <QMimeData>
#include <QWheelEvent>

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

    void findsInlineMarkdownRanges() {
        const auto markup = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic* and [site](https://example.com)"));
        QCOMPARE(markup.size(), 3);
        QCOMPARE(markup.at(0).content.start, 2);
        QCOMPARE(markup.at(0).content.length, 4);
        QCOMPARE(markup.at(2).content.length, 4);
        QCOMPARE(markup.at(2).markers[0].length, 1);
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
        backend.setTextScale(16.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 27);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 27);

        backend.setTextScale(9.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 15);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 15);
    }

    void scalesTouchpadScrollingAndCarriesMomentum() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QObject *editor = root->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *flick = root->findChild<QObject *>(QStringLiteral("editorFlick"));
        QVERIFY(window);
        QVERIFY(editor);
        QVERIFY(flick);

        QStringList lines;
        for (int i = 0; i < 200; ++i)
            lines.append(QStringLiteral("A line long enough to make the editor scroll."));
        editor->setProperty("text", lines.join(QLatin1Char('\n')));
        QTRY_VERIFY(flick->property("contentHeight").toReal()
                    > flick->property("height").toReal() + 400);
        flick->setProperty("contentY", 200.0);

        const QPointF position(window->width() / 2.0, window->height() / 2.0);
        sendTouchpadWheel(window, position, -8, -120, Qt::ScrollBegin);
        QTest::qWait(20);
        sendTouchpadWheel(window, position, -10, -120, Qt::ScrollUpdate);
        QTest::qWait(20);
        sendTouchpadWheel(window, position, -10, -120, Qt::ScrollUpdate);

        const qreal gestureEndY = flick->property("contentY").toReal();
        QVERIFY2(qAbs(gestureEndY - 380.0) <= 2.0,
                 qPrintable(QStringLiteral("Expected angle-scaled contentY near 380, got %1")
                                .arg(gestureEndY)));

        sendTouchpadWheel(window, position, 0, 0, Qt::ScrollEnd);
        QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentY").toReal()
                                 > gestureEndY + 5.0, 500);

        // With scroll phases, a pause mid-swipe never coasts; only lifting does.
        flick->setProperty("contentY", 200.0);
        sendTouchpadWheel(window, position, -8, -120, Qt::ScrollBegin);
        QTest::qWait(20);
        sendTouchpadWheel(window, position, -10, -120, Qt::ScrollUpdate);
        const qreal restingY = flick->property("contentY").toReal();
        QTest::qWait(200);
        QVERIFY(qAbs(flick->property("contentY").toReal() - restingY) <= 1.0);

        // Lifting after that pause doesn't coast on the stale velocity.
        sendTouchpadWheel(window, position, 0, 0, Qt::ScrollEnd);
        QTest::qWait(100);
        QVERIFY(qAbs(flick->property("contentY").toReal() - restingY) <= 1.0);

        // Fine movement that rounds to no pixels still moves by its angle.
        flick->setProperty("contentY", 200.0);
        sendTouchpadWheel(window, position, 0, -24, Qt::ScrollBegin);
        sendTouchpadWheel(window, position, 0, -24, Qt::ScrollUpdate);
        QVERIFY(qAbs(flick->property("contentY").toReal() - 224.0) <= 1.0);
        sendTouchpadWheel(window, position, 0, 0, Qt::ScrollEnd);
        QTest::qWait(600);

        // A sideways-only update mid-swipe keeps the gesture and its momentum.
        flick->setProperty("contentY", 200.0);
        sendTouchpadWheel(window, position, -8, -120, Qt::ScrollBegin);
        QTest::qWait(20);
        sendTouchpadWheel(window, position, -10, -120, Qt::ScrollUpdate);
        sendTouchpadWheel(window, position, 0, 0, Qt::ScrollUpdate, 5);
        const qreal sidewaysY = flick->property("contentY").toReal();
        sendTouchpadWheel(window, position, 0, 0, Qt::ScrollEnd);
        QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentY").toReal() > sidewaysY + 5.0, 500);
        QTest::qWait(600);

        // A small nudge after a pause doesn't revive the earlier swipe's speed.
        sendTouchpadWheel(window, position, -8, -120, Qt::ScrollBegin);
        QTest::qWait(20);
        sendTouchpadWheel(window, position, -10, -120, Qt::ScrollUpdate);
        QTest::qWait(200);
        sendTouchpadWheel(window, position, -1, -1, Qt::ScrollUpdate);
        const qreal nudgedY = flick->property("contentY").toReal();
        sendTouchpadWheel(window, position, 0, 0, Qt::ScrollEnd);
        QTest::qWait(100);
        QVERIFY(qAbs(flick->property("contentY").toReal() - nudgedY) <= 1.0);

        // Sub-pixel movement adds up in both directions instead of rounding away.
        flick->setProperty("contentY", 200.0);
        for (int i = 0; i < 8; ++i)
            sendTouchpadWheel(window, position, 1, 1, Qt::ScrollUpdate);
        QVERIFY(qAbs(flick->property("contentY").toReal() - 196.0) <= 1.0);
        for (int i = 0; i < 8; ++i)
            sendTouchpadWheel(window, position, -1, -1, Qt::ScrollUpdate);
        QVERIFY(qAbs(flick->property("contentY").toReal() - 200.0) <= 1.0);
        sendTouchpadWheel(window, position, 0, 0, Qt::ScrollEnd);

        // Platform momentum replaces the fallback rather than adding to it.
        sendTouchpadWheel(window, position, -2, -24, Qt::ScrollBegin);
        sendTouchpadWheel(window, position, -6, -72, Qt::ScrollMomentum);
        const qreal platformMomentumY = flick->property("contentY").toReal();
        QVERIFY(flick->property("platformMomentumActive").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!flick->property("platformMomentumActive").toBool(),
                                 300);
        QVERIFY(qAbs(flick->property("contentY").toReal() - platformMomentumY) <= 1.0);

        // Fingers landing during platform momentum aren't ended by its timer.
        sendTouchpadWheel(window, position, -6, -72, Qt::ScrollMomentum);
        sendTouchpadWheel(window, position, -8, -120, Qt::ScrollBegin);
        QTest::qWait(20);
        sendTouchpadWheel(window, position, -10, -120, Qt::ScrollUpdate);
        const qreal interruptedY = flick->property("contentY").toReal();
        QTest::qWait(200);
        QVERIFY(qAbs(flick->property("contentY").toReal() - interruptedY) <= 1.0);
        sendTouchpadWheel(window, position, 0, 0, Qt::ScrollEnd);
        QTest::qWait(600);

        // Pixel-only platforms keep direct movement and coast after an event gap.
        flick->setProperty("contentY", 200.0);
        sendTouchpadWheel(window, position, -8, 0, Qt::NoScrollPhase);
        QTest::qWait(20);
        sendTouchpadWheel(window, position, -10, 0, Qt::NoScrollPhase);
        const qreal pixelEndY = flick->property("contentY").toReal();
        QVERIFY(qAbs(pixelEndY - 236.0) <= 2.0);
        QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentY").toReal() > pixelEndY + 5, 500);

        // A new gesture stops the coast immediately, even before movement.
        sendTouchpadWheel(window, position, 1, 0, Qt::ScrollBegin);
        const qreal stoppedY = flick->property("contentY").toReal();
        QTest::qWait(40);
        QVERIFY(qAbs(flick->property("contentY").toReal() - stoppedY) <= 1.0);
        sendTouchpadWheel(window, position, 0, 0, Qt::ScrollEnd);

        // Mouse-wheel input keeps the upstream animated 120px notch distance.
        flick->setProperty("contentY", 200.0);
        sendTouchpadWheel(window, position, 0, -120, Qt::NoScrollPhase);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(flick->property("contentY").toReal() - 320.0) <= 2.0, 500);

        // Momentum and direct movement never overshoot the content bounds.
        sendTouchpadWheel(window, position, 10000, 0, Qt::ScrollBegin);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
        sendTouchpadWheel(window, position, 0, 0, Qt::ScrollEnd);
        QTest::qWait(100);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
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

    void autoSavesFilesWithWindowsLineEndings() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("crlf.md"));
        writeFile(path, "one\r\ntwo\r\n");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        editor->setProperty("text", QStringLiteral("one\ntwo\nthree\n"));
        QVERIFY(backend.editorTextChanged());
        QTRY_VERIFY(!backend.modified());
        QCOMPARE(externalChangeSpy.count(), 0);
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
    static void sendTouchpadWheel(QQuickWindow *window, const QPointF &position,
                                  int pixelDeltaY, int angleDeltaY,
                                  Qt::ScrollPhase phase, int pixelDeltaX = 0) {
        QWheelEvent event(position, window->mapToGlobal(position),
                          QPoint(pixelDeltaX, pixelDeltaY), QPoint(pixelDeltaX * 12, angleDeltaY),
                          Qt::NoButton,
                          Qt::NoModifier, phase, false);
        QCoreApplication::sendEvent(window, &event);
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

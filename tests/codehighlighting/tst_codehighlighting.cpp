#include "../../src/codehighlighter.h"
#include "../../src/markdownhighlighter.h"

#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Theme>
#include <QElapsedTimer>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtTest>

namespace {
QTextCharFormat at(const QTextBlock &block, int offset) {
    for (const auto &range : block.layout()->formats()) {
        if (range.start <= offset && offset < range.start + range.length)
            return range.format;
    }
    return {};
}
QTextCharFormat tokenAt(const CodeHighlighter::Line &line, int offset) {
    for (const auto &token : line.tokens) {
        if (token.offset <= offset && offset < token.offset + token.length)
            return token.format;
    }
    return {};
}
void settle() { QCoreApplication::processEvents(); }
}

class CodeHighlightingTest : public QObject {
    Q_OBJECT
private slots:
    void aliases() {
        const QList<QPair<QString, QString>> names = {
            {"py", "Python"}, {"js", "JavaScript"}, {"ts", "TypeScript"},
            {"cpp", "C++"}, {"c++", "C++"}, {"sh", "Bash"}, {"bash", "Bash"},
            {"cs", "C#"}, {"yml", "YAML"}, {"JSON", "JSON"}, {"pYtHoN", "Python"}
        };
        for (const auto &name : names) {
            QCOMPARE(CodeHighlighter::resolveLanguage(name.first + " extra").name(), name.second);
        }
        QVERIFY(CodeHighlighter::resolveLanguage("rs").isValid());
        QVERIFY(!CodeHighlighter::resolveLanguage("").isValid());
        QVERIFY(!CodeHighlighter::resolveLanguage("does-not-exist").isValid());
        QVERIFY(!CodeHighlighter::resolveLanguage("../file.py").isValid());
        QVERIFY(!CodeHighlighter::resolveLanguage("*.py").isValid());
        KSyntaxHighlighting::Repository repo;
        for (const auto &definition : repo.definitions()) {
            for (const auto &alias : definition.alternativeNames()) {
                if (!alias.contains(' ')) {
                    QCOMPARE(CodeHighlighter::resolveLanguage(alias.toUpper()).name(),
                             repo.definitionForName(alias).name());
                    return;
                }
            }
        }
        QFAIL("No native alternative name available");
    }

    void colors_data() {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("offset");
        QTest::addColumn<int>("style");
        QTest::newRow("python") << QString("py") << QString("# comment") << 0 << int(KSyntaxHighlighting::Theme::Comment);
        QTest::newRow("cpp") << QString("cpp") << QString("// comment") << 0 << int(KSyntaxHighlighting::Theme::Comment);
        QTest::newRow("javascript") << QString("js") << QString("// comment") << 0 << int(KSyntaxHighlighting::Theme::Comment);
        QTest::newRow("json") << QString("json") << QString("{\"key\": \"value\"}") << 9 << int(KSyntaxHighlighting::Theme::String);
    }
    void colors() {
        QFETCH(QString, language); QFETCH(QString, text); QFETCH(int, offset); QFETCH(int, style);
        KSyntaxHighlighting::Repository repo;
        CodeHighlighter lexer;
        for (bool dark : {true, false}) {
            lexer.setDarkMode(dark);
            const auto line = lexer.tokenize(text, CodeHighlighter::resolveLanguage(language));
            const auto format = tokenAt(line, offset);
            const auto theme = repo.defaultTheme(dark ? KSyntaxHighlighting::Repository::DarkTheme
                                                     : KSyntaxHighlighting::Repository::LightTheme);
            QCOMPARE(format.foreground().color(), QColor::fromRgba(theme.textColor(KSyntaxHighlighting::Theme::TextStyle(style))));
            for (const auto &token : line.tokens) {
                QVERIFY(token.offset >= 0);
                QVERIFY(token.offset + token.length <= text.size());
                for (auto property : token.format.properties().keys()) {
                    QVERIFY(property == QTextFormat::ForegroundBrush || property == QTextFormat::FontWeight
                            || property == QTextFormat::FontItalic);
                }
            }
        }
    }

    void multilineAndReset() {
        CodeHighlighter lexer;
        const auto cpp = CodeHighlighter::resolveLanguage("cpp");
        auto line = lexer.tokenize("/*", cpp);
        QVERIFY(line.state != KSyntaxHighlighting::State());
        const auto empty = lexer.tokenize("", cpp, line.state);
        const auto comment = lexer.tokenize("int value;", cpp, empty.state);
        const auto fresh = lexer.tokenize("int value;", cpp);
        QVERIFY(tokenAt(comment, 0).foreground() != tokenAt(fresh, 0).foreground());
        const auto ended = lexer.tokenize("*/", cpp, comment.state);
        QCOMPARE(tokenAt(lexer.tokenize("int value;", cpp, ended.state), 0), tokenAt(fresh, 0));
        const auto py = CodeHighlighter::resolveLanguage("py");
        const auto string = lexer.tokenize("value = \"\"\"", py);
        QVERIFY(tokenAt(lexer.tokenize("def hello():", py, string.state), 0).foreground()
                != tokenAt(lexer.tokenize("def hello():", py), 0).foreground());
        QVERIFY(lexer.tokenize("def hello():", {}, string.state).tokens.isEmpty());
    }

    void editorEditsAndBoundaries() {
        QTextDocument doc;
        doc.setPlainText("~~~py\ndef hello():\n~~~\n```cpp\n/*\nint value;\n```\n```cpp\nint fresh;\n```\n```unknown\ndef plain():\n```\n```\ndef plain():\n```");
        MarkdownHighlighter highlighter(&doc);
        highlighter.rehighlight(); settle();
        const auto block = [&doc](int n) { return doc.findBlockByNumber(n); };
        QCOMPARE(at(block(1), 0).fontWeight(), QFont::Bold);
        QCOMPARE(block(0).userState(), int(MarkdownHighlighter::InsideFence + 1));
        QCOMPARE(block(2).userState(), int(MarkdownHighlighter::Prose));
        QVERIFY(at(block(5), 0).foreground() != at(block(8), 0).foreground());
        QCOMPARE(at(block(11), 0).foreground().color(), QColor("#eeeeee"));
        QCOMPARE(at(block(14), 0).foreground().color(), QColor("#eeeeee"));
        doc.documentLayout(); // Enable QTextDocument's incremental change notifications.
        QTextCursor edit(&doc);
        edit.setPosition(3); edit.setPosition(5, QTextCursor::KeepAnchor); edit.insertText("cpp");
        // No explicit rehighlight or event-loop turn: edits must reach the body.
        QCOMPARE(at(block(1), 0).fontWeight(), QFont::Normal);
        settle();
        doc.undo(); settle();
        QCOMPARE(at(block(1), 0).fontWeight(), QFont::Bold);
        // Same integer fence state, changed embedded multiline state.
        edit.setPosition(block(4).position()); edit.setPosition(block(4).position() + 2, QTextCursor::KeepAnchor);
        edit.insertText("//"); settle();
        QCOMPARE(at(block(5), 0).foreground(), at(block(8), 0).foreground());
        doc.undo(); settle();
        QVERIFY(at(block(5), 0).foreground() != at(block(8), 0).foreground());
    }

    void incrementalMatchesFullHighlight() {
        QTextDocument doc;
        doc.setPlainText("````py\nvalue = \"\"\"\n\ninside\n````\n~~~cpp\nint value;\n~~~\nend");
        doc.documentLayout();
        MarkdownHighlighter highlighter(&doc);
        highlighter.rehighlight(); settle();
        const auto compareFull = [&] {
            QTextDocument full;
            full.setPlainText(doc.toPlainText());
            MarkdownHighlighter expected(&full);
            expected.rehighlight();
            for (auto actual = doc.begin(), fresh = full.begin(); actual.isValid();
                 actual = actual.next(), fresh = fresh.next()) {
                QVERIFY(fresh.isValid());
                QCOMPARE(actual.userState(), fresh.userState());
                for (int offset = 0; offset < actual.text().size(); ++offset)
                    QCOMPARE(at(actual, offset), at(fresh, offset));
            }
        };
        const auto replace = [&](const QString &from, const QString &to) {
            const int start = doc.toPlainText().indexOf(from);
            QVERIFY(start >= 0);
            QTextCursor edit(&doc);
            edit.setPosition(start); edit.setPosition(start + from.size(), QTextCursor::KeepAnchor);
            edit.insertText(to); settle(); compareFull();
        };
        compareFull();
        replace("````py", "````cpp");
        replace("````cpp", "````unknown-language");
        replace("````unknown-language", "````");
        replace("````\nvalue", "````py\nvalue");
        replace("value = \"\"\"", "value = 42");
        replace("value = 42", "value = \"\"\"");
        replace("inside", "inside\nmore");
        replace("inside\nmore", "inside");
        replace("````\n~~~cpp", "```\n~~~cpp"); // Too short to close.
        replace("```\n~~~cpp", "````\n~~~cpp");
        replace("~~~cpp", "~~~~cpp"); // Unterminated tilde fence.
        replace("~~~~cpp", "~~~cpp");
    }

    void overlaysFontThemeAndUndo() {
        QTextDocument doc;
        const QString source = "```py\ndef hello():\n    return 42\n```";
        doc.setPlainText(source);
        doc.clearUndoRedoStacks();
        MarkdownHighlighter highlighter(&doc);
        highlighter.rehighlight(); settle();
        const auto body = doc.findBlockByNumber(1);
        const auto original = at(body, 0);
        highlighter.setSearch("def", body.position());
        QCOMPARE(at(body, 0).foreground(), original.foreground());
        QCOMPARE(at(body, 0).background().color(), QColor("#b36b20"));
        highlighter.setCaret(body.position());
        highlighter.setSelection(body.position(), body.position() + 3);
        QFont font("monospace"); font.setPointSizeF(23);
        doc.setDefaultFont(font); highlighter.refreshFont();
        QVERIFY(!at(body, 0).hasProperty(QTextFormat::FontPointSize));
        QVERIFY(!at(body, 0).hasProperty(QTextFormat::FontFamily));
        highlighter.setTheme(false, "#ffffff", "#222324", "#2077b2", "#abcdef");
        QVERIFY(at(body, 0).foreground() != original.foreground());
        QCOMPARE(at(body, 0).background().color(), QColor("#ffad42"));
        highlighter.setSearch("", -1);
        QCOMPARE(at(body, 0).background().style(), Qt::NoBrush); // Fenced fill is a presentation panel.
        QCOMPARE(doc.toPlainText(), source);
        QVERIFY(!doc.isUndoAvailable());
        QTextCursor cursor(&doc); cursor.movePosition(QTextCursor::End); cursor.insertText("!");
        settle(); highlighter.setTheme(true, "", "", "", "");
        doc.undo(); settle(); QCOMPARE(doc.toPlainText(), source);
        QVERIFY(!doc.isUndoAvailable());
        doc.redo(); settle(); QCOMPARE(doc.toPlainText(), source + "!");
    }

    void incrementalPerformance() {
        QTextDocument doc;
        QString source = "```cpp\n";
        for (int i = 0; i < 12000; ++i)
            source += "int value = 42; // comment\n";
        source += "```\nend";
        doc.setPlainText(source);
        MarkdownHighlighter highlighter(&doc);
        highlighter.rehighlight(); settle();
        doc.documentLayout();
        QElapsedTimer timer; timer.start();
        QTextCursor edit(doc.findBlockByNumber(6000)); edit.insertText(" "); settle();
        const qint64 editTime = timer.elapsed();
        timer.restart();
        for (int i = 0; i < 100; ++i)
            highlighter.setCaret(doc.findBlockByNumber(6000 + i % 2).position());
        settle();
        QVERIFY2(editTime < 2000, qPrintable(QString("Local edit took %1 ms").arg(editTime)));
        QVERIFY2(timer.elapsed() < 2000, "Caret moves should not retokenize the whole fence");
        // A language change must traverse the fence, but still be linear.
        timer.restart();
        edit.setPosition(3); edit.setPosition(6, QTextCursor::KeepAnchor); edit.insertText("py"); settle();
        const qint64 languageTime = timer.elapsed();
        QVERIFY2(languageTime < 5000, "Language propagation should not trigger quadratic layout");
        qInfo() << "12000-line fence: local edit" << editTime << "ms; language edit" << languageTime << "ms";
        QCOMPARE(doc.toPlainText().count('\n'), source.count('\n'));
    }
};
QTEST_MAIN(CodeHighlightingTest)
#include "tst_codehighlighting.moc"

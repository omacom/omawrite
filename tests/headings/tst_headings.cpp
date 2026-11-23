#include "markdownhighlighter.h"

#include <QFont>
#include <QImage>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtTest>

class HeadingTests : public QObject {
    Q_OBJECT

    static QTextCharFormat formatAt(const QTextDocument &document, int position) {
        const QTextBlock block = document.findBlock(position);
        const int offset = position - block.position();
        for (const auto &range : block.layout()->formats()) {
            if (offset >= range.start && offset < range.start + range.length)
                return range.format;
        }
        return {};
    }

    static bool hidden(const QTextDocument &document, int position) {
        return formatAt(document, position).fontPointSize() == 1.0;
    }

private slots:
    void richObjectHighlightingNeverZerosThePage() {
        class Observer : public MarkdownHighlighter {
        public:
            using MarkdownHighlighter::MarkdownHighlighter;
            bool sawZeroPage = false;
            void highlightBlock(const QString &text) override {
                sawZeroPage |= document()->pageSize() == QSizeF(0, 0);
                MarkdownHighlighter::highlightBlock(text);
            }
        };
        QTextDocument document;
        document.setTextWidth(400);
        QTextCursor cursor(&document);
        QImage image(160, 100, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::darkCyan);
        document.addResource(QTextDocument::ImageResource, QUrl("preview:test"), image);
        QTextImageFormat imageFormat;
        imageFormat.setName("preview:test");
        cursor.insertImage(imageFormat);
        cursor.insertText("\n");
        for (int i = 0; i < 20; ++i)
            cursor.insertText(QStringLiteral("# Heading %1\n").arg(i));
        cursor.insertText("```cpp\nint value;\n/* open\nclose */\n```\n");
        Observer highlighter(&document);
        highlighter.setTheme(true, "#111111", "#eeeeee", "#5584aa", "#252525");
        highlighter.refreshFont();
        QVERIFY(!highlighter.sawZeroPage);
        highlighter.setSelection(0, document.characterCount() - 1);
        QVERIFY(!highlighter.sawZeroPage);
        cursor.setPosition(document.toPlainText().indexOf("/* open"));
        cursor.deleteChar();
        cursor.deleteChar();
        QVERIFY(!highlighter.sawZeroPage);
        QCOMPARE(document.textWidth(), 400.0);
    }

    void headingSizesMatchReadAndActiveMarkersAreMuted() {
        for (bool pixels : {false, true}) {
            for (int size : {12, 24}) {
                for (bool dark : {false, true}) {
                    for (int level = 1; level <= 6; ++level) {
                        const QString markdown = QString(level, QLatin1Char('#')) + QStringLiteral(" Title ###");
                        QFont font;
                        if (pixels) font.setPixelSize(size);
                        else font.setPointSize(size);
                        QTextDocument document;
                        document.setDefaultFont(font);
                        const QString source = markdown + QStringLiteral("\n\nafter");
                        document.setPlainText(source);
                        MarkdownHighlighter highlighter(&document);
                        highlighter.setTheme(dark, {}, {}, {}, {});
                        highlighter.setCaret(source.size());
                        highlighter.rehighlight();
                        QTextDocument read;
                        read.setDefaultFont(font);
                        read.setMarkdown(markdown);
                        document.size();
                        read.size();
                        const auto renderedSize = [](const QTextBlock &block, int start, int length) {
                            const auto runs = block.layout()->glyphRuns(start, length);
                            return runs.isEmpty() ? qreal(-1) : runs.first().rawFont().pixelSize();
                        };
                        const qreal expected = renderedSize(read.begin(), 0, 5);
                        QVERIFY(expected > 0);
                        QCOMPARE(renderedSize(document.begin(), level + 1, 5), expected);
                        QVERIFY(hidden(document, 0));
                        highlighter.setCaret(level + 2);
                        document.size();
                        QCOMPARE(renderedSize(document.begin(), level + 1, 5), expected);
                        QCOMPARE(renderedSize(document.begin(), 0, level), expected);
                        const QColor marker = dark ? QColor("#4f525a") : QColor("#aeb1b5");
                        QCOMPARE(formatAt(document, 0).foreground().color(), marker);
                        QCOMPARE(formatAt(document, markdown.size() - 1).foreground().color(), marker);
                        QCOMPARE(document.toPlainText(), source);
                        QVERIFY(!document.isUndoAvailable());
                    }
                }
            }
        }
    }

    void largeSelectionOnlyRestylesChangedBlocks() {
        class CountingHighlighter : public MarkdownHighlighter {
        public:
            using MarkdownHighlighter::MarkdownHighlighter;
            int calls = 0;
        protected:
            void highlightBlock(const QString &text) override {
                ++calls;
                MarkdownHighlighter::highlightBlock(text);
            }
        };
        QTextDocument document(QStringLiteral("# Heading\n").repeated(4000));
        document.setTextWidth(600);
        document.size();
        CountingHighlighter highlighter(&document);
        highlighter.rehighlight();
        highlighter.calls = 0;
        highlighter.setSelection(0, document.characterCount() - 1);
        QVERIFY(highlighter.calls <= document.blockCount());
        highlighter.calls = 0;
        highlighter.setSelection(0, document.characterCount() - 1);
        QCOMPARE(highlighter.calls, 0);
        highlighter.setSelection(1, document.characterCount() - 2);
        QVERIFY(highlighter.calls <= 1);
        highlighter.calls = 0;
        highlighter.setSelection(1, document.findBlockByNumber(3998).position());
        QVERIFY(highlighter.calls <= 2);
        QCOMPARE(document.toPlainText(), QStringLiteral("# Heading\n").repeated(4000));
    }

    void syntax_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("openingEnd");
        QTest::addColumn<int>("closingStart");
        QTest::newRow("ordinary") << "# title" << 2 << -1;
        QTest::newRow("indented") << "   ###\ttitle ###  " << 7 << 12;
        QTest::newRow("empty") << "#" << 1 << -1;
        QTest::newRow("empty-closing") << "## ###" << 3 << 3;
        QTest::newRow("six") << "###### title ##" << 7 << 12;
        QTest::newRow("seven") << "####### title" << 0 << -1;
        QTest::newRow("no-separator") << "#title" << 0 << -1;
        QTest::newRow("four-spaces") << "    # title" << 0 << -1;
        QTest::newRow("tab-indent") << "\t# title" << 0 << -1;
        QTest::newRow("embedded-hashes") << "# title###" << 2 << -1;
        QTest::newRow("escaped-hash") << "# title \\###" << 2 << -1;
        QTest::newRow("nonterminal") << "# title ### more" << 2 << -1;
        QTest::newRow("escaped-heading") << "\\# title" << 0 << -1;
    }

    void syntax() {
        QFETCH(QString, text);
        QFETCH(int, openingEnd);
        QFETCH(int, closingStart);
        QTextDocument document(text);
        MarkdownHighlighter highlighter(&document);
        highlighter.rehighlight();
        for (int i = 0; i < text.size(); ++i)
            QCOMPARE(hidden(document, i), i < openingEnd || (closingStart >= 0 && i >= closingStart));
        QCOMPARE(document.toPlainText(), text);
    }

    void caretAnywhere() {
        const QString text = "   ## title ##  \nplain";
        QTextDocument document(text);
        MarkdownHighlighter highlighter(&document);
        highlighter.rehighlight();
        const int end = text.indexOf('\n');
        for (int caret = 0; caret <= end; ++caret) {
            highlighter.setCaret(caret);
            QVERIFY(!hidden(document, 3));
            QVERIFY(!hidden(document, end - 3));
            highlighter.setCaret(end + 1);
            QVERIFY(hidden(document, 3));
            QVERIFY(hidden(document, end - 3));
        }
        QCOMPARE(document.toPlainText(), text);
    }

    void selectionCrossingHeadings() {
        QTextDocument document("plain\n# one\nbody\n## two ##\nend");
        MarkdownHighlighter highlighter(&document);
        highlighter.setCaret(document.characterCount() - 1);
        highlighter.rehighlight();
        const int first = document.findBlockByNumber(1).position();
        const int second = document.findBlockByNumber(3).position();
        QVERIFY(hidden(document, first));
        QVERIFY(hidden(document, second));
        highlighter.setSelection(1, second + 4);
        QVERIFY(!hidden(document, first));
        QVERIFY(!hidden(document, second));
        highlighter.setSelection(second + 2, second + 1);
        QVERIFY(hidden(document, first));
        QVERIFY(!hidden(document, second));
        highlighter.setSelection(0, first);
        QVERIFY(!hidden(document, first));
        QVERIFY(hidden(document, second));
        highlighter.setSelection(0, 0);
        QVERIFY(hidden(document, first));
        highlighter.setSelection(second + 3, second + 4);
        QVERIFY(!hidden(document, second));
        highlighter.setSelection(-100, -10);
        QVERIFY(hidden(document, second));
    }

    void inlineAndFences() {
        const QString text = "# *it* [link](url) `code` ###\n```\n# literal ###\n```\n~~~\n## literal ##\n~~~";
        QTextDocument document(text);
        MarkdownHighlighter highlighter(&document);
        highlighter.rehighlight();
        QVERIFY(hidden(document, 0));
        QVERIFY(formatAt(document, text.indexOf("it")).fontItalic());
        QCOMPARE(formatAt(document, text.indexOf("it")).fontWeight(), int(QFont::Bold));
        QVERIFY(formatAt(document, text.indexOf("link")).fontUnderline());
        QCOMPARE(formatAt(document, text.indexOf("link")).fontWeight(), int(QFont::Bold));
        QVERIFY(!hidden(document, text.indexOf("code")));
        QVERIFY(hidden(document, text.indexOf("###")));
        for (int blockNumber : {2, 5}) {
            const QTextBlock block = document.findBlockByNumber(blockNumber);
            for (int i = 0; i < block.text().size(); ++i)
                QVERIFY(!hidden(document, block.position() + i));
        }
        highlighter.setCaret(0);
        QVERIFY(!hidden(document, text.indexOf("###")));
        QVERIFY(hidden(document, text.indexOf('[')));
        highlighter.setSearch("###", text.indexOf("###"));
        QVERIFY(!hidden(document, text.indexOf("###")));
        highlighter.setCaret(document.characterCount() - 1);
        QVERIFY(hidden(document, text.indexOf("###")));
        QVERIFY(formatAt(document, text.indexOf("###")).background().style() != Qt::NoBrush);
    }

    void searchAcrossMarkers() {
        QTextDocument document("# title ###\nplain");
        MarkdownHighlighter highlighter(&document);
        highlighter.setCaret(document.characterCount() - 1);
        highlighter.rehighlight();
        for (const QString &query : {QStringLiteral("# title"), QStringLiteral("title ###")}) {
            highlighter.setSearch(query, -1);
            QVERIFY(hidden(document, 0));
            QVERIFY(!hidden(document, 2));
            QVERIFY(hidden(document, 8));
            highlighter.setCaret(4);
            QVERIFY(!hidden(document, 0));
            QVERIFY(!hidden(document, 8));
            highlighter.setCaret(document.characterCount() - 1);
            QVERIFY(hidden(document, 0));
            QVERIFY(!hidden(document, 2));
            QVERIFY(hidden(document, 8));
        }
    }

    void editsAndUndo() {
        const QString text = "plain\n# title ###\nend";
        QTextDocument document(text);
        document.setUndoRedoEnabled(true);
        document.clearUndoRedoStacks();
        MarkdownHighlighter highlighter(&document);
        highlighter.setCaret(document.characterCount() - 1);
        highlighter.setSelection(8, 10);
        QVERIFY(!document.isUndoAvailable());
        QTextCursor edit(&document);
        edit.insertText("prefix\n");
        highlighter.rehighlight();
        const int heading = document.toPlainText().indexOf('#');
        QVERIFY(!hidden(document, heading));
        highlighter.setSelection(0, 0);
        QVERIFY(hidden(document, heading));
        document.undo();
        highlighter.rehighlight();
        QCOMPARE(document.toPlainText(), text);
        QVERIFY(hidden(document, text.indexOf('#')));
        QVERIFY(!document.isUndoAvailable());
    }
};

QTEST_MAIN(HeadingTests)
#include "tst_headings.moc"

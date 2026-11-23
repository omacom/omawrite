#include "../../src/previewblocks.h"
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

using namespace PreviewBlocks;

class PreviewBlocksTest : public QObject {
    Q_OBJECT
private slots:
    void incrementalScannerMatchesFreshParsing() {
        QString text = "intro\n\n| A | B |\n| --- | --- |\n| x | y |\n\n![x](a.png)\n\n---\n\n```cpp\na\nb\n```\n<!--\n---\n-->\n";
        PreviewBlocks::Parser parser; parser.reset(text);
        quint32 random = 41237;
        const QStringList chunks = {"x", "\n", "\n\n", "---", "| --- | --- |", "```", "<!--", "-->", "😀", "\r\n", "![a](b.png)", ""};
        for (int n = 0; n < 600; ++n) {
            random = random * 1664525U + 1013904223U;
            const int at = int(random % (text.size() + 1));
            random = random * 1664525U + 1013904223U;
            const int removed = qMin(int(random % 9), int(text.size()) - at);
            const QString added = chunks.at((random >> 16) % chunks.size());
            text.replace(at, removed, added); parser.edit(text, at, removed, added.size());
            PreviewBlocks::Parser fresh; fresh.reset(text);
            QCOMPARE(parser.fences(), fresh.fences());
            QCOMPARE(parser.blocks().size(), fresh.blocks().size());
            for (int i = 0; i < fresh.blocks().size(); ++i) {
                const auto &a = parser.blocks()[i], &b = fresh.blocks()[i];
                QCOMPARE(a.start, b.start); QCOMPARE(a.end, b.end); QCOMPARE(a.kind, b.kind);
                QCOMPARE(a.markdown, b.markdown); QCOMPARE(a.imagePath, b.imagePath);
            }
        }
        text = "```cpp\n" + QString("int value = 1;\n").repeated(10000) + "```\n";
        parser.reset(text);
        const int at = text.indexOf("value"); text.insert(at, "x"); parser.edit(text, at, 0, 1);
        QCOMPARE(parser.parsedLines(), 1);
        QCOMPARE(parser.fences().last().second, text.size() - 1);
    }
    void horizontalRules() {
        const auto rules = parse(QStringLiteral("---\n\ntext\n\n  - - - \n\n```\n---\n```\n"));
        QCOMPARE(rules.size(), 2);
        QCOMPARE(rules[0].kind, Kind::HorizontalRule);
        QCOMPARE(rules[0].markdown, QString("---"));
        QCOMPARE(rules[1].kind, Kind::HorizontalRule);
        QVERIFY(parse(QStringLiteral("heading\n---\n")).isEmpty()); // Setext heading, not a rule.
        QVERIFY(parse(QStringLiteral("    ---\n\n--\n\n\\---\n")).isEmpty());
        QVERIFY(parse(QStringLiteral("<!--\n\n---\n\n-->\n")).isEmpty());
        QVERIFY(parse(QStringLiteral("<pre>\n\n---\n\n</pre>\n")).isEmpty());
        QCOMPARE(parse(QStringLiteral("`<!--`\n\n---\n")).size(), 1);
        QCOMPARE(parse(QStringLiteral("<!-- comment -->\n\n---\n")).size(), 1);
        QCOMPARE(parse(QStringLiteral("<div>\n\n---\n")).size(), 1);
        QVERIFY(parse(QStringLiteral("<pre>\n<img/>\n\n---\n</pre>\n")).isEmpty());
        const auto image = renderHorizontalRule(QFont(), Qt::black, 400);
        QCOMPARE(image.width(), 400);
        QVERIFY(image.height() >= 12);
        QVERIFY(image.pixelColor(0, image.height() / 2).alpha() > 0);
        QVERIFY(image.pixelColor(399, image.height() / 2).alpha() > 0);
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    }
    void immediateConvergenceKeepsPreviousLineSensitiveRulesFresh() {
        for (const QString &rule : {QString("---"), QString("  - - - ")}) {
            QString text = "before\n\n" + rule + "\nafter";
            Parser parser; parser.reset(text); QCOMPARE(parser.blocks().size(), 1);
            text.insert(7, 'x'); parser.edit(text, 7, 0, 1);
            QCOMPARE(parser.blocks().size(), parse(text).size()); QVERIFY(parser.blocks().isEmpty());
            text.remove(7, 1); parser.edit(text, 7, 1, 0);
            QCOMPARE(parser.blocks().size(), parse(text).size()); QCOMPARE(parser.blocks().size(), 1);
        }
        QString text = "before\n| A | B |\n| --- | --- |\n" + QString("| a | b |\n").repeated(5000);
        Parser parser; parser.reset(text); text.insert(6, 'x'); parser.edit(text, 6, 0, 1);
        QVERIFY(parser.parsedLines() < 10); QCOMPARE(parser.blocks().size(), parse(text).size());
    }
    void tables() {
        const QString source = QString::fromUtf8("😀 before\r\n")
            + QStringLiteral("| A \\| B | `x|y` |\r\n| :--- | ---: |\r\n| c | d |\r\nafter\n");
        const auto blocks = parse(source);
        QCOMPARE(blocks.size(), 1);
        QCOMPARE(blocks[0].kind, Kind::Table);
        QCOMPARE(blocks[0].start, 11);
        QCOMPARE(source.mid(blocks[0].end, 2), QStringLiteral("\r\n"));
        QCOMPARE(blocks[0].markdown, source.mid(blocks[0].start, blocks[0].end - blocks[0].start));
        QVERIFY(parse(QStringLiteral("a | b\n-- | ---\nx | y")).isEmpty());
        QVERIFY(parse(QStringLiteral("a | b\n--- | --- | ---")).isEmpty());
        QVERIFY(parse(QStringLiteral("    a | b\n    --- | ---")).isEmpty());
        QVERIFY(parse(QStringLiteral("# a | b\n--- | ---")).isEmpty());
        QVERIFY(parse(QStringLiteral("> a | b\n--- | ---")).isEmpty());
        QVERIFY(parse(QStringLiteral("- a | b\n--- | ---")).isEmpty());
        const auto strict = parse(QStringLiteral("a | b\n--- | ---\nx | y | z\n"));
        QCOMPARE(strict.size(), 1);
        QCOMPARE(strict[0].markdown, QStringLiteral("a | b\n--- | ---"));
        QCOMPARE(parse(QStringLiteral("| a |\n| --- |\n| b |\n")).size(), 1);
    }

    void fences() {
        const QString source = QStringLiteral("````text\n![a](hidden.png)\na|b\n---|---\n```\n````\n"
                                              "~~~mermaid\ngraph TD; A-->B\n~~~\n![x](ok.png)\n");
        const auto blocks = parse(source);
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].kind, Kind::Mermaid);
        QCOMPARE(blocks[0].markdown, QStringLiteral("~~~mermaid\ngraph TD; A-->B\n~~~"));
        QCOMPARE(blocks[1].imagePath, QStringLiteral("ok.png"));
        const auto unclosed = parse(QStringLiteral("```mermaid\ngraph TD\n"));
        QCOMPARE(unclosed.size(), 1);
        QCOMPARE(unclosed[0].markdown, QStringLiteral("```mermaid\ngraph TD"));
        QVERIFY(parse(QStringLiteral("```\n![a](hidden.png)\n")).isEmpty());
    }

    void images() {
        const auto images = parse(QStringLiteral("![alt](<a b.png> \"title\")\n"
                                                "  ![a](a\\(b\\).png 'title')  \n"
                                                "![a](a(b).png (title))\n"
                                                "![a](https://example.com/x.png)\n"));
        QCOMPARE(images.size(), 4);
        QCOMPARE(images[0].imagePath, QStringLiteral("a b.png"));
        QCOMPARE(images[1].imagePath, QStringLiteral("a(b).png"));
        QCOMPARE(images[2].imagePath, QStringLiteral("a(b).png"));
        QVERIFY(parse(QStringLiteral("before ![x](a.png)\n![x](a b.png)\n![x](<a.png)\n![x](a.png) after\n")).isEmpty());
        QVERIFY(parse(QStringLiteral("    ![x](a.png)\n")).isEmpty());
    }

    void rendering() {
        const QColor background(20, 30, 40);
        const auto image = renderTable(QStringLiteral("| a | b |\n| --- | --- |\n| c | d |"),
                                       QFont(), Qt::white, background, 240);
        QVERIFY(!image.isNull());
        QVERIFY(image.width() <= 240);
        QCOMPARE(image.pixelColor(0, 0), background);
        QVERIFY(renderTable(QStringLiteral("a"), QFont(), Qt::white, Qt::black, 0).isNull());
    }

    void localImages() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("a b.png"));
        QImage image(500, 2500, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(path));
        const auto loaded = loadImage(QUrl::fromLocalFile(path), 1000);
        QVERIFY(!loaded.isNull());
        QCOMPARE(loaded.height(), 1800);
        QVERIFY(loaded.width() <= 1000);
        QCOMPARE(loadImage(QUrl::fromLocalFile(path), 100).width(), 100);
        QVERIFY(loadImage(QUrl(QStringLiteral("https://example.com/a.png")), 100).isNull());
        QVERIFY(loadImage(QUrl(QStringLiteral("data:image/png;base64,AAAA")), 100).isNull());
        QVERIFY(loadImage(QUrl::fromLocalFile(path), 0).isNull());
        QFile large(directory.filePath(QStringLiteral("large.png")));
        QVERIFY(large.open(QIODevice::WriteOnly));
        QVERIFY(large.resize(20 * 1024 * 1024 + 1));
        large.close();
        QVERIFY(loadImage(QUrl::fromLocalFile(large.fileName()), 100).isNull());
        // A tiny BMP header advertising a decoded image above the pixel budget.
        QByteArray bmp(54, '\0');
        bmp[0] = 'B'; bmp[1] = 'M'; bmp[10] = 54; bmp[14] = 40;
        auto le32 = [&](int at, quint32 value) {
            for (int i = 0; i < 4; ++i)
                bmp[at + i] = char(value >> (8 * i));
        };
        le32(18, 6000); le32(22, 5000); bmp[26] = 1; bmp[28] = 24;
        QFile huge(directory.filePath(QStringLiteral("huge.bmp")));
        QVERIFY(huge.open(QIODevice::WriteOnly));
        QCOMPARE(huge.write(bmp), qint64(bmp.size()));
        huge.close();
        QVERIFY(loadImage(QUrl::fromLocalFile(huge.fileName()), 100).isNull());
    }
};

QTEST_MAIN(PreviewBlocksTest)
#include "tst_previewblocks.moc"

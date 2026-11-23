#include <QtTest>
#include "printdocument.h"
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QPainter>
#include <QPageRanges>
#include <QPrinter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QTextTable>

namespace {
Printing::Options options() {
    Printing::Options o;
    o.pageLayout = QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait, QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);
    o.font = QFont(QStringLiteral("Arial"), 11);
    return o;
}
QString table(int rows, int cols) {
    QString text;
    for (int r = -1; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) text += QStringLiteral("| %1 ").arg(r < 0 ? QStringLiteral("Header%1").arg(c) : QStringLiteral("R%1C%2").arg(r).arg(c));
        text += QStringLiteral("|\n");
        if (r == -1) { for (int c = 0; c < cols; ++c) text += QStringLiteral("| --- "); text += QStringLiteral("|\n"); }
    }
    return text;
}
QList<QTextTable *> tables(QTextDocument &doc) {
    QList<QTextTable *> result;
    for (auto *frame : doc.rootFrame()->childFrames()) if (auto *t = qobject_cast<QTextTable *>(frame)) result.append(t);
    return result;
}
QImage raster(const Printing::Section &section, int page = 0) {
    const auto size = section.document->pageSize().toSize();
    QImage image(size, QImage::Format_RGB32); image.fill(Qt::white);
    QPainter painter(&image); section.drawPage(painter, page); return image;
}
int redPixels(const QImage &image) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        QColor c(image.pixel(x, y)); if (c.red() > 180 && c.green() < 80 && c.blue() < 80) ++count;
    }
    return count;
}
}

class PrintingTest : public QObject {
    Q_OBJECT
private slots:
    void standardLineBreaksMatchReadMode() {
        const QString markdown = "soft\nwords\n\nhard\\\nbreak\n\n> quoted\n> words\n>\n> line  \n> break";
        auto job = Printing::build(markdown, options(), {});
        QVERIFY2(job.error.isEmpty(), qPrintable(job.error));
        QCOMPARE(job.sections.size(), size_t(1));
        const QString paragraph(QChar::ParagraphSeparator), hard(QChar::LineSeparator);
        QCOMPARE(job.sections.front().document->toRawText(),
                 "soft words" + paragraph + "hard" + hard + "break" + paragraph
                 + "quoted words" + paragraph + "line" + hard + "break");
        QCOMPARE(job.sections.front().document->blockCount(), 4);
    }
    void geometryUsesLayoutUnitsRatherThanPlatformFormatUnits_data() {
        QTest::addColumn<QMarginsF>("margins");
        QTest::newRow("default") << QMarginsF(15, 15, 15, 15);
        QTest::newRow("zero") << QMarginsF(0, 0, 0, 0);
        QTest::newRow("fractional-asymmetric") << QMarginsF(12.25, 16.5, 21.75, 11.25);
    }
    void geometryUsesLayoutUnitsRatherThanPlatformFormatUnits() {
        QFETCH(QMarginsF, margins);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("original.png"));
        QImage original(1600, 800, QImage::Format_RGB32); original.fill(Qt::red); QVERIFY(original.save(path));
        auto o = options(); o.pageLayout.setMargins(margins);
        const QString source = QStringLiteral("![body](image.png)\n\n| Image | Text |\n| --- | --- |\n| ![cell](image.png) | **Bold** and `code` |\n\n") + table(10, 28);
        auto job = Printing::build(source, o, [&](const QString &) { return QUrl::fromLocalFile(path); });
        QVERIFY2(job.error.isEmpty(), qPrintable(job.error)); QVERIFY(job.sections.size() > 1);
        const QImage baseline(1, 1, QImage::Format_ARGB32);
        int imageCount = 0;
        for (const auto &section : job.sections) {
            auto &doc = *section.document;
            const qreal scale = qreal(section.metrics->logicalDpiY()) / baseline.logicalDpiY();
            for (auto *t : tables(doc)) {
                const auto bounds = doc.documentLayout()->frameBoundingRect(t);
                QVERIFY(bounds.left() >= -2); QVERIFY(bounds.right() <= doc.pageSize().width() + 2);
                QVERIFY(qAbs(t->format().width().rawValue() * scale - (doc.pageSize().width() - 10)) < 1);
            }
            for (auto block = doc.begin(); block.isValid(); block = block.next()) for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (!fragment.charFormat().isImageFormat()) continue;
                ++imageCount;
                const auto format = fragment.charFormat().toImageFormat();
                QCOMPARE(qvariant_cast<QImage>(doc.resource(QTextDocument::ImageResource, QUrl(format.name()))).size(), original.size());
                QTextCursor cursor(&doc); cursor.setPosition(fragment.position());
                qreal available = doc.pageSize().width();
                if (auto *t = cursor.currentTable()) {
                    const auto cell = t->cellAt(cursor);
                    const auto columns = t->format().columnWidthConstraints(); available = 0;
                    for (int c = cell.column(); c < cell.column() + cell.columnSpan(); ++c) available += columns[c].rawValue() * scale;
                    available -= 10;
                }
                const int position = fragment.position() - block.position();
                const auto line = block.layout()->lineForTextPosition(position); QVERIFY(line.isValid());
                const qreal advance = qAbs(line.cursorToX(position + 1) - line.cursorToX(position));
                QVERIFY(advance > 20); QVERIFY(advance <= available + 2);
                QVERIFY(qAbs(advance - format.width() * scale) < 2);
            }
        }
        QCOMPARE(imageCount, 2);
    }
    void originalsResolveWithoutSavingAndMissingResourcesAreExplicit() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("staged.png"));
        QImage original(1600, 800, QImage::Format_RGB32); original.fill(Qt::red); QVERIFY(original.save(path));
        QStringList requested;
        auto job = Printing::build(QStringLiteral("Before ![shot](img/logical.png) after\n\n![missing](https://example.invalid/pixel.png)"), options(), [&](const QString &name) {
            requested.append(name); return name == QStringLiteral("img/logical.png") ? QUrl::fromLocalFile(path) : QUrl();
        });
        QVERIFY2(job.error.isEmpty(), qPrintable(job.error)); QCOMPARE(job.sections.size(), size_t(1));
        QCOMPARE(requested.size(), 2); QCOMPARE(job.warnings.size(), 1);
        auto &doc = *job.sections.front().document;
        QVERIFY(doc.toPlainText().contains(QStringLiteral("[image unavailable]")));
        int images = 0;
        for (auto block = doc.begin(); block.isValid(); block = block.next()) for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto f = it.fragment().charFormat(); if (!f.isImageFormat()) continue;
            auto image = f.toImageFormat(); ++images;
            QCOMPARE(qvariant_cast<QImage>(doc.resource(QTextDocument::ImageResource, QUrl(image.name()))).size(), original.size());
            const QImage baseline(1, 1, QImage::Format_ARGB32);
            const qreal scale = qreal(doc.documentLayout()->paintDevice()->logicalDpiY()) / baseline.logicalDpiY();
            QVERIFY(image.width() * scale <= doc.pageSize().width() + 1);
            QCOMPARE(image.width() / image.height(), 2.0);
        }
        QCOMPARE(images, 1); QVERIFY(redPixels(raster(job.sections.front())) > 1000);
        QVERIFY(QFileInfo::exists(path));
    }

    void longNarrowTablesStayInlineAndHaveRepeatableHeaders() {
        auto job = Printing::build(QStringLiteral("Introduction\n\n") + table(150, 3) + QStringLiteral("\nConclusion"), options(), {});
        QVERIFY2(job.error.isEmpty(), qPrintable(job.error)); QCOMPARE(job.sections.size(), size_t(1));
        auto &section = job.sections.front(); auto native = tables(*section.document);
        QCOMPARE(native.size(), 1); QCOMPARE(native.front()->format().headerRowCount(), 1);
        QVERIFY(section.document->pageCount() > 1);
        const QRectF bounds = section.document->documentLayout()->frameBoundingRect(native.front());
        QVERIFY2(bounds.right() <= section.document->pageSize().width() + 2, qPrintable(QString::number(bounds.right()) + " vs " + QString::number(section.document->pageSize().width())));
        QVERIFY(!section.document->toPlainText().contains(QStringLiteral("Appendix")));
        QVERIFY(section.document->toPlainText().contains(QStringLiteral("R149C2")));
        auto continuation = raster(section, 1);
        int headerPixels = 0;
        for (int y = 0; y < qMin(60, continuation.height()); ++y) for (int x = 0; x < continuation.width(); ++x) {
            QColor c(continuation.pixel(x,y));
            if (c.red() >= 225 && c.red() <= 240 && c.red() == c.green() && c.red() == c.blue()) ++headerPixels;
        }
        QVERIFY2(headerPixels > 1000, qPrintable(QStringLiteral("Continuation header pixels: %1").arg(headerPixels)));
    }

    void wideTablesBecomeLandscapeBandsWithoutLostCells() {
        const QString source = QStringLiteral("Intro\n\n") + table(4, 28) + QStringLiteral("\nBetween\n\n") + table(2, 20) + QStringLiteral("\nEnd");
        auto job = Printing::build(source, options(), {});
        QVERIFY2(job.error.isEmpty(), qPrintable(job.error)); QVERIFY(job.sections.size() > 3);
        auto &body = *job.sections.front().document;
        QCOMPARE(tables(body).size(), 0);
        QVERIFY(body.toPlainText().contains(QStringLiteral("See Appendix A — Table 1.")));
        QVERIFY(body.toPlainText().contains(QStringLiteral("See Appendix B — Table 2.")));
        QVERIFY(body.toPlainText().contains(QStringLiteral("Between"))); QVERIFY(body.toPlainText().contains(QStringLiteral("End")));
        QString appendices;
        for (size_t i = 1; i < job.sections.size(); ++i) {
            auto &section = job.sections[i]; QCOMPARE(section.layout.orientation(), QPageLayout::Landscape);
            auto native = tables(*section.document); QCOMPARE(native.size(), 1);
            QCOMPARE(native.front()->format().headerRowCount(), 1);
            const auto bounds = section.document->documentLayout()->frameBoundingRect(native.front());
            QVERIFY2(bounds.right() <= section.document->pageSize().width() + 2, qPrintable(QString::number(bounds.right())));
            appendices += section.document->toPlainText();
            QVERIFY(section.document->toPlainText().contains(QStringLiteral("R0C0")));
        }
        for (int c = 0; c < 28; ++c) QVERIFY(appendices.contains(QStringLiteral("R3C%1").arg(c)));
        for (int c = 0; c < 20; ++c) QVERIFY(appendices.contains(QStringLiteral("R1C%1").arg(c)));
        auto inlineOptions = options(); inlineOptions.wideTablesToAppendix = false;
        auto tooDense = Printing::build(table(2, 150), inlineOptions, {});
        QVERIFY2(!tooDense.error.isEmpty(), "An unprintable dense inline table must fail explicitly, not clip cells.");
        auto inlineJob = Printing::build(source, inlineOptions, {});
        QVERIFY2(inlineJob.error.isEmpty(), qPrintable(inlineJob.error));
        QCOMPARE(inlineJob.sections.size(), size_t(1)); QCOMPARE(tables(*inlineJob.sections.front().document).size(), 2);
    }

    void sparseNativeMergedCellsStillMoveToAppendicesWithoutDuplicatingText() {
        QString source;
        for (int c = 0; c < 29; ++c) source += QStringLiteral("| Header%1 ").arg(c);
        source += "|\n";
        for (int c = 0; c < 29; ++c) source += "| --- ";
        source += "|\n";
        for (int r = 0; r < 8; ++r) source += QStringLiteral("| CONTEXT_%1 | UNIQUE_VALUE_%1 |\n").arg(r);
        QTextDocument imported; imported.setMarkdown(source, QTextDocument::MarkdownDialectGitHub);
        const auto original = tables(imported); QCOMPARE(original.size(), 1);
        bool merged = false;
        for (int r=0; r<original[0]->rows(); ++r) for (int c=0; c<original[0]->columns(); ++c)
            merged |= original[0]->cellAt(r,c).columnSpan() > 1;
        QVERIFY2(merged, "Fixture must exercise Qt's ragged-row native merged cells");
        auto job = Printing::build(source, options(), {});
        QVERIFY2(job.error.isEmpty(), qPrintable(job.error)); QVERIFY(job.sections.size() > 1);
        QVERIFY(tables(*job.sections.front().document).isEmpty());
        QString joined;
        for (size_t i=1; i<job.sections.size(); ++i) {
            joined += job.sections[i].document->toPlainText();
            for (auto *native : tables(*job.sections[i].document)) {
                QCOMPARE(native->rows(), 9);
                QVERIFY(native->columns() < 29);
            }
        }
        for (int r=0; r<8; ++r) {
            QCOMPARE(joined.count(QStringLiteral("UNIQUE_VALUE_%1").arg(r)), 1);
            QCOMPARE(joined.count(QStringLiteral("CONTEXT_%1").arg(r)), int(job.sections.size()-1));
        }
        for (int c=1; c<29; ++c) {
            QRegularExpression exact(QStringLiteral("\\bHeader%1\\b").arg(c));
            auto matches = exact.globalMatch(joined); int count=0; while(matches.hasNext()) {matches.next(); ++count;}
            QCOMPARE(count, 1);
        }
    }

    void pdfUsesMixedPagesOriginalImagesAndGlobalRanges() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QImage image(100, 50, QImage::Format_RGB32); image.fill(Qt::red);
        const auto path = dir.filePath(QStringLiteral("red.png")); QVERIFY(image.save(path));
        auto job = Printing::build(QStringLiteral("![red](red.png)\n\n") + table(4, 28), options(), [&](const QString &) { return QUrl::fromLocalFile(path); });
        QVERIFY(job.error.isEmpty());
        QPrinter printer(QPrinter::HighResolution); printer.setOutputFormat(QPrinter::PdfFormat);
        const auto pdfPath = dir.filePath(QStringLiteral("test.pdf")); printer.setOutputFileName(pdfPath);
        QString error; QVERIFY2(job.print(printer, &error), qPrintable(error));
        QFile pdf(pdfPath); QVERIFY(pdf.open(QIODevice::ReadOnly)); auto bytes = pdf.readAll();
        QVERIFY(bytes.startsWith("%PDF")); QVERIFY(bytes.contains("/Subtype /Image"));
        QCOMPARE(bytes.count("/Type /Page\n"), job.pageCount());
        QVERIFY(bytes.contains("/MediaBox [0 0 595.000000 842.000000]"));
        QVERIFY(bytes.contains("/MediaBox [0 0 842.000000 595.000000]"));
        const auto exportPath = qEnvironmentVariable("OMAWRITE_PRINT_TEST_PDF");
        if (!exportPath.isEmpty()) QVERIFY(QFile::copy(pdfPath, exportPath));
        // Explicitly opt in to the native macOS print-to-file engine; never
        // address a physical printer or show a system dialog in this fixture.
        if (QGuiApplication::platformName() == QStringLiteral("cocoa") && qEnvironmentVariableIsSet("OMAWRITE_NATIVE_PRINT_TEST")) {
            QPrinter native(QPrinter::HighResolution);
            native.setOutputFileName(dir.filePath("native.pdf"));
            native.setOutputFormat(QPrinter::NativeFormat);
            if (native.outputFormat() != QPrinter::NativeFormat)
                QSKIP("No native printer engine available; Qt fell back to PDF. Native-driver behavior remains unverified.");
            QString nativeError;
            QVERIFY2(job.print(native, &nativeError), qPrintable(nativeError));
            QFile nativeFile(native.outputFileName()); QVERIFY(nativeFile.open(QIODevice::ReadOnly));
            QVERIFY(nativeFile.readAll().startsWith("%PDF"));
            if (!exportPath.isEmpty()) QVERIFY(QFile::copy(native.outputFileName(), exportPath + ".native.pdf"));
        }
        QPrinter range(QPrinter::HighResolution); range.setOutputFormat(QPrinter::PdfFormat);
        range.setOutputFileName(dir.filePath(QStringLiteral("range.pdf"))); range.setFromTo(2, 2); range.setPrintRange(QPrinter::PageRange);
        QVERIFY2(job.print(range, &error), qPrintable(error));
        QFile ranged(range.outputFileName()); QVERIFY(ranged.open(QIODevice::ReadOnly)); QCOMPARE(ranged.readAll().count("/Type /Page\n"), 1);
        QPageRanges selected; selected.addPage(1); selected.addPage(3);
        range.setPageRanges(selected); range.setPageOrder(QPrinter::LastPageFirst);
        range.setOutputFileName(dir.filePath("disjoint.pdf"));
        QVERIFY2(job.print(range, &error), qPrintable(error));
        QFile disjoint(range.outputFileName()); QVERIFY(disjoint.open(QIODevice::ReadOnly)); QCOMPARE(disjoint.readAll().count("/Type /Page\n"), 2);
        range.setFromTo(1999, 2000); range.setOutputFileName(dir.filePath("empty.pdf"));
        QVERIFY(!job.print(range, &error)); QVERIFY(!error.isEmpty());
        if (!range.supportsMultipleCopies()) {
            for (bool collated : {false, true}) {
                range.setPrintRange(QPrinter::AllPages); range.setCopyCount(2); range.setCollateCopies(collated);
                range.setOutputFileName(dir.filePath(collated ? "collated.pdf" : "copies.pdf"));
                QVERIFY2(job.print(range, &error), qPrintable(error));
                QFile copies(range.outputFileName()); QVERIFY(copies.open(QIODevice::ReadOnly));
                QCOMPARE(copies.readAll().count("/Type /Page\n"), job.pageCount() * 2);
            }
        }
    }

    void structuralPreflightRunsBeforeNativeAllocationAndHtmlStaysLiteral() {
        QString ragged;
        for (int c = 0; c < 1000; ++c) ragged += "| Header ";
        ragged += "|\n";
        for (int c = 0; c < 1000; ++c) ragged += "| --- ";
        ragged += "|\n";
        for (int r = 0; r < 150; ++r) ragged += "one cell\n";
        auto rejected = Printing::build(ragged, options(), {});
        QVERIFY(!rejected.error.isEmpty()); QVERIFY(rejected.sections.empty());
        const QString narrowFirst = QStringLiteral("| narrow |\n| --- |\n| value |\n# Separator\n") + ragged;
        auto consecutive = Printing::build(narrowFirst, options(), {});
        QVERIFY(!consecutive.error.isEmpty()); QVERIFY(consecutive.sections.empty());
        auto quoted = Printing::build("> " + ragged.split('\n').join("\n> "), options(), {});
        QVERIFY(!quoted.error.isEmpty()); QVERIFY(quoted.sections.empty());
        auto literal = Printing::build(QStringLiteral("<table><tr><td colspan=\"999999999\">content</td></tr></table>"), options(), {});
        QVERIFY(literal.error.isEmpty()); QCOMPARE(tables(*literal.sections.front().document).size(), 0);
        QVERIFY(literal.sections.front().document->toPlainText().contains(QStringLiteral("content")));
    }

    void imagesInAppendicesAndOversizedRowsStayPrintable() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QImage image(800, 400, QImage::Format_RGB32); image.fill(Qt::red);
        auto path = dir.filePath("image.png"); QVERIFY(image.save(path));
        QString source = table(3, 28);
        source.replace(QStringLiteral("R0C1"), QStringLiteral("![image](image.png)"));
        auto job = Printing::build(source, options(), [&](const QString &) { return QUrl::fromLocalFile(path); });
        QVERIFY(job.error.isEmpty());
        int pixels = 0;
        for (size_t i = 1; i < job.sections.size(); ++i) pixels += redPixels(raster(job.sections[i]));
        QVERIFY(pixels > 100);
        QString tall = "| Header | Value |\n| --- | --- |\n| first | ";
        for (int i = 0; i < 2500; ++i) tall += "word ";
        tall += "THE_END |\n| last | AFTER_TALL_ROW |\n";
        auto longRow = Printing::build(tall, options(), {});
        QVERIFY(longRow.error.isEmpty()); QCOMPARE(longRow.sections.size(), size_t(1));
        auto &section = longRow.sections.front(); QVERIFY(section.document->pageCount() > 1);
        auto native = tables(*section.document); QCOMPARE(native.size(), 1);
        auto end = section.document->find("AFTER_TALL_ROW"); QVERIFY(!end.isNull());
        auto blockRect = section.document->documentLayout()->blockBoundingRect(end.block());
        QVERIFY(blockRect.right() <= section.document->pageSize().width() + 2);
        QVERIFY(blockRect.bottom() <= section.document->pageCount() * section.document->pageSize().height());
        QVERIFY(!raster(section, section.document->pageCount()-1).isNull());
    }

    void resourceFallbackNeverCallsGlobalProvider() {
        int calls = 0;
        auto previous = QTextDocument::defaultResourceProvider();
        QTextDocument::setDefaultResourceProvider([&](const QUrl &) { ++calls; return QVariant(); });
        auto job = Printing::build(QStringLiteral("![remote](https://example.invalid/a.png)"), options(), {});
        QTextDocument::setDefaultResourceProvider(previous);
        QCOMPARE(calls, 0); QVERIFY(job.error.isEmpty());
        QVERIFY(job.sections.front().document->toPlainText().contains(QStringLiteral("[image unavailable]")));
    }
};
// QTEST_MAIN forces AA_Use96Dpi, masking the real Cocoa 72-vs-96 DPI bug.
// Keep the platform's actual defaults, just like the production application.
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    PrintingTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_printing.moc"

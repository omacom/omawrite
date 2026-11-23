#include "printdocument.h"
#include "attachmenthelper.h"
#include "listpreview.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QPageRanges>
#include <QPrinter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTextImageFormat>
#include <QTextTable>
#include <algorithm>
#include <numeric>

namespace Printing {
namespace {
constexpr qsizetype MaxSource = 8 * 1024 * 1024;
constexpr qsizetype MaxImageBytes = 64 * 1024 * 1024;
constexpr int MaxImages = 128, MaxCells = 100000, MaxPages = 2000;
constexpr qreal Padding = 4, LayoutDpi = 96;
QSizeF contentSize(const QPageLayout &layout) { return layout.paintRectPoints().size() * (LayoutDpi / 72.0); }

// Bound ragged table matrices BEFORE Qt creates/pads native cells. This is
// deliberately conservative: a table runs until a blank line, even where a
// parser might terminate sooner. Raw HTML is imported as text, not structures.
bool preflight(const QString &source) {
    qint64 total = 0, columns = 0, rows = 0, previousColumns = 0;
    int lines = 0;
    const auto finish = [&] { total += columns * rows; columns = rows = 0; };
    qsizetype start = 0;
    while (start <= source.size()) {
        if (++lines > 50000) return false;
        auto end = source.indexOf(QLatin1Char('\n'), start);
        if (end < 0) end = source.size();
        QStringView line = QStringView(source).mid(start, end - start).trimmed();
        while (line.startsWith(QLatin1Char('>'))) line = line.mid(1).trimmed();
        const qint64 pipes = line.count(QLatin1Char('|'));
        const qint64 count = pipes ? pipes + 1 - line.startsWith(QLatin1Char('|')) - line.endsWith(QLatin1Char('|')) : 0;
        bool delimiter = pipes && line.contains(QLatin1Char('-'));
        if (delimiter) for (QChar c : line) if (c != '|' && c != '-' && c != ':' && !c.isSpace()) { delimiter = false; break; }
        if (line.isEmpty()) finish();
        else if (columns) { columns = qMax(columns, count); ++rows; }
        else if (delimiter && previousColumns) { columns = qMax(previousColumns, count); rows = 1; }
        if (columns * rows + total > MaxCells || count > 1024) return false;
        previousColumns = line.isEmpty() ? 0 : count;
        if (end == source.size()) break;
        start = end + 1;
    }
    return true;
}

class Document final : public QTextDocument {
public:
    Document() { setResourceProvider([](const QUrl &) { return QVariant(); }); }
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

Section section(const QPageLayout &layout, const QFont &font, const QString &title = {}) {
    Section result;
    result.layout = layout;
    result.title = title;
    result.metrics = std::make_unique<QImage>(1, 1, QImage::Format_ARGB32_Premultiplied);
    result.metrics->setDotsPerMeterX(qRound(96 / 0.0254));
    result.metrics->setDotsPerMeterY(qRound(96 / 0.0254));
    result.document = std::make_unique<Document>();
    auto &doc = *result.document;
    doc.documentLayout()->setPaintDevice(result.metrics.get());
    doc.setLayoutEnabled(false);
    doc.setUndoRedoEnabled(false);
    doc.setDefaultFont(font);
    doc.setDocumentMargin(0);
    doc.setPageSize(contentSize(layout));
    auto option = doc.defaultTextOption();
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc.setDefaultTextOption(option);
    return result;
}

struct Table {
    QTextTable *original = nullptr;
    int number = 0;
    QString appendix;
    int rows = 0, columns = 0;
    QVector<qreal> minimum, preferred;
    QVector<QTextDocumentFragment> cells;
    QVector<QTextBlockFormat> firstBlock;
    QVector<int> units;
};

QString letters(int index) {
    QString result;
    do { result.prepend(QChar('A' + index % 26)); index = index / 26 - 1; } while (index >= 0);
    return result;
}

QFont tableFont(const QFont &body) {
    QFont font = body;
    font.setPointSizeF(9.5);
    return font;
}

// QTextDocumentLayout scales format lengths by device DPI / Qt's default
// DPI. A default image exposes that baseline through public API (including
// AA_Use96Dpi). Convert only format geometry, not layout coordinates or points.
qreal formatUnits(const QTextDocument &doc) {
    const QImage baseline(1, 1, QImage::Format_ARGB32);
    const auto *device = doc.documentLayout()->paintDevice();
    return device ? qreal(baseline.logicalDpiY()) / device->logicalDpiY() : 1;
}

qreal tableContentWidth(qreal available) {
    return qMax(qreal(1), available - 2 * (Padding + 1));
}

QVector<qreal> widths(const Table &table, const QVector<int> &columns, qreal available) {
    // Reserve collapsed outside padding/borders consistently with band packing.
    available = tableContentWidth(available);
    QVector<qreal> result;
    qreal minimum = 0, preferred = 0;
    for (int c : columns) { result.append(table.minimum[c]); minimum += table.minimum[c]; preferred += table.preferred[c]; }
    const qreal spare = qMax(qreal(0), available - minimum);
    for (int i = 0; i < columns.size(); ++i) {
        if (minimum > available) result[i] *= available / minimum;
        else result[i] += spare * (preferred > minimum ? (table.preferred[columns[i]] - result[i]) / (preferred - minimum) : 1.0 / columns.size());
    }
    return result;
}

void formatTable(QTextTable *table, const QVector<qreal> &columnWidths, const QFont &body) {
    const qreal units = formatUnits(*table->document());
    auto format = table->format();
    format.setWidth(QTextLength(QTextLength::FixedLength,
                              units * std::accumulate(columnWidths.begin(), columnWidths.end(), qreal(0))));
    QList<QTextLength> constraints;
    for (qreal w : columnWidths) constraints.append(QTextLength(QTextLength::FixedLength, w * units));
    format.setColumnWidthConstraints(constraints);
    format.setHeaderRowCount(1);
    format.setPadding(0);
    format.setCellPadding(Padding * units);
    format.setCellSpacing(0);
    format.setBorderCollapse(true);
    format.setBorder(0.5 * units);
    format.setBorderBrush(QColor(120, 120, 120));
    format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    format.setLeftMargin(0); format.setRightMargin(0);
    format.setTopMargin(8 * units); format.setBottomMargin(8 * units);
    table->setFormat(format);
    const QFont font = tableFont(body);
    for (int r = 0; r < table->rows(); ++r) for (int c = 0; c < table->columns(); ++c) {
        auto cell = table->cellAt(r, c);
        if (cell.row() != r || cell.column() != c) continue;
        auto cellFormat = cell.format().toTableCellFormat();
        cellFormat.setBackground(r == 0 ? QColor(235, 235, 235) : QColor(Qt::white));
        cellFormat.setBorder(0.5 * units); cellFormat.setBorderBrush(QColor(160, 160, 160));
        cellFormat.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        cell.setFormat(cellFormat);
        QTextCursor cursor = cell.firstCursorPosition();
        const int end = cell.lastCursorPosition().position();
        for (auto block = cursor.block(); block.isValid() && block.position() <= end; block = block.next()) {
            auto blockFormat = block.blockFormat();
            blockFormat.setNonBreakableLines(false);
            blockFormat.setTopMargin(0); blockFormat.setBottomMargin(0);
            QTextCursor(block).setBlockFormat(blockFormat);
        }
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QTextCharFormat text;
        text.setFontPointSize(font.pointSizeF());
        text.setForeground(Qt::black); text.setBackground(Qt::transparent);
        if (r == 0) text.setFontWeight(QFont::Bold);
        cursor.mergeCharFormat(text);
    }
}

void constrainImages(QTextDocument &doc, const QMap<QString, QImage> &resources, qreal width, qreal height) {
    const qreal units = formatUnits(doc);
    struct Image { int start, length; QTextImageFormat format; };
    QList<Image> images;
    for (auto block = doc.begin(); block.isValid(); block = block.next()) for (auto it = block.begin(); !it.atEnd(); ++it) {
        auto fragment = it.fragment();
        if (fragment.charFormat().isImageFormat()) images.append({fragment.position(), fragment.length(), fragment.charFormat().toImageFormat()});
    }
    for (const auto &image : images) {
        auto format = image.format;
        const auto source = resources.value(format.name());
        if (source.isNull()) continue;
        qreal available = width;
        QTextCursor cursor(&doc); cursor.setPosition(image.start);
        if (auto *table = cursor.currentTable()) {
            auto cell = table->cellAt(cursor);
            const auto columns = table->format().columnWidthConstraints();
            available = 0;
            for (int c = cell.column(); c < cell.column() + cell.columnSpan() && c < columns.size(); ++c)
                available += columns[c].rawValue() / units;
            available = qMax(qreal(1), available - 2 * Padding - 2);
        }
        QSizeF size = source.size();
        if (format.width() > 0) size = QSizeF(format.width(), format.width() * source.height() / source.width());
        if (format.height() > 0 && format.width() <= 0) size = QSizeF(format.height() * source.width() / source.height(), format.height());
        size.scale(QSizeF(available, qMax(qreal(1), height - 32)), Qt::KeepAspectRatio);
        // QSizeF::scale can enlarge; never upscale beyond the requested/natural size.
        qreal naturalWidth = format.width() > 0 ? format.width() : source.width();
        if (format.height() > 0 && format.width() <= 0) naturalWidth = format.height() * source.width() / source.height();
        if (size.width() > naturalWidth) size *= naturalWidth / size.width();
        format.setWidth(size.width() * units); format.setHeight(size.height() * units);
        cursor.setPosition(image.start + image.length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(format);
    }
    for (auto it = resources.cbegin(); it != resources.cend(); ++it)
        doc.addResource(QTextDocument::ImageResource, QUrl(it.key()), it.value());
}
bool tablesFit(QTextFrame *frame, const QTextDocument &doc) {
    for (auto *child : frame->childFrames()) {
        if (qobject_cast<QTextTable *>(child)) {
            const QRectF bounds = doc.documentLayout()->frameBoundingRect(child);
            if (bounds.left() < -2 || bounds.right() > doc.pageSize().width() + 2) return false;
        }
        if (!tablesFit(child, doc)) return false;
    }
    return true;
}
} // namespace

Job build(const QString &markdown, const Options &options, const ImageResolver &resolve) {
    Job job;
    if (markdown.size() > MaxSource || !preflight(markdown) || !options.pageLayout.isValid()
        || options.pageLayout.paintRectPoints().width() < 72 || options.pageLayout.paintRectPoints().height() < 72) {
        job.error = QStringLiteral("Document or printable page exceeds print limits."); return job;
    }
    QFont font = options.font;
    if (font.pointSizeF() <= 0) font.setPointSizeF(11);
    job.sections.push_back(section(options.pageLayout, font));
    auto &doc = *job.sections.front().document;
    QStringList hardBreakMarkers;
    const QString prepared = ListPreview::readHardBreaks(markdown, &hardBreakMarkers);
    const auto dialect = QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML);
    doc.setMarkdown(prepared, dialect);
    if (!ListPreview::restoreReadHardBreaks(doc, hardBreakMarkers)) doc.setMarkdown(markdown, dialect);
    QTextCursor all(&doc); all.select(QTextCursor::Document);
    QTextCharFormat ink; ink.setForeground(Qt::black); ink.setBackground(Qt::transparent); all.mergeCharFormat(ink);

    QMap<QString, QImage> resources;
    QMap<QString, QString> names;
    qsizetype bytes = 0;
    struct Image { int position, length; QTextImageFormat format; };
    QList<Image> images;
    for (auto block = doc.begin(); block.isValid(); block = block.next()) for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (fragment.charFormat().isImageFormat()) images.append({fragment.position(), fragment.length(), fragment.charFormat().toImageFormat()});
    }
    for (auto it = images.crbegin(); it != images.crend(); ++it) {
        const QString source = it->format.name();
        if (!names.contains(source)) {
            const QImage image = resources.size() < MaxImages ? Attachments::loadLocalImage(resolve ? resolve(source) : QUrl()) : QImage();
            QString name;
            if (!image.isNull() && image.sizeInBytes() <= MaxImageBytes - bytes) {
                name = QStringLiteral("print:image/%1").arg(resources.size());
                resources.insert(name, image); bytes += image.sizeInBytes();
            } else if (!job.warnings.contains(QStringLiteral("Some images are unavailable or exceed print safety limits."))) {
                job.warnings.append(QStringLiteral("Some images are unavailable or exceed print safety limits."));
            }
            names.insert(source, name);
        }
        QTextCursor cursor(&doc); cursor.setPosition(it->position); cursor.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
        if (names.value(source).isEmpty()) cursor.insertText(QStringLiteral("[image unavailable]"));
        else { auto f = it->format; f.setName(names.value(source)); cursor.setCharFormat(f); }
    }
    std::vector<Table> tables;
    int totalCells = 0;
    const QFontMetricsF metrics(tableFont(font), job.sections.front().metrics.get());
    // Snapshot all top-level native tables before any source positions are removed.
    for (auto *frame : doc.rootFrame()->childFrames()) {
        auto *native = qobject_cast<QTextTable *>(frame);
        if (!native) continue;
        const qint64 count = qint64(native->rows()) * native->columns();
        if (count > MaxCells - totalCells) { job.error = QStringLiteral("Tables exceed the print cell limit."); return job; }
        totalCells += count;
        Table table; table.original = native; table.number = int(tables.size()) + 1;
        table.rows = native->rows(); table.columns = native->columns();
        table.minimum.fill(48, table.columns); table.preferred.fill(48, table.columns);
        for (int r = 0; r < table.rows; ++r) for (int c = 0; c < table.columns; ++c) {
            const auto cell = native->cellAt(r, c);
            // Qt may merge missing/empty Markdown fields into a preceding
            // native cell. Expand that geometry back to logical grid positions
            // for column bands: retain the text once, at its anchor, and leave
            // covered positions empty instead of duplicating a merged value.
            if (cell.row() != r || cell.column() != c) {
                table.cells.append(QTextDocumentFragment());
                table.firstBlock.append(QTextBlockFormat());
                table.units.append(0);
                continue;
            }
            auto cursor = cell.firstCursorPosition();
            cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
            table.cells.append(QTextDocumentFragment(cursor));
            table.firstBlock.append(cell.firstCursorPosition().blockFormat());
            table.units.append(cell.lastCursorPosition().position() - cell.firstCursorPosition().position());
            const auto text = cursor.selectedText().left(512);
            qreal minimum = 0;
            for (const auto &word : text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
                minimum = qMax(minimum, metrics.horizontalAdvance(word.left(128)));
            table.minimum[c] = qMax(table.minimum[c], qMin(qreal(104), minimum + 2 * Padding + 2));
            table.preferred[c] = qMax(table.minimum[c], qMax(table.preferred[c], qMin(qreal(200), metrics.horizontalAdvance(text.left(256)) + 2 * Padding + 2)));
        }
        if (options.wideTablesToAppendix
            && std::accumulate(table.minimum.begin(), table.minimum.end(), qreal(0)) > tableContentWidth(contentSize(options.pageLayout).width()))
            table.appendix = letters(int(std::count_if(tables.begin(), tables.end(), [](const Table &t) { return !t.appendix.isEmpty(); })));
        tables.push_back(std::move(table));
    }
    qint64 outputUnits = doc.characterCount(), outputCells = totalCells;
    for (auto it = tables.rbegin(); it != tables.rend(); ++it) {
        if (it->appendix.isEmpty()) {
            QVector<int> columns; for (int c = 0; c < it->columns; ++c) columns.append(c);
            formatTable(it->original, widths(*it, columns, contentSize(options.pageLayout).width()), font);
        } else {
            // Including both frame boundary characters removes the native table,
            // not just its text. Snapshots above own every cell's rich fragment.
            QTextCursor cursor(&doc);
            cursor.setPosition(it->original->firstPosition() - 1);
            cursor.setPosition(it->original->lastPosition() + 1, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            QTextBlockFormat reference; reference.setTopMargin(8 * formatUnits(doc)); reference.setBottomMargin(8 * formatUnits(doc));
            cursor.setBlockFormat(reference);
            QTextCharFormat text; text.setFont(font); text.setForeground(Qt::black);
            cursor.insertText(QStringLiteral("See Appendix %1 — Table %2.").arg(it->appendix).arg(it->number), text);
        }
    }
    constrainImages(doc, resources, contentSize(options.pageLayout).width(), contentSize(options.pageLayout).height());
    doc.setLayoutEnabled(true);
    int pages = doc.pageCount();
    if (!tablesFit(doc.rootFrame(), doc)) { job.error = QStringLiteral("A table cannot fit this page. Enable wide-table appendices or choose a wider page."); return job; }
    if (pages > MaxPages) { job.error = QStringLiteral("Print job exceeds the 2,000-page safety limit."); return job; }
    for (const auto &table : tables) {
        if (table.appendix.isEmpty()) continue;
        auto layout = options.pageLayout;
        layout.setOrientation(QPageLayout::Landscape);
        const qreal available = contentSize(layout).width();
        int next = 1;
        do {
            QVector<int> columns{0}; qreal used = table.minimum[0];
            const int first = next;
            while (next < table.columns && (columns.size() == 1 || used + table.minimum[next] <= tableContentWidth(available))) {
                used += table.minimum[next]; columns.append(next++);
            }
            outputCells += qint64(table.rows) * columns.size();
            for (int r = 0; r < table.rows; ++r) for (int c : columns) outputUnits += table.units[r * table.columns + c];
            if (outputUnits > 64 * 1024 * 1024 || outputCells > 200000) {
                job.error = QStringLiteral("Expanded table appendices exceed print safety limits."); return job;
            }
            QString title = QStringLiteral("Appendix %1 — Table %2").arg(table.appendix).arg(table.number);
            if (first > 1 || next < table.columns)
                title += QStringLiteral(" (columns %1–%2; column 1 repeated)").arg(first + 1).arg(next);
            auto appendix = section(layout, font, title);
            auto &target = *appendix.document;
            QTextCursor cursor(&target);
            QTextCharFormat heading; heading.setFontPointSize(14); heading.setFontWeight(QFont::Bold);
            cursor.insertText(title, heading); cursor.insertBlock();
            QTextCharFormat normal; normal.setFont(font); cursor.setCharFormat(normal);
            auto *native = cursor.insertTable(table.rows, columns.size());
            for (int r = 0; r < table.rows; ++r) for (int c = 0; c < columns.size(); ++c) {
                auto cell = native->cellAt(r, c); auto out = cell.firstCursorPosition();
                out.insertFragment(table.cells[r * table.columns + columns[c]]);
                auto firstCursor = cell.firstCursorPosition();
                firstCursor.setBlockFormat(table.firstBlock[r * table.columns + columns[c]]);
            }
            formatTable(native, widths(table, columns, available), font);
            constrainImages(target, resources, available, contentSize(layout).height());
            target.setLayoutEnabled(true);
            pages += target.pageCount();
            if (!tablesFit(target.rootFrame(), target)) { job.error = QStringLiteral("An appendix table cannot fit the selected page. Choose a wider page."); return job; }
            if (pages > MaxPages) { job.error = QStringLiteral("Print job exceeds the 2,000-page safety limit."); return job; }
            job.sections.push_back(std::move(appendix));
        } while (next < table.columns);
    }
    return job;
}

int Job::pageCount() const {
    int pages = 0;
    for (const auto &section : sections) pages += section.document->pageCount();
    return pages;
}

void Section::drawPage(QPainter &painter, int page) const {
    const QSizeF size = document->pageSize();
    painter.save();
    painter.setClipRect(QRectF(QPointF(), size));
    painter.translate(0, -page * size.height());
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, Qt::black);
    context.palette.setColor(QPalette::Base, Qt::white);
    context.clip = QRectF(0, page * size.height(), size.width(), size.height());
    document->documentLayout()->draw(&painter, context);
    painter.restore();
}

bool Job::print(QPrinter &printer, QString *failure) const {
    auto fail = [&](const QString &message) { if (failure) *failure = message; return false; };
    if (!error.isEmpty() || sections.empty()) return fail(error.isEmpty() ? QStringLiteral("Nothing to print.") : error);
    struct Page { const Section *section; int page; };
    std::vector<Page> pages;
    int number = 0;
    for (const auto &section : sections) for (int p = 0; p < section.document->pageCount(); ++p) {
        ++number;
        const auto ranges = printer.pageRanges();
        const bool selected = !ranges.isEmpty() ? ranges.contains(number)
            : number >= qMax(1, printer.fromPage()) && (printer.toPage() == 0 || number <= printer.toPage());
        if (printer.printRange() != QPrinter::PageRange || selected) pages.push_back({&section, p});
    }
    if (pages.empty()) return fail(QStringLiteral("The selected page range is empty."));
    if (printer.pageOrder() == QPrinter::LastPageFirst) std::reverse(pages.begin(), pages.end());
    const int copies = printer.supportsMultipleCopies() ? 1 : qMax(1, printer.copyCount());
    if (copies > 1) {
        if (copies > 1000 || pages.size() > 10000 / size_t(copies)) return fail(QStringLiteral("Requested software copies exceed print limits."));
        const auto original = pages;
        pages.clear();
        if (printer.collateCopies()) {
            for (int copy = 0; copy < copies; ++copy) pages.insert(pages.end(), original.begin(), original.end());
        } else {
            for (const auto &page : original) for (int copy = 0; copy < copies; ++copy) pages.push_back(page);
        }
        printer.setCopyCount(1); // Software owns duplication; never ask the driver too.
    }
    if (!printer.setPageLayout(pages.front().section->layout)) return fail(QStringLiteral("Printer rejected the requested page layout."));
    printer.setFullPage(true);
    QPainter painter;
    if (!painter.begin(&printer)) return fail(QStringLiteral("Could not start the print job."));
    for (size_t i = 0; i < pages.size(); ++i) {
        const auto &page = pages[i];
        // Native printers reject setPageLayout while active; orientation alone
        // can change immediately before newPage (paper size/margins stay fixed).
        if (i && (!printer.setPageOrientation(page.section->layout.orientation()) || !printer.newPage())) {
            printer.abort(); painter.end(); return fail(QStringLiteral("Printer could not create the next page/layout. Try Landscape for the whole job."));
        }
        painter.save();
        painter.scale(printer.logicalDpiX() / 72.0, printer.logicalDpiY() / 72.0);
        const auto rect = page.section->layout.paintRectPoints();
        painter.translate(rect.topLeft());
        painter.scale(72.0 / LayoutDpi, 72.0 / LayoutDpi);
        page.section->drawPage(painter, page.page);
        painter.restore();
    }
    if (!painter.end() || printer.printerState() == QPrinter::Error || printer.printerState() == QPrinter::Aborted)
        return fail(QStringLiteral("Printing failed or was cancelled."));
    return true;
}
} // namespace Printing

#include "livepreview.h"
#include "markdowntablemodel.h"
#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSet>
#include <QGuiApplication>
#include <QClipboard>
#include <QTextCursor>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextTable>
#include <algorithm>
#include <cmath>

namespace {
constexpr int OverflowIndent = QTextFormat::UserProperty + 120;
qreal rowHeight(QObject *view) { return std::ceil(QFontMetricsF(qvariant_cast<QFont>(view->property("font"))).height()) + 16; }
}

void LivePreview::shiftTables(int position, int removed, int added) {
    const int delta = added - removed;
    for (auto &table : m_tables) {
        if (table.end <= position) continue;
        if (table.start >= position + removed) table.start += delta;
        else if (table.start > position) table.start = position;
        table.end += delta;
    }
}

void LivePreview::syncTables() {
    const bool hadDrafts = tableDraftsPending();
    QList<TableSession> next;
    qsizetype bytes = 0;
    const auto font = qvariant_cast<QFont>(m_viewItem->property("font"));
    for (const auto &block : m_blockParser.blocks()) {
        if (block.kind != PreviewBlocks::Kind::Table) continue;
        bytes += block.markdown.size() * 2;
        if (next.size() >= 64 || bytes > 32 * 1024 * 1024) break;
        auto found = std::find_if(m_tables.begin(), m_tables.end(), [&](const auto &t) { return t.start == block.start; });
        TableSession table;
        if (found != m_tables.end()) table = *found;
        else {
            table.id = m_nextPanelId++;
            table.source = false;
            table.model = new MarkdownTableModel(this);
            const auto model = table.model;
            connect(model, &MarkdownTableModel::draftsChanged, this, &LivePreview::tableDraftsChanged);
            connect(model, &MarkdownTableModel::replaceRequested, this,
                [this, model](int offset, int length, const QString &replacement) {
                    if (m_readMode || m_composing || !m_source || !model) return;
                    const auto t = std::find_if(m_tables.cbegin(), m_tables.cend(), [&](const auto &item) { return item.model == model; });
                    if (t == m_tables.cend() || offset < 0 || length < 0 || offset > t->markdown.size()
                        || length > t->markdown.size() - offset) return;
                    // Never apply an editor from an old generation/file to new source.
                    QTextCursor check(m_source);
                    if (t->start < 0 || t->end > m_source->characterCount() - 1) return;
                    check.setPosition(t->start); check.setPosition(t->end, QTextCursor::KeepAnchor);
                    if (check.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n')) != t->markdown) { schedule(); return; }
                    QTextCursor edit(m_source);
                    edit.setPosition(t->start + offset);
                    edit.setPosition(t->start + offset + length, QTextCursor::KeepAnchor);
                    edit.beginEditBlock(); edit.insertText(replacement); edit.endEditBlock();
                    model->acknowledgeReplacement();
                    schedule();
                });
        }
        table.start = block.start; table.end = block.end; table.markdown = block.markdown;
        if (table.source && (m_caret < table.start || m_caret > table.end) && (m_anchor < table.start || m_anchor > table.end)) table.source = false;
        table.model->setSource(block.markdown, font);
        next.append(table);
    }
    for (const auto &old : m_tables) if (std::none_of(next.cbegin(), next.cend(), [&](const auto &t) { return t.model == old.model; })) {
        const auto drafts = old.model->property("drafts").toMap();
        for (auto it = drafts.cbegin(); it != drafts.cend(); ++it)
            m_recoveredTableDrafts.insert(QString::number(old.id) + "/" + it.key(), it.value());
        old.model->deleteLater();
    }
    m_tables = next;
    if (hadDrafts != tableDraftsPending()) emit tableDraftsChanged();
}

void LivePreview::resetTableSessions() {
    for (const auto &table : m_tables) if (table.model) {
        const auto drafts = table.model->property("drafts").toMap();
        for (auto it = drafts.cbegin(); it != drafts.cend(); ++it)
            m_recoveredTableDrafts.insert(QString::number(table.id) + "/" + it.key(), it.value());
        table.model->deleteLater();
    }
    m_tables.clear(); m_tablePanels.clear(); m_readTablePositions.clear();
    m_readStyleKey.clear(); m_renderedRead = false;
    emit tablePanelsChanged(); emit tableDraftsChanged();
    schedule();
}

bool LivePreview::tableDraftsPending() const {
    if (!m_recoveredTableDrafts.isEmpty()) return true;
    for (const auto &table : m_tables) if (table.model && !table.model->property("drafts").toMap().isEmpty()) return true;
    return false;
}
void LivePreview::discardTableDrafts() {
    m_recoveredTableDrafts.clear();
    for (const auto &table : m_tables) if (table.model) table.model->setProperty("drafts", QVariantMap());
    emit tableDraftsChanged();
}
bool LivePreview::copyTableDrafts(int id) {
    QVariantMap drafts = id ? QVariantMap() : m_recoveredTableDrafts;
    for (const auto &table : m_tables) if ((!id || table.id == id) && table.model) {
        const auto cells = table.model->property("drafts").toMap();
        for (auto it = cells.cbegin(); it != cells.cend(); ++it) drafts.insert(QString::number(table.id) + "/" + it.key(), it.value());
    }
    if (drafts.isEmpty()) return false;
    QStringList text;
    for (auto it = drafts.cbegin(); it != drafts.cend(); ++it) text.append(it.key() + ":\n" + it.value().toMap().value("text").toString());
    QGuiApplication::clipboard()->setText(text.join("\n\n")); return true;
}

void LivePreview::setTableSource(int id, bool source) {
    if (m_readMode || m_composing) return;
    for (auto &table : m_tables) if (table.id == id) {
        table.source = source;
        if (source && (m_caret < table.start || m_caret > table.end)) {
            m_caret = m_anchor = table.start;
            select(m_sourceItem, m_anchor, m_caret);
        }
        schedule(); return;
    }
}

LivePreview::Overflow &LivePreview::overflowFor(const QTextBlock &first, const QTextBlock &last, QTextTable *table) {
    for (auto &item : m_overflows) if (item.first == first && item.table == table) { item.last = last; return item; }
    m_overflows.append({m_nextPanelId++, first, last, table, 0, 0, {}, {}});
    return m_overflows.last();
}

void LivePreview::styleOverflow() {
    if (!m_view || !m_viewItem || m_view->pageSize() == QSizeF(0, 0)) return;
    // Native single-paragraph edits retain no-wrap membership. Revisit rows
    // only after topology/list changes, a document switch, or horizontal scroll.
    if (!m_readMode && !m_overflowStyleDirty && !m_overflowScrolling && m_overflowEndpoints == m_codeEndpoints) return;
    m_overflowStyleDirty = false; m_overflowEndpoints = m_codeEndpoints;
    QScopedValueRollback<bool> guard(m_guard, true);
    QSignalBlocker layoutSignals(m_view->documentLayout());
    QTextCursor batch(m_view); batch.beginEditBlock();
    QList<QTextBlock> wanted;
    int firstDirty = m_view->characterCount(), lastDirty = 0;
    const auto dirty = [&](int first, int last) { firstDirty = qMin(firstDirty, first); lastDirty = qMax(lastDirty, last); };
    const auto &groups = m_readMode ? m_readCodeBlocks : m_editCodeGroups;
    for (const auto &group : groups) {
        const auto first = m_view->findBlockByNumber(group.first), last = m_view->findBlockByNumber(group.second);
        if (!first.isValid() || !last.isValid()) continue;
        auto &record = overflowFor(first, last);
        for (auto b = first; b.isValid() && b.blockNumber() <= group.second; b = b.next()) {
            ++m_overflowStyleVisits;
            auto f = b.blockFormat(); wanted.append(b);
            if (!f.hasProperty(OverflowIndent)) f.setProperty(OverflowIndent, f.textIndent());
            const qreal indent = f.doubleProperty(OverflowIndent) - record.offset;
            if (!f.nonBreakableLines() || f.textIndent() != indent || !b.blockFormat().hasProperty(OverflowIndent)) {
                dirty(b.position(), b.position() + b.length());
                f.setNonBreakableLines(true); f.setTextIndent(indent); QTextCursor(b).setBlockFormat(f);
            }
        }
    }
    if (m_readMode) {
        const auto frames = m_view->rootFrame()->childFrames();
        for (auto *frame : frames) if (auto *table = qobject_cast<QTextTable *>(frame)) {
            auto &record = overflowFor(table->firstCursorPosition().block(), table->lastCursorPosition().block(), table);
            auto f = table->format();
            f.setWidth(QTextLength(QTextLength::VariableLength, 0)); f.setLeftMargin(-record.offset);
            f.setBottomMargin(qMax(qreal(16), f.bottomMargin()));
            if (table->format() != f) { dirty(table->firstPosition(), table->lastPosition() + 1); table->setFormat(f); }
            for (auto b = table->firstCursorPosition().block(); b.isValid() && b.position() <= table->lastPosition(); b = b.next()) {
                wanted.append(b); auto bf = b.blockFormat();
                if (!bf.nonBreakableLines()) { dirty(b.position(), b.position() + b.length()); bf.setNonBreakableLines(true); QTextCursor(b).setBlockFormat(bf); }
            }
        }
    }
    // Paragraph splits inherit no-wrap and indent. Restore only our properties
    // when such paragraphs leave a fence/table, never touch canonical formats.
    QSet<int> current;
    for (const auto &b : wanted) current.insert(b.blockNumber());
    for (const auto &b : m_unwrapped) if (b.isValid() && b.document() == m_view && !current.contains(b.blockNumber())) {
        auto f = b.blockFormat(); f.setNonBreakableLines(false);
        if (f.hasProperty(OverflowIndent)) { f.setTextIndent(f.doubleProperty(OverflowIndent)); f.clearProperty(OverflowIndent); }
        dirty(b.position(), b.position() + b.length()); QTextCursor(b).setBlockFormat(f);
    }
    for (const auto &b : m_unwrapped) if (b.isValid() && b.document() != m_view) wanted.append(b);
    m_unwrapped = wanted;
    batch.endEditBlock();
    layoutSignals.unblock();
    if (lastDirty > firstDirty) m_view->markContentsDirty(firstDirty, lastDirty - firstDirty);
    for (auto it = m_overflows.begin(); it != m_overflows.end();) {
        if (!it->first.isValid()) { m_blockOffsets.remove(QString::number(it->id)); it = m_overflows.erase(it); }
        else ++it;
    }
}

QRectF LivePreview::visualCursorRectangle(const QRectF &native) const {
    if (!m_view || !m_viewItem || m_readMode || m_composing) return native;
    const int cursor = m_viewItem->property("cursorPosition").toInt();
    const auto block = m_view->findBlock(cursor);
    const auto *layout = block.layout();
    if (!layout || layout->lineCount() != 1 || !block.blockFormat().hasProperty(OverflowIndent)) return native;
    // ManualWrap clamps negative cursorToX positions. Re-measure just this
    // native line with NoWrap to recover the true offscreen caret, including
    // font formats, tabs and bidi; the source and native document stay intact.
    const auto line = layout->lineAt(0);
    QTextLayout logical(block.text(), m_view->defaultFont());
    auto option = layout->textOption(); option.setWrapMode(QTextOption::NoWrap);
    logical.setTextOption(option); logical.setFormats(layout->formats());
    logical.beginLayout(); auto measured = logical.createLine();
    if (!measured.isValid()) { logical.endLayout(); return native; }
    measured.setLineWidth(line.width()); logical.endLayout();
    const int position = cursor - block.position();
    QRectF result = native;
    // cursorPositionChanged can precede cursorRectangleChanged. Do not use
    // the possibly previous caret's x as an origin for navigation decisions.
    const qreal left = m_viewItem->property("leftPadding").toReal();
    qreal alignment = m_viewItem->property("width").toReal() - left
        - m_viewItem->property("rightPadding").toReal() - m_viewItem->property("contentWidth").toReal();
    const int horizontal = m_viewItem->property("effectiveHorizontalAlignment").toInt();
    alignment = horizontal == Qt::AlignHCenter ? alignment / 2 : horizontal == Qt::AlignRight ? alignment : 0;
    result.moveLeft(left + qMax(qreal(0), alignment) + layout->position().x() + line.x() + measured.cursorToX(position));
    return result;
}

void LivePreview::scrollBlock(int id, qreal offset) {
    if (!std::isfinite(offset) || m_composing || (m_pending && !m_guard) || !m_view) return;
    for (auto &item : m_overflows) if (item.id == id && item.first.isValid() && item.first.document() == m_view) {
        const qreal width = qMax(qreal(1), m_viewItem->property("width").toReal() - 8);
        offset = qBound(qreal(0), offset, qMax(qreal(0), item.width - width));
        if (qAbs(item.offset - offset) < 0.25) return;
        item.offset = offset; m_blockOffsets.insert(QString::number(id), offset);
        QScopedValueRollback<bool> scrolling(m_overflowScrolling, true);
        styleOverflow();
        emit blockOffsetsChanged();
        scheduleCodeBlockPanels();
        return;
    }
}

void LivePreview::updateTablePanels() {
    QVariantList panels, scrolls;
    if (!m_view || !m_viewItem) return;
    const qreal top = m_viewItem->property("topPadding").toReal();
    const qreal width = qMax(qreal(1), m_viewItem->property("width").toReal() - 8);
    const auto visible = visibleBlocks();
    auto *layout = m_view->documentLayout();
    for (const auto &table : m_tables) {
        if (!table.model || (!m_readMode && table.source) || table.model->rowCount() == 0) continue;
        int position = -1;
        if (m_readMode) position = m_readTablePositions.value(table.id, -1);
        else {
            const auto segment = std::find_if(m_segments.cbegin(), m_segments.cend(), [&](const auto &s) { return s.object && s.sourceStart == table.start; });
            if (segment != m_segments.cend()) position = segment->viewStart;
        }
        if (position < 0 || m_view->characterAt(position) != QChar::ObjectReplacementCharacter) continue;
        const auto block = m_view->findBlock(position);
        const auto bounds = layout->blockBoundingRect(block);
        const qreal height = qMin(qreal(480), 68 + rowHeight(m_viewItem) * table.model->rowCount());
        panels.append(QVariantMap{{"id", table.id}, {"model", QVariant::fromValue(static_cast<QObject *>(table.model))},
            {"inViewport", block.blockNumber() >= visible.first && block.blockNumber() <= visible.second},
            {"x", 4.0}, {"y", top + bounds.top()}, {"width", width}, {"height", height}});
    }
    if (m_readMode) for (auto &item : m_overflows) if (item.table && item.first.document() == m_view) {
        auto *table = item.table.data();
        if (item.last.blockNumber() < visible.first || item.first.blockNumber() > visible.second) continue;
        const auto bounds = layout->frameBoundingRect(table);
        item.width = qMax(width, bounds.width() + item.offset + 8);
        if (item.offset > item.width - width + 0.5) { scrollBlock(item.id, item.offset); return; }
        scrolls.append(QVariantMap{{"id", item.id}, {"x", 4.0}, {"y", top + bounds.top()},
            {"width", width}, {"height", bounds.height()}, {"contentWidth", item.width}});
    }
    if (panels != m_tablePanels || scrolls != m_tableScrollPanels) {
        m_tablePanels = panels; m_tableScrollPanels = scrolls; emit tablePanelsChanged();
    }
}

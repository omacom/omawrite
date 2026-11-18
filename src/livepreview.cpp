#include "livepreview.h"
#include "spellingcontroller.h"
#include "markdownhighlighter.h"
#include "mermaidrenderer.h"
#include "readcodehighlighting.h"

#include <QClipboard>
#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QInputMethod>
#include <QFontMetricsF>
#include <QPainter>
#include <QKeyEvent>
#include <QQuickTextDocument>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTextImageFormat>
#include <QTextLayout>
#include <QTextTable>
#include "markdowntablemodel.h"
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr qsizetype MaxProjectionBytes = 32 * 1024 * 1024;
constexpr int MaxProjectionObjects = 64;
QString documentText(const QTextDocument *document) {
    return document->toRawText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
}
class ResourceFreeDocument : public QTextDocument {
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};
QList<LivePreview::Segment> mappedSegments(const QList<QPair<int, int>> &objects, int length) {
    QList<LivePreview::Segment> result;
    int source = 0, view = 0;
    for (const auto &object : objects) {
        if (object.first > source) {
            result.append({source, object.first, view, view + object.first - source, false});
            view += object.first - source;
        }
        result.append({object.first, object.second, view, view + 1, true});
        source = object.second; ++view;
    }
    result.append({source, length, view, view + length - source, false});
    return result;
}
QString textRange(const QTextDocument *document, int from, int length) {
    QTextCursor cursor(const_cast<QTextDocument *>(document));
    cursor.setPosition(qBound(0, from, document->characterCount() - 1));
    cursor.setPosition(qBound(cursor.position(), from + length, document->characterCount() - 1), QTextCursor::KeepAnchor);
    return cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
}
QColor strongForeground(QObject *view) {
    const QColor foreground = view->property("previewForeground").value<QColor>();
    return foreground.isValid() ? foreground : view->property("color").value<QColor>();
}

QVector<bool> listBlankLines(const QString &source) {
    // Qt drops list looseness during Markdown import. Keep only this small
    // piece of source spacing metadata; never infer gaps from list identity
    // (nested tight items belong to different QTextLists).
    QVector<bool> gaps;
    const QRegularExpression item(QStringLiteral("^\\s*(?:[-+*]|[0-9]+[.)])\\s+"));
    const QRegularExpression fence(QStringLiteral("^\\s*(`{3,}|~{3,})(.*)$"));
    QChar fenceCharacter;
    int fenceLength = 0;
    bool blank = false;
    for (const QString &line : source.split(QLatin1Char('\n'))) {
        const auto match = fence.match(line);
        if (fenceLength) {
            if (match.hasMatch() && match.captured(1).front() == fenceCharacter
                && match.capturedLength(1) >= fenceLength && match.captured(2).trimmed().isEmpty())
                fenceLength = 0;
            continue;
        }
        if (match.hasMatch()) {
            fenceCharacter = match.captured(1).front();
            fenceLength = match.capturedLength(1);
        } else if (item.match(line).hasMatch()) {
            gaps.append(blank);
            blank = false;
        } else if (line.trimmed().isEmpty()) {
            blank = true;
        }
    }
    return gaps;
}

void styleReadDocument(QTextDocument &document, const QString &source,
                       const QColor &foreground, const QColor &body, const QColor &accent, bool dark) {
    // Measure an actual blank editor line: pointSize() is -1 for pixel fonts,
    // which makes Qt's imported Markdown paragraph margins collapse.
    QTextLayout blank(QStringLiteral(" "), document.defaultFont());
    blank.beginLayout();
    QTextLine line = blank.createLine();
    line.setLineWidth(100);
    const qreal gap = line.height();
    blank.endLayout();
    const QColor codeBackground(dark ? QStringLiteral("#252525") : QStringLiteral("#eeeeee"));
    const QColor quoteColor(dark ? QStringLiteral("#4f525a") : QStringLiteral("#aeb1b5"));
    QSet<QTextTable *> tables;
    QVector<bool> listGaps = listBlankLines(source);
    int listCount = 0;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next())
        if (block.textList()) ++listCount;
    // HTML/quoted/ambiguous list syntax is left tight rather than assigning
    // unrelated source whitespace to the wrong imported list item.
    if (listCount != listGaps.size()) listGaps.clear();
    int listIndex = -1;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        QTextTable *table = cursor.currentTable();
        if (table) tables.insert(table);
        QTextBlockFormat format = block.blockFormat();
        const bool code = format.hasProperty(QTextFormat::BlockCodeLanguage);
        const QTextBlock next = block.next();
        const bool nextCode = next.isValid()
            && next.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage);
        if (block.textList()) ++listIndex;
        const bool tightList = block.textList() && next.isValid() && next.textList()
            && !listGaps.value(listIndex + 1, false);
        const bool nextTable = next.isValid() && QTextCursor(next).currentTable();
        format.setTopMargin(0);
        format.setBottomMargin(!next.isValid() || table || nextTable || tightList
                               || (code && nextCode) || block.text().isEmpty() ? 0 : gap);
        const bool quote = format.intProperty(QTextFormat::BlockQuoteLevel) > 0;
        if (quote && !code && !table && !block.textList() && !format.headingLevel())
            format.setAlignment(Qt::AlignJustify);
        cursor.setBlockFormat(format);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat existing = fragment.charFormat();
            QTextCharFormat patch;
            patch.setForeground(quote ? quoteColor : (format.headingLevel() > 0 ? foreground : body));
            if (quote) patch.setFontItalic(true);
            // Explicit Markdown code property, not the resolved font: the
            // whole editor may itself use a monospace writing font.
            if (code || (existing.hasProperty(QTextFormat::FontFixedPitch) && existing.fontFixedPitch())) {
                patch.setForeground(foreground);
                patch.setBackground(code && format.hasProperty(QTextFormat::BlockCodeFence)
                                    ? QBrush(Qt::NoBrush) : QBrush(codeBackground));
                patch.setFontFamilies(document.defaultFont().families());
            }
            if (existing.isAnchor() && !existing.anchorHref().isEmpty()) {
                patch.setForeground(accent);
                patch.setFontUnderline(true);
            }
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(patch);
        }
    }
    // Frame margins belong outside tables, never inside every cell paragraph.
    for (QTextTable *table : tables) {
        QTextTableFormat format = table->format();
        const QTextBlock before = table->firstCursorPosition().block().previous();
        format.setTopMargin(before.isValid() && !before.text().isEmpty() ? gap : 0);
        format.setBottomMargin(gap);
        table->setFormat(format);
    }
}
}

LivePreview::LivePreview(QObject *parent) : QObject(parent), m_mermaid(new MermaidRenderer(this)) {
    connect(m_mermaid, &MermaidRenderer::ready, this, &LivePreview::refresh);
}

QObject *LivePreview::spelling() const { return m_spelling; }
void LivePreview::setSpelling(QObject *spelling) {
    auto *controller = qobject_cast<SpellingController *>(spelling);
    if (m_spelling == controller)
        return;
    if (m_spelling)
        disconnect(m_spelling, nullptr, this, nullptr);
    m_spelling = controller;
    if (m_spelling)
        connect(m_spelling, &SpellingController::rangesChanged, this, &LivePreview::refreshSpelling);
    refreshSpelling();
}
QVariantMap LivePreview::spellingWordAt(int position) {
    if (!m_spelling || m_readMode || m_composing)
        return {};
    for (const auto &segment : m_segments) {
        if (position < segment.viewStart || position >= segment.viewEnd)
            continue;
        if (segment.object)
            return {};
        const auto token = m_spelling->wordAt(segment.sourceStart + position - segment.viewStart);
        const int from = token.value(QStringLiteral("start"), -1).toInt();
        const int to = from + token.value(QStringLiteral("length")).toInt();
        return from >= segment.sourceStart && to <= segment.sourceEnd ? token : QVariantMap();
    }
    // Keyboard menu at the final insertion point is still useful.
    if (!m_segments.isEmpty() && !m_segments.last().object && position == m_segments.last().viewEnd)
        return m_spelling->wordAt(m_segments.last().sourceEnd);
    return {};
}

int LivePreview::characterAtPoint(qreal x, qreal y) const {
    if (!m_view || !m_viewItem || m_composing)
        return -1;
    const QSizeF size = m_view->documentLayout()->documentSize();
    const qreal left = m_viewItem->property("leftPadding").toReal();
    const qreal top = m_viewItem->property("topPadding").toReal();
    qreal dx = m_viewItem->property("width").toReal() - left
        - m_viewItem->property("rightPadding").toReal() - size.width();
    qreal dy = m_viewItem->property("height").toReal() - top
        - m_viewItem->property("bottomPadding").toReal() - size.height();
    const int horizontal = m_viewItem->property("effectiveHorizontalAlignment").toInt();
    const int vertical = m_viewItem->property("verticalAlignment").toInt();
    dx = horizontal == Qt::AlignHCenter ? dx / 2 : horizontal == Qt::AlignRight ? dx : 0;
    dy = vertical == Qt::AlignVCenter ? dy / 2 : vertical == Qt::AlignBottom ? dy : 0;
    const QPointF point(x - left - qMax(qreal(0), dx), y - top - dy);
    const int hit = m_view->documentLayout()->hitTest(point, Qt::ExactHit);
    if (hit < 0)
        return -1;
    const QTextBlock block = m_view->findBlock(hit);
    const QTextLayout *layout = block.layout();
    if (!layout)
        return -1;
    const QPointF local = point - layout->position();
    for (int i = 0; i < layout->lineCount(); ++i) {
        const QTextLine line = layout->lineAt(i);
        if (!line.naturalTextRect().contains(local))
            continue;
        return block.position() + line.xToCursor(local.x(), QTextLine::CursorOnCharacter);
    }
    return -1;
}

QVariantMap LivePreview::spellingWordAtPoint(qreal x, qreal y) {
    if (m_readMode) return {};
    const int position = characterAtPoint(x, y);
    if (position < 0) return {};
    const QVariantMap token = spellingWordAt(position);
    const int source = sourcePosition(position);
    const int start = token.value(QStringLiteral("start"), -1).toInt();
    return source >= start && source < start + token.value(QStringLiteral("length")).toInt()
        ? token : QVariantMap();
}

void LivePreview::refreshSpelling() {
    if (!m_highlighter || m_composing)
        return;
    QList<MarkdownHighlighter::Span> projected;
    if (m_spelling && !m_readMode) {
        int segmentIndex = 0;
        for (const auto &range : m_spelling->ranges()) {
            const int end = range.start + range.length;
            while (segmentIndex < m_segments.size() && m_segments.at(segmentIndex).sourceEnd <= range.start)
                ++segmentIndex;
            for (int i = segmentIndex; i < m_segments.size() && m_segments.at(i).sourceStart < end; ++i) {
                const auto &segment = m_segments.at(i);
                if (segment.object)
                    continue; // Never map an endpoint onto a rendered object's glyph.
                const int from = qMax(range.start, segment.sourceStart);
                const int to = qMin(end, segment.sourceEnd);
                if (from < to)
                    projected.append({segment.viewStart + from - segment.sourceStart, to - from});
            }
        }
    }
    // Deliberately no schedule(), refresh(), or rebuild(): a spelling reply
    // changes only the affected highlighter blocks, not the projection.
    m_highlighter->setSpellingRanges(projected);
}

int LivePreview::toSource(const QList<Segment> &segments, int p) {
    for (const auto &s : segments) {
        if (p < s.viewEnd)
            return s.object ? s.sourceStart : s.sourceStart + qMax(0, p - s.viewStart);
        if (p == s.viewEnd)
            return s.sourceEnd;
    }
    return segments.isEmpty() ? p : segments.last().sourceEnd;
}
int LivePreview::toView(const QList<Segment> &segments, int p) {
    for (const auto &s : segments) {
        if (p < s.sourceEnd)
            return s.object ? s.viewStart : s.viewStart + qMax(0, p - s.sourceStart);
        if (p == s.sourceEnd)
            return s.viewEnd;
    }
    return segments.isEmpty() ? p : segments.last().viewEnd;
}
int LivePreview::sourcePosition(int p) const { return toSource(m_segments, p); }
int LivePreview::viewPosition(int p) const { return toView(m_segments, p); }

void LivePreview::attach(QObject *sourceItem, QObject *viewItem) {
    if (m_sourceItem || !sourceItem || !viewItem)
        return;
    auto sourceQuick = qvariant_cast<QQuickTextDocument *>(sourceItem->property("textDocument"));
    auto viewQuick = qvariant_cast<QQuickTextDocument *>(viewItem->property("textDocument"));
    if (!sourceQuick || !viewQuick)
        return;
    m_sourceItem = sourceItem;
    m_viewItem = viewItem;
    m_source = sourceQuick->textDocument();
    // Qt 6.7+ permits supplying a native document to TextEdit. Deny implicit
    // resources at the document boundary, including rich paste/drop paths;
    // only explicitly registered, bounded local images can be displayed.
    auto *document = new ResourceFreeDocument;
    document->setParent(viewItem);
    viewQuick->setTextDocument(document);
    m_view = document;
    m_editDocument = document;
    m_view->setUndoRedoEnabled(false);
    m_highlighter = new MarkdownHighlighter(m_view);
    viewItem->installEventFilter(this);
    connect(sourceItem, SIGNAL(cursorPositionChanged()), this, SLOT(sourceSelectionChanged()));
    connect(sourceItem, SIGNAL(selectionStartChanged()), this, SLOT(sourceSelectionChanged()));
    connect(sourceItem, SIGNAL(selectionEndChanged()), this, SLOT(sourceSelectionChanged()));
    connect(viewItem, SIGNAL(cursorPositionChanged()), this, SLOT(viewSelectionChanged()));
    connect(viewItem, SIGNAL(selectionStartChanged()), this, SLOT(viewSelectionChanged()));
    connect(viewItem, SIGNAL(selectionEndChanged()), this, SLOT(viewSelectionChanged()));
    connect(m_source, &QTextDocument::contentsChanged, this, &LivePreview::sourceTextChanged);
    connect(m_view, &QTextDocument::contentsChange, this, &LivePreview::viewContentsChanged);
    connect(m_view->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, &LivePreview::scheduleCodeBlockPanels);
    connect(m_view->documentLayout(), &QAbstractTextDocumentLayout::update,
            this, &LivePreview::scheduleCodeBlockPanels);
    connect(viewItem, SIGNAL(widthChanged()), this, SLOT(sourceGeometryChanged()));
    sourceSelectionChanged();
    if (m_readMode) {
        QScopedValueRollback<bool> guard(m_guard, true);
        switchModeDocument(true);
    }
    rebuild();
}
void LivePreview::select(QObject *item, int anchor, int caret) {
    QMetaObject::invokeMethod(item, "select", Q_ARG(int, anchor), Q_ARG(int, caret));
}
void LivePreview::sourceSelectionChanged() {
    if (m_guard || !m_sourceItem)
        return;
    m_revealOverflowCaret = true;
    m_caret = m_sourceItem->property("cursorPosition").toInt();
    const int start = m_sourceItem->property("selectionStart").toInt();
    const int end = m_sourceItem->property("selectionEnd").toInt();
    m_anchor = m_caret == start ? end : start;
    schedule();
}
void LivePreview::viewSelectionChanged() {
    if (m_guard || m_readMode || !m_viewItem)
        return;
    const int caret = m_viewItem->property("cursorPosition").toInt();
    const int start = m_viewItem->property("selectionStart").toInt();
    const int end = m_viewItem->property("selectionEnd").toInt();
    m_revealOverflowCaret = true;
    m_caret = sourcePosition(caret);
    m_anchor = sourcePosition(caret == start ? end : start);
    {
        QScopedValueRollback<bool> guard(m_guard, true);
        select(m_sourceItem, m_anchor, m_caret);
    }
    schedule();
}
void LivePreview::sourceTextChanged() {
    if (!m_guard) { m_sourceDirty = true; schedule(); }
}
void LivePreview::sourceGeometryChanged() {
    m_renderDirty = true;
    schedule();
}
void LivePreview::setViewport(qreal top, qreal height) {
    if (!std::isfinite(top) || !std::isfinite(height) || height < 0) return;
    if (m_viewportTop == top && m_viewportHeight == height) return;
    m_viewportTop = top; m_viewportHeight = height;
    emit viewportChanged();
    scheduleCodeBlockPanels();
}
void LivePreview::recordViewChange(int position, int removed, const QString &added) {
    const int first = m_view->findBlock(qMin(position, m_view->characterCount() - 1)).blockNumber();
    const int addedLines = added.count(QLatin1Char('\n'));
    m_listParser.splice(first, m_viewText.mid(position, removed).count(QLatin1Char('\n')) + 1,
                        addedLines + 1);
    m_listDirtyFirst = qMin(m_listDirtyFirst, first);
    m_viewText.replace(position, removed, added);
    if (addedLines || removed > 0) {
        m_codeSpacingDirty = true;
        m_overflowStyleDirty = true;
        // Split native paragraphs inherit the old fence's spacing. Track
        // those new blocks too, so styling can normalize non-boundaries.
        for (auto b = m_view->findBlockByNumber(first); b.isValid()
             && b.blockNumber() <= first + addedLines; b = b.next()) {
            const auto f = b.blockFormat();
            if (f.nonBreakableLines() && !m_unwrapped.contains(b)) m_unwrapped.append(b);
            if ((f.topMargin() || f.bottomMargin()) && std::none_of(m_panelMargins.cbegin(), m_panelMargins.cend(),
                [&](const auto &entry) { return entry.first == b; })) m_panelMargins.append({b, {f.topMargin(), f.bottomMargin()}});
        }
    }
}
void LivePreview::viewContentsChanged(int position, int removed, int addedCount) {
    if (m_guard || m_readMode || !m_source || !m_view)
        return;
    const QString added = textRange(m_view, position, addedCount);
    if (m_viewText.mid(position, removed) == added)
        return; // Native formatting notifications are not source edits.
    // Use Qt's actual edit range, not a text diff: repeated characters and
    // NBSP must not move an edit to a different source position.
    const int prefix = qBound(0, position, int(m_viewText.size()));
    const int oldEnd = qBound(prefix, position + removed, int(m_viewText.size()));
    const int from = sourcePosition(prefix);
    const int to = sourcePosition(oldEnd);
    for (const auto &segment : m_segments) {
        if (segment.object && from < segment.sourceEnd && to > segment.sourceStart) {
            // A non-keyboard edit (e.g. an accessibility command or drop) may
            // bypass prepareEdit. Reject ambiguous object deletion, never turn
            // removal of one image glyph into removal of its Markdown block.
            m_forceReset = true; m_viewSyncedSource = false;
            schedule();
            return;
        }
    }
    // Never accept a pasted/dragged rich-text object as source Markdown.
    if (added.contains(QChar::ObjectReplacementCharacter)) {
        m_forceReset = true; m_viewSyncedSource = false;
        schedule();
        return;
    }
    {
        QScopedValueRollback<bool> guard(m_guard, true);
        QTextCursor cursor(m_source);
        cursor.setPosition(from);
        cursor.setPosition(to, QTextCursor::KeepAnchor);
        cursor.insertText(added);
        m_anchor = m_caret = from + added.size();
        select(m_sourceItem, m_anchor, m_caret);
        recordViewChange(prefix, oldEnd - prefix, added);
    }
    // Native input already changed the persistent presentation. Only canonical
    // text, bounded object mapping and resumable parser metadata need updating.
    const int delta = added.size() - (to - from);
    m_revealOverflowCaret = true;
    shiftTables(from, to - from, added.size());
    m_projectionSource.replace(from, to - from, added); ++m_sourceGeneration;
    m_blockParser.edit(m_projectionSource, from, to - from, added.size());
    QList<QPair<int, int>> objects;
    for (const auto &segment : m_segments) if (segment.object) {
        const int shift = segment.sourceStart >= to ? delta : 0;
        objects.append({segment.sourceStart + shift, segment.sourceEnd + shift});
    }
    m_segments = mappedSegments(objects, m_projectionSource.size());
    m_sourceDirty = false; m_viewSyncedSource = true;
    schedule();
}
void LivePreview::schedule() {
    if (m_pending)
        return;
    m_pending = true;
    QTimer::singleShot(0, this, [this] {
        if (!m_pending)
            return;
        m_pending = false;
        if (!m_composing)
            rebuild();
    });
}
void LivePreview::refresh() {
    ++m_resourceGeneration;
    m_images.clear();
    m_sourceDirty = true; m_styleDirty = true; m_renderDirty = true;
    schedule();
}
void LivePreview::setSearchHighlight(const QString &query, int currentMatchStart) {
    m_searchQuery = query;
    m_currentMatch = currentMatchStart;
    if (!m_readMode && m_projectionSource.contains(QStringLiteral("[^"))) schedule();
    if (m_highlighter && !m_readMode)
        m_highlighter->setSearch(query, currentMatchStart < 0 ? -1 : viewPosition(currentMatchStart));
}
void LivePreview::setReadMode(bool read) {
    if (read == m_readMode)
        return;
    // Finish native composition before replacing the presentation document.
    QGuiApplication::inputMethod()->commit();
    // Synthetic/cancelling input methods need not deliver a commit. Clear
    // their preedit before parking the native Edit layout.
    if (m_composing && m_viewItem) { QInputMethodEvent cancel; QCoreApplication::sendEvent(m_viewItem, &cancel); }
    {
        QScopedValueRollback<bool> guard(m_guard, true);
        m_readMode = read;
        // Qt's readOnly setter moves its control cursor. Deliver it inside
        // the same selection guard as document attachment, not after ready.
        emit readModeChanged();
        if (!m_view) return;
        m_projectionChanging = true;
        emit projectionAboutToChange();
        switchModeDocument(read);
    }
    rebuild();
}
void LivePreview::switchModeDocument(bool read) {
    auto &old = read ? m_savedEdit : m_savedRead;
    old.text = m_viewText; old.source = m_projectionSource;
    old.segments = m_segments; old.resources = m_resourceNames;
    old.margins = m_panelMargins; old.endpoints = m_codeEndpoints;
    old.groups = m_editCodeGroups; old.invalidImages = m_invalidImageSlots;
    old.lineHeight = m_codeLineHeight; old.spacingDirty = m_codeSpacingDirty;
    if (read && !m_readDocument) {
        auto *document = new ResourceFreeDocument;
        document->setParent(m_viewItem);
        document->setUndoRedoEnabled(false);
        document->setDefaultFont(m_view->defaultFont());
        document->setTextWidth(m_view->textWidth());
        m_readDocument = document;
        connect(document->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                this, &LivePreview::scheduleCodeBlockPanels);
        connect(document->documentLayout(), &QAbstractTextDocumentLayout::update,
                this, &LivePreview::scheduleCodeBlockPanels);
    }
    const auto &next = read ? m_savedRead : m_savedEdit;
    m_view = read ? m_readDocument : m_editDocument;
    m_viewText = next.text; m_segments = next.segments; m_resourceNames = next.resources;
    m_panelMargins = next.margins; m_codeEndpoints = next.endpoints;
    m_editCodeGroups = next.groups; m_invalidImageSlots = next.invalidImages;
    m_codeLineHeight = next.lineHeight; m_codeSpacingDirty = next.spacingDirty;
    m_viewSyncedSource = next.source == m_projectionSource;
    m_renderedRead = read && !m_readStyleKey.isEmpty();
    m_forceReset = false;
    // Read is prepared offscreen first; Edit is already a complete native
    // document, with its original highlighter, blocks and layout intact.
    if (!read) {
        // Read may have consumed a resize/theme/refresh notification. Its
        // render-dirty bit does not validate the retained Edit resources.
        m_renderDirty = true;
        auto *quick = qvariant_cast<QQuickTextDocument *>(m_viewItem->property("textDocument"));
        quick->setTextDocument(m_view);
    }
}
QUrl LivePreview::resolve(const QString &path) const {
    QUrl url;
    if (m_resolver)
        QMetaObject::invokeMethod(m_resolver, "resolveImageUrl", Qt::DirectConnection,
                                  Q_RETURN_ARG(QUrl, url), Q_ARG(QString, path));
    return url.isLocalFile() ? url : QUrl();
}
QImage LivePreview::imageFor(const PreviewBlocks::Block &block) {
    const int width = qMax(80, m_viewItem->property("width").toInt() - 8);
    const QFont font = qvariant_cast<QFont>(m_viewItem->property("font"));
    if (block.kind == PreviewBlocks::Kind::Table) {
        const auto t = std::find_if(m_tables.cbegin(), m_tables.cend(), [&](const auto &item) { return item.start == block.start; });
        if (t == m_tables.cend() || !t->model || !t->model->rowCount()) return {};
        const int height = qMin(480, 68 + (int(std::ceil(QFontMetricsF(font).height())) + 16) * t->model->rowCount());
        const QString slot = QStringLiteral("table-slot/%1/%2").arg(width).arg(height);
        if (m_images.contains(slot)) return m_images.value(slot);
        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        if (image.sizeInBytes() <= MaxProjectionBytes && m_images.size() < MaxProjectionObjects) m_images.insert(slot, image);
        return image;
    }
    const QColor foreground = qvariant_cast<QColor>(m_viewItem->property("color"));
    const QColor background = m_viewItem->property("previewBackground").value<QColor>();
    const QUrl url = block.kind == PreviewBlocks::Kind::Image ? resolve(block.imagePath) : QUrl();
    const QString key = block.markdown + QLatin1Char('|') + QString::number(width)
        + font.toString() + foreground.name() + background.name() + url.toString();
    if (m_images.contains(key))
        return m_images.value(key);
    QImage image;
    if (block.kind == PreviewBlocks::Kind::HorizontalRule)
        image = PreviewBlocks::renderHorizontalRule(font, foreground, width);
    else if (block.kind == PreviewBlocks::Kind::Table)
        image = PreviewBlocks::renderTable(block.markdown, font, foreground, background, width);
    else if (block.kind == PreviewBlocks::Kind::Image)
        image = PreviewBlocks::loadImage(url, width);
    else if (block.kind == PreviewBlocks::Kind::Mermaid)
        image = m_mermaid->request(block.markdown, m_viewItem->property("previewDark").toBool(), width);
    // Missing/failed/unsupported Mermaid stays editable source.
    if (!image.isNull()) {
        qsizetype bytes = image.sizeInBytes();
        for (const auto &cached : m_images)
            bytes += cached.sizeInBytes();
        // Retain the entire allowed projection working set (up to 64).
        // Clearing at 32 used to rerender every object on every caret move.
        while (!m_images.isEmpty() && (m_images.size() >= MaxProjectionObjects || bytes > MaxProjectionBytes)) {
            auto victim = m_images.begin(); bytes -= victim.value().sizeInBytes(); m_images.erase(victim);
        }
        if (image.sizeInBytes() <= MaxProjectionBytes)
            m_images.insert(key, image);
    }
    return image;
}
void LivePreview::clearViewResources() {
    // Explicitly release resources no longer used by a smaller projection;
    // QTextDocument's named resource cache is independent of visible content.
    for (const auto &name : m_resourceNames)
        m_view->addResource(QTextDocument::ImageResource, name, QVariant());
    m_resourceNames.clear();
    if (m_readMode) m_readImagePaths.clear();
}
void LivePreview::buildReadDocument(const QString &source) {
    // Import in a plain scratch document, not Qt Quick's resource-loading
    // document. Only sanitized preview: image names reach the visible item.
    ResourceFreeDocument rendered;
    rendered.setLayoutEnabled(false);
    m_view->setLayoutEnabled(false);
    const auto restoreLayout = qScopeGuard([this] { m_view->setLayoutEnabled(true); });
    rendered.setDefaultFont(m_view->defaultFont());
    QString markdown = source;
    QHash<QString, QImage> diagrams;
    QHash<QString, int> tableMarkers;
    QString tableNamespace;
    do { tableNamespace = QStringLiteral("preview:table/%1/").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)); }
    while (source.contains(tableNamespace));
    const auto &blocks = m_blockParser.blocks();
    qsizetype diagramBytes = 0;
    for (auto it = blocks.crbegin(); it != blocks.crend(); ++it) {
        const bool table = it->kind == PreviewBlocks::Kind::Table;
        if ((!table && it->kind != PreviewBlocks::Kind::Mermaid)
            || diagrams.size() >= MaxProjectionObjects || diagramBytes >= MaxProjectionBytes)
            continue;
        int tableId = 0;
        if (table) {
            const auto session = std::find_if(m_tables.cbegin(), m_tables.cend(), [&](const auto &t) { return t.start == it->start; });
            if (session == m_tables.cend() || !session->model || !session->model->rowCount()) continue;
            tableId = session->id;
        }
        const QImage image = imageFor(*it);
        if (image.isNull())
            continue;
        if (image.sizeInBytes() > MaxProjectionBytes - diagramBytes) {
            diagramBytes = MaxProjectionBytes;
            continue;
        }
        diagramBytes += image.sizeInBytes();
        const QString name = table ? tableNamespace + QString::number(tableId) : QStringLiteral("preview:mermaid/%1").arg(it->start);
        diagrams.insert(name, image);
        if (table) tableMarkers.insert(name, tableId);
        // One descending pass: neither table nor Mermaid source offsets drift.
        markdown.replace(it->start, it->end - it->start, QStringLiteral("![%1](%2)").arg(table ? "Table" : "Mermaid diagram", name));
    }
    const MarkdownReferences::Index references(source);
    markdown = MarkdownReferences::readMarkdown(markdown, &references);
    const QString originalReadMarkdown = markdown;
    QStringList hardBreaks;
    markdown = ListPreview::readHardBreaks(markdown, &hardBreaks);
    rendered.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    if (!ListPreview::restoreReadHardBreaks(rendered, hardBreaks))
        rendered.setMarkdown(originalReadMarkdown, QTextDocument::MarkdownDialectGitHub); // No sentinel may escape.
    markdown = originalReadMarkdown;
    // Resolve every image ourselves, including inline images. Qt Quick must
    // never receive an http(s) image URL from untrusted Markdown.
    QList<QPair<int, QTextImageFormat>> images;
    for (QTextBlock block = rendered.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.charFormat().isImageFormat())
                images.append({fragment.position(), fragment.charFormat().toImageFormat()});
        }
    }
    int index = 0;
    QList<QPair<QUrl, QImage>> resources;
    QHash<QString, QString> readImagePaths;
    QHash<QString, int> tableResources;
    qsizetype resourceBytes = 0;
    for (auto it = images.crbegin(); it != images.crend(); ++it) {
        const auto &entry = *it;
        QImage image;
        if (resources.size() < MaxProjectionObjects && resourceBytes < MaxProjectionBytes) {
            image = diagrams.contains(entry.second.name()) ? diagrams.value(entry.second.name())
                : PreviewBlocks::loadImage(resolve(entry.second.name()),
                                          qMax(80, m_viewItem->property("width").toInt() - 8));
            if (image.sizeInBytes() > MaxProjectionBytes - resourceBytes) {
                image = {};
                resourceBytes = MaxProjectionBytes;
            } else {
                resourceBytes += image.sizeInBytes();
            }
        }
        QTextCursor cursor(&rendered);
        cursor.setPosition(entry.first);
        cursor.setPosition(entry.first + 1, QTextCursor::KeepAnchor);
        if (image.isNull()) {
            cursor.insertText(QStringLiteral("[image unavailable]"));
        } else {
            QTextImageFormat format;
            format.setName(QStringLiteral("preview:read/%1").arg(index++));
            format.setWidth(image.width()); format.setHeight(image.height());
            if (!diagrams.contains(entry.second.name())) readImagePaths.insert(format.name(), entry.second.name());
            if (tableMarkers.contains(entry.second.name())) tableResources.insert(format.name(), tableMarkers.value(entry.second.name()));
            resources.append({QUrl(format.name()), image});
            cursor.insertImage(format);
        }
    }
    styleReadDocument(rendered, markdown, strongForeground(m_viewItem),
                      m_viewItem->property("color").value<QColor>(),
                      m_viewItem->property("previewAccent").value<QColor>(),
                      m_viewItem->property("previewDark").toBool());
    clearViewResources();
    m_readImagePaths = readImagePaths;
    const auto codeGroups = highlightReadCode(rendered, markdown, m_viewItem->property("previewDark").toBool());
    m_view->setHtml(rendered.toHtml());
    m_readTablePositions.clear();
    QSet<int> duplicateTables;
    for (auto block = m_view->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.charFormat().isImageFormat()) continue;
            const auto name = fragment.charFormat().toImageFormat().name();
            if (!tableResources.contains(name)) continue;
            const int id = tableResources.value(name);
            if (m_readTablePositions.contains(id) || fragment.length() != 1) duplicateTables.insert(id);
            else m_readTablePositions.insert(id, fragment.position());
        }
    }
    for (int id : duplicateTables) m_readTablePositions.remove(id);
    // Carry explicit fence boundaries across HTML only when block identities
    // survive. Adjacent same-language fences must remain separate panels.
    m_readCodeBlocks = rendered.blockCount() == m_view->blockCount()
        && rendered.toPlainText() == m_view->toPlainText() ? codeGroups : QList<QPair<int, int>>{};
    m_readCodeLanguages.clear();
    for (const auto &group : m_readCodeBlocks) {
        QString language;
        for (int n = group.first; n <= group.second; ++n) {
            const auto tag = rendered.findBlockByNumber(n).blockFormat()
                .stringProperty(QTextFormat::BlockCodeLanguage).simplified().section(QLatin1Char(' '), 0, 0);
            if (n == group.first) language = tag;
            else if (language != tag) { language.clear(); break; }
        }
        m_readCodeLanguages.append(language);
    }
    // Qt's HTML round-trip can lose the smallest heading's relative size.
    // Keep every heading on the same Markdown scale used by Edit highlighting.
    for (QTextBlock block = m_view->begin(); block.isValid(); block = block.next()) {
        const int level = block.blockFormat().headingLevel();
        if (level < 1 || level > 6)
            continue;
        QTextCursor heading(block);
        heading.select(QTextCursor::BlockUnderCursor);
        QTextCharFormat size;
        size.setProperty(QTextFormat::FontSizeAdjustment, 4 - level);
        heading.mergeCharFormat(size);
    }
    for (const auto &resource : resources) {
        m_view->addResource(QTextDocument::ImageResource, resource.first, resource.second);
        m_resourceNames.append(resource.first);
    }
    m_view->setLayoutEnabled(true);
    m_view->markContentsDirty(0, m_view->characterCount());
}
void LivePreview::completeProjectionLayout() {
    if (!m_view) return;
    auto *layout = m_view->documentLayout();
    QList<int> positions, invalidImages;
    if (!m_readMode) {
        // The bounded map identifies every Edit object: never scan all text
        // fragments to rediscover them after a one-character edit.
        for (const auto &s : m_segments) if (s.object) positions.append(s.viewStart);
    } else {
        layout->documentSize(); // Read import is an explicit whole-document operation.
        for (auto block = m_view->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (fragment.isValid() && fragment.charFormat().isImageFormat())
                    for (int i = 0; i < fragment.length(); ++i)
                        if (fragment.text()[i] == QChar::ObjectReplacementCharacter) positions.append(fragment.position() + i);
            }
    }
    for (int position : positions) {
        if (m_view->characterAt(position) != QChar::ObjectReplacementCharacter) { invalidImages.append(position); continue; }
        const auto block = m_view->findBlock(position);
        layout->blockBoundingRect(block);
        const auto *textLayout = block.layout();
        if (!textLayout || !textLayout->lineForTextPosition(position - block.position()).isValid()) invalidImages.append(position);
    }
    // Invalid layout must not reach QQuickTextNodeEngine. Keep a one-character
    // placeholder so the source/view mapping remains unchanged and recoverable.
    for (int position : invalidImages) {
        if (m_view->characterAt(position) == QChar(0x25a1)) continue;
        QTextCursor cursor(m_view);
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        cursor.insertText(QString(QChar(0x25a1)), QTextCharFormat());
    }
    m_invalidImageSlots = invalidImages;
    for (int position : invalidImages) if (position >= m_viewText.size() || m_viewText.at(position) != QChar(0x25a1)) recordViewChange(position, 1, QString(QChar(0x25a1)));
    scheduleCodeBlockPanels();
}
void LivePreview::styleEditLists() {
    const bool global = !m_listStyleValid || m_listFont != m_view->defaultFont()
        || m_listIndentWidth != m_view->indentWidth();
    if (!global && m_listDirtyFirst == std::numeric_limits<int>::max()) return;
    if (!m_listStyleValid) m_listParser.reset(m_view);
    else m_listParser.update(m_view, qMin(m_listDirtyFirst, m_view->blockCount() - 1));
    m_listFont = m_view->defaultFont(); m_listIndentWidth = m_view->indentWidth(); m_listStyleValid = true;
    m_listDirtyFirst = std::numeric_limits<int>::max();
    const int first = global ? 0 : m_listParser.changedFirst;
    const int last = global ? m_view->blockCount() - 1 : m_listParser.changedLast;
    QHash<int, ListPreview::Item> items;
    const auto &listItems = m_listParser.items;
    auto begin = std::lower_bound(listItems.cbegin(), listItems.cend(), first, [](const auto &item, int block) { return item.block < block; });
    for (auto it = begin; it != listItems.cend() && it->block <= last; ++it) items.insert(it->block, *it);
    const QFontMetricsF metrics(m_view->defaultFont());
    auto *layout = m_view->documentLayout();
    QSignalBlocker blocked(layout);
    QTextCursor batch(m_view);
    batch.beginEditBlock();
    QList<QTextBlock> changed;
    for (auto block = m_view->findBlockByNumber(first); block.isValid() && block.blockNumber() <= last; block = block.next()) {
        const auto it = items.constFind(block.blockNumber());
        const qreal margin = it == items.cend() ? 0 : m_view->indentWidth() * it->depth;
        const qreal indent = it == items.cend() ? 0 : -metrics.horizontalAdvance(block.text().left(it->prefix));
        auto format = block.blockFormat();
        const bool quote = it != items.cend() && it->quote;
        const int prefix = it == items.cend() || it->marker.isEmpty() ? 0 : it->prefix;
        const int quotePrefix = quote ? it->prefix : 0;
        const auto alignment = quote ? Qt::AlignJustify : Qt::AlignLeft;
        if (format.leftMargin() == margin && format.textIndent() == indent
            && format.intProperty(MarkdownHighlighter::ListPrefixProperty) == prefix
            && format.intProperty(MarkdownHighlighter::QuotePrefixProperty) == quotePrefix
            && format.alignment() == alignment) continue;
        format.setLeftMargin(margin); format.setTextIndent(indent); format.setAlignment(alignment);
        format.setProperty(MarkdownHighlighter::ListPrefixProperty, prefix);
        format.setProperty(MarkdownHighlighter::QuotePrefixProperty, quotePrefix);
        QTextCursor(block).setBlockFormat(format);
        m_overflowStyleDirty = true;
        changed.append(block);
    }
    batch.endEditBlock();
    blocked.unblock();
    for (const auto &block : changed) {
        m_highlighter->rehighlightBlock(block);
        m_view->markContentsDirty(block.position(), block.length());
    }
}
void LivePreview::updateListMarkers() {
    if (!m_view || !m_viewItem || m_view->pageSize() == QSizeF(0, 0)) return;
    if (m_pending && !m_readMode && !m_composing) return; // Keep the last projection until its queued replacement.
    QVariantList markers;
    if (!m_readMode && !m_composing) {
        const qreal left = m_viewItem->property("leftPadding").toReal() - m_view->documentMargin();
        qreal top = m_viewItem->property("topPadding").toReal();
        const qreal spare = m_viewItem->property("height").toReal() - top
            - m_viewItem->property("bottomPadding").toReal() - m_viewItem->property("contentHeight").toReal();
        const int alignment = m_viewItem->property("verticalAlignment").toInt();
        if (alignment == Qt::AlignVCenter) top += spare / 2;
        else if (alignment == Qt::AlignBottom) top += spare;
        const QFontMetricsF metrics(m_view->defaultFont());
        const auto visible = visibleBlocks();
        const auto &listItems = m_listParser.items;
        auto begin = std::lower_bound(listItems.cbegin(), listItems.cend(), visible.first,
            [](const auto &item, int block) { return item.block < block; });
        for (auto it = begin; it != listItems.cend() && it->block <= visible.second; ++it) {
            const auto &item = *it;
            if (item.marker.isEmpty()) continue;
            const auto block = m_view->findBlockByNumber(item.block);
            const auto text = block.text();
            if (!block.isValid() || item.prefix < 0 || item.prefix > text.size()) {
                if (!m_listMarkers.isEmpty()) { m_listMarkers.clear(); emit listMarkersChanged(); }
                return; // Stale metadata after native input/IME must never index text.
            }
            auto *layout = block.layout();
            if (!layout || !layout->lineCount()) return; // Retain last complete geometry.
            const auto line = layout->lineAt(0);
            int separator = item.prefix;
            while (separator > 0 && text[separator - 1].isSpace()) --separator;
            const qreal right = left + layout->position().x() + line.cursorToX(item.prefix)
                - metrics.horizontalAdvance(text.mid(separator, item.prefix - separator));
            markers.append(QVariantMap{{"text", item.marker}, {"right", right},
                {"baseline", top + layout->position().y() + line.y() + line.ascent()},
                {"start", sourcePosition(block.position())}, {"end", sourcePosition(block.position() + item.prefix)},
                {"searchReveal", MarkdownHighlighter::searchTouchesPrefix(text, item.prefix, m_searchQuery)}});
        }
    }
    if (markers != m_listMarkers) { m_listMarkers = markers; emit listMarkersChanged(); }
}
void LivePreview::scheduleCodeBlockPanels() {
    if (m_panelsPending) return;
    m_panelsPending = true;
    QTimer::singleShot(0, this, [this] {
        m_panelsPending = false;
        // Geometry is read-only and must follow wrapped IME preedit as well;
        // source/projection rebuilding remains deferred until composition ends.
        if (!m_guard) { updateCodeBlockPanels(); updateListMarkers(); }
    });
}
void LivePreview::styleCodeBlocks() {
    if (!m_view || m_view->pageSize() == QSizeF(0, 0)) return;
    QList<QPair<int, int>> groups = m_readCodeBlocks;
    if (!m_readMode) groups.clear();
    if (!m_readMode) for (const auto &fence : m_blockParser.fences()) {
        bool collapsed = false;
        for (const auto &s : m_segments) if (s.object && fence.first >= s.sourceStart && fence.first < s.sourceEnd) { collapsed = true; break; }
        if (collapsed) continue;
        const auto first = m_view->findBlock(viewPosition(fence.first));
        auto last = m_view->findBlock(viewPosition(fence.second));
        if (!first.isValid() || !last.isValid()) continue;
        const QString opening = first.text().trimmed();
        if (fence.second >= m_projectionSource.size() - 1 && !opening.isEmpty()
            && !MarkdownHighlighter::closesFence(last.text(), MarkdownHighlighter::openingFenceLength(first.text()), opening.at(0))) last = m_view->lastBlock();
        groups.append({first.blockNumber(), last.blockNumber()});
    }
    if (!m_readMode) for (const auto &table : m_tables) {
        const bool collapsed = std::any_of(m_segments.cbegin(), m_segments.cend(), [&](const auto &s) { return s.object && s.sourceStart == table.start; });
        if (!collapsed) groups.append({m_view->findBlock(viewPosition(table.start)).blockNumber(), m_view->findBlock(viewPosition(table.end - 1)).blockNumber()});
    }
    std::sort(groups.begin(), groups.end());
    QList<QTextBlock> endpoints;
    for (const auto &group : groups) { endpoints.append(m_view->findBlockByNumber(group.first)); endpoints.append(m_view->findBlockByNumber(group.second)); }
    const qreal lineHeight = QFontMetricsF(qvariant_cast<QFont>(m_viewItem->property("font"))).height();
    if (!m_codeSpacingDirty && endpoints == m_codeEndpoints && lineHeight == m_codeLineHeight) { m_editCodeGroups = groups; return; }
    m_codeSpacingDirty = false; m_codeEndpoints = endpoints; m_codeLineHeight = lineHeight;
    m_editCodeGroups = groups;
    const qreal paddingY = std::ceil(lineHeight / 2);
    const qreal paddingTop = paddingY + 24; // Stable space for the hover toolbar.
    const qreal gap = std::ceil(lineHeight / 3);
    // Reserve visual-only paragraph space so padded decorations cannot cover
    // neighboring prose or each other. The Markdown and its undo stay untouched.
    QScopedValueRollback<bool> guard(m_guard, true);
    auto *layout = m_view->documentLayout();
    QSignalBlocker layoutSignals(layout);
    QTextCursor batch(m_view);
    batch.beginEditBlock();
    bool spacingChanged = false;
    // Qt ignores the first block's top margin; reserve document-edge space in
    // the existing root frame as well (no new frames or source positions).
    auto frame = m_view->rootFrame()->frameFormat();
    const qreal edgeTop = !groups.isEmpty() && groups.first().first == 0
        ? qMax(m_view->documentMargin(), paddingTop + gap) : m_view->documentMargin();
    const qreal edgeBottom = !groups.isEmpty() && groups.last().second == m_view->blockCount() - 1
        ? qMax(m_view->documentMargin(), paddingY + gap + 14) : m_view->documentMargin();
    if (frame.topMargin() != edgeTop || frame.bottomMargin() != edgeBottom) {
        frame.setTopMargin(edgeTop); frame.setBottomMargin(edgeBottom);
        m_view->rootFrame()->setFrameFormat(frame);
        spacingChanged = true;
    }
    decltype(m_panelMargins) wanted;
    QHash<int, int> indexes;
    for (int i = 0; i < groups.size(); ++i) {
        const auto &group = groups.at(i);
        for (bool first : {true, false}) {
            const auto block = m_view->findBlockByNumber(first ? group.first : group.second);
            const bool adjacent = first ? i > 0 && groups[i - 1].second + 1 == group.first
                : i + 1 < groups.size() && group.second + 1 == groups[i + 1].first;
            const qreal space = (first ? paddingTop : paddingY + 14) + gap + (adjacent ? (first ? paddingY + 14 : paddingTop) : 0);
            if (!indexes.contains(block.blockNumber())) { indexes.insert(block.blockNumber(), wanted.size()); wanted.append({block, {0, 0}}); }
            auto &margins = wanted[indexes.value(block.blockNumber())].second;
            (first ? margins.first : margins.second) = space;
        }
    }
    auto changedMargins = wanted;
    for (const auto &old : m_panelMargins) if (old.first.isValid() && !indexes.contains(old.first.blockNumber()))
        changedMargins.append({old.first, {0, 0}});
    int dirtyFirst = m_view->characterCount(), dirtyEnd = 0;
    for (const auto &entry : changedMargins) {
        if (!entry.first.isValid()) continue;
        auto format = entry.first.blockFormat();
        if (format.topMargin() == entry.second.first && format.bottomMargin() == entry.second.second) continue;
        format.setTopMargin(entry.second.first); format.setBottomMargin(entry.second.second);
        QTextCursor(entry.first).setBlockFormat(format);
        dirtyFirst = qMin(dirtyFirst, entry.first.position());
        dirtyEnd = qMax(dirtyEnd, entry.first.position() + entry.first.length());
    }
    m_panelMargins = wanted;
    batch.endEditBlock(); layoutSignals.unblock();
    if (spacingChanged) m_view->markContentsDirty(0, m_view->characterCount());
    else if (dirtyEnd > dirtyFirst) m_view->markContentsDirty(dirtyFirst, dirtyEnd - dirtyFirst);
}
QPair<int, int> LivePreview::visibleBlocks() const {
    if (m_viewportHeight < 0) return {0, m_view->blockCount() - 1};
    const qreal overscan = m_viewportHeight / 2 + 128;
    const qreal top = m_viewItem->property("topPadding").toReal();
    auto *layout = m_view->documentLayout();
    const int a = layout->hitTest(QPointF(m_view->documentMargin(), qMax(qreal(0), m_viewportTop - top - overscan)), Qt::FuzzyHit);
    const int b = layout->hitTest(QPointF(m_view->documentMargin(), m_viewportTop - top + m_viewportHeight + overscan), Qt::FuzzyHit);
    return {a < 0 ? 0 : m_view->findBlock(a).blockNumber(), b < 0 ? m_view->blockCount() - 1 : m_view->findBlock(b).blockNumber()};
}
void LivePreview::updateCodeBlockPanels() {
    if (!m_view || !m_viewItem || m_view->pageSize() == QSizeF(0, 0)
        || (m_pending && !m_guard) || (!m_readMode && !m_viewSyncedSource)) return;
    const auto &groups = m_readMode ? m_readCodeBlocks : m_editCodeGroups;
    const qreal lineHeight = QFontMetricsF(qvariant_cast<QFont>(m_viewItem->property("font"))).height();
    const qreal paddingY = std::ceil(lineHeight / 2), paddingTop = paddingY + 24;
    const qreal paddingX = std::ceil(lineHeight * 0.75);
    auto *layout = m_view->documentLayout();
    const auto visible = visibleBlocks();
    const qreal left = m_viewItem->property("leftPadding").toReal() - m_view->documentMargin();
    // QTextLayout positions already include the document margin. Qt Quick's
    // vertical text origin adds padding/alignment, not a margin subtraction.
    qreal top = m_viewItem->property("topPadding").toReal();
    const qreal spare = m_viewItem->property("height").toReal()
        - m_viewItem->property("topPadding").toReal() - m_viewItem->property("bottomPadding").toReal()
        - m_viewItem->property("contentHeight").toReal();
    const int alignment = m_viewItem->property("verticalAlignment").toInt();
    if (alignment == Qt::AlignVCenter) top += spare / 2;
    else if (alignment == Qt::AlignBottom) top += spare;
    QVariantList panels;
    int cachedRows = 0, cachedFormats = 0;
    for (const auto &overflow : m_overflows) {
        cachedRows += overflow.rows.size();
        for (const auto &row : overflow.rows) cachedFormats += row.formats.size();
    }
    QSet<int> cachedGroups;
    const QFont widthFont = m_view->defaultFont();
    for (int i = 0; i < groups.size(); ++i) {
        const auto &group = groups.at(i);
        if (group.second < visible.first) continue;
        if (group.first > visible.second) break;
        const auto first = m_view->findBlockByNumber(group.first);
        const auto last = m_view->findBlockByNumber(group.second);
        if (!first.isValid() || !last.isValid()) continue;
        const QRectF bounds = layout->blockBoundingRect(first);
        layout->blockBoundingRect(last);
        const auto *a = first.layout(), *b = last.layout();
        // Do not publish a partial/empty replacement model during relayout:
        // that destroys otherwise valid background delegates. A subsequent
        // layout notification will supply the completed geometry.
        if (!a || !b || !a->lineCount() || !b->lineCount()) return;
        const qreal y = top + a->position().y() + a->lineAt(0).y() - paddingTop;
        const auto endLine = b->lineAt(b->lineCount() - 1);
        const qreal bottom = top + b->position().y() + endLine.y() + endLine.height() + paddingY + 14;
        const qreal x = left + bounds.left() - paddingX;
        QString language = m_readMode ? m_readCodeLanguages.value(i)
            : first.text().trimmed().mid(MarkdownHighlighter::openingFenceLength(first.text()))
                  .simplified().section(QLatin1Char(' '), 0, 0);
        int tableId = 0;
        if (!m_readMode) for (const auto &table : m_tables) if (viewPosition(table.start) == first.position()) { tableId = table.id; language = QStringLiteral("markdown"); break; }
        auto &overflow = overflowFor(first, last);
        qreal contentWidth = m_viewItem->property("width").toReal() - 8;
        // Cache intrinsic widths, never rectangles/line objects. Keep the exact
        // widest offscreen row; eviction/composition falls back to native layout.
        const int count = group.second - group.first + 1;
        const int oldCount = overflow.rows.size();
        const bool cache = tableId && !m_composing && count <= 8192 - cachedRows + oldCount;
        if (!cache || overflow.widthFont != widthFont || oldCount != count) {
            for (const auto &row : overflow.rows) cachedFormats -= row.formats.size();
            overflow.rows.clear();
        }
        cachedRows += (cache ? count : 0) - oldCount;
        overflow.widthFont = widthFont;
        if (cache) { overflow.rows.resize(count); cachedGroups.insert(overflow.id); }
        int row = 0;
        for (auto block = first; block.isValid() && block.blockNumber() <= group.second; block = block.next(), ++row) {
            auto *native = block.layout();
            const auto formats = native ? native->formats() : QList<QTextLayout::FormatRange>();
            const auto option = native ? native->textOption() : QTextOption();
            auto *entry = cache ? &overflow.rows[row] : nullptr;
            if (entry) cachedFormats -= entry->formats.size();
            const bool bounded = entry && formats.size() <= 256 && cachedFormats + formats.size() <= 65536;
            const bool reuse = bounded && entry->block == block && entry->revision == block.revision()
                && entry->blockFormat == block.blockFormat() && entry->charFormat == block.charFormat() && entry->formats == formats
                && entry->option.flags() == option.flags() && entry->option.alignment() == option.alignment()
                && entry->option.textDirection() == option.textDirection() && entry->option.wrapMode() == option.wrapMode()
                && entry->option.tabStopDistance() == option.tabStopDistance() && entry->option.tabs() == option.tabs()
                && entry->option.useDesignMetrics() == option.useDesignMetrics();
            qreal width = reuse ? entry->width : 0;
            if (!reuse) {
                ++m_overflowMeasurements; layout->blockBoundingRect(block); native = block.layout();
                if (native) for (int n = 0; n < native->lineCount(); ++n) width = qMax(width, native->lineAt(n).naturalTextWidth() + 8);
                if (bounded && native && native->lineCount() && native->formats().size() <= 256 && cachedFormats + native->formats().size() <= 65536)
                    *entry = {block, block.revision(), block.blockFormat(), block.charFormat(), native->textOption(), native->formats(), width};
                else if (entry) *entry = {};
            }
            if (entry) cachedFormats += entry->formats.size();
            contentWidth = qMax(contentWidth, width);
        }
        overflow.width = contentWidth;
        if (overflow.offset > qMax(qreal(0), contentWidth - (m_viewItem->property("width").toReal() - 8)) + 0.5) {
            scrollBlock(overflow.id, overflow.offset); return;
        }
        const int caret = m_viewItem->property("cursorPosition").toInt();
        if (m_revealOverflowCaret && !m_readMode && m_viewItem->property("activeFocus").toBool()
            && caret >= first.position() && caret < last.position() + last.length()) {
            const qreal cx = visualCursorRectangle(m_viewItem->property("cursorRectangle").toRectF()).x();
            const qreal viewport = m_viewItem->property("width").toReal() - 8;
            const qreal offset = qBound(qreal(0), overflow.offset + (cx < 4 ? cx - 4 : cx > viewport ? cx - viewport : 0), qMax(qreal(0), contentWidth - viewport));
            if (qAbs(offset - overflow.offset) > 0.5) { m_revealOverflowCaret = false; scrollBlock(overflow.id, offset); return; }
        }
        panels.append(QVariantMap{{"id", overflow.id}, {"contentWidth", contentWidth}, {"viewportWidth", m_viewItem->property("width").toReal() - 8}, {"tableId", tableId},
            {"language", language.isEmpty() ? QStringLiteral("text") : language},
            {"firstBlock", group.first}, {"lastBlock", group.second},
            {"readMode", m_readMode},
            {"x", x}, {"y", y},
            {"width", qMax(qreal(0), m_viewItem->property("width").toReal()
                         - m_viewItem->property("rightPadding").toReal() + paddingX - x)},
            {"height", bottom - y}});
    }
    for (auto &overflow : m_overflows) if (!cachedGroups.contains(overflow.id)) overflow.rows.clear();
    m_revealOverflowCaret = false;
    m_panelsSource = m_projectionSource;
    m_panelsView = m_viewText;
    updateTablePanels();
    if (panels != m_codeBlockPanels) {
        m_codeBlockPanels = panels;
        emit codeBlockPanelsChanged();
    }
}
void LivePreview::updateSourceSnapshot() {
    if (!m_sourceDirty) return;
    const QString source = documentText(m_source);
    if (source != m_projectionSource || m_forceReset) {
        int first = 0, suffix = 0;
        while (first < source.size() && first < m_projectionSource.size()
               && source[first] == m_projectionSource[first]) ++first;
        while (suffix < source.size() - first && suffix < m_projectionSource.size() - first
               && source[source.size() - suffix - 1] == m_projectionSource[m_projectionSource.size() - suffix - 1]) ++suffix;
        shiftTables(first, m_projectionSource.size() - first - suffix, source.size() - first - suffix);
        if (m_forceReset) m_blockParser.reset(source);
        else m_blockParser.edit(source, first, m_projectionSource.size() - first - suffix, source.size() - first - suffix);
        m_projectionSource = source; ++m_sourceGeneration;
        m_viewSyncedSource = false;
    }
    m_sourceDirty = false;
}
void LivePreview::styleProjection(bool fontChanged) {
    const bool dark = m_viewItem->property("previewDark").toBool();
    if (m_highlighter->document() != m_view) m_highlighter->setDocument(m_view);
    // Equality-checked setter also refreshes the detached Edit highlighter
    // after a theme change that was rendered while staying in Read.
    m_highlighter->setTheme(dark, m_viewItem->property("previewBackground").value<QColor>().name(),
        strongForeground(m_viewItem).name(), m_viewItem->property("previewAccent").value<QColor>().name(),
        dark ? QStringLiteral("#252525") : QStringLiteral("#eeeeee"));
    styleEditLists();
    m_highlighter->setSearch(m_searchQuery, m_currentMatch < 0 ? -1 : viewPosition(m_currentMatch));
    m_highlighter->setCaret(viewPosition(m_caret));
    m_highlighter->setSelection(viewPosition(m_anchor), viewPosition(m_caret));
    if (fontChanged) m_highlighter->refreshFont();
    refreshSpelling();
    m_styleDirty = false;
}
void LivePreview::finishProjectionChange() {
    if (!m_projectionChanging) return;
    // Structural changes/mode imports need complete extents before the QML
    // scroll anchor is restored. Ordinary native typing never enters here.
    m_view->documentLayout()->documentSize();
    updateCodeBlockPanels(); updateListMarkers();
    m_projectionChanging = false;
    emit projectionChanged();
    // TextEdit can retain glyph nodes from a previous document or collapsed
    // table even though the current QTextDocument/layout is correct. Dirtying
    // the layout alone does not clear that scenegraph cache. Invalidate after
    // attachment, styling and QML scroll restoration, queued so their updates
    // cannot downgrade UpdateAll. Ordinary native typing never enters here.
    // Private Qt slot: retain native pixel regressions when upgrading Qt.
    if (!QMetaObject::invokeMethod(m_viewItem, "q_invalidate", Qt::QueuedConnection))
        qWarning("TextEdit scenegraph invalidation hook unavailable");
}
void LivePreview::rebuild() {
    if (!m_source || !m_view || m_guard || m_composing) return;
    m_pending = false;
    QScopedValueRollback<bool> guard(m_guard, true);
    updateSourceSnapshot();
    syncTables();
    // QTextDocument is edited directly, bypassing TextEdit::setText(), which
    // normally enables Qt Quick's visible-block rendering for large text.
    // Use the public item flag so scrolling invalidates the rendered region
    // without creating glyph nodes for the entire native document.
    if (auto *item = qobject_cast<QQuickItem *>(m_viewItem.data()))
        item->setFlag(QQuickItem::ItemObservesViewport, m_projectionSource.size() > 10000);
    m_anchor = qBound(0, m_anchor, int(m_projectionSource.size()));
    m_caret = qBound(0, m_caret, int(m_projectionSource.size()));
    const QFont font = qvariant_cast<QFont>(m_viewItem->property("font"));
    const bool fontChanged = m_view->defaultFont() != font;
    if (fontChanged) { m_view->setDefaultFont(font); m_styleDirty = true; m_renderDirty = true; }
    m_mermaid->setGeneration(QString::number(m_sourceGeneration) + QLatin1Char('|') + QString::number(m_viewItem->property("width").toInt())
                            + m_viewItem->property("previewDark").toString());
    if (m_readMode) {
        const QString styleKey = font.toString() + QLatin1Char('|')
            + m_viewItem->property("width").toString() + m_viewItem->property("color").toString()
            + m_viewItem->property("previewForeground").toString()
            + m_viewItem->property("previewBackground").toString()
            + m_viewItem->property("previewAccent").toString()
            + m_viewItem->property("previewDark").toString() + QString::number(m_resourceGeneration);
        if (!m_renderedRead || !m_viewSyncedSource || m_readGeneration != m_sourceGeneration
            || m_readStyleKey != styleKey || !m_invalidImageSlots.isEmpty()) {
            if (!m_projectionChanging) { m_projectionChanging = true; emit projectionAboutToChange(); }
            buildReadDocument(m_projectionSource);
            m_panelMargins.clear(); m_codeEndpoints.clear(); m_codeSpacingDirty = true;
            m_viewText = documentText(m_view);
            m_segments.clear(); m_renderedRead = true; m_viewSyncedSource = true;
            m_styleDirty = false; m_renderDirty = false; m_forceReset = false;
            styleCodeBlocks();
            styleOverflow();
            completeProjectionLayout();
            m_readGeneration = m_sourceGeneration; m_readStyleKey = styleKey;
        }
        auto *quick = qvariant_cast<QQuickTextDocument *>(m_viewItem->property("textDocument"));
        if (quick->textDocument() != m_view) quick->setTextDocument(m_view);
        updateCodeBlockPanels(); updateListMarkers();
        finishProjectionChange();
        emit projectionReady();
        return;
    }
    syncEditProjection();
    styleProjection(fontChanged);
    styleCodeBlocks();
    styleOverflow();
    if (!m_resourceNames.isEmpty()) completeProjectionLayout();
    select(m_viewItem, viewPosition(m_anchor), viewPosition(m_caret));
    finishProjectionChange();
    emit projectionReady();
    // No documentSize(), full fragment walk, projection anchoring, or native
    // document replacement on an ordinary keystroke/caret move.
    scheduleCodeBlockPanels();
}
void LivePreview::syncEditProjection(bool revealBoundary, bool preparingEdit) {
    const QString &source = m_projectionSource;
    struct Replacement { PreviewBlocks::Block block; QImage image; bool footnote = false; };
    QList<Replacement> replacements;
    const int low = qMin(m_anchor, m_caret), high = qMax(m_anchor, m_caret);
    qsizetype projectionBytes = 0;
    for (const auto &block : m_blockParser.blocks()) {
        if (replacements.size() >= MaxProjectionObjects || projectionBytes >= MaxProjectionBytes)
            break;
        // Include both endpoints: typing/deleting against an object first
        // exposes Markdown rather than treating the whole block as one char.
        if (block.kind == PreviewBlocks::Kind::Table) {
            const auto table = std::find_if(m_tables.cbegin(), m_tables.cend(), [&](const auto &t) { return t.start == block.start; });
            if (table == m_tables.cend() || table->source || !table->model || !table->model->rowCount()
                || (low != high && low < block.end && high > block.start)
                || (m_currentMatch >= block.start && m_currentMatch < block.end)) continue;
        } else if (low <= block.end + (revealBoundary ? 1 : 0) && high >= block.start - (revealBoundary ? 1 : 0))
            continue;
        const QImage image = imageFor(block);
        if (!image.isNull()) {
            if (image.sizeInBytes() > MaxProjectionBytes - projectionBytes)
                break;
            projectionBytes += image.sizeInBytes();
            replacements.append({block, image});
        }
    }
    if (m_noteGeneration != m_sourceGeneration) {
        MarkdownReferences::Index next(source.contains(QStringLiteral("[^")) ? source : QString());
        if (next.numbers != m_noteReferences.numbers) m_renderDirty = true;
        m_noteReferences = std::move(next);
        m_noteGeneration = m_sourceGeneration;
    }
    for (const auto &ref : m_noteReferences.occurrences) {
        if (!ref.footnote) continue;
        if (replacements.size() >= MaxProjectionObjects || projectionBytes >= MaxProjectionBytes) break;
        const bool active = preparingEdit
            ? low <= ref.end + (revealBoundary ? 1 : 0) && high >= ref.start - (revealBoundary ? 1 : 0)
            : low == high ? low > ref.start && low < ref.end : low < ref.end && high > ref.start;
        const bool searched = m_currentMatch >= 0 && m_currentMatch < ref.end
            && m_currentMatch + m_searchQuery.size() > ref.start;
        if (active || searched) continue;
        // Source-visible table panels copy their literal text; never insert an
        // inline object into that Markdown (collapsed tables own the whole range).
        if (std::any_of(m_blockParser.blocks().cbegin(), m_blockParser.blocks().cend(), [&](const auto &block) {
            return block.kind == PreviewBlocks::Kind::Table && ref.start < block.end && ref.end > block.start;
        })) continue;
        if (std::any_of(replacements.cbegin(), replacements.cend(), [&](const auto &r) {
            return ref.start < r.block.end && ref.end > r.block.start;
        })) continue;
        QFont font = m_view->defaultFont();
        if (font.pixelSize() > 0) font.setPixelSize(qMax(1, qRound(font.pixelSize() * .7)));
        else font.setPointSizeF(font.pointSizeF() * .7);
        const QColor accent = m_viewItem->property("previewAccent").value<QColor>();
        const QString number = QString::number(m_noteReferences.numbers.value(ref.label));
        const QString key = QStringLiteral("footnote/") + number + font.toString() + accent.name(QColor::HexArgb);
        QImage image = m_images.value(key);
        if (image.isNull()) {
            const QFontMetricsF metrics(font);
            const QSize size(qMax(1, int(std::ceil(metrics.horizontalAdvance(number)))) + 2, int(std::ceil(metrics.height())));
            image = QImage(size * 2, QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(2);
            image.fill(Qt::transparent);
            QPainter painter(&image); painter.setFont(font); painter.setPen(accent);
            painter.drawText(QPointF(1, metrics.ascent()), number);
            painter.end();
            qsizetype cachedBytes = image.sizeInBytes();
            for (const auto &cached : m_images) cachedBytes += cached.sizeInBytes();
            if (m_images.size() < MaxProjectionObjects && cachedBytes <= MaxProjectionBytes)
                m_images.insert(key, image);
        }
        if (image.sizeInBytes() > MaxProjectionBytes - projectionBytes) break;
        projectionBytes += image.sizeInBytes();
        PreviewBlocks::Block block; block.start = ref.start; block.end = ref.end;
        replacements.append({block, image, true});
    }
    std::sort(replacements.begin(), replacements.end(), [](const auto &a, const auto &b) { return a.block.start < b.block.start; });
    QList<QPair<int, int>> ranges, oldRanges;
    for (const auto &r : replacements) ranges.append({r.block.start, r.block.end});
    for (const auto &s : m_segments) if (s.object) oldRanges.append({s.sourceStart, s.sourceEnd});
    const auto nextSegments = mappedSegments(ranges, source.size());
    const bool replaceText = m_forceReset || !m_viewSyncedSource || ranges != oldRanges;
    if (!replaceText && !m_renderDirty && m_invalidImageSlots.isEmpty()) return;
    if (replaceText && !m_projectionChanging) { m_projectionChanging = true; emit projectionAboutToChange(); }
    QString desired;
    if (replaceText) {
        int at = 0;
        for (const auto &r : ranges) {
            desired += source.mid(at, r.first - at); desired += QChar::ObjectReplacementCharacter; at = r.second;
        }
        desired += source.mid(at);
    }
    const bool reset = m_forceReset || m_renderedRead;
    if (reset) {
        ++m_fullResets;
        m_highlighter->setDocument(nullptr);
        clearViewResources(); m_view->clear(); m_viewText.clear(); m_panelMargins.clear();
        m_codeEndpoints.clear(); m_codeSpacingDirty = true; m_overflowStyleDirty = true; m_invalidImageSlots.clear();
        m_listStyleValid = false; m_listDirtyFirst = 0;
    }
    QList<QTextImageFormat> formats;
    QList<QUrl> resources;
    for (const auto &r : replacements) {
        QTextImageFormat format;
        format.setName(QStringLiteral("preview:edit/%1").arg(r.image.cacheKey()));
        format.setWidth(r.image.width() / r.image.devicePixelRatio());
        format.setHeight(r.image.height() / r.image.devicePixelRatio());
        if (r.footnote) format.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
        const QUrl url(format.name());
        m_view->addResource(QTextDocument::ImageResource, url, r.image);
        formats.append(format); resources.append(url);
    }
    struct Patch { int from, removed, nextFrom, nextLength; };
    QList<Patch> patches;
    if (replaceText && m_viewSyncedSource && !reset) {
        // Only changed objects are replaced; disjoint reveal/collapse regions
        // never destroy the native prose blocks between them.
        QList<QPair<int, int>> changed;
        for (const auto &r : oldRanges) if (!ranges.contains(r)) changed.append(r);
        for (const auto &r : ranges) if (!oldRanges.contains(r)) changed.append(r);
        std::sort(changed.begin(), changed.end());
        QList<QPair<int, int>> merged;
        for (const auto &r : changed) {
            if (!merged.isEmpty() && r.first <= merged.last().second) merged.last().second = qMax(merged.last().second, r.second);
            else merged.append(r);
        }
        for (const auto &r : merged) {
            const int from = toView(m_segments, r.first), end = toView(m_segments, r.second);
            const int next = toView(nextSegments, r.first), nextEnd = toView(nextSegments, r.second);
            patches.append({from, end - from, next, nextEnd - next});
        }
    } else if (replaceText) {
        int first = 0, suffix = 0;
        while (first < desired.size() && first < m_viewText.size() && desired[first] == m_viewText[first]) ++first;
        while (suffix < desired.size() - first && suffix < m_viewText.size() - first
               && desired[desired.size() - suffix - 1] == m_viewText[m_viewText.size() - suffix - 1]) ++suffix;
        patches.append({first, int(m_viewText.size()) - first - suffix, first, int(desired.size()) - first - suffix});
    }
    QTextCursor batch(m_view); batch.beginEditBlock();
    for (auto p = patches.crbegin(); p != patches.crend(); ++p) {
        QTextCursor cursor(m_view); cursor.setPosition(p->from);
        cursor.setPosition(p->from + p->removed, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        int at = p->nextFrom;
        const int end = at + p->nextLength;
        int image = 0;
        for (const auto &s : nextSegments) if (s.object) {
            if (s.viewStart >= at && s.viewStart < end) {
                cursor.insertText(desired.mid(at, s.viewStart - at), QTextCharFormat());
                cursor.insertImage(formats.at(image)); at = s.viewEnd;
            }
            ++image;
        }
        cursor.insertText(desired.mid(at, end - at), QTextCharFormat());
        recordViewChange(p->from, p->removed, desired.mid(p->nextFrom, p->nextLength));
        ++m_projectionPatches;
    }
    m_segments = nextSegments;
    // An image may change without changing its one-character placeholder.
    int image = 0;
    for (const auto &s : m_segments) if (s.object) {
        QTextCursor cursor(m_view); cursor.setPosition(s.viewStart);
        cursor.setPosition(s.viewEnd, QTextCursor::KeepAnchor);
        if (m_view->characterAt(s.viewStart) != QChar::ObjectReplacementCharacter) {
            cursor.insertImage(formats.at(image));
            recordViewChange(s.viewStart, 1, QString(QChar::ObjectReplacementCharacter));
        } else if (cursor.charFormat() != formats.at(image)) cursor.setCharFormat(formats.at(image));
        ++image;
    }
    batch.endEditBlock();
    for (const auto &url : m_resourceNames) if (!resources.contains(url))
        m_view->addResource(QTextDocument::ImageResource, url, QVariant());
    m_resourceNames = resources;
    m_forceReset = false; m_renderedRead = false; m_viewSyncedSource = true; m_renderDirty = false;
    if (!patches.isEmpty() || !ranges.isEmpty()) completeProjectionLayout();
}
void LivePreview::prepareEdit(bool revealBoundary) {
    if (m_readMode || !m_source)
        return;
    // Flush before another native input event sees a stale mapping.
    if (m_pending)
        rebuild();
    const int low = qMin(m_anchor, m_caret), high = qMax(m_anchor, m_caret);
    const int padding = revealBoundary ? 1 : 0;
    for (auto &table : m_tables) if (low <= table.end + padding && high >= table.start - padding) table.source = true;
    bool boundary = false;
    for (const auto &segment : m_segments) {
        if (segment.object && low <= segment.sourceEnd + padding && high >= segment.sourceStart - padding) {
            boundary = true;
            break;
        }
    }
    if (boundary) {
        QScopedValueRollback<bool> guard(m_guard, true);
        syncEditProjection(revealBoundary, true); // Ordinary neighboring prose must not expand a large object.
        styleEditLists(); styleCodeBlocks(); styleOverflow();
        select(m_viewItem, viewPosition(m_anchor), viewPosition(m_caret));
        completeProjectionLayout(); finishProjectionChange();
    }
}
QString LivePreview::linkAtPoint(qreal x, qreal y) const {
    if (m_readMode || m_pending || m_composing || !m_resolver) return {};
    const int at = characterAtPoint(x, y);
    if (at < 0) return {};
    QString result;
    QMetaObject::invokeMethod(m_resolver, "linkUrlAt", Qt::DirectConnection,
        Q_RETURN_ARG(QString, result), Q_ARG(int, sourcePosition(at)));
    return result;
}
int LivePreview::footnoteViewPosition(const QString &url) const {
    if (!m_readMode || !m_view || !url.startsWith(QStringLiteral("write-footnote:"))) return -1;
    for (auto block = m_view->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().anchorNames().contains(url)) return it.fragment().position();
    return -1;
}
QString LivePreview::imagePathAtPoint(qreal x, qreal y) const {
    const int position = characterAtPoint(x, y);
    return position < 0 ? QString() : imagePathAt(position);
}
QString LivePreview::imagePathAt(int position) const {
    if (!m_source || !m_view || m_pending || m_composing || position < 0 || position >= m_view->characterCount() - 1) return {};
    if (m_readMode) {
        if (position < 0 || position >= m_view->characterCount() - 1
            || m_view->characterAt(position) != QChar::ObjectReplacementCharacter) return {};
        QTextCursor cursor(m_view);
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        return m_readImagePaths.value(cursor.charFormat().toImageFormat().name());
    }
    const int sourceAt = sourcePosition(position);
    for (const auto &block : m_blockParser.blocks()) {
        if (block.kind == PreviewBlocks::Kind::Image
            && sourceAt >= block.start && sourceAt < block.end)
            return block.imagePath;
    }
    return {};
}
bool LivePreview::copyCodeBlock(int panelIndex) {
    if (!m_source || !m_view || m_composing || m_view->pageSize() == QSizeF(0, 0)
        || panelIndex < 0 || panelIndex >= m_codeBlockPanels.size()) return false;
    const auto panel = m_codeBlockPanels.at(panelIndex).toMap();
    // Document revisions also change during highlighting/selection. Compare
    // text snapshots only when copying, not on each hover or geometry update.
    if (documentText(m_source) != m_panelsSource || documentText(m_view) != m_panelsView
        || panel.value("readMode").toBool() != m_readMode) return false;
    auto first = m_view->findBlockByNumber(panel.value("firstBlock").toInt());
    auto last = m_view->findBlockByNumber(panel.value("lastBlock").toInt());
    if (!first.isValid() || !last.isValid() || first.blockNumber() > last.blockNumber()) return false;
    if (!m_readMode && !panel.value("tableId").toInt()) {
        const int length = MarkdownHighlighter::openingFenceLength(first.text());
        if (!length) return false;
        const QChar marker = first.text().trimmed().at(0);
        if (last != first && MarkdownHighlighter::closesFence(last.text(), length, marker))
            last = last.previous();
        first = first.next();
    }
    QStringList lines;
    for (auto block = first; block.isValid() && last.isValid()
         && block.blockNumber() <= last.blockNumber(); block = block.next())
        lines.append(block.text());
    QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    return true;
}

bool LivePreview::copyReadSelection(bool asHtml) {
    if (!m_readMode || !m_source || !m_view || !m_viewItem || !m_resolver
        || documentText(m_source) != m_projectionSource) return false;
    const int start = m_viewItem->property("selectionStart").toInt();
    const int end = m_viewItem->property("selectionEnd").toInt();
    if (start >= end) return false;
    QHash<QString, QString> tables;
    for (const auto &session : m_tables) {
        const int position = m_readTablePositions.value(session.id, -1);
        if (position < start || position >= end || !session.model) continue;
        QTextCursor slot(m_view); slot.setPosition(position); slot.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        if (slot.charFormat().isImageFormat()) tables.insert(slot.charFormat().toImageFormat().name(), session.markdown);
    }
    if (tables.isEmpty()) return false;
    ResourceFreeDocument selected;
    selected.setResourceProvider([](const QUrl &) { return QVariant(); }); selected.setLayoutEnabled(false);
    QTextCursor selection(m_view); selection.setPosition(start); selection.setPosition(end, QTextCursor::KeepAnchor);
    QTextCursor(&selected).insertFragment(selection.selection());
    QList<QPair<int, QString>> clipboardSlots;
    // Locate by exact internal resource identity, never by source/native offsets:
    // inserting a fragment that starts inside a native fallback table can shift positions.
    for (QTextBlock block = selected.begin(); block.isValid(); block = block.next()) for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment(); if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) continue;
        const auto name = fragment.charFormat().toImageFormat().name();
        if (tables.contains(name)) clipboardSlots.append({fragment.position(), tables.value(name)});
        else selected.addResource(QTextDocument::ImageResource, QUrl(name), m_view->resource(QTextDocument::ImageResource, QUrl(name)));
    }
    for (auto it = clipboardSlots.crbegin(); it != clipboardSlots.crend(); ++it) {
        ResourceFreeDocument table;
        table.setResourceProvider([](const QUrl &) { return QVariant(); }); table.setLayoutEnabled(false);
        table.setMarkdown(it->second, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML));
        // Match the grid's resource-free cell display: no nested image objects.
        QList<QPair<int, int>> images;
        for (QTextBlock block = table.begin(); block.isValid(); block = block.next()) for (auto f = block.begin(); !f.atEnd(); ++f)
            if (f.fragment().charFormat().isImageFormat()) images.prepend({f.fragment().position(), f.fragment().length()});
        for (const auto &range : images) {
            QTextCursor image(&table); image.setPosition(range.first);
            image.setPosition(range.first + range.second, QTextCursor::KeepAnchor); image.removeSelectedText();
        }
        QTextCursor replacement(&selected); replacement.setPosition(it->first); replacement.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        QTextCursor body(&table); body.select(QTextCursor::Document); replacement.insertFragment(body.selection());
    }
    return QMetaObject::invokeMethod(m_resolver, "copyAsHtml", Qt::DirectConnection,
        Q_ARG(int, 0), Q_ARG(int, selected.characterCount() - 1), Q_ARG(QObject *, &selected), Q_ARG(bool, asHtml));
}

void LivePreview::copySource() {
    if (!m_source || !m_viewItem) return;
    if (m_readMode) {
        if (m_viewItem->property("selectionStart") != m_viewItem->property("selectionEnd")) {
            if (!copyReadSelection()) QMetaObject::invokeMethod(m_viewItem, "copy");
        }
        else
            QGuiApplication::clipboard()->setText(documentText(m_source));
        return;
    }
    QTextCursor cursor(m_source);
    cursor.setPosition(m_anchor);
    cursor.setPosition(m_caret, QTextCursor::KeepAnchor);
    QGuiApplication::clipboard()->setText(cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n')));
}
void LivePreview::prepareFormatting() {
    if (m_readMode) return;
    QGuiApplication::inputMethod()->commit();
    if (m_composing && m_viewItem) { QInputMethodEvent cancel; QCoreApplication::sendEvent(m_viewItem, &cancel); }
    prepareEdit();
}
bool LivePreview::autoPair(QKeyEvent *event) {
    if (m_composing || !m_source || m_viewItem->property("overwriteMode").toBool()
        || (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier))) return false;
    const int from = qMin(m_anchor, m_caret), to = qMax(m_anchor, m_caret);
    const QChar before = from > 0 ? m_source->characterAt(from - 1) : QChar();
    const QChar after = to < m_source->characterCount() - 1 ? m_source->characterAt(to) : QChar();
    const QString opens = QStringLiteral("[{'\""), closes = QStringLiteral("]}'\"");
    auto escapedAt = [this](int position) {
        int escapes = 0;
        for (int p = position - 1; p >= 0 && m_source->characterAt(p) == QLatin1Char('\\'); --p) ++escapes;
        return escapes % 2 != 0;
    };
    auto replace = [this](int start, int end, const QString &text, int anchor, int caret) {
        QTextCursor edit(m_source); edit.beginEditBlock(); edit.setPosition(start);
        edit.setPosition(end, QTextCursor::KeepAnchor); edit.insertText(text); edit.endEditBlock();
        select(m_sourceItem, anchor, caret);
    };
    if (event->key() == Qt::Key_Backspace && from == to && from > 0) {
        const int index = opens.indexOf(before);
        if (index < 0 || after != closes[index] || escapedAt(from - 1)) return false;
        replace(from - 1, to + 1, {}, from - 1, from - 1);
        return true;
    }
    if (event->text().size() != 1) return false;
    const QChar typed = event->text().front();
    if (escapedAt(from)) return false;
    if (from == to && closes.contains(typed) && after == typed) {
        select(m_sourceItem, to + 1, to + 1);
        return true;
    }
    const int index = opens.indexOf(typed);
    if (index < 0) return false;
    const bool quote = typed == QLatin1Char('\'') || typed == QLatin1Char('"');
    if (from == to && ((quote && (before.isLetterOrNumber() || before == QLatin1Char('_')))
        || after.isLetterOrNumber() || after == QLatin1Char('_'))) return false;
    const QString selected = textRange(m_source, from, to - from);
    replace(from, to, QString(typed) + selected + closes[index],
        m_anchor <= m_caret ? from + 1 : to + 1, m_anchor <= m_caret ? to + 1 : from + 1);
    return true;
}
void LivePreview::watchTableInput(QObject *table, bool enabled) {
    auto *item = qobject_cast<QQuickItem *>(table);
    if (!item) return;
    item->setFlag(QQuickItem::ItemAcceptsInputMethod, enabled);
    if (enabled) item->installEventFilter(this);
    else item->removeEventFilter(this);
}
bool LivePreview::eventFilter(QObject *object, QEvent *event) {
    if (object != m_viewItem) {
        // TableView starts native editors for keys, but not an initial IME
        // event. Activate the same TextField and forward the unmodified event.
        if (object->property("cellInputAllowed").toBool()) {
            if (event->type() == QEvent::InputMethod) {
                auto *input = static_cast<QInputMethodEvent *>(event);
                if (!input->preeditString().isEmpty() || !input->commitString().isEmpty() || input->replacementLength()) {
                    QVariant result;
                    QMetaObject::invokeMethod(object, "inputMethodEditor", Qt::DirectConnection, Q_RETURN_ARG(QVariant, result));
                    if (auto *editor = result.value<QObject *>()) { QCoreApplication::sendEvent(editor, event); return true; }
                }
            } else if (event->type() == QEvent::InputMethodQuery) {
                auto *query = static_cast<QInputMethodQueryEvent *>(event);
                query->setValue(Qt::ImEnabled, true);
                query->setValue(Qt::ImHints, int(Qt::ImhNone));
                query->setValue(Qt::ImCursorRectangle, object->property("cellInputRectangle"));
                query->setValue(Qt::ImSurroundingText, QString());
                query->setValue(Qt::ImCursorPosition, 0); query->setValue(Qt::ImAnchorPosition, 0);
                return true;
            }
        }
        return QObject::eventFilter(object, event);
    }
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->keyCombination() == QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_M)) {
            // Native Qt Quick can miss the window shortcut in a read-only
            // TextEdit. Keep the focused-editor route working in both modes.
            emit toggleReadModeRequested();
            return true;
        }
        if (key->keyCombination() == QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_C)) {
            // Focused-editor fallback: native Qt Quick shortcut activation can
            // depend on window activation. Never insert C or perform plain Copy.
            emit copyHtmlRequested();
            return true;
        }
        if (key->matches(QKeySequence::Copy)) {
            copySource();
            return true;
        }
        if (m_readMode) return QObject::eventFilter(object, event);
        if (key->keyCombination() == QKeyCombination(Qt::ControlModifier, Qt::Key_T)
            || key->keyCombination() == QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_T)) {
            emit formatCodeRequested(key->modifiers().testFlag(Qt::ShiftModifier));
            return true;
        }
        // Qt Quick can consume Undo's ShortcutOverride even with the view's
        // undo stack disabled. Always route native undo to the source item.
        if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo)) {
            QMetaObject::invokeMethod(m_sourceItem,
                                     key->matches(QKeySequence::Undo) ? "undo" : "redo");
            return true;
        }
        prepareEdit(key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete
                    || key->matches(QKeySequence::Paste) || key->matches(QKeySequence::Cut));
        if (autoPair(key)) return true;
    } else if (!m_readMode && event->type() == QEvent::InputMethod) {
        auto *input = static_cast<QInputMethodEvent *>(event);
        if (!m_composing)
            prepareEdit(input->replacementStart() != 0 || input->replacementLength() != 0);
        m_composing = !input->preeditString().isEmpty();
        bool cursorVisible = true;
        if (m_composing) for (const auto &attribute : input->attributes())
            if (attribute.type == QInputMethodEvent::Cursor) cursorVisible = attribute.length != 0;
        if (cursorVisible != m_imeCursorVisible) {
            m_imeCursorVisible = cursorVisible;
            emit imeCursorVisibleChanged();
        }
        if (m_spelling)
            m_spelling->setComposing(m_composing);
        if (!m_composing) {
            refreshSpelling();
            schedule();
        }
    }
    return QObject::eventFilter(object, event);
}

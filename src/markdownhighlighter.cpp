#include "markdownhighlighter.h"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QSignalBlocker>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace {
// Zero-sized pages are a useful optimization for plain source, but Qt can
// leave inline-object lines without a QTextLine when restoring a rich layout.
// QQuickTextNodeEngine assumes that line exists when rendering an image.
bool canSuspendLayout(QTextDocument *document) {
    if (!document || !document->rootFrame()->childFrames().isEmpty())
        return false;
    for (const QTextFormat &format : document->allFormats())
        if (format.objectType() != QTextFormat::NoObject)
            return false;
    return true;
}
struct CodeBlockData : QTextBlockUserData {
    KSyntaxHighlighting::Definition language;
    KSyntaxHighlighting::State state;
};
}

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document) {
    rebuildFormats();
    // Connected after QSyntaxHighlighter's edit handler: propagate semantic
    // lexer changes synchronously, even when the public fence integer stays put.
    m_references = MarkdownReferences::Index(document->toPlainText());
    connect(document, &QTextDocument::contentsChange, this,
            [this](int, int removed, int added) {
        propagateCodeState();
        if (!this->document() || (!removed && !added)) return; // Detached or formatting-only.
        MarkdownReferences::Index next(this->document()->toPlainText());
        const bool changed = next.links != m_references.links || next.notes != m_references.notes;
        m_references = std::move(next);
        // A definition can change the appearance of earlier, untouched blocks.
        if (changed) rehighlightAll();
    });
}

void MarkdownHighlighter::setDocument(QTextDocument *doc) {
    m_references = MarkdownReferences::Index(doc ? doc->toPlainText() : QString());
    QSyntaxHighlighter::setDocument(doc);
}

void MarkdownHighlighter::propagateCodeState() {
    if (m_propagatingCodeState || !m_pendingCodeBlock.isValid() || !document())
        return;
    m_propagatingCodeState = true;
    QTextDocument *doc = document();
    // Incremental lexer convergence must not suspend/dirty the whole native
    // layout. Only the blocks whose outgoing lexer state changed are visited.
    QSignalBlocker layoutSignals(doc->documentLayout());
    // Coalesce QTextDocument's internal dirty ranges too: blocking signals
    // alone does not prevent downstream relayout per rehighlightBlock().
    QTextCursor batch(doc); batch.beginEditBlock();
    int first = doc->characterCount(), end = 0;
    while (m_pendingCodeBlock.isValid()) {
        const QTextBlock block = m_pendingCodeBlock;
        m_pendingCodeBlock = QTextBlock();
        first = qMin(first, block.position());
        end = qMax(end, block.position() + block.length());
        rehighlightBlock(block);
    }
    batch.endEditBlock();
    layoutSignals.unblock();
    if (end > first) doc->markContentsDirty(first, end - first);
    m_propagatingCodeState = false;
}

// One setter for the whole theme, so a theme change rehighlights once.
void MarkdownHighlighter::setTheme(bool darkMode, const QString &background,
                                   const QString &foreground, const QString &accent,
                                   const QString &codeBackground) {
    if (m_darkMode == darkMode && m_customBackground == background
            && m_customForeground == foreground && m_customAccent == accent
            && m_customCodeBackground == codeBackground)
        return;

    m_darkMode = darkMode;
    m_customBackground = background;
    m_customForeground = foreground;
    m_customAccent = accent;
    m_customCodeBackground = codeBackground;
    m_codeHighlighter.setDarkMode(darkMode);
    rebuildFormats();
    rehighlightAll();
}

// The hidden marker spacing is measured from the document's default font, so
// it has to be remeasured whenever the editor switches typeface.
void MarkdownHighlighter::refreshFont() {
    rebuildFormats();
    rehighlightAll();
}

void MarkdownHighlighter::setSearch(const QString &query, int currentMatchStart) {
    // An edit can carry the painted match away from an offset another match
    // now holds, so the offset matching is not enough to leave things be.
    const bool matchStayed = m_currentMatch.isNull() ? currentMatchStart < 0
                                                     : m_currentMatch.position() == currentMatchStart;
    if (m_searchQuery == query && m_currentMatchStart == currentMatchStart && matchStayed)
        return;
    const bool sameQuery = m_searchQuery == query;
    const QTextBlock left = m_currentMatch.isNull() ? QTextBlock() : m_currentMatch.block();
    m_searchQuery = query;
    m_currentMatchStart = currentMatchStart;
    m_currentMatch = QTextCursor();
    if (currentMatchStart >= 0) {
        m_currentMatch = QTextCursor(document());
        m_currentMatch.setPosition(qBound(0, currentMatchStart, document()->characterCount() - 1));
    }
    if (!sameQuery) {
        rehighlightAll();
        return;
    }

    // Only the current match moved, so restyle just the blocks it left and
    // entered rather than the whole document.
    const QTextBlock entered = m_currentMatch.isNull() ? QTextBlock() : m_currentMatch.block();
    if (left.isValid())
        rehighlightBlock(left);
    if (entered.isValid() && entered != left)
        rehighlightBlock(entered);
}

// QSyntaxHighlighter::rehighlight() marks the blocks dirty one at a time, and
// a laid-out document redoes layout work for the rest of itself after each
// one, so a full restyle grows with the square of its length: 40 s for a 2 MB
// file. A null page size makes the layout skip that work, so restyle under one
// and lay the document out once when the real size comes back.
//
// Never do this for rich objects/frames (see canSuspendLayout()).
// That layout has to finish here. Restoring the size only starts a lazy one,
// and Qt Quick's render thread would lay out whatever it reaches first,
// starting the layout's timers from the wrong thread.
void MarkdownHighlighter::rehighlightAll() {
    QTextDocument *doc = document();
    if (!doc) {
        rehighlight();
        return;
    }
    const QSizeF pageSize = doc->pageSize();
    const bool suspend = canSuspendLayout(doc);
    QSignalBlocker layoutSignals(doc->documentLayout());
    if (suspend)
        doc->setPageSize(QSizeF(0, 0));
    rehighlight();
    m_pendingCodeBlock = QTextBlock();
    if (suspend)
        doc->setPageSize(pageSize);
    layoutSignals.unblock();
    doc->markContentsDirty(0, doc->characterCount());
    if (QAbstractTextDocumentLayout *layout = doc->documentLayout())
        layout->documentSize();
}

void MarkdownHighlighter::setCaret(int position) {
    const QTextBlock left = m_caret.isNull() ? QTextBlock() : m_caret.block();
    if (m_caret.isNull()) {
        m_caret = QTextCursor(document());
        // Loading a file inserts at the caret, which must not drag it along
        // to the end; the editor reports where it really went.
        m_caret.setKeepPositionOnInsert(true);
    }
    m_caret.setPosition(qBound(0, position, document()->characterCount() - 1));

    // Only the lines the caret left and entered can show different markers.
    const QTextBlock entered = m_caret.block();
    if (left.isValid() && left != entered)
        rehighlightBlock(left);
    rehighlightBlock(entered);
}

void MarkdownHighlighter::setSelection(int start, int end) {
    const QTextCursor previous = m_selection;
    const int last = document()->characterCount() - 1;
    start = qBound(0, start, last);
    end = qBound(0, end, last);
    m_selection = QTextCursor(document());
    m_selection.setPosition(qMin(start, end));
    m_selection.setPosition(qMax(start, end), QTextCursor::KeepAnchor);
    m_selection.setKeepPositionOnInsert(true);

    // Inclusive endpoint blocks match the heading activation rule. Restyle
    // the symmetric difference plus the inline boundaries below, never the
    // whole unchanged range when dragging or reporting the same selection.
    const auto range = [this](const QTextCursor &cursor) {
        return cursor.isNull() || !cursor.hasSelection()
            ? std::pair<int, int>{-1, -2}
            : std::pair<int, int>{document()->findBlock(cursor.selectionStart()).blockNumber(),
                                  document()->findBlock(cursor.selectionEnd()).blockNumber()};
    };
    const auto oldRange = range(previous);
    const auto newRange = range(m_selection);
    if (oldRange == newRange && (!m_selection.hasSelection()
            || (previous.selectionStart() == m_selection.selectionStart()
                && previous.selectionEnd() == m_selection.selectionEnd())))
        return;
    std::vector<std::pair<int, int>> changed;
    const auto subtract = [&changed](std::pair<int, int> a, std::pair<int, int> b) {
        if (a.first > a.second)
            return;
        if (b.first > b.second || a.second < b.first || a.first > b.second) {
            changed.push_back(a);
            return;
        }
        if (a.first < b.first)
            changed.emplace_back(a.first, b.first - 1);
        if (a.second > b.second)
            changed.emplace_back(b.second + 1, a.second);
    };
    subtract(oldRange, newRange);
    subtract(newRange, oldRange);
    // Inline marker visibility also depends on offsets within the boundary
    // blocks. Restyle those even when the selection stays in the same blocks.
    for (const auto &range : {oldRange, newRange}) {
        if (range.first > range.second) continue;
        for (int boundary : {range.first, range.second}) {
            const bool covered = std::any_of(changed.cbegin(), changed.cend(),
                [boundary](const auto &interval) {
                    return boundary >= interval.first && boundary <= interval.second;
                });
            if (!covered) {
                const QString text = document()->findBlockByNumber(boundary).text();
                // Plain/heading blocks do not depend on offsets within them.
                if (!codeSpans(text).isEmpty() || !inlineMarkup(text).isEmpty())
                    changed.emplace_back(boundary, boundary);
            }
        }
    }
    int count = 0;
    for (const auto &interval : changed)
        count += interval.second - interval.first + 1;
    QTextDocument *doc = document();
    const QSizeF pageSize = doc->pageSize();
    // As in rehighlightAll(), suppress quadratic downstream relayout for a
    // large selection, then complete layout once on the GUI thread.
    const bool suspend = count > 8 && canSuspendLayout(doc);
    std::optional<QSignalBlocker> layoutSignals;
    if (count > 8)
        layoutSignals.emplace(doc->documentLayout());
    if (suspend)
        doc->setPageSize(QSizeF(0, 0));
    QTextCursor batch(doc); batch.beginEditBlock();
    for (const auto &interval : changed) {
        for (QTextBlock block = doc->findBlockByNumber(interval.first);
             block.isValid() && block.blockNumber() <= interval.second; block = block.next())
            rehighlightBlock(block);
    }
    batch.endEditBlock();
    if (suspend)
        doc->setPageSize(pageSize);
    if (layoutSignals) {
        layoutSignals->unblock();
        doc->markContentsDirty(0, doc->characterCount());
        if (QAbstractTextDocumentLayout *layout = doc->documentLayout())
            layout->documentSize();
    }
}

void MarkdownHighlighter::rebuildFormats() {
    const QColor marker = m_darkMode ? QColor(QStringLiteral("#4f525a"))
                                     : QColor(QStringLiteral("#aeb1b5"));
    const QColor background = !m_customBackground.isEmpty() ? QColor(m_customBackground)
        : (m_darkMode ? QColor(QStringLiteral("#101010")) : QColor(QStringLiteral("#ffffff")));
    const QColor text = !m_customForeground.isEmpty() ? QColor(m_customForeground)
        : (m_darkMode ? QColor(QStringLiteral("#eeeeee")) : QColor(QStringLiteral("#222324")));
    const QColor link = !m_customAccent.isEmpty() ? QColor(m_customAccent)
        : (m_darkMode ? QColor(QStringLiteral("#5584aa")) : QColor(QStringLiteral("#2077b2")));
    const QColor quote = marker;

    m_markerFormat = QTextCharFormat();
    m_markerFormat.setForeground(marker);

    // A sub-pixel font size combined with a stretch factor used to make these
    // markers occupy (close to) zero space, but that combination deadlocks Qt's
    // font metrics engine on some platforms. Instead, use a normal font size and
    // cancel out its advance width with negative absolute letter-spacing.
    m_hiddenMarkerFormat = QTextCharFormat();
    m_hiddenMarkerFormat.setForeground(background);
    m_hiddenMarkerFormat.setFontPointSize(1.0);

    QFont hiddenFont = document() ? document()->defaultFont() : QFont();
    hiddenFont.setPointSizeF(1.0);
    const qreal charWidth = QFontMetricsF(hiddenFont).horizontalAdvance(QLatin1Char('['));

    m_hiddenMarkerFormat.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    m_hiddenMarkerFormat.setFontLetterSpacing(-charWidth);

    m_headingFormat = QTextCharFormat();
    m_headingFormat.setForeground(text);
    m_headingFormat.setFontWeight(QFont::Bold);

    m_boldFormat = QTextCharFormat();
    m_boldFormat.setFontWeight(QFont::Bold);

    m_italicFormat = QTextCharFormat();
    m_italicFormat.setFontItalic(true);

    m_codeFormat = QTextCharFormat();
    m_codeFormat.setForeground(text);
    if (!m_customCodeBackground.isEmpty())
        m_codeFormat.setBackground(QColor(m_customCodeBackground));

    // A fence recedes the way a heading's `#` does, over the panel it opens.
    m_blockCodeFormat = m_codeFormat;
    m_blockCodeFormat.clearBackground(); // One native presentation panel, not glyph strips.
    m_fenceFormat = m_blockCodeFormat;
    m_fenceFormat.setForeground(marker);

    m_quoteFormat = QTextCharFormat();
    m_quoteFormat.setForeground(quote);
    m_quoteFormat.setFontItalic(true);

    m_linkFormat = QTextCharFormat();
    m_linkFormat.setForeground(link);
    m_linkFormat.setFontUnderline(true);

    m_searchFormat = QTextCharFormat();
    m_searchFormat.setBackground(m_darkMode ? QColor(QStringLiteral("#725b18"))
                                            : QColor(QStringLiteral("#ffe58a")));
    m_currentSearchFormat = QTextCharFormat();
    m_currentSearchFormat.setBackground(m_darkMode ? QColor(QStringLiteral("#b36b20"))
                                                   : QColor(QStringLiteral("#ffad42")));
}

void MarkdownHighlighter::highlightBlock(const QString &text) {
    // A fenced run of code is literal from its opening fence through its
    // closing one, so no markup runs inside it. The state rides from block to
    // block, and Qt rehighlights the rest of the document when it changes.
    if (m_pendingCodeBlock == currentBlock())
        m_pendingCodeBlock = QTextBlock();
    // User data holds the embedded state without changing Backend's public
    // integer fence encoding. Only propagate until this combined state settles.
    const auto saveCodeState = [this](const KSyntaxHighlighting::Definition &language,
                                      const KSyntaxHighlighting::State &state) {
        auto *old = dynamic_cast<CodeBlockData *>(currentBlockUserData());
        const bool changed = old ? old->language != language || old->state != state
                                 : language.isValid() || state != KSyntaxHighlighting::State();
        if (!old) {
            old = new CodeBlockData;
            setCurrentBlockUserData(old);
        }
        old->language = language;
        old->state = state;
        const QTextBlock next = currentBlock().next();
        if (changed && next.isValid()) {
            if (!m_pendingCodeBlock.isValid()
                    || next.position() < m_pendingCodeBlock.position())
                m_pendingCodeBlock = next;
            // Covers Qt's initial delayed highlighting as well as explicit
            // rehighlightBlock calls, which do not emit contentsChange.
            if (!m_propagatingCodeState && !m_codePropagationScheduled) {
                m_codePropagationScheduled = true;
                QMetaObject::invokeMethod(this, [this] {
                    m_codePropagationScheduled = false;
                    propagateCodeState();
                }, Qt::QueuedConnection);
            }
        }
    };
    const int previous = previousBlockState();
    if (previous >= InsideFence) {
        const int fence = previous - InsideFence;
        const bool closing = closesFence(text, fence / 2 + 3,
                                         fence % 2 ? QLatin1Char('~') : QLatin1Char('`'));
        setCurrentBlockState(closing ? Prose : previous);
        setFormat(0, text.length(), closing ? m_fenceFormat : m_blockCodeFormat);
        const auto *context = dynamic_cast<const CodeBlockData *>(currentBlock().previous().userData());
        if (!closing && context) {
            const auto line = m_codeHighlighter.tokenize(text, context->language, context->state);
            for (const auto &token : line.tokens) {
                QTextCharFormat style = m_blockCodeFormat;
                style.merge(token.format);
                setFormat(token.offset, token.length, style);
            }
            saveCodeState(context->language, line.state);
        } else {
            saveCodeState({}, {});
        }
        highlightSearch(text);
        return;
    }

    const int opening = openingFenceLength(text);
    if (opening > 0) {
        const bool tildes = text.trimmed().startsWith(QLatin1Char('~'));
        setCurrentBlockState(InsideFence + (opening - 3) * 2 + (tildes ? 1 : 0));
        setFormat(0, text.length(), m_fenceFormat);
        const QString trimmed = text.trimmed();
        saveCodeState(CodeHighlighter::resolveLanguage(trimmed.mid(opening)), {});
        highlightSearch(text);
        return;
    }
    setCurrentBlockState(Prose);
    saveCodeState({}, {});

    if (!text.isEmpty()) {
        highlightMarkers(text);
        if (text.contains(QLatin1Char('`')) || text.contains(QLatin1Char('*'))
            || text.contains(QLatin1Char('_')) || text.contains(QLatin1Char('['))
            || text.contains(QLatin1Char('~')) || text.contains(QLatin1Char('<'))) {
            highlightInline(text);
        }
        // Last among markup styles, so inline links/emphasis cannot restore
        // heading markers that should be hidden (or hide active ones).
        highlightHeadingMarkers(text);
        const int quotePrefix = currentBlock().blockFormat().intProperty(QuotePrefixProperty);
        if (quotePrefix) {
            const auto block = currentBlock();
            const bool active = !m_caret.isNull() && m_caret.block() == block;
            const bool selected = m_selection.hasSelection() && m_selection.selectionStart() < block.position() + block.length()
                && m_selection.selectionEnd() >= block.position();
            auto format = m_markerFormat;
            if (!active && !selected && !searchTouchesPrefix(text, quotePrefix, m_searchQuery)) format.setForeground(Qt::transparent);
            setFormat(0, qMin(quotePrefix, int(text.size())), format);
        }
        const int prefix = currentBlock().blockFormat().intProperty(ListPrefixProperty);
        if (prefix) {
            const int start = currentBlock().position();
            const bool active = !m_caret.isNull() && m_caret.position() >= start && m_caret.position() < start + prefix;
            const bool selected = m_selection.hasSelection() && m_selection.selectionStart() < start + prefix && m_selection.selectionEnd() > start;
            auto format = m_markerFormat;
            const bool searched = searchTouchesPrefix(text, prefix, m_searchQuery);
            if (!active && !selected && !searched) format.setForeground(Qt::transparent);
            // Only ink changes: preserve prefix advances and hanging geometry.
            setFormat(0, qMin(prefix, int(text.size())), format);
        }
    }
    highlightSpelling(text);
    highlightSearch(text);
}

bool MarkdownHighlighter::searchTouchesPrefix(const QString &text, int prefixLength, const QString &query) {
    if (query.isEmpty()) return false;
    for (int start : findMatches(text, query)) if (start < prefixLength) return true;
    return false;
}

void MarkdownHighlighter::setSpellingRanges(const QList<Span> &ranges) {
    QSet<int> affected;
    const auto addBlocks = [this, &affected](const Span &range) {
        if (!document() || range.length <= 0)
            return;
        const int end = qMin(range.start + range.length, document()->characterCount() - 1);
        for (QTextBlock block = document()->findBlock(qMax(0, range.start));
             block.isValid() && block.position() < end; block = block.next())
            affected.insert(block.position());
    };
    int old = 0, next = 0;
    while (old < m_spellingRanges.size() || next < ranges.size()) {
        if (old < m_spellingRanges.size() && next < ranges.size()
                && m_spellingRanges.at(old).start == ranges.at(next).start
                && m_spellingRanges.at(old).length == ranges.at(next).length) {
            ++old;
            ++next;
        } else if (next == ranges.size() || (old < m_spellingRanges.size()
                   && m_spellingRanges.at(old).start <= ranges.at(next).start)) {
            addBlocks(m_spellingRanges.at(old++));
        } else {
            addBlocks(ranges.at(next++));
        }
    }
    m_spellingRanges = ranges;
    if (affected.isEmpty() || !document())
        return;
    QTextDocument *doc = document();
    // Spelling is paint-only: suppress formatting-only contentsChanged so a
    // source highlighter reply cannot schedule a LivePreview projection. Also
    // coalesce Quick's synchronous layout callbacks without changing page size
    // (in particular, never suspend rich object/frame layout with pageSize 0).
    QSignalBlocker documentSignals(doc);
    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    QSignalBlocker layoutSignals(layout);
    for (int position : std::as_const(affected)) {
        const QTextBlock block = doc->findBlock(position);
        if (block.isValid())
            rehighlightBlock(block);
    }
    layout->documentSize(); // finish rich-object layout on the GUI thread
    layoutSignals.unblock();
    emit layout->update();
}

void MarkdownHighlighter::highlightSpelling(const QString &text) {
    const int base = currentBlock().position();
    const auto first = std::lower_bound(m_spellingRanges.cbegin(), m_spellingRanges.cend(), base,
        [](const Span &range, int position) { return range.start + range.length <= position; });
    for (auto it = first; it != m_spellingRanges.cend() && it->start < base + text.size(); ++it) {
        const int start = qMax(0, it->start - base);
        const int end = qMin(int(text.size()), it->start + it->length - base);
        for (int i = start; i < end; ++i) {
            QTextCharFormat merged = format(i);
            merged.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
            merged.setUnderlineColor(QColor(QStringLiteral("#d74646")));
            setFormat(i, 1, merged);
        }
    }
}

// Three or more backticks or tildes, indented at most three spaces. An info
// string may follow, but a backtick fence's may not hold a backtick:
// "```code```" is inline code.
int MarkdownHighlighter::openingFenceLength(const QString &text) {
    static const QRegularExpression openingRe(
        QStringLiteral("^ {0,3}(?:(`{3,})[^`]*|(~{3,}).*)$"));
    const QRegularExpressionMatch match = openingRe.match(text);
    return match.hasMatch() ? int(qMax(match.capturedLength(1), match.capturedLength(2))) : 0;
}

// At least as many of the same fence character as opened it, and nothing after.
bool MarkdownHighlighter::closesFence(const QString &text, int openingLength, QChar marker) {
    static const QRegularExpression closingRe(QStringLiteral("^ {0,3}(`{3,}|~{3,})\\s*$"));
    const QRegularExpressionMatch match = closingRe.match(text);
    return match.hasMatch() && match.capturedLength(1) >= openingLength
        && match.captured(1).at(0) == marker;
}

namespace {

// Each UTF-16 unit case-folded and, for Latin and Greek letters, stripped of
// its canonical diacritics: 'Ț' becomes 't'. Other scripts keep their marks,
// since there they tell letters apart (и and й, か and が), and Hangul
// syllables would fold to their first jamo. Surrogates map to themselves here
// and are folded in pairs by searchFolded.
const char16_t *searchFoldTable() {
    static const std::vector<char16_t> table = [] {
        std::vector<char16_t> folded(0x10000);
        for (int unit = 0; unit < 0x10000; ++unit) {
            const QChar original{char16_t(unit)};
            if (original.isSurrogate()) {
                folded[unit] = char16_t(unit);
                continue;
            }
            const QChar caseFolded = original.toCaseFolded();
            QChar base = caseFolded;
            while (base.decompositionTag() == QChar::Canonical) {
                const QChar next = base.decomposition().at(0);
                if (next.isSurrogate())
                    break;
                base = next.toCaseFolded();
            }
            const bool strips = base.script() == QChar::Script_Latin
                || base.script() == QChar::Script_Greek;
            folded[unit] = strips ? base.unicode() : caseFolded.unicode();
        }
        // A stroke or a missing dot is part of the letter, not a mark on it,
        // so these have no decomposition to strip.
        const std::pair<char16_t, char16_t> plain[] = {
            {u'\u0142', u'l'}, {u'\u0111', u'd'}, {u'\u00f8', u'o'},
            {u'\u0127', u'h'}, {u'\u0131', u'i'}};
        for (char16_t &unit : folded) {
            for (const auto &[from, to] : plain) {
                if (unit == from)
                    unit = to;
            }
        }
        return folded;
    }();
    return table.data();
}

// Romanian is written with ș and ț but much text has the older ş and ţ.
QChar commaBelow(QChar c) {
    if (c == QChar(0x015F))
        return QChar(0x0219);
    if (c == QChar(0x0163))
        return QChar(0x021B);
    return c;
}

// The text folded unit by unit through the table, then each surrogate pair
// case-folded as the one letter it encodes, which stays a pair: 𐐀 becomes 𐐨.
QString searchFolded(const QString &text) {
    const char16_t *fold = searchFoldTable();
    QString folded(text.size(), Qt::Uninitialized);
    const QChar *source = text.constData();
    QChar *target = folded.data();
    for (qsizetype i = 0; i < text.size(); ++i)
        target[i] = QChar(fold[source[i].unicode()]);
    for (qsizetype i = 0; i + 1 < text.size(); ++i) {
        if (!source[i].isHighSurrogate() || !source[i + 1].isLowSurrogate())
            continue;
        const char32_t caseFolded = QChar::toCaseFolded(QChar::surrogateToUcs4(source[i], source[i + 1]));
        if (QChar::requiresSurrogates(caseFolded)) {
            target[i] = QChar(QChar::highSurrogate(caseFolded));
            target[i + 1] = QChar(QChar::lowSurrogate(caseFolded));
        }
        ++i;
    }
    return folded;
}

} // namespace

QList<int> MarkdownHighlighter::findMatches(const QString &text, const QString &query) {
    QList<int> matches;
    const qsizetype length = query.size();
    if (length == 0 || text.size() < length)
        return matches;

    const QString foldedQuery = searchFolded(query);
    // The query's accented letters, case-folded, which the text must match
    // as written rather than folded.
    QList<std::pair<qsizetype, QChar>> exact;
    for (qsizetype i = 0; i < length; ++i) {
        const QChar unit = query.at(i);
        const QChar caseFolded = unit.toCaseFolded();
        if (!unit.isSurrogate() && foldedQuery.at(i) != caseFolded)
            exact.append({i, commaBelow(caseFolded)});
    }

    const QString foldedText = searchFolded(text);

    qsizetype from = 0;
    while ((from = foldedText.indexOf(foldedQuery, from)) >= 0) {
        const bool accentsMatch = std::all_of(exact.cbegin(), exact.cend(), [&](const auto &letter) {
            return commaBelow(text.at(from + letter.first).toCaseFolded()) == letter.second;
        });
        if (accentsMatch) {
            matches.append(int(from));
            from += length;
        } else {
            ++from;
        }
    }
    return matches;
}

void MarkdownHighlighter::highlightSearch(const QString &text) {
    if (m_searchQuery.isEmpty())
        return;

    for (const int from : findMatches(text, m_searchQuery)) {
        const int documentStart = currentBlock().position() + from;
        // A match can cross hidden heading markers and visible content.
        // Preserve each character's markup, changing only its background.
        for (int i = from; i < from + m_searchQuery.length(); ++i) {
            QTextCharFormat format = this->format(i);
            format.setBackground(documentStart == m_currentMatchStart
                                     ? m_currentSearchFormat.background()
                                     : m_searchFormat.background());
            setFormat(i, 1, format);
        }
    }
}

namespace {

struct HeadingMarkers {
    int openingEnd = 0;
    int closingStart = -1;
    int level = 0;
};

HeadingMarkers headingMarkers(const QString &text) {
    // ATX headings allow up to three leading spaces, one to six hashes,
    // and require a space/tab or end of line after the opening hashes.
    static const QRegularExpression openingRe(QStringLiteral("^ {0,3}#{1,6}(?:[ \\t]+|$)"));
    const auto opening = openingRe.match(text);
    if (!opening.hasMatch())
        return {};
    HeadingMarkers markers;
    markers.openingEnd = int(opening.capturedEnd());
    const int hashStart = text.indexOf(QLatin1Char('#'));
    while (hashStart + markers.level < markers.openingEnd
           && text.at(hashStart + markers.level) == QLatin1Char('#'))
        ++markers.level;
    // Closing hashes are optional, must be whitespace-separated, and may
    // only be followed by spaces/tabs. Escaped or embedded hashes are text.
    static const QRegularExpression closingRe(QStringLiteral("[ \\t]+#+[ \\t]*$"));
    const auto closing = closingRe.match(text);
    if (closing.hasMatch())
        markers.closingStart = qMax(markers.openingEnd, int(closing.capturedStart()));
    return markers;
}

} // namespace

void MarkdownHighlighter::highlightHeadingMarkers(const QString &text) {
    const HeadingMarkers markers = headingMarkers(text);
    if (!markers.openingEnd)
        return;
    const bool atCaret = !m_caret.isNull() && m_caret.block() == currentBlock();
    const int start = currentBlock().position();
    const bool selected = m_selection.hasSelection()
        && m_selection.selectionStart() <= start + text.length()
        && m_selection.selectionEnd() >= start;
    QTextCharFormat markerFormat = m_hiddenMarkerFormat;
    if (atCaret || selected) {
        markerFormat = m_markerFormat;
        markerFormat.setProperty(QTextFormat::FontSizeAdjustment, 4 - markers.level);
    }
    setFormat(0, markers.openingEnd, markerFormat);
    if (markers.closingStart >= 0)
        setFormat(markers.closingStart, text.length() - markers.closingStart, markerFormat);
}

void MarkdownHighlighter::highlightMarkers(const QString &text) {
    int first = 0;
    while (first < text.length() && text.at(first).isSpace())
        ++first;
    if (first >= text.length())
        return;

    const QChar firstChar = text.at(first);
    const HeadingMarkers heading = headingMarkers(text);
    if (heading.openingEnd) {
        QTextCharFormat headingFormat = m_headingFormat;
        // Use the same relative sizes as Qt's Markdown heading importer (Read).
        // A relative adjustment also follows pixel fonts, zoom and font changes.
        headingFormat.setProperty(QTextFormat::FontSizeAdjustment, 4 - heading.level);
        setFormat(heading.openingEnd, text.length() - heading.openingEnd, headingFormat);
        return;
    }

    if (firstChar == QLatin1Char('>')) {
        static const QRegularExpression quoteRe(QStringLiteral("^((?: {0,3}>[ \\t]?)+)(.*)$"));
        const QRegularExpressionMatch quote = quoteRe.match(text);
        if (quote.hasMatch()) {
            setFormat(0, quote.capturedLength(1), m_markerFormat);
            setFormat(quote.capturedStart(2), quote.capturedLength(2), m_quoteFormat);
        }
    }

    if (firstChar == QLatin1Char('-') || firstChar == QLatin1Char('+')
            || firstChar == QLatin1Char('*') || firstChar.isDigit()) {
        static const QRegularExpression listRe(
            QStringLiteral("^(\\s*(?:[-+*]|\\d+[.)])\\s+)(.*)$"));
        const QRegularExpressionMatch list = listRe.match(text);
        if (list.hasMatch())
            setFormat(0, list.capturedLength(1), m_markerFormat);
    }

    if (isThematicBreak(text))
        setFormat(0, text.length(), m_markerFormat);
}

// A line of three or more -, * or _ (spaces allowed between) is a rule, not
// emphasis: "***" and "___" draw a line.
bool MarkdownHighlighter::isThematicBreak(const QString &text) {
    static const QRegularExpression ruleRe(QStringLiteral("^\\s{0,3}([-*_])(?:\\s*\\1){2,}\\s*$"));
    return ruleRe.match(text).hasMatch();
}

void MarkdownHighlighter::highlightInline(const QString &text) {
    // Inline styles merge onto what the block already set, so a heading stays
    // bold and a quote keeps its colour under emphasis. setFormat replaces, so
    // merge one character at a time.
    const auto mergeFormat = [this](int position, const QTextCharFormat &extra) {
        QTextCharFormat merged = format(position);
        merged.merge(extra);
        setFormat(position, 1, merged);
    };

    // Code keeps its own styling even when emphasis or a link encloses it.
    QList<bool> inCode(text.length(), false);
    const QList<Span> codes = codeSpans(text);
    for (const Span &code : codes) {
        for (int i = code.start; i < code.start + code.length; ++i) {
            mergeFormat(i, m_codeFormat);
            inCode[i] = true;
        }
    }

    // Outer markup first, so what it encloses has the last word: a link inside
    // bold keeps its colour. Strikethrough draws over code too.
    static const QTextCharFormat struck = [] {
        QTextCharFormat format;
        format.setFontStrikeOut(true);
        return format;
    }();
    static const QTextCharFormat underlined = [] {
        QTextCharFormat format;
        format.setFontUnderline(true);
        return format;
    }();
    QList<InlineMarkup> markup = inlineMarkup(text);
    for (const auto &ref : m_references.scan(MarkdownReferences::proseMask(text), text)) {
        const int contentEnd = ref.contentStart + ref.contentLength;
        markup.append({InlineKind::Link, {ref.contentStart, ref.contentLength},
                       {{ref.start, ref.contentStart - ref.start},
                        {contentEnd, ref.end - contentEnd}}, {}});
    }
    std::stable_sort(markup.begin(), markup.end(),
                     [](const InlineMarkup &a, const InlineMarkup &b) {
                         return a.content.length > b.content.length;
                     });
    for (const InlineMarkup &item : markup) {
        const QTextCharFormat &contentFormat =
            item.kind == InlineKind::Bold ? m_boldFormat
            : item.kind == InlineKind::Italic ? m_italicFormat
            : item.kind == InlineKind::Strikethrough ? struck
            : item.kind == InlineKind::Underline ? underlined
                                                   : m_linkFormat;
        for (int i = item.content.start; i < item.content.start + item.content.length; ++i) {
            if (!inCode[i] || item.kind == InlineKind::Strikethrough)
                mergeFormat(i, contentFormat);
        }
    }

    // Last, so no enclosing style shows a marker that should be hidden.
    const int caret = !m_caret.isNull() && m_caret.block() == currentBlock()
        ? m_caret.positionInBlock() : -1;
    const auto styleMarkers = [&](const Span &opening, const Span &closing) {
        const int end = closing.start + closing.length;
        const bool atCaret = caret >= opening.start && caret <= end;
        const int base = currentBlock().position();
        const bool selected = m_selection.hasSelection()
            && m_selection.selectionStart() < base + end
            && m_selection.selectionEnd() > base + opening.start;
        for (const Span &marker : {opening, closing}) {
            if (!atCaret && !selected) {
                setFormat(marker.start, marker.length, m_hiddenMarkerFormat);
                continue;
            }
            for (int i = marker.start; i < marker.start + marker.length; ++i)
                mergeFormat(i, m_markerFormat);
        }
    };
    for (const InlineMarkup &item : markup)
        styleMarkers(item.markers[0], item.markers[1]);
    for (const Span &span : codes) {
        int ticks = 0;
        while (text.at(span.start + ticks) == QLatin1Char('`'))
            ++ticks;
        const int bodyStart = span.start + ticks;
        const int bodyEnd = span.start + span.length - ticks;
        // CommonMark removes one edge space, but never an all-space body.
        const bool padded = text.at(bodyStart) == QLatin1Char(' ')
            && text.at(bodyEnd - 1) == QLatin1Char(' ')
            && std::any_of(text.cbegin() + bodyStart, text.cbegin() + bodyEnd,
                           [](QChar ch) { return ch != QLatin1Char(' '); });
        const int markerLength = ticks + (padded ? 1 : 0);
        styleMarkers({span.start, markerLength},
                     {span.start + span.length - markerLength, markerLength});
    }
}

QList<MarkdownHighlighter::Span> MarkdownHighlighter::codeSpans(const QString &text) {
    QList<Span> spans;
    if (!text.contains(QLatin1Char('`')))
        return spans;

    // Code delimiters are maximal, equal-length runs, not individual ticks.
    // Index the next closing run once to avoid rescanning unmatched openers.
    // Keep matching rules aligned with SpellingProse::words (spellingprose.cpp),
    // which additionally handles paragraph boundaries and excluded regions.
    struct Run { int start; int length; bool escaped; int next = -1; };
    QList<Run> runs;
    for (int p = 0; p < text.size();) {
        if (text.at(p) != QLatin1Char('`')) {
            ++p;
            continue;
        }
        const int start = p++;
        while (p < text.size() && text.at(p) == QLatin1Char('`'))
            ++p;
        int backslashes = 0;
        for (int i = start - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i)
            ++backslashes;
        runs.append({start, p - start, backslashes % 2 != 0});
    }
    QHash<int, int> nextRun;
    for (int r = int(runs.size()) - 1; r >= 0; --r) {
        Run &run = runs[r];
        // An escaped first tick leaves the remainder available as an opener.
        // In code, however, backslashes are literal and closers use the full run.
        const int openingLength = run.length - (run.escaped ? 1 : 0);
        if (openingLength > 0)
            run.next = nextRun.value(openingLength, -1);
        nextRun.insert(run.length, r);
    }
    for (int r = 0; r < runs.size(); ++r) {
        const Run &run = runs.at(r);
        if (run.next < 0)
            continue;
        const Run &close = runs.at(run.next);
        const int start = run.start + (run.escaped ? 1 : 0);
        spans.append({start, close.start + close.length - start});
        r = run.next;
    }
    return spans;
}

QList<MarkdownHighlighter::InlineMarkup> MarkdownHighlighter::inlineMarkup(const QString &text) {
    QList<InlineMarkup> markup;
    if (!text.contains(QLatin1Char('*')) && !text.contains(QLatin1Char('_'))
            && !text.contains(QLatin1Char('[')) && !text.contains(QLatin1Char('~'))
            && !text.contains(QLatin1Char('<'))) {
        return markup;
    }
    if (isThematicBreak(text))
        return markup;

    const auto span = [](const QRegularExpressionMatch &match, int group) {
        return Span{int(match.capturedStart(group)), int(match.capturedLength(group))};
    };

    // Inline code is literal, so a `*`, `_` or `[` inside backticks is not a
    // marker: `default_line_height` keeps its underscores. Markup whose
    // markers land in a code span is dropped; markup that merely wraps one
    // (**bold with `code` inside**) still applies.
    // A backslash escapes a marker, as it does a backtick: \\*literal\\*.
    const QList<Span> code = codeSpans(text);
    const auto escaped = [&text](int position) {
        int backslashes = 0;
        for (int i = position - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i)
            ++backslashes;
        return backslashes % 2 != 0;
    };
    const auto append = [&](const InlineMarkup &item) {
        for (const Span &marker : item.markers) {
            if (escaped(marker.start))
                return false;
            for (const Span &span : code) {
                if (marker.start < span.start + span.length
                        && span.start < marker.start + marker.length)
                    return false;
            }
        }
        markup.append(item);
        return true;
    };

    // "***word***" is bold and italic at once, as Ctrl+B then Ctrl+I writes it:
    // the outer two asterisks of each run are bold, the inner one italic. Its
    // runs are then masked so the patterns below do not split them again.
    QString unclaimed = text;
    static const QRegularExpression boldItalicRe(
        QStringLiteral("(?<!\\*)\\*\\*\\*(?![\\s*])(.*?[^\\s*])\\*\\*\\*(?!\\*)"));
    QRegularExpressionMatchIterator boldItalicMatches = boldItalicRe.globalMatch(text);
    while (boldItalicMatches.hasNext()) {
        const QRegularExpressionMatch match = boldItalicMatches.next();
        const Span whole = span(match, 0);
        const int end = whole.start + whole.length;
        if (!append({InlineKind::Bold, {whole.start + 2, whole.length - 4},
                     {{whole.start, 2}, {end - 2, 2}}}))
            continue;
        markup.append({InlineKind::Italic, span(match, 1),
                       {{whole.start + 2, 1}, {end - 3, 1}}});
        for (int i : {whole.start, whole.start + 1, whole.start + 2, end - 3, end - 2, end - 1})
            unclaimed[i] = QChar::ObjectReplacementCharacter;
    }

    // Underscores only delimit emphasis at a word boundary, so identifiers such
    // as snake_case_name read as themselves. Asterisks delimit anywhere.
    static const QRegularExpression boldRe(
        QStringLiteral("\\*\\*(.+?)\\*\\*|(?<!\\w)__(.+?)__(?!\\w)"),
        QRegularExpression::UseUnicodePropertiesOption);
    QRegularExpressionMatchIterator boldMatches = boldRe.globalMatch(unclaimed);
    while (boldMatches.hasNext()) {
        const QRegularExpressionMatch match = boldMatches.next();
        const Span whole = span(match, 0);
        const int contentIndex = match.capturedStart(1) >= 0 ? 1 : 2;
        append({InlineKind::Bold, span(match, contentIndex),
                {{whole.start, 2}, {whole.start + whole.length - 2, 2}}});
    }

    // An asterisk delimiter may not be followed (opening) or preceded
    // (closing) by whitespace, so `* first *important* point` keeps its bullet
    // and `2 * 3 * 4` stays literal instead of pairing stray asterisks.
    static const QRegularExpression italicRe(
        QStringLiteral("(?<!\\*)\\*(?![\\s*])([^*\\n]*[^\\s*])\\*(?!\\*)|(?<!\\w)_([^_\\n]+)_(?!\\w)"),
        QRegularExpression::UseUnicodePropertiesOption);
    QRegularExpressionMatchIterator italicMatches = italicRe.globalMatch(unclaimed);
    while (italicMatches.hasNext()) {
        const QRegularExpressionMatch match = italicMatches.next();
        const Span whole = span(match, 0);
        const int contentIndex = match.capturedStart(1) >= 0 ? 1 : 2;
        append({InlineKind::Italic, span(match, contentIndex),
                {{whole.start, 1}, {whole.start + whole.length - 1, 1}}});
    }

    // Exactly two tildes, opening before text and closing after it.
    static const QRegularExpression strikeRe(
        QStringLiteral("(?<!~)~~(?![\\s~])(.*?[^\\s~])~~(?!~)"));
    QRegularExpressionMatchIterator strikeMatches = strikeRe.globalMatch(unclaimed);
    while (strikeMatches.hasNext()) {
        const QRegularExpressionMatch match = strikeMatches.next();
        const Span whole = span(match, 0);
        append({InlineKind::Strikethrough, span(match, 1),
                {{whole.start, 2}, {whole.start + whole.length - 2, 2}}});
    }

    // Underline has no Markdown delimiter; Qt's Read importer accepts HTML <u>.
    static const QRegularExpression underlineRe(
        QStringLiteral("</?u>"), QRegularExpression::CaseInsensitiveOption);
    auto underlineMatches = underlineRe.globalMatch(text);
    QList<int> underlineOpenings;
    while (underlineMatches.hasNext()) {
        const auto match = underlineMatches.next();
        const Span tag = span(match, 0);
        bool literal = escaped(tag.start);
        for (const Span &span : code)
            literal |= tag.start < span.start + span.length && span.start < tag.start + tag.length;
        if (literal) continue;
        if (tag.length == 3) {
            underlineOpenings.append(tag.start);
        } else if (!underlineOpenings.isEmpty()) {
            const int opening = underlineOpenings.takeLast();
            if (tag.start > opening + 3)
                append({InlineKind::Underline, {opening + 3, tag.start - opening - 3},
                        {{opening, 3}, tag}});
        }
    }

    static const QRegularExpression linkRe(
        QStringLiteral("\\[([^\\]]+)\\]\\(((?:\\\\.|\\([^()]*\\)|[^()])+)\\)"));
    QRegularExpressionMatchIterator linkMatches = linkRe.globalMatch(unclaimed);
    while (linkMatches.hasNext()) {
        const QRegularExpressionMatch match = linkMatches.next();
        const Span whole = span(match, 0);
        const Span content = span(match, 1);
        const int contentEnd = content.start + content.length;
        append({InlineKind::Link, content,
                {{whole.start, 1},
                 {contentEnd, whole.start + whole.length - contentEnd}},
                span(match, 2)});
    }

    return markup;
}

QString MarkdownHighlighter::linkUrlAt(const QString &text, int position) {
    if (position < 0 || position >= text.length())
        return {};

    const QList<InlineMarkup> markup = inlineMarkup(text);
    for (const InlineMarkup &item : markup) {
        if (item.kind != InlineKind::Link
                || position < item.content.start
                || position >= item.content.start + item.content.length) {
            continue;
        }
        // Markdown escapes punctuation in a destination, as the editor does
        // for parentheses; show the URL itself.
        static const QRegularExpression escapedPunctuation(
            QStringLiteral(R"(\\([!-/:-@\[-`{-~]))"));
        return text.mid(item.destination.start, item.destination.length)
            .replace(escapedPunctuation, QStringLiteral("\\1"));
    }
    return {};
}

#include "markdownhighlighter.h"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace {

constexpr int kProseMaxBlockLength = 4096;
constexpr int kAmbientEchoWindowWords = 50;
constexpr int kCaretEchoWindowWords = 80;
constexpr int kNeighborBlockLimit = 8;
constexpr int kLongSentenceWords = 28;
constexpr int kVeryLongSentenceWords = 40;
constexpr int kMonotoneMinWords = 6;

QColor blendColor(const QColor &from, const QColor &to, qreal amount) {
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

bool hasInlineDelimiters(const QString &text) {
    return text.contains(QLatin1Char('*')) || text.contains(QLatin1Char('_'))
        || text.contains(QLatin1Char('[')) || text.contains(QLatin1Char('~'));
}

} // namespace

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document) {
    rebuildFormats();
}

// One setter for the whole theme, so a theme change rehighlights once.
void MarkdownHighlighter::setTheme(bool darkMode, const QString &background,
                                   const QString &foreground, const QString &accent,
                                   const QString &codeBackground,
                                   const QString &yellow, const QString &red) {
    if (m_darkMode == darkMode && m_customBackground == background
            && m_customForeground == foreground && m_customAccent == accent
            && m_customCodeBackground == codeBackground
            && m_customYellow == yellow && m_customRed == red)
        return;

    m_darkMode = darkMode;
    m_customBackground = background;
    m_customForeground = foreground;
    m_customAccent = accent;
    m_customCodeBackground = codeBackground;
    m_customYellow = yellow;
    m_customRed = red;
    rebuildFormats();
    rehighlightAll();
}

void MarkdownHighlighter::setProseXRay(bool enabled) {
    if (m_proseXRay == enabled)
        return;

    m_proseXRay = enabled;
    if (m_proseXRay && !m_caret.isNull())
        m_caretEchoWord = caretWordAt(m_caret.block(), m_caret.positionInBlock());
    else
        m_caretEchoWord.clear();
    rehighlightAll();
}

void MarkdownHighlighter::rehighlightEchoNeighborhood(int position) {
    if (!m_proseXRay || !document())
        return;
    const int clamped = qBound(0, position, document()->characterCount() - 1);
    const QTextBlock block = document()->findBlock(clamped);
    if (!block.isValid())
        return;
    rehighlightNeighborhoodBlocks(block);
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
    if (!sameQuery)
        m_searchPattern = buildSearchPattern(query);
    m_currentMatchStart = currentMatchStart;
    m_currentMatch = QTextCursor();
    if (currentMatchStart >= 0 && document()) {
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

void MarkdownHighlighter::clearCurrentMatch() {
    m_currentMatchStart = -1;
    m_currentMatch = QTextCursor();
}

// QSyntaxHighlighter::rehighlight() marks the blocks dirty one at a time, and
// a laid-out document redoes layout work for the rest of itself after each
// one, so a full restyle grows with the square of its length: 40 s for a 2 MB
// file. A null page size makes the layout skip that work, so restyle under one
// and lay the document out once when the real size comes back.
//
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
    doc->setPageSize(QSizeF(0, 0));
    rehighlight();
    doc->setPageSize(pageSize);
    if (QAbstractTextDocumentLayout *layout = doc->documentLayout())
        layout->documentSize();
}

void MarkdownHighlighter::setCaret(int position) {
    if (!document())
        return;

    const int clamped = qBound(0, position, document()->characterCount() - 1);
    if (!m_caret.isNull() && m_caret.position() == clamped)
        return;

    const bool hadCaret = !m_caret.isNull();
    const QTextBlock left = hadCaret ? m_caret.block() : QTextBlock();
    const int oldOffset = hadCaret ? m_caret.positionInBlock() : -1;
    if (!hadCaret) {
        m_caret = QTextCursor(document());
        // Loading a file inserts at the caret, which must not drag it along
        // to the end; the editor reports where it really went.
        m_caret.setKeepPositionOnInsert(true);
    }
    m_caret.setPosition(clamped);

    const auto touchesMarkup = [](const QTextBlock &block, int offset) {
        if (!block.isValid() || offset < 0 || block.userState() >= InsideFence
                || block.previous().userState() >= InsideFence)
            return false;
        const QString text = block.text();
        if (!hasInlineDelimiters(text))
            return false;
        const QList<InlineMarkup> markup = inlineMarkup(text);
        return std::any_of(markup.cbegin(), markup.cend(), [offset](const InlineMarkup &item) {
            return offset >= item.markers[0].start
                && offset <= item.markers[1].start + item.markers[1].length;
        });
    };

    // Only the lines the caret left and entered can show different markers,
    // and only when the caret enters, leaves, or crosses an inline markup span.
    const QTextBlock entered = m_caret.block();
    const int newOffset = m_caret.positionInBlock();
    bool rehighlightedLeft = false;
    bool rehighlightedEntered = false;
    if (left != entered) {
        if (touchesMarkup(left, oldOffset)) {
            rehighlightBlock(left);
            rehighlightedLeft = true;
        }
        if (touchesMarkup(entered, newOffset)) {
            rehighlightBlock(entered);
            rehighlightedEntered = true;
        }
    } else if (entered.isValid() && entered.userState() < InsideFence
               && entered.previous().userState() < InsideFence) {
        const QString text = entered.text();
        if (hasInlineDelimiters(text)) {
            const QList<InlineMarkup> markup = inlineMarkup(text);
            const bool visibilityChanged = std::any_of(
                markup.cbegin(), markup.cend(), [&](const InlineMarkup &item) {
                    const int start = item.markers[0].start;
                    const int end = item.markers[1].start + item.markers[1].length;
                    return (oldOffset >= start && oldOffset <= end)
                        != (newOffset >= start && newOffset <= end);
                });
            if (visibilityChanged) {
                rehighlightBlock(entered);
                rehighlightedEntered = true;
            }
        }
    }

    if (!m_proseXRay)
        return;

    const QString nextEchoWord = caretWordAt(entered, newOffset);
    if (nextEchoWord == m_caretEchoWord && left == entered)
        return;
    const bool hadEchoWord = !m_caretEchoWord.isEmpty();
    const bool hasEchoWord = !nextEchoWord.isEmpty();
    m_caretEchoWord = nextEchoWord;
    if (hadEchoWord && left.isValid() && left != entered)
        rehighlightNeighborhoodBlocks(left);
    if (hasEchoWord || hadEchoWord) {
        if (entered.isValid())
            rehighlightNeighborhoodBlocks(entered);
    } else {
        Q_UNUSED(rehighlightedLeft);
        Q_UNUSED(rehighlightedEntered);
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
    const QColor warningYellow = !m_customYellow.isEmpty() ? QColor(m_customYellow)
        : (m_darkMode ? QColor(QStringLiteral("#e2c27a")) : QColor(QStringLiteral("#c98a00")));
    const QColor warningRed = !m_customRed.isEmpty() ? QColor(m_customRed)
        : (m_darkMode ? QColor(QStringLiteral("#d0786a")) : QColor(QStringLiteral("#c94a38")));
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
    m_fenceFormat = m_codeFormat;
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

    m_longSentenceFormat = QTextCharFormat();
    m_longSentenceFormat.setBackground(
        blendColor(background, warningYellow, m_darkMode ? 0.12 : 0.15));

    m_veryLongSentenceFormat = QTextCharFormat();
    m_veryLongSentenceFormat.setBackground(
        blendColor(background, warningRed, m_darkMode ? 0.17 : 0.20));

    m_monotoneSentenceFormat = QTextCharFormat();
    m_monotoneSentenceFormat.setUnderlineStyle(QTextCharFormat::DotLine);
    m_monotoneSentenceFormat.setUnderlineColor(
        blendColor(background, link, m_darkMode ? 0.65 : 0.55));

    m_ambientEchoFormat = QTextCharFormat();
    m_ambientEchoFormat.setUnderlineStyle(QTextCharFormat::DashUnderline);
    m_ambientEchoFormat.setUnderlineColor(warningYellow);

    m_caretEchoFormat = QTextCharFormat();
    m_caretEchoFormat.setBackground(
        blendColor(background, link, m_darkMode ? 0.24 : 0.20));
    m_caretEchoFormat.setUnderlineStyle(QTextCharFormat::SingleUnderline);
    m_caretEchoFormat.setUnderlineColor(link);
}

void MarkdownHighlighter::highlightBlock(const QString &text) {
    // A fenced run of code is literal from its opening fence through its
    // closing one, so no markup runs inside it. The state rides from block to
    // block, and Qt rehighlights the rest of the document when it changes.
    const int previous = previousBlockState();
    if (previous >= InsideFence) {
        const int fence = previous - InsideFence;
        const bool closing = closesFence(text, fence / 2 + 3,
                                         fence % 2 ? QLatin1Char('~') : QLatin1Char('`'));
        setCurrentBlockState(closing ? Prose : previous);
        setFormat(0, text.length(), closing ? m_fenceFormat : m_codeFormat);
        highlightSearch(text);
        return;
    }

    const int opening = openingFenceLength(text);
    if (opening > 0) {
        const bool tildes = text.trimmed().startsWith(QLatin1Char('~'));
        setCurrentBlockState(InsideFence + (opening - 3) * 2 + (tildes ? 1 : 0));
        setFormat(0, text.length(), m_fenceFormat);
        highlightSearch(text);
        return;
    }
    setCurrentBlockState(Prose);

    if (!text.isEmpty()) {
        highlightMarkers(text);
        if (text.contains(QLatin1Char('`')) || hasInlineDelimiters(text))
            highlightInline(text);
        if (m_proseXRay)
            highlightProseXRay(text);
    }
    highlightSearch(text);
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

// The text folded unit by unit through the table, folding surrogate pairs as
// the single letter each pair encodes, which stays a pair: 𐐀 becomes 𐐨.
QString searchFolded(const QString &text) {
    const char16_t *fold = searchFoldTable();
    const qsizetype size = text.size();
    QString folded(size, Qt::Uninitialized);
    const QChar *source = text.constData();
    QChar *target = folded.data();
    for (qsizetype i = 0; i < size; ++i) {
        const char16_t unit = source[i].unicode();
        if (QChar::isHighSurrogate(unit) && i + 1 < size && source[i + 1].isLowSurrogate()) {
            const char32_t caseFolded = QChar::toCaseFolded(
                QChar::surrogateToUcs4(unit, source[i + 1].unicode()));
            if (QChar::requiresSurrogates(caseFolded)) {
                target[i] = QChar(QChar::highSurrogate(caseFolded));
                target[i + 1] = QChar(QChar::lowSurrogate(caseFolded));
                ++i;
                continue;
            }
        }
        target[i] = QChar(fold[unit]);
    }
    return folded;
}

} // namespace

MarkdownHighlighter::SearchPattern MarkdownHighlighter::buildSearchPattern(const QString &query) {
    SearchPattern pattern;
    pattern.length = query.size();
    if (pattern.length == 0)
        return pattern;

    pattern.foldedQuery = searchFolded(query);
    // The query's accented letters, case-folded, which the text must match
    // as written rather than folded.
    for (qsizetype i = 0; i < pattern.length; ++i) {
        const QChar unit = query.at(i);
        const QChar caseFolded = unit.toCaseFolded();
        if (!unit.isSurrogate() && pattern.foldedQuery.at(i) != caseFolded)
            pattern.exact.append({i, commaBelow(caseFolded)});
    }
    return pattern;
}

QList<int> MarkdownHighlighter::findMatches(const QString &text, const SearchPattern &pattern) {
    QList<int> matches;
    if (pattern.length == 0 || text.size() < pattern.length)
        return matches;

    const QString foldedText = searchFolded(text);

    qsizetype from = 0;
    while ((from = foldedText.indexOf(pattern.foldedQuery, from)) >= 0) {
        const bool accentsMatch = std::all_of(
            pattern.exact.cbegin(), pattern.exact.cend(), [&](const auto &letter) {
                return commaBelow(text.at(from + letter.first).toCaseFolded()) == letter.second;
            });
        if (accentsMatch) {
            matches.append(int(from));
            from += pattern.length;
        } else {
            ++from;
        }
    }
    return matches;
}

QList<int> MarkdownHighlighter::findMatches(const QString &text, const QString &query) {
    if (query.isEmpty() || text.size() < query.size())
        return {};
    return findMatches(text, buildSearchPattern(query));
}

void MarkdownHighlighter::highlightSearch(const QString &text) {
    if (m_searchPattern.length == 0 || text.size() < m_searchPattern.length)
        return;

    const int length = m_searchQuery.length();
    for (const int from : findMatches(text, m_searchPattern)) {
        const int documentStart = currentBlock().position() + from;
        const QBrush background = documentStart == m_currentMatchStart
            ? m_currentSearchFormat.background()
            : m_searchFormat.background();
        for (int i = from; i < from + length; ++i) {
            QTextCharFormat format = this->format(i);
            if (format == m_hiddenMarkerFormat)
                continue;
            format.setBackground(background);
            setFormat(i, 1, format);
        }
    }
}

void MarkdownHighlighter::highlightMarkers(const QString &text) {
    int first = 0;
    while (first < text.length() && text.at(first).isSpace())
        ++first;
    if (first >= text.length())
        return;

    const QChar firstChar = text.at(first);
    if (first == 0 && firstChar == QLatin1Char('#')) {
        static const QRegularExpression headingRe(QStringLiteral("^(#{1,6})(\\s+)(.*)$"));
        const QRegularExpressionMatch heading = headingRe.match(text);
        if (heading.hasMatch()) {
            setFormat(0, heading.capturedLength(1) + heading.capturedLength(2),
                      m_markerFormat);
            setFormat(heading.capturedStart(3), heading.capturedLength(3),
                      m_headingFormat);
            return;
        }
    }

    if (firstChar == QLatin1Char('>')) {
        static const QRegularExpression quoteRe(QStringLiteral("^(\\s*>+\\s?)(.*)$"));
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
    const QList<Span> code = codeSpans(text);
    QList<bool> inCode(text.length(), false);
    for (const Span &span : code) {
        for (int i = span.start; i < span.start + span.length; ++i) {
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
    QList<InlineMarkup> markup = inlineMarkup(text, code);
    std::stable_sort(markup.begin(), markup.end(),
                     [](const InlineMarkup &a, const InlineMarkup &b) {
                         return a.content.length > b.content.length;
                     });
    for (const InlineMarkup &item : markup) {
        const QTextCharFormat &contentFormat =
            item.kind == InlineKind::Bold ? m_boldFormat
            : item.kind == InlineKind::Italic ? m_italicFormat
            : item.kind == InlineKind::Strikethrough ? struck
                                                     : m_linkFormat;
        for (int i = item.content.start; i < item.content.start + item.content.length; ++i) {
            if (!inCode[i] || item.kind == InlineKind::Strikethrough)
                mergeFormat(i, contentFormat);
        }
    }

    // Last, so no enclosing style shows a marker that should be hidden.
    const int caret = !m_caret.isNull() && m_caret.block() == currentBlock()
        ? m_caret.positionInBlock() : -1;
    for (const InlineMarkup &item : markup) {
        const bool atCaret = caret >= item.markers[0].start
            && caret <= item.markers[1].start + item.markers[1].length;
        for (const Span &marker : item.markers) {
            if (!atCaret) {
                setFormat(marker.start, marker.length, m_hiddenMarkerFormat);
                continue;
            }
            for (int i = marker.start; i < marker.start + marker.length; ++i)
                mergeFormat(i, m_markerFormat);
        }
    }
}

QList<MarkdownHighlighter::Span> MarkdownHighlighter::codeSpans(const QString &text) {
    QList<Span> spans;
    int from = text.indexOf(QLatin1Char('`'));
    if (from < 0)
        return spans;

    while (from >= 0) {
        int backslashes = 0;
        for (int i = from - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i)
            ++backslashes;
        if (backslashes % 2 != 0) {
            from = text.indexOf(QLatin1Char('`'), from + 1);
            continue;
        }

        // Backslashes escape opening backticks in prose, but are literal once
        // inside code, including immediately before the closing backtick.
        const int close = text.indexOf(QLatin1Char('`'), from + 1);
        if (close < 0)
            break;
        if (close == from + 1) {
            from = text.indexOf(QLatin1Char('`'), from + 1);
            continue;
        }
        spans.append({from, close - from + 1});
        from = text.indexOf(QLatin1Char('`'), close + 1);
    }
    return spans;
}

QList<MarkdownHighlighter::InlineMarkup> MarkdownHighlighter::inlineMarkup(const QString &text) {
    if (!hasInlineDelimiters(text))
        return {};
    return inlineMarkup(text, codeSpans(text));
}

QList<MarkdownHighlighter::InlineMarkup> MarkdownHighlighter::inlineMarkup(
    const QString &text, const QList<Span> &code) {
    QList<InlineMarkup> markup;
    const bool hasStar = text.contains(QLatin1Char('*'));
    const bool hasUnderscore = text.contains(QLatin1Char('_'));
    const bool hasBracket = text.contains(QLatin1Char('['));
    const bool hasTilde = text.contains(QLatin1Char('~'));
    if (!hasStar && !hasUnderscore && !hasBracket && !hasTilde)
        return markup;
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
    QString unclaimed;
    if (hasStar && text.contains(QLatin1String("***"))) {
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
            if (unclaimed.isEmpty())
                unclaimed = text;
            for (int i : {whole.start, whole.start + 1, whole.start + 2, end - 3, end - 2, end - 1})
                unclaimed[i] = QChar::ObjectReplacementCharacter;
        }
    }
    const QString &remaining = unclaimed.isEmpty() ? text : unclaimed;

    // Underscores only delimit emphasis at a word boundary, so identifiers such
    // as snake_case_name read as themselves. Asterisks delimit anywhere.
    if ((hasStar && remaining.contains(QLatin1String("**")))
            || (hasUnderscore && remaining.contains(QLatin1String("__")))) {
        static const QRegularExpression boldRe(
            QStringLiteral("\\*\\*(.+?)\\*\\*|(?<!\\w)__(.+?)__(?!\\w)"),
            QRegularExpression::UseUnicodePropertiesOption);
        QRegularExpressionMatchIterator boldMatches = boldRe.globalMatch(remaining);
        while (boldMatches.hasNext()) {
            const QRegularExpressionMatch match = boldMatches.next();
            const Span whole = span(match, 0);
            const int contentIndex = match.capturedStart(1) >= 0 ? 1 : 2;
            append({InlineKind::Bold, span(match, contentIndex),
                    {{whole.start, 2}, {whole.start + whole.length - 2, 2}}});
        }
    }

    // An asterisk delimiter may not be followed (opening) or preceded
    // (closing) by whitespace, so `* first *important* point` keeps its bullet
    // and `2 * 3 * 4` stays literal instead of pairing stray asterisks.
    if (hasStar || hasUnderscore) {
        static const QRegularExpression italicRe(
            QStringLiteral("(?<!\\*)\\*(?![\\s*])([^*\\n]*[^\\s*])\\*(?!\\*)|(?<!\\w)_([^_\\n]+)_(?!\\w)"),
            QRegularExpression::UseUnicodePropertiesOption);
        QRegularExpressionMatchIterator italicMatches = italicRe.globalMatch(remaining);
        while (italicMatches.hasNext()) {
            const QRegularExpressionMatch match = italicMatches.next();
            const Span whole = span(match, 0);
            const int contentIndex = match.capturedStart(1) >= 0 ? 1 : 2;
            append({InlineKind::Italic, span(match, contentIndex),
                    {{whole.start, 1}, {whole.start + whole.length - 1, 1}}});
        }
    }

    // Exactly two tildes, opening before text and closing after it.
    if (hasTilde && remaining.contains(QLatin1String("~~"))) {
        static const QRegularExpression strikeRe(
            QStringLiteral("(?<!~)~~(?![\\s~])(.*?[^\\s~])~~(?!~)"));
        QRegularExpressionMatchIterator strikeMatches = strikeRe.globalMatch(remaining);
        while (strikeMatches.hasNext()) {
            const QRegularExpressionMatch match = strikeMatches.next();
            const Span whole = span(match, 0);
            append({InlineKind::Strikethrough, span(match, 1),
                    {{whole.start, 2}, {whole.start + whole.length - 2, 2}}});
        }
    }

    if (hasBracket && remaining.contains(QLatin1String("]("))) {
        static const QRegularExpression linkRe(
            QStringLiteral("\\[([^\\]]+)\\]\\(((?:\\\\.|\\([^()]*\\)|[^()])+)\\)"));
        QRegularExpressionMatchIterator linkMatches = linkRe.globalMatch(remaining);
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

namespace {

enum class ProseBlockKind {
    Blank,
    SectionBoundary,
    ListItem,
    Paragraph,
    TooLong,
};

ProseBlockKind classifyProseBlock(const QTextBlock &block, int *contentStart = nullptr) {
    if (contentStart)
        *contentStart = 0;
    if (!block.isValid())
        return ProseBlockKind::SectionBoundary;
    if (block.userState() >= MarkdownHighlighter::InsideFence
            || block.previous().userState() >= MarkdownHighlighter::InsideFence)
        return ProseBlockKind::SectionBoundary;

    const QString text = block.text();
    if (text.length() > kProseMaxBlockLength)
        return ProseBlockKind::TooLong;

    int first = 0;
    while (first < text.length() && text.at(first).isSpace())
        ++first;
    if (first >= text.length())
        return ProseBlockKind::Blank;

    if (MarkdownHighlighter::openingFenceLength(text) > 0
            || MarkdownHighlighter::isThematicBreak(text))
        return ProseBlockKind::SectionBoundary;

    const QChar firstChar = text.at(first);
    if (first == 0 && firstChar == QLatin1Char('#')) {
        static const QRegularExpression headingRe(QStringLiteral("^#{1,6}\\s+.*$"));
        if (headingRe.match(text).hasMatch())
            return ProseBlockKind::SectionBoundary;
    }

    if (firstChar == QLatin1Char('>')) {
        static const QRegularExpression quoteRe(QStringLiteral("^\\s*>+\\s?.*$"));
        if (quoteRe.match(text).hasMatch())
            return ProseBlockKind::SectionBoundary;
    }

    if (firstChar == QLatin1Char('-') || firstChar == QLatin1Char('+')
            || firstChar == QLatin1Char('*') || firstChar.isDigit()) {
        static const QRegularExpression listRe(
            QStringLiteral("^(\\s*(?:[-+*]|\\d+[.)])\\s+)"));
        const QRegularExpressionMatch list = listRe.match(text);
        if (list.hasMatch()) {
            if (contentStart)
                *contentStart = int(list.capturedLength(1));
            return ProseBlockKind::ListItem;
        }
    }

    return ProseBlockKind::Paragraph;
}

QList<MarkdownHighlighter::Span> proseExcludedSpans(const QString &text, int contentStart) {
    QList<MarkdownHighlighter::Span> excluded;
    if (contentStart > 0)
        excluded.append({0, contentStart});

    const QList<MarkdownHighlighter::Span> code = MarkdownHighlighter::codeSpans(text);
    for (const auto &span : code)
        excluded.append(span);

    if (hasInlineDelimiters(text)) {
        const QList<MarkdownHighlighter::InlineMarkup> markup =
            MarkdownHighlighter::inlineMarkup(text);
        for (const auto &item : markup) {
            for (const auto &marker : item.markers) {
                if (marker.length > 0)
                    excluded.append(marker);
            }
        }
    }
    return excluded;
}

std::vector<bool> buildExcludedMask(int length, const QList<MarkdownHighlighter::Span> &excluded) {
    std::vector<bool> mask(size_t(length), false);
    for (const auto &span : excluded) {
        const int start = qBound(0, span.start, length);
        const int end = qBound(start, span.start + span.length, length);
        for (int i = start; i < end; ++i)
            mask[size_t(i)] = true;
    }
    return mask;
}

struct ProseWord {
    int start = 0;
    int length = 0;
    QString folded;
    int letterCount = 0;
    bool isCjk = false;
};

bool isCjkUnit(QChar ch) {
    const QChar::Script script = ch.script();
    return script == QChar::Script_Han
        || script == QChar::Script_Hiragana
        || script == QChar::Script_Katakana;
}

bool isWordJoiner(QChar ch) {
    return ch == QLatin1Char('\'') || ch == QLatin1Char('-') || ch == QChar(0x2019);
}

QList<ProseWord> extractProseWords(const QString &text, const std::vector<bool> &excluded) {
    QList<ProseWord> words;
    const int n = text.length();
    int i = 0;
    while (i < n) {
        if (excluded[size_t(i)]) {
            ++i;
            continue;
        }

        const QChar ch = text.at(i);
        if (isCjkUnit(ch)) {
            int j = i + 1;
            while (j < n && !excluded[size_t(j)] && isCjkUnit(text.at(j)))
                ++j;
            // Two-character CJK compounds catch real lexical echoes without
            // flagging single grammatical particles.
            for (int k = i; k + 1 < j; ++k) {
                const QString bigram = text.mid(k, 2);
                words.append({k, 2, searchFolded(bigram), 2, true});
            }
            i = j;
            continue;
        }

        if (!ch.isLetterOrNumber()) {
            ++i;
            continue;
        }

        const int start = i;
        int letters = 0;
        while (i < n && !excluded[size_t(i)]) {
            const QChar c = text.at(i);
            if (isCjkUnit(c))
                break;
            if (c.isLetter()) {
                ++letters;
                ++i;
            } else if (c.isNumber()) {
                ++i;
            } else if (isWordJoiner(c) && i + 1 < n && !excluded[size_t(i + 1)]
                       && text.at(i + 1).isLetterOrNumber() && !isCjkUnit(text.at(i + 1))) {
                ++i;
            } else {
                break;
            }
        }
        if (letters > 0) {
            const int len = i - start;
            words.append({start, len, searchFolded(text.mid(start, len)), letters, false});
        }
    }
    return words;
}

bool isEchoStopword(const QString &folded) {
    // Compact sorted table of closed-class function words (>= 5 letters,
    // diacritic- and case-folded via searchFolded) so ambient echo radar stays
    // quiet on high-frequency grammar words across core languages.
    static constexpr std::array<QLatin1String, 173> kStopwords = {
        QLatin1String("about"),      QLatin1String("above"),      QLatin1String("after"),
        QLatin1String("again"),      QLatin1String("against"),    QLatin1String("ainsi"),
        QLatin1String("allem"),      QLatin1String("allen"),      QLatin1String("aller"),
        QLatin1String("alles"),      QLatin1String("almost"),     QLatin1String("along"),
        QLatin1String("alors"),      QLatin1String("already"),    QLatin1String("although"),
        QLatin1String("always"),     QLatin1String("among"),      QLatin1String("andere"),
        QLatin1String("anderem"),    QLatin1String("anderen"),    QLatin1String("anderer"),
        QLatin1String("anderes"),    QLatin1String("another"),    QLatin1String("apres"),
        QLatin1String("around"),     QLatin1String("aussi"),      QLatin1String("autre"),
        QLatin1String("autres"),     QLatin1String("avant"),      QLatin1String("because"),
        QLatin1String("before"),     QLatin1String("beide"),      QLatin1String("beiden"),
        QLatin1String("being"),      QLatin1String("below"),      QLatin1String("bereits"),
        QLatin1String("between"),    QLatin1String("bisher"),     QLatin1String("cannot"),
        QLatin1String("cette"),      QLatin1String("comme"),      QLatin1String("could"),
        QLatin1String("dabei"),      QLatin1String("dadurch"),    QLatin1String("dafur"),
        QLatin1String("dagegen"),    QLatin1String("daher"),      QLatin1String("damit"),
        QLatin1String("danach"),     QLatin1String("daran"),      QLatin1String("darauf"),
        QLatin1String("daraus"),     QLatin1String("darin"),      QLatin1String("daruber"),
        QLatin1String("darum"),      QLatin1String("davon"),      QLatin1String("davor"),
        QLatin1String("deine"),      QLatin1String("deinem"),     QLatin1String("deinen"),
        QLatin1String("deiner"),     QLatin1String("deines"),     QLatin1String("denen"),
        QLatin1String("deren"),      QLatin1String("desde"),      QLatin1String("deshalb"),
        QLatin1String("dessen"),     QLatin1String("diese"),      QLatin1String("diesem"),
        QLatin1String("diesen"),     QLatin1String("dieser"),     QLatin1String("dieses"),
        QLatin1String("doing"),      QLatin1String("donde"),      QLatin1String("durch"),
        QLatin1String("durfen"),     QLatin1String("during"),     QLatin1String("einem"),
        QLatin1String("einen"),      QLatin1String("einer"),      QLatin1String("eines"),
        QLatin1String("einige"),     QLatin1String("einigen"),    QLatin1String("either"),
        QLatin1String("enough"),     QLatin1String("entre"),      QLatin1String("estas"),
        QLatin1String("estos"),      QLatin1String("etwas"),      QLatin1String("every"),
        QLatin1String("first"),      QLatin1String("gegen"),      QLatin1String("gewesen"),
        QLatin1String("haben"),      QLatin1String("hasta"),      QLatin1String("hatte"),
        QLatin1String("hatten"),     QLatin1String("having"),     QLatin1String("heraus"),
        QLatin1String("herein"),     QLatin1String("hinter"),     QLatin1String("however"),
        QLatin1String("ihnen"),      QLatin1String("ihrem"),      QLatin1String("ihren"),
        QLatin1String("ihrer"),      QLatin1String("ihres"),      QLatin1String("immer"),
        QLatin1String("indem"),      QLatin1String("itself"),     QLatin1String("jedem"),
        QLatin1String("jeden"),      QLatin1String("jeder"),      QLatin1String("jedes"),
        QLatin1String("jedoch"),     QLatin1String("jetzt"),      QLatin1String("keine"),
        QLatin1String("keinem"),     QLatin1String("keinen"),     QLatin1String("keiner"),
        QLatin1String("keines"),     QLatin1String("konnen"),     QLatin1String("konnte"),
        QLatin1String("konnten"),    QLatin1String("manche"),     QLatin1String("manchen"),
        QLatin1String("meine"),      QLatin1String("meinem"),     QLatin1String("meinen"),
        QLatin1String("meiner"),     QLatin1String("meines"),     QLatin1String("might"),
        QLatin1String("mussen"),     QLatin1String("musste"),     QLatin1String("never"),
        QLatin1String("nicht"),      QLatin1String("nichts"),     QLatin1String("notre"),
        QLatin1String("nuestro"),    QLatin1String("often"),      QLatin1String("other"),
        QLatin1String("others"),     QLatin1String("ought"),      QLatin1String("parce"),
        QLatin1String("pendant"),    QLatin1String("perche"),     QLatin1String("porque"),
        QLatin1String("quand"),      QLatin1String("quello"),     QLatin1String("questa"),
        QLatin1String("questo"),     QLatin1String("schon"),      QLatin1String("seine"),
        QLatin1String("seinem"),     QLatin1String("seinen"),     QLatin1String("seiner"),
        QLatin1String("seines"),     QLatin1String("selbst"),     QLatin1String("shall"),
        QLatin1String("should"),     QLatin1String("since"),      QLatin1String("sobald"),
        QLatin1String("sobre"),      QLatin1String("sodass"),     QLatin1String("solche"),
        QLatin1String("solchen"),    QLatin1String("sollen"),     QLatin1String("sollte"),
        QLatin1String("sondern"),    QLatin1String("sonst"),      QLatin1String("sowie"),
        QLatin1String("still"),
    };
    static constexpr std::array<QLatin1String, 47> kStopwordsTail = {
        QLatin1String("tengo"),      QLatin1String("their"),      QLatin1String("theirs"),
        QLatin1String("themselves"), QLatin1String("there"),      QLatin1String("these"),
        QLatin1String("thing"),      QLatin1String("things"),     QLatin1String("those"),
        QLatin1String("though"),     QLatin1String("through"),    QLatin1String("todos"),
        QLatin1String("toute"),      QLatin1String("toutes"),     QLatin1String("under"),
        QLatin1String("unser"),      QLatin1String("unsere"),     QLatin1String("unserem"),
        QLatin1String("unseren"),    QLatin1String("unserer"),    QLatin1String("unter"),
        QLatin1String("until"),      QLatin1String("viele"),      QLatin1String("vielen"),
        QLatin1String("vieler"),     QLatin1String("votre"),      QLatin1String("waren"),
        QLatin1String("warum"),      QLatin1String("weder"),      QLatin1String("wegen"),
        QLatin1String("weiter"),     QLatin1String("welche"),     QLatin1String("welchem"),
        QLatin1String("welchen"),    QLatin1String("welcher"),    QLatin1String("welches"),
        QLatin1String("werden"),     QLatin1String("where"),      QLatin1String("whereas"),
        QLatin1String("whether"),    QLatin1String("which"),      QLatin1String("while"),
        QLatin1String("whose"),      QLatin1String("wieder"),     QLatin1String("within"),
        QLatin1String("without"),    QLatin1String("wobei"),
    };
    static constexpr std::array<QLatin1String, 9> kStopwordsEnd = {
        QLatin1String("wofur"),      QLatin1String("wollen"),     QLatin1String("wollte"),
        QLatin1String("worden"),     QLatin1String("would"),      QLatin1String("wurde"),
        QLatin1String("wurden"),     QLatin1String("yours"),      QLatin1String("yourself"),
    };
    if (folded == QLatin1String("zwischen"))
        return true;
    const auto inTable = [&folded](const auto &table) {
        return std::binary_search(table.cbegin(), table.cend(), folded,
                                  [](const auto &a, const auto &b) { return a < b; });
    };
    return inTable(kStopwords) || inTable(kStopwordsTail) || inTable(kStopwordsEnd);
}

bool isSentenceTerminator(QChar ch) {
    return ch == QLatin1Char('.') || ch == QLatin1Char('!') || ch == QLatin1Char('?')
        || ch == QChar(0x2026) || ch == QChar(0x3002) || ch == QChar(0xFF01)
        || ch == QChar(0xFF1F) || ch == QChar(0x061F);
}

bool isClosingTrailChar(QChar ch) {
    return ch == QLatin1Char('"') || ch == QLatin1Char('\'') || ch == QLatin1Char(')')
        || ch == QLatin1Char(']') || ch == QChar(0x201D) || ch == QChar(0x201C)
        || ch == QChar(0x2019) || ch == QChar(0x00BB) || ch == QChar(0x00AB)
        || ch == QChar(0x300D) || ch == QChar(0x300F);
}

bool isSentenceAbbreviation(const QString &text, int dotIndex, const std::vector<bool> &excluded) {
    if (dotIndex <= 0)
        return true;
    // Decimal numbers and version strings ("1.5", "v0.7") or ellipsis ("...").
    if (dotIndex + 1 < text.length() && !excluded[size_t(dotIndex + 1)]) {
        const QChar next = text.at(dotIndex + 1);
        if (next == QLatin1Char('.') || next.isDigit() || next.isLetter())
            return true;
    }
    int start = dotIndex - 1;
    if (excluded[size_t(start)])
        return false;
    // Ordinal dates and numbered references ("28. November", "1. Kapitel").
    if (text.at(start).isDigit())
        return true;
    while (start > 0 && !excluded[size_t(start - 1)] && text.at(start - 1).isLetter())
        --start;
    const int len = dotIndex - start;
    if (len <= 0)
        return false;
    // Single-letter abbreviations ("z. B.", "d. h.", "e. g.", "A. Smith").
    if (len == 1)
        return true;

    const QString token = searchFolded(text.mid(start, len));
    static constexpr std::array<QLatin1String, 31> kAbbreviations = {
        QLatin1String("abb"),  QLatin1String("abs"),  QLatin1String("apr"),  QLatin1String("aug"),
        QLatin1String("bzw"),  QLatin1String("ca"),   QLatin1String("dez"),  QLatin1String("dr"),
        QLatin1String("etc"),  QLatin1String("feb"),  QLatin1String("ggf"),  QLatin1String("inkl"),
        QLatin1String("jan"),  QLatin1String("jul"),  QLatin1String("jun"),  QLatin1String("kap"),
        QLatin1String("max"),  QLatin1String("min"),  QLatin1String("mr"),   QLatin1String("mrs"),
        QLatin1String("ms"),   QLatin1String("nov"),  QLatin1String("nr"),   QLatin1String("okt"),
        QLatin1String("prof"), QLatin1String("sep"),  QLatin1String("str"),  QLatin1String("usw"),
        QLatin1String("vgl"),  QLatin1String("vs"),   QLatin1String("zb"),
    };
    return std::binary_search(kAbbreviations.cbegin(), kAbbreviations.cend(), token,
                              [](const auto &a, const auto &b) { return a < b; });
}

int countProseWordsInRange(const QString &text, const std::vector<bool> &excluded, int from, int to) {
    int count = 0;
    bool inWord = false;
    bool pendingJoiner = false;
    for (int i = from; i < to; ++i) {
        if (excluded[size_t(i)]) {
            inWord = false;
            pendingJoiner = false;
            continue;
        }
        const QChar ch = text.at(i);
        if (isCjkUnit(ch)) {
            ++count;
            inWord = false;
            pendingJoiner = false;
        } else if (ch.isLetterOrNumber()) {
            if (!inWord)
                ++count;
            inWord = true;
            pendingJoiner = false;
        } else if (isWordJoiner(ch)) {
            if (inWord && !pendingJoiner) {
                pendingJoiner = true;
            } else {
                inWord = false;
                pendingJoiner = false;
            }
        } else {
            inWord = false;
            pendingJoiner = false;
        }
    }
    return count;
}

bool isMonotoneTriple(int a, int b, int c) {
    const int lo = std::min({a, b, c});
    const int hi = std::max({a, b, c});
    if (lo < kMonotoneMinWords)
        return false;
    const double mean = (a + b + c) / 3.0;
    const int tolerance = std::max(2, int(std::lround(mean * 0.20)));
    return (hi - lo) <= tolerance;
}

} // namespace

QList<MarkdownHighlighter::ProseSentence> MarkdownHighlighter::proseSentences(
    const QString &text, const QList<Span> &excluded) {
    QList<ProseSentence> sentences;
    const int n = text.length();
    if (n == 0)
        return sentences;

    const std::vector<bool> mask = buildExcludedMask(n, excluded);
    int segStart = 0;
    while (segStart < n && (mask[size_t(segStart)] || text.at(segStart).isSpace()))
        ++segStart;

    int i = segStart;
    while (i < n) {
        if (mask[size_t(i)]) {
            ++i;
            continue;
        }
        const QChar ch = text.at(i);
        if (isSentenceTerminator(ch)) {
            if (ch == QLatin1Char('.') && isSentenceAbbreviation(text, i, mask)) {
                ++i;
                continue;
            }
            int segEnd = i + 1;
            while (segEnd < n && !mask[size_t(segEnd)]
                   && (isSentenceTerminator(text.at(segEnd)) || isClosingTrailChar(text.at(segEnd))))
                ++segEnd;
            const int wc = countProseWordsInRange(text, mask, segStart, segEnd);
            if (wc > 0)
                sentences.append({{segStart, segEnd - segStart}, wc});
            i = segEnd;
            while (i < n && (mask[size_t(i)] || text.at(i).isSpace()))
                ++i;
            segStart = i;
            continue;
        }
        ++i;
    }

    if (segStart < n) {
        int segEnd = n;
        while (segEnd > segStart && (mask[size_t(segEnd - 1)] || text.at(segEnd - 1).isSpace()))
            --segEnd;
        const int wc = countProseWordsInRange(text, mask, segStart, segEnd);
        if (wc > 0)
            sentences.append({{segStart, segEnd - segStart}, wc});
    }
    return sentences;
}

QString MarkdownHighlighter::caretWordAt(const QTextBlock &block, int offset) const {
    int contentStart = 0;
    const ProseBlockKind kind = classifyProseBlock(block, &contentStart);
    if (kind != ProseBlockKind::Paragraph && kind != ProseBlockKind::ListItem)
        return {};

    const QString text = block.text();
    const std::vector<bool> mask = buildExcludedMask(text.length(), proseExcludedSpans(text, contentStart));
    const QList<ProseWord> words = extractProseWords(text, mask);
    for (const ProseWord &word : words) {
        if (offset >= word.start && offset <= word.start + word.length) {
            if (word.isCjk || word.letterCount >= 4)
                return word.folded;
            return {};
        }
    }
    return {};
}

void MarkdownHighlighter::rehighlightNeighborhoodBlocks(const QTextBlock &center) {
    if (!center.isValid())
        return;

    QList<QTextBlock> targets;
    targets.append(center);

    QTextBlock prev = center.previous();
    int visited = 0;
    while (prev.isValid() && visited < kNeighborBlockLimit) {
        const ProseBlockKind kind = classifyProseBlock(prev);
        if (kind == ProseBlockKind::SectionBoundary)
            break;
        if (kind == ProseBlockKind::Paragraph || kind == ProseBlockKind::ListItem) {
            targets.append(prev);
            ++visited;
        }
        prev = prev.previous();
    }

    QTextBlock next = center.next();
    visited = 0;
    while (next.isValid() && visited < kNeighborBlockLimit) {
        const ProseBlockKind kind = classifyProseBlock(next);
        if (kind == ProseBlockKind::SectionBoundary)
            break;
        if (kind == ProseBlockKind::Paragraph || kind == ProseBlockKind::ListItem) {
            targets.append(next);
            ++visited;
        }
        next = next.next();
    }

    for (const QTextBlock &b : targets)
        rehighlightBlock(b);
}

void MarkdownHighlighter::highlightProseXRay(const QString &text) {
    const QTextBlock block = currentBlock();
    int contentStart = 0;
    const ProseBlockKind kind = classifyProseBlock(block, &contentStart);
    if (kind != ProseBlockKind::Paragraph && kind != ProseBlockKind::ListItem)
        return;

    const QList<Span> excluded = proseExcludedSpans(text, contentStart);
    const std::vector<bool> mask = buildExcludedMask(text.length(), excluded);

    const auto mergeNonHidden = [this, &mask](int start, int length, const QTextCharFormat &extra) {
        const int end = start + length;
        for (int i = start; i < end; ++i) {
            if (mask[size_t(i)])
                continue;
            QTextCharFormat current = this->format(i);
            if (current == m_hiddenMarkerFormat)
                continue;
            current.merge(extra);
            setFormat(i, 1, current);
        }
    };

    // 1. Sentence cadence: overlong sentences (28+ / 40+ words) and monotone
    // length streaks (3+ consecutive prose sentences within +/-20%).
    const QList<ProseSentence> currentSentences = proseSentences(text, excluded);
    for (const ProseSentence &sentence : currentSentences) {
        if (sentence.wordCount >= kVeryLongSentenceWords)
            mergeNonHidden(sentence.span.start, sentence.span.length, m_veryLongSentenceFormat);
        else if (sentence.wordCount >= kLongSentenceWords)
            mergeNonHidden(sentence.span.start, sentence.span.length, m_longSentenceFormat);
    }

    if (kind == ProseBlockKind::Paragraph && !currentSentences.isEmpty()) {
        QList<int> prevLengths;
        QTextBlock prev = block.previous();
        int visited = 0;
        while (prev.isValid() && visited < kNeighborBlockLimit && prevLengths.size() < 2) {
            int prevStart = 0;
            const ProseBlockKind prevKind = classifyProseBlock(prev, &prevStart);
            if (prevKind == ProseBlockKind::SectionBoundary || prevKind == ProseBlockKind::ListItem)
                break;
            if (prevKind == ProseBlockKind::Paragraph) {
                const QString prevText = prev.text();
                const QList<ProseSentence> prevSents =
                    proseSentences(prevText, proseExcludedSpans(prevText, prevStart));
                for (int idx = int(prevSents.size()) - 1; idx >= 0 && prevLengths.size() < 2; --idx)
                    prevLengths.prepend(prevSents.at(idx).wordCount);
                ++visited;
            }
            prev = prev.previous();
        }

        QList<int> nextLengths;
        QTextBlock next = block.next();
        visited = 0;
        while (next.isValid() && visited < kNeighborBlockLimit && nextLengths.size() < 2) {
            int nextStart = 0;
            const ProseBlockKind nextKind = classifyProseBlock(next, &nextStart);
            if (nextKind == ProseBlockKind::SectionBoundary || nextKind == ProseBlockKind::ListItem)
                break;
            if (nextKind == ProseBlockKind::Paragraph) {
                const QString nextText = next.text();
                const QList<ProseSentence> nextSents =
                    proseSentences(nextText, proseExcludedSpans(nextText, nextStart));
                for (int idx = 0; idx < nextSents.size() && nextLengths.size() < 2; ++idx)
                    nextLengths.append(nextSents.at(idx).wordCount);
                ++visited;
            }
            next = next.next();
        }

        QList<int> combined;
        combined.reserve(prevLengths.size() + currentSentences.size() + nextLengths.size());
        for (int len : prevLengths)
            combined.append(len);
        const int currentOffset = int(combined.size());
        for (const ProseSentence &s : currentSentences)
            combined.append(s.wordCount);
        for (int len : nextLengths)
            combined.append(len);

        std::vector<bool> monotone(size_t(combined.size()), false);
        for (int idx = 0; idx + 2 < combined.size(); ++idx) {
            if (isMonotoneTriple(combined.at(idx), combined.at(idx + 1), combined.at(idx + 2))) {
                monotone[size_t(idx)] = true;
                monotone[size_t(idx + 1)] = true;
                monotone[size_t(idx + 2)] = true;
            }
        }
        for (int idx = 0; idx < currentSentences.size(); ++idx) {
            if (monotone[size_t(currentOffset + idx)]) {
                mergeNonHidden(currentSentences.at(idx).span.start,
                               currentSentences.at(idx).span.length,
                               m_monotoneSentenceFormat);
            }
        }
    }

    // 2. Word Echo Radar: bounded +/-80-word neighborhood across adjacent prose
    // blocks, stopping at headings, blockquotes, and code fences.
    const QList<ProseWord> currentWords = extractProseWords(text, mask);
    if (currentWords.isEmpty())
        return;

    struct NeighborWord {
        QString folded;
        int blockNumber = -1;
        int wordIndex = -1;
    };

    QList<NeighborWord> prevWords;
    int caretGlobalIndex = -1;
    int caretPrevRevIndex = -1;
    const int caretBlockNum = !m_caret.isNull() ? m_caret.blockNumber() : -1;
    const int caretOffset = !m_caret.isNull() ? m_caret.positionInBlock() : -1;

    QTextBlock prev = block.previous();
    int visited = 0;
    while (prev.isValid() && visited < kNeighborBlockLimit && prevWords.size() < kCaretEchoWindowWords) {
        int prevStart = 0;
        const ProseBlockKind prevKind = classifyProseBlock(prev, &prevStart);
        if (prevKind == ProseBlockKind::SectionBoundary)
            break;
        if (prevKind == ProseBlockKind::Paragraph || prevKind == ProseBlockKind::ListItem) {
            const QString prevText = prev.text();
            const std::vector<bool> prevMask =
                buildExcludedMask(prevText.length(), proseExcludedSpans(prevText, prevStart));
            const QList<ProseWord> words = extractProseWords(prevText, prevMask);
            for (int idx = int(words.size()) - 1;
                 idx >= 0 && prevWords.size() < kCaretEchoWindowWords; --idx) {
                const ProseWord &w = words.at(idx);
                if (prev.blockNumber() == caretBlockNum && caretOffset >= w.start
                        && caretOffset <= w.start + w.length)
                    caretPrevRevIndex = int(prevWords.size());
                prevWords.prepend({w.folded, prev.blockNumber(), idx});
            }
            ++visited;
        }
        prev = prev.previous();
    }
    if (caretPrevRevIndex >= 0)
        caretGlobalIndex = int(prevWords.size()) - 1 - caretPrevRevIndex;

    QList<NeighborWord> nextWords;
    QTextBlock next = block.next();
    visited = 0;
    while (next.isValid() && visited < kNeighborBlockLimit && nextWords.size() < kCaretEchoWindowWords) {
        int nextStart = 0;
        const ProseBlockKind nextKind = classifyProseBlock(next, &nextStart);
        if (nextKind == ProseBlockKind::SectionBoundary)
            break;
        if (nextKind == ProseBlockKind::Paragraph || nextKind == ProseBlockKind::ListItem) {
            const QString nextText = next.text();
            const std::vector<bool> nextMask =
                buildExcludedMask(nextText.length(), proseExcludedSpans(nextText, nextStart));
            const QList<ProseWord> words = extractProseWords(nextText, nextMask);
            for (int idx = 0; idx < words.size() && nextWords.size() < kCaretEchoWindowWords; ++idx) {
                const ProseWord &w = words.at(idx);
                if (next.blockNumber() == caretBlockNum && caretOffset >= w.start
                        && caretOffset <= w.start + w.length)
                    caretGlobalIndex = int(prevWords.size()) + int(currentWords.size())
                        + int(nextWords.size());
                nextWords.append({w.folded, next.blockNumber(), idx});
            }
            ++visited;
        }
        next = next.next();
    }

    const int currentBase = int(prevWords.size());
    if (block.blockNumber() == caretBlockNum) {
        for (int idx = 0; idx < currentWords.size(); ++idx) {
            const ProseWord &w = currentWords.at(idx);
            if (caretOffset >= w.start && caretOffset <= w.start + w.length) {
                caretGlobalIndex = currentBase + idx;
                break;
            }
        }
    }

    QList<QString> allFolded;
    allFolded.reserve(prevWords.size() + currentWords.size() + nextWords.size());
    for (const auto &w : prevWords)
        allFolded.append(w.folded);
    for (const auto &w : currentWords)
        allFolded.append(w.folded);
    for (const auto &w : nextWords)
        allFolded.append(w.folded);

    // Caret Echo Focus activates only when the focused word has at least one
    // twin inside +/-80 words, keeping unique words calm.
    bool caretWordHasTwin = false;
    if (!m_caretEchoWord.isEmpty() && caretGlobalIndex >= 0) {
        const int lo = std::max(0, caretGlobalIndex - kCaretEchoWindowWords);
        const int hi = std::min(int(allFolded.size()) - 1, caretGlobalIndex + kCaretEchoWindowWords);
        int matches = 0;
        for (int idx = lo; idx <= hi; ++idx) {
            if (allFolded.at(idx) == m_caretEchoWord) {
                if (++matches >= 2) {
                    caretWordHasTwin = true;
                    break;
                }
            }
        }
    }

    for (int idx = 0; idx < currentWords.size(); ++idx) {
        const ProseWord &w = currentWords.at(idx);
        const int globalIdx = currentBase + idx;

        if (caretWordHasTwin && w.folded == m_caretEchoWord
                && std::abs(globalIdx - caretGlobalIndex) <= kCaretEchoWindowWords) {
            mergeNonHidden(w.start, w.length, m_caretEchoFormat);
            continue;
        }

        if (!w.isCjk && (w.letterCount < 5 || isEchoStopword(w.folded)))
            continue;

        const int lo = std::max(0, globalIdx - kAmbientEchoWindowWords);
        const int hi = std::min(int(allFolded.size()) - 1, globalIdx + kAmbientEchoWindowWords);
        bool echoed = false;
        for (int other = lo; other <= hi; ++other) {
            if (other != globalIdx && allFolded.at(other) == w.folded) {
                echoed = true;
                break;
            }
        }
        if (echoed)
            mergeNonHidden(w.start, w.length, m_ambientEchoFormat);
    }
}

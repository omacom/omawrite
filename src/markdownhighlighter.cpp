#include "markdownhighlighter.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>
#include <utility>
#include <vector>

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document) {
    rebuildFormats();
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
    rebuildFormats();
    rehighlight();
}

// The hidden marker spacing is measured from the document's default font, so
// it has to be remeasured whenever the editor switches typeface.
void MarkdownHighlighter::refreshFont() {
    rebuildFormats();
    rehighlight();
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
        rehighlight();
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
        if (text.contains(QLatin1Char('`')) || text.contains(QLatin1Char('*'))
            || text.contains(QLatin1Char('_')) || text.contains(QLatin1Char('['))
            || text.contains(QLatin1Char('~'))) {
            highlightInline(text);
        }
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
// syllables would fold to their first jamo. Surrogates map to themselves.
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

} // namespace

QList<int> MarkdownHighlighter::findMatches(const QString &text, const QString &query) {
    QList<int> matches;
    const qsizetype length = query.size();
    if (length == 0 || text.size() < length)
        return matches;

    const char16_t *fold = searchFoldTable();
    QString foldedQuery(length, Qt::Uninitialized);
    // The query's accented letters, case-folded, which the text must match
    // as written rather than folded.
    QList<std::pair<qsizetype, QChar>> exact;
    for (qsizetype i = 0; i < length; ++i) {
        const QChar unit = query.at(i);
        foldedQuery[i] = QChar(fold[unit.unicode()]);
        const QChar caseFolded = unit.toCaseFolded();
        if (foldedQuery.at(i) != caseFolded)
            exact.append({i, commaBelow(caseFolded)});
    }

    QString foldedText(text.size(), Qt::Uninitialized);
    const QChar *source = text.constData();
    QChar *target = foldedText.data();
    for (qsizetype i = 0; i < text.size(); ++i)
        target[i] = QChar(fold[source[i].unicode()]);

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
        QTextCharFormat format = this->format(from);
        format.setBackground(documentStart == m_currentMatchStart
                                 ? m_currentSearchFormat.background()
                                 : m_searchFormat.background());
        setFormat(from, m_searchQuery.length(), format);
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
    QList<bool> inCode(text.length(), false);
    for (const Span &code : codeSpans(text)) {
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
    QList<InlineMarkup> markup = inlineMarkup(text);
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

    // Last, so no enclosing style shows a marker that the caret skips over.
    for (const InlineMarkup &item : markup) {
        for (const Span &marker : item.markers)
            setFormat(marker.start, marker.length, m_hiddenMarkerFormat);
    }
}

QList<MarkdownHighlighter::Span> MarkdownHighlighter::codeSpans(const QString &text) {
    QList<Span> spans;
    if (!text.contains(QLatin1Char('`')))
        return spans;

    static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
    int from = 0;
    while ((from = text.indexOf(QLatin1Char('`'), from)) >= 0) {
        int backslashes = 0;
        for (int i = from - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i)
            ++backslashes;
        if (backslashes % 2 != 0) {
            ++from;
            continue;
        }

        // Backslashes escape opening backticks in prose, but are literal once
        // inside code, including immediately before the closing backtick.
        const auto match = codeRe.match(text, from, QRegularExpression::NormalMatch,
                                        QRegularExpression::AnchorAtOffsetMatchOption);
        if (!match.hasMatch()) {
            ++from;
            continue;
        }
        spans.append({from, int(match.capturedLength(0))});
        from = match.capturedEnd(0);
    }
    return spans;
}

QList<MarkdownHighlighter::InlineMarkup> MarkdownHighlighter::inlineMarkup(const QString &text) {
    QList<InlineMarkup> markup;
    if (!text.contains(QLatin1Char('*')) && !text.contains(QLatin1Char('_'))
            && !text.contains(QLatin1Char('[')) && !text.contains(QLatin1Char('~'))) {
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

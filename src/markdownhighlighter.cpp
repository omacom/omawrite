#include "markdownhighlighter.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QTextDocument>

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document) {
    rebuildFormats();
}

void MarkdownHighlighter::setDarkMode(bool darkMode) {
    if (m_darkMode == darkMode)
        return;

    m_darkMode = darkMode;
    rebuildFormats();
    rehighlight();
}

void MarkdownHighlighter::setColors(const QString &background, const QString &foreground,
                                    const QString &accent) {
    if (m_customBackground == background && m_customForeground == foreground
            && m_customAccent == accent)
        return;

    m_customBackground = background;
    m_customForeground = foreground;
    m_customAccent = accent;
    rebuildFormats();
    rehighlight();
}

void MarkdownHighlighter::setSearch(const QString &query, int currentMatchStart) {
    if (m_searchQuery == query && m_currentMatchStart == currentMatchStart)
        return;
    m_searchQuery = query;
    m_currentMatchStart = currentMatchStart;
    rehighlight();
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
    const QColor codeBackground = m_darkMode ? QColor(QStringLiteral("#1c1a1a"))
                                             : QColor(QStringLiteral("#f8f8f8"));

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
    m_boldFormat.setForeground(text);

    m_italicFormat = QTextCharFormat();
    m_italicFormat.setFontItalic(true);
    m_italicFormat.setForeground(text);

    m_codeFormat = QTextCharFormat();
    m_codeFormat.setForeground(text);
    m_codeFormat.setBackground(codeBackground);

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
    if (!text.isEmpty()) {
        highlightMarkers(text);
        if (text.contains(QLatin1Char('`')) || text.contains(QLatin1Char('*'))
            || text.contains(QLatin1Char('_')) || text.contains(QLatin1Char('['))
            || text.contains(QLatin1Char('<')) || text.contains(QLatin1String("http"))) {
            highlightInline(text);
        }
    }
    highlightSearch(text);
}

void MarkdownHighlighter::highlightSearch(const QString &text) {
    if (m_searchQuery.isEmpty())
        return;

    int from = 0;
    while ((from = text.indexOf(m_searchQuery, from, Qt::CaseInsensitive)) >= 0) {
        const int documentStart = currentBlock().position() + from;
        QTextCharFormat format = this->format(from);
        format.setBackground(documentStart == m_currentMatchStart
                                 ? m_currentSearchFormat.background()
                                 : m_searchFormat.background());
        setFormat(from, m_searchQuery.length(), format);
        from += qMax(1, m_searchQuery.length());
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

    if (firstChar == QLatin1Char('-') || firstChar == QLatin1Char('*')
            || firstChar == QLatin1Char('_')) {
        static const QRegularExpression ruleRe(QStringLiteral("^\\s{0,3}([-*_])(?:\\s*\\1){2,}\\s*$"));
        const QRegularExpressionMatch rule = ruleRe.match(text);
        if (rule.hasMatch())
            setFormat(0, text.length(), m_markerFormat);
    }
}

void MarkdownHighlighter::highlightInline(const QString &text) {
    if (text.contains(QLatin1Char('`'))) {
        static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
        QRegularExpressionMatchIterator codeMatches = codeRe.globalMatch(text);
        while (codeMatches.hasNext()) {
            const QRegularExpressionMatch match = codeMatches.next();
            setFormat(match.capturedStart(0), match.capturedLength(0), m_codeFormat);
        }
    }

    const QList<InlineMarkup> markup = inlineMarkup(text);
    for (const InlineMarkup &item : markup) {
        const QTextCharFormat &contentFormat =
            item.kind == InlineKind::Bold ? m_boldFormat
            : item.kind == InlineKind::Italic ? m_italicFormat
                                              : m_linkFormat;
        setFormat(item.content.start, item.content.length, contentFormat);
        for (const Span &marker : item.markers)
            setFormat(marker.start, marker.length, m_hiddenMarkerFormat);
    }

    // Bare URLs are not in inlineMarkup; paint those without touching hidden
    // markers. Markup links already have content styled above. Clickable spans
    // cover the whole [[...]] / []() so hit-testing includes collapsed markers.
    for (const Clickable &item : clickableSpans(text)) {
        if (item.kind != InlineKind::Link || item.span.length != item.target.length())
            continue;
        setFormat(item.span.start, item.span.length, m_linkFormat);
    }
}

static bool insideCodeSpan(const QString &text, int start, int length) {
    static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
    QRegularExpressionMatchIterator matches = codeRe.globalMatch(text);
    const int end = start + length;
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        if (start >= match.capturedStart(0) && end <= match.capturedEnd(0))
            return true;
    }
    return false;
}

static bool overlaps(MarkdownHighlighter::Span a, int start, int length) {
    const int aEnd = a.start + a.length;
    const int bEnd = start + length;
    return start < aEnd && a.start < bEnd;
}

static MarkdownHighlighter::Span fullSpan(const MarkdownHighlighter::InlineMarkup &item) {
    int start = item.content.start;
    int end = item.content.start + item.content.length;
    for (const MarkdownHighlighter::Span &marker : item.markers) {
        if (marker.length <= 0)
            continue;
        start = qMin(start, marker.start);
        end = qMax(end, marker.start + marker.length);
    }
    return {start, end - start};
}

QList<MarkdownHighlighter::InlineMarkup> MarkdownHighlighter::inlineMarkup(const QString &text) {
    QList<InlineMarkup> markup;
    if (!text.contains(QLatin1Char('*')) && !text.contains(QLatin1Char('_'))
            && !text.contains(QLatin1Char('[')) && !text.contains(QLatin1Char('<'))) {
        return markup;
    }

    const auto span = [](const QRegularExpressionMatch &match, int group) {
        return Span{int(match.capturedStart(group)), int(match.capturedLength(group))};
    };

    static const QRegularExpression boldRe(QStringLiteral("(\\*\\*|__)(.+?)(\\1)"));
    QRegularExpressionMatchIterator boldMatches = boldRe.globalMatch(text);
    while (boldMatches.hasNext()) {
        const QRegularExpressionMatch match = boldMatches.next();
        markup.append({InlineKind::Bold, span(match, 2),
                       {span(match, 1), span(match, 3)}, {}});
    }

    static const QRegularExpression italicRe(
        QStringLiteral("(?<!\\*)\\*([^*\\n]+)\\*(?!\\*)|(?<!_)_([^_\\n]+)_(?!_)"));
    QRegularExpressionMatchIterator italicMatches = italicRe.globalMatch(text);
    while (italicMatches.hasNext()) {
        const QRegularExpressionMatch match = italicMatches.next();
        const Span whole = span(match, 0);
        const int contentIndex = match.capturedStart(1) >= 0 ? 1 : 2;
        markup.append({InlineKind::Italic, span(match, contentIndex),
                       {{whole.start, 1}, {whole.start + whole.length - 1, 1}}, {}});
    }

    static const QRegularExpression wikiRe(
        QStringLiteral("\\[\\[([^\\]|#]+)(?:#[^\\]|]*)?(?:\\|([^\\]]+))?\\]\\]"));
    QRegularExpressionMatchIterator wikiMatches = wikiRe.globalMatch(text);
    while (wikiMatches.hasNext()) {
        const QRegularExpressionMatch match = wikiMatches.next();
        if (insideCodeSpan(text, match.capturedStart(0), match.capturedLength(0)))
            continue;
        const Span whole = span(match, 0);
        const QString target = match.captured(1).trimmed();
        const bool hasAlias = match.lastCapturedIndex() >= 2 && match.capturedStart(2) >= 0
            && !match.captured(2).isEmpty();
        const Span content = hasAlias ? span(match, 2) : span(match, 1);
        const int prefixLen = content.start - whole.start;
        const int suffixStart = content.start + content.length;
        markup.append({InlineKind::WikiLink, content,
                       {{whole.start, prefixLen},
                        {suffixStart, whole.start + whole.length - suffixStart}},
                       target});
    }

    static const QRegularExpression linkRe(
        QStringLiteral("\\[([^\\]]+)\\]\\(((?:\\\\.|[^)])+)\\)"));
    QRegularExpressionMatchIterator linkMatches = linkRe.globalMatch(text);
    while (linkMatches.hasNext()) {
        const QRegularExpressionMatch match = linkMatches.next();
        if (insideCodeSpan(text, match.capturedStart(0), match.capturedLength(0)))
            continue;
        const Span whole = span(match, 0);
        const Span content = span(match, 1);
        const int contentEnd = content.start + content.length;
        QString destination = match.captured(2);
        destination.replace(QStringLiteral("\\("), QStringLiteral("("));
        destination.replace(QStringLiteral("\\)"), QStringLiteral(")"));
        destination.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        markup.append({InlineKind::Link, content,
                       {{whole.start, 1},
                        {contentEnd, whole.start + whole.length - contentEnd}},
                       destination});
    }

    static const QRegularExpression autoRe(
        QStringLiteral("<(https?://[^>\\s]+)>"));
    QRegularExpressionMatchIterator autoMatches = autoRe.globalMatch(text);
    while (autoMatches.hasNext()) {
        const QRegularExpressionMatch match = autoMatches.next();
        if (insideCodeSpan(text, match.capturedStart(0), match.capturedLength(0)))
            continue;
        const Span whole = span(match, 0);
        markup.append({InlineKind::Link, span(match, 1),
                       {{whole.start, 1}, {whole.start + whole.length - 1, 1}},
                       match.captured(1)});
    }

    return markup;
}

QList<MarkdownHighlighter::Clickable> MarkdownHighlighter::clickableSpans(const QString &text) {
    QList<Clickable> spans;
    const QList<InlineMarkup> markup = inlineMarkup(text);
    QList<Span> taken;
    for (const InlineMarkup &item : markup) {
        if (item.kind != InlineKind::Link && item.kind != InlineKind::WikiLink)
            continue;
        const Span whole = fullSpan(item);
        spans.append({item.kind, whole, item.target});
        taken.append(whole);
    }

    static const QRegularExpression bareRe(
        QStringLiteral("\\bhttps?://[^\\s<>\\]\\)]+"));
    QRegularExpressionMatchIterator bareMatches = bareRe.globalMatch(text);
    while (bareMatches.hasNext()) {
        const QRegularExpressionMatch match = bareMatches.next();
        const int start = int(match.capturedStart(0));
        const int length = int(match.capturedLength(0));
        if (insideCodeSpan(text, start, length))
            continue;
        bool covered = false;
        for (const Span &span : taken) {
            if (overlaps(span, start, length)) {
                covered = true;
                break;
            }
        }
        if (covered)
            continue;
        QString url = match.captured(0);
        while (url.endsWith(QLatin1Char('.')) || url.endsWith(QLatin1Char(','))
               || url.endsWith(QLatin1Char(';')) || url.endsWith(QLatin1Char(':')))
            url.chop(1);
        spans.append({InlineKind::Link, {start, int(url.length())}, url});
    }
    return spans;
}

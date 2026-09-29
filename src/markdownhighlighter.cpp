#include "markdownhighlighter.h"

#include "mathrenderer.h"

#include <QAbstractTextDocumentLayout>
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtMath>

#include <algorithm>
#include <memory>

namespace {
// Stands in for math while markup is matched, so markup can neither start nor
// end inside a formula. A private-use character no markup pattern names.
constexpr QChar mathMask(0xE000);

// Renders arriving within a window share one refresh, so a document full of
// formulas does not reflow once per formula. The window is twice what the
// last refresh cost, which in a long note is mostly Qt laying the text out.
constexpr int minimumMathRenderBatchMs = 40;
constexpr int maximumMathRenderBatchMs = 1000;

struct MathBox {
    qreal width;
    qreal height;
    qreal depth; // below the baseline
};

// What the highlighter hid in a block and the space it held in its place,
// kept so the overlay draws exactly what the text layout made room for.
class MathBlockData : public QTextBlockUserData {
public:
    struct Formula {
        int offset;
        int length;
        bool standalone;
        MathBox box;
        QByteArray svg;
        QString tex;
    };
    QList<Formula> formulas;
};

qreal mathAvailableWidth(const QTextDocument *document) {
    const qreal width = document->textWidth();
    return width > 0 ? width - 2 * document->documentMargin() : 0;
}

// MathJax measures in ex of its TeX font; matching that to the editor font's
// x-height sizes math the way MathJax does next to text in a browser.
MathBox mathBox(const MathRenderer::Result &result, const QTextDocument *document) {
    const qreal ex = QFontMetricsF(document->defaultFont()).xHeight();
    qreal scale = ex;
    const qreal available = mathAvailableWidth(document);
    if (available > 0 && result.width * ex > available)
        scale = available / result.width;
    return {result.width * scale, result.height * scale, result.depth * scale};
}

bool isBlank(const QString &text, int from, int to) {
    for (int i = from; i < to; ++i) {
        if (!text.at(i).isSpace())
            return false;
    }
    return true;
}

// The next unescaped occurrence of a closing delimiter in [from, to), or -1.
int findClosing(const QString &text, QStringView delimiter, int from, int to) {
    for (int i = from; i + delimiter.size() <= to; ++i) {
        if (QStringView(text).mid(i, delimiter.size()) == delimiter)
            return i;
        if (text.at(i) == QLatin1Char('\\'))
            ++i;
    }
    return -1;
}

// Pandoc's rule for a closing $: no space before it and no digit after it,
// which keeps prices like "$5 and $10" out of math. Code spans bind tighter
// than math, so a backtick ends the search.
int findInlineClosing(const QString &text, int from, int to) {
    for (int i = from; i < to; ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('`')) {
            return -1;
        } else if (c == QLatin1Char('\\')) {
            ++i;
        } else if (c == QLatin1Char('$') && !text.at(i - 1).isSpace()
                   && !(i + 1 < text.size() && text.at(i + 1).isDigit())) {
            return i;
        }
    }
    return -1;
}

// Completes a two-character delimited span opening at `open`. Display math
// alone on its line may close on a later line, before any blank line.
bool closeMath(const QString &text, int open, int lineStart, int lineEnd, QStringView closing,
               bool multiline, MarkdownHighlighter::MathSpan &span) {
    const int contentStart = open + 2;
    int close = findClosing(text, closing, contentStart, lineEnd);
    bool standalone = false;
    if (close < 0) {
        if (!multiline || !isBlank(text, lineStart, open))
            return false;
        for (int from = lineEnd + 1; from <= text.size() && close < 0;) {
            int to = text.indexOf(QLatin1Char('\n'), from);
            if (to < 0)
                to = text.size();
            if (isBlank(text, from, to))
                return false;
            close = findClosing(text, closing, from, to);
            if (close >= 0 && !isBlank(text, close + 2, to))
                return false;
            lineEnd = to;
            from = to + 1;
        }
        if (close < 0)
            return false;
        standalone = true;
    } else {
        standalone = span.display && isBlank(text, lineStart, open)
                     && isBlank(text, close + 2, lineEnd);
    }
    if (isBlank(text, contentStart, close))
        return false;

    span.start = open;
    span.end = close + 2;
    span.delimiter = 2;
    span.standalone = standalone;
    span.tex = text.mid(contentStart, close - contentStart);
    span.outerStart = standalone ? lineStart : span.start;
    span.outerEnd = standalone ? lineEnd : span.end;
    return true;
}

bool sameMath(const MarkdownHighlighter::MathSpan &a, const MarkdownHighlighter::MathSpan &b) {
    return a.start == b.start && a.end == b.end && a.outerStart == b.outerStart
           && a.outerEnd == b.outerEnd && a.display == b.display
           && a.standalone == b.standalone && a.tex == b.tex;
}

QString maskedText(const QString &text, const QList<MarkdownHighlighter::Span> &masked) {
    QString visible = text;
    for (const MarkdownHighlighter::Span &span : masked)
        visible.replace(span.start, span.length, QString(span.length, mathMask));
    return visible;
}
}

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(static_cast<QObject *>(document)) {
    // Index math before QSyntaxHighlighter reacts to the same change, so the
    // blocks it re-highlights already see the new formulas: slots run in the
    // order they were connected, and setDocument() connects the highlighter.
    connect(document, &QTextDocument::contentsChange, this,
            &MarkdownHighlighter::updateMathIndex);
    connect(document->documentLayout(), &QAbstractTextDocumentLayout::update, this,
            &MarkdownHighlighter::documentLayoutChanged);
    m_mathRefreshTimer.setSingleShot(true);
    connect(&m_mathRefreshTimer, &QTimer::timeout, this, &MarkdownHighlighter::refreshMath);
    m_math = mathSpans(document->toPlainText());
    m_mathCaret = QTextCursor(document);
    m_mathFont = document->defaultFont();
    m_mathTextWidth = document->textWidth();

    setDocument(document);
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

    m_mathErrorFormat = QTextCharFormat();
    m_mathErrorFormat.setForeground(m_darkMode ? QColor(QStringLiteral("#e5534b"))
                                               : QColor(QStringLiteral("#c9302c")));

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
            || text.contains(QLatin1Char('_')) || text.contains(QLatin1Char('['))) {
            highlightInline(text, mathSegments(currentBlock()));
        }
    }
    highlightMath(text);
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

void MarkdownHighlighter::highlightInline(const QString &text, const QList<Span> &math) {
    if (text.contains(QLatin1Char('`'))) {
        static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
        QRegularExpressionMatchIterator codeMatches = codeRe.globalMatch(maskedText(text, math));
        while (codeMatches.hasNext()) {
            const QRegularExpressionMatch match = codeMatches.next();
            setFormat(match.capturedStart(0), match.capturedLength(0), m_codeFormat);
        }
    }

    const QList<InlineMarkup> markup = inlineMarkup(text, math);
    for (const InlineMarkup &item : markup) {
        const QTextCharFormat &contentFormat =
            item.kind == InlineKind::Bold ? m_boldFormat
            : item.kind == InlineKind::Italic ? m_italicFormat
                                              : m_linkFormat;
        setFormat(item.content.start, item.content.length, contentFormat);
        for (const Span &marker : item.markers)
            setFormat(marker.start, marker.length, m_hiddenMarkerFormat);
    }
}

QList<MarkdownHighlighter::InlineMarkup> MarkdownHighlighter::inlineMarkup(
        const QString &text, const QList<Span> &masked) {
    if (!masked.isEmpty())
        return inlineMarkup(maskedText(text, masked));

    QList<InlineMarkup> markup;
    if (!text.contains(QLatin1Char('*')) && !text.contains(QLatin1Char('_'))
            && !text.contains(QLatin1Char('['))) {
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
                       {span(match, 1), span(match, 3)}});
    }

    static const QRegularExpression italicRe(
        QStringLiteral("(?<!\\*)\\*([^*\\n]+)\\*(?!\\*)|(?<!_)_([^_\\n]+)_(?!_)"));
    QRegularExpressionMatchIterator italicMatches = italicRe.globalMatch(text);
    while (italicMatches.hasNext()) {
        const QRegularExpressionMatch match = italicMatches.next();
        const Span whole = span(match, 0);
        const int contentIndex = match.capturedStart(1) >= 0 ? 1 : 2;
        markup.append({InlineKind::Italic, span(match, contentIndex),
                       {{whole.start, 1}, {whole.start + whole.length - 1, 1}}});
    }

    static const QRegularExpression linkRe(
        QStringLiteral("\\[([^\\]]+)\\]\\(((?:\\\\.|[^)])+)\\)"));
    QRegularExpressionMatchIterator linkMatches = linkRe.globalMatch(text);
    while (linkMatches.hasNext()) {
        const QRegularExpressionMatch match = linkMatches.next();
        const Span whole = span(match, 0);
        const Span content = span(match, 1);
        const int contentEnd = content.start + content.length;
        markup.append({InlineKind::Link, content,
                       {{whole.start, 1},
                        {contentEnd, whole.start + whole.length - contentEnd}}});
    }

    return markup;
}

namespace {
// The character that holds a formula's place: as wide as the formula, in a
// font size that makes its line as tall as the formula needs. It is drawn
// transparent under the formula, and the overlay covers it when selected.
QTextCharFormat mathSpaceFormat(const QTextDocument *document, const QTextBlock &block,
                                const MathBox &box, bool standalone, QChar glyph) {
    const QFont font = document->defaultFont();
    const QFontMetricsF metrics(font);
    const qreal ascent = metrics.ascent();
    const qreal descent = metrics.descent();
    const qreal height = ascent + descent;
    qreal scale = 1;
    if (standalone) {
        const QTextBlockFormat blockFormat = block.blockFormat();
        const qreal spacing =
            blockFormat.lineHeightType() == QTextBlockFormat::ProportionalHeight
                ? blockFormat.lineHeight() / 100 : 1;
        // The formula and half a line of air, over the spaced line.
        scale = (box.height + height / 2) / (height * spacing);
    } else {
        // Inline math may reach a little into the leading above and below
        // before its line has to grow, so subscripts keep the spacing even.
        scale = qMax((box.height - box.depth) / (ascent + 0.2 * height),
                     box.depth / (descent + 0.35 * height));
    }

    const int basePixelSize = font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize();
    const int pixelSize = qCeil(basePixelSize * qMax<qreal>(1, scale));
    QFont spaceFont = font;
    spaceFont.setPixelSize(pixelSize);

    QTextCharFormat format;
    format.setForeground(Qt::transparent);
    format.setProperty(QTextFormat::FontPixelSize, pixelSize);
    format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    format.setFontLetterSpacing(box.width - QFontMetricsF(spaceFont).horizontalAdvance(glyph));
    return format;
}

const MathBlockData::Formula *placedFormula(const QTextBlock &block,
                                            const MarkdownHighlighter::MathSpan &span) {
    const auto *data = static_cast<const MathBlockData *>(block.userData());
    if (!data)
        return nullptr;
    const int offset = span.start - block.position();
    for (const MathBlockData::Formula &formula : data->formulas) {
        if (formula.offset == offset && formula.length == span.end - span.start)
            return &formula;
    }
    return nullptr;
}
}

QList<MarkdownHighlighter::MathSpan> MarkdownHighlighter::mathSpans(const QString &text) {
    QList<MathSpan> spans;
    if (!text.contains(QLatin1Char('$')) && !text.contains(QLatin1Char('\\')))
        return spans;

    const int length = int(text.size());
    QChar fence;
    int fenceLength = 0;
    int lineStart = 0;
    while (lineStart <= length) {
        int lineEnd = int(text.indexOf(QLatin1Char('\n'), lineStart));
        if (lineEnd < 0)
            lineEnd = length;
        int nextLine = lineEnd + 1;

        // Fenced code opens and closes with ``` or ~~~, indented up to three spaces.
        int marker = lineStart;
        while (marker < lineEnd && marker - lineStart < 3 && text.at(marker) == QLatin1Char(' '))
            ++marker;
        int run = 0;
        if (marker < lineEnd && (text.at(marker) == QLatin1Char('`') || text.at(marker) == QLatin1Char('~'))) {
            while (marker + run < lineEnd && text.at(marker + run) == text.at(marker))
                ++run;
        }
        if (fenceLength > 0) {
            if (run >= fenceLength && text.at(marker) == fence && isBlank(text, marker + run, lineEnd))
                fenceLength = 0;
            lineStart = nextLine;
            continue;
        }
        if (run >= 3) {
            fence = text.at(marker);
            fenceLength = run;
            lineStart = nextLine;
            continue;
        }

        int position = lineStart;
        while (position < lineEnd) {
            const QChar c = text.at(position);
            if (c == QLatin1Char('`')) {
                // A code span closes with a run of exactly as many backticks.
                int ticks = 1;
                while (position + ticks < lineEnd && text.at(position + ticks) == QLatin1Char('`'))
                    ++ticks;
                int close = position + ticks;
                while (close < lineEnd) {
                    close = int(text.indexOf(QLatin1Char('`'), close));
                    if (close < 0 || close >= lineEnd) {
                        close = lineEnd;
                        break;
                    }
                    int closeTicks = 1;
                    while (close + closeTicks < lineEnd && text.at(close + closeTicks) == QLatin1Char('`'))
                        ++closeTicks;
                    if (closeTicks == ticks)
                        break;
                    close += closeTicks;
                }
                position = close < lineEnd ? close + ticks : position + ticks;
                continue;
            }

            MathSpan span;
            if (c == QLatin1Char('\\')) {
                const QChar opener = position + 1 < lineEnd ? text.at(position + 1) : QChar();
                span.display = opener == QLatin1Char('[');
                const bool opens = (opener == QLatin1Char('(') || span.display)
                    && closeMath(text, position, lineStart, lineEnd,
                                 span.display ? u"\\]" : u"\\)", span.display, span)
                    && (span.standalone || !span.display);
                if (!opens) {
                    position += 2; // an escaped character, \$ and \[ included
                    continue;
                }
            } else if (c == QLatin1Char('$') && position + 1 < lineEnd
                       && text.at(position + 1) == QLatin1Char('$')) {
                span.display = true;
                if (!closeMath(text, position, lineStart, lineEnd, u"$$", true, span)) {
                    position += 2;
                    continue;
                }
            } else if (c == QLatin1Char('$')) {
                const int close = position + 1 < lineEnd && !text.at(position + 1).isSpace()
                    ? findInlineClosing(text, position + 1, lineEnd) : -1;
                if (close < 0) {
                    ++position;
                    continue;
                }
                span.start = span.outerStart = position;
                span.end = span.outerEnd = close + 1;
                span.tex = text.mid(position + 1, close - position - 1);
            } else {
                ++position;
                continue;
            }

            spans.append(span);
            position = span.end;
            if (span.end > lineEnd) {
                // Display math that closed on a later line; scanning resumes
                // on the line after it.
                lineEnd = int(text.indexOf(QLatin1Char('\n'), span.end));
                nextLine = lineEnd < 0 ? length + 1 : lineEnd + 1;
                break;
            }
        }
        lineStart = nextLine;
    }
    return spans;
}

void MarkdownHighlighter::setMathRenderer(MathRenderer *renderer) {
    if (m_mathRenderer)
        disconnect(m_mathRenderer, nullptr, this, nullptr);
    m_mathRenderer = renderer;
    if (renderer)
        connect(renderer, &MathRenderer::rendered, this, &MarkdownHighlighter::mathRendered);
    updateMathPreamble();
    for (const MathSpan &span : std::as_const(m_math))
        scheduleMathRefresh(span.start, span.end);
}

void MarkdownHighlighter::updateMathPreamble() {
    if (!m_mathRenderer)
        return;

    static const QRegularExpression definitionRe(QStringLiteral(
        "\\\\(?:newcommand|renewcommand|newenvironment|renewenvironment|def|let"
        "|DeclareMathOperator)(?![A-Za-z])"));
    const int caret = m_mathCaret.position();
    QStringList definitions;
    for (const MathSpan &span : std::as_const(m_math)) {
        if (!definitionRe.match(span.tex).hasMatch())
            continue;
        // A definition being edited applies once the caret leaves it, rather
        // than typesetting every formula again at each keystroke.
        if (caret >= span.outerStart && caret <= span.outerEnd)
            return;
        definitions.append(span.tex);
    }
    if (m_mathRenderer->setPreamble(definitions)) {
        for (const MathSpan &span : std::as_const(m_math))
            scheduleMathRefresh(span.start, span.end);
    }
}

void MarkdownHighlighter::setMathCaret(int position) {
    if (!document())
        return;
    const int before = mathSpanAt(m_mathCaret.position());
    m_mathCaret.setPosition(qBound(0, position, document()->characterCount() - 1));
    const int after = mathSpanAt(m_mathCaret.position());
    if (m_mathRenderer)
        m_mathRenderer->setFocus(m_mathCaret.position());
    if (before == after)
        return;
    for (const int index : {before, after}) {
        if (index >= 0)
            scheduleMathRefresh(m_math.at(index).outerStart, m_math.at(index).outerEnd);
    }
    updateMathPreamble();
}

QList<MarkdownHighlighter::Span> MarkdownHighlighter::mathSegments(const QTextBlock &block) const {
    QList<Span> segments;
    const int blockStart = block.position();
    const int blockEnd = blockStart + block.length() - 1;
    for (int i = firstMathSpanAfter(blockStart);
         i < m_math.size() && m_math.at(i).start < blockEnd; ++i) {
        const int from = qMax(m_math.at(i).start, blockStart);
        segments.append({from - blockStart, qMin(m_math.at(i).end, blockEnd) - from});
    }
    return segments;
}

void MarkdownHighlighter::highlightMath(const QString &text) {
    const int blockStart = currentBlock().position();
    const int blockEnd = blockStart + int(text.size());
    const int caret = m_mathCaret.position();
    std::unique_ptr<MathBlockData> data;
    if (m_mathRenderer)
        m_mathRenderer->setFocus(caret);

    for (int i = firstMathSpanAfter(blockStart);
         i < m_math.size() && m_math.at(i).start < blockEnd; ++i) {
        const MathSpan &span = m_math.at(i);
        const int from = qMax(span.start, blockStart) - blockStart;
        const int to = qMin(span.end, blockEnd) - blockStart;
        const bool revealed = caret >= span.outerStart && caret <= span.outerEnd;
        const std::optional<MathRenderer::Result> result = revealed || !m_mathRenderer
            ? std::nullopt : m_mathRenderer->result(span.tex, span.display, span.start);

        // A formula being typeset again, as after a macro definition changed,
        // keeps its previous rendering meanwhile instead of flashing its source.
        MathBox box{};
        QByteArray svg;
        const MathBlockData::Formula *previous =
            placedFormula(document()->findBlock(span.start), span);
        if (result && result->ok) {
            box = mathBox(*result, document());
            svg = result->svg;
        } else if (!result && !revealed && previous && previous->tex == span.tex) {
            box = previous->box;
            svg = previous->svg;
        } else {
            // The source shows while it is being edited, while MathJax is
            // still typesetting it, and in the error colour if MathJax failed.
            if (result)
                setFormat(from, to - from, m_mathErrorFormat);
            if (span.start >= blockStart)
                setFormat(span.start - blockStart, span.delimiter, m_markerFormat);
            if (span.end <= blockEnd)
                setFormat(span.end - blockStart - span.delimiter, span.delimiter, m_markerFormat);
            continue;
        }

        if (span.standalone)
            setFormat(0, int(text.size()), m_hiddenMarkerFormat);
        else
            setFormat(from, to - from, m_hiddenMarkerFormat);
        if (span.start < blockStart)
            continue;

        const int offset = span.start - blockStart;
        setFormat(offset, 1, mathSpaceFormat(document(), currentBlock(), box, span.standalone,
                                             text.at(offset)));
        if (!data)
            data = std::make_unique<MathBlockData>();
        data->formulas.append({offset, span.end - span.start, span.standalone, box, svg, span.tex});
    }
    setCurrentBlockUserData(data.release());
}

QList<MarkdownHighlighter::MathPlacement> MarkdownHighlighter::mathPlacements() const {
    QList<MathPlacement> placements;
    QTextDocument *doc = document();
    if (!doc)
        return placements;

    QAbstractTextDocumentLayout *layout = doc->documentLayout();
    for (const MathSpan &span : m_math) {
        const QTextBlock block = doc->findBlock(span.start);
        const MathBlockData::Formula *formula = placedFormula(block, span);
        const QTextLayout *textLayout = block.layout();
        if (!formula || !textLayout)
            continue;
        const QTextLine line = textLayout->lineForTextPosition(formula->offset);
        if (!line.isValid())
            continue;

        const QPointF origin = layout->blockBoundingRect(block).topLeft();
        const qreal top = origin.y() + line.y();
        const QSizeF size(formula->box.width, formula->box.height);
        MathPlacement placement{{}, {}, span.start, formula->svg};
        if (formula->standalone) {
            // Centered in the text column and between the formula's lines.
            const QTextBlock last = doc->findBlock(span.end);
            const QTextBlock after = last.next();
            const qreal bottom = after.isValid() ? layout->blockBoundingRect(after).top()
                                                 : layout->blockBoundingRect(last).bottom();
            const qreal left = origin.x() + line.x();
            placement.image = QRectF(QPointF(left + (line.width() - size.width()) / 2,
                                             top + (bottom - top - size.height()) / 2), size);
            placement.backing = QRectF(left, top, line.width(), line.height());
        } else {
            const qreal x = origin.x() + line.cursorToX(formula->offset);
            const qreal baseline = top + line.ascent();
            placement.image = QRectF(QPointF(x, baseline - size.height() + formula->box.depth), size);
            placement.backing = QRectF(x, top, size.width(), line.height());
        }
        placements.append(placement);
    }
    return placements;
}

void MarkdownHighlighter::updateMathIndex(int position, int charsRemoved, int charsAdded) {
    const QList<MathSpan> previous = std::exchange(m_math, mathSpans(document()->toPlainText()));
    if (previous.isEmpty() && m_math.isEmpty())
        return;
    updateMathPreamble();

    // A formula the edit left as it was is highlighted correctly already.
    // Any other one is refreshed, which covers the blocks QSyntaxHighlighter
    // does not revisit: an opening $$ lines above the edit, or math below a
    // code fence that the edit opened or closed.
    const int delta = charsAdded - charsRemoved;
    const auto map = [&](int p) {
        if (p < position)
            return p;
        if (p >= position + charsRemoved)
            return p + delta;
        return charsRemoved == charsAdded ? p : position;
    };
    int i = 0;
    int j = 0;
    while (i < previous.size() || j < m_math.size()) {
        MathSpan old;
        if (i < previous.size()) {
            old = previous.at(i);
            old.start = map(old.start);
            old.end = map(old.end);
            old.outerStart = map(old.outerStart);
            old.outerEnd = map(old.outerEnd);
        }
        if (i < previous.size() && j < m_math.size() && sameMath(old, m_math.at(j))) {
            ++i;
            ++j;
        } else if (j >= m_math.size() || (i < previous.size() && old.start <= m_math.at(j).start)) {
            scheduleMathRefresh(old.start, qMax(old.start, old.end));
            ++i;
        } else {
            scheduleMathRefresh(m_math.at(j).start, m_math.at(j).end);
            ++j;
        }
    }
}

void MarkdownHighlighter::mathRendered(const QString &tex, bool display) {
    for (const MathSpan &span : std::as_const(m_math)) {
        if (span.display == display && span.tex == tex)
            scheduleMathRefresh(span.start, span.end, m_mathRenderBatchMs);
    }
}

void MarkdownHighlighter::documentLayoutChanged() {
    const QTextDocument *doc = document();
    if (!doc)
        return;

    // The space held for a formula depends on the editor font, and on the
    // text width for formulas scaled down to fit it; neither change reaches
    // the highlighter by itself.
    const bool fontChanged = doc->defaultFont() != m_mathFont;
    if (fontChanged || doc->textWidth() != m_mathTextWidth) {
        const qreal narrower = qMin(doc->textWidth(), m_mathTextWidth) - 2 * doc->documentMargin();
        m_mathFont = doc->defaultFont();
        m_mathTextWidth = doc->textWidth();
        for (const MathSpan &span : std::as_const(m_math)) {
            const MathBlockData::Formula *formula = placedFormula(doc->findBlock(span.start), span);
            if (fontChanged || (formula && formula->box.width >= narrower - 0.5))
                scheduleMathRefresh(span.start, span.end);
        }
    }
    emit mathLayoutChanged();
}

void MarkdownHighlighter::scheduleMathRefresh(int start, int end, int delay) {
    m_mathRefresh.append({start, end});
    if (!m_mathRefreshTimer.isActive() || m_mathRefreshTimer.remainingTime() > delay)
        m_mathRefreshTimer.start(delay);
}

void MarkdownHighlighter::refreshMath() {
    const QList<std::pair<int, int>> ranges = std::exchange(m_mathRefresh, {});
    QTextDocument *doc = document();
    if (!doc)
        return;

    QElapsedTimer cost;
    cost.start();
    // One edit block, so the editor re-renders once for the whole batch.
    const int last = doc->characterCount() - 1;
    QSet<int> refreshed;
    QTextCursor batch(doc);
    batch.beginEditBlock();
    for (const auto &[start, end] : ranges) {
        const int stop = qBound(0, end, last);
        for (QTextBlock block = doc->findBlock(qBound(0, start, last));
             block.isValid() && block.position() <= stop; block = block.next()) {
            if (!refreshed.contains(block.blockNumber())) {
                refreshed.insert(block.blockNumber());
                rehighlightBlock(block);
            }
        }
    }
    batch.endEditBlock();
    m_mathRenderBatchMs = qBound(minimumMathRenderBatchMs, int(2 * cost.elapsed()),
                                 maximumMathRenderBatchMs);
}

int MarkdownHighlighter::mathSpanAt(int position) const {
    // The caret at either edge of a formula counts as inside it.
    const auto found = std::partition_point(m_math.cbegin(), m_math.cend(),
                                            [position](const MathSpan &span) {
        return span.outerEnd < position;
    });
    return found != m_math.cend() && found->outerStart <= position
               ? int(found - m_math.cbegin()) : -1;
}

int MarkdownHighlighter::firstMathSpanAfter(int position) const {
    return int(std::partition_point(m_math.cbegin(), m_math.cend(),
                                    [position](const MathSpan &span) {
        return span.end <= position;
    }) - m_math.cbegin());
}

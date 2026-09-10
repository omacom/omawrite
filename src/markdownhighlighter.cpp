#include "markdownhighlighter.h"

#include <QColor>
#include <QFont>
#include <QSet>
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
                                    const QString &accent, const QString &codeBackground) {
    if (m_customBackground == background && m_customForeground == foreground
            && m_customAccent == accent && m_customCodeBackground == codeBackground)
        return;

    m_customBackground = background;
    m_customForeground = foreground;
    m_customAccent = accent;
    m_customCodeBackground = codeBackground;
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
    const QColor codeBackground = !m_customCodeBackground.isEmpty()
        ? QColor(m_customCodeBackground)
        : (m_darkMode ? QColor(QStringLiteral("#1c1a1a")) : QColor(QStringLiteral("#f8f8f8")));

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

    // Inline code keeps a character background: a span inside a sentence has
    // no block of its own to sit on.
    m_inlineCodeFormat = QTextCharFormat();
    m_inlineCodeFormat.setForeground(text);
    m_inlineCodeFormat.setBackground(codeBackground);

    // Fenced code gets none. The panel behind it is drawn in QML, and a
    // character background on top of it is the stripe that ends at the last
    // glyph on each line.
    m_codeFormat = QTextCharFormat();
    m_codeFormat.setForeground(text);

    // A fence recedes the way a heading's `#` does, over the panel it opens.
    m_fenceFormat = m_codeFormat;
    m_fenceFormat.setForeground(marker);

    // Keywords take the theme's accent, so highlighted code belongs to the
    // same document rather than looking like a widget dropped into it. The
    // other two are fixed pairs, picked to sit against either page without
    // competing with the accent.
    m_codeCommentFormat = m_codeFormat;
    m_codeCommentFormat.setForeground(
        codeTokenColor(CodeToken::Comment, m_darkMode, link, marker));
    m_codeCommentFormat.setFontItalic(true);

    m_codeStringFormat = m_codeFormat;
    m_codeStringFormat.setForeground(
        codeTokenColor(CodeToken::String, m_darkMode, link, marker));

    m_codeNumberFormat = m_codeFormat;
    m_codeNumberFormat.setForeground(
        codeTokenColor(CodeToken::Number, m_darkMode, link, marker));

    m_codeKeywordFormat = m_codeFormat;
    m_codeKeywordFormat.setForeground(
        codeTokenColor(CodeToken::Keyword, m_darkMode, link, marker));
    m_codeKeywordFormat.setFontWeight(QFont::DemiBold);

    m_codeFunctionFormat = m_codeFormat;
    m_codeFunctionFormat.setForeground(
        codeTokenColor(CodeToken::Function, m_darkMode, link, marker));

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
    const bool fence = isFence(text);
    const int previous = previousBlockState() < 0 ? Prose : previousBlockState();
    const bool insideFence = (previous & InsideFence) != 0;

    // The opening fence names the language; every line under it needs to know,
    // so the language rides in the block state above the inside-a-fence bit.
    QStringList languages = codeLanguages();
    int languageIndex = previous >> LanguageShift;
    if (fence && !insideFence)
        languageIndex = qMax(0, languages.indexOf(languageForFence(text)) + 1);
    else if (!insideFence)
        languageIndex = 0;

    const bool nowInside = fence != insideFence;
    setCurrentBlockState(nowInside ? (InsideFence | (languageIndex << LanguageShift))
                                   : Prose);

    if (fence || insideFence) {
        // No character background here: the panel is drawn behind the whole
        // fence in QML. Painting one per line is what made it striped.
        setFormat(0, text.length(), fence ? m_fenceFormat : m_codeFormat);
        if (insideFence && languageIndex > 0 && languageIndex <= languages.size())
            highlightCode(text, languages.at(languageIndex - 1));
        highlightSearch(text);
        return;
    }

    if (!text.isEmpty()) {
        highlightMarkers(text);
        if (text.contains(QLatin1Char('`')) || text.contains(QLatin1Char('*'))
            || text.contains(QLatin1Char('_')) || text.contains(QLatin1Char('['))) {
            highlightInline(text);
        }
    }
    highlightSearch(text);
}

namespace {
struct LanguageRules {
    const char *name;
    const char *aliases;      // space separated, name included
    const char *lineComment;  // may hold two, separated by a space
    const char *keywords;     // space separated
    bool commandFirst;        // a shell: the word in command position is the verb
};

// Short on purpose. A keyword list long enough to be exhaustive is a
// dictionary to maintain; these are the words that actually carry the shape of
// a snippet, and anything missing simply reads as plain code.
const LanguageRules languageTable[] = {
    {"bash", "bash sh shell zsh console", "#",
     "if then else elif fi for while do done case esac function return in "
     "export local readonly set unset echo cd exit source alias trap", true},
    {"python", "python py", "#",
     "def class return if elif else for while in is not and or none true false "
     "import from as with try except finally raise yield lambda pass break "
     "continue global nonlocal assert async await self", false},
    {"javascript", "javascript js typescript ts jsx tsx node", "//",
     "const let var function return if else for while do class extends new "
     "import export from default async await try catch finally throw typeof "
     "instanceof this null undefined true false switch case break continue "
     "interface type enum implements readonly public private", false},
    {"cpp", "cpp c c++ cc h hpp objc", "//",
     "int char bool void float double long short unsigned signed const "
     "constexpr static inline class struct enum union namespace template "
     "typename public private protected virtual override final return if else "
     "for while do switch case break continue new delete nullptr true false "
     "auto using include define sizeof", false},
    {"rust", "rust rs", "//",
     "fn let mut const static struct enum impl trait pub use mod match if else "
     "for while loop return break continue where self Self as dyn ref move "
     "async await unsafe true false Some None Ok Err", false},
    {"go", "go golang", "//",
     "func package import var const type struct interface map chan go defer "
     "return if else for range switch case break continue nil true false make "
     "new select", false},
    {"ruby", "ruby rb", "#",
     "def class module end if elsif else unless while until for in do return "
     "yield require require_relative attr_accessor attr_reader self nil true "
     "false begin rescue ensure raise", false},
    {"sql", "sql postgres postgresql mysql sqlite", "--",
     "select from where insert into values update set delete create table "
     "alter drop index view join left right inner outer on group by order "
     "having limit offset distinct as and or not null primary key foreign "
     "references default constraint returning with union", false},
    {"json", "json", "",
     "true false null", false},
    {"yaml", "yaml yml", "#", "true false null yes no on off", false},
    {"toml", "toml", "#", "true false", false},
    {"qml", "qml", "//",
     "import property readonly signal function var let const if else for while "
     "return true false null anchors id on as", false},
};

const LanguageRules *rulesFor(const QString &language) {
    if (language.isEmpty())
        return nullptr;

    for (const LanguageRules &rules : languageTable) {
        const QStringList aliases =
            QString::fromLatin1(rules.aliases).split(QLatin1Char(' '));
        if (aliases.contains(language))
            return &rules;
    }
    return nullptr;
}

bool isWordCharacter(QChar character) {
    return character.isLetterOrNumber() || character == QLatin1Char('_');
}
}

QColor MarkdownHighlighter::codeTokenColor(CodeToken token, bool darkMode,
                                           const QColor &accent, const QColor &muted) {
    switch (token) {
    case CodeToken::Comment:
        return muted;
    case CodeToken::String:
        return darkMode ? QColor(QStringLiteral("#8fd4ae"))
                        : QColor(QStringLiteral("#0b7a5a"));
    case CodeToken::Number:
        return darkMode ? QColor(QStringLiteral("#e2a978"))
                        : QColor(QStringLiteral("#9a5300"));
    case CodeToken::Keyword:
        // The theme's accent, so highlighted code belongs to the document
        // rather than looking like a widget dropped into it.
        return accent;
    case CodeToken::Function:
        return darkMode ? QColor(QStringLiteral("#9db8f0"))
                        : QColor(QStringLiteral("#3b5fa8"));
    }
    return accent;
}

QString MarkdownHighlighter::languageForFence(const QString &fenceLine) {
    static const QRegularExpression infoRe(
        QStringLiteral("^\\s*(?:```|~~~)\\s*([A-Za-z0-9_+#-]+)"));
    const QRegularExpressionMatch match = infoRe.match(fenceLine);
    if (!match.hasMatch())
        return {};

    const QString requested = match.captured(1).toLower();
    const LanguageRules *rules = rulesFor(requested);
    return rules ? QString::fromLatin1(rules->name) : QString();
}

QList<MarkdownHighlighter::CodeSpanToken>
MarkdownHighlighter::codeTokens(const QString &line, const QString &language) {
    QList<CodeSpanToken> tokens;
    const LanguageRules *rules = rulesFor(language);
    if (!rules)
        return tokens;

    const QStringList commentMarkers = QString::fromLatin1(rules->lineComment)
                                           .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QStringList keywordList =
        QString::fromLatin1(rules->keywords).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QSet<QString> keywords(keywordList.cbegin(), keywordList.cend());

    int index = 0;
    bool commandPosition = true;
    while (index < line.size()) {
        const QChar character = line.at(index);

        // A comment runs to the end of the line, so nothing after it is code.
        bool comment = false;
        for (const QString &marker : commentMarkers) {
            if (!marker.isEmpty() && line.mid(index, marker.size()) == marker) {
                tokens.append({index, int(line.size() - index), CodeToken::Comment});
                comment = true;
                break;
            }
        }
        if (comment)
            break;

        if (character == QLatin1Char('"') || character == QLatin1Char('\'')
                || character == QLatin1Char('`')) {
            const QChar quote = character;
            int end = index + 1;
            while (end < line.size()) {
                if (line.at(end) == QLatin1Char('\\')) {
                    end += 2;
                    continue;
                }
                if (line.at(end) == quote) {
                    ++end;
                    break;
                }
                ++end;
            }
            // An unterminated quote takes the rest of the line, which is what
            // it looks like on screen anyway.
            const int stop = qMin(end, line.size());
            tokens.append({index, stop - index, CodeToken::String});
            index = stop;
            continue;
        }

        if (character.isDigit()
                && (index == 0 || !isWordCharacter(line.at(index - 1)))) {
            int end = index;
            while (end < line.size()
                   && (line.at(end).isLetterOrNumber() || line.at(end) == QLatin1Char('.')
                       || line.at(end) == QLatin1Char('x'))) {
                ++end;
            }
            tokens.append({index, end - index, CodeToken::Number});
            index = end;
            continue;
        }

        if (isWordCharacter(character)
                && (index == 0 || !isWordCharacter(line.at(index - 1)))) {
            int end = index;
            while (end < line.size() && isWordCharacter(line.at(end)))
                ++end;
            const QString word = line.mid(index, end - index);

            if (keywords.contains(word.toLower()) || keywords.contains(word)) {
                tokens.append({index, end - index, CodeToken::Keyword});
                commandPosition = false;
            } else if (rules->commandFirst && commandPosition) {
                // The verb of a shell line. Without this a snippet like
                // `open some/path` has nothing in it to highlight at all.
                tokens.append({index, end - index, CodeToken::Function});
                commandPosition = false;
            } else if (end < line.size() && line.at(end) == QLatin1Char('(')) {
                tokens.append({index, end - index, CodeToken::Function});
            }

            index = end;
            continue;
        }

        // A pipe or a separator starts a new command.
        if (rules->commandFirst
                && (character == QLatin1Char('|') || character == QLatin1Char(';')
                    || character == QLatin1Char('&'))) {
            commandPosition = true;
        } else if (!character.isSpace()) {
            commandPosition = false;
        }

        ++index;
    }

    return tokens;
}

QStringList MarkdownHighlighter::codeLanguages() {
    QStringList names;
    for (const LanguageRules &rules : languageTable)
        names << QString::fromLatin1(rules.name);
    return names;
}

void MarkdownHighlighter::highlightCode(const QString &text, const QString &language) {
    for (const CodeSpanToken &token : codeTokens(text, language)) {
        switch (token.token) {
        case CodeToken::Comment:
            setFormat(token.start, token.length, m_codeCommentFormat);
            break;
        case CodeToken::String:
            setFormat(token.start, token.length, m_codeStringFormat);
            break;
        case CodeToken::Number:
            setFormat(token.start, token.length, m_codeNumberFormat);
            break;
        case CodeToken::Keyword:
            setFormat(token.start, token.length, m_codeKeywordFormat);
            break;
        case CodeToken::Function:
            setFormat(token.start, token.length, m_codeFunctionFormat);
            break;
        }
    }
}

bool MarkdownHighlighter::isFence(const QString &text) {
    static const QRegularExpression fenceRe(QStringLiteral("^\\s*```"));
    return text.contains(QLatin1Char('`')) && fenceRe.match(text).hasMatch();
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
    for (const Span &code : codeSpans(text))
        setFormat(code.start, code.length, m_inlineCodeFormat);

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
}

QList<MarkdownHighlighter::Span> MarkdownHighlighter::codeSpans(const QString &text) {
    QList<Span> spans;
    if (!text.contains(QLatin1Char('`')))
        return spans;

    static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
    QRegularExpressionMatchIterator codeMatches = codeRe.globalMatch(text);
    while (codeMatches.hasNext()) {
        const QRegularExpressionMatch match = codeMatches.next();
        spans.append({int(match.capturedStart(0)), int(match.capturedLength(0))});
    }
    return spans;
}

QList<MarkdownHighlighter::InlineMarkup> MarkdownHighlighter::inlineMarkup(const QString &text) {
    QList<InlineMarkup> markup;
    if (!text.contains(QLatin1Char('*')) && !text.contains(QLatin1Char('_'))
            && !text.contains(QLatin1Char('['))) {
        return markup;
    }

    const auto span = [](const QRegularExpressionMatch &match, int group) {
        return Span{int(match.capturedStart(group)), int(match.capturedLength(group))};
    };

    // Inline code is literal, so a `*`, `_` or `[` inside backticks is not a
    // marker: `default_line_height` keeps its underscores. Markup whose
    // markers land in a code span is dropped; markup that merely wraps one
    // (**bold with `code` inside**) still applies.
    const QList<Span> code = codeSpans(text);
    const auto append = [&](const InlineMarkup &item) {
        for (const Span &span : code) {
            for (const Span &marker : item.markers) {
                if (marker.start < span.start + span.length
                        && span.start < marker.start + marker.length)
                    return;
            }
        }
        markup.append(item);
    };

    // Underscores only delimit emphasis at a word boundary, so identifiers such
    // as snake_case_name read as themselves. Asterisks delimit anywhere.
    static const QRegularExpression boldRe(
        QStringLiteral("\\*\\*(.+?)\\*\\*|(?<!\\w)__(.+?)__(?!\\w)"),
        QRegularExpression::UseUnicodePropertiesOption);
    QRegularExpressionMatchIterator boldMatches = boldRe.globalMatch(text);
    while (boldMatches.hasNext()) {
        const QRegularExpressionMatch match = boldMatches.next();
        const Span whole = span(match, 0);
        const int contentIndex = match.capturedStart(1) >= 0 ? 1 : 2;
        append({InlineKind::Bold, span(match, contentIndex),
                {{whole.start, 2}, {whole.start + whole.length - 2, 2}}});
    }

    static const QRegularExpression italicRe(
        QStringLiteral("(?<!\\*)\\*([^*\\n]+)\\*(?!\\*)|(?<!\\w)_([^_\\n]+)_(?!\\w)"),
        QRegularExpression::UseUnicodePropertiesOption);
    QRegularExpressionMatchIterator italicMatches = italicRe.globalMatch(text);
    while (italicMatches.hasNext()) {
        const QRegularExpressionMatch match = italicMatches.next();
        const Span whole = span(match, 0);
        const int contentIndex = match.capturedStart(1) >= 0 ? 1 : 2;
        append({InlineKind::Italic, span(match, contentIndex),
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
        append({InlineKind::Link, content,
                {{whole.start, 1},
                 {contentEnd, whole.start + whole.length - contentEnd}}});
    }

    return markup;
}

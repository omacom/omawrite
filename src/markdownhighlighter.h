#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QColor>
#include <QTextCharFormat>

class MarkdownHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit MarkdownHighlighter(QTextDocument *document);

    void setDarkMode(bool darkMode);
    void setColors(const QString &background, const QString &foreground, const QString &accent,
                   const QString &codeBackground);
    void setSearch(const QString &query, int currentMatchStart);

    struct Span {
        int start;
        int length;
    };

    enum class InlineKind { Bold, Italic, Link };

    // Carried from block to block so a fenced run of code knows it is inside
    // one. Stored on the block, which is where Backend::hiddenRangesAt reads it.
    // Prose or inside a fence, with the language index packed above so every
    // line of a fenced run knows what it is written in.
    enum BlockState { Prose = 0, InsideFence = 1 };
    static constexpr int LanguageShift = 8;

    // The state carries the language above the flag, so it must be asked
    // rather than compared: a fence that names a language does not equal
    // InsideFence on its own.
    static bool isInsideFence(int blockState) {
        return blockState > 0 && (blockState & InsideFence) != 0;
    }

    struct InlineMarkup {
        InlineKind kind;
        Span content;
        Span markers[2];
    };

    // Single source of truth for inline markdown spans: the highlighter uses it
    // to style content and hide markers, and the editor uses it (via
    // Backend::hiddenRangesAt) to skip the caret over the hidden markers.
    static QList<InlineMarkup> inlineMarkup(const QString &text);

    // Inline code spans, backticks included.
    static QList<Span> codeSpans(const QString &text);

    // What a run of code inside a fence is made of. Comments, strings and
    // numbers are found the same way in every language; only the keywords and
    // the comment marker differ, which is what the language table holds.
    // Function covers the thing being invoked: the command a shell line
    // starts with, and a name called with parentheses anywhere else. Without
    // it a shell snippet -- which is mostly commands and paths -- comes out
    // entirely unhighlighted.
    enum class CodeToken { Comment, String, Number, Keyword, Function };

    struct CodeSpanToken {
        int start;
        int length;
        CodeToken token;
    };

    // The language a fence opens with, reduced to one the tokeniser knows:
    // "```py" and "```python" are the same thing, and anything unknown comes
    // back empty and is styled as plain code.
    static QString languageForFence(const QString &fenceLine);

    // Tokenises one line of code. Deliberately line-at-a-time: a string or a
    // comment that runs across lines is rare in a snippet and not worth the
    // state to track.
    static QList<CodeSpanToken> codeTokens(const QString &line, const QString &language);

    // The colour a token is drawn in. One place, because the editor and the
    // rendered page have to agree: highlighted code that changes palette when
    // you press Ctrl+/ reads as a different document.
    static QColor codeTokenColor(CodeToken token, bool darkMode,
                                 const QColor &accent, const QColor &muted);

    // The languages the tokeniser knows, in the order the block state indexes
    // them. One-based in the state, so zero can mean "plain code".
    static QStringList codeLanguages();

protected:
    void highlightBlock(const QString &text) override;

private:
    void rebuildFormats();
    static bool isFence(const QString &text);
    void highlightMarkers(const QString &text);
    void highlightInline(const QString &text);
    void highlightSearch(const QString &text);
    void highlightCode(const QString &text, const QString &language);

    bool m_darkMode = true;
    QString m_customBackground;
    QString m_customForeground;
    QString m_customAccent;
    QString m_customCodeBackground;
    QTextCharFormat m_markerFormat;
    QTextCharFormat m_hiddenMarkerFormat;
    QTextCharFormat m_headingFormat;
    QTextCharFormat m_boldFormat;
    QTextCharFormat m_italicFormat;
    QTextCharFormat m_codeFormat;
    QTextCharFormat m_inlineCodeFormat;
    QTextCharFormat m_fenceFormat;
    QTextCharFormat m_codeCommentFormat;
    QTextCharFormat m_codeStringFormat;
    QTextCharFormat m_codeNumberFormat;
    QTextCharFormat m_codeKeywordFormat;
    QTextCharFormat m_codeFunctionFormat;
    QTextCharFormat m_quoteFormat;
    QTextCharFormat m_linkFormat;
    QString m_searchQuery;
    int m_currentMatchStart = -1;
    QTextCharFormat m_searchFormat;
    QTextCharFormat m_currentSearchFormat;
};

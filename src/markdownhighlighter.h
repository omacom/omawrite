#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextCursor>

class MarkdownHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit MarkdownHighlighter(QTextDocument *document);

    void setTheme(bool darkMode, const QString &background, const QString &foreground,
                  const QString &accent, const QString &codeBackground);
    void setSearch(const QString &query, int currentMatchStart);
    void setCaret(int position);
    void refreshFont();

    struct Span {
        int start;
        int length;
    };

    enum class InlineKind { Bold, Italic, Strikethrough, Link };

    // Carried from block to block so a fenced run of code knows it is inside
    // one. Stored on the block, which is where Backend reads it.
    // Inside a fence the state is InsideFence plus twice the fence characters
    // past three that opened it, plus one for tildes, since only a fence of the
    // same character at least that long closes it.
    enum BlockState { Prose = 0, InsideFence = 1 };

    struct InlineMarkup {
        InlineKind kind;
        Span content;
        Span markers[2];
        Span destination{};
    };

    // Single source of truth for inline markdown spans: the highlighter uses it
    // to style content and hide markers, and Backend to find what the caret
    // sits in.
    static QList<InlineMarkup> inlineMarkup(const QString &text);
    static QString linkUrlAt(const QString &text, int position);

    // Inline code spans, backticks included.
    static QList<Span> codeSpans(const QString &text);
    static int openingFenceLength(const QString &text);
    static bool isThematicBreak(const QString &text);
    // Where a task list item's "[ ]" or "[x]" starts: right after the list
    // marker, followed by whitespace or the line's end, as GitHub reads it.
    // -1 when the line is no task.
    static int taskMarker(const QString &text);
    static bool closesFence(const QString &text, int openingLength,
                            QChar marker = QLatin1Char('`'));

    // Where query occurs in text, ignoring case and, for the query's plain
    // Latin and Greek letters, diacritics: "tara" finds "țară". An accented
    // letter in the query matches only itself, though ş and ș, ţ and ț count
    // as one. Folding keeps one UTF-16 unit per unit, so offsets and lengths
    // are the text's own.
    static QList<int> findMatches(const QString &text, const QString &query);

protected:
    void highlightBlock(const QString &text) override;

private:
    void rebuildFormats();
    void rehighlightAll();
    void highlightMarkers(const QString &text);
    void highlightInline(const QString &text);
    void highlightSearch(const QString &text);

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
    QTextCharFormat m_fenceFormat;
    QTextCharFormat m_quoteFormat;
    QTextCharFormat m_linkFormat;
    QTextCharFormat m_checkedTaskFormat;
    QString m_searchQuery;
    int m_currentMatchStart = -1;
    // Moves with edits, so it still finds the block the current match was
    // painted in after text above it changes.
    QTextCursor m_currentMatch;
    // The markers of whatever markup the caret is in or against are shown,
    // so they can be seen and edited.
    QTextCursor m_caret;
    QTextCharFormat m_searchFormat;
    QTextCharFormat m_currentSearchFormat;
};

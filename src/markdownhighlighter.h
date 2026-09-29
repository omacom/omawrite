#pragma once

#include <QFont>
#include <QPointer>
#include <QRectF>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>

class MathRenderer;

class MarkdownHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit MarkdownHighlighter(QTextDocument *document);

    void setDarkMode(bool darkMode);
    void setColors(const QString &background, const QString &foreground, const QString &accent);
    void setSearch(const QString &query, int currentMatchStart);

    struct Span {
        int start;
        int length;
    };

    enum class InlineKind { Bold, Italic, Link };

    struct InlineMarkup {
        InlineKind kind;
        Span content;
        Span markers[2];
    };

    // Single source of truth for inline markdown spans: the highlighter uses it
    // to style content and hide markers, and the editor uses it (via
    // Backend::hiddenRangesAt) to skip the caret over the hidden markers.
    // Markup is not looked for inside the masked spans, which hold math.
    static QList<InlineMarkup> inlineMarkup(const QString &text, const QList<Span> &masked = {});

    struct MathSpan {
        int start = 0;           // the opening delimiter
        int end = 0;             // just past the closing delimiter
        int delimiter = 1;       // length of each delimiter: 1 for $, 2 for $$, \( and \[
        bool display = false;    // typeset in TeX display style
        bool standalone = false; // alone on its lines, so shown as a centered block
        QString tex;             // the source between the delimiters
        // Where the caret opens the formula: the whole lines of a standalone
        // formula, whose centered image sits away from its first character.
        int outerStart = 0;
        int outerEnd = 0;
    };

    // TeX math in a whole document, in order: $...$ and \(...\) inline,
    // $$...$$ display, and \[...\] display when alone on its lines, since
    // Markdown prose escapes brackets that way. Display math alone on its lines
    // may span several lines, up to a blank line. Code is skipped.
    static QList<MathSpan> mathSpans(const QString &text);

    // Where a typeset formula goes over the editor text, in document coordinates.
    struct MathPlacement {
        QRectF image;
        QRectF backing;  // the space held for the formula, painted when selected
        int position;    // the character holding that space
        QByteArray svg;
    };

    void setMathRenderer(MathRenderer *renderer);
    // A formula shows its source while the caret is in it or at its edges.
    void setMathCaret(int position);
    QList<Span> mathSegments(const QTextBlock &block) const;
    QList<MathPlacement> mathPlacements() const;

signals:
    // The formulas may have moved, appeared or disappeared.
    void mathLayoutChanged();

protected:
    void highlightBlock(const QString &text) override;

private:
    void rebuildFormats();
    void highlightMarkers(const QString &text);
    void highlightInline(const QString &text, const QList<Span> &math);
    void highlightMath(const QString &text);
    void highlightSearch(const QString &text);

    void updateMathIndex(int position, int charsRemoved, int charsAdded);
    void mathRendered(const QString &tex, bool display);
    void updateMathPreamble();
    void documentLayoutChanged();
    void scheduleMathRefresh(int start, int end, int delay = 0);
    void refreshMath();
    int mathSpanAt(int position) const;
    int firstMathSpanAfter(int position) const;

    bool m_darkMode = true;
    QString m_customBackground;
    QString m_customForeground;
    QString m_customAccent;
    QTextCharFormat m_markerFormat;
    QTextCharFormat m_hiddenMarkerFormat;
    QTextCharFormat m_headingFormat;
    QTextCharFormat m_boldFormat;
    QTextCharFormat m_italicFormat;
    QTextCharFormat m_codeFormat;
    QTextCharFormat m_quoteFormat;
    QTextCharFormat m_linkFormat;
    QTextCharFormat m_mathErrorFormat;
    QString m_searchQuery;
    int m_currentMatchStart = -1;
    QTextCharFormat m_searchFormat;
    QTextCharFormat m_currentSearchFormat;

    QList<MathSpan> m_math;
    QPointer<MathRenderer> m_mathRenderer;
    QTextCursor m_mathCaret;
    QList<std::pair<int, int>> m_mathRefresh;
    QTimer m_mathRefreshTimer;
    int m_mathRenderBatchMs = 40;
    QFont m_mathFont;
    qreal m_mathTextWidth = -1;
};

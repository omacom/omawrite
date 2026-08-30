#pragma once

#include <QString>
#include <QTextBlock>
#include <QUrl>

class QTextDocument;

// Omarchy colors.toml tokens used by the browser preview.
struct PreviewPalette {
    bool darkMode = true;
    QString background;
    QString foreground;
    QString accent;
    QString selection;
    QString muted;
    QString lighterBackground;
    QString darkForeground;
    QString lightForeground;
    QString red;
    QString yellow;
    QString orange;
    QString green;
    QString cyan;
    QString blue;
    QString magenta;
    QString brightYellow;
    QString brightBlue;
    int fontPixelSize = 20;

    static PreviewPalette fallback(bool darkMode);
    QString hex(const QString &value, const QString &ifInvalid) const;
};

// Turns Markdown into a browser-preview HTML page. Fenced code is colored with
// the `highlight` CLI when it is on PATH, using Omarchy theme colours.
class CodeBlockHighlighter {
public:
    static bool isCodeBlock(const QTextBlock &block);
    // First token of the fence info string, lowercased. Empty for indented
    // blocks and fences with no language.
    static QString codeLanguage(const QTextBlock &block);
    static bool highlightAvailable();

    static QString html(const QString &markdown, const QString &title,
                        const PreviewPalette &palette,
                        const QUrl &documentUrl = {});
};

#pragma once

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/State>
#include <QList>
#include <QString>
#include <QTextCharFormat>
#include <memory>

// Standalone line lexer; never attaches a QSyntaxHighlighter to a document.
// Use on the GUI thread. Definitions share one process-lifetime repository.
class CodeHighlighter {
public:
    struct Token {
        int offset;
        int length;
        // Only foreground, font weight and italic: merge onto the caller's
        // base code format to retain its background, font family and size.
        QTextCharFormat format;
    };
    struct Line {
        QList<Token> tokens;
        KSyntaxHighlighting::State state;
    };

    explicit CodeHighlighter(bool darkMode = true);
    ~CodeHighlighter();
    CodeHighlighter(const CodeHighlighter &) = delete;
    CodeHighlighter &operator=(const CodeHighlighter &) = delete;

    void setDarkMode(bool darkMode);
    // First whitespace-delimited info token; invalid means plain code.
    static KSyntaxHighlighting::Definition resolveLanguage(const QString &info);
    // Pass a default State at each opening fence, then the preceding result's
    // state for every body line (including empty lines). Never feed delimiters.
    Line tokenize(const QString &text, const KSyntaxHighlighting::Definition &language,
                  const KSyntaxHighlighting::State &state = {});

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

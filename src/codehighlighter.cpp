#include "codehighlighter.h"

#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Theme>
#include <QFont>
#include <QHash>
#include <QRegularExpression>

#include <utility>

namespace {
KSyntaxHighlighting::Repository &repository() {
    static KSyntaxHighlighting::Repository instance;
    return instance;
}
}

class CodeHighlighter::Impl : public KSyntaxHighlighting::AbstractHighlighter {
public:
    using KSyntaxHighlighting::AbstractHighlighter::highlightLine;
    QList<Token> tokens;

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override {
        QTextCharFormat style;
        if (format.hasTextColor(theme()))
            style.setForeground(format.textColor(theme()));
        // Explicit normal values prevent emphasis leaking from surrounding code.
        style.setFontWeight(format.isBold(theme()) ? QFont::Bold : QFont::Normal);
        style.setFontItalic(format.isItalic(theme()));
        if (length > 0)
            tokens.append({offset, length, style});
    }
};

CodeHighlighter::CodeHighlighter(bool darkMode) : m_impl(std::make_unique<Impl>()) {
    setDarkMode(darkMode);
}
CodeHighlighter::~CodeHighlighter() = default;

void CodeHighlighter::setDarkMode(bool darkMode) {
    m_impl->setTheme(repository().defaultTheme(darkMode
        ? KSyntaxHighlighting::Repository::DarkTheme
        : KSyntaxHighlighting::Repository::LightTheme));
}

KSyntaxHighlighting::Definition CodeHighlighter::resolveLanguage(const QString &info) {
    static const QRegularExpression separator(QStringLiteral("\\s+"));
    const QString tag = info.trimmed().section(separator, 0, 0).toCaseFolded();
    if (tag.isEmpty())
        return {};
    static QHash<QString, KSyntaxHighlighting::Definition> cache;
    const auto found = cache.constFind(tag);
    if (found != cache.cend())
        return found.value();
    static const QHash<QString, QString> aliases = {
        {QStringLiteral("py"), QStringLiteral("Python")},
        {QStringLiteral("js"), QStringLiteral("JavaScript")},
        {QStringLiteral("ts"), QStringLiteral("TypeScript")},
        {QStringLiteral("cpp"), QStringLiteral("C++")},
        {QStringLiteral("c++"), QStringLiteral("C++")},
        {QStringLiteral("sh"), QStringLiteral("Bash")},
        {QStringLiteral("bash"), QStringLiteral("Bash")},
        {QStringLiteral("cs"), QStringLiteral("C#")},
        {QStringLiteral("yml"), QStringLiteral("YAML")}
    };
    auto &repo = repository();
    auto definition = repo.definitionForName(aliases.value(tag, tag));
    if (!definition.isValid()) {
        for (const auto &candidate : repo.definitions()) {
            if (candidate.name().compare(tag, Qt::CaseInsensitive) == 0
                    || candidate.alternativeNames().contains(tag, Qt::CaseInsensitive)) {
                definition = candidate;
                break;
            }
        }
    }
    // A tag is not a path or glob. Only a simple extension may use filename
    // lookup; never let "../x.py", "*.py" or arbitrary info become filenames.
    static const QRegularExpression safeExtension(QStringLiteral("^[a-z0-9][a-z0-9_+-]*$"));
    if (!definition.isValid() && safeExtension.match(tag).hasMatch())
        definition = repo.definitionForFileName(QStringLiteral("code.") + tag);
    cache.insert(tag, definition);
    return definition;
}

CodeHighlighter::Line CodeHighlighter::tokenize(
        const QString &text, const KSyntaxHighlighting::Definition &language,
        const KSyntaxHighlighting::State &state) {
    if (!language.isValid())
        return {};
    m_impl->setDefinition(language);
    m_impl->tokens.clear();
    const auto next = m_impl->highlightLine(QStringView(text), state);
    return {std::move(m_impl->tokens), next};
}

#include "spellchecker.h"
#include "markdownhighlighter.h"

#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QLocale>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextCursor>
#include <QtConcurrent/QtConcurrentRun>
#include <hunspell/hunspell.hxx>

namespace {
const QString languageKey = QStringLiteral("spelling/languages");
QString personalKey(const QString &id) { return QStringLiteral("spelling/personal/") + id; }
QString normalizedWord(QString word) {
    return word.replace(QChar(0x2019), QLatin1Char('\'')).normalized(QString::NormalizationForm_C);
}
struct Result { QList<SpellChecker::Word> words; QString error; };
}

SpellChecker::SpellChecker(QObject *parent, const QStringList &paths)
    : QObject(parent), m_paths(paths) {
    if (m_paths.isEmpty()) {
        for (const QString &base : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
            m_paths << base + QStringLiteral("/hunspell")
                    << base + QStringLiteral("/omawrite/dictionaries");
        }
        m_paths << QStringLiteral("/usr/share/myspell") << QStringLiteral("/usr/share/myspell/dicts");
        m_paths.removeDuplicates();
    }
    m_languages = QSettings().value(languageKey).toStringList();
    refreshDictionaries();
    // Only initialize once, and never silently substitute a different language.
    if (!QSettings().contains(languageKey)) {
        const QString locale = QLocale::system().name();
        for (const QVariant &entry : m_dictionaries) {
            if (entry.toMap().value(QStringLiteral("id")).toString() == locale) {
                setLanguages({locale});
                break;
            }
        }
    }
}

SpellChecker::~SpellChecker() { if (m_cancel) *m_cancel = true; }

QString SpellChecker::dictionaryDirectory() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/omawrite/dictionaries");
}

void SpellChecker::refreshDictionaries() {
    QVariantList dictionaries;
    QSet<QString> seen;
    for (const QString &path : m_paths) {
        QDir dir(path);
        for (const QString &file : dir.entryList({QStringLiteral("*.dic")}, QDir::Files, QDir::Name)) {
            const QString id = file.chopped(4);
            const QString aff = dir.filePath(id + QStringLiteral(".aff"));
            if (seen.contains(id) || !QFileInfo(aff).isReadable()) continue;
            seen.insert(id);
            const QLocale locale(id);
            const QString name = locale.language() == QLocale::C ? id
                : QStringLiteral("%1 (%2) — %3").arg(QLocale::languageToString(locale.language()),
                    QLocale::territoryToString(locale.territory()), id);
            dictionaries.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), name},
                {QStringLiteral("aff"), aff}, {QStringLiteral("dic"), dir.filePath(file)}});
        }
    }
    if (m_dictionaries == dictionaries) return;
    invalidate(tr("Dictionaries changed. Start a new check."));
    m_dictionaries = dictionaries;
    emit dictionariesChanged();
}

void SpellChecker::setLanguages(const QStringList &languages) {
    QStringList unique = languages;
    unique.removeDuplicates();
    if (unique == m_languages) return;
    invalidate(tr("Languages changed. Start a new check."));
    m_languages = unique;
    QSettings().setValue(languageKey, unique);
    emit languagesChanged();
}

void SpellChecker::attachDocument(QTextDocument *document) {
    if (m_document) disconnect(m_document, nullptr, this, nullptr);
    invalidate(QString());
    m_document = document;
    if (!document) return;
    connect(document, &QTextDocument::contentsChanged, this, [this] {
        // Typography and syntax highlighting can emit contentsChanged as well.
        if (!m_replacing && (m_busy || !m_words.isEmpty()) && m_document
                && m_document->toPlainText() != m_snapshot) {
            invalidate(tr("Text changed. Start a new check."));
            emit textInvalidated();
        }
    });
}

void SpellChecker::invalidate(const QString &message) {
    if (m_cancel) *m_cancel = true;
    ++m_generation;
    m_busy = false;
    m_words.clear();
    m_index = 0;
    m_status = message;
    emit changed();
}
void SpellChecker::cancel() { invalidate(QString()); }

QVariantMap SpellChecker::current() const {
    if (m_index >= m_words.size()) return {};
    const auto &word = m_words[m_index];
    return {{QStringLiteral("word"), word.text}, {QStringLiteral("start"), word.start},
            {QStringLiteral("length"), word.length}, {QStringLiteral("suggestions"), word.suggestions},
            {QStringLiteral("context"), m_snapshot.mid(qMax(0, word.start - 60), word.length + 120)}};
}

QList<SpellChecker::Word> SpellChecker::proseWords(const QString &text, int start, int end) {
    QList<Word> words;
    int offset = 0, fence = 0;
    QChar fenceMarker;
    static const QRegularExpression tokens(QStringLiteral("\\p{L}[\\p{L}\\p{M}]*(?:['’][\\p{L}\\p{M}]+)*"));
    static const QRegularExpression addresses(QStringLiteral("(?:[a-zA-Z][a-zA-Z0-9+.-]*://|www\\.)[^\\s<>]+|[\\w.+%-]+@[\\w.-]+\\.[\\p{L}]+"));
    static const QRegularExpression reference(QStringLiteral("^\\s{0,3}\\[[^\\]]+\\]:\\s*\\S+.*$"));
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const int next = offset + line.size() + 1;
        const int opening = MarkdownHighlighter::openingFenceLength(line);
        if (fence) {
            if (MarkdownHighlighter::closesFence(line, fence, fenceMarker)) fence = 0;
            offset = next;
            continue;
        }
        if (opening) {
            fence = opening;
            fenceMarker = line.trimmed().at(0);
            offset = next;
            continue;
        }
        if (offset > end) break;
        if (next <= start || line.startsWith(QStringLiteral("    ")) || line.startsWith(QLatin1Char('\t'))
                || reference.match(line).hasMatch()) { offset = next; continue; }
        QString prose = line;
        const auto mask = [&prose](int pos, int length) {
            for (int i = pos; i < pos + length && i < prose.size(); ++i) prose[i] = QLatin1Char(' ');
        };
        for (const auto &span : MarkdownHighlighter::codeSpans(line)) mask(span.start, span.length);
        for (const auto &markup : MarkdownHighlighter::inlineMarkup(line)) {
            if (markup.kind == MarkdownHighlighter::InlineKind::Link)
                mask(markup.destination.start, markup.destination.length);
        }
        auto links = addresses.globalMatch(prose);
        while (links.hasNext()) { const auto m = links.next(); mask(m.capturedStart(), m.capturedLength()); }
        auto matches = tokens.globalMatch(prose);
        while (matches.hasNext()) {
            const auto m = matches.next();
            const int pos = offset + m.capturedStart();
            if (pos >= start && pos + m.capturedLength() <= end)
                words.append({pos, int(m.capturedLength()), m.captured(), {}});
        }
        offset = next;
    }
    return words;
}

void SpellChecker::start(int start, int end) {
    invalidate(QString());
    m_corrections = 0;
    if (!m_document) return;
    if (m_languages.isEmpty()) {
        m_status = tr("Choose at least one language before checking spelling.");
        emit changed();
        return;
    }
    QVariantList selected;
    QSettings settings;
    for (const QString &id : m_languages) {
        QVariantMap found;
        for (const QVariant &entry : m_dictionaries)
            if (entry.toMap().value(QStringLiteral("id")) == id) found = entry.toMap();
        if (found.isEmpty()) {
            m_status = tr("Dictionary %1 is unavailable. Install it or change the selected languages.").arg(id);
            emit changed();
            return;
        }
        found.insert(QStringLiteral("personal"), settings.value(personalKey(id)).toStringList());
        selected.append(found);
    }
    m_snapshot = m_document->toPlainText();
    const QString snapshot = m_snapshot;
    start = qBound(0, start, int(snapshot.size()));
    end = end < 0 ? snapshot.size() : qBound(start, end, int(snapshot.size()));
    m_cancel = std::make_shared<std::atomic_bool>(false);
    auto cancelled = m_cancel;
    const auto generation = m_generation;
    m_busy = true;
    m_status = tr("Checking spelling…");
    emit changed();
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, generation] {
        const Result result = watcher->result();
        watcher->deleteLater();
        if (generation != m_generation || !m_document) return;
        if (m_document->toPlainText() != m_snapshot) {
            invalidate(tr("Text changed. Start a new check."));
            return;
        }
        m_busy = false;
        m_words = result.words;
        m_status = result.error;
        if (m_status.isEmpty()) advance();
        else emit changed();
    });
    watcher->setFuture(QtConcurrent::run([snapshot, start, end, selected, cancelled] {
        Result result;
        struct Dictionary {
            std::unique_ptr<Hunspell> engine;
            QStringConverter::Encoding encoding;
            QSet<QString> personal;
        };
        std::vector<Dictionary> dictionaries;
        for (const QVariant &entry : selected) {
            if (*cancelled) return result;
            const auto map = entry.toMap();
            const auto aff = QFile::encodeName(map.value(QStringLiteral("aff")).toString());
            const auto dic = QFile::encodeName(map.value(QStringLiteral("dic")).toString());
            QFile affFile(QString::fromLocal8Bit(aff)), dicFile(QString::fromLocal8Bit(dic));
            if (!affFile.open(QIODevice::ReadOnly) || !dicFile.open(QIODevice::ReadOnly)) {
                result.error = tr("Could not read dictionary %1.").arg(map.value(QStringLiteral("id")).toString());
                return result;
            }
            auto engine = std::make_unique<Hunspell>(aff.constData(), dic.constData());
            const auto encoding = QStringConverter::encodingForName(engine->get_dic_encoding());
            if (!encoding) {
                result.error = tr("Dictionary %1 uses an unsupported encoding. Install a UTF-8 dictionary.")
                    .arg(map.value(QStringLiteral("id")).toString());
                return result;
            }
            QSet<QString> personal;
            for (const QString &word : map.value(QStringLiteral("personal")).toStringList()) personal.insert(word);
            dictionaries.push_back({std::move(engine), *encoding, personal});
        }
        QHash<QString, QStringList> misspellings;
        QSet<QString> correct;
        const auto words = proseWords(snapshot, start, end);
        for (auto word : words) {
            if (*cancelled) return Result{};
            const QString normalized = normalizedWord(word.text);
            if (correct.contains(normalized)) continue;
            if (!misspellings.contains(normalized)) {
                bool accepted = false;
                for (auto &dictionary : dictionaries) {
                    QStringEncoder encoder(dictionary.encoding);
                    const QByteArray bytes = encoder(normalized);
                    if (dictionary.personal.contains(normalized)
                            || (!encoder.hasError() && dictionary.engine->spell(bytes.toStdString()))) {
                        accepted = true;
                        break;
                    }
                }
                if (accepted) { correct.insert(normalized); continue; }
                QStringList suggestions;
                for (auto &dictionary : dictionaries) {
                    if (*cancelled) return Result{};
                    QStringEncoder encoder(dictionary.encoding);
                    const QByteArray bytes = encoder(normalized);
                    if (encoder.hasError()) continue;
                    for (const auto &suggestion : dictionary.engine->suggest(bytes.toStdString())) {
                        QStringDecoder decoder(dictionary.encoding);
                        const QString decoded = decoder(QByteArray::fromStdString(suggestion));
                        if (!decoder.hasError() && !suggestions.contains(decoded)) suggestions.append(decoded);
                    }
                }
                misspellings.insert(normalized, suggestions.mid(0, 12));
            }
            word.suggestions = misspellings.value(normalized);
            result.words.append(word);
        }
        return result;
    }));
}

bool SpellChecker::validCurrent() {
    if (!m_document || m_index >= m_words.size()) return false;
    if (m_document->toPlainText() != m_snapshot) {
        invalidate(tr("Text changed. Start a new check."));
        return false;
    }
    return true;
}
void SpellChecker::advance() {
    if (m_index >= m_words.size()) {
        m_words.clear();
        m_index = 0;
        m_status = tr("Spell check complete. Corrections: %1.").arg(m_corrections);
    } else m_status = tr("Reviewing spelling");
    emit changed();
}
void SpellChecker::skip(bool all) {
    if (!validCurrent()) return;
    const QString word = normalizedWord(m_words[m_index].text);
    if (all) {
        for (int i = m_words.size() - 1; i >= m_index; --i)
            if (normalizedWord(m_words[i].text) == word) m_words.removeAt(i);
    } else ++m_index;
    advance();
}
void SpellChecker::replace(const QString &replacement) {
    if (!validCurrent() || replacement.isEmpty() || replacement.contains(QLatin1Char('\n'))
            || replacement.contains(QLatin1Char('\r'))) return;
    const Word word = m_words[m_index];
    m_replacing = true;
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    cursor.setPosition(word.start);
    cursor.setPosition(word.start + word.length, QTextCursor::KeepAnchor);
    cursor.insertText(replacement);
    cursor.endEditBlock();
    m_snapshot = m_document->toPlainText();
    m_replacing = false;
    const int delta = replacement.size() - word.length;
    for (int i = m_index + 1; i < m_words.size(); ++i) m_words[i].start += delta;
    emit replacementApplied(delta);
    ++m_corrections;
    ++m_index;
    advance();
}
void SpellChecker::addWord(const QString &language) {
    if (!validCurrent() || !m_languages.contains(language)) return;
    QSettings settings;
    QStringList words = settings.value(personalKey(language)).toStringList();
    const QString word = normalizedWord(m_words[m_index].text);
    if (!words.contains(word)) words.append(word);
    settings.setValue(personalKey(language), words);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        m_status = tr("Could not save the personal dictionary.");
        emit changed();
        return;
    }
    skip(true);
}

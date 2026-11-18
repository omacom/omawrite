#include "spellingcontroller.h"

#include <Sonnet/Speller>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocale>
#include <QSettings>
#include <QTextDocument>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

namespace {
class NativeDictionary final : public SpellingDictionary {
public:
    QMap<QString, QString> dictionaries() const override { return m_speller.availableDictionaries(); }
    bool selectLanguage(const QString &language) override {
        m_speller.setLanguage(language);
        return m_speller.isValid();
    }
    bool isCorrect(const QString &word) override { return m_speller.isCorrect(word); }
    QStringList suggestions(const QString &word) override { return m_speller.suggest(word); }
    bool learn(const QString &word) override { return m_speller.addToPersonal(word); }
private:
    Sonnet::Speller m_speller;
};
}

SpellingController::SpellingController(QObject *parent)
    : SpellingController(std::make_unique<NativeDictionary>(), parent) {
    m_persist = true;
    QSettings settings;
    m_enabled = settings.value(QStringLiteral("spelling/enabled"), true).toBool();
    const QString saved = settings.value(QStringLiteral("spelling/language")).toString();
    if (!saved.isEmpty()) {
        m_language = saved;
        m_available = m_dictionaries.values().contains(saved) && m_dictionary->selectLanguage(saved);
    }
}

SpellingController::SpellingController(std::unique_ptr<SpellingDictionary> dictionary, QObject *parent)
    : QObject(parent), m_dictionary(std::move(dictionary)) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    initialize();
}
SpellingController::~SpellingController() = default;

void SpellingController::initialize() {
    if (m_dictionary) {
        m_dictionaries = m_dictionary->dictionaries();
        const QString locale = QLocale::system().name().toLower();
        const auto ids = m_dictionaries.values();
        const auto normalized = [](QString id) {
            return id.replace(QLatin1Char('-'), QLatin1Char('_')).toLower();
        };
        // Windows exposes BCP-47 IDs (en-US), while Qt and Hunspell usually
        // use en_US and macOS may use just en. Keep the backend's original ID.
        for (const auto &id : ids) {
            if (normalized(id) == locale) {
                m_language = id;
                break;
            }
        }
        if (m_language.isEmpty()) {
            for (const auto &id : ids) {
                if (normalized(id).section(QLatin1Char('_'), 0, 0) == locale.section(QLatin1Char('_'), 0, 0)) {
                    m_language = id;
                    break;
                }
            }
        }
        // Do not silently check prose against an unrelated dictionary.
        m_available = !m_language.isEmpty() && m_dictionary->selectLanguage(m_language);
    }
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    m_batch.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, &SpellingController::classify);
    connect(&m_batch, &QTimer::timeout, this, &SpellingController::processBatch);
    connect(&m_classifier, &QFutureWatcher<QList<SpellingProse::Word>>::finished, this, [this] {
        m_classifying = false;
        if (m_classifyingGeneration == m_generation && m_enabled && m_available) {
            // Preedit may have ended while this snapshot was outstanding and
            // armed another debounce. This result already covers that source;
            // never reclassify it and append the same ranges a second time.
            m_debounce.stop();
            m_classifyAgain = false;
            m_words = m_classifier.result();
            m_nextWord = 0;
            if (!m_composing)
                m_batch.start(0);
        }
        if (m_classifyAgain) {
            m_classifyAgain = false;
            if (!m_composing)
                m_debounce.start();
        }
    });
}

QVariantList SpellingController::languages() const {
    QVariantList result;
    for (auto i = m_dictionaries.cbegin(); i != m_dictionaries.cend(); ++i)
        result.append(QVariantMap{{QStringLiteral("label"), i.key()}, {QStringLiteral("id"), i.value()}});
    return result;
}
void SpellingController::saveSettings() {
    if (!m_persist)
        return;
    QSettings settings;
    settings.setValue(QStringLiteral("spelling/enabled"), m_enabled);
    settings.setValue(QStringLiteral("spelling/language"), m_language);
}
void SpellingController::setEnabled(bool enabled) {
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    saveSettings();
    invalidate();
    emit settingsChanged();
}
void SpellingController::setLanguage(const QString &language) {
    if (m_language == language || !m_dictionaries.values().contains(language))
        return;
    m_language = language;
    m_available = m_dictionary && m_dictionary->selectLanguage(language);
    m_cache.clear();
    saveSettings();
    invalidate();
    emit settingsChanged();
}
void SpellingController::attach(QTextDocument *document) {
    disconnect(m_documentConnection);
    m_document = document;
    m_source = document ? document->toPlainText() : QString();
    if (document)
        m_documentConnection = connect(document, &QTextDocument::contentsChanged, this, &SpellingController::sourceChanged);
    invalidate();
}
void SpellingController::sourceChanged() {
    if (!m_document)
        return;
    const QString source = m_document->toPlainText();
    if (m_source == source)
        return; // formatting/highlighting is not a source edit
    m_source = source;
    invalidate();
}
void SpellingController::invalidate() {
    ++m_generation;
    m_debounce.stop();
    m_batch.stop();
    m_words.clear();
    m_nextWord = 0;
    m_publishedCount = -1;
    m_suggestionQueue.clear();
    const bool hadRanges = !m_ranges.isEmpty();
    m_ranges.clear();
    if (!m_composing && hadRanges)
        emit rangesChanged();
    emit menuChanged();
    if (m_enabled && m_available && m_document && !m_composing)
        m_debounce.start();
}
void SpellingController::setComposing(bool composing) {
    if (m_composing == composing)
        return;
    m_composing = composing;
    if (composing) {
        m_debounce.stop();
        m_batch.stop();
    } else {
        if (m_nextWord == m_words.size()) {
            m_publishedCount = m_ranges.size();
            emit rangesChanged();
        }
        if (m_enabled && m_available && m_document) {
            if (m_words.isEmpty())
                m_debounce.start();
            else
                m_batch.start(0);
        }
    }
}
void SpellingController::classify() {
    if (!m_enabled || !m_available || !m_document || m_composing)
        return;
    if (m_classifying) {
        m_classifyAgain = true;
        return;
    }
    m_classifying = true;
    m_classifyingGeneration = m_generation;
    const QString snapshot = m_source;
    m_classifier.setFuture(QtConcurrent::run([snapshot] { return SpellingProse::words(snapshot); }));
}
QString SpellingController::ignoreKey(const QString &word) const {
    return m_language + QChar(u'\0') + word;
}
void SpellingController::processBatch() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_enabled || !m_available || !m_document || m_composing)
        return;
    QElapsedTimer budget;
    budget.start();
    int checked = 0;
    // Menu requests take priority over background document checks, otherwise a
    // large document can starve its open menu (and even evict that cache entry).
    // Never call the native suggestion backend from the hit-test itself.
    if (!m_suggestionQueue.isEmpty()) {
        const QString word = m_suggestionQueue.takeFirst();
        if (auto *cached = m_cache.object(word); cached && !cached->correct) {
            ++checked;
            cached->suggestions = m_dictionary->suggestions(word).mid(0, 8);
            cached->hasSuggestions = true;
            emit menuChanged();
        }
    }
    // One native call cannot be preempted, but never run an unbounded set of
    // native calls in one event turn. Cache hits are bounded too.
    while (m_nextWord < m_words.size() && checked++ < 64 && budget.elapsed() < 4) {
        const auto &word = m_words.at(m_nextWord++);
        if (m_ignored.contains(ignoreKey(word.text)))
            continue;
        if (!m_cache.contains(word.text))
            m_cache.insert(word.text, new CachedWord{m_dictionary->isCorrect(word.text), false, {}});
        if (const auto *cached = m_cache.object(word.text); cached && !cached->correct)
            m_ranges.append({word.start, word.length});
    }
    publish();
    if (m_nextWord < m_words.size() || !m_suggestionQueue.isEmpty())
        m_batch.start(0);
}
void SpellingController::publish() {
    // Publish once per completed snapshot, not growing prefixes. Otherwise
    // projection/diff work and repeated long-paragraph restyles become quadratic
    // even with a timer throttle. Menu suggestions still get priority per batch.
    if (!m_composing && m_nextWord == m_words.size() && m_publishedCount != m_ranges.size()) {
        m_publishedCount = m_ranges.size();
        emit rangesChanged();
    }
}
QVariantMap SpellingController::wordAt(int position) {
    if (!m_enabled || !m_available || m_composing)
        return {};
    for (const auto &range : m_ranges) {
        if (position < range.start)
            break;
        if (position >= range.start && position <= range.start + range.length) {
            const QString word = m_source.mid(range.start, range.length);
            // A current-generation range is itself a cached negative result.
            // The bounded cross-document reuse cache may have evicted it in a
            // large document; restore its menu record without a native call.
            if (!m_cache.contains(word))
                m_cache.insert(word, new CachedWord{false, false, {}});
            auto *cached = m_cache.object(word);
            if (cached->correct)
                return {};
            if (!cached->hasSuggestions && !m_suggestionQueue.contains(word)) {
                m_suggestionQueue.append(word);
                m_batch.start(0);
            }
            return {{QStringLiteral("start"), range.start}, {QStringLiteral("length"), range.length},
                    {QStringLiteral("word"), word}, {QStringLiteral("generation"), QVariant::fromValue(m_generation)},
                    {QStringLiteral("suggestions"), cached->suggestions},
                    {QStringLiteral("pending"), !cached->hasSuggestions}};
        }
    }
    return {};
}
bool SpellingController::validate(const QVariantMap &token) const {
    if (!m_enabled || !m_available || m_composing || !m_document || token.isEmpty()
            || token.value(QStringLiteral("generation")).toULongLong() != m_generation)
        return false;
    const int start = token.value(QStringLiteral("start"), -1).toInt();
    const int length = token.value(QStringLiteral("length")).toInt();
    if (start < 0 || length <= 0 || m_source.mid(start, length) != token.value(QStringLiteral("word")).toString()
            || m_document->toPlainText() != m_source)
        return false;
    return m_ranges.contains({start, length});
}
bool SpellingController::ignore(const QVariantMap &token) {
    if (!validate(token))
        return false;
    m_ignored.insert(ignoreKey(token.value(QStringLiteral("word")).toString()));
    invalidate();
    return true;
}
bool SpellingController::learn(const QVariantMap &token) {
    if (!validate(token))
        return false;
    const QString word = token.value(QStringLiteral("word")).toString();
    if (!m_dictionary->learn(word)) {
        emit operationFailed(tr("The dictionary could not learn this word."));
        return false;
    }
    m_cache.remove(word);
    invalidate();
    return true;
}

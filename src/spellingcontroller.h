#pragma once

#include "spellingprose.h"
#include <QObject>
#include <QCache>
#include <QFutureWatcher>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <memory>

class QTextDocument;

// All dictionary methods (including construction/destruction) run on the GUI
// thread. Tests inject a deterministic dictionary; no platform service required.
class SpellingDictionary {
public:
    virtual ~SpellingDictionary() = default;
    virtual QMap<QString, QString> dictionaries() const = 0; // label -> ID
    virtual bool selectLanguage(const QString &language) = 0;
    virtual bool isCorrect(const QString &word) = 0;
    virtual QStringList suggestions(const QString &word) = 0;
    virtual bool learn(const QString &word) = 0;
};

class SpellingController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool available READ available NOTIFY settingsChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList languages READ languages NOTIFY settingsChanged)
public:
    struct Range {
        int start, length;
        bool operator==(const Range &other) const { return start == other.start && length == other.length; }
    };
    // Injected dictionaries don't read/write global settings. Production uses
    // spelling/enabled and spelling/language; an unset language follows locale.
    explicit SpellingController(QObject *parent = nullptr);
    explicit SpellingController(std::unique_ptr<SpellingDictionary> dictionary, QObject *parent = nullptr);
    ~SpellingController() override;
    void attach(QTextDocument *document);
    bool enabled() const { return m_enabled; }
    bool available() const { return m_available; }
    QString language() const { return m_language; }
    QVariantList languages() const;
    void setEnabled(bool enabled);
    void setLanguage(const QString &language);
    void setComposing(bool composing);
    const QList<Range> &ranges() const { return m_ranges; }
    // Cache-only lookup. Suggestions are requested cooperatively; menuChanged
    // announces their arrival. Tokens must be revalidated immediately on use.
    Q_INVOKABLE QVariantMap wordAt(int sourcePosition);
    Q_INVOKABLE bool validate(const QVariantMap &token) const;
    Q_INVOKABLE bool ignore(const QVariantMap &token);
    Q_INVOKABLE bool learn(const QVariantMap &token);
signals:
    void settingsChanged();
    void rangesChanged();
    void menuChanged();
    void operationFailed(const QString &message);
private:
    struct CachedWord { bool correct; bool hasSuggestions = false; QStringList suggestions; };
    void initialize();
    void invalidate();
    void sourceChanged();
    void classify();
    void processBatch();
    void publish();
    void saveSettings();
    QString ignoreKey(const QString &word) const;
    std::unique_ptr<SpellingDictionary> m_dictionary;
    QMap<QString, QString> m_dictionaries;
    QPointer<QTextDocument> m_document;
    QMetaObject::Connection m_documentConnection;
    QTimer m_debounce, m_batch;
    QFutureWatcher<QList<SpellingProse::Word>> m_classifier;
    QCache<QString, CachedWord> m_cache{8192};
    QSet<QString> m_ignored;
    QList<SpellingProse::Word> m_words;
    QList<Range> m_ranges;
    QStringList m_suggestionQueue;
    QString m_source, m_language;
    quint64 m_generation = 0, m_classifyingGeneration = 0;
    int m_nextWord = 0;
    int m_publishedCount = -1;
    bool m_enabled = true, m_available = false, m_persist = false;
    bool m_composing = false, m_classifyAgain = false, m_classifying = false;
};

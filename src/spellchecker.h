#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantList>
#include <QTextDocument>
#include <atomic>
#include <memory>

// Owns a review session, not an as-you-type highlighter. Worker jobs only see
// immutable snapshots; all document changes stay on the GUI thread.
class SpellChecker : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString dictionaryDirectory READ dictionaryDirectory CONSTANT)
    Q_PROPERTY(QVariantList dictionaries READ dictionaries NOTIFY dictionariesChanged)
    Q_PROPERTY(QStringList languages READ languages WRITE setLanguages NOTIFY languagesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantMap current READ current NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit SpellChecker(QObject *parent = nullptr, const QStringList &paths = {});
    ~SpellChecker() override;
    void attachDocument(QTextDocument *document);
    QString dictionaryDirectory() const;
    QVariantList dictionaries() const { return m_dictionaries; }
    QStringList languages() const { return m_languages; }
    void setLanguages(const QStringList &languages);
    bool busy() const { return m_busy; }
    QVariantMap current() const;
    QString status() const { return m_status; }
    Q_INVOKABLE void refreshDictionaries();
    Q_INVOKABLE void start(int start = 0, int end = -1);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void skip(bool all = false);
    Q_INVOKABLE void replace(const QString &replacement);
    Q_INVOKABLE void addWord(const QString &language);

    struct Word { int start; int length; QString text; QStringList suggestions; };
    static QList<Word> proseWords(const QString &text, int start, int end);
signals:
    void dictionariesChanged();
    void languagesChanged();
    void changed();
    void textInvalidated();
    void replacementApplied(int lengthDifference);
private:
    void invalidate(const QString &message);
    bool validCurrent();
    void advance();
    QStringList m_paths;
    QVariantList m_dictionaries;
    QStringList m_languages;
    QPointer<QTextDocument> m_document;
    QString m_snapshot;
    QList<Word> m_words;
    int m_index = 0;
    int m_corrections = 0;
    quint64 m_generation = 0;
    bool m_busy = false;
    bool m_replacing = false;
    QString m_status;
    std::shared_ptr<std::atomic_bool> m_cancel;
};

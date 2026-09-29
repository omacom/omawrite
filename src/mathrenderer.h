#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <optional>

class MathJaxEngine;

// Typesets TeX with MathJax on a worker thread. Results are cached by source,
// so a formula is typeset once however often its block is highlighted.
class MathRenderer : public QObject {
    Q_OBJECT

public:
    struct Result {
        bool ok = false;
        QByteArray svg; // strokes and fills use currentColor
        // Extents in ex of the TeX font; depth is the part below the baseline.
        qreal width = 0;
        qreal height = 0;
        qreal depth = 0;
        QString error;
    };

    // Typeset formulas are kept in cacheDirectory across sessions; without
    // one, they are only kept in memory.
    explicit MathRenderer(const QString &cacheDirectory = {}, QObject *parent = nullptr);
    ~MathRenderer() override;

    // The finished rendering, or nothing while it is still being typeset. The
    // first request for a source queues it; rendered() announces the result.
    // Position is where the formula is in the document.
    std::optional<Result> result(const QString &tex, bool display, int position = 0);

    // Queued formulas nearest this document position are typeset first.
    void setFocus(int position);

    // The document's macro definitions (\newcommand and the like), in order.
    // Every formula is typeset after them and cached under them, so it renders
    // the same whether MathJax has just started or has been running all along.
    // Returns whether they changed, in which case every formula needs asking for
    // again.
    bool setPreamble(const QStringList &definitions);

signals:
    void rendered(const QString &tex, bool display);

private:
    void finish(const QString &key, const QString &tex, bool display, const Result &result);

    QString key(const QString &tex, bool display) const;

    QString m_cacheDirectory;
    QStringList m_preamble;
    QString m_preambleKey; // a hash of the preamble; empty without one
    QThread m_thread;
    MathJaxEngine *m_engine = nullptr;
    std::atomic_bool m_stopping = false;
    QHash<QString, Result> m_results;
    QSet<QString> m_pending; // queued or being typeset
    int m_focus = 0;
};

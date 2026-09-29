#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
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

    explicit MathRenderer(QObject *parent = nullptr);
    ~MathRenderer() override;

    // The finished rendering, or nothing while it is still being typeset. The
    // first request for a source queues it; rendered() announces the result.
    std::optional<Result> result(const QString &tex, bool display);

signals:
    void rendered(const QString &tex, bool display);

private:
    void finish(const QString &key, const QString &tex, bool display, const Result &result);

    QThread m_thread;
    MathJaxEngine *m_engine = nullptr;
    std::atomic_bool m_stopping = false;
    QHash<QString, Result> m_results;
    QSet<QString> m_pending;
};

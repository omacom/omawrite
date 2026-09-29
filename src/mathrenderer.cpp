#include "mathrenderer.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValue>
#include <QLoggingCategory>
#include <QMutex>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <memory>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace {
// Formulas edited from outside their source view (undo, replace all) leave
// stale entries behind; start over rather than grow without bound.
constexpr int maximumCachedResults = 4000;

// Part of every disk cache key: bump it whenever the MathJax build or the
// driver's configuration changes what a formula typesets to.
const QByteArray diskCacheVersion = QByteArrayLiteral("MathJax 3.2.2, SVG, local font cache, 1");
constexpr int maximumDiskCacheFiles = 20000;
constexpr quint32 diskCacheMagic = 0x6d61746a; // "matj"

// MathJax holds 30 to 40 MB; let it go once the document stops asking.
constexpr int engineIdleMs = 60000;
}

struct MathJob {
    QString key;
    QString tex;
    bool display;
    int position;
    QStringList preamble;
};

// Lives on the worker thread. Formulas typeset before are read back from the
// disk cache; the JavaScript engine is started there only for new ones, so
// reopening a note, or a note without math, never loads MathJax.
class MathJaxEngine : public QObject {
public:
    explicit MathJaxEngine(const QString &cacheDirectory) : m_cacheDirectory(cacheDirectory) {}

    // The queue is shared with the GUI thread, which adds to it, so the
    // worker never waits on a busy GUI to learn what comes next.
    void enqueue(const MathJob &job) {
        QMutexLocker locker(&m_queueMutex);
        m_queue.append(job);
    }

    QStringList dropQueued() {
        QMutexLocker locker(&m_queueMutex);
        QStringList keys;
        for (const MathJob &job : std::as_const(m_queue))
            keys.append(job.key);
        m_queue.clear();
        return keys;
    }

    void setFocus(int position) { m_focus = position; }

    // The queued formula nearest the caret, which is usually in view.
    std::optional<MathJob> takeNearest() {
        QMutexLocker locker(&m_queueMutex);
        if (m_queue.isEmpty())
            return std::nullopt;
        const int focus = m_focus;
        const auto nearest = std::min_element(m_queue.cbegin(), m_queue.cend(),
                                              [focus](const MathJob &a, const MathJob &b) {
            return qAbs(a.position - focus) < qAbs(b.position - focus);
        });
        MathJob job = *nearest;
        m_queue.removeAt(nearest - m_queue.cbegin());
        return job;
    }

    MathRenderer::Result render(const QString &key, const QString &tex, bool display,
                                const QStringList &preamble) {
        const QString cachePath = cacheFile(key);
        if (std::optional<MathRenderer::Result> cached = readCache(cachePath))
            return *cached;

        MathRenderer::Result result;
        if (!start(preamble)) {
            result.error = m_startError;
            return result;
        }

        const QJSValue value = m_render.call({tex, display});
        if (value.isError() || m_engine->hasError()) {
            result.error = m_engine->hasError() ? m_engine->catchError().toString()
                                                : value.toString();
        } else if (value.hasProperty(QStringLiteral("error"))) {
            result.error = value.property(QStringLiteral("error")).toString();
        } else {
            result.ok = true;
            result.svg = value.property(QStringLiteral("svg")).toString().toUtf8();
            result.width = value.property(QStringLiteral("width")).toNumber();
            result.height = value.property(QStringLiteral("height")).toNumber();
            result.depth = value.property(QStringLiteral("depth")).toNumber();
        }
        // MathJax churns through short-lived objects; collecting after every
        // formula keeps the heap small and typesets about a sixth faster.
        m_engine->collectGarbage();
        m_idleTimer->start();
        writeCache(cachePath, result);
        return result;
    }

private:
    QString cacheFile(const QString &key) const {
        if (m_cacheDirectory.isEmpty())
            return {};
        const QByteArray hash = QCryptographicHash::hash(diskCacheVersion + '\n' + key.toUtf8(),
                                                         QCryptographicHash::Sha1);
        return m_cacheDirectory + QLatin1Char('/') + QString::fromLatin1(hash.toHex());
    }

    std::optional<MathRenderer::Result> readCache(const QString &path) const {
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
            return std::nullopt;
        QDataStream in(&file);
        quint32 magic = 0;
        MathRenderer::Result result;
        in >> magic >> result.ok >> result.svg >> result.width >> result.height >> result.depth
           >> result.error;
        if (magic != diskCacheMagic || in.status() != QDataStream::Ok)
            return std::nullopt;
        return result;
    }

    // Written atomically, since other Omawrite windows share the directory.
    void writeCache(const QString &path, const MathRenderer::Result &result) {
        if (path.isEmpty() || (!m_pruned && !prepareCache()))
            return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return;
        QDataStream out(&file);
        out << diskCacheMagic << result.ok << result.svg << result.width << result.height
            << result.depth << result.error;
        file.commit();
    }

    // Once a session, before the first write: drop the oldest entries past the cap.
    bool prepareCache() {
        m_pruned = true;
        QDir directory(m_cacheDirectory);
        if (!directory.mkpath(QStringLiteral(".")))
            return false;
        const QFileInfoList entries = directory.entryInfoList(QDir::Files, QDir::Time);
        if (entries.size() > maximumDiskCacheFiles) {
            for (qsizetype i = maximumDiskCacheFiles * 4 / 5; i < entries.size(); ++i)
                QFile::remove(entries.at(i).filePath());
        }
        return true;
    }

    // Starts MathJax if needed and brings its macros up to the preamble.
    bool start(const QStringList &preamble) {
        // A definition cannot be taken back, only replaced, so unless the
        // preamble merely grew, MathJax starts over from the new one.
        if (m_render.isCallable() && m_preamble != preamble.mid(0, m_preamble.size())) {
            m_render = QJSValue();
            m_engine.reset();
        }
        if (m_render.isCallable()) {
            define(preamble);
            return true;
        }
        if (!m_startError.isEmpty())
            return false;

        if (!m_idleTimer) {
            m_idleTimer = new QTimer(this);
            m_idleTimer->setSingleShot(true);
            m_idleTimer->setInterval(engineIdleMs);
            QObject::connect(m_idleTimer, &QTimer::timeout, this, [this]() {
                m_render = QJSValue();
                m_engine.reset();
#ifdef __GLIBC__
                // glibc keeps the freed engine in this thread's arena otherwise.
                malloc_trim(0);
#endif
            });
        }

        // MathJax's minified bundles trip the JavaScript compiler's
        // used-before-declared lint hundreds of times; the code is fine.
        QLoggingCategory::setFilterRules(QStringLiteral("qt.qml.usedbeforedeclared=false"));
        m_engine = std::make_unique<QJSEngine>();
        // MathJax expects its global and may log; a bare engine has no console.
        m_engine->evaluate(QStringLiteral(
            "var MathJax = {};"
            "var console = {log: function () {}, info: function () {},"
            "               warn: function () {}, error: function () {}};"));

        static const char *const scripts[] = {
            "loader.js", "core.js", "adaptors/liteDOM.js", "input/tex-base.js",
            "input/tex-full.js", "output/svg.js", "output/svg/fonts/tex.js", "driver.js"};
        for (const char *script : scripts) {
            const QString path = QStringLiteral(":/mathjax/") + QLatin1String(script);
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                m_startError = QStringLiteral("Could not read %1").arg(path);
                return false;
            }
            const QJSValue value = m_engine->evaluate(QString::fromUtf8(file.readAll()), path);
            if (value.isError() || m_engine->hasError()) {
                const QString error = m_engine->hasError() ? m_engine->catchError().toString()
                                                           : value.toString();
                m_startError = QStringLiteral("Could not load %1: %2").arg(path, error);
                return false;
            }
        }

        m_render = m_engine->globalObject().property(QStringLiteral("omawriteRenderMath"));
        if (!m_render.isCallable()) {
            m_startError = QStringLiteral("The MathJax driver did not start.");
            return false;
        }
        m_preamble.clear();
        define(preamble);
        return true;
    }

    // Typesets the definitions MathJax has not seen, for their side effect.
    void define(const QStringList &preamble) {
        for (qsizetype i = m_preamble.size(); i < preamble.size(); ++i)
            m_render.call({preamble.at(i), false});
        m_preamble = preamble;
    }

    QMutex m_queueMutex;
    QList<MathJob> m_queue;
    std::atomic_int m_focus = 0;
    QString m_cacheDirectory;
    bool m_pruned = false;
    QTimer *m_idleTimer = nullptr;
    QStringList m_preamble; // the definitions the running engine has seen
    // Declared before the function so the function is released first.
    std::unique_ptr<QJSEngine> m_engine;
    QJSValue m_render;
    QString m_startError;
};

MathRenderer::MathRenderer(const QString &cacheDirectory, QObject *parent)
    : QObject(parent), m_cacheDirectory(cacheDirectory) {}

MathRenderer::~MathRenderer() {
    // A job still running on the worker is left to finish; its result is dropped.
    m_stopping = true;
    m_thread.quit();
    m_thread.wait();
}

QString MathRenderer::key(const QString &tex, bool display) const {
    return m_preambleKey + (display ? QLatin1Char('D') : QLatin1Char('I')) + tex;
}

bool MathRenderer::setPreamble(const QStringList &definitions) {
    if (definitions == m_preamble)
        return false;
    m_preamble = definitions;
    m_preambleKey = definitions.isEmpty()
        ? QString()
        : QString::fromLatin1(QCryptographicHash::hash(definitions.join(QLatin1Char('\n')).toUtf8(),
                                                       QCryptographicHash::Sha1).toHex().left(16))
              + QLatin1Char(' ');
    // Renderings under the old definitions, and jobs waiting to make more of
    // them, are of no further use.
    m_results.clear();
    if (m_engine) {
        for (const QString &key : m_engine->dropQueued())
            m_pending.remove(key);
    }
    return true;
}

void MathRenderer::setFocus(int position) {
    m_focus = position;
    if (m_engine)
        m_engine->setFocus(position);
}

std::optional<MathRenderer::Result> MathRenderer::result(const QString &tex, bool display,
                                                         int position) {
    const QString key = this->key(tex, display);
    const auto found = m_results.constFind(key);
    if (found != m_results.cend())
        return found.value();
    if (m_pending.contains(key))
        return std::nullopt;

    if (!m_engine) {
        m_engine = new MathJaxEngine(m_cacheDirectory);
        m_engine->setFocus(m_focus);
        m_engine->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, m_engine, &QObject::deleteLater);
        m_thread.setObjectName(QStringLiteral("MathJax"));
        m_thread.start(QThread::LowPriority);
    }

    m_pending.insert(key);
    m_engine->enqueue({key, tex, display, position, m_preamble});
    // One wake-up per job; each typesets whichever queued formula is nearest
    // the caret by then, and finds nothing to do if its job was dropped.
    MathJaxEngine *engine = m_engine;
    QMetaObject::invokeMethod(engine, [this, engine]() {
        if (m_stopping)
            return;
        const std::optional<MathJob> job = engine->takeNearest();
        if (!job)
            return;
        const Result rendered = engine->render(job->key, job->tex, job->display, job->preamble);
        QMetaObject::invokeMethod(this, [this, job = *job, rendered]() {
            finish(job.key, job.tex, job.display, rendered);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    return std::nullopt;
}

void MathRenderer::finish(const QString &key, const QString &tex, bool display,
                          const Result &result) {
    m_pending.remove(key);
    if (m_results.size() >= maximumCachedResults)
        m_results.clear();
    // A result typeset under definitions since replaced goes under its old key,
    // where nothing will look for it.
    m_results.insert(key, result);
    emit rendered(tex, display);
}

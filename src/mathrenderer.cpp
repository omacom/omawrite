#include "mathrenderer.h"

#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QLoggingCategory>

#include <memory>

namespace {
// Formulas edited from outside their source view (undo, replace all) leave
// stale entries behind; start over rather than grow without bound.
constexpr int maximumCachedResults = 4000;

QString cacheKey(const QString &tex, bool display) {
    return (display ? QLatin1Char('D') : QLatin1Char('I')) + tex;
}
}

// Lives on the worker thread. The JavaScript engine is created there on the
// first formula, so documents without math never load MathJax.
class MathJaxEngine : public QObject {
public:
    MathRenderer::Result render(const QString &tex, bool display) {
        MathRenderer::Result result;
        if (!start()) {
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
        return result;
    }

private:
    bool start() {
        if (m_render.isCallable())
            return true;
        if (!m_startError.isEmpty())
            return false;

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
        return true;
    }

    // Declared before the function so the function is released first.
    std::unique_ptr<QJSEngine> m_engine;
    QJSValue m_render;
    QString m_startError;
};

MathRenderer::MathRenderer(QObject *parent) : QObject(parent) {}

MathRenderer::~MathRenderer() {
    // Jobs still queued on the worker see the flag and return at once.
    m_stopping = true;
    m_thread.quit();
    m_thread.wait();
}

std::optional<MathRenderer::Result> MathRenderer::result(const QString &tex, bool display) {
    const QString key = cacheKey(tex, display);
    const auto found = m_results.constFind(key);
    if (found != m_results.cend())
        return found.value();
    if (m_pending.contains(key))
        return std::nullopt;

    if (!m_engine) {
        m_engine = new MathJaxEngine;
        m_engine->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, m_engine, &QObject::deleteLater);
        m_thread.setObjectName(QStringLiteral("MathJax"));
        m_thread.start(QThread::LowPriority);
    }

    m_pending.insert(key);
    MathJaxEngine *engine = m_engine;
    QMetaObject::invokeMethod(engine, [this, engine, key, tex, display]() {
        if (m_stopping)
            return;
        const Result rendered = engine->render(tex, display);
        QMetaObject::invokeMethod(this, [this, key, tex, display, rendered]() {
            finish(key, tex, display, rendered);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    return std::nullopt;
}

void MathRenderer::finish(const QString &key, const QString &tex, bool display,
                          const Result &result) {
    m_pending.remove(key);
    if (m_results.size() >= maximumCachedResults)
        m_results.clear();
    m_results.insert(key, result);
    emit rendered(tex, display);
}

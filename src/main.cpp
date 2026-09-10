#include <QFont>
#include <QFontDatabase>
#include <QApplication>
#include <QFileOpenEvent>
#include <QIcon>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QUrl>
#include <QWindow>
#include <QFile>
#include <QList>

#include <functional>
#include <utility>

#include "backend.h"
#include "systemtheme.h"

class OmawriteApplication final : public QApplication {
public:
    using QApplication::QApplication;
    using FileOpenHandler = std::function<void(const QUrl &)>;

    void setFileOpenHandler(FileOpenHandler handler) {
        m_fileOpenHandler = std::move(handler);
        QList<QUrl> pendingUrls;
        pendingUrls.swap(m_pendingFileUrls);
        for (const QUrl &url : pendingUrls)
            m_fileOpenHandler(url);
    }

protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::FileOpen) {
            const QUrl url = static_cast<QFileOpenEvent *>(event)->url();
            if (url.isValid()) {
                if (m_fileOpenHandler)
                    m_fileOpenHandler(url);
                else
                    m_pendingFileUrls.append(url);
            }
            return true;
        }
        return QApplication::event(event);
    }

private:
    FileOpenHandler m_fileOpenHandler;
    QList<QUrl> m_pendingFileUrls;
};

int main(int argc, char *argv[]) {
    OmawriteApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("omawrite"));
    app.setDesktopFileName(QStringLiteral("omawrite"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("omawrite")));

    // iA's three writing faces differ only in how many character widths they
    // allow: Mono is fully monospaced, Duo gives W and M more room, and Quattro
    // has four, which is the one iA Writer itself writes in.
    for (const QString &family : {QStringLiteral("Mono"), QStringLiteral("Duo"),
                                  QStringLiteral("Quattro")}) {
        for (const QString &style : {QStringLiteral("Regular"), QStringLiteral("Italic"),
                                     QStringLiteral("Bold"), QStringLiteral("BoldItalic")}) {
            QFontDatabase::addApplicationFont(
                QStringLiteral(":/fonts/iAWriter%1S-%2.ttf").arg(family, style));
        }
    }
    app.setOrganizationName(QStringLiteral("Omacom"));
    app.setOrganizationDomain(QStringLiteral("omacom.io"));

    QQuickStyle::setStyle(QStringLiteral("Material"));
#ifdef Q_OS_MACOS
    QQuickStyle::setStyle(QStringLiteral("macOS"));
#endif

    SystemTheme systemTheme(&app);

    // Carry the desktop's text scale into the default font, so the chrome that
    // inherits it (dialog titles, buttons) grows along with the writing area.
    const QFont interfaceFont(Backend::bundledFontFamilies().constFirst());
    const qreal basePointSize = interfaceFont.pointSizeF() > 0
        ? interfaceFont.pointSizeF()
        : app.font().pointSizeF();
    const auto applyInterfaceFont = [&app, interfaceFont, basePointSize](qreal textScale) {
        QFont scaled = interfaceFont;
        scaled.setPointSizeF(basePointSize * textScale);
        app.setFont(scaled);
    };
#ifndef Q_OS_MACOS
    applyInterfaceFont(systemTheme.textScale());
#endif

    std::function<Backend *()> spawnWindow;
    spawnWindow = [&]() -> Backend * {
        auto *backend = new Backend(&app);
#ifdef Q_OS_MACOS
        backend->setNativeMacChrome(true);
#endif
        backend->setDarkMode(systemTheme.darkMode());
        backend->setTextScale(systemTheme.textScale());

        auto *engine = new QQmlApplicationEngine(&app);
        QObject::connect(engine, &QQmlApplicationEngine::warnings, &app,
                         [](const QList<QQmlError> &warnings) {
            for (const QQmlError &warning : warnings)
                qWarning().noquote() << warning.toString();
        });
        engine->rootContext()->setContextProperty(QStringLiteral("backend"), backend);
        engine->load(QUrl(QStringLiteral("qrc:/Main.qml")));
        if (engine->rootObjects().isEmpty()) {
            qCritical() << "Could not load the Omawrite interface; resource available:"
                        << QFile::exists(QStringLiteral(":/Main.qml"));
            engine->deleteLater();
            backend->deleteLater();
            return nullptr;
        }

        backend->setParentWindow(qobject_cast<QWindow *>(engine->rootObjects().constFirst()));
        QObject::connect(backend, &Backend::newWindowRequested, &app,
                         [&]() { spawnWindow(); });
        QObject::connect(backend, &Backend::detachTabRequested, &app,
                         [backend, &spawnWindow](int index) {
            Backend *created = spawnWindow();
            if (created)
                created->takeDetachedTab(backend, index);
        });
        return backend;
    };

    QObject::connect(&systemTheme, &SystemTheme::darkModeChanged, &app, [](bool darkMode) {
        for (Backend *window : Backend::liveWindows())
            window->setDarkMode(darkMode);
    });
    QObject::connect(&systemTheme, &SystemTheme::textScaleChanged, &app,
                     [applyInterfaceFont](qreal textScale) {
#ifndef Q_OS_MACOS
        applyInterfaceFont(textScale);
#endif
        for (Backend *window : Backend::liveWindows())
            window->setTextScale(textScale);
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        Backend::prepareToQuit();
    });

    const int windowCount = qMax(1, Backend::storedWindowCount());
    for (int i = 0; i < windowCount; ++i) {
        if (!spawnWindow())
            return -1;
    }

    const QStringList args = app.arguments();
    const QList<Backend *> live = Backend::liveWindows();
    Backend *first = live.isEmpty() ? nullptr : live.constFirst();
    if (first && args.size() > 1 && !first->modified())
        first->open(QUrl::fromLocalFile(args.at(1)));

    // Finder delivers documents through QFileOpenEvent rather than argv.
    app.setFileOpenHandler([](const QUrl &url) {
        if (!url.isLocalFile())
            return;
        Backend *target = nullptr;
        for (Backend *window : Backend::liveWindows()) {
            if (window->parentWindow() && window->parentWindow()->isActive()) {
                target = window;
                break;
            }
        }
        if (!target && !Backend::liveWindows().isEmpty())
            target = Backend::liveWindows().constFirst();
        if (target)
            target->open(url);
    });

    return app.exec();
}

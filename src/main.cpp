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

    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono-Italic.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono-Bold.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono-BoldItalic.ttf"));
    app.setOrganizationName(QStringLiteral("Omacom"));
    app.setOrganizationDomain(QStringLiteral("omacom.io"));

    QQuickStyle::setStyle(QStringLiteral("Material"));

    Backend backend(&app);
    SystemTheme systemTheme(&app);
    backend.setDarkMode(systemTheme.darkMode());
    QObject::connect(&systemTheme, &SystemTheme::darkModeChanged, &backend,
                     &Backend::setDarkMode);

    // Carry the desktop's text scale into the default font, so the chrome that
    // inherits it (dialog titles, buttons) grows along with the writing area.
    const QFont interfaceFont(QStringLiteral("IBM Plex Mono"));
    const qreal basePointSize = interfaceFont.pointSizeF() > 0
        ? interfaceFont.pointSizeF()
        : app.font().pointSizeF();
    const auto applyInterfaceFont = [&app, interfaceFont, basePointSize](qreal textScale) {
        QFont scaled = interfaceFont;
        scaled.setPointSizeF(basePointSize * textScale);
        app.setFont(scaled);
    };
    applyInterfaceFont(systemTheme.textScale());

    backend.setTextScale(systemTheme.textScale());
    QObject::connect(&systemTheme, &SystemTheme::textScaleChanged, &backend,
                     [&backend, applyInterfaceFont](qreal textScale) {
        applyInterfaceFont(textScale);
        backend.setTextScale(textScale);
    });

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qWarning().noquote() << warning.toString();
    });
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);

    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Could not load the Omawrite interface; resource available:"
                    << QFile::exists(QStringLiteral(":/Main.qml"));
        return -1;
    }

    backend.setParentWindow(qobject_cast<QWindow *>(engine.rootObjects().constFirst()));

    const QStringList args = app.arguments();
    if (args.size() > 1 && !backend.modified())
        backend.open(QUrl::fromLocalFile(args.at(1)));

    // Finder delivers documents through QFileOpenEvent rather than argv. Keep
    // one document per window, matching the existing New Window behavior.
    app.setFileOpenHandler([&backend](const QUrl &url) {
        if (!url.isLocalFile() || url == backend.fileUrl())
            return;
        const bool alreadyShowingFile = backend.fileUrl().isValid()
            && !backend.fileUrl().isEmpty();
        if (backend.modified() || alreadyShowingFile) {
            QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                    QStringList{url.toLocalFile()});
        } else {
            backend.open(url);
        }
    });

    return app.exec();
}

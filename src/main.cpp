#include <QFont>
#include <QFontDatabase>
#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QUrl>
#include <QWindow>
#include <QFile>

#include "backend.h"
#include "systemtheme.h"
#include "windowmanager.h"
#include "workspacesession.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("omawrite"));
    app.setDesktopFileName(QStringLiteral("omawrite"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("omawrite")));

    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/iAWriterMonoS-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/iAWriterMonoS-Italic.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/iAWriterMonoS-Bold.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/iAWriterMonoS-BoldItalic.ttf"));
    app.setOrganizationName(QStringLiteral("Omacom"));
    app.setOrganizationDomain(QStringLiteral("omacom.io"));

    QQuickStyle::setStyle(QStringLiteral("Material"));

    SystemTheme systemTheme(&app);

    // Carry the desktop's text scale into the default font, so the chrome that
    // inherits it (dialog titles, buttons) grows along with the writing area.
    const QFont interfaceFont(QStringLiteral("iA Writer Mono S"));
    const qreal basePointSize = interfaceFont.pointSizeF() > 0
        ? interfaceFont.pointSizeF()
        : app.font().pointSizeF();
    const auto applyInterfaceFont = [&app, interfaceFont, basePointSize](qreal textScale) {
        QFont scaled = interfaceFont;
        scaled.setPointSizeF(basePointSize * textScale);
        app.setFont(scaled);
    };
    applyInterfaceFont(systemTheme.textScale());

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qWarning().noquote() << warning.toString();
    });
    WorkspaceSession workspaceSession(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    workspaceSession.restore();
    WindowManager windows(&workspaceSession, &engine, QUrl(QStringLiteral("qrc:/Main.qml")));
    windows.setDarkMode(systemTheme.darkMode());
    windows.setTextScale(systemTheme.textScale());
    QObject::connect(&systemTheme, &SystemTheme::darkModeChanged, &windows,
                     &WindowManager::setDarkMode);
    QObject::connect(&systemTheme, &SystemTheme::textScaleChanged, &windows,
                     [&windows, applyInterfaceFont](qreal textScale) {
        applyInterfaceFont(textScale);
        windows.setTextScale(textScale);
    });

    if (windows.restoreWindows() == 0)
        windows.createWindow();
    if (!windows.primaryBackend()) {
        qCritical() << "Could not load the Omawrite interface; resource available:"
                    << QFile::exists(QStringLiteral(":/Main.qml"));
        return -1;
    }
    windows.recoverLegacySnapshots();

    const QStringList args = app.arguments();
    if (args.size() > 1 && !windows.primaryBackend()->modified())
        windows.primaryBackend()->open(QUrl::fromLocalFile(args.at(1)));

    return app.exec();
}

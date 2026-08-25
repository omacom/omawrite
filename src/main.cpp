#include <QFont>
#include <QFontDatabase>
#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QUrl>
#include <QWindow>
#include <QFile>

#include "backend.h"
#include "systemtheme.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("omawrite"));
    app.setDesktopFileName(QStringLiteral("omawrite"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("omawrite")));
    // Backend's constructor reads QSettings, which needs the full application
    // identity in place first.
    app.setOrganizationName(QStringLiteral("Omacom"));
    app.setOrganizationDomain(QStringLiteral("omacom.io"));

    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/iAWriterMonoS-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/iAWriterMonoS-Italic.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/iAWriterMonoS-Bold.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/iAWriterMonoS-BoldItalic.ttf"));

    QQuickStyle::setStyle(QStringLiteral("Material"));

    Backend backend(&app);
    SystemTheme systemTheme(&app);
    backend.setDarkMode(systemTheme.darkMode());
    QObject::connect(&systemTheme, &SystemTheme::darkModeChanged, &backend,
                     &Backend::setDarkMode);

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

    return app.exec();
}

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
#include "cli.h"
#include "portalfilepicker.h"
#include "systemtheme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // Answered before the application exists, so it needs no display.
    if (const std::optional<int> exitCode = Cli::handleArguments(argc, argv))
        return *exitCode;

    QApplication app(argc, argv);
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

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Backend backend(&app);
    PortalFilePicker filePicker(&app);
    SystemTheme systemTheme(&app);
    backend.setDarkMode(systemTheme.darkMode());
    QObject::connect(&systemTheme, &SystemTheme::darkModeChanged, &backend,
                     &Backend::setDarkMode);

    // Carry the text scale into the default font, so the chrome that inherits
    // it (dialog titles, buttons) grows along with the writing area.
    const QFont interfaceFont(QStringLiteral("IBM Plex Mono"));
    const qreal basePointSize = interfaceFont.pointSizeF() > 0
        ? interfaceFont.pointSizeF()
        : app.font().pointSizeF();
    const int basePixelSize = interfaceFont.pixelSize() > 0
        ? interfaceFont.pixelSize()
        : app.font().pixelSize();
    const auto applyInterfaceFont = [&app, interfaceFont, basePointSize, basePixelSize](qreal textScale) {
        QFont scaled = interfaceFont;
        if (basePointSize > 0)
            scaled.setPointSizeF(basePointSize * textScale);
        else if (basePixelSize > 0)
            scaled.setPixelSize(qMax(1, qRound(basePixelSize * textScale)));
        else
            scaled.setPointSizeF(10.0 * textScale);
        app.setFont(scaled);
    };

    // The desktop sets the baseline; the backend folds the user's own text
    // size into it and reports the total.
    backend.setDesktopTextScale(systemTheme.textScale());
    applyInterfaceFont(backend.textScale());
    QObject::connect(&systemTheme, &SystemTheme::textScaleChanged, &backend,
                     &Backend::setDesktopTextScale);
    QObject::connect(&backend, &Backend::textScaleChanged, &app,
                     [&backend, applyInterfaceFont]() {
        applyInterfaceFont(backend.textScale());
    });

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qWarning().noquote() << warning.toString();
    });
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    QObject::connect(&translator, &Translator::changed, &backend, &Backend::retranslate);
    engine.rootContext()->setContextProperty(QStringLiteral("translator"), &translator);
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("filePicker"), &filePicker);

    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Could not load the Omawrite interface; resource available:"
                    << QFile::exists(QStringLiteral(":/Main.qml"));
        return -1;
    }

    backend.setParentWindow(qobject_cast<QWindow *>(engine.rootObjects().constFirst()));
    // Unsaved drafts that more tabs left behind come back as tabs of their own.
    QMetaObject::invokeMethod(engine.rootObjects().constFirst(), "openRecoveredDrafts");

    const QStringList args = app.arguments();
    if (args.size() > 1)
        backend.openOrStartFile(QUrl::fromLocalFile(args.at(1)));

    return app.exec();
}

#include "spellingcontroller.h"

#include <QJsonObject>
#include <QPluginLoader>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QtTest>

class NativeSpellcheckingTest : public QObject {
    Q_OBJECT
private slots:
    void registersNativeBackend() {
#ifdef Q_OS_MACOS
        const QString expected = QStringLiteral("NSSpellCheckerClient");
#elif defined(Q_OS_WIN)
        const QString expected = QStringLiteral("ISpellCheckerClient");
#else
        const QString expected = QStringLiteral("HunspellClient");
#endif
        // Do not mistake a discarded static plugin for missing OS dictionaries.
        // This check is independent of installed dictionaries and GUI services.
        for (const auto &plugin : QPluginLoader::staticPlugins()) {
            const QJsonObject metadata = plugin.metaData();
            if (metadata.value(QStringLiteral("IID")).toString() == QStringLiteral("org.kde.sonnet.Client")
                && metadata.value(QStringLiteral("className")).toString() == expected)
                return;
        }
        QFAIL(qPrintable(QStringLiteral("Native spelling plugin was not linked/registered: %1").arg(expected)));
    }

    void osDictionarySmoke() {
        if (qEnvironmentVariable("OMAWRITE_NATIVE_SPELLCHECK") != QStringLiteral("1"))
            QSKIP("Manual opt-in only: set OMAWRITE_NATIVE_SPELLCHECK=1 on macOS.");
#ifndef Q_OS_MACOS
        QSKIP("This smoke test exercises the macOS NSSpellChecker backend only.");
#else
        QTemporaryDir settingsDirectory;
        QVERIFY(settingsDirectory.isValid());
        QCoreApplication::setOrganizationName("OmawriteNativeSpellingTest");
        QCoreApplication::setApplicationName("IsolatedSmokeTest");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());

        QTextDocument document("hello qzxqzxqzxqzx");
        SpellingController controller;
        qInfo() << "Native spelling dictionaries:" << controller.languages().size();
        QString english;
        for (const auto &entry : controller.languages()) {
            const QString id = entry.toMap().value("id").toString();
            if (id == QStringLiteral("en_US") || id == QStringLiteral("en")) { english = id; break; }
            if (english.isEmpty() && id.section(QLatin1Char('_'), 0, 0) == QStringLiteral("en"))
                english = id; // macOS may expose simply "en" rather than en_US
        }
        if (english.isEmpty())
            QSKIP("The OS does not provide an English dictionary; enable one before running this smoke test.");
        controller.setLanguage(english);
        controller.setEnabled(true);
        QVERIFY2(controller.available(), "The OS English dictionary could not be selected.");
        controller.attach(&document);
        QTRY_COMPARE_WITH_TIMEOUT(controller.ranges().size(), 1, 10000);
        QCOMPARE(controller.ranges().first().start, 6);
        QCOMPARE(controller.ranges().first().length, 12);
        QVERIFY(controller.wordAt(0).isEmpty()); // Known English word is accepted.
        const auto token = controller.wordAt(6);
        QVERIFY(controller.validate(token));
        QCOMPARE(token.value("word").toString(), QString("qzxqzxqzxqzx"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.wordAt(6).value("pending").toBool(), 10000);
        // The OS may legitimately have no suggestions; never assert their content.
        QVERIFY(controller.validate(token));
        // Deliberately never call learn(): smoke tests must not change user words.
#endif
    }
};

QTEST_MAIN(NativeSpellcheckingTest)
#include "tst_native.moc"

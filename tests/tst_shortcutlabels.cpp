#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QJSValue>
#include <QQuickStyle>
#include <QQuickItem>
#include <memory>

class ShortcutLabelsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    void previewToggleLabelsAndCentering() {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(
            QFINDTESTDATA("../src/PreviewToggle.qml")));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *button = qobject_cast<QQuickItem *>(object.get());
        auto *canvas = object->findChild<QQuickItem *>("previewToggleCanvas");
        QVERIFY(button && canvas);
        for (const QString &platform : {QStringLiteral("osx"), QStringLiteral("linux"), QStringLiteral("windows")}) {
            object->setProperty("shortcutPlatform", platform);
            QCOMPARE(object->property("shortcutText").toString(),
                     platform == "osx" ? QStringLiteral("Cmd+Shift+M") : QStringLiteral("Ctrl+Shift+M"));
        }
        for (int size : {40, 52}) {
            button->setWidth(size);
            button->setHeight(size);
            QCoreApplication::processEvents();
            const QPointF center = canvas->mapToItem(button, QPointF(canvas->width() / 2, canvas->height() / 2));
            QCOMPARE(center, QPointF(size / 2.0, size / 2.0));
        }
    }

    void labels_data() {
        QTest::addColumn<QString>("platform");
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("expected");
        QTest::newRow("mac-save") << "osx" << "Ctrl+S" << "Cmd+S";
        QTest::newRow("mac-fullscreen") << "osx" << "F11 / Ctrl+Meta+F" << "F11 / Cmd+Ctrl+F";
        QTest::newRow("mac-replace") << "osx" << "Ctrl+Alt+F" << "Cmd+Option+F";
        QTest::newRow("mac-reading") << "osx" << "Ctrl+Shift+M" << "Cmd+Shift+M";
        QTest::newRow("mac-redo") << "osx" << "Ctrl+Shift+Z / Ctrl+Y" << "Cmd+Shift+Z / Cmd+Y";
        QTest::newRow("mac-zoom") << "osx" << "Ctrl+= / Ctrl++" << "Cmd+= / Cmd++";
        QTest::newRow("mac-scroll") << "osx" << "Ctrl+scroll" << "Cmd+scroll";
        QTest::newRow("mac-unsaved") << "osx" << "Alt+C / Escape" << "Option+C / Escape";
        QTest::newRow("linux-fullscreen") << "linux" << "F11 / Meta+F" << "F11 / Super+F";
        QTest::newRow("linux-replace") << "linux" << "Ctrl+H" << "Ctrl+H";
        QTest::newRow("windows-reading") << "windows" << "Ctrl+Shift+M" << "Ctrl+Shift+M";
        QTest::newRow("unmodified") << "osx" << "Shift+Enter / F1" << "Shift+Enter / F1";
    }

    void labels() {
        QFETCH(QString, platform);
        QFETCH(QString, source);
        QFETCH(QString, expected);
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(
            QFINDTESTDATA("../src/ShortcutsDialog.qml")));
        std::unique_ptr<QObject> dialog(component.create());
        QVERIFY2(dialog, qPrintable(component.errorString()));
        QVERIFY(dialog->setProperty("platform", platform));
        QVariant result;
        QVERIFY(QMetaObject::invokeMethod(dialog.get(), "shortcutLabel",
            Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, source)));
        QCOMPARE(result.toString(), expected);
    }

    void referenceUpdatesWhenPlatformChanges() {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(
            QFINDTESTDATA("../src/ShortcutsDialog.qml")));
        std::unique_ptr<QObject> dialog(component.create());
        QVERIFY2(dialog, qPrintable(component.errorString()));
        const auto keyFor = [&dialog](const QString &description) {
            const QJSValue entries = dialog->property("shortcuts").value<QJSValue>();
            for (quint32 i = 0; i < entries.property("length").toUInt(); ++i) {
                const QJSValue entry = entries.property(i);
                if (entry.property("desc").toString() == description)
                    return entry.property("key").toString();
            }
            return QString();
        };
        for (const QString &platform : {QStringLiteral("linux"), QStringLiteral("osx"),
                                       QStringLiteral("windows"), QStringLiteral("osx")}) {
            QVERIFY(dialog->setProperty("platform", platform));
            const bool mac = platform == "osx";
            QCOMPARE(keyFor("Save"), mac ? "Cmd+S" : "Ctrl+S");
            QCOMPARE(keyFor("Fullscreen"), mac ? "F11 / Cmd+Ctrl+F" : "F11 / Super+F");
            QCOMPARE(keyFor("Find & Replace"), mac ? "Cmd+Option+F" : "Ctrl+H");
            QCOMPARE(keyFor("Toggle Reading Mode"), mac ? "Cmd+Shift+M" : "Ctrl+Shift+M");
            QCOMPARE(keyFor("Save (Unsaved Changes)"), mac ? "Option+S" : "Alt+S");
            QCOMPARE(keyFor("Discard (Unsaved Changes)"), mac ? "Option+D" : "Alt+D");
            QCOMPARE(keyFor("Cancel (Unsaved Changes)"), mac ? "Option+C / Escape" : "Alt+C / Escape");
        }
    }
};

QTEST_MAIN(ShortcutLabelsTest)
#include "tst_shortcutlabels.moc"

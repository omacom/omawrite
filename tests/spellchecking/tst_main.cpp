#include <QtTest>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QTextDocument>
#include <memory>

#include "backend.h"
#include "livepreview.h"
#include "spellingcontroller.h"

namespace {
class FakeDictionary final : public SpellingDictionary {
public:
    QMap<QString, QString> dictionaries() const override {
        return {{QStringLiteral("English"), QStringLiteral("en_US")},
                {QStringLiteral("French"), QStringLiteral("fr_FR")}};
    }
    bool selectLanguage(const QString &language) override {
        return dictionaries().values().contains(language);
    }
    bool isCorrect(const QString &word) override { return word != QLatin1String("typo"); }
    QStringList suggestions(const QString &) override { return {QStringLiteral("fixed")}; }
    bool learn(const QString &) override { return true; }
};

QObject *menuItem(QObject *menu, const QString &text) {
    for (QObject *child : menu->findChildren<QObject *>()) {
        if (child->property("text").toString() == text
                && child->metaObject()->indexOfSignal("triggered()") >= 0)
            return child;
    }
    return nullptr;
}
}

class SpellingMainTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    std::unique_ptr<Backend> backend;
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root;
    SpellingController *spelling = nullptr;
    QQuickWindow *window = nullptr;
    QObject *source = nullptr;
    QQuickItem *view = nullptr;
    QTextDocument *document = nullptr;
    QStringList warnings;

    QString text() const { return source->property("text").toString(); }
    void setText(const QString &value) {
        source->setProperty("text", value);
        source->setProperty("cursorPosition", 1);
        QCoreApplication::processEvents();
        document->clearUndoRedoStacks();
    }
    bool openAt(int position) {
        QRectF rectangle;
        if (!QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, rectangle),
                                       Q_ARG(int, position)))
            return false;
        QVariant result;
        return QMetaObject::invokeMethod(window, "openEditorMenuAt", Q_RETURN_ARG(QVariant, result),
                                         Q_ARG(QVariant, rectangle.x()),
                                         Q_ARG(QVariant, rectangle.center().y())) && result.toBool();
    }
    void keyboardMenu(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMetaObject::invokeMethod(view, "forceActiveFocus");
        QTest::keyClick(window, key, modifiers);
    }

private slots:
    void initTestCase() {
        QVERIFY(settings.isValid());
        const QString id = QFileInfo(settings.path()).fileName();
        QCoreApplication::setOrganizationName(QStringLiteral("omawrite-spelling-main-") + id);
        QCoreApplication::setApplicationName(QStringLiteral("omawrite-spelling-main-") + id);
        QCoreApplication::setApplicationVersion(QStringLiteral("1.2.3-test"));
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        qmlRegisterType<LivePreview>("Omawrite", 1, 0, "LivePreview");
    }
    void init() {
        warnings.clear();
        backend = std::make_unique<Backend>(std::make_unique<FakeDictionary>());
        backend->holdAutoSave(true);
        spelling = qobject_cast<SpellingController *>(backend->spelling());
        QVERIFY(spelling);
        spelling->setLanguage(QStringLiteral("en_US"));
        spelling->setEnabled(true);
        QVERIFY(spelling->available());
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.append(error.toString());
        });
        const QString path = QFINDTESTDATA("../../src/Main.qml");
        QVERIFY(!path.isEmpty());
        engine->rootContext()->setContextProperty(QStringLiteral("backend"), backend.get());
        QQmlComponent component(engine.get(), QUrl::fromLocalFile(path));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        source = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        view = window->findChild<QQuickItem *>(QStringLiteral("previewEditor"));
        QVERIFY(source && view);
        auto *quick = qvariant_cast<QQuickTextDocument *>(source->property("textDocument"));
        QVERIFY(quick);
        document = quick->textDocument();
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
    }
    void cleanup() {
        root.reset();
        engine.reset();
        backend.reset();
        QCoreApplication::processEvents();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    }
    void replacementIsOneUndoAndRejectsStaleTokens() {
        const QString original = QString::fromUtf8("😀 typo and `typo`\n\nlast line\n");
        const QString corrected = QString::fromUtf8("😀 fixed and `typo`\n\nlast line\n");
        setText(original);
        // No manual controller/document attachment: Main.qml must wire both services.
        QTRY_COMPARE(spelling->ranges().size(), 1);
        const auto token = spelling->wordAt(4);
        QCOMPARE(token.value(QStringLiteral("word")).toString(), QStringLiteral("typo"));
        QVERIFY(backend->replaceSpelling(token, QStringLiteral("fixed")));
        QCOMPARE(text(), corrected);
        QVERIFY(source->property("canUndo").toBool());
        QVERIFY(QMetaObject::invokeMethod(source, "undo"));
        QCOMPARE(text(), original);
        QVERIFY(!source->property("canUndo").toBool());
        QVERIFY(QMetaObject::invokeMethod(source, "redo"));
        QCOMPARE(text(), corrected);
        QVERIFY(!backend->replaceSpelling(token, QStringLiteral("fixed")));
        QCOMPARE(text(), corrected);

        setText(QStringLiteral("typo tail"));
        QTRY_COMPARE(spelling->ranges().size(), 1);
        const auto beforeEdit = spelling->wordAt(1);
        QVERIFY(spelling->validate(beforeEdit));
        QVERIFY(QMetaObject::invokeMethod(source, "insert", Q_ARG(int, 0),
                                         Q_ARG(QString, QStringLiteral("prefix "))));
        const QString edited = text();
        QVERIFY(!backend->replaceSpelling(beforeEdit, QStringLiteral("fixed")));
        QCOMPARE(text(), edited);
        QTRY_COMPARE(spelling->ranges().size(), 1);
        const auto beforeLanguage = spelling->wordAt(8);
        QVERIFY(spelling->validate(beforeLanguage));
        spelling->setLanguage(QStringLiteral("fr_FR"));
        QVERIFY(!backend->replaceSpelling(beforeLanguage, QStringLiteral("fixed")));
        QCOMPARE(text(), edited);
    }
    void pointerAndKeyboardMenus() {
        setText(QStringLiteral("typo tail"));
        QTRY_COMPARE(spelling->ranges().size(), 1);
        auto *menu = window->findChild<QObject *>(QStringLiteral("editorContextMenu"));
        auto *languages = window->findChild<QObject *>(QStringLiteral("spellingMenu"));
        auto *toggle = window->findChild<QObject *>(QStringLiteral("spellingEnabledAction"));
        QVERIFY(menu && languages && toggle);
        QCOMPARE(languages->property("title").toString(), QStringLiteral("Spelling"));
        QVERIFY(languages->property("enabled").toBool());
        QVERIFY(toggle->property("enabled").toBool());
        QVERIFY(toggle->property("checkable").toBool());
        QVERIFY(toggle->property("checked").toBool());
        QTRY_VERIFY(menuItem(languages, QStringLiteral("English")));
        QTRY_VERIFY(menuItem(languages, QStringLiteral("French")));
        QVERIFY(menuItem(languages, QStringLiteral("English"))->property("checked").toBool());
        QVERIFY(spelling->wordAt(1).value(QStringLiteral("pending")).toBool());
        QVERIFY(openAt(1));
        QVERIFY(!menuItem(menu, QStringLiteral("fixed")));
        QTRY_VERIFY(menu->property("visible").toBool());
        QVERIFY(menu->property("hasWord").toBool());
        // First lookup queues suggestions; Main's Connections and Instantiator
        // must update the already-open menu without another pointer event.
        QTRY_VERIFY(menuItem(menu, QStringLiteral("fixed")));
        QVERIFY(QMetaObject::invokeMethod(menuItem(menu, QStringLiteral("fixed")), "triggered"));
        QTRY_COMPARE(text(), QStringLiteral("fixed tail"));
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY(!menu->property("visible").toBool());

        setText(QStringLiteral("typo tail"));
        QTRY_COMPARE(spelling->ranges().size(), 1);
        for (const auto key : {Qt::Key_Menu, Qt::Key_F10}) {
            keyboardMenu(key, key == Qt::Key_F10 ? Qt::ShiftModifier : Qt::NoModifier);
            QTRY_VERIFY(menu->property("visible").toBool());
            QVERIFY(menu->property("hasWord").toBool());
            QTRY_VERIFY(menuItem(menu, QStringLiteral("fixed")));
            QVERIFY(QMetaObject::invokeMethod(menu, "close"));
            QTRY_VERIFY(!menu->property("visible").toBool());
        }
        QVERIFY(QMetaObject::invokeMethod(toggle, "triggered"));
        QVERIFY(!spelling->enabled());
        QVERIFY(!toggle->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(toggle, "triggered"));
        QVERIFY(spelling->enabled());
        QVERIFY(QMetaObject::invokeMethod(menuItem(languages, QStringLiteral("French")), "triggered"));
        QCOMPARE(spelling->language(), QStringLiteral("fr_FR"));
        QVERIFY(menuItem(languages, QStringLiteral("French"))->property("checked").toBool());
    }
    void attachmentMenuHasPriority() {
        setText(QStringLiteral("![typo](img/missing.png)\n\ntypo"));
        source->setProperty("cursorPosition", 4);
        QCoreApplication::processEvents();
        auto *attachment = window->findChild<QObject *>(QStringLiteral("attachmentMenu"));
        auto *menu = window->findChild<QObject *>(QStringLiteral("editorContextMenu"));
        QVERIFY(attachment && menu);
        keyboardMenu(Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(attachment->property("visible").toBool());
        QCOMPARE(attachment->property("imagePath").toString(), QStringLiteral("img/missing.png"));
        QVERIFY(!menu->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(attachment, "close"));
    }
};

QTEST_MAIN(SpellingMainTest)
#include "tst_main.moc"

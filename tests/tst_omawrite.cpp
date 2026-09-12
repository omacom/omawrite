#include <QtTest>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include "backend.h"
#include "markdownhighlighter.h"

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_settingsDirectory.isValid());
        // Recovery drafts go to AppDataLocation; keep the suite out of the
        // real one so a test run cannot touch a live pad's state.
        QStandardPaths::setTestModeEnabled(true);
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDirectory.path());
    }

    void countsWords() {
        QCOMPARE(Backend::countWords(QStringLiteral("one two-three don't 42")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好 世界")), 2);
        QCOMPARE(Backend::countWords(QString()), 0);
    }

    void normalizesLinks() {
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("www.example.com/path")),
                 QStringLiteral("https://www.example.com/path"));
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("mailto:writer@example.com")),
                 QStringLiteral("mailto:writer@example.com"));
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("example.com")).isEmpty());
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("file:///tmp/private")).isEmpty());
    }

    void suggestsSafeNames() {
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("My first draft\nBody")),
                 QStringLiteral("My first draft.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("A/B")), QStringLiteral("A-B.md"));
        QCOMPARE(Backend::suggestedFileName(QString()), QStringLiteral("Untitled.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("Already.md")),
                 QStringLiteral("Already.md"));
    }

    void findsInlineMarkdownRanges() {
        const auto markup = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic* and [site](https://example.com)"));
        QCOMPARE(markup.size(), 3);
        QCOMPARE(markup.at(0).content.start, 2);
        QCOMPARE(markup.at(0).content.length, 4);
        QCOMPARE(markup.at(2).content.length, 4);
        QCOMPARE(markup.at(2).markers[0].length, 1);
    }

    void loadsCurrentOmarchyTheme() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());

        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        const QString themeDirectory = homeDirectory.path()
            + QStringLiteral("/.local/state/omarchy/current/theme");
        QVERIFY(QDir().mkpath(themeDirectory));

        QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
        QVERIFY(colorsFile.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray palette(
            "mode = \"light\"\n"
            "accent = \"#112233\"\n"
            "selection = \"#445566\"\n"
            "background = \"#fefefe\"\n"
            "foreground = \"#101010\"\n");
        QCOMPARE(colorsFile.write(palette), qint64(palette.size()));
        colorsFile.close();

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#fefefe"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#101010"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QVERIFY(!backend.darkMode());
    }

    void ignoresFileWatcherEventsForSavedContents() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("first-save.md"));
        Backend backend;
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        backend.saveAs(QUrl::fromLocalFile(path));
        QVERIFY(QFileInfo::exists(path));

        QFile sameContents(path);
        QVERIFY(sameContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        sameContents.close();
        QTest::qWait(100);
        QCOMPARE(externalChangeSpy.count(), 0);

        QFile changedContents(path);
        QVERIFY(changedContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(changedContents.write("changed elsewhere"), qint64(17));
        changedContents.close();
        QTRY_COMPARE(externalChangeSpy.count(), 1);
    }

    void keepsCursorAndSelectionStableAcrossInsertions() {
        const QString mutationsPath = QFINDTESTDATA("../src/EditorMutations.js");
        QVERIFY(!mutationsPath.isEmpty());

        QQmlEngine engine;
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import "EditorMutations.js" as EditorMutations

            TextEdit {
                property string insertionText
                property int insertionCursor
                property string wrappedText
                property int wrappedSelectionStart
                property int wrappedSelectionEnd

                Component.onCompleted: {
                    text = "alpha omega";
                    cursorPosition = 5;
                    EditorMutations.replaceRange(this, 5, 5, "one\r\ntwo");
                    insertionText = text;
                    insertionCursor = cursorPosition;

                    text = "alpha beta omega";
                    select(6, 10);
                    EditorMutations.replaceRange(this, selectionStart, selectionEnd,
                                                 "**beta**", 2, 6);
                    wrappedText = text;
                    wrappedSelectionStart = selectionStart;
                    wrappedSelectionEnd = selectionEnd;
                }
            }
        )QML";
        const QUrl harnessUrl = QUrl::fromLocalFile(
            QFileInfo(mutationsPath).absolutePath() + QStringLiteral("/MutationHarness.qml"));
        component.setData(harness, harnessUrl);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        QCOMPARE(editor->property("insertionText").toString(),
                 QStringLiteral("alphaone\ntwo omega"));
        QCOMPARE(editor->property("insertionCursor").toInt(), 12);
        QCOMPARE(editor->property("wrappedText").toString(),
                 QStringLiteral("alpha **beta** omega"));
        QCOMPARE(editor->property("wrappedSelectionStart").toInt(), 8);
        QCOMPARE(editor->property("wrappedSelectionEnd").toInt(), 12);
    }

    void savesAndOpensFromFooterButtons() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QVERIFY(window->findChild<QObject *>(QStringLiteral("sourceEditor")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("renderedPreview")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("modeToggle")));

        QObject *saveButton = window->findChild<QObject *>(QStringLiteral("saveButton"));
        QObject *openButton = window->findChild<QObject *>(QStringLiteral("openButton"));
        QVERIFY(saveButton);
        QVERIFY(openButton);

        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(saveButton, "clicked"));
        QCOMPARE(saveDialogSpy.count(), 1);

        QSignalSpy openDialogSpy(&backend, &Backend::openDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(openButton, "clicked"));
        QCOMPARE(openDialogSpy.count(), 1);
    }

    // --- OmaPad: pad mode -------------------------------------------------
    //
    // The pad has no dirty state to resolve on the way out: edits land on disk
    // behind a short debounce, and flushPad() drains whatever is still in it.

    void autosavesInPadMode() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("pad.md"));

        Backend backend;
        backend.setPadMode(true);
        QScopedPointer<QObject> editor(createEditor());
        QVERIFY(editor);
        backend.attachDocument(editor->property("textDocument").value<QObject *>());
        backend.saveAs(QUrl::fromLocalFile(path));

        editor->setProperty("text", QStringLiteral("typed into the pad"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        // No keystroke, no button: the debounce alone puts it on disk.
        QTRY_COMPARE(readAll(path), QStringLiteral("typed into the pad"));
        QVERIFY(!backend.modified());
    }

    void flushPadWritesWithoutWaitingForTheDebounce() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("pad.md"));

        Backend backend;
        backend.setPadMode(true);
        QScopedPointer<QObject> editor(createEditor());
        QVERIFY(editor);
        backend.attachDocument(editor->property("textDocument").value<QObject *>());
        backend.saveAs(QUrl::fromLocalFile(path));

        editor->setProperty("text", QStringLiteral("last keystroke"));
        QVERIFY(backend.editorTextChanged());

        // This is what closing the window does. It must land synchronously,
        // because the window is gone immediately afterwards.
        backend.flushPad();
        QCOMPARE(readAll(path), QStringLiteral("last keystroke"));
        QVERIFY(!backend.modified());
    }

    void leavesSavingAloneOutsidePadMode() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("doc.md"));

        Backend backend;
        QScopedPointer<QObject> editor(createEditor());
        QVERIFY(editor);
        backend.attachDocument(editor->property("textDocument").value<QObject *>());
        backend.saveAs(QUrl::fromLocalFile(path));

        editor->setProperty("text", QStringLiteral("ordinary editing"));
        QVERIFY(backend.editorTextChanged());

        QTest::qWait(700);
        QCOMPARE(readAll(path), QString());
        QVERIFY(backend.modified());
    }

    void padWindowDropsTheDirtyMarker() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        backend.setPadMode(true);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        // The pad is always saved, so the title never carries the dirty marker.
        QVERIFY(!window->property("title").toString().startsWith(QLatin1Char('*')));
        QVERIFY(window->property("title").toString().endsWith(QStringLiteral(" - Omawrite")));
    }

    void remembersThePadCursorPerFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString first = directory.filePath(QStringLiteral("first.md"));
        const QString second = directory.filePath(QStringLiteral("second.md"));

        Backend backend;
        backend.setPadMode(true);
        QScopedPointer<QObject> editor(createEditor());
        QVERIFY(editor);
        backend.attachDocument(editor->property("textDocument").value<QObject *>());

        backend.saveAs(QUrl::fromLocalFile(first));
        // Nothing remembered yet: the editor reads -1 as end of document.
        QCOMPARE(backend.padCursorPosition(), -1);
        backend.savePadCursorPosition(12);
        QCOMPARE(backend.padCursorPosition(), 12);

        // A second pad keeps its own caret rather than inheriting the first.
        backend.saveAs(QUrl::fromLocalFile(second));
        QCOMPARE(backend.padCursorPosition(), -1);
        backend.savePadCursorPosition(3);
        QCOMPARE(backend.padCursorPosition(), 3);

        backend.saveAs(QUrl::fromLocalFile(first));
        QCOMPARE(backend.padCursorPosition(), 12);
    }

    void restoresThePadCursorOnOpen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("pad.md"));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Text));
        seed.write("# pad\n\nsome text to put a caret into\n");
        seed.close();

        Backend backend;
        backend.setPadMode(true);
        QScopedPointer<QObject> editor(createEditor());
        QVERIFY(editor);
        backend.attachDocument(editor->property("textDocument").value<QObject *>());

        QSignalSpy restoreSpy(&backend, &Backend::padCursorRestoreRequested);
        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(restoreSpy.count(), 1);
        QCOMPARE(restoreSpy.takeFirst().at(0).toInt(), -1);

        backend.savePadCursorPosition(9);
        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(restoreSpy.count(), 1);
        QCOMPARE(restoreSpy.takeFirst().at(0).toInt(), 9);
    }

    void doesNotRememberACursorOutsidePadMode() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("doc.md"));

        Backend backend;
        QScopedPointer<QObject> editor(createEditor());
        QVERIFY(editor);
        backend.attachDocument(editor->property("textDocument").value<QObject *>());
        backend.saveAs(QUrl::fromLocalFile(path));

        QSignalSpy restoreSpy(&backend, &Backend::padCursorRestoreRequested);
        backend.savePadCursorPosition(7);
        QCOMPARE(backend.padCursorPosition(), -1);

        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(restoreSpy.count(), 0);
    }

    // A draft left by an abnormally-ended session used to be adopted on
    // attach, which marks the document modified — and main() only opens the
    // file when it is unmodified. The pad would then autosave that stale draft
    // straight over the newer file on disk, with nobody typing anything.
    void padModeIgnoresAndClearsARecoveryDraft() {
        const QString stateDirectory =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(!stateDirectory.isEmpty());
        QDir().mkpath(stateDirectory);
        const QDir stateDir(stateDirectory);
        for (const QString &leftover : stateDir.entryList({QStringLiteral("recovery-*.json")}, QDir::Files))
            QFile::remove(stateDir.filePath(leftover));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("pad.md"));
        QFile disk(path);
        QVERIFY(disk.open(QIODevice::WriteOnly | QIODevice::Text));
        disk.write("the newer content that is actually on disk\n");
        disk.close();

        QFile draft(stateDir.filePath(QStringLiteral("recovery-0.json")));
        QVERIFY(draft.open(QIODevice::WriteOnly));
        draft.write(QJsonDocument(QJsonObject{
            {QStringLiteral("fileUrl"), QUrl::fromLocalFile(path).toString()},
            {QStringLiteral("text"), QStringLiteral("a stale draft")}}).toJson());
        draft.close();

        Backend backend;
        backend.setPadMode(true);
        QScopedPointer<QObject> editor(createEditor());
        QVERIFY(editor);
        backend.attachDocument(editor->property("textDocument").value<QObject *>());

        QVERIFY(!backend.modified());
        QVERIFY(!QFileInfo::exists(stateDir.filePath(QStringLiteral("recovery-0.json"))));

        backend.open(QUrl::fromLocalFile(path));
        backend.flushPad();
        QCOMPARE(readAll(path),
                 QStringLiteral("the newer content that is actually on disk\n"));
    }

    void padModeWritesNoRecoveryDrafts() {
        const QString stateDirectory =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(stateDirectory);
        const QDir stateDir(stateDirectory);
        for (const QString &leftover : stateDir.entryList({QStringLiteral("recovery-*.json")}, QDir::Files))
            QFile::remove(stateDir.filePath(leftover));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("pad.md"));

        Backend backend;
        backend.setPadMode(true);
        QScopedPointer<QObject> editor(createEditor());
        QVERIFY(editor);
        backend.attachDocument(editor->property("textDocument").value<QObject *>());
        backend.saveAs(QUrl::fromLocalFile(path));

        editor->setProperty("text", QStringLiteral("typing into a pad"));
        QVERIFY(backend.editorTextChanged());
        QTest::qWait(900);

        QCOMPARE(stateDir.entryList({QStringLiteral("recovery-*.json")}, QDir::Files).count(), 0);
    }

    void scalesTextWithDesktopTextSize() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 20);

        // `omarchy display text size 16` sets the GNOME factor to 16/12.
        backend.setTextScale(16.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 27);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 27);

        backend.setTextScale(9.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 15);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 15);
    }

    void remembersLastSaveDirectory() {
        QTemporaryDir saveDirectory;
        QVERIFY(saveDirectory.isValid());

        const QString savedPath = saveDirectory.filePath(QStringLiteral("first.md"));
        Backend savedDocument;
        savedDocument.saveAs(QUrl::fromLocalFile(savedPath));

        Backend nextDocument;
        QSignalSpy saveDialogSpy(&nextDocument, &Backend::saveDialogRequested);
        nextDocument.saveAsDialog();
        QCOMPARE(saveDialogSpy.count(), 1);

        const QUrl suggestedUrl = saveDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).absolutePath(),
                 saveDirectory.path());
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).fileName(),
                 QStringLiteral("Untitled.md"));

        QSettings().setValue(QStringLiteral("file/lastSaveDirectory"),
                             saveDirectory.filePath(QStringLiteral("missing")));
        Backend fallbackDocument;
        QSignalSpy fallbackDialogSpy(&fallbackDocument, &Backend::saveDialogRequested);
        fallbackDocument.saveAsDialog();
        const QUrl fallbackUrl = fallbackDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(fallbackUrl.toLocalFile()).absolutePath(), QDir::homePath());
    }

private:
    QTemporaryDir m_settingsDirectory;
private:
    // A minimal stand-in for the editor in Main.qml: a TextEdit whose
    // QQuickTextDocument is what Backend::attachDocument expects.
    QObject *createEditor() {
        m_editorComponent.reset(new QQmlComponent(&m_editorEngine));
        m_editorComponent->setData(QByteArrayLiteral(
            "import QtQuick\nTextEdit { textFormat: TextEdit.PlainText }"), QUrl());
        if (!m_editorComponent->isReady()) {
            qWarning() << m_editorComponent->errorString();
            return nullptr;
        }
        return m_editorComponent->create();
    }

    static QString readAll(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        return QString::fromUtf8(file.readAll());
    }

    QQmlEngine m_editorEngine;
    QScopedPointer<QQmlComponent> m_editorComponent;

};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"

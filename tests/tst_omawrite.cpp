#include <QtTest>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QTextDocument>
#include <QTextLayout>

#include "backend.h"
#include "markdownhighlighter.h"

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_settingsDirectory.isValid());
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

    void togglesFocusMode() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("focus-test.md"));

        Backend backend;
        QSignalSpy focusSpy(&backend, &Backend::focusModeChanged);
        QVERIFY(!backend.focusMode());

        backend.saveAs(QUrl::fromLocalFile(path));
        QCOMPARE(backend.status(), QStringLiteral("Saved focus-test.md"));

        backend.toggleFocusMode();
        QVERIFY(backend.focusMode());
        QCOMPARE(focusSpy.count(), 1);
        QCOMPARE(backend.status(), QStringLiteral("Saved focus-test.md"));

        backend.toggleFocusMode();
        QVERIFY(!backend.focusMode());
        QCOMPARE(focusSpy.count(), 2);
        QCOMPARE(backend.status(), QStringLiteral("Saved focus-test.md"));
    }

    void focusModeDimsInactiveBlocks() {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("First paragraph\n\nSecond paragraph"));
        MarkdownHighlighter highlighter(&doc);
        highlighter.setColors(QStringLiteral("#101010"), QStringLiteral("#eeeeee"),
                              QStringLiteral("#5584aa"));

        // Enable focus mode with cursor in the first block
        highlighter.setFocusCursorPosition(0);
        highlighter.setFocusMode(true);

        // First block (active) should keep its original undimmed foreground
        QTextBlock firstBlock = doc.findBlockByNumber(0);
        bool firstIsDimmed = !firstBlock.layout()->formats().isEmpty()
            && firstBlock.layout()->formats().first().format.foreground().color() != QColor(QStringLiteral("#eeeeee"));
        QVERIFY(!firstIsDimmed);

        // Third block (inactive, "Second paragraph") should be dimmed
        QTextBlock thirdBlock = doc.findBlockByNumber(2);
        QVERIFY(!thirdBlock.layout()->formats().isEmpty());
        QColor dimmedColor = thirdBlock.layout()->formats().first().format.foreground().color();
        QVERIFY(dimmedColor.isValid());
        QVERIFY(dimmedColor != QColor(QStringLiteral("#eeeeee")));
        QVERIFY(dimmedColor != QColor(QStringLiteral("#101010")));

        // Move cursor to third block — first should dim, third should un-dim
        highlighter.setFocusCursorPosition(thirdBlock.position());
        QTextBlock updatedFirst = doc.findBlockByNumber(0);
        QVERIFY(!updatedFirst.layout()->formats().isEmpty());
        QColor nowDimmed = updatedFirst.layout()->formats().first().format.foreground().color();
        QCOMPARE(nowDimmed, dimmedColor);

        QTextBlock updatedThird = doc.findBlockByNumber(2);
        bool thirdIsDimmed = !updatedThird.layout()->formats().isEmpty()
            && updatedThird.layout()->formats().first().format.foreground().color() == dimmedColor;
        QVERIFY(!thirdIsDimmed);
    }

    void focusModeKeepsInlineMarkersHidden() {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("active line\n\nsome **bold** here"));
        MarkdownHighlighter highlighter(&doc);
        highlighter.setColors(QStringLiteral("#101010"), QStringLiteral("#eeeeee"),
                              QStringLiteral("#5584aa"));

        highlighter.setFocusCursorPosition(0);
        highlighter.setFocusMode(true);

        const QTextBlock dimmed = doc.findBlockByNumber(2);
        QVERIFY(!dimmed.layout()->formats().isEmpty());
        QColor markerColor;
        QColor textColor;
        for (const QTextLayout::FormatRange &range : dimmed.layout()->formats()) {
            const QString run = dimmed.text().mid(range.start, range.length);
            if (run == QStringLiteral("**"))
                markerColor = range.format.foreground().color();
            else if (run == QStringLiteral("bold"))
                textColor = range.format.foreground().color();
        }

        QCOMPARE(markerColor, QColor(QStringLiteral("#101010")));
        QVERIFY(textColor.isValid());
        QVERIFY(textColor != markerColor);
    }

    void recentresOnlyWhenFocusModeMovesTheEditor() {
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
        QObject *viewport = editor->parent();
        while (viewport && !viewport->property("contentY").isValid())
            viewport = viewport->parent();
        QVERIFY(viewport);

        QString text;
        for (int line = 0; line < 600; ++line)
            text += QStringLiteral("line %1 of the document\n").arg(line);
        editor->setProperty("text", text);
        const int caretPosition = text.indexOf(QStringLiteral("line 150 "));
        QVERIFY(caretPosition > 0);
        editor->setProperty("cursorPosition", caretPosition);
        QTest::qWait(50);

        // Reading elsewhere, with the caret scrolled out of sight: resizing
        // the window has to leave the page where the reader put it.
        const qreal readingAt = editor->property("y").toReal()
            + editor->property("cursorRectangle").toRectF().y()
            + viewport->property("height").toReal() + 200;
        viewport->setProperty("contentY", readingAt);
        QCOMPARE(viewport->property("contentY").toReal(), readingAt);
        const qreal viewportBefore = viewport->property("height").toReal();
        window->setProperty("height", window->property("height").toReal() + 80);
        QTest::qWait(50);
        // A resize the layout never receives leaves the page in place for the
        // wrong reason, and everything below it would then pass on anything.
        QVERIFY2(qAbs(viewport->property("height").toReal() - (viewportBefore + 80)) <= 2,
                 qPrintable(QStringLiteral("the resize never reached the editor: viewport %1, "
                                           "expected %2. Run this through bin/test, which "
                                           "forces QT_QPA_PLATFORM=offscreen.")
                                .arg(viewport->property("height").toReal())
                                .arg(viewportBefore + 80)));
        const qreal stillReadingAt = viewport->property("contentY").toReal();
        QVERIFY2(qAbs(stillReadingAt - readingAt) <= 2,
                 qPrintable(QStringLiteral("the page moved from %1 to %2")
                                .arg(readingAt)
                                .arg(stillReadingAt)));

        // Entering focus mode does have to move it: the caret line goes to
        // the middle of the viewport straight away, not at the next keystroke.
        backend.updateCursorPosition(caretPosition);
        backend.toggleFocusMode();
        QTest::qWait(50);
        const QRectF caret = editor->property("cursorRectangle").toRectF();
        const qreal viewportHeight = viewport->property("height").toReal();
        const qreal caretCentre = editor->property("y").toReal() + caret.y()
            + caret.height() / 2 - viewport->property("contentY").toReal();
        QVERIFY2(qAbs(caretCentre - viewportHeight / 2) <= 2,
                 qPrintable(QStringLiteral("caret centre %1 in a viewport of %2")
                                .arg(caretCentre)
                                .arg(viewportHeight)));
    }

private:
    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"

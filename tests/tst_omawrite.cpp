#include <QtTest>
#include <QClipboard>
#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QMimeData>
#include <QTextDocument>
#include <QTextLayout>
#include <QQmlComponent>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>

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

    void sanitizesAndSeparatesRenderedMarkdown() {
        Backend backend;
        const QString markdown = QStringLiteral(
            "<script>alert('no')</script>\n\n"
            "![tracker](https://tracker.invalid/pixel.png)\n\n"
            "```cpp\nint first = 1;\n```\n\n"
            "```cpp\nint second = 2;\n```\n");
        const QString html = backend.renderMarkdown(
            markdown, QStringLiteral("#ffffff"), QStringLiteral("#222324"), 20);

        QVERIFY(!html.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
        QVERIFY(!html.contains(QStringLiteral("tracker.invalid"), Qt::CaseInsensitive));

        QTextDocument document;
        document.setHtml(html);
        QTextBlock first = document.begin();
        while (first.isValid() && !first.text().contains(QStringLiteral("int first")))
            first = first.next();
        QTextBlock second = first.next();
        while (second.isValid() && !second.text().contains(QStringLiteral("int second")))
            second = second.next();

        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        QVERIFY(first.blockFormat().background().style() != Qt::NoBrush);
        QVERIFY(second.blockFormat().background().style() != Qt::NoBrush);
        QVERIFY(!first.blockFormat().nonBreakableLines());
        QCOMPARE(first.blockFormat().background().color(),
                 second.blockFormat().background().color());
        QVERIFY(first.blockFormat().background().color() != QColor(QStringLiteral("#ffffff")));
    }

    void spacesRenderedMarkdownForReading() {
        Backend backend;
        const QString html = backend.renderMarkdown(
            QStringLiteral("# Title\n\n## Section\n\nFirst paragraph.\n\n"
                           "Second paragraph.\n\n- First item\n- Second item\n"),
            QStringLiteral("#ffffff"), QStringLiteral("#222324"), 20);

        QTextDocument document;
        document.setHtml(html);
        QTextBlock title = document.begin();
        QTextBlock section = title.next();
        QTextBlock firstParagraph = section.next();
        QTextBlock secondParagraph = firstParagraph.next();
        QTextBlock firstItem = secondParagraph.next();
        QTextBlock secondItem = firstItem.next();

        QCOMPARE(title.blockFormat().headingLevel(), 1);
        QCOMPARE(section.blockFormat().headingLevel(), 2);
        QCOMPARE(firstParagraph.blockFormat().lineHeightType(),
                 int(QTextBlockFormat::ProportionalHeight));
        QCOMPARE(firstParagraph.blockFormat().lineHeight(), qreal(140));
        QVERIFY(title.blockFormat().bottomMargin() > 0);
        QVERIFY(section.blockFormat().topMargin()
                > section.blockFormat().bottomMargin());
        QVERIFY(firstParagraph.blockFormat().bottomMargin() > 0);
        QVERIFY(secondParagraph.blockFormat().bottomMargin() > 0);
        QCOMPARE(firstItem.blockFormat().bottomMargin(), qreal(0));
        QVERIFY(secondItem.blockFormat().bottomMargin() > 0);
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

    void leavesFencedCodeLiteral() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("prose _italic_\n"
                                            "```ruby\n"
                                            "snake_case_name = *value*\n"
                                            "# not a heading\n"
                                            "```\n"
                                            "after _italic_\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.rehighlight();

        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        QCOMPARE(stateOf(0), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(1), int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(2), int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(3), int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(4), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(5), int(MarkdownHighlighter::Prose));

        const auto formatsOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).layout()->formats();
        };

        const QList<QTextLayout::FormatRange> fenced = formatsOf(2);
        QCOMPARE(fenced.size(), 1);
        QCOMPARE(fenced.constFirst().length, document.findBlockByNumber(2).text().length());
        QVERIFY(fenced.constFirst().format.background().style() != Qt::NoBrush);
        for (const QTextLayout::FormatRange &range : fenced) {
            QVERIFY(!range.format.fontItalic());
            QVERIFY(range.format.fontWeight() != QFont::Bold);
            QVERIFY(range.format.foreground().color() != range.format.background().color());
        }

        const QList<QTextLayout::FormatRange> comment = formatsOf(3);
        QCOMPARE(comment.size(), 1);
        QVERIFY(comment.constFirst().format.background().style() != Qt::NoBrush);
        QVERIFY(comment.constFirst().format.fontWeight() != QFont::Bold);

        const QList<QTextLayout::FormatRange> fence = formatsOf(1);
        QCOMPARE(fence.size(), 1);
        QVERIFY(fence.constFirst().format.background().style() != Qt::NoBrush);

        const QList<QTextLayout::FormatRange> prose = formatsOf(5);
        QVERIFY(std::any_of(prose.cbegin(), prose.cend(),
                            [](const QTextLayout::FormatRange &range) {
                                return range.format.fontItalic();
                            }));
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
        QVERIFY(window->findChild<QObject *>(QStringLiteral("renderedPreview")));
        QVERIFY(window->findChild<QObject *>(QStringLiteral("modeToggle")));

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

    void switchesToCopyableReaderAndScalesDocumentText() {
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
        QObject *reader = window->findChild<QObject *>(QStringLiteral("renderedPreview"));
        QObject *modeToggle = window->findChild<QObject *>(QStringLiteral("modeToggle"));
        QVERIFY(editor);
        QVERIFY(reader);
        QVERIFY(modeToggle);

        const QString markdown = QStringLiteral(
            "# Reader heading\n\nA **formatted** paragraph.\n\n```cpp\nint answer = 42;\n```\n");
        editor->setProperty("text", markdown);

        QVERIFY(editor->property("visible").toBool());
        QVERIFY(!reader->property("visible").toBool());
        QVERIFY(window->property("readerHtml").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(modeToggle, "clicked"));
        QVERIFY(window->property("readerMode").toBool());
        QVERIFY(!editor->property("visible").toBool());
        QVERIFY(reader->property("visible").toBool());
        QVERIFY(reader->property("readOnly").toBool());
        QVERIFY(reader->property("selectByMouse").toBool());
        QVERIFY(reader->property("persistentSelection").toBool());
        QVERIFY(reader->property("textFormat").toInt() != 0); // Not PlainText.

        QVERIFY(QMetaObject::invokeMethod(reader, "selectAll"));
        QVERIFY(QMetaObject::invokeMethod(reader, "copy"));
        const QMimeData *copied = QGuiApplication::clipboard()->mimeData();
        QVERIFY(copied);
        QVERIFY(copied->hasText());
        QVERIFY(copied->hasHtml());
        QVERIFY(copied->text().contains(QStringLiteral("Reader heading")));

        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            reader->property("textDocument").value<QObject *>());
        QVERIFY(quickDocument);
        QTextBlock codeBlock = quickDocument->textDocument()->findBlockByNumber(2);
        while (codeBlock.isValid() && !codeBlock.text().contains(QStringLiteral("int answer")))
            codeBlock = codeBlock.next();
        QVERIFY(codeBlock.isValid());
        QVERIFY(codeBlock.blockFormat().background().style() != Qt::NoBrush);
        QCOMPARE(codeBlock.blockFormat().background().color().toRgb().rgba(),
                 MarkdownHighlighter::codeBackgroundColor(
                     backend.themeBackground(), backend.themeForeground()).toRgb().rgba());
        QTextBlock proseBlock = quickDocument->textDocument()->begin();
        while (proseBlock.isValid()
                && !proseBlock.text().contains(QStringLiteral("formatted paragraph")))
            proseBlock = proseBlock.next();
        QVERIFY(proseBlock.isValid());
        QVERIFY(proseBlock.blockFormat().background().style() == Qt::NoBrush);
        QVERIFY(copied->html().contains(QStringLiteral("int answer = 42")));

        auto *quickWindow = qobject_cast<QQuickWindow *>(window.data());
        QVERIFY(quickWindow);

        const int defaultSize = editor->property("font").value<QFont>().pixelSize();
        QTest::keyClick(quickWindow, Qt::Key_Plus, Qt::ControlModifier);
        QVERIFY(editor->property("font").value<QFont>().pixelSize() > defaultSize);
        QCOMPARE(reader->property("font").value<QFont>().pixelSize(),
                 editor->property("font").value<QFont>().pixelSize());
        QTest::keyClick(quickWindow, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), defaultSize);

        QTest::keyClick(quickWindow, Qt::Key_R,
                        Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(!window->property("readerMode").toBool());

        for (int i = 0; i < 12; ++i)
            QTest::keyClick(quickWindow, Qt::Key_Minus, Qt::ControlModifier);
        QVERIFY(editor->property("font").value<QFont>().pixelSize() > 0);
        for (int i = 0; i < 9; ++i)
            QTest::keyClick(quickWindow, Qt::Key_Plus, Qt::ControlModifier);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), defaultSize);

        // Exercise the Reader shortcut in the entry direction too.
        QTest::keyClick(quickWindow, Qt::Key_R,
                        Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(window->property("readerMode").toBool());
        QVERIFY(QMetaObject::invokeMethod(window.data(), "openSearch",
                                          Q_ARG(QVariant, false)));
        QVERIFY(!window->property("readerMode").toBool());
        QVERIFY(window->property("searchOpen").toBool());
    }

    void readerKeyboardNavigationFollowsCursor() {
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
        QObject *reader = window->findChild<QObject *>(QStringLiteral("renderedPreview"));
        QObject *modeToggle = window->findChild<QObject *>(QStringLiteral("modeToggle"));
        QObject *flick = window->findChild<QObject *>(QStringLiteral("documentFlick"));
        QVERIFY(editor);
        QVERIFY(reader);
        QVERIFY(modeToggle);
        QVERIFY(flick);

        QStringList lines;
        for (int i = 0; i < 100; ++i)
            lines.append(QStringLiteral("Reader line %1").arg(i));
        editor->setProperty("text", lines.join(QStringLiteral("\n\n")));
        QVERIFY(QMetaObject::invokeMethod(modeToggle, "clicked"));
        auto *quickWindow = qobject_cast<QQuickWindow *>(window.data());
        QVERIFY(quickWindow);
        QTRY_VERIFY(reader->property("activeFocus").toBool());
        QTest::keyClick(quickWindow, Qt::Key_End, Qt::ControlModifier);
        QTRY_VERIFY(flick->property("contentY").toReal() > 0);
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
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"

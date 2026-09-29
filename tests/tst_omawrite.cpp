#include <QtTest>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>

#include <algorithm>

#include "backend.h"
#include "markdownhighlighter.h"
#include "mathoverlay.h"
#include "mathrenderer.h"

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_settingsDirectory.isValid());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        qmlRegisterType<MathOverlay>("Omawrite", 1, 0, "MathOverlay");
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

    void findsMath() {
        const auto spans = [](const char *text) {
            return MarkdownHighlighter::mathSpans(QString::fromUtf8(text));
        };

        auto found = spans("a $x^2$ b");
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).start, 2);
        QCOMPARE(found.at(0).end, 7);
        QCOMPARE(found.at(0).tex, QStringLiteral("x^2"));
        QVERIFY(!found.at(0).display);

        QVERIFY(spans("costs $5 and $10 each").isEmpty());
        QVERIFY(spans("$ x$ and $x $").isEmpty());
        QVERIFY(spans("a \\$x\\$ b").isEmpty());
        QVERIFY(spans("costs $5, see `$x$`").isEmpty());
        QVERIFY(spans("```\n$x$\n```").isEmpty());

        found = spans("then \\(a+b\\) done");
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).tex, QStringLiteral("a+b"));
        QCOMPARE(found.at(0).delimiter, 2);
        QVERIFY(!found.at(0).display);

        found = spans("  $$\\int f$$ ");
        QCOMPARE(found.size(), 1);
        QVERIFY(found.at(0).display);
        QVERIFY(found.at(0).standalone);
        QCOMPARE(found.at(0).tex, QStringLiteral("\\int f"));
        QVERIFY(!spans("text $$x$$ more").at(0).standalone);

        found = spans("para\n\n$$\n\\begin{aligned}a&=b\\end{aligned}\n$$\nafter $y$");
        QCOMPARE(found.size(), 2);
        QVERIFY(found.at(0).standalone);
        QCOMPARE(found.at(0).tex, QStringLiteral("\n\\begin{aligned}a&=b\\end{aligned}\n"));
        QCOMPARE(found.at(1).tex, QStringLiteral("y"));

        found = spans("\\[\nx\n\\]");
        QCOMPARE(found.size(), 1);
        QVERIFY(found.at(0).standalone);
        QCOMPARE(spans("\\[x\\]").size(), 1);
        // Markdown prose escapes brackets the same way.
        QVERIFY(spans("see \\[1\\] and \\[sic\\]").isEmpty());

        // An unclosed $$ ends at the blank line instead of swallowing the rest.
        found = spans("$$\nx\n\ny $z$");
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).tex, QStringLiteral("z"));
    }

    void masksMathFromInlineMarkup() {
        const QString text = QStringLiteral("$x_1 + y_2$ and _it_");
        const auto math = MarkdownHighlighter::mathSpans(text);
        QCOMPARE(math.size(), 1);
        const auto markup = MarkdownHighlighter::inlineMarkup(
            text, {{math.at(0).start, math.at(0).end - math.at(0).start}});
        QCOMPARE(markup.size(), 1);
        QCOMPARE(markup.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(markup.at(0).content.start, int(text.indexOf(QStringLiteral("it"))));
    }

    void rendersMathWithMathJax() {
        MathRenderer renderer;
        QSignalSpy renderedSpy(&renderer, &MathRenderer::rendered);
        QVERIFY(!renderer.result(QStringLiteral("\\frac{a}{b}"), false));
        QVERIFY(!renderer.result(QStringLiteral("\\frac{a"), false));
        QTRY_COMPARE_WITH_TIMEOUT(renderedSpy.count(), 2, 20000);

        const auto rendered = renderer.result(QStringLiteral("\\frac{a}{b}"), false);
        QVERIFY(rendered && rendered->ok);
        QVERIFY(rendered->svg.startsWith("<svg"));
        QVERIFY(rendered->width > 0);
        QVERIFY(rendered->depth > 0);
        QVERIFY(rendered->height > rendered->depth);

        const auto failed = renderer.result(QStringLiteral("\\frac{a"), false);
        QVERIFY(failed && !failed->ok);
        QVERIFY(!failed->error.isEmpty());
    }

    void showsMathSourceUnderTheCaret() {
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

        const QString text = QStringLiteral("Euler: $e^{i\\pi}+1=0$ and $a_1 + b_2$ _it_\n\n"
                                            "$$\nx\n$$\n");
        editor->setProperty("text", text);
        editor->setProperty("cursorPosition", 0);
        QTRY_COMPARE_WITH_TIMEOUT(backend.mathPlacements().size(), 3, 20000);
        QCOMPARE(backend.mathPlacements().at(0).position, 7);

        // Underscores in math are not italic markers for the caret to skip.
        const QVariantList hidden = backend.hiddenRangesAt(0);
        QCOMPARE(hidden.size(), 2);
        QCOMPARE(hidden.at(0).toMap().value(QStringLiteral("start")).toInt(),
                 int(text.indexOf(QStringLiteral("_it_"))));

        // The caret inside a formula shows its source; leaving renders it again.
        editor->setProperty("cursorPosition", 12);
        QTRY_COMPARE(backend.mathPlacements().size(), 2);
        editor->setProperty("cursorPosition", 0);
        QTRY_COMPARE(backend.mathPlacements().size(), 3);

        // Editing a later line of display math re-typesets it from its first line.
        const QByteArray before = backend.mathPlacements().at(2).svg;
        QVERIFY(QMetaObject::invokeMethod(editor, "insert",
                                          Q_ARG(int, int(text.indexOf(QStringLiteral("x\n$$")))),
                                          Q_ARG(QString, QStringLiteral("y+"))));
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(backend.mathPlacements().size() == 3
                                 && backend.mathPlacements().at(2).svg != before, 20000);

        // An opening code fence above turns everything after it into code.
        QVERIFY(QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 0), Q_ARG(QString, QStringLiteral("```\n"))));
        QTRY_VERIFY(backend.mathPlacements().isEmpty());
    }

    void opensMathSourceOnClick() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        auto *overlay = window->findChild<QQuickItem *>(QStringLiteral("mathOverlay"));
        QVERIFY(editor);
        QVERIFY(overlay);

        // Inline math, a display line with a trailing space, and a display block.
        const QString text = QStringLiteral("Inline $x^2$ here\n\n$$\\int f$$ \n\n"
                                            "$$\nx+1\n$$\n\nend");
        editor->setProperty("text", text);
        const int away = int(text.size());
        editor->setProperty("cursorPosition", away);
        QTRY_COMPARE_WITH_TIMEOUT(backend.mathPlacements().size(), 3, 20000);

        const auto isRendered = [&backend](int position) {
            const auto placements = backend.mathPlacements();
            return std::any_of(placements.cbegin(), placements.cend(),
                               [position](const auto &placement) {
                                   return placement.position == position;
                               });
        };
        for (int formula = 0; formula < 3; ++formula) {
            const auto placement = backend.mathPlacements().at(formula);
            const QPointF click = overlay->mapToScene(placement.image.center());
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, click.toPoint());
            QTRY_VERIFY2(!isRendered(placement.position),
                         qPrintable(QStringLiteral("formula %1 did not open; caret at %2")
                                        .arg(formula)
                                        .arg(editor->property("cursorPosition").toInt())));

            editor->setProperty("cursorPosition", away);
            QTRY_COMPARE(backend.mathPlacements().size(), 3);
        }
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

private:
    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"

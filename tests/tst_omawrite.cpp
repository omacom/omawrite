#include <QtTest>
#include <QFont>
#include <QColor>
#include <QQuickItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextBlock>
#include <QWheelEvent>
#include <QPainter>
#include <QAbstractTextDocumentLayout>
#include <QSet>
#include <QTextFragment>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPrinter>
#include <QTextLayout>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QWindow>
#include <QSignalSpy>

#include "backend.h"
#include "markdownhighlighter.h"
#include "systemtheme.h"

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_settingsDirectory.isValid());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDirectory.path());
    }

    void init() {
        Backend::allowSessionWrites();
        QSettings().clear();
        const QString stateDirectory =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QFile::remove(Backend::sessionPath());
        QDir dir(stateDirectory);
        const QStringList leftovers = dir.entryList(
            {QStringLiteral("recovery-*.json"), QStringLiteral("session.json")},
            QDir::Files);
        for (const QString &name : leftovers)
            QFile::remove(dir.filePath(name));
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

    void detectsSystemAppearanceWithoutRequiringAPortal() {
        SystemTheme theme;
        QVERIFY(theme.textScale() > 0.0);
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

    void leavesInlineCodeLiteral() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("see `default_line_height` and `file_name`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("a_b `c_d`")).isEmpty());
        // Word-boundary underscores inside a code span are still literal.
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("`_private_`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("`a * b * c`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("`[not](a link)`")).isEmpty());

        // Emphasis that merely wraps a code span still applies.
        const auto wrapping = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**`file_name` only**"));
        QCOMPARE(wrapping.size(), 1);
        QCOMPARE(wrapping.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(wrapping.at(0).content.start, 2);
        QCOMPARE(wrapping.at(0).content.length, 16);
        QCOMPARE(wrapping.at(0).markers[0].length, 2);
        QCOMPARE(wrapping.at(0).markers[1].start, 18);
    }

    void keepsIntrawordUnderscoresLiteral() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("snake_case_name and default_line_height")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("a__b__c")).isEmpty());

        // Emphasis still opens at a word boundary.
        const auto underscoreBold =
            MarkdownHighlighter::inlineMarkup(QStringLiteral("say __bold__ here"));
        QCOMPARE(underscoreBold.size(), 1);
        QCOMPARE(underscoreBold.at(0).content.start, 6);
        QCOMPARE(underscoreBold.at(0).content.length, 4);

        const auto emphasis = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("_italic_, **bold** and mid*word*"));
        QCOMPARE(emphasis.size(), 3);
        QCOMPARE(emphasis.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(emphasis.at(0).content.length, 4);
        QCOMPARE(emphasis.at(1).content.start, 1);
        QCOMPARE(emphasis.at(1).content.length, 6);
        QCOMPARE(emphasis.at(2).content.length, 4);
    }

    void leavesFencedCodeLiteral() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("prose _italic_\n"
                                            "```ruby\n"
                                            "snake_case_name = *value*\n"
                                            "```\n"
                                            "after _italic_\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.rehighlight();

        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        // The state also carries the fence's language, so ask rather than
        // compare -- "```ruby" does not leave the flag standing alone.
        QVERIFY(!MarkdownHighlighter::isInsideFence(stateOf(0)));
        QVERIFY(MarkdownHighlighter::isInsideFence(stateOf(1)));
        QVERIFY(MarkdownHighlighter::isInsideFence(stateOf(2)));
        QVERIFY(!MarkdownHighlighter::isInsideFence(stateOf(3)));
        QVERIFY(!MarkdownHighlighter::isInsideFence(stateOf(4)));

        // The fenced line sits on the code panel, with nothing italic and no
        // marker hidden.
        const QList<QTextLayout::FormatRange> fenced =
            document.findBlockByNumber(2).layout()->formats();
        QCOMPARE(fenced.size(), 1);
        QCOMPARE(fenced.constFirst().length, document.findBlockByNumber(2).text().length());
        // The panel behind a fence is drawn in QML, so the run carries no
        // character background of its own -- that was the stripe.
        QCOMPARE(fenced.constFirst().format.background().style(), Qt::NoBrush);
        for (const QTextLayout::FormatRange &range : fenced) {
            QVERIFY(!range.format.fontItalic());
            QVERIFY(range.format.foreground().color() != range.format.background().color());
        }

        const QList<QTextLayout::FormatRange> prose =
            document.findBlockByNumber(4).layout()->formats();
        QVERIFY(std::any_of(prose.cbegin(), prose.cend(),
                            [](const QTextLayout::FormatRange &range) {
                                return range.format.fontItalic();
                            }));
    }

    void takesTheCodePanelFromTheTheme() {
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

        const auto codePanelFor = [&themeDirectory](const QByteArray &palette) {
            QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
            if (!colorsFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                return QColor();
            colorsFile.write(palette);
            colorsFile.close();
            return QColor(Backend().themeCodeBackground());
        };

        // The shade the theme names for panels wins.
        QCOMPARE(codePanelFor("mode = \"dark\"\n"
                              "background = \"#111c18\"\n"
                              "foreground = \"#c1c497\"\n"
                              "lighter_background = \"#23372b\"\n"),
                 QColor(QStringLiteral("#23372b")));

        // Themes naming none leave code on a shade mixed from the page: a
        // little off the background, and nowhere near the text.
        const QColor mixedDark = codePanelFor("mode = \"dark\"\n"
                                              "background = \"#000000\"\n"
                                              "foreground = \"#ffffff\"\n");
        QVERIFY(mixedDark != QColor(Qt::black));
        QVERIFY(mixedDark.lightness() < 60);

        const QColor mixedLight = codePanelFor("mode = \"light\"\n"
                                               "background = \"#ffffff\"\n"
                                               "foreground = \"#000000\"\n");
        QVERIFY(mixedLight != QColor(Qt::white));
        QVERIFY(mixedLight.lightness() > 195);

        // A theme whose lighter background is the page itself would leave code
        // with no panel at all, so it falls back to the mix as well.
        QVERIFY(codePanelFor("mode = \"dark\"\n"
                             "background = \"#0c0b0c\"\n"
                             "foreground = \"#fafcfb\"\n"
                             "lighter_background = \"#0c0b0c\"\n")
                != QColor(QStringLiteral("#0c0b0c")));
    }

    void loadsCurrentOmarchyTheme() {
        ScopedTheme theme(
            "mode = \"light\"\n"
            "accent = \"#112233\"\n"
            "selection = \"#445566\"\n"
            "background = \"#fefefe\"\n"
            "foreground = \"#101010\"\n");
        QVERIFY(theme.ok);

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#fefefe"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#101010"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QVERIFY(!backend.darkMode());
    }

    void readsTomlValuesPastInlineComments() {
        // Every colour in every theme is a quoted "#rrggbb": the hash inside the quotes stays.
        QCOMPARE(Backend::tomlValue(QStringLiteral(" \"#1a1b26\" ")), QStringLiteral("#1a1b26"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"#EDE6D6\"   # unbleached cloth")),
                 QStringLiteral("#EDE6D6"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("'#445566'# no space before the hash")),
                 QStringLiteral("#445566"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"rebecca#purple\"")),
                 QStringLiteral("rebecca#purple"));
        QCOMPARE(Backend::tomlValue(QStringLiteral(" #1a1b26 ")), QStringLiteral("#1a1b26"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"#1a1b26")), QStringLiteral("\"#1a1b26"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"\"")), QString());
        QCOMPARE(Backend::tomlValue(QString()), QString());
    }

    void keepsDefaultsForColorsItCannotParse() {
        ScopedTheme theme(
            "mode = \"dark\"          # warmed ink\r\n"
            "accent = \"#112233\"     # shell-white pigment\r\n"
            "selection = '#445566'\r\n"
            "background = \"#1a1b17\" # ink, warmed\r\n"
            "foreground = \"unbleached cloth\"\r\n");
        QVERIFY(theme.ok);

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#1a1b17"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#eeeeee"));
        QVERIFY(backend.darkMode());
    }

    void takesItsDefaultsFromTheModeTheThemeAsksFor() {
        ScopedTheme theme(
            "mode = \"light\"\n"
            "background = \"#ffffff\"\n"
            "foreground = \"unbleached cloth\"\n");
        QVERIFY(theme.ok);

        Backend backend;
        QVERIFY(!backend.darkMode());
        QCOMPARE(backend.themeForeground(), QStringLiteral("#222324"));
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

    void startsANewFileFromAPathThatIsNotThereYet() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString newPath = directory.filePath(QStringLiteral("new_document.md"));
        const QString existingPath = directory.filePath(QStringLiteral("already-there.md"));
        QFile existing(existingPath);
        QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Text));
        existing.write("on disk already");
        existing.close();

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

        // A name from the command line that is not on disk yet is still this
        // document's name, blank as the document is.
        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        backend.open(QUrl::fromLocalFile(newPath));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(newPath));
        QCOMPARE(backend.fileName(), QStringLiteral("new_document.md"));
        QCOMPARE(backend.status(), QStringLiteral("New file new_document.md"));
        QCOMPARE(editor->property("text").toString(), QString());
        QVERIFY(!backend.modified());

        // Opening it wrote nothing: the file appears when the writer saves.
        QVERIFY(!QFileInfo::exists(newPath));

        editor->setProperty("text", QStringLiteral("first words"));
        QVERIFY(backend.modified());
        backend.save();
        QCOMPARE(saveDialogSpy.count(), 0);
        QVERIFY(!backend.modified());

        QFile written(newPath);
        QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(written.readAll(), QByteArray("first words"));
        written.close();

        // A file that is there still opens and reads.
        backend.open(QUrl::fromLocalFile(existingPath));
        QCOMPARE(backend.fileName(), QStringLiteral("already-there.md"));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("on disk already"));

        // A path that is there but cannot be read is still an error, and
        // leaves the document it could not replace alone.
        backend.open(QUrl::fromLocalFile(directory.path()));
        QCOMPARE(backend.status(),
                 QStringLiteral("Could not open %1.")
                     .arg(QFileInfo(directory.path()).fileName()));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(existingPath));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("on disk already"));

        // A name under a directory that is not there is not a file anyone can
        // start, so it stays an error rather than a document that cannot save.
        backend.open(QUrl::fromLocalFile(
            directory.filePath(QStringLiteral("not-there/child.md"))));
        QCOMPARE(backend.status(), QStringLiteral("Could not open child.md."));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(existingPath));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("on disk already"));
    }

    void asksBeforeAFirstSaveReplacesAFileThatAppeared() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("arriving.md"));

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

        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(backend.status(), QStringLiteral("New file arriving.md"));
        editor->setProperty("text", QStringLiteral("my draft"));
        QVERIFY(backend.modified());

        // A file that is not there cannot be watched, so nothing tells us when
        // a `git pull` or a sync client puts one on that path. The first save
        // is the first look, and it must not replace a file it has never read.
        QFile arrived(path);
        QVERIFY(arrived.open(QIODevice::WriteOnly | QIODevice::Text));
        arrived.write("arrived from elsewhere");
        arrived.close();

        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
        QCOMPARE(appearedSpy.takeFirst().constFirst().toBool(), true);

        // Asked, not answered: the file on disk is whole and the draft is
        // still unsaved. The name is not in question, so no Save As dialog.
        QCOMPARE(saveDialogSpy.count(), 0);
        QVERIFY(backend.modified());
        QFile untouched(path);
        QVERIFY(untouched.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(untouched.readAll(), QByteArray("arrived from elsewhere"));
        untouched.close();

        // Keeping your version is what the dialog offers, and the save that
        // follows it goes through: the guard asks once, it does not lock the
        // writer out of the name they gave.
        backend.keepExternalVersion();
        backend.save();
        QVERIFY(!backend.modified());
        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(written.readAll(), QByteArray("my draft"));
        written.close();
    }

    void asksOnlyOnceWhenWhatAppearedCannotBeRead() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("blocked.md"));

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

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("my draft"));
        QVERIFY(backend.modified());

        // What turns up on the path need not be a readable file. A directory
        // is the plainest case: keepExternalVersion() cannot read it, so it
        // has no contents to remember afterwards.
        QVERIFY(QDir().mkpath(path));

        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);

        // Keeping your version answers the question, and an answer that could
        // not be read is still an answer. Asking again would put the writer in
        // a dialog with no way out of it, every Ctrl+S for the rest of the
        // session. The second save goes to the filesystem and reports what the
        // filesystem says, which is the only thing that can end this.
        backend.keepExternalVersion();
        QCOMPARE(backend.status(), QStringLiteral("Kept your version"));
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
        QCOMPARE(backend.status(), QStringLiteral("Could not save blocked.md."));
    }

    void dropsThePendingCloseWhenTheSaveIsRefused() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("closing.md"));

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

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("my draft"));
        QVERIFY(backend.modified());

        QFile arrived(path);
        QVERIFY(arrived.open(QIODevice::WriteOnly | QIODevice::Text));
        arrived.write("arrived from elsewhere");
        arrived.close();

        // Closing the window with unsaved changes leaves "close" standing
        // while the unsaved-changes dialog's Save runs. The guard turns that
        // save into a question, so the close it was for cannot follow.
        window->setProperty("pendingAction", QStringLiteral("close"));
        window->setProperty("awaitingPendingSave", true);
        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
        QCOMPARE(window->property("pendingAction").toString(), QString());
        QVERIFY(!window->property("awaitingPendingSave").toBool());

        // Otherwise the next successful save -- this one, minutes later and
        // asked for on its own -- closes the window on the earlier request.
        backend.keepExternalVersion();
        backend.save();
        QVERIFY(!backend.modified());
        QVERIFY(!window->property("closeConfirmed").toBool());
    }

    void putsKeepMineForwardWhenAFileAppeared() {
        const QString dialogPath = QFINDTESTDATA("../src/ExternalChangeDialog.qml");
        QVERIFY(!dialogPath.isEmpty());

        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(dialogPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> dialog(component.create());
        QVERIFY2(dialog, qPrintable(component.errorString()));

        QObject *keep = dialog->findChild<QObject *>(QStringLiteral("keepMineButton"));
        QObject *reload = dialog->findChild<QObject *>(QStringLiteral("reloadButton"));
        QObject *message = dialog->findChild<QObject *>(QStringLiteral("externalChangeMessage"));
        QObject *heading = dialog->findChild<QObject *>(QStringLiteral("externalChangeHeading"));
        QVERIFY(keep);
        QVERIFY(reload);
        QVERIFY(message);
        QVERIFY(heading);

        // For an ordinary outside edit the file on disk is a second copy of
        // the work, so Reload is the safe answer and leads, as it always has.
        QVERIFY(!dialog->property("keepIsSafer").toBool());
        QVERIFY(reload->property("primary").toBool());
        QVERIFY(!keep->property("primary").toBool());

        // For a file that appeared there is no second copy: every word the
        // writer has is in the editor, and reloading throws all of it away,
        // recovery snapshot included. The button that does that must not be
        // the one Enter presses, and the text must say what is at stake.
        dialog->setProperty("appeared", true);
        QVERIFY(dialog->property("keepIsSafer").toBool());
        QVERIFY(keep->property("primary").toBool());
        QVERIFY(!reload->property("primary").toBool());
        QCOMPARE(heading->property("text").toString(), QStringLiteral("File appeared"));
        const QString message_ = message->property("text").toString();
        QVERIFY2(message_.contains(QStringLiteral("created this file")), qPrintable(message_));
        QVERIFY2(message_.contains(QStringLiteral("discard everything")), qPrintable(message_));
    }

    void keepsTheDocumentWhenReloadRacesADeletion() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("racing.md"));
        QFile onDisk(path);
        QVERIFY(onDisk.open(QIODevice::WriteOnly | QIODevice::Text));
        onDisk.write("what was there");
        onDisk.close();

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

        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("what was there"));
        editor->setProperty("text", QStringLiteral("words only I have"));
        QVERIFY(backend.modified());

        // The "File changed" dialog leaves Reload enabled for a file that was
        // still there when it opened. If the file goes away before the click,
        // the reload has nothing to read: it must say so, not take the missing
        // path for a new document and blank the only copy of this text.
        QVERIFY(QFile::remove(path));
        backend.reloadFromDisk();
        QCOMPARE(backend.status(), QStringLiteral("Could not open racing.md."));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("words only I have"));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(path));
        QVERIFY(backend.modified());

        // Refusing the reload leaves this document where a new file starts:
        // a name, nothing behind it, and a watcher that let the path go when
        // the file did. So the same thing can happen again from here -- the
        // pull that removed the file landing the next commit -- and the save
        // has to ask about it just the same.
        QFile returned(path);
        QVERIFY(returned.open(QIODevice::WriteOnly | QIODevice::Text));
        returned.write("came back different");
        returned.close();

        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
        QFile intact(path);
        QVERIFY(intact.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(intact.readAll(), QByteArray("came back different"));
        intact.close();
    }

    void writesTheNeverReadPathIntoTheSnapshot() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("fresh.md"));

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

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("words only I have"));
        QVERIFY(backend.modified());

        // The snapshot the next run reads is the one this run wrote, so the
        // flag has to survive the write as well as the read. Hand-writing the
        // JSON proves only half of that, and it is the half that cannot lose
        // a file.
        const QString snapshotPath = Backend::sessionPath();
        QTRY_VERIFY(QFile::exists(snapshotPath));

        QFile snapshot(snapshotPath);
        QVERIFY(snapshot.open(QIODevice::ReadOnly));
        const QJsonObject recovery =
            QJsonDocument::fromJson(snapshot.readAll()).object()
                .value(QStringLiteral("windows")).toArray().at(0).toObject()
                .value(QStringLiteral("tabs")).toArray().at(0).toObject();
        snapshot.close();
        QVERIFY(recovery.contains(QStringLiteral("pathNeverRead")));
        QVERIFY(recovery.value(QStringLiteral("pathNeverRead")).toBool());
    }

    void remembersANeverReadPathAcrossRecovery() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("fresh.md"));

        // The snapshot a crash leaves behind, for a new file whose first save
        // never happened.
        const QString stateDirectory =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(stateDirectory));
        QFile::remove(Backend::sessionPath());
        QFile snapshot(QDir(stateDirectory).filePath(QStringLiteral("recovery-0.json")));
        QVERIFY(snapshot.open(QIODevice::WriteOnly));
        const QJsonObject recovery{
            {QStringLiteral("fileUrl"), QUrl::fromLocalFile(path).toString()},
            {QStringLiteral("pathNeverRead"), true},
            {QStringLiteral("text"), QStringLiteral("words only I have")}};
        snapshot.write(QJsonDocument(recovery).toJson(QJsonDocument::Compact));
        snapshot.close();

        // A file turns up on the path while Omawrite is not running to see it.
        QFile arrived(path);
        QVERIFY(arrived.open(QIODevice::WriteOnly | QIODevice::Text));
        arrived.write("arrived while we were down");
        arrived.close();

        // Reading it back on restore says what is on the path now, which is
        // not the same as this document having read it. Without the flag the
        // first save takes the guard's silence for permission.
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QCOMPARE(backend.status(), QStringLiteral("Recovered unsaved changes"));
        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
        QFile untouched(path);
        QVERIFY(untouched.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(untouched.readAll(), QByteArray("arrived while we were down"));
        untouched.close();
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

    void breaksLinesOnReturn() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto load = [&](const QString &content, int position) {
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };
        auto returnKey = [&] { QTest::keyClick(window, Qt::Key_Return); };

        // Upstream's behaviour, which is opt-in here.
        backend.setParagraphOnReturn(true);

        // Ending a paragraph leaves the blank line that separates it from the
        // next one.
        load(QStringLiteral("one\n\ntwo"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n\n"));

        // On a line that is already blank there is nothing to separate from,
        // so Return is worth one line, not two.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));

        // An empty document is a blank line too.
        load(QString(), 0);
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n"));

        // Whitespace left behind on a line still reads as blank.
        load(QStringLiteral("one\n  \ntwo"), 5);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n \n \ntwo"));

        // A selection dragged right to left leaves the caret on the blank line
        // it began on, but the break lands on the line it leaves behind.
        load(QStringLiteral("one\n\ntwo"), 7);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 4));
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\no"));

        // A list carries its marker down, and an item left empty drops the
        // marker to end the list.
        load(QStringLiteral("- item"), 6);
        returnKey();
        QCOMPARE(text(), QStringLiteral("- item\n- "));
        returnKey();
        QCOMPARE(text(), QStringLiteral("- item\n\n"));

        // Inside a code fence every line is its own, blank ones included.
        load(QStringLiteral("```\ncode\n```"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("```\ncode\n\n```"));
        returnKey();
        QCOMPARE(text(), QStringLiteral("```\ncode\n\n\n```"));
    }

    void closesParagraphBreaksOnBackspace() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto caret = [&] { return editor->property("cursorPosition").toInt(); };
        auto load = [&](const QString &content, int position) {
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };
        auto returnKey = [&] { QTest::keyClick(window, Qt::Key_Return); };
        auto backspaceKey = [&] { QTest::keyClick(window, Qt::Key_Backspace); };

        // This covers the Backspace that closes a paragraph break, so it needs
        // Return to open one -- which is opt-in here.
        backend.setParagraphOnReturn(true);

        // Backspace at the head of a paragraph closes the whole break above
        // it, so the two paragraphs join in one press rather than two, and
        // the caret lands where they meet.
        load(QStringLiteral("one\n\ntwo"), 5);
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("onetwo"));
        QCOMPARE(caret(), 3);

        // Ending a paragraph writes both breaks at once, and one Backspace
        // takes both back. A Return on the blank line of "one\n\n\ntwo" can
        // leave this very document with the caret in this very place, so it
        // is not provenance that decides here: it is that the gap is left
        // standing either way, and this is much the commoner press.
        load(QStringLiteral("one\n\ntwo"), 3);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));
        QCOMPARE(caret(), 3);

        // Which is what a Return inside a wide gap costs: the same rule reads
        // this as the paragraph-ending press it cannot be told apart from, so
        // the writer gets back one blank line fewer than they had. The gap
        // still separates the paragraphs, and one more Return returns it.
        load(QStringLiteral("one\n\n\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        QCOMPARE(caret(), 3);

        // Return on a blank line writes a single break, so Backspace must
        // take back a single break too. Closing the pair behind the caret
        // would swallow the separator that was there beforehand and leave
        // the two paragraphs with nothing between them.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // The same holds however far the gap has grown: each Return is worth
        // one Backspace.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));
        backspaceKey();
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // A line with nothing but spaces on it is a blank line, and Return
        // above one writes a single break there too.
        load(QStringLiteral("one\n  \ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n  \ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n  \ntwo"));

        // On the blank line a document ends with, the two Returns leave the
        // same text behind and no reading of it says which was pressed.
        // Backspace takes one break, because taking two would carry off the
        // line the document already ended with.
        load(QStringLiteral("one\n"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n"));

        // Which costs the paragraph that ends a document a second press,
        // and that is the whole of what it costs: nothing is lost on the
        // way through.
        load(QStringLiteral("one\n\ntwo"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // An empty document is a blank line, so it takes two Returns to open
        // a gap and two Backspaces to close it again.
        load(QString(), 0);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("\n"));
        backspaceKey();
        QCOMPARE(text(), QString());

        // A document holding nothing but spaces is a blank line as well, and
        // the spaces are text: they must still be there at the end of it.
        load(QStringLiteral("   "), 3);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("   \n\n"));
        backspaceKey();
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("   "));

        // Return at the very head of a document writes both breaks, since
        // the paragraph below it is not blank, and one Backspace takes them.
        load(QStringLiteral("two"), 0);
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("two"));
        QCOMPARE(caret(), 0);

        // Spaces left on the line above a break do not make the break any
        // less of one: Return between them and the paragraph below wrote
        // both of these, so Backspace still takes both.
        load(QStringLiteral("one\n  \n\ntwo"), 8);
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n  two"));
        QCOMPARE(caret(), 6);
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

    void dispatchesEditorFontSizeShortcuts() {
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
        window->requestActivate();
        QTRY_VERIFY(window->isActive());

        QCOMPARE(backend.editorFontSize(), 20);
        QTest::keyClick(window, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(backend.editorFontSize(), 22);
        QTest::keyClick(window, Qt::Key_Plus, Qt::ControlModifier);
        QCOMPARE(backend.editorFontSize(), 24);
        QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(backend.editorFontSize(), 22);
        QTest::keyClick(window, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(backend.editorFontSize(), 20);
        QCOMPARE(QSettings().value(QStringLiteral("editor/fontSize")).toInt(), 20);
    }

    void persistsEditorFontSizeAcrossBackendInstances() {
        {
            Backend backend;
            QCOMPARE(backend.editorFontSize(), 20);

            QSignalSpy changedSpy(&backend, &Backend::editorFontSizeChanged);
            backend.setEditorFontSize(28);
            QCOMPARE(changedSpy.count(), 1);
            QCOMPARE(QSettings().value(QStringLiteral("editor/fontSize")).toInt(), 28);
        }

        Backend restoredBackend;
        QCOMPARE(restoredBackend.editorFontSize(), 28);

        restoredBackend.setEditorFontSize(100);
        QCOMPARE(restoredBackend.editorFontSize(), 48);
        restoredBackend.setEditorFontSize(0);
        QCOMPARE(restoredBackend.editorFontSize(), 10);
    }

    // Issue #29: the editor sizes its font in pixels, and a HighResolution
    // QPrinter reads those as 1200-dpi dots, printing text about 0.4 mm tall.
    void convertsPixelFontsToPointsForPrinting() {
        QFont editorFont(QStringLiteral("iA Writer Mono S"));
        editorFont.setPixelSize(18);

        const QFont printed = Backend::printFont(editorFont, 96.0);
        QCOMPARE(printed.pixelSize(), -1);
        QVERIFY(qAbs(printed.pointSizeF() - 13.5) < 0.01);

        // An unusable DPI falls back to 96 rather than producing a zero size.
        QVERIFY(qAbs(Backend::printFont(editorFont, 0.0).pointSizeF() - 13.5) < 0.01);

        // A font that already carries a point size is left alone.
        QFont pointFont(QStringLiteral("iA Writer Mono S"));
        pointFont.setPointSizeF(11.0);
        QCOMPARE(Backend::printFont(pointFont, 96.0).pointSizeF(), 11.0);
    }

    void printsTextAtALegibleSizeOnAHighResolutionPrinter() {
        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());

        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(outputDirectory.filePath(QStringLiteral("out.pdf")));

        QFont editorFont(QStringLiteral("iA Writer Mono S"));
        editorFont.setPixelSize(18);

        // Measured on the printer itself: at 1200 dpi the unconverted font
        // lays out around 18 dots. A legible line is a good deal more than a
        // tenth of an inch.
        const QFontMetricsF printedMetrics(Backend::printFont(editorFont, 96.0), &printer);
        QVERIFY2(printedMetrics.height() > printer.logicalDpiY() * 0.1,
                 qPrintable(QStringLiteral("line height was %1 dots at %2 dpi")
                                .arg(printedMetrics.height())
                                .arg(printer.logicalDpiY())));

        const QFontMetricsF rawMetrics(editorFont, &printer);
        QVERIFY(printedMetrics.height() > rawMetrics.height() * 4);
    }

    // Issue #23: a save that does not happen used to leave pendingAction at
    // "close", so the next successful save closed the window.
    void dropsThePendingCloseWhenASaveFails() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        QTemporaryDir documentDirectory;
        QVERIFY(documentDirectory.isValid());
        const QString documentPath = documentDirectory.filePath(QStringLiteral("doc.md"));
        {
            QFile seed(documentPath);
            QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Text));
            seed.write("draft\n");
        }

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        backend.open(QUrl::fromLocalFile(documentPath));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(documentPath));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("draft plus more"));
        QVERIFY(backend.modified());

        // Put a directory where the file was, so the save fails for any uid.
        QVERIFY(QFile::remove(documentPath));
        QVERIFY(QDir().mkpath(documentPath));

        // What the unsaved-changes dialog's Save button does.
        window->setProperty("pendingAction", QStringLiteral("close"));
        window->setProperty("awaitingPendingSave", true);
        backend.save();

        QVERIFY(backend.status().startsWith(QStringLiteral("Could not")));
        QCOMPARE(window->property("pendingAction").toString(), QString());
        QCOMPARE(window->property("awaitingPendingSave").toBool(), false);
        QCOMPARE(window->property("closeConfirmed").toBool(), false);

        // An ordinary save minutes later must not carry out that close.
        QVERIFY(QDir().rmdir(documentPath));
        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);
        backend.save();
        QCOMPARE(savedSpy.count(), 1);
        QCOMPARE(window->property("closeConfirmed").toBool(), false);
    }

    void findsTheAppBundleAroundAMacExecutable() {
        QCOMPARE(Backend::enclosingBundlePath(
                     QStringLiteral("/Applications/Omawrite.app/Contents/MacOS")),
                 QStringLiteral("/Applications/Omawrite.app"));

        // A bare executable, as on Linux or in an unbundled test build.
        QVERIFY(Backend::enclosingBundlePath(QStringLiteral("/usr/bin")).isEmpty());
        QVERIFY(Backend::enclosingBundlePath(QStringLiteral("/tmp/build")).isEmpty());
        // The right layout, but the top directory is not a bundle.
        QVERIFY(Backend::enclosingBundlePath(
                    QStringLiteral("/tmp/omawrite/Contents/MacOS")).isEmpty());
    }

    void keepsTextColumnInsideNarrowWindows() {
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
        QVERIFY(viewport);

        // A resize reaches the window through its platform window, so the
        // bindings only see the new width once the events are delivered.
        const int minimumWidth = window->property("minimumWidth").toInt();
        window->setProperty("width", minimumWidth);
        QCoreApplication::processEvents();
        QCOMPARE(window->property("width").toInt(), minimumWidth);
        QVERIFY(editor->property("x").toInt() > 0);

        // A tiling compositor resizes below the minimum the window asks for.
        window->setProperty("width", 394);
        QCoreApplication::processEvents();
        QCOMPARE(window->property("width").toInt(), 394);
        QVERIFY(editor->property("width").toInt()
                <= viewport->property("width").toInt());
        QVERIFY(editor->property("x").toInt() >= 0);
    }
    void reservesAnOpaqueFooterBelowTheEditor() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        auto *footer = window->findChild<QQuickItem *>(QStringLiteral("footer"));
        auto *editorViewport = window->findChild<QQuickItem *>(
            QStringLiteral("editorViewport"));
        auto *saveButton = window->findChild<QQuickItem *>(QStringLiteral("saveButton"));
        QVERIFY(footer);
        QVERIFY(editorViewport);
        QVERIFY(saveButton);

        QCOMPARE(footer->opacity(), qreal(1));
        const QColor footerColor = footer->property("color").value<QColor>();
        QCOMPARE(footerColor.alpha(), 255);
        QCOMPARE(footerColor, QColor(backend.themeBackground()));
        QCOMPARE(editorViewport->mapToScene(QPointF(0, editorViewport->height())).y(),
                 footer->mapToScene(QPointF()).y());

        backend.setTextScale(0.5);
        QVERIFY(saveButton->mapToScene(QPointF()).y()
                >= footer->mapToScene(QPointF()).y());
        QCOMPARE(editorViewport->mapToScene(QPointF(0, editorViewport->height())).y(),
                 footer->mapToScene(QPointF()).y());
    }

    // Autosave is on by default: a named file follows the writer without a
    // Ctrl+S, once the debounce settles.
    void autosavesANamedFileOnceTypingStops() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("doc.md"));
        {
            QFile seed(path);
            QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Text));
            seed.write("first\n");
        }

        Backend backend;
        QVERIFY(backend.autosave());
        backend.setAutosaveDelayMs(200);
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        backend.open(QUrl::fromLocalFile(path));

        document->textDocument()->setPlainText(QStringLiteral("second"));
        backend.editorTextChanged();
        QVERIFY(backend.modified());

        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);
        QTRY_VERIFY_WITH_TIMEOUT(savedSpy.count() == 1, 3000);
        QVERIFY(!backend.modified());

        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(written.readAll()).trimmed(),
                 QStringLiteral("second"));
    }

    // An untitled draft has nowhere to be saved, so it keeps getting the
    // snapshot instead -- and no file is invented for it.
    void snapshotsUntitledDocumentsInsteadOfSavingThem() {
        Backend backend;
        backend.setAutosaveDelayMs(200);
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);

        QVERIFY(!backend.canAutosaveToFile());
        document->textDocument()->setPlainText(QStringLiteral("a scratch note"));
        backend.editorTextChanged();
        QVERIFY(backend.modified());

        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(backend.recoveryPath()), 3000);
        QVERIFY(backend.modified());

        QFile snapshot(backend.recoveryPath());
        QVERIFY(snapshot.open(QIODevice::ReadOnly));
        const QJsonObject saved =
            QJsonDocument::fromJson(snapshot.readAll()).object()
                .value(QStringLiteral("windows")).toArray().at(0).toObject()
                .value(QStringLiteral("tabs")).toArray().at(0).toObject();
        QCOMPARE(saved.value(QStringLiteral("text")).toString(),
                 QStringLiteral("a scratch note"));
        QVERIFY(saved.value(QStringLiteral("fileUrl")).toString().isEmpty());
    }

    // The guardrail that matters most: something else changed the file and the
    // writer has not said which version wins. Autosave must not answer for them.
    void doesNotAutosaveOverAnUnansweredExternalChange() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("doc.md"));
        {
            QFile seed(path);
            QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Text));
            seed.write("mine\n");
        }

        Backend backend;
        backend.setAutosaveDelayMs(200);
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        backend.open(QUrl::fromLocalFile(path));
        QVERIFY(backend.canAutosaveToFile());

        document->textDocument()->setPlainText(QStringLiteral("my edit"));
        backend.editorTextChanged();
        QVERIFY(backend.modified());

        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);
        {
            QFile outside(path);
            QVERIFY(outside.open(QIODevice::WriteOnly | QIODevice::Text));
            outside.write("theirs\n");
        }
        QTRY_VERIFY_WITH_TIMEOUT(externalSpy.count() >= 1, 5000);

        QVERIFY(!backend.canAutosaveToFile());

        // Give the debounce every chance to fire, then check it did not.
        QTest::qWait(600);
        QFile onDisk(path);
        QVERIFY(onDisk.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(onDisk.readAll()).trimmed(),
                 QStringLiteral("theirs"));
        QVERIFY(backend.modified());

        // Once they answer, autosave is allowed again.
        backend.keepExternalVersion();
        QVERIFY(backend.canAutosaveToFile());
    }

    // A name taken for a file that was not there yet is unguarded until the
    // first explicit save, so autosave leaves that path alone entirely.
    void doesNotAutosaveOntoANeverReadPath() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("later.md"));

        Backend backend;
        backend.setAutosaveDelayMs(200);
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);

        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(path));
        // The name is taken, but nothing has read that path, so autosave holds
        // off until the writer has saved there once themselves.
        QVERIFY(!backend.canAutosaveToFile());

        {
            QFile appeared(path);
            QVERIFY(appeared.open(QIODevice::WriteOnly | QIODevice::Text));
            appeared.write("someone else got here first\n");
        }
        QVERIFY(!backend.canAutosaveToFile());

        document->textDocument()->setPlainText(QStringLiteral("my draft"));
        backend.editorTextChanged();
        QVERIFY(backend.modified());
        QTest::qWait(600);

        QFile onDisk(path);
        QVERIFY(onDisk.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(onDisk.readAll()).trimmed(),
                 QStringLiteral("someone else got here first"));

        // An explicit save still asks rather than replacing it unseen.
        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
    }

    void turnsAutosaveOffAndRemembersIt() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("doc.md"));
        {
            QFile seed(path);
            QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Text));
            seed.write("first\n");
        }

        {
            Backend backend;
            QVERIFY(backend.autosave());
            QSignalSpy changedSpy(&backend, &Backend::autosaveChanged);
            backend.setAutosave(false);
            QCOMPARE(changedSpy.count(), 1);

            backend.setAutosaveDelayMs(200);
            QQuickTextDocument *document = attachTextEdit(&backend);
            QVERIFY(document);
            backend.open(QUrl::fromLocalFile(path));
            QVERIFY(!backend.canAutosaveToFile());

            document->textDocument()->setPlainText(QStringLiteral("second"));
            backend.editorTextChanged();
            QVERIFY(backend.modified());
            // The snapshot still happens: turning autosave off is not a choice
            // to lose work on a crash.
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(backend.recoveryPath()), 3000);

            QFile onDisk(path);
            QVERIFY(onDisk.open(QIODevice::ReadOnly | QIODevice::Text));
            QCOMPARE(QString::fromUtf8(onDisk.readAll()).trimmed(),
                     QStringLiteral("first"));
        }

        Backend restored;
        QVERIFY(!restored.autosave());
    }

    void boundsTheAutosaveDelayAndRemembersIt() {
        {
            Backend backend;
            QCOMPARE(backend.autosaveDelayMs(), 750);
            backend.setAutosaveDelayMs(5);
            QCOMPARE(backend.autosaveDelayMs(), 200);
            backend.setAutosaveDelayMs(999999);
            QCOMPARE(backend.autosaveDelayMs(), 60000);
            backend.setAutosaveDelayMs(2000);
            QCOMPARE(backend.autosaveDelayMs(), 2000);
        }

        Backend restored;
        QCOMPARE(restored.autosaveDelayMs(), 2000);
    }

    // Command+P then "Save as PDF" offered "Untitled" whatever the document
    // was called, because the job carried no name at all.
    void namesThePrintJobAfterTheDocument() {
        QCOMPARE(Backend::printJobName(QStringLiteral("notes.md")),
                 QStringLiteral("notes"));
        QCOMPARE(Backend::printJobName(QStringLiteral("Quarterly Report.markdown")),
                 QStringLiteral("Quarterly Report"));
        // A name with dots keeps all but the Markdown extension.
        QCOMPARE(Backend::printJobName(QStringLiteral("2026.08.31 standup.md")),
                 QStringLiteral("2026.08.31 standup"));
        // Anything that is not Markdown keeps the name it has.
        QCOMPARE(Backend::printJobName(QStringLiteral("readme.txt")),
                 QStringLiteral("readme.txt"));
        QCOMPARE(Backend::printJobName(QString()), QStringLiteral("Untitled"));
    }

    void carriesTheDocumentNameIntoThePrinter() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("chapter-one.md"));
        {
            QFile seed(path);
            QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Text));
            seed.write("text\n");
        }

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);

        // The untitled case is covered by namesThePrintJobAfterTheDocument();
        // a fresh Backend here can legitimately claim a leftover snapshot.
        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(backend.fileName(), QStringLiteral("chapter-one.md"));
        QCOMPARE(Backend::printJobName(backend.fileName()),
                 QStringLiteral("chapter-one"));
    }

    void fallsBackToTheBundledFaceForAFontTheSystemLacks() {
        const QStringList available{QStringLiteral("Aptos"),
                                    QStringLiteral("iA Writer Quattro S")};
        QCOMPARE(Backend::resolveFontFamily(QStringLiteral("Aptos"), available),
                 QStringLiteral("Aptos"));
        // Named in settings on another machine, absent here.
        QCOMPARE(Backend::resolveFontFamily(QStringLiteral("Comic Sans MS"), available),
                 QStringLiteral("iA Writer Quattro S"));
        QCOMPARE(Backend::resolveFontFamily(QString(), available),
                 QStringLiteral("iA Writer Quattro S"));
    }

    void carriesAllThreeIaFacesAndDefaultsToQuattro() {
        // All three ship with the app, so the typeface list never depends on
        // what a given machine happens to have installed.
        QCOMPARE(Backend::bundledFontFamilies(),
                 QStringList({QStringLiteral("iA Writer Quattro S"),
                              QStringLiteral("iA Writer Duo S"),
                              QStringLiteral("iA Writer Mono S")}));

        // Every one of them is really in the binary, under the name Qt will
        // register it as.
        for (const QString &family : Backend::bundledFontFamilies()) {
            const QString stem = QString(family)
                                     .remove(QStringLiteral("iA Writer "))
                                     .remove(QLatin1Char(' '));
            for (const QString &style : {QStringLiteral("Regular"), QStringLiteral("Italic"),
                                         QStringLiteral("Bold"), QStringLiteral("BoldItalic")}) {
                const QString path =
                    QStringLiteral(":/fonts/iAWriter%1-%2.ttf").arg(stem, style);
                QVERIFY2(QFile::exists(path), qPrintable(path));
                const int id = QFontDatabase::addApplicationFont(path);
                QVERIFY2(id >= 0, qPrintable(path));
                QVERIFY2(QFontDatabase::applicationFontFamilies(id).contains(family),
                         qPrintable(family + QStringLiteral(" <- ") + path));
            }
        }

        // Quattro leads the Preferences list, and is what an unconfigured
        // editor writes in.
        QCOMPARE(Backend::availableFontFamilies().first(),
                 QStringLiteral("iA Writer Quattro S"));
        Backend backend;
        QCOMPARE(backend.editorFontFamily(), QStringLiteral("iA Writer Quattro S"));
    }

    void remembersCaretAndMeasureAndPrintMargins() {
        {
            Backend backend;
            QCOMPARE(backend.caretStyle(), QStringLiteral("line"));
            QVERIFY(backend.caretBlink());
            QCOMPARE(backend.editorMeasureChars(), 65);
            QCOMPARE(backend.printMarginMm(), 15.0);

            QSignalSpy caretSpy(&backend, &Backend::caretStyleChanged);
            backend.setCaretStyle(QStringLiteral("block"));
            QCOMPARE(caretSpy.count(), 1);
            // Anything unrecognised means the default, never an empty caret.
            backend.setCaretStyle(QStringLiteral("wedge"));
            QCOMPARE(backend.caretStyle(), QStringLiteral("line"));
            backend.setCaretStyle(QStringLiteral("block"));

            backend.setCaretBlink(false);
            backend.setEditorMeasureChars(80);
            backend.setPrintMarginMm(12.5);

            // Bounds hold on both ends.
            backend.setEditorMeasureChars(5);
            QCOMPARE(backend.editorMeasureChars(), 20);
            backend.setEditorMeasureChars(9999);
            QCOMPARE(backend.editorMeasureChars(), 200);
            backend.setEditorMeasureChars(80);
            backend.setPrintMarginMm(-4);
            QCOMPARE(backend.printMarginMm(), 0.0);
            backend.setPrintMarginMm(500);
            QCOMPARE(backend.printMarginMm(), 60.0);
            backend.setPrintMarginMm(12.5);
        }

        Backend restored;
        QCOMPARE(restored.caretStyle(), QStringLiteral("block"));
        QVERIFY(!restored.caretBlink());
        QCOMPARE(restored.editorMeasureChars(), 80);
        QCOMPARE(restored.printMarginMm(), 12.5);
    }

    void keepsTheEditorFontOnTheDocumentAndRemembersIt() {
        // Switch away from whatever this machine resolved to by default, so
        // the test is a real change rather than a no-op.
        QString family;
        {
            Backend probe;
            family = probe.editorFontFamily() == QStringLiteral("iA Writer Mono S")
                ? QStringLiteral("Aptos")
                : QStringLiteral("iA Writer Mono S");
        }
        if (family == QStringLiteral("Aptos")
                && !QFontDatabase::families().contains(family)) {
            QSKIP("no second family available to switch to");
        }

        {
            Backend backend;
            QQuickTextDocument *document = attachTextEdit(&backend);
            QVERIFY(document);

            QSignalSpy familySpy(&backend, &Backend::editorFontFamilyChanged);
            backend.setEditorFontFamily(family);
            QCOMPARE(backend.editorFontFamily(), family);
            QCOMPARE(familySpy.count(), 1);
            // The printed page is rendered from this document's font.
            QCOMPARE(document->textDocument()->defaultFont().family(), family);
        }

        Backend restored;
        QCOMPARE(restored.editorFontFamily(), family);
    }

    void togglesPreviewFromFooterButtonAndBack() {
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
        QObject *preview = window->findChild<QObject *>(QStringLiteral("previewView"));
        QObject *previewButton = window->findChild<QObject *>(QStringLiteral("previewButton"));
        QVERIFY(editor);
        QVERIFY(preview);
        QVERIFY(previewButton);

        // Source is the default: the editor is the visible surface.
        QVERIFY(!window->property("previewMode").toBool());
        QVERIFY(editor->property("visible").toBool());
        QVERIFY(!preview->property("visible").toBool());

        QVERIFY(QMetaObject::invokeMethod(previewButton, "clicked"));
        QVERIFY(window->property("previewMode").toBool());
        QVERIFY(!editor->property("visible").toBool());
        QVERIFY(preview->property("visible").toBool());

        QVERIFY(QMetaObject::invokeMethod(previewButton, "clicked"));
        QVERIFY(!window->property("previewMode").toBool());
        QVERIFY(editor->property("visible").toBool());
        QVERIFY(!preview->property("visible").toBool());
    }

    void restoresBlockSpacingLostByTheMarkdownReader() {
        QQmlEngine engine;
        QScopedPointer<QObject> harness(makeRenderHarness(engine));
        QVERIFY(harness);

        Backend backend;
        QTextDocument *rendered = renderThrough(backend, harness.data(),
            QStringLiteral("# Heading\n\nParagraph one.\n\n- item\n- item\n\n> quoted\n"));
        QVERIFY(rendered);

        qreal headingTop = -1, paragraphBottom = -1, listBottom = -1, quoteTop = -1;
        for (QTextBlock block = rendered->begin(); block.isValid(); block = block.next()) {
            const QTextBlockFormat format = block.blockFormat();
            if (format.headingLevel() > 0)
                headingTop = format.topMargin();
            else if (block.textList())
                listBottom = format.bottomMargin();
            else if (format.intProperty(QTextFormat::BlockQuoteLevel) > 0)
                quoteTop = format.topMargin();
            else if (block.text().startsWith(QStringLiteral("Paragraph")))
                paragraphBottom = format.bottomMargin();
        }

        // Qt's Markdown reader leaves every block flush against the next; the
        // render pass is the only thing putting the source's blank lines back.
        QVERIFY2(paragraphBottom > 0, "paragraphs must gain a trailing gap");
        QVERIFY2(headingTop > 0, "headings must lead their section");
        QVERIFY2(quoteTop > 0, "quotes need a leading gap or they read as a list row");
        QVERIFY2(listBottom >= 0 && listBottom < paragraphBottom,
                 "list items stay tighter than paragraphs");
    }

    void collapsesRunsOfBlankLinesToOneGap() {
        QQmlEngine engine;
        QScopedPointer<QObject> harness(makeRenderHarness(engine));
        QVERIFY(harness);

        // CommonMark treats any run of blank lines as one block separator, so
        // the render must not grow with the number of newlines in the source.
        const auto spacingOf = [&](const QString &markdown) {
            Backend backend;
            QTextDocument *rendered = renderThrough(backend, harness.data(), markdown);
            QList<qreal> margins;
            if (rendered) {
                for (QTextBlock block = rendered->begin(); block.isValid();
                     block = block.next())
                    margins.append(block.blockFormat().bottomMargin());
            }
            return margins;
        };

        const QList<qreal> single = spacingOf(QStringLiteral("A.\n\nB.\n"));
        const QList<qreal> quadruple = spacingOf(QStringLiteral("A.\n\n\n\n\nB.\n"));
        QCOMPARE(single.size(), 2);
        QCOMPARE(quadruple, single);
    }

    // Two TextEdits standing in for the real pair: one the highlighter binds to,
    // one the preview renders into.
    static QObject *makeRenderHarness(QQmlEngine &engine) {
        auto *component = new QQmlComponent(&engine, &engine);
        component->setData(R"QML(
            import QtQuick
            Item {
                property alias source: sourceEdit
                property alias rendered: renderedEdit
                TextEdit { id: sourceEdit }
                TextEdit { id: renderedEdit }
            }
        )QML", QUrl());
        if (!component->isReady())
            return nullptr;
        return component->create();
    }

    static QTextDocument *renderThrough(Backend &backend, QObject *harness,
                                        const QString &markdown) {
        QObject *source = harness->property("source").value<QObject *>();
        QObject *rendered = harness->property("rendered").value<QObject *>();
        if (!source || !rendered)
            return nullptr;

        auto *sourceDocument = source->property("textDocument").value<QQuickTextDocument *>();
        auto *renderedDocument = rendered->property("textDocument").value<QQuickTextDocument *>();
        if (!sourceDocument || !renderedDocument)
            return nullptr;

        backend.attachDocument(sourceDocument);
        source->setProperty("text", markdown);
        backend.renderPreview(renderedDocument);
        return renderedDocument->textDocument();
    }

    void splitsParagraphsIntoSentences() {
        const QStringList plain = Backend::splitSentences(
            QStringLiteral("One thing. Then another! And a third? Yes."));
        QCOMPARE(plain, QStringList({QStringLiteral("One thing."),
                                     QStringLiteral("Then another!"),
                                     QStringLiteral("And a third?"),
                                     QStringLiteral("Yes.")}));

        // Abbreviations and initials are not sentence ends.
        QCOMPARE(Backend::splitSentences(
                     QStringLiteral("Dr. Smith wrote it. J. B. Peterson agrees.")),
                 QStringList({QStringLiteral("Dr. Smith wrote it."),
                              QStringLiteral("J. B. Peterson agrees.")}));
        QCOMPARE(Backend::splitSentences(
                     QStringLiteral("Use tools, e.g. this one. Then stop.")),
                 QStringList({QStringLiteral("Use tools, e.g. this one."),
                              QStringLiteral("Then stop.")}));

        // Decimals stay put.
        QCOMPARE(Backend::splitSentences(QStringLiteral("It grew 3.5 times. Truly.")),
                 QStringList({QStringLiteral("It grew 3.5 times."),
                              QStringLiteral("Truly.")}));

        QCOMPARE(Backend::splitSentences(QString()), QStringList());
        QCOMPARE(Backend::splitSentences(QStringLiteral("No terminator here")),
                 QStringList({QStringLiteral("No terminator here")}));
    }

    void explodesAndCollapsesTheParagraphAtTheCursor() {
        const QString text = QStringLiteral(
            "Intro line.\n\nFirst point. Second point. Third point.\n\nAfter.");
        const int cursor = text.indexOf(QStringLiteral("Second"));

        const QVariantMap exploded = Backend::explodeSentences(text, cursor);
        QCOMPARE(exploded.value(QStringLiteral("text")).toString(),
                 QStringLiteral("Intro line.\n\nFirst point.\nSecond point.\n"
                                "Third point.\n\nAfter."));

        // Round trip: collapsing the exploded paragraph restores it.
        const QVariantMap collapsed = Backend::collapseSentences(
            exploded.value(QStringLiteral("text")).toString(),
            exploded.value(QStringLiteral("cursor")).toInt());
        QCOMPARE(collapsed.value(QStringLiteral("text")).toString(), text);

        // Neighbouring paragraphs are untouched, and a one-sentence paragraph
        // is left exactly as it is.
        const QVariantMap single = Backend::explodeSentences(text, 0);
        QCOMPARE(single.value(QStringLiteral("text")).toString(), text);
    }

    void movesTheParagraphAtTheCursorPastItsNeighbour() {
        const QString text = QStringLiteral("Alpha one.\n\nBeta two.\n\nGamma three.");
        const int inBeta = text.indexOf(QStringLiteral("Beta"));

        const QVariantMap up = Backend::moveParagraph(text, inBeta, -1);
        QCOMPARE(up.value(QStringLiteral("text")).toString(),
                 QStringLiteral("Beta two.\n\nAlpha one.\n\nGamma three."));
        // The cursor rides along with the paragraph it was in.
        const QString movedUp = up.value(QStringLiteral("text")).toString();
        QCOMPARE(movedUp.mid(up.value(QStringLiteral("cursor")).toInt(), 4),
                 QStringLiteral("Beta"));

        const QVariantMap down = Backend::moveParagraph(text, inBeta, 1);
        QCOMPARE(down.value(QStringLiteral("text")).toString(),
                 QStringLiteral("Alpha one.\n\nGamma three.\n\nBeta two."));

        // At either end there is nothing to trade places with.
        QCOMPARE(Backend::moveParagraph(text, 0, -1)
                     .value(QStringLiteral("text")).toString(), text);
        QCOMPARE(Backend::moveParagraph(text, text.indexOf(QStringLiteral("Gamma")), 1)
                     .value(QStringLiteral("text")).toString(), text);
    }

    void movesMultiLineParagraphsWhole() {
        const QString text = QStringLiteral(
            "First line\nsecond line\n\nOther paragraph.");
        const QVariantMap down = Backend::moveParagraph(text, 0, 1);
        QCOMPARE(down.value(QStringLiteral("text")).toString(),
                 QStringLiteral("Other paragraph.\n\nFirst line\nsecond line"));
    }

    void readsTheOutlineFromHeadings() {
        const QString text = QStringLiteral(
            "# Title\n\nSome words.\n\n## First part\n\nMore.\n\n"
            "```\n# not a heading\n```\n\n### Deep\n");
        const QVariantList outline = Backend::outlineFor(text);
        QCOMPARE(outline.size(), 3);

        const QVariantMap first = outline.at(0).toMap();
        QCOMPARE(first.value(QStringLiteral("level")).toInt(), 1);
        QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("Title"));
        QCOMPARE(first.value(QStringLiteral("position")).toInt(), 0);

        QCOMPARE(outline.at(1).toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("First part"));
        QCOMPARE(outline.at(2).toMap().value(QStringLiteral("level")).toInt(), 3);

        // The position is where the heading actually starts.
        const int deepPosition =
            outline.at(2).toMap().value(QStringLiteral("position")).toInt();
        QVERIFY(text.mid(deepPosition).startsWith(QStringLiteral("### Deep")));

        QCOMPARE(Backend::outlineFor(QStringLiteral("no headings here")).size(), 0);
    }

    void computesADraftTargetAQuarterAboveTheFinalLength() {
        QCOMPARE(Backend::draftTargetFor(1000), 1250);
        QCOMPARE(Backend::draftTargetFor(4), 5);
        // No target means no draft target, rather than a target of zero words.
        QCOMPARE(Backend::draftTargetFor(0), 0);
        QCOMPARE(Backend::draftTargetFor(-10), 0);
    }

    void remembersTheWordTarget() {
        {
            Backend backend;
            QCOMPARE(backend.wordTarget(), 0);
            QSignalSpy targetSpy(&backend, &Backend::wordTargetChanged);
            backend.setWordTarget(1500);
            QCOMPARE(targetSpy.count(), 1);
            backend.setWordTarget(-5);
            QCOMPARE(backend.wordTarget(), 0);
            backend.setWordTarget(1500);
        }

        Backend restored;
        QCOMPARE(restored.wordTarget(), 1500);
    }

    void showsProgressAgainstTheWordTargetInTheFooter() {
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
        editor->setProperty("text", QStringLiteral("one two three"));

        QObject *count = window->findChild<QObject *>(QStringLiteral("wordCountLabel"));
        QVERIFY(count);
        QTRY_COMPARE(count->property("text").toString(), QStringLiteral("3 Words"));

        backend.setWordTarget(1000);
        QTRY_COMPARE(count->property("text").toString(),
                     QStringLiteral("3 / 1000 Words  (1250 draft)"));
    }

    void listsTheDocumentHeadingsInTheOutlineDialog() {
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
        editor->setProperty("text",
                            QStringLiteral("# One\n\nwords\n\n## Two\n\nmore\n"));

        QObject *dialog = window->findChild<QObject *>(QStringLiteral("outlineDialog"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_COMPARE(dialog->property("entries").toList().size(), 2);
        QCOMPARE(dialog->property("entries").toList().at(1).toMap()
                     .value(QStringLiteral("title")).toString(),
                 QStringLiteral("Two"));
    }

    void writesEverySettingFromThePreferencesDialog() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *dialog = window->findChild<QObject *>(QStringLiteral("preferencesDialog"));
        QVERIFY2(dialog, "the preferences dialog did not load");
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));

        // Every setting has a control, and every control writes the backend.
        // QVERIFY2 returns void, so the lookup asserts through a pointer the
        // caller checks rather than returning early from inside the lambda.
        auto control = [&](const char *name) -> QObject * {
            QObject *found = window->findChild<QObject *>(QLatin1String(name));
            if (!found)
                qWarning("preferences control missing: %s", name);
            return found;
        };

        QObject *measure = control("measureBox");
        QVERIFY(measure);
        measure->setProperty("value", 90);
        QVERIFY(QMetaObject::invokeMethod(measure, "valueModified"));
        QCOMPARE(backend.editorMeasureChars(), 90);

        QObject *target = control("wordTargetBox");
        QVERIFY(target);
        target->setProperty("value", 800);
        QVERIFY(QMetaObject::invokeMethod(target, "valueModified"));
        QCOMPARE(backend.wordTarget(), 800);

        QObject *margin = control("printMarginBox");
        QVERIFY(margin);
        margin->setProperty("value", 30);
        QVERIFY(QMetaObject::invokeMethod(margin, "valueModified"));
        QCOMPARE(backend.printMarginMm(), 30.0);

        QObject *delay = control("autosaveDelayBox");
        QVERIFY(delay);
        delay->setProperty("value", 1500);
        QVERIFY(QMetaObject::invokeMethod(delay, "valueModified"));
        QCOMPARE(backend.autosaveDelayMs(), 1500);

        QObject *blink = control("caretBlinkSwitch");
        QVERIFY(blink);
        blink->setProperty("checked", false);
        QVERIFY(QMetaObject::invokeMethod(blink, "toggled"));
        QVERIFY(!backend.caretBlink());

        QObject *autosave = control("autosaveSwitch");
        QVERIFY(autosave);
        autosave->setProperty("checked", false);
        QVERIFY(QMetaObject::invokeMethod(autosave, "toggled"));
        QVERIFY(!backend.autosave());

        QObject *caret = control("caretStyleBox");
        QVERIFY(caret);
        caret->setProperty("currentIndex", 1);
        QVERIFY(QMetaObject::invokeMethod(caret, "activated", Q_ARG(int, 1)));
        QCOMPARE(backend.caretStyle(), QStringLiteral("block"));

        // The typeface list always offers the shipped faces, so there is
        // something to choose even on a bare machine.
        QVERIFY(control("fontFamilyBox"));
        for (const QString &family : Backend::bundledFontFamilies())
            QVERIFY2(Backend::availableFontFamilies().contains(family), qPrintable(family));
        QCOMPARE(Backend::availableFontFamilies().first(),
                 QStringLiteral("iA Writer Quattro S"));
    }

    // The preview is the same document seen another way, so it has to be set
    // the same way: the editor's leading, and a paragraph gap that is the
    // blank line the source actually contains.
    void setsThePreviewWithTheSameLeadingAsTheEditor() {
        QQmlEngine engine;
        QScopedPointer<QObject> harness(makeRenderHarness(engine));
        QVERIFY(harness);

        Backend backend;
        QQuickTextDocument *editorDocument = attachTextEdit(&backend);
        QVERIFY(editorDocument);
        editorDocument->textDocument()->setPlainText(
            QStringLiteral("One paragraph.\n\nAnother paragraph.\n"));
        backend.editorTextChanged();

        // What the editor writes with.
        const QTextBlockFormat editorFormat =
            editorDocument->textDocument()->begin().blockFormat();
        QCOMPARE(editorFormat.lineHeightType(), int(QTextBlockFormat::ProportionalHeight));
        const qreal editorLeading = editorFormat.lineHeight();
        QVERIFY(editorLeading > 100);

        QTextDocument *rendered = renderThrough(backend, harness.data(),
            QStringLiteral("One paragraph.\n\nAnother paragraph.\n"));
        QVERIFY(rendered);

        int paragraphs = 0;
        for (QTextBlock block = rendered->begin(); block.isValid(); block = block.next()) {
            const QTextBlockFormat format = block.blockFormat();
            QCOMPARE(format.lineHeightType(), int(QTextBlockFormat::ProportionalHeight));
            QCOMPARE(format.lineHeight(), editorLeading);
            ++paragraphs;
        }
        QVERIFY(paragraphs >= 2);

        // And the gap between paragraphs is one of those lines, which is what
        // the blank line in the source comes to.
        const qreal line = QFontMetricsF(rendered->defaultFont()).height();
        QVERIFY(line > 0);
        QCOMPARE(rendered->begin().blockFormat().bottomMargin(),
                 line * editorLeading / 100.0);
    }

    // By default Return breaks the line once. Opening a whole paragraph is a
    // setting, because a Return that inserts a blank line surprises people.
    void breaksTheLineOnceOnReturnByDefault() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QVERIFY(!backend.paragraphOnReturn());

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto load = [&](const QString &content, int position) {
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };
        auto returnKey = [&] { QTest::keyClick(window, Qt::Key_Return); };

        // The caret sits after a full stop: one break, not a blank line.
        load(QStringLiteral("A sentence."), 11);
        returnKey();
        QCOMPARE(text(), QStringLiteral("A sentence.\n"));

        // A second Return is how the blank line gets there, deliberately.
        returnKey();
        QCOMPARE(text(), QStringLiteral("A sentence.\n\n"));

        // Lists still continue themselves rather than breaking once.
        load(QStringLiteral("- item"), 6);
        returnKey();
        QCOMPARE(text(), QStringLiteral("- item\n- "));

        // And the setting brings the paragraph behaviour back.
        backend.setParagraphOnReturn(true);
        load(QStringLiteral("A sentence."), 11);
        returnKey();
        QCOMPARE(text(), QStringLiteral("A sentence.\n\n"));
    }

    void readsTheLanguageOffTheFence() {
        QCOMPARE(MarkdownHighlighter::languageForFence(QStringLiteral("```python")),
                 QStringLiteral("python"));
        // Aliases land on the same rules.
        QCOMPARE(MarkdownHighlighter::languageForFence(QStringLiteral("```py")),
                 QStringLiteral("python"));
        QCOMPARE(MarkdownHighlighter::languageForFence(QStringLiteral("```ts")),
                 QStringLiteral("javascript"));
        QCOMPARE(MarkdownHighlighter::languageForFence(QStringLiteral("   ```Bash ")),
                 QStringLiteral("bash"));
        // A bare fence, or one naming something unknown, is plain code.
        QVERIFY(MarkdownHighlighter::languageForFence(QStringLiteral("```")).isEmpty());
        QVERIFY(MarkdownHighlighter::languageForFence(
                    QStringLiteral("```brainfuck")).isEmpty());
        // Every language the state can index is resolvable.
        for (const QString &language : MarkdownHighlighter::codeLanguages()) {
            QCOMPARE(MarkdownHighlighter::languageForFence(
                         QStringLiteral("```") + language), language);
        }
    }

    void tokenisesCodeInsideAFence() {
        using Token = MarkdownHighlighter::CodeToken;
        auto tokens = [](const QString &line, const QString &language) {
            return MarkdownHighlighter::codeTokens(line, language);
        };
        auto textOf = [](const QString &line,
                         const MarkdownHighlighter::CodeSpanToken &token) {
            return line.mid(token.start, token.length);
        };

        const QString shell = QStringLiteral("export NAME=\"omawrite\"  # a comment");
        const auto shellTokens = tokens(shell, QStringLiteral("bash"));
        QCOMPARE(shellTokens.size(), 3);
        QCOMPARE(shellTokens.at(0).token, Token::Keyword);
        QCOMPARE(textOf(shell, shellTokens.at(0)), QStringLiteral("export"));
        QCOMPARE(shellTokens.at(1).token, Token::String);
        QCOMPARE(textOf(shell, shellTokens.at(1)), QStringLiteral("\"omawrite\""));
        QCOMPARE(shellTokens.at(2).token, Token::Comment);
        QCOMPARE(textOf(shell, shellTokens.at(2)), QStringLiteral("# a comment"));

        // A comment takes the rest of the line, so nothing after it is code.
        const QString commented = QStringLiteral("x = 1  // return 42");
        const auto commentedTokens = tokens(commented, QStringLiteral("javascript"));
        QCOMPARE(commentedTokens.size(), 2);
        QCOMPARE(commentedTokens.at(1).token, Token::Comment);

        // A number is a number, but not when it is part of a name.
        const QString numbers = QStringLiteral("let sha256 = 3.5");
        const auto numberTokens = tokens(numbers, QStringLiteral("javascript"));
        QCOMPARE(numberTokens.size(), 2);
        QCOMPARE(numberTokens.at(0).token, Token::Keyword);
        QCOMPARE(numberTokens.at(1).token, Token::Number);
        QCOMPARE(textOf(numbers, numberTokens.at(1)), QStringLiteral("3.5"));

        // An unterminated quote takes the rest of the line, which is how it
        // looks on screen anyway.
        const QString unterminated = QStringLiteral("echo \"unclosed");
        const auto unterminatedTokens = tokens(unterminated, QStringLiteral("bash"));
        QCOMPARE(unterminatedTokens.size(), 2);
        QCOMPARE(textOf(unterminated, unterminatedTokens.at(1)),
                 QStringLiteral("\"unclosed"));

        // A shell line is mostly a command and a path, and the command is the
        // part worth seeing. Without this the snippet had nothing in it at all.
        const QString command = QStringLiteral("open /Users/me/build/omawrite.dmg");
        const auto commandTokens = tokens(command, QStringLiteral("bash"));
        QCOMPARE(commandTokens.size(), 1);
        QCOMPARE(commandTokens.at(0).token, Token::Function);
        QCOMPARE(textOf(command, commandTokens.at(0)), QStringLiteral("open"));

        // A pipe starts another command.
        const QString piped = QStringLiteral("cat notes | grep todo");
        const auto pipedTokens = tokens(piped, QStringLiteral("bash"));
        QCOMPARE(pipedTokens.size(), 2);
        QCOMPARE(textOf(piped, pipedTokens.at(0)), QStringLiteral("cat"));
        QCOMPARE(textOf(piped, pipedTokens.at(1)), QStringLiteral("grep"));

        // An argument is not a command, and a keyword still wins.
        const QString mixed = QStringLiteral("export PATH=/usr/bin");
        const auto mixedTokens = tokens(mixed, QStringLiteral("bash"));
        QCOMPARE(mixedTokens.size(), 1);
        QCOMPARE(mixedTokens.at(0).token, Token::Keyword);

        // Elsewhere it is a call that makes a name a function.
        const QString call = QStringLiteral("result = compute(x)");
        const auto callTokens = tokens(call, QStringLiteral("python"));
        QCOMPARE(callTokens.size(), 1);
        QCOMPARE(callTokens.at(0).token, Token::Function);
        QCOMPARE(textOf(call, callTokens.at(0)), QStringLiteral("compute"));

        // No language, no tokens: plain code stays plain.
        QVERIFY(tokens(shell, QString()).isEmpty());
        QVERIFY(tokens(shell, QStringLiteral("brainfuck")).isEmpty());

        // Spans never run past the line.
        for (const QString &language : MarkdownHighlighter::codeLanguages()) {
            for (const auto &token : tokens(shell, language)) {
                QVERIFY(token.start >= 0);
                QVERIFY(token.length > 0);
                QVERIFY(token.start + token.length <= shell.size());
            }
        }
    }

    // The regression the state packing could cause: markdown markers must stay
    // literal inside a fence that names a language, not just a bare one.
    void keepsMarkersLiteralInsideALanguageFence() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("```python\n"
                                             "name = *not italic*\n"
                                             "```\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.rehighlight();

        const QTextBlock code = document.findBlockByNumber(1);
        QVERIFY(MarkdownHighlighter::isInsideFence(code.userState()));

        // One run for the whole line: the code panel, with nothing hidden and
        // nothing italic inside it.
        const QList<QTextLayout::FormatRange> runs = code.layout()->formats();
        QVERIFY(!runs.isEmpty());
        for (const QTextLayout::FormatRange &run : runs)
            QVERIFY(!run.format.fontItalic());
    }

    void scalesTouchpadScrollingAndCarriesMomentum() {
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
        QObject *editor = root->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *flick = root->findChild<QObject *>(QStringLiteral("editorViewport"));
        QVERIFY(window);
        QVERIFY(editor);
        QVERIFY(flick);

        QStringList lines;
        for (int i = 0; i < 200; ++i)
            lines.append(QStringLiteral("A line long enough to make the editor scroll."));
        editor->setProperty("text", lines.join(QLatin1Char('\n')));
        QTRY_VERIFY(flick->property("contentHeight").toReal()
                    > flick->property("height").toReal() + 400);
        flick->setProperty("contentY", 200.0);

        const QPointF position(window->width() / 2.0, window->height() / 2.0);
        sendTouchpadWheel(window, position, -8, Qt::ScrollBegin);
        QTest::qWait(20);
        sendTouchpadWheel(window, position, -10, Qt::ScrollUpdate);
        QTest::qWait(20);
        sendTouchpadWheel(window, position, -10, Qt::ScrollUpdate);

        // The scale is platform-dependent -- macOS accelerates trackpad deltas
        // before Qt sees them -- so the expectation comes from the viewport
        // rather than from the number one platform happens to produce.
        const qreal scale = flick->property("touchpadScale").toReal();
        QVERIFY(scale > 0);
        const qreal expected = 200.0 + (8 + 10 + 10) * scale;
        const qreal gestureEndY = flick->property("contentY").toReal();
        QVERIFY2(qAbs(gestureEndY - expected) <= 2.0,
                 qPrintable(QStringLiteral("Expected contentY near %1 at scale %2, got %3")
                                .arg(expected).arg(scale).arg(gestureEndY)));

        // The coast is driven by a velocity the gesture estimates from how far
        // apart its events arrive -- and only counts events closer together
        // than the gap window. Synthetic events on a loaded machine overshoot
        // that window, so the estimate comes out zero and there is nothing to
        // coast: this failed on CI while passing here every time.
        //
        // So the estimate and the coast are checked apart. The estimate is
        // whatever this machine managed; the coast is given a known velocity,
        // which is the part that has behaviour worth asserting.
        flick->setProperty("touchpadVelocity", 600.0);
        sendTouchpadWheel(window, position, 0, Qt::ScrollEnd);
        QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentY").toReal()
                                 > gestureEndY + 5.0, 2000);


        // Platform momentum replaces the fallback rather than adding to it.
        sendTouchpadWheel(window, position, -2, Qt::ScrollBegin);
        sendTouchpadWheel(window, position, -6, Qt::ScrollMomentum);
        const qreal platformMomentumY = flick->property("contentY").toReal();
        QVERIFY(flick->property("platformMomentumActive").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!flick->property("platformMomentumActive").toBool(),
                                 300);
        QVERIFY(qAbs(flick->property("contentY").toReal() - platformMomentumY) <= 1.0);
    }

    void recognisesAContentBlockLine() {
        Backend::ContentBlock block;

        QVERIFY(Backend::contentBlockOnLine(QStringLiteral("notes.txt"), &block));
        QCOMPARE(block.path, QStringLiteral("notes.txt"));
        QVERIFY(block.caption.isEmpty());

        QVERIFY(Backend::contentBlockOnLine(
            QStringLiteral("figures/diagram.png \"Figure 1\""), &block));
        QCOMPARE(block.path, QStringLiteral("figures/diagram.png"));
        QCOMPARE(block.caption, QStringLiteral("Figure 1"));

        QVERIFY(Backend::contentBlockOnLine(
            QStringLiteral("data.csv (Quarterly figures)"), &block));
        QCOMPARE(block.caption, QStringLiteral("Quarterly figures"));

        // Prose that merely mentions a file is not a block.
        QVERIFY(!Backend::contentBlockOnLine(
            QStringLiteral("Open notes.txt and read it."), nullptr));
        // Neither is a link, a URL, or a path out of the folder.
        QVERIFY(!Backend::contentBlockOnLine(QStringLiteral("![](a.png)"), nullptr));
        QVERIFY(!Backend::contentBlockOnLine(
            QStringLiteral("https://example.com/a.png"), nullptr));
        QVERIFY(!Backend::contentBlockOnLine(QStringLiteral("/etc/passwd.txt"), nullptr));
        QVERIFY(!Backend::contentBlockOnLine(QStringLiteral("~/secrets.txt"), nullptr));
        QVERIFY(!Backend::contentBlockOnLine(QStringLiteral("no extension"), nullptr));
    }

    void embedsContentBlocksWhenRendering() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString root = folder.path();
        QVERIFY(QDir().mkpath(root + QStringLiteral("/parts")));

        auto write = [&](const QString &relative, const QByteArray &contents) {
            QFile file(root + QLatin1Char('/') + relative);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(contents);
        };
        write(QStringLiteral("parts/intro.txt"), "Borrowed prose.\n");
        write(QStringLiteral("snippet.py"), "def hello():\n    return 1\n");
        write(QStringLiteral("data.csv"), "Quarter,Revenue\nQ1,\"1,200\"\nQ2,1400\n");
        write(QStringLiteral("chart.png"), "not really a png");

        const QString source = QStringLiteral(
            "# Report\n\nparts/intro.txt\n\nsnippet.py\n\n"
            "data.csv (Quarterly figures)\n\nchart.png \"Figure 1\"\n");
        const QString rendered = Backend::expandContentBlocks(source, root);

        // Prose comes in as itself.
        QVERIFY(rendered.contains(QStringLiteral("Borrowed prose.")));
        QVERIFY(!rendered.contains(QStringLiteral("parts/intro.txt")));

        // Code arrives fenced under its own extension, so it is highlighted.
        QVERIFY(rendered.contains(QStringLiteral("```py\ndef hello():")));

        // A CSV becomes a table, with the quoted comma kept inside its cell.
        QVERIFY(rendered.contains(QStringLiteral("| Quarter | Revenue |")));
        QVERIFY(rendered.contains(QStringLiteral("| --- | --- |")));
        QVERIFY(rendered.contains(QStringLiteral("| Q1 | 1,200 |")));
        QVERIFY(rendered.contains(QStringLiteral("*Quarterly figures*")));

        // An image becomes an image, with its caption as the alt text.
        QVERIFY(rendered.contains(QStringLiteral("![Figure 1](file://")));
    }

    void leavesContentBlocksAloneWhenItShould() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString root = folder.path();
        {
            QFile file(root + QStringLiteral("/notes.txt"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("inlined\n");
        }

        // A path that climbs out of the document's folder is not followed,
        // however it is spelled.
        const QString escape = QStringLiteral("../../../../etc/hosts\n");
        QCOMPARE(Backend::expandContentBlocks(escape, root), escape);

        // A file that is not there stays as the writer typed it.
        const QString missing = QStringLiteral("nothing-here.txt\n");
        QCOMPARE(Backend::expandContentBlocks(missing, root), missing);

        // Inside a fence a path is code, not a block.
        const QString fenced =
            QStringLiteral("```\nnotes.txt\n```\n");
        QCOMPARE(Backend::expandContentBlocks(fenced, root), fenced);

        // A line with prose around it is prose.
        const QString surrounded = QStringLiteral("See below\nnotes.txt\nand above\n");
        QCOMPARE(Backend::expandContentBlocks(surrounded, root), surrounded);

        // An untitled draft has no folder, so nothing resolves.
        QCOMPARE(Backend::expandContentBlocks(QStringLiteral("notes.txt\n"), QString()),
                 QStringLiteral("notes.txt\n"));

        // And the real thing still works from the same folder.
        QVERIFY(Backend::expandContentBlocks(QStringLiteral("notes.txt\n"), root)
                    .contains(QStringLiteral("inlined")));
    }

    void assemblesSiblingMarkdownInFilenameOrder() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString root = folder.path();
        {
            QFile later(root + QStringLiteral("/02-b.md"));
            QVERIFY(later.open(QIODevice::WriteOnly | QIODevice::Text));
            later.write("bravo words\n");
            QFile earlier(root + QStringLiteral("/01-a.md"));
            QVERIFY(earlier.open(QIODevice::WriteOnly | QIODevice::Text));
            earlier.write("alpha words\n");
        }

        const QVariantMap assembled = Backend::assembleFolder(root);
        QVERIFY(assembled.value(QStringLiteral("ok")).toBool());
        QCOMPARE(assembled.value(QStringLiteral("markdown")).toString(),
                 QStringLiteral("alpha words\n\nbravo words"));
        QCOMPARE(assembled.value(QStringLiteral("wordCount")).toInt(),
                 Backend::countWords(QStringLiteral("alpha words"))
                     + Backend::countWords(QStringLiteral("bravo words")));
        const QVariantList roots = assembled.value(QStringLiteral("roots")).toList();
        QCOMPARE(roots.size(), 2);
        QCOMPARE(roots.at(0).toMap().value(QStringLiteral("fileName")).toString(),
                 QStringLiteral("01-a.md"));
        QCOMPARE(roots.at(1).toMap().value(QStringLiteral("fileName")).toString(),
                 QStringLiteral("02-b.md"));
        const QString suggested =
            assembled.value(QStringLiteral("suggestedSaveUrl")).toUrl().toLocalFile();
        const QString canonicalFolder = QDir(root).canonicalPath();
        QCOMPARE(QFileInfo(suggested).absolutePath(), QFileInfo(canonicalFolder).absolutePath());
        QCOMPARE(QFileInfo(suggested).fileName(),
                 QFileInfo(canonicalFolder).fileName() + QStringLiteral(".md"));
    }

    void assemblesIncludedChaptersOnceThroughTheMaster() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString root = folder.path();
        {
            QFile a(root + QStringLiteral("/01-a.md"));
            QVERIFY(a.open(QIODevice::WriteOnly | QIODevice::Text));
            a.write("alpha words\n");
            QFile b(root + QStringLiteral("/02-b.md"));
            QVERIFY(b.open(QIODevice::WriteOnly | QIODevice::Text));
            b.write("bravo words\n");
            QFile book(root + QStringLiteral("/book.md"));
            QVERIFY(book.open(QIODevice::WriteOnly | QIODevice::Text));
            book.write("01-a.md\n\n02-b.md\n");
        }

        const QString bookText = QStringLiteral("01-a.md\n\n02-b.md\n");
        const QVariantMap assembled = Backend::assembleFolder(root);
        QVERIFY(assembled.value(QStringLiteral("ok")).toBool());
        QCOMPARE(assembled.value(QStringLiteral("markdown")).toString(),
                 Backend::expandContentBlocks(bookText, root).trimmed());
        QCOMPARE(assembled.value(QStringLiteral("markdown")).toString().count(
                     QStringLiteral("alpha words")),
                 1);
        QCOMPARE(assembled.value(QStringLiteral("markdown")).toString().count(
                     QStringLiteral("bravo words")),
                 1);
        const QVariantList roots = assembled.value(QStringLiteral("roots")).toList();
        QCOMPARE(roots.size(), 1);
        QCOMPARE(roots.at(0).toMap().value(QStringLiteral("fileName")).toString(),
                 QStringLiteral("book.md"));
    }

    void assembleUsesUnsavedTabTextThenDiskAfterReload() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString aPath = folder.filePath(QStringLiteral("01-a.md"));
        const QString bPath = folder.filePath(QStringLiteral("02-b.md"));
        {
            QFile a(aPath);
            QVERIFY(a.open(QIODevice::WriteOnly | QIODevice::Text));
            a.write("disk a\n");
            QFile b(bPath);
            QVERIFY(b.open(QIODevice::WriteOnly | QIODevice::Text));
            b.write("bravo words\n");
        }

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        backend.open(QUrl::fromLocalFile(aPath));
        QVERIFY(backend.canAssembleThisFolder());
        document->textDocument()->setPlainText(QStringLiteral("typed a"));
        QVERIFY(backend.editorTextChanged());

        const QVariantMap dirty = backend.assembleThisFolder();
        QVERIFY(dirty.value(QStringLiteral("ok")).toBool());
        QVERIFY(dirty.value(QStringLiteral("usesUnsavedWork")).toBool());
        QVERIFY(dirty.value(QStringLiteral("markdown")).toString().contains(
            QStringLiteral("typed a")));
        QVERIFY(!dirty.value(QStringLiteral("markdown")).toString().contains(
            QStringLiteral("disk a")));

        backend.reloadFromDisk();
        const QVariantMap clean = backend.assembleThisFolder();
        QVERIFY(clean.value(QStringLiteral("ok")).toBool());
        QVERIFY(!clean.value(QStringLiteral("usesUnsavedWork")).toBool());
        QVERIFY(clean.value(QStringLiteral("markdown")).toString().contains(
            QStringLiteral("disk a")));
        QVERIFY(!clean.value(QStringLiteral("markdown")).toString().contains(
            QStringLiteral("typed a")));
    }

    void untitledDocumentCannotAssembleAFolder() {
        Backend backend;
        QVERIFY(attachTextEdit(&backend));
        QVERIFY(!backend.canAssembleThisFolder());
        QVERIFY(!backend.assembleThisFolder().value(QStringLiteral("ok")).toBool());
    }

    void savesAssembledMarkdownAndPdf() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString root = folder.path();
        {
            QFile a(root + QStringLiteral("/01-a.md"));
            QVERIFY(a.open(QIODevice::WriteOnly | QIODevice::Text));
            a.write("alpha words\n");
        }

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        backend.open(QUrl::fromLocalFile(root + QStringLiteral("/01-a.md")));

        const QVariantMap assembled = backend.assembleThisFolder();
        QVERIFY(assembled.value(QStringLiteral("ok")).toBool());
        const QString markdown = assembled.value(QStringLiteral("markdown")).toString();
        const QUrl mdUrl = assembled.value(QStringLiteral("suggestedSaveUrl")).toUrl();
        QVERIFY(backend.saveAssembledMarkdown(mdUrl, markdown));
        QFile saved(mdUrl.toLocalFile());
        QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(saved.readAll()), markdown);

        const QUrl pdfUrl = assembled.value(QStringLiteral("suggestedPdfUrl")).toUrl();
        QVERIFY(backend.saveAssembledPdf(pdfUrl, markdown));
        QVERIFY(QFileInfo(pdfUrl.toLocalFile()).size() > 0);
    }

    void showsTheVersionAndCommitInAbout() {
        // Both are baked in at build time, so an unknown here means the build
        // lost track of what it was built from.
        QVERIFY(!Backend::appVersion().isEmpty());

        // Read from version.pri rather than written here as well: a release
        // bumps one number, and a test that hardcodes it turns every release
        // into a failing build.
        const QString versionPri = QFINDTESTDATA("../version.pri");
        QVERIFY(!versionPri.isEmpty());
        QFile pri(versionPri);
        QVERIFY(pri.open(QIODevice::ReadOnly | QIODevice::Text));
        QString declared;
        while (!pri.atEnd()) {
            const QString line = QString::fromUtf8(pri.readLine()).trimmed();
            if (line.startsWith(QStringLiteral("VERSION"))) {
                declared = line.section(QLatin1Char('='), 1).trimmed();
                break;
            }
        }
        QVERIFY2(!declared.isEmpty(), "version.pri declares no VERSION");
        QCOMPARE(Backend::appVersion(), declared);
        QVERIFY(!Backend::appCommit().isEmpty());
        QVERIFY2(Backend::appCommit() != QStringLiteral("unknown"),
                 "the build did not record a commit");

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *about = window->findChild<QObject *>(QStringLiteral("aboutDialog"));
        QVERIFY(about);
        QVERIFY(QMetaObject::invokeMethod(about, "open"));

        auto text = [&](const char *name) {
            QObject *item = window->findChild<QObject *>(QString::fromLatin1(name));
            return item ? item->property("text").toString() : QString();
        };
        QCOMPARE(text("aboutName"), QStringLiteral("Omawrite"));
        QCOMPARE(text("aboutVersion"),
                 QStringLiteral("Version ") + Backend::appVersion());
        // The commit reads as itself, and carries a link to the page it can be
        // read on when there is one.
        QVERIFY(text("aboutCommit").contains(Backend::appCommit()));

        const QUrl commitUrl = Backend::appCommitUrl();
        if (Backend::appCommit().endsWith(QLatin1Char('+'))) {
            // A modified tree links nowhere: the page would not be this code.
            QVERIFY(commitUrl.isEmpty());
        } else {
            QCOMPARE(commitUrl.toString(),
                     QStringLiteral("https://github.com/frivas/omawrite/commit/")
                         + Backend::appCommit());
            QVERIFY(text("aboutCommit").contains(commitUrl.toString()));
        }

        // The mark is really in the binary, not a path that resolves to
        // nothing at runtime.
        QObject *icon = window->findChild<QObject *>(QStringLiteral("aboutIcon"));
        QVERIFY(icon);
        QVERIFY(QFile::exists(QStringLiteral(":/omawrite-icon.png")));
        QTRY_COMPARE(icon->property("status").toInt(), 1);  // Image.Ready
    }

    void findsTheFencesToDrawAPanelBehind() {
        Backend backend;
        QQuickTextDocument *doc = attachTextEdit(&backend);
        QVERIFY(doc);

        const QString text = QStringLiteral(
            "Prose.\n\n```python\ndef hello():\n    return 1\n```\n\nMore.\n\n"
            "```\nplain\n```\n");
        doc->textDocument()->setPlainText(text);
        backend.editorTextChanged();

        const QVariantList ranges = backend.fencedRanges();
        QCOMPARE(ranges.size(), 2);

        // Each range spans its opening fence through its closing one.
        const QVariantMap first = ranges.at(0).toMap();
        QCOMPARE(text.mid(first.value(QStringLiteral("start")).toInt(), 9),
                 QStringLiteral("```python"));
        const int firstEnd = first.value(QStringLiteral("end")).toInt();
        QCOMPARE(text.mid(firstEnd - 3, 3), QStringLiteral("```"));

        const QVariantMap second = ranges.at(1).toMap();
        QVERIFY(second.value(QStringLiteral("start")).toInt() > firstEnd);

        // A fence left open still wants a panel, to the end of the document.
        doc->textDocument()->setPlainText(QStringLiteral("```python\nunclosed\n"));
        backend.editorTextChanged();
        const QVariantList unclosed = backend.fencedRanges();
        QCOMPARE(unclosed.size(), 1);
        QCOMPARE(unclosed.at(0).toMap().value(QStringLiteral("end")).toInt(),
                 doc->textDocument()->characterCount() - 1);

        // No fences, no panels.
        doc->textDocument()->setPlainText(QStringLiteral("just prose\n"));
        backend.editorTextChanged();
        QCOMPARE(backend.fencedRanges().size(), 0);
    }

    // Checked on the pixels of a shown window, because the editor is where
    // this went wrong before: the document carried a block background, every
    // format assertion passed, and Qt Quick's TextEdit never painted it. Only
    // the character background showed, which is what made code look striped.
    void drawsThePanelBehindFencedCodeInTheEditor() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->setProperty("text",
            QStringLiteral("Prose.\n\n```python\ndef hello():\n    x = 1\n```\n\nAfter.\n"));
        QTRY_COMPARE(editor->property("fencedRanges").toList().size(), 1);
        QTest::qWait(300);

        const QImage shot = window->grabWindow();
        QVERIFY(!shot.isNull());
        const QColor panel(backend.themeCodeBackground());
        QVERIFY(panel.isValid());

        auto isPanel = [&](const QColor &pixel) {
            return qAbs(pixel.red() - panel.red()) <= 4
                && qAbs(pixel.green() - panel.green()) <= 4
                && qAbs(pixel.blue() - panel.blue()) <= 4;
        };

        // Count panel pixels per row, so the panel can be found without
        // knowing where the window put it.
        QList<int> panelRows;
        for (int y = 0; y < shot.height(); ++y) {
            int run = 0;
            for (int x = 0; x < shot.width(); ++x) {
                if (isPanel(shot.pixelColor(x, y)))
                    ++run;
            }
            // A panel row is most of the width; a stray glyph pixel is not.
            if (run > shot.width() / 2)
                panelRows << y;
        }

        // Four lines of fence, at whatever the line height works out to, is
        // far more than the couple of rows a character background would give.
        QVERIFY2(panelRows.size() > 40,
                 qPrintable(QStringLiteral("only %1 rows carried a panel")
                                .arg(panelRows.size())));

        // Contiguous: one panel, not a stripe per line with gaps between.
        QCOMPARE(panelRows.last() - panelRows.first() + 1, panelRows.size());

        // And the prose above it is not on a panel.
        QVERIFY(panelRows.first() > 0);
        int proseRun = 0;
        for (int x = 0; x < shot.width(); ++x) {
            if (isPanel(shot.pixelColor(x, panelRows.first() - 4)))
                ++proseRun;
        }
        QVERIFY(proseRun < shot.width() / 2);
    }

    // What is printed is what is rendered, so this covers Save as PDF too.
    // Checked by drawing the page and reading the pixels rather than trusting
    // the format flags: the flags were set before and it still looked wrong.
    void printsCodeOnAPanelThatCoversTheLine() {
        QQmlEngine engine;
        QScopedPointer<QObject> harness(makeRenderHarness(engine));
        QVERIFY(harness);

        Backend backend;
        QTextDocument *rendered = renderThrough(backend, harness.data(),
            QStringLiteral("Prose above.\n\n```python\ndef hello():\n    return 1\n```\n"));
        QVERIFY(rendered);

        QTextBlock code;
        for (QTextBlock block = rendered->begin(); block.isValid(); block = block.next()) {
            if (block.text().contains(QStringLiteral("def hello")))
                code = block;
        }
        QVERIFY(code.isValid());
        QVERIFY2(code.blockFormat().background().style() != Qt::NoBrush,
                 "the printed code block carries no panel");

        // Draw it and look: the panel must reach past the end of the text,
        // which is exactly what a character background fails to do.
        rendered->setTextWidth(600);
        QImage page(600, int(rendered->size().height()) + 8,
                    QImage::Format_ARGB32_Premultiplied);
        page.fill(Qt::white);
        QPainter painter(&page);
        rendered->drawContents(&painter);
        painter.end();

        const QAbstractTextDocumentLayout *layout = rendered->documentLayout();
        const QRectF codeRect = layout->blockBoundingRect(code);
        const int y = int(codeRect.center().y());
        const QColor panel(backend.themeCodeBackground());
        QVERIFY(panel.isValid());

        // Rendering antialiases, so the panel is compared with a tolerance --
        // and against the page, which is what it must not be.
        auto isPanel = [&](int x) {
            const QColor pixel = page.pixelColor(x, y);
            return qAbs(pixel.red() - panel.red()) <= 3
                && qAbs(pixel.green() - panel.green()) <= 3
                && qAbs(pixel.blue() - panel.blue()) <= 3;
        };

        // Counted across the row rather than sampled at two points: the code
        // is highlighted now, so a fixed point can land on a keyword.
        int panelRun = 0;
        for (int x = 0; x < page.width(); ++x) {
            if (isPanel(x))
                ++panelRun;
        }
        QVERIFY2(panelRun > page.width() / 2,
                 qPrintable(QStringLiteral("only %1 of %2 pixels were panel")
                                .arg(panelRun).arg(page.width())));

        // Well past "def hello():", where a character background would have
        // stopped and left the page showing through.
        QVERIFY2(isPanel(560), qPrintable(page.pixelColor(560, y).name()));
        // The page above the block is not the panel, or this proves nothing.
        QVERIFY(page.pixelColor(560, 2) != panel);
    }

    // Printing is not previewing: paper is white however the app is themed,
    // so the panel behind code comes from the light palette. The first version
    // of this printed a near-black slab with black text on it.
    void printsCodeOnALightPanelWhateverTheTheme() {
        QQmlEngine engine;
        QScopedPointer<QObject> harness(makeRenderHarness(engine));
        QVERIFY(harness);

        Backend backend;
        backend.setDarkMode(true);
        const QString source =
            QStringLiteral("Prose.\n\n```python\ndef hello():\n    return 1\n```\n\nAfter.\n");

        QTextDocument printed;
        printed.setMarkdown(source);
        backend.styleRenderedDocumentForTest(&printed, true);

        QTextBlock code;
        int codeBlocks = 0;
        for (QTextBlock block = printed.begin(); block.isValid(); block = block.next()) {
            if (block.blockFormat().background().style() != Qt::NoBrush) {
                ++codeBlocks;
                if (!code.isValid())
                    code = block;
            }
        }
        QCOMPARE(codeBlocks, 2);
        QVERIFY(code.isValid());

        // Light enough to read black text on, whatever the editor is set to.
        const QColor panel = code.blockFormat().background().color();
        QVERIFY2(panel.lightness() > 200,
                 qPrintable(QStringLiteral("printed panel was %1").arg(panel.name())));

        // The dark theme is still what the preview would use, so this is a
        // real difference rather than the app having no theme.
        QTextDocument previewed;
        previewed.setMarkdown(source);
        backend.styleRenderedDocumentForTest(&previewed, false);
        QColor previewPanel;
        for (QTextBlock block = previewed.begin(); block.isValid(); block = block.next()) {
            if (block.blockFormat().background().style() != Qt::NoBrush) {
                previewPanel = block.blockFormat().background().color();
                break;
            }
        }
        QVERIFY(previewPanel.isValid());
        QVERIFY(previewPanel.lightness() < 100);

        // A run of code opens and closes with a gap, so the panel does not run
        // straight into the prose above and below it.
        QVERIFY(code.blockFormat().topMargin() > 0);
        QCOMPARE(code.blockFormat().bottomMargin(), qreal(0));
        QTextBlock last = code.next();
        QVERIFY(last.isValid());
        QVERIFY(last.blockFormat().bottomMargin() > 0);
        QCOMPARE(last.blockFormat().topMargin(), qreal(0));

        // And it is set in a monospaced face, whatever the prose is in.
        QCOMPARE(code.begin().fragment().charFormat().fontFamilies()
                     .toStringList().value(0),
                 QStringLiteral("iA Writer Mono S"));
    }

    // The rendered page had a panel and a monospaced face but no colour at
    // all: the tokeniser only ever ran in the editor's highlighter. Reading
    // the same code highlighted in one view and flat in the other reads as two
    // different documents.
    void highlightsCodeInTheRenderedPageToo() {
        QQmlEngine engine;
        QScopedPointer<QObject> harness(makeRenderHarness(engine));
        QVERIFY(harness);

        Backend backend;
        const QString source = QStringLiteral(
            "```python\ndef hello():\n    return \"hi\"  # a comment\n```\n");

        QTextDocument rendered;
        rendered.setMarkdown(source);
        backend.styleRenderedDocumentForTest(&rendered, false);

        // Collect every colour the code is drawn in.
        QSet<QString> colours;
        bool sawItalic = false;
        bool sawBold = false;
        for (QTextBlock block = rendered.begin(); block.isValid(); block = block.next()) {
            if (block.blockFormat().background().style() == Qt::NoBrush)
                continue;
            for (auto it = block.begin(); it != block.end(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid())
                    continue;
                colours.insert(fragment.charFormat().foreground().color().name());
                sawItalic = sawItalic || fragment.charFormat().fontItalic();
                sawBold = sawBold
                    || fragment.charFormat().fontWeight() >= QFont::DemiBold;
                // And every fragment is monospaced, colour or not.
                QCOMPARE(fragment.charFormat().fontFamilies().toStringList().value(0),
                         QStringLiteral("iA Writer Mono S"));
            }
        }

        // def and return are keywords, "hi" a string, the trailing # a comment:
        // several distinct colours, not one.
        QVERIFY2(colours.size() >= 3,
                 qPrintable(QStringLiteral("only %1 colour(s) in the rendered code")
                                .arg(colours.size())));
        QVERIFY(sawItalic);   // the comment
        QVERIFY(sawBold);     // the keywords

        // Printing takes the light palette, so its colours differ from a dark
        // preview's rather than being handed straight through.
        backend.setDarkMode(true);
        QTextDocument previewed;
        previewed.setMarkdown(source);
        backend.styleRenderedDocumentForTest(&previewed, false);

        QTextDocument printed;
        printed.setMarkdown(source);
        backend.styleRenderedDocumentForTest(&printed, true);

        auto firstColour = [](const QTextDocument &document) {
            for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
                if (block.blockFormat().background().style() == Qt::NoBrush)
                    continue;
                for (auto it = block.begin(); it != block.end(); ++it) {
                    if (it.fragment().isValid())
                        return it.fragment().charFormat().foreground().color();
                }
            }
            return QColor();
        };
        QVERIFY(firstColour(printed).isValid());
        QVERIFY(firstColour(printed) != firstColour(previewed));
    }

    // The panel has to be visibly grey, not a shade that reads as the page.
    // It is easy to derive one so subtle it looks like the feature is missing.
    void keepsTheCodePanelVisibleAgainstThePage() {
        auto separation = [](const QColor &panel, const QColor &page) {
            return qAbs(panel.lightness() - page.lightness());
        };

        for (bool dark : {false, true}) {
            Backend backend;
            backend.setDarkMode(dark);
            const QColor panel(backend.themeCodeBackground());
            const QColor page(backend.themeBackground());
            QVERIFY(panel.isValid());
            QVERIFY2(separation(panel, page) >= 15,
                     qPrintable(QStringLiteral("%1 on %2 is too close to see")
                                    .arg(panel.name(), page.name())));
            // And not so heavy it becomes a slab.
            QVERIFY(separation(panel, page) <= 60);
        }

        // The printed panel too, against white paper.
        Backend backend;
        QTextDocument printed;
        printed.setMarkdown(QStringLiteral("```python\ndef hello():\n    return 1\n```\n"));
        backend.styleRenderedDocumentForTest(&printed, true);
        QColor printedPanel;
        for (QTextBlock block = printed.begin(); block.isValid(); block = block.next()) {
            if (block.blockFormat().background().style() != Qt::NoBrush) {
                printedPanel = block.blockFormat().background().color();
                break;
            }
        }
        QVERIFY(printedPanel.isValid());
        QVERIFY2(separation(printedPanel, QColor(Qt::white)) >= 15,
                 qPrintable(printedPanel.name()));
        QVERIFY(printedPanel.lightness() > 200);
    }

    // A long line of code used to run off the page: Qt marks code blocks
    // non-breakable, so it was neither wrapped nor inside the panel, which
    // only ever spans the measure. It was cut at the paper's edge.
    void wrapsLongCodeLinesInsideThePage() {
        Backend backend;
        const QString longLine = QStringLiteral("open ") + QString(300, QLatin1Char('x'));
        QTextDocument printed;
        printed.setMarkdown(QStringLiteral("```bash\n") + longLine
                            + QStringLiteral("\n```\n"));
        backend.styleRenderedDocumentForTest(&printed, true);

        QTextBlock code;
        for (QTextBlock block = printed.begin(); block.isValid(); block = block.next()) {
            if (block.text().contains(QStringLiteral("xxx")))
                code = block;
        }
        QVERIFY(code.isValid());
        QVERIFY2(!code.blockFormat().nonBreakableLines(),
                 "code is still non-breakable, so a long line runs off the page");

        // Laid out at a page's width, the line takes more than one row and
        // nothing reaches past the measure.
        printed.setTextWidth(480);
        const QAbstractTextDocumentLayout *layout = printed.documentLayout();
        const QRectF codeRect = layout->blockBoundingRect(code);
        QVERIFY2(code.layout()->lineCount() > 1,
                 qPrintable(QStringLiteral("the long line stayed on %1 row(s)")
                                .arg(code.layout()->lineCount())));
        QVERIFY2(codeRect.right() <= 481.0,
                 qPrintable(QStringLiteral("code reached %1 past a 480 measure")
                                .arg(codeRect.right())));
    }

    void printsTheBodyAtAReadableSizeOnPaper() {
        // The editor's size is in pixels and the page's is in points; reading
        // one as the other made a 20px editor print at 20pt.
        Backend backend;
        QCOMPARE(backend.printFontPointSize(), 11.0);

        QSignalSpy spy(&backend, &Backend::printFontPointSizeChanged);
        backend.setPrintFontPointSize(9.5);
        QCOMPARE(spy.count(), 1);
        backend.setPrintFontPointSize(1);
        QCOMPARE(backend.printFontPointSize(), 6.0);
        backend.setPrintFontPointSize(500);
        QCOMPARE(backend.printFontPointSize(), 32.0);

        // And the margin leaves a usable measure rather than a column.
        Backend fresh;
        QCOMPARE(fresh.printMarginMm(), 15.0);
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

    void revertsEmptyDocumentToCleanState() {
        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        QVERIFY(!backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        quickDocument->textDocument()->setPlainText(QString());
        QVERIFY(backend.editorTextChanged());
        QVERIFY(!backend.modified());
    }

    void revertsSavedDocumentToCleanState() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("doc.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(file.write("hello"), qint64(5));
        file.close();

        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        backend.open(QUrl::fromLocalFile(path));
        QVERIFY(!backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello world"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(!backend.modified());
    }

    void keepsADocumentUnsavedAfterItsFileDisappears() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("gone.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(file.write("hello"), qint64(5));
        file.close();

        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);
        backend.open(QUrl::fromLocalFile(path));
        QVERIFY(!backend.modified());

        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(externalChangeSpy.count(), 1);

        // The writer can dismiss the dialog without picking a version, so the
        // text the deleted file used to hold is not a saved state to return to.
        quickDocument->textDocument()->setPlainText(QStringLiteral("hello world"));
        QVERIFY(backend.editorTextChanged());
        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        // Nor is emptying it: an empty document only reads as clean while there
        // is no file behind it.
        quickDocument->textDocument()->setPlainText(QString());
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
    }

    void opensASecondDocumentAsATab() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString first = directory.filePath(QStringLiteral("one.md"));
        const QString second = directory.filePath(QStringLiteral("two.md"));
        {
            QFile a(first);
            QVERIFY(a.open(QIODevice::WriteOnly | QIODevice::Text));
            a.write("alpha");
            QFile b(second);
            QVERIFY(b.open(QIODevice::WriteOnly | QIODevice::Text));
            b.write("beta");
        }

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        QCOMPARE(backend.tabCount(), 1);
        QCOMPARE(backend.tabTitle(), QStringLiteral("Untitled"));

        backend.open(QUrl::fromLocalFile(first));
        QCOMPARE(backend.tabCount(), 1);
        QCOMPARE(backend.fileName(), QStringLiteral("one.md"));
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("alpha"));

        backend.open(QUrl::fromLocalFile(second));
        QCOMPARE(backend.tabCount(), 2);
        QCOMPARE(backend.activeTabIndex(), 1);
        QCOMPARE(backend.fileName(), QStringLiteral("two.md"));
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("beta"));

        backend.setActiveTab(0);
        QCOMPARE(backend.fileName(), QStringLiteral("one.md"));
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("alpha"));
    }

    void focusesAnAlreadyOpenPathInsteadOfDuplicatingIt() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("same.md"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write("once");
        }

        Backend backend;
        QVERIFY(attachTextEdit(&backend));
        backend.open(QUrl::fromLocalFile(path));
        backend.newTab();
        QCOMPARE(backend.tabCount(), 2);
        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(backend.tabCount(), 2);
        QCOMPARE(backend.activeTabIndex(), 0);
    }

    void numbersUntitledTabsAcrossAWindow() {
        Backend backend;
        QVERIFY(attachTextEdit(&backend));
        QCOMPARE(backend.tabTitle(), QStringLiteral("Untitled"));
        backend.newTab();
        QCOMPARE(backend.tabTitle(), QStringLiteral("Untitled 2"));
        backend.setActiveTab(0);
        QCOMPARE(backend.tabTitle(), QStringLiteral("Untitled"));
    }

    void closingTheLastTabAsksTheWindowToClose() {
        Backend backend;
        QVERIFY(attachTextEdit(&backend));
        QSignalSpy closeSpy(&backend, &Backend::closeWindowRequested);
        QVERIFY(backend.closeTab(0));
        QCOMPARE(closeSpy.count(), 1);
        QCOMPARE(backend.tabCount(), 0);
    }

    void restoresUntitledTabsFromTheSessionFile() {
        {
            Backend writer;
            QQuickTextDocument *document = attachTextEdit(&writer);
            QVERIFY(document);
            document->textDocument()->setPlainText(QStringLiteral("still here"));
            QVERIFY(writer.editorTextChanged());
            writer.persistSession();
        }

        Backend reader;
        QQuickTextDocument *document = attachTextEdit(&reader);
        QVERIFY(document);
        QCOMPARE(reader.tabTitle(), QStringLiteral("Untitled"));
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("still here"));
        QVERIFY(reader.modified());
    }

    void movesATabOntoAnotherWindow() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("moved.md"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write("cargo");
        }

        Backend source;
        QVERIFY(attachTextEdit(&source));
        source.open(QUrl::fromLocalFile(path));
        source.newTab();
        QCOMPARE(source.tabCount(), 2);

        Backend destination;
        QVERIFY(attachTextEdit(&destination));
        destination.adoptTabFrom(&source, 0);
        QCOMPARE(destination.tabCount(), 2);
        QCOMPARE(destination.fileName(), QStringLiteral("moved.md"));
        QCOMPARE(source.tabCount(), 1);
    }

    void reordersTabsInTheSameWindow() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString first = directory.filePath(QStringLiteral("first.md"));
        const QString second = directory.filePath(QStringLiteral("second.md"));
        {
            QFile a(first);
            QVERIFY(a.open(QIODevice::WriteOnly | QIODevice::Text));
            a.write("one");
            QFile b(second);
            QVERIFY(b.open(QIODevice::WriteOnly | QIODevice::Text));
            b.write("two");
        }

        Backend backend;
        QVERIFY(attachTextEdit(&backend));
        backend.open(QUrl::fromLocalFile(first));
        backend.open(QUrl::fromLocalFile(second));
        QCOMPARE(backend.activeTabIndex(), 1);
        QCOMPARE(backend.fileName(), QStringLiteral("second.md"));

        backend.moveTab(1, 0);
        QCOMPARE(backend.activeTabIndex(), 0);
        QCOMPARE(backend.fileName(), QStringLiteral("second.md"));
        backend.setActiveTab(1);
        QCOMPARE(backend.fileName(), QStringLiteral("first.md"));
    }

    void newWindowAsksForAnotherWritingWindow() {
        Backend backend;
        QQmlEngine engine;
        QScopedPointer<QObject> root(openWritingWindow(&backend, &engine));
        QVERIFY(root);
        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        window->requestActivate();
        QTRY_VERIFY(window->isActive());

        QSignalSpy spy(&backend, &Backend::newWindowRequested);
        backend.newWindow();
        QCOMPARE(spy.count(), 1);

        QScopedPointer<Backend> second;
        QQmlEngine secondEngine;
        QScopedPointer<QObject> secondRoot;
        QObject::connect(&backend, &Backend::newWindowRequested, this, [&]() {
            second.reset(new Backend);
            secondRoot.reset(openWritingWindow(second.data(), &secondEngine));
        });
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(spy.count(), 2);
        QVERIFY(second);
        QVERIFY(secondRoot);
        QVERIFY(secondRoot->findChild<QQuickItem *>(QStringLiteral("tabStrip")));
        QCOMPARE(Backend::liveWindows().size(), 2);
    }

    void restoresTwoWindowsFromTheSessionFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString first = directory.filePath(QStringLiteral("left.md"));
        const QString second = directory.filePath(QStringLiteral("right.md"));
        {
            QFile a(first);
            QVERIFY(a.open(QIODevice::WriteOnly | QIODevice::Text));
            a.write("left body");
            QFile b(second);
            QVERIFY(b.open(QIODevice::WriteOnly | QIODevice::Text));
            b.write("right body");
        }

        const QJsonObject session{
            {QStringLiteral("windows"),
             QJsonArray{
                 QJsonObject{
                     {QStringLiteral("activeTab"), 0},
                     {QStringLiteral("tabs"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("fileUrl"), QUrl::fromLocalFile(first).toString()},
                          {QStringLiteral("modified"), false},
                      }}},
                 },
                 QJsonObject{
                     {QStringLiteral("activeTab"), 0},
                     {QStringLiteral("tabs"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("fileUrl"), QUrl::fromLocalFile(second).toString()},
                          {QStringLiteral("modified"), false},
                      }}},
                 },
             }},
        };
        QDir().mkpath(QFileInfo(Backend::sessionPath()).absolutePath());
        QFile sessionFile(Backend::sessionPath());
        QVERIFY(sessionFile.open(QIODevice::WriteOnly));
        sessionFile.write(QJsonDocument(session).toJson(QJsonDocument::Compact));
        sessionFile.close();
        QCOMPARE(Backend::storedWindowCount(), 2);

        Backend left;
        QQuickTextDocument *leftDocument = attachTextEdit(&left);
        QVERIFY(leftDocument);
        QCOMPARE(left.fileName(), QStringLiteral("left.md"));
        QCOMPARE(leftDocument->textDocument()->toPlainText(), QStringLiteral("left body"));

        Backend right;
        QQuickTextDocument *rightDocument = attachTextEdit(&right);
        QVERIFY(rightDocument);
        QCOMPARE(right.fileName(), QStringLiteral("right.md"));
        QCOMPARE(rightDocument->textDocument()->toPlainText(), QStringLiteral("right body"));
    }

    void quittingKeepsEveryWindowInTheSession() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString first = directory.filePath(QStringLiteral("keep-a.md"));
        const QString second = directory.filePath(QStringLiteral("keep-b.md"));
        {
            QFile a(first);
            QVERIFY(a.open(QIODevice::WriteOnly | QIODevice::Text));
            a.write("a");
            QFile b(second);
            QVERIFY(b.open(QIODevice::WriteOnly | QIODevice::Text));
            b.write("b");
        }

        {
            Backend left;
            QVERIFY(attachTextEdit(&left));
            left.open(QUrl::fromLocalFile(first));
            Backend right;
            QVERIFY(attachTextEdit(&right));
            right.open(QUrl::fromLocalFile(second));
            Backend::prepareToQuit();
        }

        QCOMPARE(Backend::storedWindowCount(), 2);
        Backend::allowSessionWrites();

        Backend left;
        QVERIFY(attachTextEdit(&left));
        QCOMPARE(left.fileName(), QStringLiteral("keep-a.md"));
        Backend right;
        QVERIFY(attachTextEdit(&right));
        QCOMPARE(right.fileName(), QStringLiteral("keep-b.md"));
    }

    void droppingATabOntoAnotherWindowMovesIt() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("cargo.md"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write("cargo");
        }

        Backend source;
        QQmlEngine sourceEngine;
        QScopedPointer<QObject> sourceRoot(openWritingWindow(&source, &sourceEngine));
        QVERIFY(sourceRoot);
        Backend destination;
        QQmlEngine destEngine;
        QScopedPointer<QObject> destRoot(openWritingWindow(&destination, &destEngine));
        QVERIFY(destRoot);

        auto *sourceWindow = qobject_cast<QQuickWindow *>(sourceRoot.data());
        auto *destWindow = qobject_cast<QQuickWindow *>(destRoot.data());
        QVERIFY(sourceWindow);
        QVERIFY(destWindow);
        sourceWindow->setGeometry(0, 0, 400, 400);
        destWindow->setGeometry(800, 0, 400, 400);
        QCoreApplication::processEvents();
        QVERIFY(!sourceWindow->geometry().contains(destWindow->geometry().center()));
        QVERIFY(destWindow->geometry().contains(destWindow->geometry().center()));

        source.open(QUrl::fromLocalFile(path));
        source.newTab();
        QCOMPARE(source.tabCount(), 2);

        const QPoint destPoint = destWindow->geometry().center();
        source.finishTabDrag(0, destPoint.x(), destPoint.y());
        QCOMPARE(destination.fileName(), QStringLiteral("cargo.md"));
        QCOMPARE(source.tabCount(), 1);
        QCOMPARE(destination.tabCount(), 2);
        QVERIFY(sourceRoot->findChild<QQuickItem *>(QStringLiteral("tabStrip")));
        QVERIFY(destRoot->findChild<QQuickItem *>(QStringLiteral("tabStrip")));
    }

    void droppingATabOffEveryWindowDetachesIt() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("solo.md"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write("alone");
        }

        Backend source;
        QQmlEngine sourceEngine;
        QScopedPointer<QObject> sourceRoot(openWritingWindow(&source, &sourceEngine));
        QVERIFY(sourceRoot);
        auto *sourceWindow = qobject_cast<QQuickWindow *>(sourceRoot.data());
        QVERIFY(sourceWindow);
        sourceWindow->setGeometry(0, 0, 400, 400);
        QCoreApplication::processEvents();

        source.open(QUrl::fromLocalFile(path));
        QSignalSpy detachSpy(&source, &Backend::detachTabRequested);
        source.finishTabDrag(0, -2000, -500);
        QCOMPARE(detachSpy.count(), 1);
        QCOMPARE(detachSpy.at(0).at(0).toInt(), 0);

        Backend created;
        QQmlEngine createdEngine;
        QScopedPointer<QObject> createdRoot(openWritingWindow(&created, &createdEngine));
        QVERIFY(createdRoot);
        QSignalSpy closeSpy(&source, &Backend::closeWindowRequested);
        created.takeDetachedTab(&source, 0);
        QCOMPARE(created.tabCount(), 1);
        QCOMPARE(created.fileName(), QStringLiteral("solo.md"));
        QCOMPARE(source.tabCount(), 0);
        QCOMPARE(closeSpy.count(), 1);
        QTRY_VERIFY(!sourceWindow->isVisible());
    }

    void closingTheLastTabClosesTheWritingWindow() {
        Backend backend;
        QQmlEngine engine;
        QScopedPointer<QObject> root(openWritingWindow(&backend, &engine));
        QVERIFY(root);
        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        QVERIFY(window->isVisible());
        QVERIFY(backend.closeTab(0));
        QTRY_VERIFY(!window->isVisible());
    }

    void closingADirtyTabAsksBeforeDroppingIt() {
        Backend backend;
        QQmlEngine engine;
        QScopedPointer<QObject> root(openWritingWindow(&backend, &engine));
        QVERIFY(root);
        QObject *editor = root->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("keep me"));
        QTRY_VERIFY(backend.modified());

        QVERIFY(QMetaObject::invokeMethod(root.data(), "requestCloseTab",
                                          Q_ARG(QVariant, 0)));
        auto *dialog = root->findChild<QObject *>(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QObject *discard = root->findChild<QObject *>(QStringLiteral("discardUnsavedButton"));
        QVERIFY(discard);
        QVERIFY(QMetaObject::invokeMethod(discard, "clicked"));
        QTRY_COMPARE(backend.tabCount(), 0);
        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        QTRY_VERIFY(!window->isVisible());
    }

    void externalChangeOnABackgroundTabSwitchesToIt() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString first = directory.filePath(QStringLiteral("front.md"));
        const QString second = directory.filePath(QStringLiteral("back.md"));
        {
            QFile a(first);
            QVERIFY(a.open(QIODevice::WriteOnly | QIODevice::Text));
            a.write("front");
            QFile b(second);
            QVERIFY(b.open(QIODevice::WriteOnly | QIODevice::Text));
            b.write("back");
        }

        Backend backend;
        QQmlEngine engine;
        QScopedPointer<QObject> root(openWritingWindow(&backend, &engine));
        QVERIFY(root);
        backend.open(QUrl::fromLocalFile(first));
        backend.open(QUrl::fromLocalFile(second));
        backend.setActiveTab(0);
        QCOMPARE(backend.activeTabIndex(), 0);
        QCOMPARE(backend.fileName(), QStringLiteral("front.md"));

        QSignalSpy spy(&backend, &Backend::externalChangeDetected);
        QFile file(second);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        QCOMPARE(file.write("changed"), qint64(7));
        file.close();
        QTRY_VERIFY(spy.count() >= 1);
        QCOMPARE(backend.activeTabIndex(), 1);
        QCOMPARE(backend.fileName(), QStringLiteral("back.md"));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("externalChangeHeading")));
    }

    void assembleFolderDialogListsTheReceiptOnTheWritingWindow() {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QString path = folder.filePath(QStringLiteral("01-a.md"));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write("alpha words\n");
        }

        Backend backend;
        QQmlEngine engine;
        QScopedPointer<QObject> root(openWritingWindow(&backend, &engine));
        QVERIFY(root);
        backend.open(QUrl::fromLocalFile(path));
        QVERIFY(backend.canAssembleThisFolder());
        QVERIFY(QMetaObject::invokeMethod(root.data(), "openAssembleFolder"));
        auto *dialog = root->findChild<QObject *>(QStringLiteral("assembleFolderDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QCOMPARE(dialog->property("wordCount").toInt(),
                 Backend::countWords(QStringLiteral("alpha words")));
    }

    void tabStripIsOnTheWritingWindow() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QVERIFY(window->findChild<QQuickItem *>(QStringLiteral("tabStrip")));
        QCOMPARE(window->property("title").toString(),
                 QStringLiteral("Untitled - Omawrite"));
        backend.newTab();
        QCOMPARE(window->property("title").toString(),
                 QStringLiteral("Untitled 2 - Omawrite"));
    }

private:
    QObject *openWritingWindow(Backend *backend, QQmlEngine *engine) {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        if (mainQmlPath.isEmpty())
            return nullptr;
        engine->rootContext()->setContextProperty(QStringLiteral("backend"), backend);
        QQmlComponent component(engine, QUrl::fromLocalFile(mainQmlPath));
        if (!component.isReady())
            return nullptr;
        QObject *window = component.create();
        if (!window)
            return nullptr;
        backend->setParentWindow(qobject_cast<QWindow *>(window));
        return window;
    }

    static void sendTouchpadWheel(QQuickWindow *window, const QPointF &position,
                                  int pixelDeltaY, Qt::ScrollPhase phase) {
        QWheelEvent event(position, window->mapToGlobal(position),
                          QPoint(0, pixelDeltaY), QPoint(), Qt::NoButton,
                          Qt::NoModifier, phase, false);
        QCoreApplication::sendEvent(window, &event);
    }

    // Loads a bare TextEdit, attaches its document to the backend, and returns
    // the QQuickTextDocument. The engine and TextEdit are parented to the
    // backend so they outlive the helper call.
    QQuickTextDocument *attachTextEdit(Backend *backend) {
        auto *engine = new QQmlEngine(backend);
        auto *component = new QQmlComponent(engine);
        component->setData(R"QML(
            import QtQuick
            TextEdit { }
        )QML", QUrl::fromLocalFile(QStringLiteral("Harness.qml")));
        if (!component->isReady())
            return nullptr;

        QObject *editor = component->create();
        if (!editor)
            return nullptr;
        editor->setParent(backend);

        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        if (!quickDocument)
            return nullptr;

        backend->attachDocument(quickDocument);
        return quickDocument;
    }

    // Points HOME at a scratch tree holding one colors.toml, and puts it back on the way out.
    struct ScopedTheme {
        explicit ScopedTheme(const QByteArray &palette) {
            const QString themeDirectory = home.path()
                + QStringLiteral("/.local/state/omarchy/current/theme");
            QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
            ok = home.isValid() && QDir().mkpath(themeDirectory)
                && qputenv("HOME", home.path().toUtf8())
                && colorsFile.open(QIODevice::WriteOnly | QIODevice::Text)
                && colorsFile.write(palette) == qint64(palette.size());
        }
        ~ScopedTheme() { qputenv("HOME", originalHome); }

        QTemporaryDir home;
        QByteArray originalHome = qgetenv("HOME");
        bool ok = false;
    };

    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"

#include <QtTest>
#include <QClipboard>
#include <QFont>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>

#include "backend.h"
#include "markdownhighlighter.h"

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_settingsDirectory.isValid());
        // Keep recovery snapshots out of the state directory of the copy of
        // Omawrite the developer is writing in.
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

    void undoesTypingAWordAtATime() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        auto *quickWindow = qobject_cast<QQuickWindow *>(window.data());
        QVERIFY(quickWindow);
        QVERIFY(QTest::qWaitForWindowExposed(quickWindow));

        QQuickItem *editor = window->findChild<QQuickItem *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->forceActiveFocus();
        QCOMPARE(editor->property("text").toString(), QString());

        for (char letter : QByteArray("one two three"))
            QTest::keyClick(quickWindow, letter);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("one two three"));

        // Qt would otherwise merge the whole run of keystrokes into one undo
        // command, so a single Ctrl+Z threw away everything typed and left the
        // caret at the top of the document.
        QTest::keyClick(quickWindow, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("one two "));
        QCOMPARE(editor->property("cursorPosition").toInt(), 8);

        QTest::keyClick(quickWindow, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("one "));
        QCOMPARE(editor->property("cursorPosition").toInt(), 4);

        // And every press of redo puts a word back, rather than stalling on the
        // formatting-only edits that mark where one word ends.
        QTest::keyClick(quickWindow, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("one two "));
        QCOMPARE(editor->property("cursorPosition").toInt(), 8);

        QTest::keyClick(quickWindow, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("one two three"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 13);
    }

    void movesAndEditsWithVimKeys() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        // Motions stay on a character, never on the line break past it.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta\ngamma"), 0,
                        QStringLiteral("$")).cursor, 9);
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 0,
                        QStringLiteral("wl")).cursor, 7);
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 9,
                        QStringLiteral("b")).cursor, 6);
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha, beta"), 0,
                        QStringLiteral("w")).cursor, 5);
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha, beta"), 0,
                        QStringLiteral("W")).cursor, 7);
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 0,
                        QStringLiteral("G")).cursor, 8);
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 10,
                        QStringLiteral("gg")).cursor, 0);
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 0,
                        QStringLiteral("2G")).cursor, 4);
        QCOMPARE(runVim(editor.data(), QStringLiteral("hello there"), 0,
                        QStringLiteral("fh")).cursor, 7);
        QCOMPARE(runVim(editor.data(), QStringLiteral("hello there"), 0,
                        QStringLiteral("th")).cursor, 6);
        QCOMPARE(runVim(editor.data(), QStringLiteral("hello there"), 10,
                        QStringLiteral("Fh")).cursor, 7);

        // A repeated t walks on instead of re-finding the target it already
        // stopped short of, while a fresh t does stop short of an adjacent one.
        QCOMPARE(runVim(editor.data(), QStringLiteral("a.b.c.d"), 0,
                        QStringLiteral("t.")).cursor, 0);
        QCOMPARE(runVim(editor.data(), QStringLiteral("a.b.c.d"), 0,
                        QStringLiteral("t.;")).cursor, 2);
        QCOMPARE(runVim(editor.data(), QStringLiteral("a.b.c.d"), 0,
                        QStringLiteral("t.;;")).cursor, 4);
        QCOMPARE(runVim(editor.data(), QStringLiteral("a.b.c.d"), 0,
                        QStringLiteral("f.;")).cursor, 3);

        // Astral characters step and delete whole, never half a pair.
        const QString emoji = QStringLiteral("a\U0001F600b");
        QCOMPARE(runVim(editor.data(), emoji, 0, QStringLiteral("l")).cursor, 1);
        QCOMPARE(runVim(editor.data(), emoji, 0, QStringLiteral("ll")).cursor, 3);
        QCOMPARE(runVim(editor.data(), emoji, 3, QStringLiteral("h")).cursor, 1);
        QCOMPARE(runVim(editor.data(), emoji, 1, QStringLiteral("x")).text,
                 QStringLiteral("ab"));
        QCOMPARE(runVim(editor.data(), emoji, 1, QStringLiteral("rz")).text,
                 QStringLiteral("azb"));

        // A visual range and the word-end motions count characters too. Both
        // feed operators, so a stray half of a pair would go to disk on the
        // next save rather than stopping at a caret that looks wrong.
        QCOMPARE(runVim(editor.data(), emoji, 0, QStringLiteral("lvd")).text,
                 QStringLiteral("ab"));
        const QString spacedEmoji = QStringLiteral("a\U0001F600 b");
        QCOMPARE(runVim(editor.data(), spacedEmoji, 0, QStringLiteral("le")).cursor, 4);
        QCOMPARE(runVim(editor.data(), spacedEmoji, 0, QStringLiteral("de")).text,
                 QStringLiteral(" b"));
        QCOMPARE(runVim(editor.data(), spacedEmoji, 4, QStringLiteral("ge")).cursor, 1);
        QCOMPARE(runVim(editor.data(), spacedEmoji, 4, QStringLiteral("gex")).text,
                 QStringLiteral("a b"));
        QCOMPARE(runVim(editor.data(), spacedEmoji, 4, QStringLiteral("dge")).text,
                 QStringLiteral("a"));

        // A motion that stops between the halves of a character is as bad as a
        // range that splits one: the caret looks right and the next x takes
        // half a pair. t stops one character short of its target, and gj and
        // gk measure their column out in characters on the line they land on.
        const QString beforeTarget = QStringLiteral("a\U0001F600X");
        QCOMPARE(runVim(editor.data(), beforeTarget, 0, QStringLiteral("tX")).cursor, 1);
        QCOMPARE(runVim(editor.data(), beforeTarget, 0, QStringLiteral("tXx")).text,
                 QStringLiteral("aX"));
        const QString twoEmoji = QStringLiteral("a\U0001F600 b\U0001F600 c");
        QCOMPARE(runVim(editor.data(), twoEmoji, 0, QStringLiteral("t ;")).cursor, 5);
        QCOMPARE(runVim(editor.data(), twoEmoji, 0, QStringLiteral("t ;x")).text,
                 QStringLiteral("a\U0001F600 b c"));
        const QString emojiBelow = QStringLiteral("ab\n\U0001F600z");
        QCOMPARE(runVim(editor.data(), emojiBelow, 1, QStringLiteral("gj")).cursor, 5);
        QCOMPARE(runVim(editor.data(), emojiBelow, 1, QStringLiteral("gjx")).text,
                 QStringLiteral("ab\n\U0001F600"));
        const QString emojiAbove = QStringLiteral("\U0001F600b\nc\U0001F600");
        QCOMPARE(runVim(editor.data(), emojiAbove, 5, QStringLiteral("gk")).cursor, 2);
        QCOMPARE(runVim(editor.data(), emojiAbove, 5, QStringLiteral("gkx")).text,
                 QStringLiteral("\U0001F600\nc\U0001F600"));

        // A text object leaves the caret on its last character, which is a
        // whole character back from the exclusive end rather than one unit.
        QCOMPARE(runVim(editor.data(), QStringLiteral("\U0001F600"), 0,
                        QStringLiteral("viw<Esc>")).cursor, 0);
        QCOMPARE(runVim(editor.data(), QStringLiteral("\U0001F600"), 0,
                        QStringLiteral("viw<Esc>x")).text, QString());

        // What . deletes behind the caret is counted in characters, because it
        // is measured during one insert and spent wherever the repeat lands.
        QCOMPARE(runVim(editor.data(), QStringLiteral("a \U0001F600x"), 1,
                        QStringLiteral("i<Backspace><Esc>wl.")).text,
                 QStringLiteral(" x"));

        // ~ leaves the caret after the run it toggled, and toggling can change
        // that run's length: ß becomes SS and pushes the rest along.
        QCOMPARE(runVim(editor.data(), QStringLiteral("ß\U0001F600x"), 0,
                        QStringLiteral("2~")).cursor, 4);
        QCOMPARE(runVim(editor.data(), QStringLiteral("ß\U0001F600x"), 0,
                        QStringLiteral("2~x")).text, QStringLiteral("SS\U0001F600"));

        // j and k hold the column they started from across a short line.
        QCOMPARE(runVim(editor.data(), QStringLiteral("abcdef\nx\nabcdef"), 4,
                        QStringLiteral("jj")).cursor, 13);

        // Sentences run to the first word after the punctuation, and stop at
        // the end of the paragraph rather than running into the next one.
        const QString prose = QStringLiteral("One thing. Two things! Three?\n\nNext para.");
        QCOMPARE(runVim(editor.data(), prose, 0, QStringLiteral(")")).cursor, 11);
        QCOMPARE(runVim(editor.data(), prose, 0, QStringLiteral("2)")).cursor, 23);
        QCOMPARE(runVim(editor.data(), prose, 23, QStringLiteral("(")).cursor, 11);
        QCOMPARE(runVim(editor.data(), prose, 0, QStringLiteral("3)")).cursor, 30);
        QCOMPARE(runVim(editor.data(), prose, 0, QStringLiteral("d)")).text,
                 QStringLiteral("Two things! Three?\n\nNext para."));

        // ge walks back to the end of the previous word, and takes the
        // caret's own character when an operator is waiting.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 11,
                        QStringLiteral("ge")).cursor, 9);
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 11,
                        QStringLiteral("2ge")).cursor, 4);
        // From the t of "beta" back through the a of "alpha", both ends
        // taken, which leaves the trailing a of "beta" behind.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 8,
                        QStringLiteral("dge")).text, QStringLiteral("alpha"));

        const VimResult deleted = runVim(editor.data(), QStringLiteral("alpha beta gamma"), 0,
                                         QStringLiteral("dw"));
        QCOMPARE(deleted.text, QStringLiteral("beta gamma"));
        QCOMPARE(deleted.cursor, 0);

        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 6,
                        QStringLiteral("d$")).text, QStringLiteral("alpha "));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 0,
                        QStringLiteral("2dd")).text, QStringLiteral("three"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 5,
                        QStringLiteral("dd")).text, QStringLiteral("one"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("abcdef"), 1,
                        QStringLiteral("3x")).text, QStringLiteral("aef"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                        QStringLiteral("J")).text, QStringLiteral("one two"));
        // Nothing below to join to, so the line is left as it is.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one two"), 0,
                        QStringLiteral("J")).text, QStringLiteral("one two"));

        // dw on the last word of a line stops at the line's end rather than
        // dragging the line below up, and an exclusive motion that lands in
        // column one turns linewise from the start of a line.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta\ngamma"), 6,
                        QStringLiteral("dw")).text, QStringLiteral("alpha \ngamma"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\n\ntwo\n\nthree"), 0,
                        QStringLiteral("d}")).text, QStringLiteral("\ntwo\n\nthree"));

        // A line motion with nowhere to go fails its operator instead of
        // taking the line the caret is already on.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 5,
                        QStringLiteral("dj")).text, QStringLiteral("one\ntwo"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                        QStringLiteral("dk")).text, QStringLiteral("one\ntwo"));

        // Counts past the edge of the document stop there.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 0,
                        QStringLiteral("99j")).cursor, 8);
        QCOMPARE(runVim(editor.data(), QStringLiteral("abc"), 0,
                        QStringLiteral("99x")).text, QString());

        // A join after a line that already ends in a space adds no second one.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one \ntwo"), 0,
                        QStringLiteral("J")).text, QStringLiteral("one two"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\n\ntwo"), 0,
                        QStringLiteral("J")).text, QStringLiteral("one\ntwo"));

        // A find that misses leaves both the document and the register alone.
        const VimResult missed = runVim(editor.data(), QStringLiteral("alpha beta"), 0,
                                        QStringLiteral("yiwdfz"));
        QCOMPARE(missed.text, QStringLiteral("alpha beta"));
        QCOMPARE(missed.mode, QStringLiteral("normal"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 0,
                        QStringLiteral("yiwdfzP")).text, QStringLiteral("alphaalpha beta"));

        // Whole-line yank pastes below the line the caret is on.
        const VimResult pasted = runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                                        QStringLiteral("yyp"));
        QCOMPARE(pasted.text, QStringLiteral("one\none\ntwo"));
        QCOMPARE(pasted.cursor, 4);

        const VimResult swapped = runVim(editor.data(), QStringLiteral("ab"), 0,
                                         QStringLiteral("xp"));
        QCOMPARE(swapped.text, QStringLiteral("ba"));
        QCOMPARE(swapped.cursor, 1);
    }

    void changesTextAndRepeatsWithVimKeys() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        const VimResult changed = runVim(editor.data(), QStringLiteral("alpha beta"), 0,
                                         QStringLiteral("cwomega<Esc>"));
        QCOMPARE(changed.text, QStringLiteral("omega beta"));
        QCOMPARE(changed.mode, QStringLiteral("normal"));
        QCOMPARE(changed.cursor, 4);

        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha"), 0,
                        QStringLiteral("A!<Esc>")).text, QStringLiteral("alpha!"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha"), 2,
                        QStringLiteral("Inew <Esc>")).text, QStringLiteral("new alpha"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one"), 0,
                        QStringLiteral("otwo<Esc>")).text, QStringLiteral("one\ntwo"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("two"), 0,
                        QStringLiteral("Oone<Esc>")).text, QStringLiteral("one\ntwo"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("  - item"), 4,
                        QStringLiteral("ccnext<Esc>")).text, QStringLiteral("  next"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("cat"), 0,
                        QStringLiteral("rb")).text, QStringLiteral("bat"));

        // Visual mode operates on the highlighted characters, inclusive.
        const VimResult visual = runVim(editor.data(), QStringLiteral("alpha beta"), 0,
                                        QStringLiteral("vlld"));
        QCOMPARE(visual.text, QStringLiteral("ha beta"));
        QCOMPARE(visual.mode, QStringLiteral("normal"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 5,
                        QStringLiteral("Vd")).text, QStringLiteral("one\nthree"));

        // The dot command repeats the last change, typed text included.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 0,
                        QStringLiteral("dw.")).text, QStringLiteral("gamma"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one two"), 0,
                        QStringLiteral("cwX<Esc>w.")).text, QStringLiteral("X X"));

        // Undo takes back the whole command, not the edits inside it.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                        QStringLiteral("ddu")).text, QStringLiteral("one\ntwo"));

        // Text objects take a span without a motion. iw stops at the word,
        // aw takes the space after it, and both work under any operator.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 7,
                        QStringLiteral("diw")).text, QStringLiteral("alpha  gamma"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 7,
                        QStringLiteral("daw")).text, QStringLiteral("alpha gamma"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 8,
                        QStringLiteral("ciwomega<Esc>")).text, QStringLiteral("alpha omega"));

        // The quote and bracket pairs, from anywhere inside them.
        QCOMPARE(runVim(editor.data(), QStringLiteral("say \"hello there\" now"), 9,
                        QStringLiteral("di\"")).text, QStringLiteral("say \"\" now"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("say \"hello\" now"), 6,
                        QStringLiteral("da\"")).text, QStringLiteral("say  now"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("f(a, g(b), c)"), 4,
                        QStringLiteral("di(")).text, QStringLiteral("f()"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("f(a, g(b), c)"), 7,
                        QStringLiteral("di(")).text, QStringLiteral("f(a, g(), c)"));

        // ip is linewise and stops at the blank line; ap takes the gap too.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\n\nthree"), 4,
                        QStringLiteral("dip")).text, QStringLiteral("\nthree"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\n\nthree"), 4,
                        QStringLiteral("dap")).text, QStringLiteral("three"));

        // From visual mode the object becomes the selection.
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta gamma"), 7,
                        QStringLiteral("viwd")).text, QStringLiteral("alpha  gamma"));

        // A selection dragged out with the mouse stands in for a visual range.
        QMetaObject::invokeMethod(editor.data(), "reset",
                                  Q_ARG(QVariant, QStringLiteral("alpha beta")),
                                  Q_ARG(QVariant, 0));
        QMetaObject::invokeMethod(editor.data(), "selectRange",
                                  Q_ARG(QVariant, 0), Q_ARG(QVariant, 6));
        QMetaObject::invokeMethod(editor.data(), "feed", Q_ARG(QVariant, QStringLiteral("d")));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("beta"));

        // r over a visual selection replaces every character in it and steps
        // over the line breaks, rather than stopping at the end of the first
        // line the way the single-line r does.
        QCOMPARE(runVim(editor.data(), QStringLiteral("abc\ndef"), 0,
                        QStringLiteral("vjrz")).text, QStringLiteral("zzz\nzef"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("abc\ndef"), 0,
                        QStringLiteral("Vjrz")).text, QStringLiteral("zzz\nzzz"));

        // One replacement per character, not per UTF-16 unit, so a selection
        // holding an emoji comes back the same length it went in.
        QCOMPARE(runVim(editor.data(), QStringLiteral("a\U0001F600b"), 0,
                        QStringLiteral("vllrz")).text, QStringLiteral("zzz"));

        // An object that does not resolve leaves the document alone.
        QCOMPARE(runVim(editor.data(), QStringLiteral("no quotes here"), 3,
                        QStringLiteral("di\"")).text, QStringLiteral("no quotes here"));

        // Normal mode never lets a plain letter reach the document.
        const VimResult swallowed = runVim(editor.data(), QStringLiteral("text"), 0,
                                           QStringLiteral("zq"));
        QCOMPARE(swallowed.text, QStringLiteral("text"));

        // Insert mode hands every key back to the editor.
        QCOMPARE(editor->property("unhandled").toStringList(), QStringList());
        runVim(editor.data(), QStringLiteral("x"), 0, QStringLiteral("ia"));
        QCOMPARE(editor->property("unhandled").toStringList(), QStringList{QStringLiteral("a")});
    }

    // The engine runs its edits inside one of the document's edit blocks,
    // where the editor's text property stops moving until the block closes.
    // Everything it does has to survive that; the harness freezes its text
    // the same way so these run without a window.
    void editsThroughAnOpenEditBlock() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        // A command that throws partway through still closes its block. Left
        // open, the document would hold its change signal for the rest of the
        // session: the text the engine reads freezes, and so does everything
        // hanging off onTextChanged, while the writer keeps typing.
        QMetaObject::invokeMethod(editor.data(), "reset",
                                  Q_ARG(QVariant, QStringLiteral("alpha")),
                                  Q_ARG(QVariant, 0));
        editor->setProperty("hookThrows", true);
        QMetaObject::invokeMethod(editor.data(), "feedThrough",
                                  Q_ARG(QVariant, QStringLiteral("\"+p")));
        QVERIFY(editor->property("threw").toBool());
        QCOMPARE(editor->property("blockDepth").toInt(), 0);

        // And the next command edits the document rather than a frozen copy.
        editor->setProperty("hookThrows", false);
        QMetaObject::invokeMethod(editor.data(), "feed", Q_ARG(QVariant, QStringLiteral("x")));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("lpha"));

        // The second edit of a repeat lands past where the document ended
        // when the block opened. Clamping it to the stale length would drop
        // it back inside the old text and mangle both.
        QCOMPARE(runVim(editor.data(), QStringLiteral("ab"), 0,
                        QStringLiteral("oX<Esc>.")).text, QStringLiteral("ab\nX\nX"));

        // Commands that edit repeatedly read the document as they go.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 0,
                        QStringLiteral("3J")).text, QStringLiteral("one two three"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                        QStringLiteral("yyp")).text, QStringLiteral("one\none\ntwo"));
    }

    void yanksAndPastesThroughNamedRegisters() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        auto clipboard = [&] { return editor->property("clipboardText").toString(); };

        // "a holds a line aside while the unnamed register moves on, and "ap
        // pastes what was put there rather than what was deleted since.
        const VimResult aside = runVim(editor.data(), QStringLiteral("one\ntwo\nthree"), 0,
                                       QStringLiteral("\"ayyjdd\"ap"));
        QCOMPARE(aside.text, QStringLiteral("one\nthree\none"));

        // A bare p still pastes the last thing taken, named or not.
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                        QStringLiteral("\"ayyp")).text, QStringLiteral("one\none\ntwo"));

        // Nothing reaches the system clipboard unless a register asks it to.
        runVim(editor.data(), QStringLiteral("one\ntwo"), 0, QStringLiteral("yydd"));
        QCOMPARE(clipboard(), QString());

        // "+y does, and marks a linewise yank with the trailing newline that
        // is all a clipboard can carry.
        runVim(editor.data(), QStringLiteral("one\ntwo"), 0, QStringLiteral("\"+yy"));
        QCOMPARE(clipboard(), QStringLiteral("one\n"));
        runVim(editor.data(), QStringLiteral("alpha beta"), 0, QStringLiteral("\"+yw"));
        QCOMPARE(clipboard(), QStringLiteral("alpha "));

        // "+p reads it back, and the trailing newline makes it linewise again.
        editor->setProperty("clipboardText", QStringLiteral("carried\n"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("one\ntwo"), 0,
                        QStringLiteral("\"+p")).text, QStringLiteral("one\ncarried\ntwo"));
        editor->setProperty("clipboardText", QStringLiteral("carried"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("ab"), 0,
                        QStringLiteral("\"+p")).text, QStringLiteral("acarriedb"));

        // "* is the primary selection, a different register entirely.
        editor->setProperty("clipboardText", QString());
        runVim(editor.data(), QStringLiteral("one\ntwo"), 0, QStringLiteral("\"*yy"));
        QCOMPARE(editor->property("selectionText").toString(), QStringLiteral("one\n"));
        QCOMPARE(clipboard(), QString());

        // A delete into "b keeps the clipboard out of it.
        editor->setProperty("clipboardText", QStringLiteral("untouched"));
        QCOMPARE(runVim(editor.data(), QStringLiteral("alpha beta"), 0,
                        QStringLiteral("\"bdw\"bP")).text, QStringLiteral("alpha beta"));
        QCOMPARE(clipboard(), QStringLiteral("untouched"));

        // The link shortcut is offered charwise ranges only, and it tells the
        // editor which register asked, so "+ and "* reach the clipboard each
        // of them names rather than whichever one is nearer.
        editor->setProperty("clipboardText", QStringLiteral("https://example.com"));
        runVim(editor.data(), QStringLiteral("alpha beta"), 0, QStringLiteral("ve\"+p"));
        QCOMPARE(editor->property("linkPasteCalls").toStringList(),
                 QStringList{QStringLiteral("0:5:https://example.com:+")});

        editor->setProperty("selectionText", QStringLiteral("https://example.org"));
        runVim(editor.data(), QStringLiteral("alpha beta"), 0, QStringLiteral("ve\"*p"));
        QCOMPARE(editor->property("linkPasteCalls").toStringList(),
                 QStringList{QStringLiteral("0:5:https://example.org:*")});

        // A V-LINE range carries the raw anchors rather than whole lines, so
        // wrapping it would take part of the selection.
        runVim(editor.data(), QStringLiteral("alpha\nbeta"), 0, QStringLiteral("V\"+p"));
        QCOMPARE(editor->property("linkPasteCalls").toStringList(), QStringList());

        // The pending register shows in the footer while it waits.
        QMetaObject::invokeMethod(editor.data(), "reset",
                                  Q_ARG(QVariant, QStringLiteral("text")), Q_ARG(QVariant, 0));
        QMetaObject::invokeMethod(editor.data(), "feed", Q_ARG(QVariant, QStringLiteral("\"a2")));
        QVariant status;
        QMetaObject::invokeMethod(editor.data(), "status", Q_RETURN_ARG(QVariant, status));
        QCOMPARE(status.toString(), QStringLiteral("NORMAL \"a2"));
    }

    void runsExCommands() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        const QString document = QStringLiteral("one fish\ntwo fish\nred fish\nblue fish");

        // A bare range jumps; the caret lands on the first non-blank.
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":3")).cursor, 18);
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":$")).cursor, 27);
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":99")).cursor, 27);

        // Substitute defaults to the current line, once per line.
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":s/fish/cat/")).text,
                 QStringLiteral("one cat\ntwo fish\nred fish\nblue fish"));
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":%s/fish/cat/")).text,
                 QStringLiteral("one cat\ntwo cat\nred cat\nblue cat"));
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":2,3s/fish/cat/")).text,
                 QStringLiteral("one fish\ntwo cat\nred cat\nblue fish"));

        // g replaces every match on the line, i ignores case.
        QCOMPARE(runEx(editor.data(), QStringLiteral("a a a"), 0,
                       QStringLiteral(":s/a/b/")).text, QStringLiteral("b a a"));
        QCOMPARE(runEx(editor.data(), QStringLiteral("a a a"), 0,
                       QStringLiteral(":s/a/b/g")).text, QStringLiteral("b b b"));
        QCOMPARE(runEx(editor.data(), QStringLiteral("Fish fish"), 0,
                       QStringLiteral(":s/fish/cat/gi")).text, QStringLiteral("cat cat"));

        // & is the whole match, \1 a captured group, and a separator can be
        // escaped or swapped for one that does not collide with the text.
        QCOMPARE(runEx(editor.data(), QStringLiteral("hello"), 0,
                       QStringLiteral(":s/hello/[&]/")).text, QStringLiteral("[hello]"));
        QCOMPARE(runEx(editor.data(), QStringLiteral("Doe, John"), 0,
                       QStringLiteral(":s/(\\w+), (\\w+)/\\2 \\1/")).text,
                 QStringLiteral("John Doe"));
        QCOMPARE(runEx(editor.data(), QStringLiteral("a/b"), 0,
                       QStringLiteral(":s#/#-#")).text, QStringLiteral("a-b"));

        // The caret lands on the last line substituted, not the first. The
        // loop runs bottom up so that replacing a line cannot shift the ones
        // still to come, which is the opposite order to where it should stop.
        const VimResult swept = runEx(editor.data(),
                                      QStringLiteral("a\nb\na\nb\na"), 0,
                                      QStringLiteral(":%s/a/z/"));
        QCOMPARE(swept.text, QStringLiteral("z\nb\nz\nb\nz"));
        QCOMPARE(swept.cursor, 8);

        // A replacement carrying \n moves the line it was recorded on: the
        // edits above it push it down, and its own new lines extend the
        // change past it. Vim lands on the last line of the whole change,
        // which here is the second b rather than the line where it started.
        const VimResult grown = runEx(editor.data(),
                                      QStringLiteral("one x\ntwo x\nthree"), 0,
                                      QStringLiteral(":%s/x/a\\nb/"));
        QCOMPARE(grown.text, QStringLiteral("one a\nb\ntwo a\nb\nthree"));
        QCOMPARE(grown.cursor, 14);

        // A pattern that backtracks catastrophically stops rather than
        // hanging the window. QML's JS engine compiles RegExp through PCRE2,
        // whose match limit bounds the work: the same expression that runs
        // for hours under V8 gives up here in milliseconds and reports no
        // match. The line is long enough that an unbounded engine would not
        // finish this test.
        QElapsedTimer pathological;
        pathological.start();
        const VimResult bounded = runEx(editor.data(),
                                        QString(2000, QLatin1Char('a')) + QStringLiteral("b"), 0,
                                        QStringLiteral(":s/(a+)+$/z/"));
        const qint64 spent = pathological.elapsed();
        QVERIFY(!bounded.ok);
        QCOMPARE(bounded.message, QStringLiteral("Pattern not found: (a+)+$"));
        QVERIFY2(spent < 2000, qPrintable(QStringLiteral("substitute took %1 ms").arg(spent)));

        // Reported outcomes, including the ones that change nothing.
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":%s/fish/cat/")).message,
                 QStringLiteral("4 substitutions on 4 lines"));
        const VimResult missing = runEx(editor.data(), document, 0,
                                        QStringLiteral(":s/whale/cat/"));
        QVERIFY(!missing.ok);
        QCOMPARE(missing.message, QStringLiteral("Pattern not found: whale"));
        QCOMPARE(missing.text, document);

        const VimResult unknown = runEx(editor.data(), document, 0, QStringLiteral(":frobnicate"));
        QVERIFY(!unknown.ok);
        QCOMPARE(unknown.message, QStringLiteral("Not an editor command: frobnicate"));

        const VimResult broken = runEx(editor.data(), document, 0, QStringLiteral(":s/(unclosed/x/"));
        QVERIFY(!broken.ok);
        QCOMPARE(broken.text, document);

        // :d takes whole lines, and leaves them in the register for p.
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":2d")).text,
                 QStringLiteral("one fish\nred fish\nblue fish"));
        QCOMPARE(runEx(editor.data(), document, 0, QStringLiteral(":2,3d")).text,
                 QStringLiteral("one fish\nblue fish"));

        // An empty pattern reuses the last one.
        QMetaObject::invokeMethod(editor.data(), "reset", Q_ARG(QVariant, document), Q_ARG(QVariant, 0));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":s/fish/cat/")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":2s//dog/")));
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one cat\ntwo dog\nred fish\nblue fish"));

        // Pressing : in visual mode opens the command line prefilled with the
        // range covering the selected lines.
        QMetaObject::invokeMethod(editor.data(), "reset", Q_ARG(QVariant, document), Q_ARG(QVariant, 9));
        QMetaObject::invokeMethod(editor.data(), "feed", Q_ARG(QVariant, QStringLiteral("Vj:")));
        QCOMPARE(editor->property("calls").toStringList(),
                 QStringList{QStringLiteral("commandLine:'<,'>")});
        QCOMPARE(readVim(editor.data()).mode, QStringLiteral("normal"));
        QMetaObject::invokeMethod(editor.data(), "ex",
                                  Q_ARG(QVariant, QStringLiteral(":'<,'>s/fish/cat/")));
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one fish\ntwo cat\nred cat\nblue fish"));
    }

    void callsTheApplicationForFileExCommands() {
        QScopedPointer<QObject> editor(createVimHarness());
        QVERIFY(!editor.isNull());

        QMetaObject::invokeMethod(editor.data(), "reset", Q_ARG(QVariant, QString()), Q_ARG(QVariant, 0));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":w")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":w draft.md")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":wq")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":q")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":q!")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":e notes.md")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":noh")));
        // Abbreviations reach the same commands as the spelled-out names.
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":write")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":x")));
        QMetaObject::invokeMethod(editor.data(), "ex", Q_ARG(QVariant, QStringLiteral(":quit")));

        QCOMPARE(editor->property("calls").toStringList(),
                 QStringList({QStringLiteral("save"),
                              QStringLiteral("saveAs:draft.md"),
                              QStringLiteral("saveAndQuit"),
                              QStringLiteral("quit:false"),
                              QStringLiteral("quit:true"),
                              QStringLiteral("open:notes.md:false"),
                              QStringLiteral("clearSearch"),
                              QStringLiteral("save"),
                              QStringLiteral("saveAndQuit"),
                              QStringLiteral("quit:false")}));
    }

    void resolvesPathsTypedOnTheCommandLine() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        Backend backend;
        // With no file open, a bare name lands in the home directory.
        QCOMPARE(backend.resolvePath(QStringLiteral("draft.md")).toLocalFile(),
                 QDir::homePath() + QStringLiteral("/draft.md"));
        QCOMPARE(backend.resolvePath(QStringLiteral("~/notes/draft.md")).toLocalFile(),
                 QDir::homePath() + QStringLiteral("/notes/draft.md"));
        QCOMPARE(backend.resolvePath(QStringLiteral("/tmp/draft.md")).toLocalFile(),
                 QStringLiteral("/tmp/draft.md"));
        QVERIFY(backend.resolvePath(QString()).isEmpty());

        // Once a file is open, a relative name is a sibling of it.
        backend.saveAs(QUrl::fromLocalFile(directory.filePath(QStringLiteral("open.md"))));
        QCOMPARE(backend.resolvePath(QStringLiteral("draft.md")).toLocalFile(),
                 directory.filePath(QStringLiteral("draft.md")));
    }

    void routesKeyPressesThroughVimMode() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        backend.setVimMode(true);
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
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("NORMAL"));
        QVERIFY(editor->property("vimNormalMode").toBool());

        editor->setProperty("text", QStringLiteral("alpha beta"));
        editor->setProperty("cursorPosition", 0);

        // Normal mode keeps its keys out of the document and runs commands.
        QTest::keyClick(window, Qt::Key_D);
        QTest::keyClick(window, Qt::Key_W);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("beta"));

        // Insert mode types, and leaves every editor behaviour in place.
        QTest::keyClick(window, Qt::Key_I);
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("INSERT"));
        QVERIFY(!editor->property("vimNormalMode").toBool());
        QTest::keyClick(window, Qt::Key_X);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("xbeta"));

        QTest::keyClick(window, Qt::Key_Escape);
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("NORMAL"));

        // Ctrl+[ is vim's other Escape, and it is not a letter, so it needs
        // naming separately from the C-<letter> chords.
        QTest::keyClick(window, Qt::Key_I);
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("INSERT"));
        QTest::keyClick(window, Qt::Key_BracketLeft, Qt::ControlModifier);
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("NORMAL"));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("xbeta"));

        backend.setVimMode(false);
        QCOMPARE(window->property("vimStatus").toString(), QString());
        // Escape stepped the caret back onto the x, so plain typing lands there.
        QTest::keyClick(window, Qt::Key_D);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("dxbeta"));
    }

    // o, O and a visual p go through the editor's own Markdown handling
    // rather than reimplementing it inside the vim engine.
    void reusesTheEditorsMarkdownBehaviour() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        backend.setVimMode(true);
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
        auto text = [&] { return editor->property("text").toString(); };
        auto esc = [&] { QTest::keyClick(window, Qt::Key_Escape); };
        auto load = [&](const QString &content, int position) {
            esc();
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };

        // Where a command leaves the caret survives the edit block closing.
        load(QStringLiteral("one\ntwo\nthree"), 0);
        typeInto(window, QStringLiteral("jdd"));
        QCOMPARE(text(), QStringLiteral("one\nthree"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 4);

        // Commands that edit more than once read the document as they go, not
        // the editor's cache, which an open edit block leaves behind.
        load(QStringLiteral("one\ntwo\nthree"), 0);
        typeInto(window, QStringLiteral("3J"));
        QCOMPARE(text(), QStringLiteral("one two three"));

        load(QStringLiteral("one\ntwo"), 0);
        typeInto(window, QStringLiteral("yyp"));
        QCOMPARE(text(), QStringLiteral("one\none\ntwo"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 4);

        // An edit that runs past where the document ended before the command
        // started must not be clamped to the editor's stale copy of it.
        load(QStringLiteral("one\n\ntwo"), 0);
        typeInto(window, QStringLiteral("otwo"));
        esc();
        editor->setProperty("cursorPosition", 12);
        typeInto(window, QStringLiteral("."));
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n\ntwo\n\ntwo"));

        // o carries a bullet down the way Return does in insert mode.
        load(QStringLiteral("- item"), 3);
        typeInto(window, QStringLiteral("o"));
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("INSERT"));
        QCOMPARE(text(), QStringLiteral("- item\n- "));
        QCOMPARE(editor->property("cursorPosition").toInt(), 9);
        typeInto(window, QStringLiteral("next"));
        QCOMPARE(text(), QStringLiteral("- item\n- next"));

        // Numbers count on downwards; a line opened above keeps its number.
        load(QStringLiteral("2. second"), 3);
        typeInto(window, QStringLiteral("o"));
        QCOMPARE(text(), QStringLiteral("2. second\n3. "));

        load(QStringLiteral("2. second"), 3);
        typeInto(window, QStringLiteral("O"));
        QCOMPARE(text(), QStringLiteral("2. \n2. second"));

        // A quote continues, and a plain paragraph opens a blank line between,
        // which is what Return does in this editor.
        load(QStringLiteral("> quoted"), 3);
        typeInto(window, QStringLiteral("o"));
        QCOMPARE(text(), QStringLiteral("> quoted\n> "));

        load(QStringLiteral("plain"), 2);
        typeInto(window, QStringLiteral("o"));
        QCOMPARE(text(), QStringLiteral("plain\n\n"));

        // A URL pasted over a visual selection becomes a Markdown link, the
        // same as Ctrl+V does.
        backend.setClipboardText(QStringLiteral("https://example.com"));
        load(QStringLiteral("read the docs here"), 9);
        typeInto(window, QStringLiteral("ve"));
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("VISUAL"));
        QCOMPARE(editor->property("selectionStart").toInt(), 9);
        QCOMPARE(editor->property("selectionEnd").toInt(), 13);
        typeInto(window, QStringLiteral("\"+"));
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("VISUAL \"+"));
        typeInto(window, QStringLiteral("p"));
        QCOMPARE(text(), QStringLiteral("read the [docs](https://example.com) here"));

        // A link paste is a change like any other, and . repeats it. Before
        // it recorded itself, . replayed whatever was recorded last — here a
        // dd, which took a line out of the document instead.
        backend.setClipboardText(QStringLiteral("https://example.com"));
        load(QStringLiteral("one\nread the docs here"), 0);
        typeInto(window, QStringLiteral("dd"));
        QCOMPARE(text(), QStringLiteral("read the docs here"));
        editor->setProperty("cursorPosition", 9);
        typeInto(window, QStringLiteral("ve\"+p"));
        QCOMPARE(text(), QStringLiteral("read the [docs](https://example.com) here"));
        editor->setProperty("cursorPosition", 0);
        typeInto(window, QStringLiteral("."));
        QCOMPARE(text(),
                 QStringLiteral("rhttps://example.comead the [docs](https://example.com) here"));

        // P is the literal paste, so there is a way to mean the text itself.
        load(QStringLiteral("read the docs here"), 9);
        typeInto(window, QStringLiteral("ve\"+P"));
        QCOMPARE(text(), QStringLiteral("read the https://example.com here"));

        // Anything that is not a URL pastes as text either way.
        backend.setClipboardText(QStringLiteral("manual"));
        load(QStringLiteral("read the docs here"), 9);
        typeInto(window, QStringLiteral("ve\"+p"));
        QCOMPARE(text(), QStringLiteral("read the manual here"));

        // Going to the search bar and back leaves normal mode, but not the
        // registers: yanking something and then looking for where it belongs
        // is the whole point of having gone.
        load(QStringLiteral("one\ntwo"), 0);
        typeInto(window, QStringLiteral("yy"));
        window->setProperty("searchOpen", true);
        QMetaObject::invokeMethod(window, "closeSearch");
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("NORMAL"));
        typeInto(window, QStringLiteral("p"));
        QCOMPARE(text(), QStringLiteral("one\none\ntwo"));

        // A URL yanked into a register links the same as a copied one, with
        // the clipboard empty to prove the register is what is being read.
        backend.setClipboardText(QString());
        load(QStringLiteral("https://example.org\nread the docs here"), 0);
        typeInto(window, QStringLiteral("\"ay$"));
        editor->setProperty("cursorPosition", 29);
        typeInto(window, QStringLiteral("ve\"ap"));
        QCOMPARE(text(),
                 QStringLiteral("https://example.org\nread the [docs](https://example.org) here"));

        // One undo takes the whole link back, not the delete and the insert
        // of it separately.
        typeInto(window, QStringLiteral("u"));
        QCOMPARE(text(), QStringLiteral("https://example.org\nread the docs here"));
    }

    void walksDisplayLinesWithJAndK() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        backend.setVimMode(true);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->resize(420, 400);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // One paragraph per line, long enough that the first one wraps.
        const QString first = QStringLiteral(
            "The paragraph runs on well past the width of the window so that it "
            "has to wrap onto a second display line before it ends.");
        editor->setProperty("text", first + QStringLiteral("\nSecond paragraph."));
        editor->setProperty("cursorPosition", 0);
        QVERIFY(editor->property("width").toReal() > 0);

        // j follows the wrapped text, so it stays inside the first paragraph
        // rather than jumping over the whole of it.
        QTest::keyClick(window, Qt::Key_J);
        const int afterJ = editor->property("cursorPosition").toInt();
        QVERIFY2(afterJ > 0 && afterJ < first.length(),
                 qPrintable(QStringLiteral("j landed at %1, outside the first paragraph")
                                .arg(afterJ)));

        // k comes back to where it started, holding the goal column.
        QTest::keyClick(window, Qt::Key_K);
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);

        // gj is the logical line, so it reaches the second paragraph in one.
        editor->setProperty("cursorPosition", 0);
        QTest::keyClick(window, Qt::Key_G);
        QTest::keyClick(window, Qt::Key_J);
        QCOMPARE(editor->property("cursorPosition").toInt(), first.length() + 1);

        // An operator over j stays logical: dj takes both whole paragraphs,
        // never half of the wrapped one.
        editor->setProperty("cursorPosition", 0);
        QTest::keyClick(window, Qt::Key_D);
        QTest::keyClick(window, Qt::Key_J);
        QCOMPARE(editor->property("text").toString(), QString());
    }

    void opensTheCommandLineFromTheEditor() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        backend.setVimMode(true);
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
        QObject *commandField = window->findChild<QObject *>(QStringLiteral("commandField"));
        QVERIFY(editor);
        QVERIFY(commandField);
        QVERIFY(!window->property("commandOpen").toBool());

        editor->setProperty("text", QStringLiteral("one fish\ntwo fish\nred fish"));
        editor->setProperty("cursorPosition", 0);

        // : opens the command line and takes the keyboard with it.
        QTest::keyClick(window, Qt::Key_Colon);
        QVERIFY(window->property("commandOpen").toBool());
        QVERIFY(commandField->property("activeFocus").toBool());
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one fish\ntwo fish\nred fish"));

        // A line number jumps and closes the command line.
        typeInto(window, QStringLiteral("2"));
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(!window->property("commandOpen").toBool());
        QCOMPARE(editor->property("cursorPosition").toInt(), 9);
        QVERIFY(editor->property("activeFocus").toBool());

        // A substitute runs against the document.
        QTest::keyClick(window, Qt::Key_Colon);
        typeInto(window, QStringLiteral("%s/fish/cat/"));
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one cat\ntwo cat\nred cat"));
        QCOMPARE(window->property("vimMessage").toString(),
                 QStringLiteral("3 substitutions on 3 lines"));

        // u takes the whole substitute back in one step.
        QTest::keyClick(window, Qt::Key_U);
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one fish\ntwo fish\nred fish"));
        QCOMPARE(window->property("vimMessage").toString(), QString());

        // Escape abandons a command line without running it.
        QTest::keyClick(window, Qt::Key_Colon);
        typeInto(window, QStringLiteral("%s/fish/dog/"));
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("commandOpen").toBool());
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("one fish\ntwo fish\nred fish"));

        // With vim mode off, : is just a character again.
        backend.setVimMode(false);
        QTest::keyClick(window, Qt::Key_Colon);
        QVERIFY(!window->property("commandOpen").toBool());
        QVERIFY(editor->property("text").toString().contains(QStringLiteral(":")));
    }

    void startsAtTheTopOfAnOpenedDocument() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("opened.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("# Title\n\nA paragraph of prose.\n\nAnother one.\n");
        file.close();

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        backend.setVimMode(true);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // Opening a document leaves the caret on its first character, where
        // vim starts, rather than adrift on the trailing empty line.
        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(editor->property("text").toString().left(7), QStringLiteral("# Title"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);
        QCOMPARE(window->property("vimStatus").toString(), QStringLiteral("NORMAL"));

        // Reloading from disk does the same.
        editor->setProperty("cursorPosition", 12);
        backend.reloadFromDisk();
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);

        backend.setVimMode(false);
    }

    void remembersVimModePreference() {
        Backend backend;
        backend.setVimMode(false);

        QSignalSpy vimModeSpy(&backend, &Backend::vimModeChanged);
        backend.setVimMode(true);
        QCOMPARE(vimModeSpy.count(), 1);

        Backend restored;
        QVERIFY(restored.vimMode());
        restored.setVimMode(false);

        Backend cleared;
        QVERIFY(!cleared.vimMode());
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
        QVERIFY(window->findChild<QObject *>(QStringLiteral("vimButton")));
        QVERIFY(window->findChild<QObject *>(QStringLiteral("vimStatus")));

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

    // What "+y and "+p sit on: the clipboard, and the primary selection where
    // the desktop has one.
    void carriesTextThroughTheClipboard() {
        Backend backend;
        backend.setClipboardText(QStringLiteral("yanked\n"));
        QCOMPARE(backend.clipboardText(), QStringLiteral("yanked\n"));

        if (!QGuiApplication::clipboard()->supportsSelection())
            QSKIP("this platform has no primary selection");

        backend.setClipboardText(QStringLiteral("selected"), true);
        QCOMPARE(backend.clipboardText(true), QStringLiteral("selected"));
        QCOMPARE(backend.clipboardText(), QStringLiteral("yanked\n"));
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
    struct VimResult {
        QString text;
        int cursor;
        QString mode;
        QString message;
        bool ok;
    };

    // A bare TextEdit driven by the vim engine: keys it does not consume are
    // typed into the document, the way insert mode leaves them to the editor.
    QObject *createVimHarness() {
        const QString vimPath = QFINDTESTDATA("../src/Vim.js");
        if (vimPath.isEmpty())
            return nullptr;

        const QByteArray harness = R"QML(
            import QtQuick
            import "Vim.js" as Vim

            TextEdit {
                id: harness
                property var state: Vim.createState()
                property var host: null
                property var unhandled: []
                property var calls: []
                property string message: ""
                property bool ok: true
                property string clipboardText: ""
                property string selectionText: ""

                // A stand-in for a QTextDocument edit block. While one is open
                // the real editor's text property stops moving, because the
                // document holds its change signals back until the block
                // closes; everything else stays live. The engine has to work
                // through that, so the harness reproduces it rather than
                // handing the engine a document that is always up to date.
                property var linkPasteCalls: []
                property int blockDepth: 0
                property string frozenText: ""
                // Stands in for a hook failing partway through a command.
                property bool hookThrows: false
                property bool threw: false

                property var blockedEditor: ({
                    get text() {
                        return harness.blockDepth > 0 ? harness.frozenText : harness.text;
                    },
                    get cursorPosition() { return harness.cursorPosition; },
                    set cursorPosition(value) { harness.cursorPosition = value; },
                    get selectionStart() { return harness.selectionStart; },
                    get selectionEnd() { return harness.selectionEnd; },
                    liveLength: function() { return harness.text.length; },
                    deselect: function() { harness.deselect(); },
                    select: function(from, to) { harness.select(from, to); },
                    moveCursorSelection: function(to) { harness.moveCursorSelection(to); },
                    remove: function(from, to) { harness.remove(from, to); },
                    insert: function(at, what) { harness.insert(at, what); },
                    // The editor wraps undo and redo to step past the
                    // formatting-only edits that end an undo run; a bare
                    // TextEdit has no runs to end, so plain undo stands in.
                    undoEdit: function() { harness.undo(); },
                    redoEdit: function() { harness.redo(); },
                    positionAt: function(x, y) { return harness.positionAt(x, y); },
                    positionToRectangle: function(at) {
                        return harness.positionToRectangle(at);
                    }
                })

                Component.onCompleted: host = Vim.createHost(blockedEditor, {
                    text: function() { return harness.text; },
                    beginChange: function() {
                        if (harness.blockDepth++ === 0)
                            harness.frozenText = harness.text;
                    },
                    endChange: function() {
                        harness.blockDepth = Math.max(0, harness.blockDepth - 1);
                    },
                    // Records what the engine asked for and declines, so the
                    // plain paste still runs and the arguments can be checked.
                    linkPaste: function(start, end, payload, register) {
                        harness.linkPasteCalls = harness.linkPasteCalls.concat(
                            [start + ":" + end + ":" + payload + ":" + register]);
                        return false;
                    },
                    clipboard: function(fromSelection) {
                        if (harness.hookThrows)
                            throw new Error("hook failed");
                        return fromSelection ? harness.selectionText : harness.clipboardText;
                    },
                    setClipboard: function(text, toSelection) {
                        if (toSelection)
                            harness.selectionText = text;
                        else
                            harness.clipboardText = text;
                    },
                    save: function() { record("save"); },
                    saveAs: function(path) { record("saveAs:" + path); },
                    saveAndQuit: function() { record("saveAndQuit"); },
                    quit: function(force) { record("quit:" + force); },
                    open: function(path, force) { record("open:" + path + ":" + force); },
                    clearSearch: function() { record("clearSearch"); },
                    commandLine: function(prefill) { record("commandLine:" + prefill); }
                })

                function record(call) { calls = calls.concat([call]); }

                function reset(startText, startCursor) {
                    state = Vim.createState();
                    unhandled = [];
                    calls = [];
                    linkPasteCalls = [];
                    message = "";
                    ok = true;
                    threw = false;
                    text = startText;
                    cursorPosition = startCursor;
                }

                function ex(command) {
                    var result = Vim.runCommand(state, host, command);
                    message = result.message;
                    ok = result.ok;
                }

                function feedThrough(sequence) {
                    try {
                        feed(sequence);
                    } catch (error) {
                        threw = true;
                    }
                }

                function feed(sequence) {
                    var keys = tokenize(sequence);
                    for (var i = 0; i < keys.length; i++) {
                        if (Vim.handleKey(state, host, keys[i]))
                            continue;
                        unhandled = unhandled.concat([keys[i]]);
                        if (keys[i] === "Return")
                            insert(cursorPosition, "\n");
                        else if (keys[i] === "Backspace")
                            remove(Math.max(0, cursorPosition - 1), cursorPosition);
                        else if (keys[i].length === 1)
                            insert(cursorPosition, keys[i]);
                    }
                }

                function selectRange(from, to) { select(from, to); }

                function mode() { return state.mode; }

                function status() { return Vim.statusText(state); }

                // "<Esc>", "<CR>" and "<C-r>" name the keys that are not
                // a single character.
                function tokenize(sequence) {
                    var keys = [];
                    for (var i = 0; i < sequence.length; i++) {
                        if (sequence.charAt(i) !== "<") {
                            keys.push(sequence.charAt(i));
                            continue;
                        }
                        var close = sequence.indexOf(">", i);
                        var name = sequence.slice(i + 1, close);
                        keys.push(name === "Esc" ? "Escape" : name === "CR" ? "Return" : name);
                        i = close;
                    }
                    return keys;
                }
            }
        )QML";

        QQmlComponent component(&m_vimEngine);
        component.setData(harness, QUrl::fromLocalFile(
            QFileInfo(vimPath).absolutePath() + QStringLiteral("/VimHarness.qml")));
        if (!component.isReady()) {
            qWarning("%s", qPrintable(component.errorString()));
            return nullptr;
        }
        return component.create();
    }

    // QTest::keyClicks only takes widgets, so type into a QWindow by hand.
    void typeInto(QQuickWindow *window, const QString &characters) {
        for (const QChar &character : characters)
            QTest::keyClick(window, character.toLatin1());
    }

    VimResult readVim(QObject *editor) {
        QVariant mode;
        QMetaObject::invokeMethod(editor, "mode", Q_RETURN_ARG(QVariant, mode));
        return {editor->property("text").toString(),
                editor->property("cursorPosition").toInt(),
                mode.toString(),
                editor->property("message").toString(),
                editor->property("ok").toBool()};
    }

    VimResult runVim(QObject *editor, const QString &text, int cursor, const QString &keys) {
        QMetaObject::invokeMethod(editor, "reset", Q_ARG(QVariant, text), Q_ARG(QVariant, cursor));
        QMetaObject::invokeMethod(editor, "feed", Q_ARG(QVariant, keys));
        return readVim(editor);
    }

    VimResult runEx(QObject *editor, const QString &text, int cursor, const QString &command) {
        QMetaObject::invokeMethod(editor, "reset", Q_ARG(QVariant, text), Q_ARG(QVariant, cursor));
        QMetaObject::invokeMethod(editor, "ex", Q_ARG(QVariant, command));
        return readVim(editor);
    }

    QTemporaryDir m_settingsDirectory;
    QQmlEngine m_vimEngine;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"

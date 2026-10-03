#include <QtTest>
#include <QFont>
#include <QQmlComponent>
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

    void suggestsSafeNames() {
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("My first draft\nBody")),
                 QStringLiteral("My first draft.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("A/B")), QStringLiteral("A-B.md"));
        QCOMPARE(Backend::suggestedFileName(QString()), QStringLiteral("Untitled.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("Already.md")),
                 QStringLiteral("Already.md"));
    }

    void keepsNotoFromCrowdingTheFontList() {
        const QStringList fonts = Backend::selectableFontFamilies({
            QStringLiteral("Noto Sans Devanagari"), QStringLiteral("Liberation Serif"),
            QStringLiteral("Noto Serif"), QStringLiteral("Noto Sans Tamil UI"),
            QStringLiteral("adwaita Sans"), QStringLiteral("IBM Plex Mono"),
            QStringLiteral("Noto Sans"), QStringLiteral("Noto Sans Mono"),
            QStringLiteral("Liberation Serif"), QStringLiteral("Monospace"),
            QStringLiteral("Standard Symbols PS"), QStringLiteral("D050000L"),
            QStringLiteral("Noto Color Emoji"), QStringLiteral("Nimbus Sans [UKWN]"),
            QStringLiteral("Nimbus Sans [URW ]")});
        QCOMPARE(fonts, QStringList({QStringLiteral("IBM Plex Mono"),
                                     QStringLiteral("adwaita Sans"),
                                     QStringLiteral("Liberation Serif"),
                                     QStringLiteral("Nimbus Sans"),
                                     QStringLiteral("Noto Sans"),
                                     QStringLiteral("Noto Sans Mono"),
                                     QStringLiteral("Noto Serif")}));
    }

    void remembersEditorFont() {
        const QString installed = Backend().availableFonts().value(1);
        QVERIFY(!installed.isEmpty());

        {
            Backend backend;
            QCOMPARE(backend.editorFont(), Backend::defaultEditorFont());
            QSignalSpy fontSpy(&backend, &Backend::editorFontChanged);
            backend.setEditorFont(installed);
            QCOMPARE(fontSpy.count(), 1);
        }

        QCOMPARE(Backend().editorFont(), installed);

        // A remembered font that has since been uninstalled falls back.
        QSettings().setValue(QStringLiteral("editor/font"), QStringLiteral("No Such Font"));
        QCOMPARE(Backend().editorFont(), Backend::defaultEditorFont());
        QSettings().remove(QStringLiteral("editor/font"));
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
        const QString snapshotPath =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("recovery-0.json"));
        QTRY_VERIFY(QFile::exists(snapshotPath));

        QFile snapshot(snapshotPath);
        QVERIFY(snapshot.open(QIODevice::ReadOnly));
        const QJsonObject recovery = QJsonDocument::fromJson(snapshot.readAll()).object();
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

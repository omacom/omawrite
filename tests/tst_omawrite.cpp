#include <QtTest>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QStandardPaths>

#include "backend.h"
#include "markdownhighlighter.h"

// Repeater-created delegates keep their QObject parent on the delegate
// model rather than reparenting into the visual tree, so window->findChild
// can't locate them; go through the Repeater's own itemAt() instead.
static QQuickItem *activeEditorItem(QObject *window, Backend &backend) {
    QObject *repeater = window->findChild<QObject *>(QStringLiteral("editorRepeater"));
    if (!repeater)
        return nullptr;
    QQuickItem *item = nullptr;
    QMetaObject::invokeMethod(repeater, "itemAt", Q_RETURN_ARG(QQuickItem *, item),
                              Q_ARG(int, backend.property("activeTabIndex").toInt()));
    return item;
}

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
        const QByteArray originalXdgDataHome = qgetenv("XDG_DATA_HOME");
        struct HomeRestorer {
            QByteArray home;
            QByteArray xdgDataHome;
            ~HomeRestorer() {
                qputenv("HOME", home);
                if (xdgDataHome.isNull())
                    qunsetenv("XDG_DATA_HOME");
                else
                    qputenv("XDG_DATA_HOME", xdgDataHome);
            }
        } restoreHome{originalHome, originalXdgDataHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));
        // QStandardPaths::AppDataLocation prefers $XDG_DATA_HOME over $HOME
        // when the session already exports it, so both must be sandboxed.
        QVERIFY(qputenv("XDG_DATA_HOME",
                        homeDirectory.filePath(QStringLiteral("xdg-data")).toUtf8()));

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

    void autosavesNamedTabToDisk() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("notes.md"));

        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                          QUrl::fromLocalFile(directory.filePath(QStringLiteral("Harness.qml"))));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        Backend backend;
        backend.attachDocument(editor->property("textDocument").value<QObject *>());
        backend.saveAs(QUrl::fromLocalFile(path));
        QVERIFY(QFileInfo::exists(path));

        editor->setProperty("text", QStringLiteral("autosaved content"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        // No explicit save() call: the debounced autosave timer (750ms) must
        // flush this named document to disk on its own.
        QTRY_VERIFY(!backend.modified());

        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(saved.readAll()), QStringLiteral("autosaved content"));
    }

    void doesNotAutosaveUntitledTabToDisk() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());

        const QByteArray originalHome = qgetenv("HOME");
        const QByteArray originalXdgDataHome = qgetenv("XDG_DATA_HOME");
        struct HomeRestorer {
            QByteArray home;
            QByteArray xdgDataHome;
            ~HomeRestorer() {
                qputenv("HOME", home);
                if (xdgDataHome.isNull())
                    qunsetenv("XDG_DATA_HOME");
                else
                    qputenv("XDG_DATA_HOME", xdgDataHome);
            }
        } restoreHome{originalHome, originalXdgDataHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));
        // QStandardPaths::AppDataLocation prefers $XDG_DATA_HOME over $HOME
        // when the session already exports it, so both must be sandboxed.
        QVERIFY(qputenv("XDG_DATA_HOME",
                        homeDirectory.filePath(QStringLiteral("xdg-data")).toUtf8()));

        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                          QUrl::fromLocalFile(homeDirectory.filePath(QStringLiteral("Harness.qml"))));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        Backend backend;
        backend.attachDocument(editor->property("textDocument").value<QObject *>());

        editor->setProperty("text", QStringLiteral("untitled draft"));
        QVERIFY(backend.editorTextChanged());
        QTest::qWait(1000);

        // Untitled documents never gain a file from autosave: they stay
        // "modified" and pointing at no file, protected only by the
        // recovery snapshot.
        QVERIFY(backend.modified());
        QVERIFY(backend.fileUrl().isEmpty());

        const QDir stateDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
        const QStringList recoveryFiles =
            stateDir.entryList(QStringList() << QStringLiteral("untitled-*.json"), QDir::Files);
        QCOMPARE(recoveryFiles.size(), 1);
        QFile recoveryFile(stateDir.filePath(recoveryFiles.first()));
        QVERIFY(recoveryFile.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(recoveryFile.readAll()).contains(QStringLiteral("untitled draft")));
    }

    void restoresNamedTabCursorAcrossRestart() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QByteArray originalHome = qgetenv("HOME");
        const QByteArray originalXdgDataHome = qgetenv("XDG_DATA_HOME");
        struct HomeRestorer {
            QByteArray home;
            QByteArray xdgDataHome;
            ~HomeRestorer() {
                qputenv("HOME", home);
                if (xdgDataHome.isNull())
                    qunsetenv("XDG_DATA_HOME");
                else
                    qputenv("XDG_DATA_HOME", xdgDataHome);
            }
        } restoreHome{originalHome, originalXdgDataHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));
        // QStandardPaths::AppDataLocation prefers $XDG_DATA_HOME over $HOME
        // when the session already exports it, so both must be sandboxed.
        QVERIFY(qputenv("XDG_DATA_HOME",
                        homeDirectory.filePath(QStringLiteral("xdg-data")).toUtf8()));

        QTemporaryDir saveDirectory;
        QVERIFY(saveDirectory.isValid());
        const QString path = saveDirectory.filePath(QStringLiteral("session-notes.md"));

        {
            QQmlEngine engine;
            QQmlComponent component(&engine);
            component.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                              QUrl::fromLocalFile(saveDirectory.filePath(QStringLiteral("Harness.qml"))));
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
            QScopedPointer<QObject> editor(component.create());
            QVERIFY2(editor, qPrintable(component.errorString()));

            Backend backend;
            backend.attachDocument(editor->property("textDocument").value<QObject *>());
            backend.saveAs(QUrl::fromLocalFile(path));
            editor->setProperty("text", QStringLiteral("line one\nline two"));
            QVERIFY(backend.editorTextChanged());
            backend.updateCursorState(6, 6, 6);

            // Wait for the autosave (content) and session (cursor) debounces
            // to both flush before this Backend and its session lock go away.
            QTRY_VERIFY(!backend.modified());
            QTest::qWait(900);
        }

        QQmlEngine engine2;
        QQmlComponent component2(&engine2);
        component2.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                           QUrl::fromLocalFile(saveDirectory.filePath(QStringLiteral("Harness2.qml"))));
        QVERIFY2(component2.isReady(), qPrintable(component2.errorString()));
        QScopedPointer<QObject> editor2(component2.create());
        QVERIFY2(editor2, qPrintable(component2.errorString()));

        Backend restored;
        restored.attachDocument(editor2->property("textDocument").value<QObject *>());

        QCOMPARE(restored.fileUrl(), QUrl::fromLocalFile(path));
        QCOMPARE(editor2->property("text").toString(), QStringLiteral("line one\nline two"));
        QVERIFY(!restored.modified());

        const QVariantMap restore = restored.consumePendingCursorRestore();
        QVERIFY(restore.value(QStringLiteral("valid")).toBool());
        QCOMPARE(restore.value(QStringLiteral("cursor")).toInt(), 6);
    }

    void importsLegacyRecoveryFileOnFirstLaunch() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QByteArray originalHome = qgetenv("HOME");
        const QByteArray originalXdgDataHome = qgetenv("XDG_DATA_HOME");
        struct HomeRestorer {
            QByteArray home;
            QByteArray xdgDataHome;
            ~HomeRestorer() {
                qputenv("HOME", home);
                if (xdgDataHome.isNull())
                    qunsetenv("XDG_DATA_HOME");
                else
                    qputenv("XDG_DATA_HOME", xdgDataHome);
            }
        } restoreHome{originalHome, originalXdgDataHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));
        // QStandardPaths::AppDataLocation prefers $XDG_DATA_HOME over $HOME
        // when the session already exports it, so both must be sandboxed.
        QVERIFY(qputenv("XDG_DATA_HOME",
                        homeDirectory.filePath(QStringLiteral("xdg-data")).toUtf8()));

        const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(stateDir));
        const QString legacyPath = QDir(stateDir).filePath(QStringLiteral("recovery-0.json"));
        QFile legacy(legacyPath);
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        const QJsonObject legacyObject{{QStringLiteral("fileUrl"), QString()},
                                       {QStringLiteral("text"), QStringLiteral("legacy draft")}};
        legacy.write(QJsonDocument(legacyObject).toJson(QJsonDocument::Compact));
        legacy.close();

        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                          QUrl::fromLocalFile(homeDirectory.filePath(QStringLiteral("Harness.qml"))));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        Backend backend;
        backend.attachDocument(editor->property("textDocument").value<QObject *>());

        QCOMPARE(editor->property("text").toString(), QStringLiteral("legacy draft"));
        QVERIFY(backend.modified());
        QVERIFY(!QFileInfo::exists(legacyPath));
    }

    void tabsMaintainIndependentState() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        Backend backend;
        QQmlEngine engine;

        QQmlComponent component0(&engine);
        component0.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                           QUrl::fromLocalFile(directory.filePath(QStringLiteral("Harness0.qml"))));
        QScopedPointer<QObject> editor0(component0.create());
        QVERIFY2(editor0, qPrintable(component0.errorString()));

        QVariantList tabs = backend.property("tabs").toList();
        QCOMPARE(tabs.size(), 1);
        QObject *tab0 = tabs.at(0).value<QObject *>();
        backend.attachTabDocument(tab0, editor0->property("textDocument").value<QObject *>());

        backend.newTab();
        QCOMPARE(backend.property("tabs").toList().size(), 2);
        QCOMPARE(backend.activeTabIndex(), 1);

        QQmlComponent component1(&engine);
        component1.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                           QUrl::fromLocalFile(directory.filePath(QStringLiteral("Harness1.qml"))));
        QScopedPointer<QObject> editor1(component1.create());
        QVERIFY2(editor1, qPrintable(component1.errorString()));
        QObject *tab1 = backend.property("tabs").toList().at(1).value<QObject *>();
        backend.attachTabDocument(tab1, editor1->property("textDocument").value<QObject *>());

        // Editing the active (second) tab must not affect the first tab.
        // Word count refreshes on its own 120ms debounce.
        editor1->setProperty("text", QStringLiteral("second tab words here"));
        QVERIFY(backend.editorTextChangedForTab(tab1));
        QTRY_COMPARE(backend.wordCount(), 4);

        backend.setActiveTabIndex(0);
        QCOMPARE(backend.wordCount(), 0);
        QVERIFY(!backend.modified());

        backend.setActiveTabIndex(1);
        QCOMPARE(backend.wordCount(), 4);
        QVERIFY(backend.modified());
    }

    void closingNamedActiveTabRemovesItWithoutPrompting() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("named.md"));

        Backend backend;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                          QUrl::fromLocalFile(directory.filePath(QStringLiteral("Harness.qml"))));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        backend.newTab();
        QObject *tab1 = backend.property("tabs").toList().at(1).value<QObject *>();
        backend.attachTabDocument(tab1, editor->property("textDocument").value<QObject *>());
        backend.saveAs(QUrl::fromLocalFile(path));
        QVERIFY(!backend.modified());

        QSignalSpy confirmSpy(&backend, &Backend::closeActiveTabRequiresConfirmation);
        backend.closeActiveTab();
        QCOMPARE(confirmSpy.count(), 0);
        QCOMPARE(backend.property("tabs").toList().size(), 1);
        QCOMPARE(backend.activeTabIndex(), 0);
    }

    void closingTheOnlyTabEmitsLastTabClosed() {
        Backend backend;
        QSignalSpy lastTabSpy(&backend, &Backend::lastTabClosed);

        backend.closeActiveTab();

        QCOMPARE(lastTabSpy.count(), 1);
        QCOMPARE(backend.property("tabs").toList().size(), 0);
    }

    void skipsLegacyRecoveryFileStillHeldByALiveProcess() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QByteArray originalHome = qgetenv("HOME");
        const QByteArray originalXdgDataHome = qgetenv("XDG_DATA_HOME");
        struct HomeRestorer {
            QByteArray home;
            QByteArray xdgDataHome;
            ~HomeRestorer() {
                qputenv("HOME", home);
                if (xdgDataHome.isNull())
                    qunsetenv("XDG_DATA_HOME");
                else
                    qputenv("XDG_DATA_HOME", xdgDataHome);
            }
        } restoreHome{originalHome, originalXdgDataHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));
        QVERIFY(qputenv("XDG_DATA_HOME",
                        homeDirectory.filePath(QStringLiteral("xdg-data")).toUtf8()));

        const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(stateDir));
        const QString legacyPath = QDir(stateDir).filePath(QStringLiteral("recovery-0.json"));
        QFile legacy(legacyPath);
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        const QJsonObject legacyObject{{QStringLiteral("fileUrl"), QString()},
                                       {QStringLiteral("text"), QStringLiteral("still being edited elsewhere")}};
        legacy.write(QJsonDocument(legacyObject).toJson(QJsonDocument::Compact));
        legacy.close();

        // Simulate a live process (e.g. one still running the pre-tabs
        // single-slot scheme) that currently owns this snapshot.
        QLockFile liveLock(QDir(stateDir).filePath(QStringLiteral("recovery-0.lock")));
        QVERIFY(liveLock.tryLock());

        Backend backend;

        // The held snapshot must be left completely untouched: not imported
        // as a tab, and not deleted out from under the process that owns it.
        QCOMPARE(backend.property("tabs").toList().size(), 1);
        QVERIFY(!backend.modified());
        QVERIFY(QFileInfo::exists(legacyPath));
        QFile stillThere(legacyPath);
        QVERIFY(stillThere.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(stillThere.readAll()).contains(QStringLiteral("still being edited elsewhere")));

        liveLock.unlock();
    }

    void importsOrphanedLegacyFileEvenWhenASessionAlreadyExists() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QByteArray originalHome = qgetenv("HOME");
        const QByteArray originalXdgDataHome = qgetenv("XDG_DATA_HOME");
        struct HomeRestorer {
            QByteArray home;
            QByteArray xdgDataHome;
            ~HomeRestorer() {
                qputenv("HOME", home);
                if (xdgDataHome.isNull())
                    qunsetenv("XDG_DATA_HOME");
                else
                    qputenv("XDG_DATA_HOME", xdgDataHome);
            }
        } restoreHome{originalHome, originalXdgDataHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));
        QVERIFY(qputenv("XDG_DATA_HOME",
                        homeDirectory.filePath(QStringLiteral("xdg-data")).toUtf8()));

        const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(stateDir));

        // An unrelated session already occupies slot 0 -- the orphaned
        // recovery-0.json (a leftover from before sessions existed) must
        // still be imported as an *additional* tab, not silently skipped
        // just because a same-numbered session file happens to exist.
        QFile session(QDir(stateDir).filePath(QStringLiteral("session-0.json")));
        QVERIFY(session.open(QIODevice::WriteOnly));
        session.write(QByteArrayLiteral(
            "{\"version\":1,\"activeIndex\":0,\"tabs\":["
            "{\"kind\":\"untitled\",\"slot\":\"untitled-0\",\"cursor\":0,"
            "\"selectionStart\":0,\"selectionEnd\":0}]}"));
        session.close();

        const QString legacyPath = QDir(stateDir).filePath(QStringLiteral("recovery-0.json"));
        QFile legacy(legacyPath);
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        const QJsonObject legacyObject{{QStringLiteral("fileUrl"), QString()},
                                       {QStringLiteral("text"), QStringLiteral("orphaned draft")}};
        legacy.write(QJsonDocument(legacyObject).toJson(QJsonDocument::Compact));
        legacy.close();

        Backend backend;

        QCOMPARE(backend.property("tabs").toList().size(), 2);
        QVERIFY(!QFileInfo::exists(legacyPath));
    }

    void restoresMultipleTabsAndActiveIndexAcrossRestart() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QByteArray originalHome = qgetenv("HOME");
        const QByteArray originalXdgDataHome = qgetenv("XDG_DATA_HOME");
        struct HomeRestorer {
            QByteArray home;
            QByteArray xdgDataHome;
            ~HomeRestorer() {
                qputenv("HOME", home);
                if (xdgDataHome.isNull())
                    qunsetenv("XDG_DATA_HOME");
                else
                    qputenv("XDG_DATA_HOME", xdgDataHome);
            }
        } restoreHome{originalHome, originalXdgDataHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));
        QVERIFY(qputenv("XDG_DATA_HOME",
                        homeDirectory.filePath(QStringLiteral("xdg-data")).toUtf8()));

        QTemporaryDir saveDirectory;
        QVERIFY(saveDirectory.isValid());
        const QString path = saveDirectory.filePath(QStringLiteral("tab-a.md"));

        {
            QQmlEngine engine;
            Backend backend;

            QQmlComponent component0(&engine);
            component0.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                               QUrl::fromLocalFile(saveDirectory.filePath(QStringLiteral("H0.qml"))));
            QScopedPointer<QObject> editor0(component0.create());
            QVERIFY2(editor0, qPrintable(component0.errorString()));
            QObject *tab0 = backend.property("tabs").toList().at(0).value<QObject *>();
            backend.attachTabDocument(tab0, editor0->property("textDocument").value<QObject *>());
            backend.saveAs(QUrl::fromLocalFile(path));

            backend.newTab();
            QQmlComponent component1(&engine);
            component1.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                               QUrl::fromLocalFile(saveDirectory.filePath(QStringLiteral("H1.qml"))));
            QScopedPointer<QObject> editor1(component1.create());
            QVERIFY2(editor1, qPrintable(component1.errorString()));
            QObject *tab1 = backend.property("tabs").toList().at(1).value<QObject *>();
            backend.attachTabDocument(tab1, editor1->property("textDocument").value<QObject *>());
            editor1->setProperty("text", QStringLiteral("second tab draft"));
            QVERIFY(backend.editorTextChangedForTab(tab1));

            backend.setActiveTabIndex(0);
            QTest::qWait(900);
        }

        QQmlEngine engine2;
        Backend restored;
        const QVariantList tabs = restored.property("tabs").toList();
        QCOMPARE(tabs.size(), 2);
        QCOMPARE(restored.activeTabIndex(), 0);

        QQmlComponent component0(&engine2);
        component0.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                           QUrl::fromLocalFile(saveDirectory.filePath(QStringLiteral("H0b.qml"))));
        QScopedPointer<QObject> editor0(component0.create());
        QVERIFY2(editor0, qPrintable(component0.errorString()));
        restored.attachTabDocument(tabs.at(0).value<QObject *>(),
                                   editor0->property("textDocument").value<QObject *>());
        QCOMPARE(editor0->property("text").toString(), QStringLiteral(""));
        QCOMPARE(restored.fileUrl(), QUrl::fromLocalFile(path));

        QQmlComponent component1(&engine2);
        component1.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                           QUrl::fromLocalFile(saveDirectory.filePath(QStringLiteral("H1b.qml"))));
        QScopedPointer<QObject> editor1(component1.create());
        QVERIFY2(editor1, qPrintable(component1.errorString()));
        restored.attachTabDocument(tabs.at(1).value<QObject *>(),
                                   editor1->property("textDocument").value<QObject *>());
        QCOMPARE(editor1->property("text").toString(), QStringLiteral("second tab draft"));
    }

    void closingUntitledTabWithContentRequiresConfirmationThenDiscards() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        Backend backend;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral("import QtQuick\nTextEdit { }"),
                          QUrl::fromLocalFile(directory.filePath(QStringLiteral("Harness.qml"))));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        backend.attachDocument(editor->property("textDocument").value<QObject *>());
        editor->setProperty("text", QStringLiteral("draft content"));
        QVERIFY(backend.editorTextChanged());

        QSignalSpy confirmSpy(&backend, &Backend::closeActiveTabRequiresConfirmation);
        QSignalSpy lastTabSpy(&backend, &Backend::lastTabClosed);
        backend.closeActiveTab();
        QCOMPARE(confirmSpy.count(), 1);
        QCOMPARE(lastTabSpy.count(), 0);
        QCOMPARE(backend.property("tabs").toList().size(), 1);

        backend.discardActiveTab();
        QCOMPARE(backend.property("tabs").toList().size(), 0);
        QCOMPARE(lastTabSpy.count(), 1);
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

        QVERIFY(activeEditorItem(window.data(), backend));
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

        QQuickItem *editor = activeEditorItem(window.data(), backend);
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

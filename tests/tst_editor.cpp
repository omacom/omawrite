#include <QtTest>
#include <QThread>
#include <QScreen>
#include <QCryptographicHash>
#include <atomic>
#include <cstdlib>
#include <QScopeGuard>
#include <QThreadPool>
#include <QSignalSpy>
#include <QItemSelectionModel>
#include <QSemaphore>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QAbstractTextDocumentLayout>
#include <QTextTable>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QStandardPaths>
#include <QClipboard>
#include <QMimeData>
#include <QDesktopServices>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "cli.h"

#include "backend.h"
#include "profile.h"
#include "windowmanager.h"
#include "translator.h"
#include "fileopenrequests.h"
#include <QFileOpenEvent>
#include <QScopeGuard>
#include "markdownhighlighter.h"
#include "livepreview.h"
#include "markdowntablemodel.h"
#include "spellingcontroller.h"

// Counts QML "Binding loop detected" warnings while it is alive, forwarding
// everything to the previous handler so test output stays intact.
class BindingLoopCounter {
public:
    BindingLoopCounter() {
        s_count = 0;
        m_previous = qInstallMessageHandler(handle);
        s_previous = m_previous;
    }
    ~BindingLoopCounter() { qInstallMessageHandler(m_previous); }
    int count() const { return s_count; }

private:
    static void handle(QtMsgType type, const QMessageLogContext &context, const QString &message) {
        if (message.contains(QLatin1String("Binding loop detected")))
            ++s_count;
        if (s_previous)
            s_previous(type, context, message);
    }
    static inline int s_count = 0;
    static inline QtMessageHandler s_previous = nullptr;
    QtMessageHandler m_previous = nullptr;
};

static QList<QQuickItem *> visualDescendants(QQuickItem *parent) {
    QList<QQuickItem *> result;
    for (auto *child : parent->childItems()) { result.append(child); result.append(visualDescendants(child)); }
    return result;
}

class LinkReceiver : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl &url) { urls.append(url); }
};

class OmawriteEditorTest : public QObject {
    Q_OBJECT

    static void activateInputWindow(QQuickWindow *window) {
        // QML shortcuts use the native focus window, not just item.activeFocus.
        // Windows need not activate a newly created/re-exposed fixture window.
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
        }
    }

    static void editorKeyClick(QQuickWindow *window, Qt::Key key,
                               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        // Tests set the canonical source directly. Let its disposable view
        // catch up, then send physical input to the actual visible editor.
        QCoreApplication::processEvents();
        activateInputWindow(window);
        QObject *view = window->findChild<QObject *>(QStringLiteral("previewEditor"));
        QVERIFY(view);
        QVERIFY(QMetaObject::invokeMethod(view, "forceActiveFocus"));
        QTest::keyClick(window, key, modifiers);
        QCoreApplication::processEvents();
    }

    static void openReplace(QQuickWindow *window) {
#ifdef Q_OS_MACOS
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier | Qt::AltModifier);
#else
        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
#endif
    }

private slots:
    void bundledApplicationIconWorksWithoutSystemTheme() {
        const QIcon fallback(QStringLiteral(":/icons/omawrite.png"));
        QVERIFY(!fallback.isNull());
        const auto icon = QIcon::fromTheme(QStringLiteral("omawrite-test-missing-theme-icon"), fallback);
        for (const int size : {16, 32, 48, 256}) {
            const auto pixmap = icon.pixmap(QSize(size, size), 1.0);
            QVERIFY(!pixmap.isNull());
            QCOMPARE(pixmap.size(), QSize(size, size));
        }
    }
    void renamedProfileKeepsDraftStoreAndImportsOnlyMissingPreferences() {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const QString old = temporary.filePath("previous");
        const QString fresh = temporary.filePath("current");
        QVERIFY(QDir().mkpath(old + "/attachments/id"));
        const QByteArray draft = "{\"text\":\"unsaved\",\"attachments\":{\"id\":\"draft.png\"}}";
        QFile file(old + "/recovery-0.json"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(draft); file.close();
        QFile image(old + "/attachments/id/draft.png"); QVERIFY(image.open(QIODevice::WriteOnly)); image.write("original image bytes"); image.close();
        QSettings settings(temporary.filePath("preferences.ini"), QSettings::IniFormat);
        settings.setValue("zoom", 1.4);
        const QVariantMap legacy{{"zoom", 1.0}, {"font", "IBM Plex Mono"}, {"readingPositions/file", 0.6}};
        QVERIFY(Profile::adopt(settings, legacy, old, fresh));
        QCOMPARE(settings.value("zoom").toDouble(), 1.4);
        QCOMPARE(settings.value("font").toString(), QString("IBM Plex Mono"));
        QCOMPARE(settings.value("readingPositions/file").toDouble(), 0.6);
        QCOMPARE(settings.value("profileStorageRoot").toString(), old);
        settings.setValue("font", "new preference");
        QVERIFY(Profile::adopt(settings, legacy, {}, fresh));
        QCOMPARE(settings.value("font").toString(), QString("new preference"));
        QCOMPARE(settings.value("profileStorageRoot").toString(), old);
        QVERIFY(!QFileInfo::exists(fresh)); // No snapshot/image copying or collision.
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), draft);
        QVERIFY(image.open(QIODevice::ReadOnly)); QCOMPARE(image.readAll(), QByteArray("original image bytes"));
        QSettings clean(temporary.filePath("clean.ini"), QSettings::IniFormat);
        QVERIFY(Profile::adopt(clean, {}, temporary.filePath("absent"), fresh));
        QCOMPARE(clean.value("profileStorageRoot").toString(), fresh);
        QCOMPARE(Profile::ipcDirectoryName(), QStringLiteral("omawrite-instance")); // Stable cross-version lock namespace.
        QSettings returning(temporary.filePath("returning.ini"), QSettings::IniFormat);
        const QVariantMap poncho{{"profileStorageRoot", old}, {"profileMigrationVersion", 1}};
        QVERIFY(Profile::adopt(returning, poncho, fresh, fresh));
        QCOMPARE(returning.value("profileStorageRoot").toString(), old);
        QSettings chosen(temporary.filePath("chosen.ini"), QSettings::IniFormat);
        chosen.setValue("profileStorageRoot", old);
        QVERIFY(Profile::adopt(chosen, {}, fresh, fresh));
        QCOMPARE(chosen.value("profileStorageRoot").toString(), old);
        QSettings write(temporary.filePath("write.ini"), QSettings::IniFormat);
        write.setValue("profileMigrationVersion", 1);
        write.setValue("profileStorageRoot", fresh);
        write.setValue("textZoom", 1.0);
        const QVariantMap latest{{"textZoom", 1.5}, {"profileStorageRoot", old}};
        QVERIFY(Profile::adoptWrite(write, latest, old, fresh));
        QCOMPARE(write.value("profileStorageRoot").toString(), old);
        QCOMPARE(write.value("textZoom").toDouble(), 1.5);
        write.setValue("textZoom", 2.0);
        QVERIFY(Profile::adoptWrite(write, latest, fresh, fresh));
        QCOMPARE(write.value("profileStorageRoot").toString(), old);
        QCOMPARE(write.value("textZoom").toDouble(), 2.0);
    }
    void emptyDocumentHintSurvivesStartupFocusAndTracksIme() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *hint = root->findChild<QQuickItem *>("emptyDocumentPlaceholder");
        auto *caret = root->findChild<QQuickItem *>("visibleEditorCaret"); QVERIFY(source && view && hint && caret);
        const auto hintClearsCaret = [hint, caret] {
            return hint->mapToScene(QPointF()).x() > caret->mapToScene(QPointF(caret->width(), 0)).x();
        };
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(view->hasActiveFocus()); QTRY_VERIFY(hint->isVisible()); QTRY_VERIFY(hintClearsCaret());
        backend.setDesktopTextScale(1.7); QTRY_VERIFY(hintClearsCaret());
        backend.setDesktopTextScale(1.0); QTRY_VERIFY(hintClearsCaret());
        auto *document = qvariant_cast<QQuickTextDocument *>(source->property("textDocument"))->textDocument();
        QVERIFY(document->isEmpty()); QVERIFY(!document->isUndoAvailable());
        QTest::keyClick(window, Qt::Key_X); QTRY_VERIFY(!hint->isVisible());
        QTRY_COMPARE(document->toPlainText(), QString("x"));
        QTest::keyClick(window, Qt::Key_Backspace); QTRY_VERIFY(hint->isVisible()); QVERIFY(document->isEmpty());
        QTRY_VERIFY(hintClearsCaret());
        document->clearUndoRedoStacks();
        view->setFocus(false); QTRY_VERIFY(hint->isVisible()); view->forceActiveFocus();
        root->setProperty("readMode", true); QTRY_VERIFY(!hint->isVisible());
        root->setProperty("readMode", false); QTRY_VERIFY(hint->isVisible()); view->forceActiveFocus();
        QInputMethodEvent preedit(QString::fromUtf8("かな"), {}); QCoreApplication::sendEvent(view, &preedit);
        QTRY_VERIFY(view->property("inputMethodComposing").toBool()); QTRY_VERIFY(!hint->isVisible()); QVERIFY(document->isEmpty());
        QInputMethodEvent cancel; QCoreApplication::sendEvent(view, &cancel); QTRY_VERIFY(hint->isVisible());
        QVERIFY(!document->isUndoAvailable());
        QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("かな")); QCoreApplication::sendEvent(view, &commit);
        QTRY_VERIFY(!hint->isVisible()); QTRY_COMPARE(document->toPlainText(), QString::fromUtf8("かな"));
        QVERIFY(!document->toPlainText().contains("Start writing"));
        QVERIFY(QMetaObject::invokeMethod(source, "undo")); QTRY_VERIFY(hint->isVisible()); QTRY_VERIFY(hintClearsCaret());
        QVERIFY(document->isEmpty()); QVERIFY(!document->isUndoAvailable());
        QVERIFY(QMetaObject::invokeMethod(source, "redo")); QTRY_VERIFY(!hint->isVisible());
        QTRY_COMPARE(document->toPlainText(), QString::fromUtf8("かな"));
    }
    void tableScreenshotRegression() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *projection = root->findChild<LivePreview *>(); QVERIFY(source && view && projection);
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        const QString text = "# This is a beautiful app.\n\nI like how everything is displayed\n\n\nhere is a table\n\nasdsa\nasdsada\n\n\n| header 1 | header 2 | header 3 |\n| --- | :---: | --- |\n| pepe | pepe | pepe|";
        source->setProperty("text", text);
        source->setProperty("cursorPosition", 0);
        QTRY_COMPARE(projection->tablePanels().size(), 1);
        const auto panel = projection->tablePanels().first().toMap();
        auto *model = qobject_cast<MarkdownTableModel *>(panel["model"].value<QObject *>()); QVERIFY(model);
        QCOMPARE(model->rowCount(), 2);
        QCOMPARE(model->data(model->index(1, 2), MarkdownTableModel::MarkdownRole).toString(), QString("pepe"));
        auto *document = qvariant_cast<QQuickTextDocument *>(view->property("textDocument"))->textDocument();
        // The blinking caret is unrelated to stale glyphs; exclude it from
        // pixel comparisons without changing the editor's focus/selection.
        auto *caret = root->findChild<QQuickItem *>("visibleEditorCaret"); QVERIFY(caret);
        caret->setOpacity(0);
        QImage baseline;
        const auto headingImage = [window, view] {
            const QPoint p = view->mapToScene(QPointF(0, 0)).toPoint();
            const QImage image = window->grabWindow();
            const qreal scale = image.devicePixelRatio();
            return image.copy(QRect(p.x() * scale, p.y() * scale, view->width() * scale, 80 * scale));
        };
        QTest::qWait(80);
        if (QGuiApplication::platformName() != "offscreen") {
            baseline = headingImage();
            QVERIFY2(!baseline.isNull(), "Native heading capture is required for the glyph regression");
        }
        for (int i = 0; i < 3; ++i) {
            projection->setTableSource(panel["id"].toInt(), true);
            QTRY_VERIFY(projection->tablePanels().isEmpty());
            QTRY_VERIFY(document->toPlainText().contains("| --- | :---: | --- |"));
            QTest::qWait(40);
            source->setProperty("cursorPosition", 0);
            // Leaving the source table should restore the grid automatically.
            QTRY_COMPARE(projection->tablePanels().size(), 1);
            QTRY_VERIFY(!document->toPlainText().contains("| --- |"));
            auto *grid = root->findChild<QQuickItem *>("tableCellView"); QVERIFY(grid);
            const qreal row = std::ceil(QFontMetricsF(qvariant_cast<QFont>(view->property("font"))).height()) + 16;
            QVERIFY(grid->height() >= row * 2);
            QVERIFY(grid->x() >= 12);
            QTest::qWait(80);
            int bodyCells = 0;
            for (auto *item : visualDescendants(grid)) {
                if (item->objectName() != "tableCellDisplay" || !item->property("text").toString().contains("pepe")) continue;
                const QPointF p = item->mapToItem(grid, QPointF());
                QVERIFY(p.y() >= 0 && p.y() + item->height() <= grid->height());
                ++bodyCells;
            }
            QCOMPARE(bodyCells, 3);
            if (!baseline.isNull()) QCOMPARE(headingImage(), baseline);
            QCOMPARE(source->property("text").toString(), text);
        }
    }
    void tableGridEditsTabImeAndViewportKeepCanonicalUndo() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *projection = root->findChild<LivePreview *>(); QVERIFY(source && view && projection);
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        const QString original = "before\n\n| A | B |\n| --- | --- |\n| one | two |\n\nafter";
        source->setProperty("text", original); source->setProperty("cursorPosition", original.size());
        QTRY_COMPARE(projection->tablePanels().size(), 1);
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((grid = root->findChild<QQuickItem *>("tableCellView")));
        QTest::qWait(30);
        const qreal row = std::ceil(QFontMetricsF(qvariant_cast<QFont>(view->property("font"))).height()) + 16;
        auto *document = qvariant_cast<QQuickTextDocument *>(source->property("textDocument"))->textDocument();
        document->clearUndoRedoStacks();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, grid->mapToScene(QPointF(25, row * 1.5)).toPoint());
        QTRY_VERIFY(root->property("activeTableCell").value<QObject *>());
        QPointer<QObject> field = root->property("activeTableCell").value<QObject *>();
        QQuickItem *display = nullptr;
        for (auto *item : visualDescendants(grid)) if (item->objectName() == "tableCellDisplay" && !item->isVisible()) { display = item; break; }
        QVERIFY(display && !display->isVisible()); // No old glyphs beneath the transparent edit field.
        const qreal narrowWidth = field->property("width").toReal();
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (int n = 0; n < 60; ++n) QTest::keyClick(window, Qt::Key_W);
        QTRY_VERIFY(field->property("width").toReal() > narrowWidth + 200);
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const char c : QByteArray("changed")) QTest::keyClick(window, c);
        QCOMPARE(field->property("text").toString(), QString("changed"));
        QInputMethodEvent preedit(QString::fromUtf8("かな"), {}); QCoreApplication::sendEvent(field, &preedit);
        QVERIFY(field->property("inputMethodComposing").toBool());
        window->setWidth(window->width() + 40); QTest::qWait(30);
        QCOMPARE(root->property("activeTableCell").value<QObject *>(), field.data());
        QVERIFY(field->property("inputMethodComposing").toBool()); QCOMPARE(document->toPlainText(), original);
        QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("かな")); QCoreApplication::sendEvent(field, &commit);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(document->toPlainText().contains(QString::fromUtf8("changedかな")));
        QTRY_VERIFY(root->property("activeTableCell").value<QObject *>() != field.data());
        QTest::keyClick(window, Qt::Key_Escape); QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(source, "undo")); QTRY_COMPARE(document->toPlainText(), original); QVERIFY(!document->isUndoAvailable());
        auto *alignedModel = qobject_cast<MarkdownTableModel *>(projection->tablePanels().first().toMap()["model"].value<QObject *>()); QVERIFY(alignedModel);
        QTRY_COMPARE(alignedModel->data(alignedModel->index(1, 0), MarkdownTableModel::MarkdownRole).toString(), QString("one"));
        auto *actions = root->findChild<QObject *>("tableActionsMenu"); QVERIFY(actions);
        QVERIFY(actions->property("width").toReal() >= QFontMetricsF(qvariant_cast<QFont>(actions->property("font"))).horizontalAdvance("Discard pending cell edits") + 40);
        auto *alignCenter = root->findChild<QObject *>("alignColumnCenter"); QVERIFY(alignCenter);
        QVERIFY(QMetaObject::invokeMethod(alignCenter, "triggered"));
        QTRY_VERIFY(document->toPlainText().contains(":---:"));
        QVERIFY(QMetaObject::invokeMethod(source, "undo")); QTRY_COMPARE(document->toPlainText(), original);
        const auto panelId = projection->tablePanels().first().toMap()["id"].toInt();
        projection->setTableSource(panelId, true); QTRY_VERIFY(projection->tablePanels().isEmpty());
        QTRY_VERIFY(qvariant_cast<QQuickTextDocument *>(view->property("textDocument"))->textDocument()->toPlainText().contains("| A | B |"));
        const QString unbordered = "before\n\nA | B\n--- | ---\none | two\n\nafter";
        source->setProperty("text", unbordered); source->setProperty("cursorPosition", unbordered.size());
        QTRY_COMPARE(projection->tablePanels().size(), 1);
        QTRY_VERIFY((grid = root->findChild<QQuickItem *>("tableCellView")));
        QTest::qWait(30);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, grid->mapToScene(QPointF(25, row * 1.5)).toPoint());
        QTRY_VERIFY(root->property("activeTableCell").value<QObject *>());
        field = root->property("activeTableCell").value<QObject *>();
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier); QTest::keyClick(window, Qt::Key_Backspace);
        QCOMPARE(field->property("text").toString(), QString());
        QTest::keyClick(window, Qt::Key_Tab); // Rejected edge-cell clearing must not lose its draft.
        QVERIFY(projection->tableDraftsPending()); QCOMPARE(document->toPlainText(), unbordered);
        auto *tableModel = projection->tablePanels().first().toMap()["model"].value<QObject *>(); QVERIFY(tableModel);
        const auto draft = tableModel->property("drafts").toMap().value("1:0").toMap();
        QVERIFY(draft.contains("text")); QCOMPARE(draft["text"].toString(), QString());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, grid->mapToScene(QPointF(85, row * 1.5)).toPoint());
        QTRY_VERIFY(root->property("activeTableCell").value<QObject *>() != field.data());
        QVariant allowed;
        QVERIFY(QMetaObject::invokeMethod(root.data(), "commitTableCell", Q_RETURN_ARG(QVariant, allowed)));
        QVERIFY(!allowed.toBool()); QVERIFY(projection->tableDraftsPending());
        QVERIFY(projection->copyTableDrafts(0)); QVERIFY(QGuiApplication::clipboard()->text().contains("1:0"));
        projection->discardTableDrafts(); QCOMPARE(document->toPlainText(), unbordered);
        QVERIFY(!projection->tableDraftsPending());
        QString large = "before\n\n| A | B | C | D | E | F | G | H |\n|---|---|---|---|---|---|---|---|\n";
        for (int n = 0; n < 1500; ++n) large += QString("| row%1 | b | c | d | e | f | g | h |\n").arg(n);
        large += "\nafter";
        source->setProperty("text", large); source->setProperty("cursorPosition", 0);
        QTRY_COMPARE(projection->tablePanels().size(), 1); QTest::qWait(50);
        int delegates = 0;
        for (auto *item : visualDescendants(view->parentItem())) if (item->objectName() == "tableCellDelegate") ++delegates;
        QVERIFY(delegates > 0 && delegates < 500); // Not 12,008 rich-text delegates.
        QCOMPARE(document->toPlainText(), large);
    }
    void sharedReadTableAndExpandedViewPreserveModeUndoAndDrafts() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *projection = root->findChild<LivePreview *>(); QVERIFY(source && view && projection);
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        QString original = "| A | B | C | D | E | F | G | H |\n|---|---|---|---|---|---|---|---|\n";
        for (int n = 0; n < 300; ++n) original += QString("| row%1 | b | c | d | e | f | g | h |\n").arg(n);
        source->setProperty("text", original); source->setProperty("cursorPosition", 0);
        QTRY_COMPARE(projection->tablePanels().size(), 1);
        root->setProperty("readMode", true); QTest::qWait(30);
        auto *document = qvariant_cast<QQuickTextDocument *>(source->property("textDocument"))->textDocument();
        document->clearUndoRedoStacks();
        QPointer<QQuickItem> inlinePanel = root->findChild<QQuickItem *>("markdownTableGrid"); QVERIFY(inlinePanel);
        auto *model = inlinePanel->property("modelData").toMap()["model"].value<QObject *>(); QVERIFY(model);
        auto *expand = inlinePanel->findChild<QQuickItem *>("tableExpandButton"); QVERIFY(expand && expand->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, expand->mapToScene(QPointF(expand->width()/2, expand->height()/2)).toPoint());
        auto *dialog = root->findChild<QObject *>("expandedTableDialog"); QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QQuickItem *expanded = nullptr;
        QTRY_VERIFY((expanded = root->findChild<QQuickItem *>("expandedTableGrid")));
        QVERIFY(expanded->property("readOnly").toBool()); QVERIFY(!inlinePanel->isEnabled());
        QCOMPARE(expanded->property("modelData").toMap()["model"].value<QObject *>(), model);
        QVERIFY(expanded->width() > inlinePanel->width()); QVERIFY(expanded->height() > inlinePanel->height());
        for (auto *item : visualDescendants(expanded)) if (item->objectName() == "tableSourceButton" || item->objectName() == "tableActionsButton" || item->objectName() == "tableExpandButton") QVERIFY(!item->isVisible());
        auto *grid = expanded->findChild<QQuickItem *>("tableCellView"); QVERIFY(grid);
        const qreal row = std::ceil(QFontMetricsF(qvariant_cast<QFont>(view->property("font"))).height()) + 16;
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, grid->mapToScene(QPointF(25, row * 1.5)).toPoint());
        QTest::keyClick(window, Qt::Key_X); QTest::qWait(20);
        QVERIFY(!root->property("activeTableCell").value<QObject *>()); QCOMPARE(document->toPlainText(), original); QVERIFY(!document->isUndoAvailable());
        grid->setProperty("contentY", 1000); QTest::qWait(20);
        int cells = 0; for (auto *item : visualDescendants(expanded)) if (item->objectName() == "tableCellDelegate") ++cells;
        QVERIFY(cells > 0 && cells < 400); // Same virtualized component, not all 2,408 cells.
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestClose")); QTRY_VERIFY(!dialog->property("visible").toBool());
        root->setProperty("readMode", false); QTest::qWait(20);
        model->setProperty("scrollY", 0); // Start at the first body row for the Edit modal.
        expand = inlinePanel->findChild<QQuickItem *>("tableExpandButton"); QVERIFY(expand);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, expand->mapToScene(QPointF(expand->width()/2, expand->height()/2)).toPoint());
        QTRY_VERIFY(dialog->property("visible").toBool()); QTRY_VERIFY((expanded = root->findChild<QQuickItem *>("expandedTableGrid")));
        QVERIFY(!expanded->property("readOnly").toBool());
        grid = expanded->findChild<QQuickItem *>("tableCellView"); QVERIFY(grid); QTest::qWait(20);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, grid->mapToScene(QPointF(25, row * 1.5)).toPoint());
        QTRY_VERIFY(root->property("activeTableCell").value<QObject *>());
        QPointer<QObject> field = root->property("activeTableCell").value<QObject *>();
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (char c : QByteArray("modal")) QTest::keyClick(window, c);
        QInputMethodEvent preedit(QString::fromUtf8("かな"), {}); QCoreApplication::sendEvent(field, &preedit);
        window->setWidth(window->width() + 40); QTest::qWait(20);
        QCOMPARE(root->property("activeTableCell").value<QObject *>(), field.data()); QVERIFY(field->property("inputMethodComposing").toBool());
        QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("かな")); QCoreApplication::sendEvent(field, &commit);
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestClose")); QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(document->toPlainText().contains(QString::fromUtf8("modalかな")));
        document->undo(); QTRY_COMPARE(document->toPlainText(), original); QVERIFY(!document->isUndoAvailable());
        const QString unbordered = "A | B\n--- | ---\none | two";
        source->setProperty("text", unbordered); QTRY_COMPARE(projection->tablePanels().size(), 1); QTest::qWait(20);
        inlinePanel = root->findChild<QQuickItem *>("markdownTableGrid"); QVERIFY(inlinePanel);
        expand = inlinePanel->findChild<QQuickItem *>("tableExpandButton"); QVERIFY(expand);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, expand->mapToScene(QPointF(expand->width()/2, expand->height()/2)).toPoint());
        QTRY_VERIFY(dialog->property("visible").toBool()); QTRY_VERIFY((expanded = root->findChild<QQuickItem *>("expandedTableGrid")));
        grid = expanded->findChild<QQuickItem *>("tableCellView"); QVERIFY(grid); QTest::qWait(20);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, grid->mapToScene(QPointF(25, row * 1.5)).toPoint());
        QTRY_VERIFY(root->property("activeTableCell").value<QObject *>());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier); QTest::keyClick(window, Qt::Key_Backspace);
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestClose")); QVERIFY(dialog->property("visible").toBool());
        QVERIFY(projection->tableDraftsPending()); QCOMPARE(document->toPlainText(), unbordered);
        QVERIFY(QMetaObject::invokeMethod(root.data(), "toggleReadMode")); QVERIFY(!root->property("readMode").toBool());
        // Cocoa may refuse activation for a test-launched window. Native
        // WindowShortcut dispatch is then unavailable, despite internal focus.
        // Exercise the same handler without stealing the writer's OS focus;
        // active native windows and offscreen still receive physical Escape.
        if (QGuiApplication::platformName() == "cocoa" && !window->isActive()) {
            auto *active = root->property("activeTableCell").value<QObject *>(); QVERIFY(active);
            auto *cancel = active->findChild<QObject *>("tableCancelCellShortcut"); QVERIFY(cancel);
            QVERIFY(QMetaObject::invokeMethod(cancel, "activated"));
        } else QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestClose")); QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(!projection->tableDraftsPending()); QCOMPARE(document->toPlainText(), unbordered);
    }
    void readGridLinksAndClipboardRetainTableContents() {
        QTemporaryDir directory; Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create()); auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor"); auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *projection = root->findChild<LivePreview *>(); QVERIFY(source && view && projection);
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        const QString table = "| Link | Auto |\n| --- | --- |\n| [guide](https://example.invalid/explicit) | <https://example.invalid/auto> |\n| ![x](https://example.invalid/image.png)![x](https://example.invalid/image.png) | safe |";
        const QString text = "before\n\n" + table + "\n\nmiddle\n\n" + table + "\n\nafter";
        source->setProperty("text", text); source->setProperty("cursorPosition", 0); QTest::qWait(30);
        QVERIFY(QMetaObject::invokeMethod(root.data(), "toggleReadMode")); QTRY_COMPARE(projection->tablePanels().size(), 2);
        auto *inlinePanel = root->findChild<QQuickItem *>("markdownTableGrid"); QVERIFY(inlinePanel); QTest::qWait(30);
        LinkReceiver receiver; QDesktopServices::setUrlHandler("https", &receiver, "open");
        const auto cleanup = qScopeGuard([] { QDesktopServices::unsetUrlHandler("https"); });
        auto clickLink = [&](QQuickItem *panel, const QString &url) {
            auto *grid = panel->findChild<QQuickItem *>("tableCellView");
            if (!grid) return false;
            for (int attempt = 0; attempt < 3; ++attempt) {
                QPointer<QQuickItem> label; QPointF point;
                for (auto *item : visualDescendants(panel)) if (!label && item->objectName() == "tableCellDisplay" && item->isVisible() && item->property("text").toString().contains(url)) {
                    // Hit-test an actual rich-text anchor, not a wrapped label's
                    // geometric center (which can fall between text lines).
                    for (qreal y = 1; !label && y < item->height(); y += 2) for (qreal x = 1; x < item->width(); x += 2) {
                        QString target;
                        if (!QMetaObject::invokeMethod(item, "linkAt", Q_RETURN_ARG(QString, target), Q_ARG(qreal, x), Q_ARG(qreal, y))) return false;
                        if (target == url) { label = item; point = QPointF(x,y); break; }
                    }
                }
                if (!label) return false;
                const auto inGrid = label->mapToItem(grid, point);
                // A cell can be loaded yet clipped horizontally, especially
                // with large fonts/raw-image destinations in another row.
                if (inGrid.x() < 1 || inGrid.x() > grid->width()-2) {
                    grid->setProperty("contentX", grid->property("contentX").toReal() + inGrid.x() - grid->width()/2);
                    QTest::qWait(30); // Reacquire possibly recycled delegates.
                    continue;
                }
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, label->mapToScene(point).toPoint());
                return true;
            }
            return false;
        };
        for (int mode = 0; mode < 2; ++mode) {
            QQuickItem *panel = inlinePanel;
            if (mode) {
                auto *expand = inlinePanel->findChild<QQuickItem *>("tableExpandButton"); QVERIFY(expand);
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, expand->mapToScene(QPointF(expand->width()/2, expand->height()/2)).toPoint());
                QTRY_VERIFY((panel = root->findChild<QQuickItem *>("expandedTableGrid"))); QTest::qWait(30);
            }
            QVERIFY(clickLink(panel, "https://example.invalid/explicit")); QTRY_COMPARE(receiver.urls.size(), mode * 2 + 1);
            QCOMPARE(receiver.urls.last(), QUrl("https://example.invalid/explicit"));
            QVERIFY(clickLink(panel, "https://example.invalid/auto")); QTRY_COMPARE(receiver.urls.size(), mode * 2 + 2);
            QCOMPARE(receiver.urls.last(), QUrl("https://example.invalid/auto"));
        }
        auto *dialog = root->findChild<QObject *>("expandedTableDialog"); QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestClose")); QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(view, "selectAll")); projection->copySource();
        const QString plain = QGuiApplication::clipboard()->text();
        QVERIFY(plain.contains("before") && plain.contains("middle") && plain.contains("after"));
        QCOMPARE(plain.count("guide"), 2); QCOMPARE(plain.count("https://example.invalid/auto"), 2);
        QVERIFY(!plain.contains(QChar::ObjectReplacementCharacter));
        QVERIFY(QMetaObject::invokeMethod(root.data(), "copyHtml"));
        const auto *mime = QGuiApplication::clipboard()->mimeData(); QVERIFY(mime->hasHtml());
        QCOMPARE(mime->text(), mime->html()); QCOMPARE(mime->html().count("<table"), 2);
        QVERIFY(mime->html().contains("guide")); QVERIFY(!mime->html().contains("preview:"));
        QVERIFY(!mime->html().contains("<img", Qt::CaseInsensitive)); QVERIFY(!mime->html().contains("image.png"));
        QVERIFY(QMetaObject::invokeMethod(view, "deselect")); projection->copySource(); QCOMPARE(QGuiApplication::clipboard()->text(), text);
        QCOMPARE(source->property("text").toString(), text);
    }
    void overflowScrollbarsTrackViewportAndCaretBothDirections() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *flick = root->findChild<QQuickItem *>("editorScroll");
        auto *projection = root->findChild<LivePreview *>(); QVERIFY(source && view && flick && projection);
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        QString text = "before\n\n```text\n";
        for (int n = 0; n < 400; ++n) text += QString("line%1 ").arg(n) + QString("wide_identifier ").repeated(35) + "\n";
        text += "```\n\nafter";
        source->setProperty("text", text); source->setProperty("cursorPosition", text.indexOf("line200 "));
        QTRY_COMPARE(projection->codeBlockPanels().size(), 1); QTest::qWait(30);
        view->forceActiveFocus();
        view->setProperty("cursorPosition", text.indexOf("line200 "));
        const bool cocoa = QGuiApplication::platformName() == "cocoa";
        QTest::keyClick(window, cocoa ? Qt::Key_Right : Qt::Key_End, cocoa ? Qt::ControlModifier : Qt::NoModifier);
        const auto id = projection->codeBlockPanels().first().toMap()["id"].toInt();
        QTRY_VERIFY(projection->blockOffsets().value(QString::number(id)).toReal() > 100);
        QTest::keyClick(window, cocoa ? Qt::Key_Left : Qt::Key_Home, cocoa ? Qt::ControlModifier : Qt::NoModifier);
        QTRY_VERIFY(projection->blockOffsets().value(QString::number(id)).toReal() < 1);
        QVERIFY(view->property("displayCursorRectangle").toRectF().x() >= -1);
        flick->setProperty("contentY", flick->property("contentY").toReal() + 120);
        QTest::qWait(30);
        QQuickItem *bar = nullptr;
        for (auto *item : visualDescendants(view->parentItem())) if (item->objectName() == "blockHorizontalScrollBar" && item->isVisible()) { bar = item; break; }
        QVERIFY(bar);
        const auto viewport = flick->mapRectToScene(QRectF(0, 0, flick->width(), flick->height()));
        const auto scrollRect = bar->mapRectToScene(QRectF(0, 0, bar->width(), bar->height()));
        QVERIFY(qAbs(scrollRect.bottom() - viewport.bottom()) < 3); // Not thousands of pixels below us.
        source->setProperty("cursorPosition", 0); flick->setProperty("contentY", 0); QTest::qWait(30);
        // The document's positive top inset must not push the pinned bar below
        // the viewport when the viewport origin is negative in text coordinates.
        bar = nullptr;
        for (auto *item : visualDescendants(view->parentItem())) if (item->objectName() == "blockHorizontalScrollBar" && item->isVisible()) { bar = item; break; }
        QVERIFY(bar);
        QVERIFY(qAbs(bar->mapRectToScene(QRectF(0, 0, bar->width(), bar->height())).bottom() - viewport.bottom()) < 3);
        const QString tableText = "before\n\n| Left heading | Right heading |\n| --- | --- |\n| " + QString("W").repeated(160) + " | right cell |\n\nafter\n\n| Another heading | Last heading |\n| --- | --- |\n| " + QString("Z").repeated(140) + " | other cell |";
        source->setProperty("text", tableText); source->setProperty("cursorPosition", 0);
        root->setProperty("readMode", true);
        QTRY_COMPARE(projection->tablePanels().size(), 2); flick->setProperty("contentY", 0); QTest::qWait(30);
        QVERIFY(projection->tableScrollPanels().isEmpty()); // Supported Read tables use the same grid, not native frames.
        const auto firstTable = projection->tablePanels().first().toMap();
        const int tableId = firstTable["id"].toInt();
        auto *model = qvariant_cast<QObject *>(firstTable["model"]); QVERIFY(model);
        QPointer<QQuickItem> readGrid, readBar;
        for (auto *item : visualDescendants(view->parentItem())) if (item->objectName() == "markdownTableGrid"
            && item->property("modelData").toMap()["id"].toInt() == tableId) { readGrid = item; break; }
        QVERIFY(readGrid); QVERIFY(readGrid->property("readOnly").toBool());
        for (auto *item : visualDescendants(readGrid)) {
            if (item->objectName() == "blockHorizontalScrollBar" && item->isVisible()) readBar = item;
            if (item->objectName() == "tableSourceButton" || item->objectName() == "tableActionsButton") QVERIFY(!item->isVisible());
        }
        const auto region = readGrid->mapRectToScene(QRectF(1, 32, readGrid->width() - 2, readGrid->height() - 46)).toAlignedRect();
        QImage before; QRect pixelRegion = region;
        if (QGuiApplication::platformName() != "offscreen") {
            before = window->grabWindow(); QVERIFY(!before.isNull());
            const qreal scale = qreal(before.width()) / window->width();
            pixelRegion = QRectF(region.x()*scale, region.y()*scale, region.width()*scale, region.height()*scale).toAlignedRect();
        }
        QVERIFY(readBar);
        const auto press = readBar->mapToScene(QPointF(readBar->width() * readBar->property("size").toReal() / 2, readBar->height() / 2)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, press);
        QScopeGuard release([&] { QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, press + QPoint(120, 0)); });
        QTRY_VERIFY(readBar && readBar->property("pressed").toBool());
        for (int dx : {20, 40, 60, 80, 100, 120}) {
            QTest::mouseMove(window, press + QPoint(dx, 0), 10); QCoreApplication::processEvents();
            QVERIFY(readBar); // Publishing new native table bounds must not destroy the mouse grabber.
            QVERIFY(readBar->property("pressed").toBool());
        }
        const qreal offset = model->property("scrollX").toReal();
        QVERIFY(offset > 100);
        QTest::qWait(30);
        if (!before.isNull()) QVERIFY(before.copy(pixelRegion) != window->grabWindow().copy(pixelRegion)); // Visible grid glyphs actually moved, including Retina.
        QTest::mouseMove(window, press + QPoint(20, 0), 10); QCoreApplication::processEvents();
        QVERIFY(readBar && readBar->property("pressed").toBool());
        QVERIFY(model->property("scrollX").toReal() < offset / 2);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, press + QPoint(20, 0)); release.dismiss();
        const auto wheelPoint = readBar->mapToScene(QPointF(readBar->width() / 2, -25));
        const qreal beforeWheel = model->property("scrollX").toReal();
        QWheelEvent wheel(wheelPoint, window->mapToGlobal(wheelPoint.toPoint()), QPoint(-80, 0), QPoint(), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
        QCoreApplication::sendEvent(window, &wheel);
        QTRY_VERIFY(model->property("scrollX").toReal() > beforeWheel);
        QVERIFY(readBar);
        const qreal beforeTouchpad = model->property("scrollX").toReal();
        QWheelEvent touchpad(wheelPoint, window->mapToGlobal(wheelPoint.toPoint()), QPoint(40, 0), QPoint(), Qt::NoButton, Qt::NoModifier, Qt::ScrollEnd, false, Qt::MouseEventSynthesizedBySystem);
        QCoreApplication::sendEvent(window, &touchpad);
        QTRY_VERIFY(model->property("scrollX").toReal() < beforeTouchpad);
        const auto second = projection->tablePanels().last().toMap();
        QVERIFY(second["id"].toInt() != tableId);
        QCOMPARE(qvariant_cast<QObject *>(second["model"])->property("scrollX").toReal(), 0.0);
        QCOMPARE(qvariant_cast<QQuickTextDocument *>(source->property("textDocument"))->textDocument()->toPlainText(), tableText);
        root->setProperty("readMode", false);
        QTRY_VERIFY(readGrid && !readGrid->property("readOnly").toBool());
        root->setProperty("readMode", true);
        QTRY_COMPARE(projection->tablePanels().size(), 2);
        QVERIFY(readGrid->property("readOnly").toBool());
    }
    void tableRectangleDragTypingImeClearAndUndo() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString())); QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        if (QGuiApplication::platformName() == "windows") {
            // Leave room for native frames at fractional DPI instead of letting
            // the OS repeatedly clamp the default 1280x820 during the gesture.
            const QSize available = window->screen()->availableGeometry().size() - QSize(64, 64);
            window->resize(QSize(1000, 700).boundedTo(available));
        }
        auto *source = root->findChild<QObject *>("sourceEditor"); auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *projection = root->findChild<LivePreview *>(); QVERIFY(source && view && projection);
        QString text = "before\n\n| A | B | C";
        for (int c = 3; c < 12; ++c) text += QString(" | heading%1").arg(c);
        text += " |\n|" + QString(" --- |").repeated(12) + "\n";
        for (int r = 0; r < 40; ++r) {
            text += QString("| a%1 | b%1 | untouched%1").arg(r);
            for (int c = 3; c < 12; ++c) text += " | " + QString(5 + (c % 4) * 10, 'W');
            text += " |\n";
        }
        text += "\nafter"; source->setProperty("text", text); source->setProperty("cursorPosition", 0);
        QTRY_COMPARE(projection->tablePanels().size(), 1); QTest::qWait(30);
        if (QGuiApplication::platformName() != "offscreen")
            QVERIFY(QTest::qWaitForWindowExposed(window));
        activateInputWindow(window);
        if (QTest::currentTestFailed()) return;
        QQuickItem *panel = nullptr, *grid = nullptr;
        for (auto *item : visualDescendants(view->parentItem())) if (item->objectName() == "markdownTableGrid") panel = item;
        QVERIFY(panel);
        for (auto *item : visualDescendants(panel)) if (item->objectName() == "tableCellView") grid = item;
        QVERIFY(grid);
        auto *selection = qvariant_cast<QItemSelectionModel *>(grid->property("selectionModel")); QVERIFY(selection);
        auto *model = qobject_cast<MarkdownTableModel *>(selection->model()); QVERIFY(model);
        auto endpoints = [&](const QString &name, bool horizontal) {
            QPointer<QQuickItem> bar;
            for (auto *item : visualDescendants(panel)) if (item->objectName() == name) bar = item;
            QVERIFY(bar && bar->isVisible());
            const qreal length = horizontal ? bar->width() : bar->height();
            const auto point = [&](qreal p) { return bar->mapToScene(horizontal ? QPointF(p, bar->height()/2) : QPointF(bar->width()/2, p)).toPoint(); };
            auto *thumb = qvariant_cast<QQuickItem *>(bar->property("contentItem"));
            QVERIFY(thumb && thumb->width() > 0 && thumb->height() > 0);
            const auto start = thumb->mapToScene(QPointF(thumb->width()/2, thumb->height()/2)).toPoint();
            // Native styles can pad the track and enforce a minimum thumb size.
            // Drag the actual thumb to both track edges, without synthesizing
            // coordinates outside a window clamped to the available screen.
            const auto finish = point(length - 1), back = point(1);
            const auto details = [&] {
                return QString("%1 read=%2 position=%3 size=%4 content=%5 origin=%6 first=%7 last=%8 press=(%9,%10) end=(%11,%12) window=%13x%14 length=%15 pressed=%16")
                    .arg(name).arg(panel->property("readOnly").toBool())
                    .arg(bar->property("position").toReal()).arg(bar->property("size").toReal())
                    .arg(grid->property(horizontal ? "contentX" : "contentY").toReal())
                    .arg(grid->property(horizontal ? "originX" : "originY").toReal())
                    .arg(grid->property(horizontal ? "leftColumn" : "topRow").toInt())
                    .arg(grid->property(horizontal ? "rightColumn" : "bottomRow").toInt())
                    .arg(start.x()).arg(start.y()).arg(finish.x()).arg(finish.y())
                    .arg(window->width()).arg(window->height())
                    .arg(horizontal ? bar->width() : bar->height()).arg(bar->property("pressed").toBool());
            };
            qInfo().noquote() << "Before drag:" << details();
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            QScopeGuard release([&] { QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, back); });
            QTRY_VERIFY2(bar->property("pressed").toBool(), qPrintable(details()));
            // Keep neighboring rows/columns loaded while crossing the track.
            // Teleporting across a virtualized TableView makes it estimate a
            // new origin/end extent after the final mouse move.
            const auto drag = [&](const QPoint &from, const QPoint &to) {
                for (int step = 1; step <= 16; ++step) {
                    QTest::mouseMove(window, from + (to - from) * step / 16, 10);
                    QTest::qWait(20);
                }
            };
            drag(start, finish);
            const char *content = horizontal ? "contentX" : "contentY", *origin = horizontal ? "originX" : "originY";
            QTRY_VERIFY2(bar->property("position").toReal() + bar->property("size").toReal() > .999, qPrintable(details()));
            QVERIFY(grid->property(horizontal ? "atXEnd" : "atYEnd").toBool());
            QTRY_COMPARE(grid->property(horizontal ? "rightColumn" : "bottomRow").toInt(), horizontal ? model->columnCount() - 1 : model->rowCount() - 1);
            drag(finish, back);
            QTRY_VERIFY2(qAbs(grid->property(content).toReal() - grid->property(origin).toReal()) < 2, qPrintable(details()));
            QTRY_VERIFY2(grid->property(horizontal ? "leftColumn" : "topRow").toInt() == 0, qPrintable(details()));
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, back); release.dismiss();
            QVERIFY(bar); QTest::qWait(20);
        };
        endpoints("blockHorizontalScrollBar", true);
        if (QTest::currentTestFailed()) return;
        endpoints("tableVerticalScrollBar", false);
        if (QTest::currentTestFailed()) return;
        auto cellPoint = [&](int r, int c) {
            for (auto *item : visualDescendants(grid)) if (item->objectName() == "tableCellDelegate"
                && item->property("row").toInt() == r && item->property("column").toInt() == c)
                return item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint();
            return QPoint(-1, -1);
        };
        auto *document = qvariant_cast<QQuickTextDocument *>(source->property("textDocument"))->textDocument(); document->clearUndoRedoStacks();
        const auto first = cellPoint(1, 0), last = cellPoint(3, 1); QVERIFY(first.x() >= 0 && last.x() >= 0);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, first);
        for (int i = 1; i <= 8; ++i) QTest::mouseMove(window, first + (last - first) * i / 8, 10);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, last);
        QTRY_COMPARE(selection->selectedIndexes().size(), 6);
        QVERIFY(!qvariant_cast<QObject *>(panel->property("activeEditor")));
        QInputMethodQueryEvent query(Qt::ImEnabled | Qt::ImCursorRectangle); QCoreApplication::sendEvent(grid, &query);
        QVERIFY(query.value(Qt::ImEnabled).toBool());
        QInputMethodEvent preedit(QString::fromUtf8("かな"), {}); QCoreApplication::sendEvent(window, &preedit); QCOMPARE(document->toPlainText(), text);
        QTRY_VERIFY(qvariant_cast<QObject *>(panel->property("activeEditor")));
        QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("かな")); QCoreApplication::sendEvent(window, &commit);
        for (Qt::Key key : {Qt::Key_B, Qt::Key_U, Qt::Key_L, Qt::Key_K}) QTest::keyClick(window, key);
        auto *field = qvariant_cast<QObject *>(panel->property("activeEditor")); QCOMPARE(field->property("text").toString(), QString::fromUtf8("かなbulk"));
        QCOMPARE(document->toPlainText(), text);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!qvariant_cast<QObject *>(panel->property("activeEditor")));
        for (int r = 1; r <= 3; ++r) for (int c = 0; c < 2; ++c)
            QCOMPARE(model->data(model->index(r, c), MarkdownTableModel::MarkdownRole).toString(), QString::fromUtf8("かなbulk"));
        QCOMPARE(model->data(model->index(2, 2), MarkdownTableModel::MarkdownRole).toString(), QString("untouched1"));
        document->undo(); QTRY_COMPARE(document->toPlainText(), text); QVERIFY(!document->isUndoAvailable());
        QTest::qWait(30); selection->select(QItemSelection(model->index(1, 0), model->index(3, 1)), QItemSelectionModel::ClearAndSelect);
        selection->setCurrentIndex(model->index(1, 0), QItemSelectionModel::NoUpdate); grid->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Backspace);
        QTRY_VERIFY(model->data(model->index(1, 0), MarkdownTableModel::MarkdownRole).toString().isEmpty());
        for (int r = 1; r <= 3; ++r) for (int c = 0; c < 2; ++c) QVERIFY(model->data(model->index(r, c), MarkdownTableModel::MarkdownRole).toString().isEmpty());
        document->undo(); QTRY_COMPARE(document->toPlainText(), text); QVERIFY(!document->isUndoAvailable());
        root->setProperty("readMode", true); QTRY_VERIFY(panel->property("readOnly").toBool()); QTest::qWait(30);
        endpoints("blockHorizontalScrollBar", true); endpoints("tableVerticalScrollBar", false);
        QCOMPARE(document->toPlainText(), text);
    }
    void codePanelHoverCopyAndReadLanguageUseQtClipboard() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *projection = root->findChild<LivePreview *>();
        QVERIFY(source && view && projection);
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        const QString text = "```json\n{\"value\":42}\n```\n\n```\nplain\n```\n";
        source->setProperty("text", text); source->setProperty("cursorPosition", text.size());
        for (bool read : {false, true}) {
            root->setProperty("readMode", read);
            QTRY_COMPARE(projection->codeBlockPanels().size(), 2);
            QTRY_COMPARE(projection->codeBlockPanels().first().toMap().value("readMode").toBool(), read);
            QCoreApplication::processEvents();
            QQuickItem *controls = nullptr;
            for (auto *item : visualDescendants(view->parentItem())) {
                if (item->objectName() != "codeBlockControls") continue;
                if (!controls || item->y() < controls->y()) controls = item;
                auto *label = item->findChild<QQuickItem *>("codeBlockLanguage"); QVERIFY(label);
                QCOMPARE(label->isVisible(), read);
            }
            QVERIFY(controls);
            auto *button = controls->findChild<QQuickItem *>("codeBlockCopyButton");
            auto *label = controls->findChild<QQuickItem *>("codeBlockLanguage");
            QVERIFY(button && label); QCOMPARE(label->property("text").toString(), QString("json"));
            QTest::mouseMove(window, QPoint(5, 5));
            QTRY_COMPARE(button->opacity(), qreal(0));
            QTest::mouseMove(window, controls->mapToScene(QPointF(controls->width() / 2, controls->height() / 2)).toPoint());
            QTRY_COMPARE(button->opacity(), qreal(1));
            QVERIFY(button->isEnabled());
            auto *document = view->property("textDocument").value<QQuickTextDocument *>()->textDocument();
            const int position = document->find("value").selectionStart();
            QVERIFY(QMetaObject::invokeMethod(view, "select", Q_ARG(int, position), Q_ARG(int, position + 5)));
            QVERIFY(QMetaObject::invokeMethod(view, "forceActiveFocus"));
            const auto selection = view->property("selectedText").toString();
            QGuiApplication::clipboard()->setText("before");
            QSignalSpy clicked(button, SIGNAL(clicked()));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
            QCOMPARE(clicked.count(), 1);
            QTRY_COMPARE(QGuiApplication::clipboard()->text(), QString("{\"value\":42}"));
            QCOMPARE(view->property("selectedText").toString(), selection);
            QVERIFY(view->hasActiveFocus());
            QCOMPARE(source->property("text").toString(), text);
            QVERIFY(!source->property("canUndo").toBool());
            QTest::mouseMove(window, QPoint(5, 5));
            QTRY_COMPARE(button->opacity(), qreal(0));
        }
    }
    void codePanelsHaveNativePaddingInBothModesAtDifferentZooms() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const int font = QFontDatabase::addApplicationFont(QFINDTESTDATA("../fonts/IBMPlexMono-Regular.ttf"));
        QVERIFY(font >= 0);
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        const qreal previousZoom = backend.m_textZoom;
        const QString previousFont = backend.editorFont();
        const auto restoreSettings = qScopeGuard([&] { backend.setTextZoom(previousZoom); backend.setEditorFont(previousFont); });
        backend.setEditorFont(QFontDatabase::applicationFontFamilies(font).first());
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *projection = root->findChild<LivePreview *>();
        QVERIFY(source && view && projection);
        if (QGuiApplication::platformName() != "offscreen")
            QVERIFY(QTest::qWaitForWindowExposed(window));
        const QString text = "Testing background\n\n```json\n{\"this\":\"that\"}\n```\n";
        source->setProperty("text", text); source->setProperty("cursorPosition", text.size());
        for (bool dark : {true, false}) {
            backend.setDarkMode(dark);
            for (bool read : {false, true}) {
                root->setProperty("readMode", read);
                for (qreal zoom : {1.0, 1.6}) {
                    backend.setTextZoom(zoom);
                    QCoreApplication::processEvents(); QCoreApplication::processEvents();
                    QTRY_COMPARE(projection->codeBlockPanels().size(), 1);
                    auto *document = view->property("textDocument").value<QQuickTextDocument *>()->textDocument();
                    const auto block = document->find("\"this\"").block(); QVERIFY(block.isValid());
                    QRectF first, last;
                    const int firstPosition = read ? block.position() : block.previous().position();
                    const int lastPosition = read ? block.position() : block.next().position();
                    QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, first), Q_ARG(int, firstPosition)));
                    QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, last), Q_ARG(int, lastPosition)));
                    const auto panel = projection->codeBlockPanels().first().toMap();
                    const qreal pixels = qvariant_cast<QFont>(view->property("font")).pixelSize();
                    QVERIFY(first.top() - panel.value("y").toReal() >= pixels / 2);
                    QVERIFY(panel.value("y").toReal() + panel.value("height").toReal() - last.bottom() >= pixels / 2);
                    if (QGuiApplication::platformName() != "offscreen") {
                        const auto image = window->grabWindow(); QVERIFY(!image.isNull());
                        const QPointF point = view->mapToScene(QPointF(view->width() - 20, last.bottom() + pixels / 4));
                        const qreal scale = image.width() / qreal(window->width());
                        QCOMPARE(image.pixelColor(qRound(point.x() * scale), qRound(point.y() * scale)),
                                 QColor(dark ? "#252525" : "#eeeeee"));
                    }
                    QCOMPARE(source->property("text").toString(), text);
                }
            }
        }
        QVERIFY(!source->property("canUndo").toBool());
    }
    void quotePreviewPhysicalClickImeAndReadBreaks() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        const bool previousDark = backend.darkMode();
        const auto restore = qScopeGuard([&] { backend.setDarkMode(previousDark); });
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor"); QVERIFY(source && view);
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto document = [view] { return view->property("textDocument").value<QQuickTextDocument *>()->textDocument(); };
        const QString text = "Before.\n\n> quote body " + QString("words ").repeated(25) + "\n> second line\n\nAfter.\n";
        const int at = text.indexOf("> quote");
        for (bool dark : {false, true}) {
            backend.setDarkMode(dark); source->setProperty("text", text);
            source->setProperty("cursorPosition", text.size());
            QTRY_COMPARE(document()->toPlainText(), text);
            auto block = document()->findBlock(at);
            const auto prefixAlpha = [&] {
                for (const auto &range : block.layout()->formats()) if (range.start == 0) return range.format.foreground().color().alpha();
                return -1;
            };
            QTRY_COMPARE(block.blockFormat().intProperty(MarkdownHighlighter::QuotePrefixProperty), 2);
            QTRY_COMPARE(prefixAlpha(), 0);
            QCOMPARE(block.blockFormat().alignment(), Qt::Alignment(Qt::AlignJustify));
            QRectF rectangle;
            QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, rectangle), Q_ARG(int, at + 8)));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, view->mapToScene(rectangle.center()).toPoint());
            QTRY_VERIFY(prefixAlpha() > 0);
            QVERIFY(source->property("cursorPosition").toInt() > at);
            QCOMPARE(source->property("text").toString(), text); QVERIFY(!source->property("canUndo").toBool());
            QInputMethodEvent preedit(QString::fromUtf8("かな"), {}); QCoreApplication::sendEvent(view, &preedit);
            QCOMPARE(source->property("text").toString(), text);
            QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("かな")); QCoreApplication::sendEvent(view, &commit);
            QTRY_VERIFY(source->property("text").toString().contains(QString::fromUtf8("かな")));
            QVERIFY(QMetaObject::invokeMethod(source, "undo"));
            QTRY_COMPARE(source->property("text").toString(), text);
            QVERIFY(QMetaObject::invokeMethod(root.data(), "toggleReadMode")); QTRY_VERIFY(root->property("readMode").toBool());
            QTRY_VERIFY(document()->toRawText().contains(QString(" second line")));
            QVERIFY(!document()->toRawText().contains(QChar::LineSeparator));
            QVERIFY(!document()->toPlainText().contains("PWHARDBREAK"));
            QCOMPARE(source->property("text").toString(), text);
            QVERIFY(QMetaObject::invokeMethod(root.data(), "toggleReadMode")); QTRY_VERIFY(!root->property("readMode").toBool());
            QTRY_COMPARE(document()->toPlainText(), text);
        }
    }
    void horizontalRulesRevealMarkdownOnPhysicalClick() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        QVERIFY(source && view);
        if (QGuiApplication::platformName() != "offscreen")
            QVERIFY(QTest::qWaitForWindowExposed(window));
        // attach() supplies the safe presentation document on a queued turn.
        const auto document = [view] { return view->property("textDocument").value<QQuickTextDocument *>()->textDocument(); };
        const QString text = "Before.\n\n---\n\nAfter.\n";
        source->setProperty("text", text);
        for (bool dark : {false, true}) {
            backend.setDarkMode(dark);
            source->setProperty("cursorPosition", text.size());
            QTRY_COMPARE(document()->toPlainText().count(QChar::ObjectReplacementCharacter), 1);
            const int object = document()->toPlainText().indexOf(QChar::ObjectReplacementCharacter);
            QRectF rectangle;
            QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, rectangle), Q_ARG(int, object)));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                view->mapToScene(QPointF(view->width() / 2, rectangle.center().y())).toPoint());
            QTRY_VERIFY(!document()->toPlainText().contains(QChar::ObjectReplacementCharacter));
            QVERIFY(document()->toPlainText().contains("---"));
            QCOMPARE(source->property("text").toString(), text);
            QVERIFY(!source->property("canUndo").toBool());
        }
    }
    void codeBlockPanelsAreContinuousDecorationsInBothThemes() {
        QTemporaryDir directory;
        Backend backend;
        backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        QVERIFY(source && view);
        const auto panels = [&] {
            QList<QQuickItem *> result;
            for (auto *item : visualDescendants(view->parentItem()))
                if (item->objectName() == "codeBlockPanel") result.append(item);
            return result;
        };
        const QString text = "Before.\n\n```cpp\nint value = 1;\n\n// comment\n```\n\nAfter.\n";
        source->setProperty("text", text);
        source->setProperty("cursorPosition", text.size());
        for (bool dark : {true, false}) {
            backend.setDarkMode(dark);
            for (bool read : {false, true}) {
                root->setProperty("readMode", read);
                QCoreApplication::processEvents();
                QCoreApplication::processEvents();
                QTRY_COMPARE(panels().size(), 1);
                auto *panel = panels().first();
                QTRY_COMPARE(panel->property("color").value<QColor>(), QColor(dark ? "#252525" : "#eeeeee"));
                QVERIFY(panel->z() < 0 || panel->parentItem()->z() < 0);
                QVERIFY(panel->width() >= view->width());
                QVERIFY(panel->height() > 3 * view->property("font").value<QFont>().pixelSize());
                QVERIFY(panel->property("radius").toReal() > 0);
                const auto border = panel->property("border").value<QObject *>();
                QVERIFY(border);
                QCOMPARE(border->property("width").toReal(), 0.5);
                if (QGuiApplication::platformName() != "offscreen") {
                    QVERIFY(QTest::qWaitForWindowExposed(window));
                    activateInputWindow(window);
                    const auto fillIsRendered = [&] {
                        if (panels().size() != 1) return false;
                        const auto *panel = panels().first();
                        const QImage frame = window->grabWindow();
                        if (frame.isNull()) return false;
                        const QPointF point = panel->mapToScene(QPointF(panel->width() - 20, panel->height() / 2));
                        const QPoint pixel(qRound(point.x() * frame.width() / window->width()),
                                           qRound(point.y() * frame.height() / window->height()));
                        if (!frame.rect().contains(pixel)) return false;
                        const QColor actual = frame.pixelColor(pixel), expected = panel->property("color").value<QColor>();
                        return qAbs(actual.red() - expected.red()) <= 3
                            && qAbs(actual.green() - expected.green()) <= 3 && qAbs(actual.blue() - expected.blue()) <= 3;
                    };
                    QTRY_VERIFY(fillIsRendered()); // Empty right-hand area is filled, not just glyph strips.
                    if (!read) {
                        auto *projection = root->findChild<LivePreview *>();
                        QVERIFY(projection);
                        bool stayedVisible = true;
                        const auto watch = connect(projection, &LivePreview::codeBlockPanelsChanged, projection, [&] {
                            stayedVisible &= projection->codeBlockPanels().size() == 1;
                        });
                        const auto stopWatching = qScopeGuard([&] { QObject::disconnect(watch); });
                        for (int repeat = 0; repeat < 4; ++repeat) {
                            for (int position : {0, int(text.indexOf("int value")), int(text.indexOf("// comment")), int(text.indexOf("```cpp"))}) {
                                QRectF cursor;
                                QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, cursor), Q_ARG(int, position)));
                                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                    view->mapToScene(cursor.center() + QPointF(2, 0)).toPoint());
                                QTest::qWait(30);
                                QCOMPARE(source->property("text").toString(), text);
                                QTRY_COMPARE(panels().size(), 1);
                                QTRY_VERIFY(fillIsRendered());
                                QVERIFY(stayedVisible); // No transient removal followed by recovery.
                            }
                        }
                    }
                }
                QCOMPARE(source->property("text").toString(), text);
            }
        }
    }
    void editLinksOpenOnlyWithCommandClickOrContextAction() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *projection = root->findChild<LivePreview *>(); QVERIFY(source && view && projection);
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        const QString text = "before\n\n---\n\n[guide](https://example.invalid/guide)\n\nlast\n";
        source->setProperty("text", text); source->setProperty("cursorPosition", text.size());
        QTest::qWait(50);
        LinkReceiver receiver; QDesktopServices::setUrlHandler("https", &receiver, "open");
        const auto cleanup = qScopeGuard([] { QDesktopServices::unsetUrlHandler("https"); });
        auto pointAt = [&](int position) {
            QRectF a, b;
            QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, a), Q_ARG(int, position));
            QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, b), Q_ARG(int, position + 1));
            return QPointF((a.x() + b.x()) / 2, a.center().y());
        };
        QPointF point = pointAt(projection->viewPosition(text.indexOf("guide") + 1));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, view->mapToScene(point).toPoint());
        QCoreApplication::processEvents(); QCOMPARE(receiver.urls.size(), 0);
        QTest::mouseClick(window, Qt::LeftButton, Qt::ControlModifier, view->mapToScene(point).toPoint());
        QTRY_COMPARE(receiver.urls.size(), 1);
        QCOMPARE(receiver.urls.last(), QUrl("https://example.invalid/guide"));
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, view->mapToScene(point).toPoint());
        auto *menu = root->findChild<QObject *>("editorContextMenu"); QVERIFY(menu);
        QTRY_VERIFY(menu->property("visible").toBool());
        auto *action = root->findChild<QObject *>("openLinkAction"); QVERIFY(action);
        QVERIFY(action->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(action, "triggered"));
        QTRY_COMPARE(receiver.urls.size(), 2); QMetaObject::invokeMethod(menu, "close");
        QTest::qWait(20);
        const QPointF blank(view->width() - 2, point.y());
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, view->mapToScene(blank).toPoint());
        QTRY_VERIFY(menu->property("visible").toBool()); QVERIFY(!action->property("visible").toBool());
        QMetaObject::invokeMethod(menu, "close");
        QCOMPARE(source->property("text").toString(), text);
        projection->setReadMode(true); root->setProperty("readMode", true); QTest::qWait(20);
        auto *document = view->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        point = pointAt(document->toPlainText().indexOf("guide") + 1);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, view->mapToScene(point).toPoint());
        QTRY_COMPARE(receiver.urls.size(), 3);
        QCOMPARE(source->property("text").toString(), text);
    }
    void underlineShortcutUsesHtmlAndCanonicalUndo() {
        QTemporaryDir directory;
        Backend backend;
        backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *projection = root->findChild<LivePreview *>();
        QVERIFY(source && projection);
        if (QGuiApplication::platformName() != "offscreen") {
            QVERIFY(QTest::qWaitForWindowExposed(window));
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
        }
        const auto toggle = [&](const QString &text, int from, int to) {
            source->setProperty("text", text);
            QCoreApplication::processEvents();
            QMetaObject::invokeMethod(source, "select", Q_ARG(int, from), Q_ARG(int, to));
            editorKeyClick(window, Qt::Key_U, Qt::ControlModifier);
            QCoreApplication::processEvents();
            return backend.documentText();
        };
        QCOMPARE(toggle("hello world", 0, 5), QString("<u>hello</u> world"));
        QCOMPARE(source->property("selectedText").toString(), QString("hello"));
        QMetaObject::invokeMethod(source, "undo");
        QCOMPARE(backend.documentText(), QString("hello world"));
        QCOMPARE(toggle("<u>hello</u>", 3, 8), QString("hello"));
        QCOMPARE(toggle("<u>hello</u>", 0, 12), QString("hello"));
        QCOMPARE(toggle("<U>hello</U>", 3, 8), QString("hello"));
        QCOMPARE(toggle("<u>a</u> and <u>b</u>", 0, 21), QString("<u><u>a</u> and <u>b</u></u>"));
        QCOMPARE(toggle("", 0, 0), QString("<u></u>"));
        QCOMPARE(source->property("cursorPosition").toInt(), 3);
        QCOMPARE(toggle("<u>word</u>", 5, 5), QString("word"));
        QCOMPARE(source->property("cursorPosition").toInt(), 2);
        QCOMPARE(toggle("<u>word</u>", 7, 7), QString("<u>word</u>"));
        QCOMPARE(source->property("cursorPosition").toInt(), 11);
        QCOMPARE(toggle("<u>word</u>", 11, 11), QString("<u>word</u>"));
        QCOMPARE(source->property("cursorPosition").toInt(), 7);
        const QString unicode = QStringLiteral("\u00e9\u00a0\U0001f31f");
        QCOMPARE(toggle(unicode, 0, unicode.size()), "<u>" + unicode + "</u>");
        QCOMPARE(toggle(QStringLiteral(R"(\<u>x</u>)"), 4, 5), QStringLiteral(R"(\<u><u>x</u></u>)"));
        const QString before = backend.documentText();
        QMetaObject::invokeMethod(window, "toggleReadMode");
        editorKeyClick(window, Qt::Key_U, Qt::ControlModifier);
        QCOMPARE(backend.documentText(), before);
        QVERIFY(window->property("readMode").toBool());
    }

    void codeShortcutsWrapSafelyAndUseCanonicalUndo() {
        QTemporaryDir directory; Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create()); auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor"); auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *projection = root->findChild<LivePreview *>(); QVERIFY(source && view && projection);
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto format = [&](const QString &text, int from, int to, bool fenced) {
            source->setProperty("text", text); QTest::qWait(5);
            QMetaObject::invokeMethod(source, "select", Q_ARG(int, from), Q_ARG(int, to));
            QCoreApplication::processEvents(); view->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_T, fenced ? Qt::ControlModifier | Qt::ShiftModifier : Qt::ControlModifier);
            QCoreApplication::processEvents(); return source->property("text").toString();
        };
        QCOMPARE(format("word", 0, 4, false), QString("`word`"));
        QCOMPARE(source->property("selectedText").toString(), QString("word"));
        QMetaObject::invokeMethod(source, "undo"); QCoreApplication::processEvents();
        QCOMPARE(source->property("text").toString(), QString("word"));
        QCOMPARE(format("`word`", 1, 5, false), QString("word"));
        QCOMPARE(format("a`b", 0, 3, false), QString("``a`b``"));
        QCOMPARE(format("`a` and `b`", 0, 11, false), QString("`` `a` and `b` ``"));
        QCOMPARE(format("``", 0, 2, false), QString("``` `` ```"));
        QCOMPARE(format("```foo``", 0, 8, false), QString("```` ```foo`` ````"));
        QCOMPARE(format("`literal", 0, 8, false), QString("`` `literal ``"));
        QCOMPARE(format("`` `literal ``", 3, 11, false), QString("`literal"));
        QCOMPARE(format(" x ", 0, 3, false), QString("`  x  `"));
        QCOMPARE(format("`  x  `", 2, 5, false), QString(" x "));
        QCOMPARE(format("", 0, 0, false), QString("``"));
        QCOMPARE(source->property("cursorPosition").toInt(), 1);
        QCOMPARE(format("before word after", 7, 11, true), QString("before \n```\nword\n```\n after"));
        QCOMPARE(source->property("selectedText").toString(), QString("word"));
        QMetaObject::invokeMethod(source, "undo"); QCoreApplication::processEvents();
        QCOMPARE(source->property("text").toString(), QString("before word after"));
        QCOMPARE(format("a\nb\n", 0, 4, true), QString("```\na\nb\n```"));
        QCOMPARE(format("```\na\nb\n```", 4, 8, true), QString("a\nb\n"));
        QCOMPARE(format("``` literal", 0, 11, true), QString("````\n``` literal\n````"));
        const QString separate = "```\na\n```\n\n```\nb\n```";
        QCOMPARE(format(separate, 0, separate.size(), true), QString("````\n") + separate + "\n````");
        QCOMPARE(format("", 0, 0, true), QString("```\n\n```"));
        QCOMPARE(source->property("cursorPosition").toInt(), 4);
        projection->setReadMode(true);
        const QString before = source->property("text").toString(); view->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_T, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(source->property("text").toString(), before);
        projection->setReadMode(false); source->setProperty("text", "word"); QTest::qWait(5);
        source->setProperty("cursorPosition", 2); QCoreApplication::processEvents(); view->forceActiveFocus();
        QInputMethodEvent preedit(QString::fromUtf8("に"), {{QInputMethodEvent::Cursor, 0, 0, {}}});
        QCoreApplication::sendEvent(view, &preedit);
        QTest::keyClick(window, Qt::Key_T, Qt::ControlModifier);
        QVERIFY(projection->property("imeCursorVisible").toBool());
        QCOMPARE(source->property("text").toString(), QString("wo``rd"));
        QInputMethodEvent commit; commit.setCommitString("Z"); QCoreApplication::sendEvent(view, &commit);
        QCoreApplication::processEvents(); QCOMPARE(source->property("text").toString(), QString("wo`Z`rd"));
    }
    void caretAndNativeTextSurviveDistantScrollAndModeChanges() {
        QTemporaryDir directory; Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create()); auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor"); auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *scroll = root->findChild<QQuickItem *>("editorScroll"); auto *projection = root->findChild<LivePreview *>();
        auto *caret = root->findChild<QQuickItem *>("visibleEditorCaret"); QVERIFY(source && view && scroll && projection && caret);
        if (QGuiApplication::platformName() != "offscreen") QVERIFY(QTest::qWaitForWindowExposed(window));
        const QString text = QString("Distant paragraph has visible native glyphs and editable words.\n\n").repeated(700);
        source->setProperty("text", text); source->setProperty("cursorPosition", 0); QTest::qWait(50);
        QSignalSpy visibility(window, &QQuickWindow::visibleChanged);
        for (int n = 0; n < 4; ++n) {
            if (projection->readMode()) projection->setReadMode(false);
            const int position = text.indexOf("paragraph", (n % 2 ? text.size() / 4 : text.size() * 3 / 4));
            QRectF cursor; QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, cursor), Q_ARG(int, position)));
            // Old insertion point is outside the viewport before this click.
            scroll->setProperty("contentY", qMax(qreal(0), view->y() + cursor.center().y() - scroll->height() / 2));
            QTest::qWait(40);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, view->mapToScene(cursor.center() + QPointF(2, 0)).toPoint());
            QTRY_VERIFY(qAbs(source->property("cursorPosition").toInt() - position) <= 1);
            QVERIFY(caret->isVisible()); QVERIFY(view->hasActiveFocus());
            const QPointF caretPoint = caret->mapToScene(QPointF(0, caret->height() / 2));
            QVERIFY(caretPoint.y() > 0 && caretPoint.y() < window->height());
            projection->setReadMode(true); QTest::qWait(40); QVERIFY(!caret->isVisible());
            if (QGuiApplication::platformName() != "offscreen") {
                const QImage frame = window->grabWindow(); QVERIFY(!frame.isNull());
                auto *doc = view->property("textDocument").value<QQuickTextDocument *>()->textDocument();
                const int hit = doc->documentLayout()->hitTest(QPointF(20, scroll->property("contentY").toReal() - view->y() + scroll->height() / 2), Qt::FuzzyHit);
                auto block = doc->findBlock(hit); if (block.text().isEmpty()) block = block.next(); QVERIFY(block.isValid());
                QRectF rect; QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, block.position())));
                const QPointF origin = view->mapToScene(rect.topLeft() + QPointF(2, 2));
                const qreal dpr = window->devicePixelRatio();
                const QRect area(int(origin.x() * dpr), int(origin.y() * dpr), int(100 * dpr), int((rect.height() - 4) * dpr));
                QVERIFY(frame.rect().contains(area)); const QColor background = frame.pixelColor(area.topLeft());
                int ink = 0; for (int y = area.top(); y < area.bottom(); ++y) for (int x = area.left(); x < area.right(); ++x)
                    if (frame.pixelColor(x, y) != background) ++ink;
                QVERIFY2(ink > 20, "Read glyphs must be painted before any further scrolling");
            }
        }
        QCOMPARE(visibility.count(), 0); QCOMPARE(source->property("text").toString(), text);
    }
    void tableGridNativeEditingBenchmark() {
        if (!qEnvironmentVariableIsSet("OMAWRITE_TABLE_PERF")) QSKIP("Opt-in native table benchmark; private fixtures are never saved or logged");
        if (QGuiApplication::platformName() == "offscreen" || QGuiApplication::platformName() == "minimal") QSKIP("Requires native frames");
        QString text;
        const QString path = qEnvironmentVariable("OMAWRITE_TABLE_PERF_FIXTURE");
        if (!path.isEmpty()) { QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); text = QString::fromUtf8(file.readAll()); }
        else {
            text = "before\n\n|";
            for (int c = 0; c < 28; ++c) text += QString(" column%1 |").arg(c);
            text += "\n|"; for (int c = 0; c < 28; ++c) text += "---|";
            for (int r = 0; r < 1360; ++r) { text += "\n|"; for (int c = 0; c < 28; ++c) text += QString(" synthetic%1value |").arg((r + c) % 100); }
            text += "\n\nafter";
        }
        for (const char *name : {"Regular", "Italic", "Bold", "BoldItalic"}) {
            const QString font = QStringLiteral("../fonts/IBMPlexMono-%1.ttf").arg(QLatin1String(name));
            QVERIFY(QFontDatabase::addApplicationFont(QTest::qFindTestData(font.toUtf8().constData(), __FILE__, __LINE__)) >= 0);
        }
        const QFont oldFont = QGuiApplication::font(); QGuiApplication::setFont(QFont("IBM Plex Mono", 16));
        const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(oldFont); });
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json"); backend.setOpenRouter([](const QUrl &) {});
        auto *spelling = qobject_cast<SpellingController *>(backend.spelling()); QVERIFY(spelling);
        const bool spell = spelling->enabled(); spelling->setEnabled(false);
        const auto restoreSpell = qScopeGuard([&] { spelling->setEnabled(spell); });
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create()); auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        window->setFlag(Qt::WindowDoesNotAcceptFocus, true); window->resize(1000, 760);
        const QString screenName = qEnvironmentVariable("OMAWRITE_TABLE_PERF_SCREEN");
        if (!screenName.isEmpty()) {
            QScreen *chosen = nullptr; for (auto *screen : QGuiApplication::screens()) if (screen->name() == screenName) chosen = screen;
            QVERIFY(chosen); window->setScreen(chosen); window->setPosition(chosen->availableGeometry().center() - QPoint(500, 380));
        }
        window->show(); QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *source = root->findChild<QObject *>("sourceEditor"); auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *projection = root->findChild<LivePreview *>(); QVERIFY(source && view && projection);
        source->setProperty("text", text); source->setProperty("cursorPosition", 0);
        QTRY_VERIFY_WITH_TIMEOUT(!projection->tablePanels().isEmpty(), 30000); QTest::qWait(300);
        if (!screenName.isEmpty()) QTRY_COMPARE(window->screen()->name(), screenName);
        auto *model = qobject_cast<MarkdownTableModel *>(projection->tablePanels().first().toMap()["model"].value<QObject *>()); QVERIFY(model);
        auto *grid = root->findChild<QQuickItem *>("tableCellView"); QVERIFY(grid);
        const QString area = qEnvironmentVariable("OMAWRITE_TABLE_PERF_AREA", "cells");
        if (area == "cells") {
            const qreal rowHeight = std::ceil(QFontMetricsF(qvariant_cast<QFont>(view->property("font"))).height()) + 16;
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, grid->mapToScene(QPointF(25, rowHeight * 1.5)).toPoint());
            QTRY_VERIFY(root->property("activeTableCell").value<QObject *>());
        } else {
            QVERIFY(area == "before" || area == "raw" || area == "distant");
            const auto blocks = PreviewBlocks::parse(text);
            const auto table = std::find_if(blocks.cbegin(), blocks.cend(), [](const auto &b) { return b.kind == PreviewBlocks::Kind::Table; }); QVERIFY(table != blocks.cend());
            int at = area == "distant" ? text.indexOf('\n') : qMax(0, table->start - 1);
            if (area == "raw") {
                projection->setTableSource(projection->tablePanels().first().toMap()["id"].toInt(), true);
                at = text.indexOf('\n', text.indexOf('\n', table->start) + 1) + 3;
            }
            source->setProperty("cursorPosition", at); view->forceActiveFocus(); QTest::qWait(200);
        }
        auto submittedAfter = [&](const std::function<void()> &operation, double &milliseconds) {
            struct Frame { QElapsedTimer clock; std::atomic<bool> armed{false}, synced{false}, done{false}; qint64 ns = 0; };
            auto frame = std::make_shared<Frame>(); QEventLoop loop; QPointer<QEventLoop> safeLoop(&loop);
            const auto sync = connect(window, &QQuickWindow::afterSynchronizing, window, [frame] { if (frame->armed.load()) frame->synced.store(true); }, Qt::DirectConnection);
            const auto swap = connect(window, &QQuickWindow::frameSwapped, &loop, [frame, safeLoop, window] {
                if (frame->synced.load() && !frame->done.load()) { frame->ns = frame->clock.nsecsElapsed(); frame->done.store(true); QMetaObject::invokeMethod(window, [safeLoop] { if (safeLoop) safeLoop->quit(); }, Qt::QueuedConnection); }
            }, Qt::DirectConnection);
            QTimer timer; timer.setSingleShot(true); connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            frame->clock.start(); operation(); QTimer::singleShot(0, &loop, [frame, window] { frame->armed.store(true); window->update(); });
            timer.start(30000); loop.exec(); disconnect(sync); disconnect(swap); milliseconds = double(frame->ns) / 1000000; return frame->done.load();
        };
        QJsonArray typing, commits;
        for (int n = 0; n < 12; ++n) {
            if (area != "cells") {
                const QString before = source->property("text").toString();
                double ms = 0; QVERIFY(submittedAfter([&] { QTest::keyClick(window, Qt::Key_X); }, ms)); typing.append(ms);
                QVERIFY(source->property("text").toString() != before);
                continue;
            }
            auto *field = qobject_cast<QQuickItem *>(root->property("activeTableCell").value<QObject *>()); QVERIFY(field); field->forceActiveFocus();
            field->setProperty("cursorPosition", field->property("text").toString().size()); QVERIFY(QMetaObject::invokeMethod(field, "deselect"));
            const QString before = source->property("text").toString();
            double ms = 0; QVERIFY(submittedAfter([&] { QTest::keyClick(window, Qt::Key_X); }, ms)); typing.append(ms);
            QVERIFY(source->property("text").toString() == before); // Draft only; never print private fixture bytes on failure.
            QVERIFY(submittedAfter([&] { QTest::keyClick(window, Qt::Key_Tab); }, ms)); commits.append(ms);
            QVERIFY(source->property("text").toString() != before); QVERIFY(!projection->tableDraftsPending());
            QTRY_VERIFY(root->property("activeTableCell").value<QObject *>());
            if (!screenName.isEmpty()) QCOMPARE(window->screen()->name(), screenName);
        }
        QJsonObject report{{"complete", true}, {"label", qEnvironmentVariable("OMAWRITE_TABLE_PERF_LABEL")}, {"area", area},
            {"rows", model->rowCount()}, {"columns", model->columnCount()}, {"source_units", text.size()},
            {"screen", window->screen()->name()}, {"refresh_hz", window->screen()->refreshRate()}, {"qt", qVersion()},
            {"font", qvariant_cast<QFont>(view->property("font")).toString()}, {"dpr", window->devicePixelRatio()},
            {"typing_followup_submission_ms", typing}, {"tab_followup_submission_ms", commits},
            {"scope", "12 native keys in the named area (cells also measure canonical Tab commits) to follow-up frame submission; not display latency or FPS; spelling disabled; private fixture bytes/path never included"}};
        const QString outputPath = qEnvironmentVariable("OMAWRITE_TABLE_PERF_OUTPUT");
        if (!outputPath.isEmpty()) { QFile output(outputPath); QVERIFY(output.open(QIODevice::WriteOnly)); output.write(QJsonDocument(report).toJson()); }
        qInfo() << "Table benchmark complete:" << model->rowCount() << "rows," << model->columnCount() << "columns; no cell content logged";
    }
    void tenThousandLineNativeTypingBenchmark() {
        if (!qEnvironmentVariableIsSet("OMAWRITE_INCREMENTAL_PERF")) QSKIP("Opt-in native 10,000-line benchmark");
        if (QGuiApplication::platformName() == "offscreen" || QGuiApplication::platformName() == "minimal") QSKIP("Requires native rendered frames");
        QFile fixture(qEnvironmentVariable("OMAWRITE_INCREMENTAL_PERF_FIXTURE"));
        QVERIFY(fixture.open(QIODevice::ReadOnly)); const QString text = QString::fromUtf8(fixture.readAll());
        QVERIFY(text.count('\n') >= 10000); // The manual scratch note may contain the writer's later edits.
        for (const char *name : {"Regular", "Italic", "Bold", "BoldItalic"}) {
            const QString path = QStringLiteral("../fonts/IBMPlexMono-%1.ttf").arg(QLatin1String(name));
            QVERIFY(QFontDatabase::addApplicationFont(QTest::qFindTestData(path.toUtf8().constData(), __FILE__, __LINE__)) >= 0);
        }
        const QFont previousFont = QGuiApplication::font();
        QGuiApplication::setFont(QFont("IBM Plex Mono", 16));
        const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(previousFont); });
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        // Managed creation suppresses Main.qml's initial show/maximize, so
        // focus protection and display selection precede the first OS show.
        backend.setOpenRouter([](const QUrl &) {});
        auto *spelling = qobject_cast<SpellingController *>(backend.spelling()); QVERIFY(spelling);
        const bool spell = spelling->enabled(); spelling->setEnabled(false);
        const auto restoreSpell = qScopeGuard([&] { spelling->setEnabled(spell); });
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create()); auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        // Physical keyboard input must stay with the writer's application.
        // QTest delivers measured events directly; the benchmark window never
        // accepts OS focus, even during a slow reference layout.
        window->setFlag(Qt::WindowDoesNotAcceptFocus, true);
        const QString screenName = qEnvironmentVariable("OMAWRITE_INCREMENTAL_PERF_SCREEN");
        if (!screenName.isEmpty()) {
            QScreen *selected = nullptr; for (auto *screen : QGuiApplication::screens()) if (screen->name() == screenName) selected = screen;
            QVERIFY2(selected, "Requested benchmark display not found"); window->setScreen(selected);
            window->setPosition(selected->availableGeometry().center() - QPoint(500, 380));
        }
        window->resize(QSize(1000, 760)); window->show(); QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *source = root->findChild<QObject *>("sourceEditor"); auto *view = root->findChild<QQuickItem *>("previewEditor");
        QVERIFY(source && view); source->setProperty("text", text); source->setProperty("cursorPosition", text.size() - 1);
        QTest::qWait(300); view->forceActiveFocus();
        // Initial projection/layout may restore a viewport caret. Position the
        // measured insertion point only after that lifecycle has settled.
        source->setProperty("cursorPosition", 0);
        source->setProperty("cursorPosition", text.size() - 1);
        QTRY_COMPARE_WITH_TIMEOUT(view->property("cursorPosition").toInt(), text.size() - 1, 30000);
        QCoreApplication::processEvents();
        const QString settled = source->property("text").toString();
        if (settled != text) {
            int mismatch = 0; while (mismatch < qMin(settled.size(), text.size()) && settled[mismatch] == text[mismatch]) ++mismatch;
            qInfo() << "Fixture mismatch" << mismatch << settled.size() << text.size() << settled.mid(mismatch, 100) << text.mid(mismatch, 100);
        }
        QCOMPARE(settled, text);
        QCOMPARE(source->property("cursorPosition").toInt(), text.size() - 1);
        // macOS/window managers can reposition an already-visible QML window
        // after its first layout. Enforce and verify the actual timed display.
        if (!screenName.isEmpty()) {
            QScreen *selected = nullptr;
            for (auto *screen : QGuiApplication::screens()) if (screen->name() == screenName) selected = screen;
            QVERIFY(selected);
            window->setScreen(selected);
            window->setPosition(selected->availableGeometry().center() - QPoint(500, 380));
            QTest::qWait(300);
            QTRY_COMPARE_WITH_TIMEOUT(window->screen()->name(), screenName, 5000);
        }
        view->forceActiveFocus();
        QTRY_VERIFY(view->hasActiveFocus());
        auto submittedAfter = [&](const std::function<void()> &operation, double &milliseconds) {
            struct Frame { QElapsedTimer clock; std::atomic<bool> armed{false}, synced{false}, done{false}; qint64 ns = 0; };
            auto frame = std::make_shared<Frame>(); QEventLoop loop;
            const auto sync = connect(window, &QQuickWindow::afterSynchronizing, window, [frame] {
                if (frame->armed.load()) frame->synced.store(true);
            }, Qt::DirectConnection);
            const QPointer<QEventLoop> safeLoop(&loop);
            const auto swapped = connect(window, &QQuickWindow::frameSwapped, &loop, [frame, safeLoop, window] {
                if (frame->synced.load() && !frame->done.load()) {
                    frame->ns = frame->clock.nsecsElapsed(); frame->done.store(true);
                    QMetaObject::invokeMethod(window, [safeLoop] { if (safeLoop) safeLoop->quit(); }, Qt::QueuedConnection);
                }
            }, Qt::DirectConnection);
            QTimer timeout; timeout.setSingleShot(true); connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            frame->clock.start(); operation();
            QTimer::singleShot(0, &loop, [frame, window] { frame->armed.store(true); window->update(); });
            timeout.start(30000); loop.exec(); disconnect(sync); disconnect(swapped);
            milliseconds = double(frame->ns) / 1000000.0; return frame->done.load();
        };
        QJsonArray modeSamples;
        if (qEnvironmentVariableIsSet("OMAWRITE_MODE_PERF")) {
            auto *projection = root->findChild<LivePreview *>(); QVERIFY(projection);
            QSignalSpy visibility(window, &QQuickWindow::visibleChanged);
            for (int n = 0; n < 6; ++n) {
                const bool read = n % 2 == 0; double milliseconds = 0;
                QVERIFY(submittedAfter([&] { projection->setReadMode(read); }, milliseconds));
                QCOMPARE(source->property("text").toString(), text);
                if (!screenName.isEmpty()) QCOMPARE(window->screen()->name(), screenName);
                modeSamples.append(QJsonObject{{"read", read}, {"ms", milliseconds}});
            }
            QCOMPARE(visibility.count(), 0);
        }
        QJsonArray samples;
        QString expected = text;
        for (int n = 0; n < 12; ++n) {
            // OS focus belongs to the writer; restore only this isolated
            // window's internal focus item before directly delivered events.
            view->forceActiveFocus();
            QVERIFY(view->hasActiveFocus());
            // Global relayouts in the reference can restore a viewport caret.
            // Re-establish the same insertion point outside the timed region.
            const int insertion = int(text.size()) - 1 + n;
            if (source->property("cursorPosition").toInt() != insertion
                || view->property("cursorPosition").toInt() != insertion) {
                source->setProperty("cursorPosition", 0);
                source->setProperty("cursorPosition", insertion);
                QTRY_COMPARE_WITH_TIMEOUT(view->property("cursorPosition").toInt(), insertion, 30000);
            }
            if (!screenName.isEmpty()) QCOMPARE(window->screen()->name(), screenName);
            double milliseconds = 0;
            QVERIFY2(submittedAfter([&] { QTest::keyClick(window, Qt::Key_X); }, milliseconds),
                     "No follow-up submitted frame within 30 seconds");
            expected.insert(text.size() - 1 + n, 'x');
            if (source->property("text").toString() != expected) {
                const QString actual = source->property("text").toString(); int mismatch = 0;
                while (mismatch < qMin(actual.size(), expected.size()) && actual[mismatch] == expected[mismatch]) ++mismatch;
                qInfo() << "Typing mismatch" << mismatch << "caret" << source->property("cursorPosition")
                        << "actual" << actual.mid(mismatch, 80) << "expected" << expected.mid(mismatch, 80);
            }
            QCOMPARE(source->property("text").toString(), expected);
            samples.append(milliseconds);
        }
        if (!screenName.isEmpty()) QCOMPARE(window->screen()->name(), screenName);
        QJsonObject result{{"label", qEnvironmentVariable("OMAWRITE_INCREMENTAL_PERF_LABEL")}, {"lines", text.count('\n')},
            {"bytes_utf8", text.toUtf8().size()}, {"fixture_sha256", QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex())},
            {"screen", window->screen()->name()}, {"refresh_hz", window->screen()->refreshRate()}, {"device_pixel_ratio", window->devicePixelRatio()},
            {"font", view->property("font").value<QFont>().toString()},
            {"typing_to_followup_frame_ms", samples}, {"mode_to_followup_frame_ms", modeSamples},
            {"qt", QString::fromLatin1(qVersion())}, {"platform", QGuiApplication::platformName()},
            {"scope", "Native Main.qml physical typing to marked follow-up frame submission; spelling disabled; not display latency"}};
        QFile report(qEnvironmentVariable("OMAWRITE_INCREMENTAL_PERF_OUTPUT")); QVERIFY(report.open(QIODevice::WriteOnly));
        report.write(QJsonDocument(result).toJson()); qInfo().noquote() << QJsonDocument(result).toJson(QJsonDocument::Compact);
    }
    void largeCodeBlockKeepsItsFullScrollRangeWhenClicked() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString text;
        const QString fixture = qEnvironmentVariable("OMAWRITE_LARGE_CODE_FIXTURE");
        if (!fixture.isEmpty()) {
            QFile file(fixture); QVERIFY(file.open(QIODevice::ReadOnly));
            text = QString::fromUtf8(file.readAll()); // Never attach Backend to the original path.
        } else {
            text = "Configuration:\n\n```toml\n";
            for (int i = 0; i < 260; ++i)
                text += QString("# Configuration comment %1 https://example.invalid/guide/normalization-and-window-management\nvalue_%1 = 'workspace'\n").arg(i);
            text += "```\n";
        }
        for (const char *name : {"Regular", "Italic", "Bold", "BoldItalic"}) {
            const QString path = QStringLiteral("../fonts/IBMPlexMono-%1.ttf").arg(QLatin1String(name));
            QVERIFY(QFontDatabase::addApplicationFont(QTest::qFindTestData(path.toUtf8().constData(), __FILE__, __LINE__)) >= 0);
        }
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *scroll = root->findChild<QQuickItem *>("editorScroll");
        auto *projection = root->findChild<LivePreview *>();
        QVERIFY(source && view && scroll && projection);
        if (QGuiApplication::platformName() != "offscreen")
            QVERIFY(QTest::qWaitForWindowExposed(window));
        source->setProperty("text", text); source->setProperty("cursorPosition", text.size());
        QTRY_COMPARE(projection->codeBlockPanels().size(), 1);
        QTRY_COMPARE(view->flags().testFlag(QQuickItem::ItemObservesViewport), text.size() > 10000);
        QTest::qWait(100);
        const qreal height = view->property("contentHeight").toReal();
        const qreal panelHeight = projection->codeBlockPanels().first().toMap().value("height").toReal();
        QVERIFY(height > 3000);
        const qreal panelBottom = projection->codeBlockPanels().first().toMap().value("y").toReal() + panelHeight;
        QVERIFY(panelBottom <= height + 1);
        QVERIFY(scroll->property("contentHeight").toReal() >= view->y() + panelBottom);
        const auto finalLineIsReachable = [&] {
            QRectF last;
            if (!QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, last), Q_ARG(int, int(text.size()))))
                return false;
            const qreal maxScroll = scroll->property("contentHeight").toReal() - scroll->height();
            const qreal y = view->y() + last.y() - qMax(qreal(0), maxScroll);
            return last.bottom() <= view->property("contentHeight").toReal() + 1
                && y >= 0 && y + last.height() <= scroll->height();
        };
        QVERIFY(finalLineIsReachable());
        for (int repeat = 0; repeat < 2; ++repeat) {
            for (qreal fraction : {0.1, 0.5, 0.85, 0.2}) {
                int position = int(text.size() * fraction);
                position = text.lastIndexOf('\n', position) + 1;
                QRectF cursor;
                QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, cursor), Q_ARG(int, position)));
                scroll->setProperty("contentY", qMax(qreal(0), view->y() + cursor.center().y() - scroll->height() / 2));
                QCoreApplication::processEvents();
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                    view->mapToScene(cursor.center() + QPointF(12, 0)).toPoint());
                QTest::qWait(50);
                QCOMPARE(projection->codeBlockPanels().size(), 1);
                QVERIFY(qAbs(source->property("cursorPosition").toInt() - position) <= 4);
                const qreal after = view->property("contentHeight").toReal();
                QVERIFY2(qAbs(after - height) < 1,
                    qPrintable(QString("contentHeight %1 -> %2, panel=%3, scroll=%4, doc=%5, implicit=%6").arg(height).arg(after)
                        .arg(projection->codeBlockPanels().first().toMap().value("height").toReal())
                        .arg(scroll->property("contentHeight").toReal())
                        .arg(view->property("textDocument").value<QQuickTextDocument *>()->textDocument()->documentLayout()->documentSize().height())
                        .arg(view->implicitHeight())));
                QCOMPARE(projection->codeBlockPanels().first().toMap().value("height").toReal(), panelHeight);
                QVERIFY(finalLineIsReachable());
                QCOMPARE(source->property("text").toString(), text);
                if (QGuiApplication::platformName() != "offscreen" && position < text.size() && text[position] != '\n') {
                    // Verify actual glyph pixels after distant scrolls, not
                    // merely canonical text or the background decoration.
                    const QImage frame = window->grabWindow(); QVERIFY(!frame.isNull());
                    const qreal dpr = window->devicePixelRatio();
                    const QPointF origin = view->mapToScene(cursor.topLeft() + QPointF(6, 2));
                    const QRect region(int(origin.x() * dpr), int(origin.y() * dpr),
                        int(120 * dpr), int((cursor.height() - 4) * dpr));
                    QVERIFY(frame.rect().contains(region));
                    const QColor background = frame.pixelColor(region.topLeft());
                    int ink = 0;
                    for (int y = region.top(); y <= region.bottom(); ++y)
                        for (int x = region.left(); x <= region.right(); ++x) {
                            const QColor pixel = frame.pixelColor(x, y);
                            if (qMax(qAbs(pixel.red() - background.red()), qMax(qAbs(pixel.green() - background.green()),
                                qAbs(pixel.blue() - background.blue()))) > 30) ++ink;
                        }
                    QVERIFY2(ink > 40 * dpr * dpr, "Native glyphs missing after viewport scroll");
                }
            }
        }
    }
    void forwardsNativeFileEventsDeliveredAfterStartupAndWaitsForQuiet() {
        FileOpenRequests events(qApp);
        QSignalSpy requests(&events, &FileOpenRequests::filesRequested);
        QSignalSpy idle(&events, &FileOpenRequests::forwardingIdle);
        QElapsedTimer elapsed; elapsed.start();
        events.activate(true);
        const QString path = QDir::temp().absoluteFilePath("delayed native note.md");
        QTimer::singleShot(180, &events, [&] { QCoreApplication::postEvent(qApp, new QFileOpenEvent(path)); });
        QTRY_COMPARE(requests.count(), 1);
        QCOMPARE(requests.first().first().toStringList(), QStringList{path});
        QCOMPARE(idle.count(), 0);
        QTRY_COMPARE(idle.count(), 1);
        QVERIFY(elapsed.elapsed() >= 550);
        // A primary has no exit timer, even after later Finder events.
        events.activate(false);
        QFileOpenEvent later(path);
        QCoreApplication::sendEvent(qApp, &later);
        QTRY_COMPARE(requests.count(), 2);
        QTest::qWait(450);
        QCOMPARE(idle.count(), 1);
    }

    void macReopenAppleEventsRequestBlankWindows() {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() != "cocoa") QSKIP("Requires native Cocoa Apple events");
        QTest::qWait(1); // Finish AppKit launch before installing the handler.
        FileOpenRequests events(qApp);
        events.activate(false);
        installMacReopenHandler(&events);
        QSignalSpy requests(&events, &FileOpenRequests::filesRequested);
        extern bool dispatchMacReopenForTest();
        QVERIFY(dispatchMacReopenForTest());
        QTRY_COMPARE(requests.count(), 1);
        QVERIFY(requests.first()[0].toStringList().isEmpty());
        QVERIFY(dispatchMacReopenForTest());
        QTRY_COMPARE(requests.count(), 2);
#else
        QSKIP("macOS-only Apple events");
#endif
    }

    void forwardsReopensAsDistinctBlankRequestsButIgnoresActivation() {
        QObject application;
        FileOpenRequests events(&application);
        QSignalSpy requests(&events, &FileOpenRequests::filesRequested);
        events.requestReopen(); events.requestReopen();
        QCOMPARE(requests.count(), 0);
        events.activate(false);
        QTRY_COMPARE(requests.count(), 2);
        QVERIFY(requests[0][0].toStringList().isEmpty());
        QVERIFY(requests[1][0].toStringList().isEmpty());
        QEvent activation(QEvent::ApplicationActivate);
        QCoreApplication::sendEvent(&application, &activation);
        QCoreApplication::processEvents();
        QCOMPARE(requests.count(), 2);
        const QString path = QDir::temp().absoluteFilePath("native file request.md");
        QFileOpenEvent file(path);
        QCoreApplication::sendEvent(&application, &file);
        QTRY_COMPARE(requests.count(), 3);
        QCOMPARE(requests.last()[0].toStringList(), QStringList{path});
        events.requestReopen();
        QTRY_COMPARE(requests.count(), 4);
        QVERIFY(requests.last()[0].toStringList().isEmpty());
    }

    void languageChangesRetranslateEveryManagedWindow() {
        const QString oldName = QCoreApplication::applicationName();
        const QLocale oldLocale;
        const auto oldDirection = QGuiApplication::layoutDirection();
        QCoreApplication::setApplicationName(oldName + "-translations");
        const QString state = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto cleanup = qScopeGuard([&] {
            QDir(state).removeRecursively();
            QCoreApplication::setApplicationName(oldName);
            QLocale::setDefault(oldLocale);
            QGuiApplication::setLayoutDirection(oldDirection);
        });
        QTemporaryDir catalogs; QVERIFY(catalogs.isValid());
        const QString language = catalogs.filePath("language");
        writeFile(catalogs.filePath("en.json"), R"({"Write":"Write","Untitled":"Untitled"})");
        writeFile(catalogs.filePath("de.json"), R"({"Write":"Schreiben","Untitled":"Unbenannt"})");
        writeFile(language, "en\n");
        Translator translator(language, catalogs.path());
        QVERIFY(qApp->installTranslator(&translator));
        WindowManager manager;
        connect(&translator, &Translator::changed, &manager, &WindowManager::retranslate);
        manager.start({});
        manager.newWindow();
        QCOMPARE(manager.windows().size(), 2);
        for (auto *window : manager.windows())
            QVERIFY(window->title().endsWith("Write"));
        writeFile(language, "de\n");
        for (auto *window : manager.windows()) {
            QTRY_VERIFY(window->title().endsWith("Schreiben"));
            QCOMPARE(manager.backendFor(window)->fileName(), QString("Unbenannt.md"));
        }
    }

    void emptyLaunchesAlwaysCreateBlankWindowsAndKeepFileOwners() {
        const QString oldName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(oldName + "-routing-blank");
        const QString state = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto cleanup = qScopeGuard([&] { QDir(state).removeRecursively(); QCoreApplication::setApplicationName(oldName); });
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("saved.md");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("saved source"); }
        WindowManager manager;
        manager.start({path});
        QCOMPARE(manager.windows().size(), 1);
        auto *owner = manager.windows().first();
        auto *saved = manager.backendFor(owner);
        QVERIFY(!saved->modified());
        for (int count = 2; count <= 3; ++count) {
            manager.openFiles({});
            QCOMPARE(manager.windows().size(), count);
            auto *blank = manager.backendFor(manager.windows().last());
            QVERIFY(blank); QVERIFY(blank->fileUrl().isEmpty()); QVERIFY(!blank->modified());
            QCOMPARE(blank->m_document->toPlainText(), QString());
        }
        auto *source = owner->findChild<QObject *>("sourceEditor");
        QVERIFY(source);
        source->setProperty("text", "unsaved source");
        QTRY_VERIFY(saved->modified());
        manager.openFiles({});
        QCOMPARE(manager.windows().size(), 4);
        auto *blank = manager.backendFor(manager.windows().last());
        QVERIFY(blank->fileUrl().isEmpty()); QVERIFY(!blank->modified());
        QCOMPARE(blank->m_document->toPlainText(), QString());
        manager.openFiles({path});
        QCOMPARE(manager.windows().size(), 4);
        QCOMPARE(manager.openFile(QUrl::fromLocalFile(path)), owner);
        QCOMPARE(saved->m_document->toPlainText(), QString("unsaved source"));
        QVERIFY(saved->modified());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("saved source"));
    }

    void emptyStartupKeepsABlankWindowAlongsideRecoveredDrafts() {
        const QString oldName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(oldName + "-routing-blank-recovery");
        const QString state = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto cleanup = qScopeGuard([&] { QDir(state).removeRecursively(); QCoreApplication::setApplicationName(oldName); });
        QVERIFY(QDir().mkpath(state));
        QFile snapshot(QDir(state).filePath("recovery-0.json"));
        QVERIFY(snapshot.open(QIODevice::WriteOnly));
        snapshot.write(QJsonDocument(QJsonObject{{"fileUrl", ""}, {"text", "recovered draft"}}).toJson());
        snapshot.close();
        WindowManager manager;
        manager.start({});
        QCOMPARE(manager.windows().size(), 2);
        auto *recovered = manager.backendFor(manager.windows().first());
        QCOMPARE(recovered->m_document->toPlainText(), QString("recovered draft"));
        QVERIFY(recovered->modified());
        auto *blank = manager.backendFor(manager.windows().last());
        QVERIFY(blank->fileUrl().isEmpty()); QVERIFY(!blank->modified());
        QCOMPARE(blank->m_document->toPlainText(), QString());
    }

    void routesFilesWithoutReplacingDraftsAndReleasesAcceptedWindows() {
        const QString oldName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(oldName + "-routing");
        const QString state = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto cleanup = qScopeGuard([&] { QDir(state).removeRecursively(); QCoreApplication::setApplicationName(oldName); });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString first = directory.filePath("first note.md");
        const QString second = directory.filePath("second.md");
        { QFile file(first); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("first source"); }
        { QFile file(second); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("second source"); }
        WindowManager manager;
        manager.start({first});
        QCOMPARE(manager.windows().size(), 1);
        auto *a = manager.windows().first();
        auto *backend = manager.backendFor(a);
        QCOMPARE(backend->m_document->toPlainText(), QString("first source"));
        auto *blank = manager.newWindow();
        QVERIFY(blank);
        auto *draft = manager.backendFor(blank);
        auto *source = blank->findChild<QObject *>("sourceEditor");
        QVERIFY(source);
        source->setProperty("text", "untitled draft stays here");
        QTRY_VERIFY(draft->modified());
        QVERIFY(!blank->close());
        auto *question = blank->findChild<QObject *>("unsavedChangesDialog");
        QVERIFY(question);
        QTRY_VERIFY(question->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(question, "reject"));
        QTRY_VERIFY(!question->property("visible").toBool());
        QCOMPARE(manager.windows().size(), 2); // Cancel did not release the draft.
        draft->open(QUrl::fromLocalFile(second)); // Routed picker: no replacement/prompt.
        QCOMPARE(manager.windows().size(), 3);
        QCOMPARE(draft->m_document->toPlainText(), QString("untitled draft stays here"));
        QCOMPARE(manager.openFile(QUrl::fromLocalFile(first)), a);
        QCOMPARE(manager.openFile(QUrl::fromLocalFile(directory.filePath("./first note.md"))), a);
#ifndef Q_OS_WIN
        const QString alias = directory.filePath("alias.md");
        QVERIFY(QFile::link(first, alias));
        QCOMPARE(manager.openFile(QUrl::fromLocalFile(alias)), a);
#endif
        QCOMPARE(manager.windows().size(), 3);
        QImage image(2, 2, QImage::Format_ARGB32); image.fill(Qt::black);
        QGuiApplication::clipboard()->setImage(image);
        const QString markdown = draft->pasteClipboardImage();
        QVERIFY(!markdown.isEmpty());
        const QString protectedDraft = QString("untitled draft stays here\n\n") + markdown;
        source->setProperty("text", protectedDraft);
        QSignalSpy failed(draft, &Backend::saveFailed);
        draft->saveAs(QUrl::fromLocalFile(first));
        QCOMPARE(failed.count(), 1);
        QVERIFY(draft->fileUrl().isEmpty());
        QCOMPARE(draft->m_document->toPlainText(), protectedDraft);
        QVERIFY(!QFileInfo::exists(directory.filePath("img"))); // Guard ran before copying assets.
        QFile original(first); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("first source"));
        QPointer<Backend> gone = backend;
        QPointer<QWindow> goneWindow = a;
        QVERIFY(a->close());
        QTRY_VERIFY(gone.isNull() && goneWindow.isNull());
        QCOMPARE(manager.windows().size(), 2);
        QVERIFY(manager.openFile(QUrl::fromLocalFile(first)));
        QCOMPARE(manager.windows().size(), 3);
        const auto count = manager.windows().size();
        draft->newWindow();
        QCOMPARE(manager.windows().size(), count + 1);
    }

    void startupRecoveryKeepsDuplicateDraftsAndNewWindowsStayBlank() {
        const QString oldName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(oldName + "-routing-recovery");
        const QString state = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto cleanup = qScopeGuard([&] { QDir(state).removeRecursively(); QCoreApplication::setApplicationName(oldName); });
        QVERIFY(QDir().mkpath(state));
        QTemporaryDir directory;
        const QString path = directory.filePath("owned.md");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("disk baseline"); }
        const auto snapshot = [&](int slot, const QString &text) {
            QFile file(QDir(state).filePath(QString("recovery-%1.json").arg(slot)));
            if (!file.open(QIODevice::WriteOnly)) return false;
            return file.write(QJsonDocument(QJsonObject{{"fileUrl", QUrl::fromLocalFile(path).toString()}, {"text", text}}).toJson()) > 0;
        };
        QVERIFY(snapshot(0, "draft one")); QVERIFY(snapshot(1, "draft two"));
        WindowManager manager;
        manager.start({path});
        QCOMPARE(manager.windows().size(), 2);
        auto *first = manager.backendFor(manager.windows().first());
        auto *second = manager.backendFor(manager.windows().last());
        QCOMPARE(first->m_document->toPlainText(), QString("draft one"));
        QCOMPARE(second->m_document->toPlainText(), QString("draft two"));
        QVERIFY(first->m_autoSaveSuspended && second->m_autoSaveSuspended);
        QSignalSpy failed(second, &Backend::saveFailed);
        second->save();
        QCOMPARE(failed.count(), 1);
        QVERIFY(second->modified());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("disk baseline")); file.close();
        QVERIFY(snapshot(2, "later orphan, not a New Window"));
        auto *fresh = manager.backendFor(manager.newWindow());
        QVERIFY(fresh); QVERIFY(fresh->fileUrl().isEmpty()); QVERIFY(!fresh->modified());
        QCOMPARE(fresh->m_document->toPlainText(), QString());
        QVERIFY(QFileInfo::exists(QDir(state).filePath("recovery-2.json")));
        first->save();
        QVERIFY(!first->modified());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("draft one"));
    }

    void initTestCase() {
        qmlRegisterType<LivePreview>("Omawrite", 1, 0, "LivePreview");
        QCoreApplication::setApplicationVersion(QStringLiteral("1.2.3-test"));
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_settingsDirectory.isValid());
        QCoreApplication::setApplicationName(QStringLiteral("omawrite-editor-tests-")
            + QFileInfo(m_settingsDirectory.path()).fileName());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDirectory.path());
    }

    void init() {
        // Native rendering tests persist zoom too. Isolate each test/data row,
        // while preserving persistence between backends within a single test.
        QSettings().remove(QStringLiteral("text/zoom"));
    }

    void cleanupTestCase() {
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    }

    void rendersModeSwitchesWithDocumentImages() {
        if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
            QSKIP("Run with the native window-system platform and threaded scene graph.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        for (const char *name : {"Regular", "Italic", "Bold", "BoldItalic"}) {
            const QString path = QStringLiteral("../fonts/IBMPlexMono-%1.ttf").arg(QLatin1String(name));
            const QString absolute = QTest::qFindTestData(path.toUtf8().constData(), __FILE__, __LINE__);
            QVERIFY(QFontDatabase::addApplicationFont(absolute) >= 0);
        }
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("img")));
        QString text = QStringLiteral("# Render regression\n\nBefore **bold**, *italic* and `code`.\n\n| A | B |\n| --- | --- |\n| one | two |\n\n```cpp\n/* open\nclose */\nint value = 42;\n```\n\n```mermaid\nflowchart LR\n A[Source] --> B[Preview]\n```\n\n![Large](img/image.png)\n\nAfter.\n");
        const QString fixturePath = qEnvironmentVariable("OMAWRITE_RENDER_FIXTURE");
        if (!fixturePath.isEmpty()) {
            // Optional real-world fixture: read/copy only. Auto-save and recovery
            // are attached exclusively to this temporary directory.
            QFile fixture(fixturePath);
            QVERIFY(fixture.open(QIODevice::ReadOnly));
            text = QString::fromUtf8(fixture.readAll());
            const QDir images(QFileInfo(fixturePath).dir().filePath(QStringLiteral("img")));
            for (const QString &name : images.entryList(QDir::Files))
                QVERIFY(QFile::copy(images.filePath(name), directory.filePath(QStringLiteral("img/") + name)));
        } else {
            QImage image(1600, 1000, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::darkCyan);
            QVERIFY(image.save(directory.filePath(QStringLiteral("img/image.png"))));
        }
        Backend backend;
        backend.m_recoveryPath = directory.filePath(QStringLiteral("recovery.json"));
        backend.m_fileUrl = QUrl::fromLocalFile(directory.filePath(QStringLiteral("fixture.md")));
        backend.holdAutoSave(true);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        auto threaded = std::make_shared<std::atomic_bool>(false);
        connect(window, &QQuickWindow::beforeRendering, window, [threaded] {
            if (QThread::currentThread() != QCoreApplication::instance()->thread())
                threaded->store(true);
        }, Qt::DirectConnection);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *source = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        auto *scroll = window->findChild<QObject *>(QStringLiteral("editorScroll"));
        QVERIFY(source && scroll);
        source->setProperty("text", text);
        source->setProperty("cursorPosition", text.size());
        QSignalSpy frames(window, &QQuickWindow::frameSwapped);
        for (int i = 0; i < 60; ++i) {
            frames.clear();
            // Exercise the same handler as the button/shortcut without relying
            // on OS keyboard focus while the native window is being resized.
            QVERIFY(QMetaObject::invokeMethod(window, "toggleReadMode"));
            QCOMPARE(window->property("readMode").toBool(), i % 2 == 0);
            if (i % 10 == 9)
                backend.increaseTextSize();
            window->resize(680 + (i % 3) * 120, 700 + (i % 2) * 80);
            const qreal range = qMax<qreal>(0, scroll->property("contentHeight").toReal() - scroll->property("height").toReal());
            scroll->setProperty("contentY", (i % 5) * range / 4);
            window->requestUpdate();
            QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 0, 5000);
            frames.clear();
            QVERIFY(!window->grabWindow().isNull());
            QTest::qWait(20); // Include asynchronous diagram/layout updates.
            const QString actual = source->property("text").toString();
            if (actual != text) {
                int first = 0;
                while (first < qMin(actual.size(), text.size()) && actual.at(first) == text.at(first)) ++first;
                qWarning() << "mode iteration" << i << "lengths" << actual.size() << text.size()
                           << "first diff" << first << actual.mid(first, 100) << text.mid(first, 100);
            }
            QCOMPARE(actual, text);
        }
        QVERIFY2(threaded->load(), "Use QSG_RENDER_LOOP=threaded and a native hardware rendering backend.");
    }

    void modeShortcutWorksThroughTheFocusedEditorInBothModes() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Backend backend; backend.m_recoveryPath = directory.filePath("recovery.json");
        QQmlEngine engine; engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *source = root->findChild<QObject *>("sourceEditor");
        auto *view = root->findChild<QQuickItem *>("previewEditor");
        auto *preview = root->findChild<LivePreview *>();
        QVERIFY(source && view && preview);
        const QString text = QStringLiteral("Before\n\n**source** and `code`\n");
        source->setProperty("text", text);
        QCoreApplication::processEvents();
        QSignalSpy changes(preview, &LivePreview::readModeChanged);
        QSignalSpy requests(preview, &LivePreview::toggleReadModeRequested);
        for (const bool read : {true, false}) {
            view->forceActiveFocus();
            // Bypass the window shortcut map to exercise the focused fallback,
            // including the read-only TextEdit on affected native Qt versions.
            QKeyEvent press(QEvent::KeyPress, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
            QCoreApplication::sendEvent(view, &press);
            QCOMPARE(preview->readMode(), read);
            QCOMPARE(changes.size(), read ? 1 : 2);
            QCOMPARE(requests.size(), changes.size());
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
            QCoreApplication::sendEvent(view, &release);
            QCOMPARE(changes.size(), read ? 1 : 2);
            QCOMPARE(source->property("text").toString(), text);
            QVERIFY(!source->property("canUndo").toBool());
        }
    }

    void resumesEditingAtTheReadingViewport() {
        QTemporaryDir recovery;
        Backend backend;
        backend.m_recoveryPath = recovery.filePath(QStringLiteral("recovery.json"));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *source = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        auto *view = window->findChild<QQuickItem *>(QStringLiteral("previewEditor"));
        auto *scroll = window->findChild<QObject *>(QStringLiteral("editorScroll"));
        QVERIFY(source && view && scroll);
        QString text;
        for (int i = 0; i < 80; ++i)
            text += QStringLiteral("## Section %1\n\nA paragraph for reading and editing at this location.\n\n").arg(i);
        source->setProperty("text", text);
        for (qreal ratio : {0.25, 0.65}) {
            source->setProperty("cursorPosition", text.size());
            editorKeyClick(window, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
            QTRY_VERIFY(window->property("readMode").toBool());
            const auto range = [&] {
                return scroll->property("contentHeight").toReal() - scroll->property("height").toReal();
            };
            QVERIFY(range() > 1000);
            scroll->setProperty("contentY", ratio * range());
            QCoreApplication::processEvents();
            const qreal readingRatio = scroll->property("contentY").toReal() / range();
            editorKeyClick(window, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
            QTRY_VERIFY(!window->property("readMode").toBool());
            QTRY_VERIFY(!window->property("restoringReadingPosition").toBool());
            QTest::qWait(50); // include queued projection/cursor-rectangle updates
            const qreal editingRatio = scroll->property("contentY").toReal() / range();
            QVERIFY(qAbs(editingRatio - readingRatio) < 0.03);
            const QRectF caret = view->property("cursorRectangle").toRectF();
            const qreal caretY = view->y() + caret.center().y() - scroll->property("contentY").toReal();
            const qreal height = scroll->property("height").toReal();
            QVERIFY(caretY > height * 0.25 && caretY < height * 0.75);
            QVERIFY(source->property("cursorPosition").toInt() < text.size() - 100);
            QCOMPARE(source->property("text").toString(), text);
        }
    }

    void spellingMenuUsesTheNativeDictionaryWithoutChangingCodeOrUndo() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        QTemporaryDir recovery;
        QVERIFY(recovery.isValid());
        Backend backend;
        backend.m_recoveryPath = recovery.filePath(QStringLiteral("recovery.json"));
        auto *spelling = qobject_cast<SpellingController *>(backend.spelling());
        QVERIFY(spelling);
        QString english;
        for (const QVariant &item : spelling->languages()) {
            const QString id = item.toMap().value(QStringLiteral("id")).toString();
            if (id == QLatin1String("en") || id == QLatin1String("en_US")) {
                english = id;
                break;
            }
        }
        if (english.isEmpty()) QSKIP("This native integration check needs an installed English spelling dictionary.");
        spelling->setLanguage(english);
        spelling->setEnabled(true);
        QVERIFY(spelling->available());
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *source = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *view = window->findChild<QObject *>(QStringLiteral("previewEditor"));
        QVERIFY(source && view);
        const QString markdown = QString::fromUtf8("dictionnaryy\n\n```python\ndictionnaryy\n```\n\n😀 dictionnaryy\n");
        source->setProperty("text", markdown);
        source->setProperty("cursorPosition", 4);
        QTRY_COMPARE_WITH_TIMEOUT(spelling->ranges().size(), 2, 10000);
        auto *quick = qvariant_cast<QQuickTextDocument *>(view->property("textDocument"));
        QVERIFY(quick);
        auto spelledBlocks = [&]() {
            int count = 0;
            for (QTextBlock block = quick->textDocument()->begin(); block.isValid(); block = block.next()) {
                bool spelled = false;
                for (const auto &range : block.layout()->formats())
                    spelled |= range.format.underlineStyle() == QTextCharFormat::SpellCheckUnderline;
                count += spelled ? 1 : 0;
            }
            return count;
        };
        QTRY_COMPARE_WITH_TIMEOUT(spelledBlocks(), 2, 10000);
        QObject *menu = window->findChild<QObject *>(QStringLiteral("editorContextMenu"));
        QVERIFY(menu);
        QRectF endOfWord;
        QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, endOfWord), Q_ARG(int, 11)));
        auto *viewItem = qobject_cast<QQuickItem *>(view);
        QVERIFY(viewItem);
        const QPointF blank(endOfWord.x() + 50, endOfWord.center().y());
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, viewItem->mapToScene(blank).toPoint());
        QTRY_VERIFY(menu->property("visible").toBool());
        QVERIFY(!menu->property("hasWord").toBool());
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY(!menu->property("visible").toBool());
        view->setProperty("cursorPosition", 4);
        editorKeyClick(window, Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(menu->property("visible").toBool());
        QObject *suggestion = nullptr;
        auto findSuggestion = [&]() {
            for (QObject *child : menu->findChildren<QObject *>()) {
                if (child->property("text").toString() == QLatin1String("dictionary")
                    && child->metaObject()->indexOfSignal("triggered()") >= 0) {
                    suggestion = child;
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(findSuggestion(), 10000);
        QVERIFY(QMetaObject::invokeMethod(suggestion, "triggered"));
        QTRY_VERIFY2(source->property("text").toString().startsWith(QLatin1String("dictionary\n")),
                     qPrintable(source->property("text").toString()));
        QVERIFY(source->property("text").toString().contains(QLatin1String("```python\ndictionnaryy\n```")));
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY(!menu->property("visible").toBool());
        editorKeyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(source->property("text").toString(), markdown);
        QVERIFY(QMetaObject::invokeMethod(window, "toggleReadMode"));
        QVERIFY(window->property("readMode").toBool());
        for (QTextBlock block = quick->textDocument()->begin(); block.isValid(); block = block.next()) {
            for (const auto &range : block.layout()->formats())
                QVERIFY(range.format.underlineStyle() != QTextCharFormat::SpellCheckUnderline);
        }
        QCOMPARE(source->property("text").toString(), markdown);
    }

    void selectionCountsDiscardStaleResultsAndUseExistingWordRules() {
        Backend backend;
        backend.countSelectedText(QStringLiteral("one two three"));
        QTRY_COMPARE(backend.selectionWordCount(), 3);
        backend.countSelectedText(QString(300000, QLatin1Char('a')));
        backend.countSelectedText(QStringLiteral("hello 世界"));
        QTRY_COMPARE(backend.selectionWordCount(), 3);
        backend.countSelectedText(QString());
        QCOMPARE(backend.selectionWordCount(), 0);
        QTest::qWait(150);
        QCOMPARE(backend.selectionWordCount(), 0);
    }

    void readingPositionStorageIsLocalBoundedAndClamped() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Backend backend;
        const QUrl url = QUrl::fromLocalFile(dir.filePath("a space,é.md"));
        backend.rememberReadingPosition(url, 0.4, true, 25);
        QVERIFY(backend.readingPosition(url).isEmpty()); // Not created yet.
        QFile file(url.toLocalFile());
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("hello"); file.close();
        backend.rememberReadingPosition(url, 2, true, -5);
        const auto state = backend.readingPosition(url);
        QCOMPARE(state.value("ratio").toDouble(), 1.0);
        QCOMPARE(state.value("caret").toInt(), 0);
        QVERIFY(state.value("readMode").toBool());
        Backend anotherWindow;
        QCOMPARE(anotherWindow.readingPosition(url), state);
        QVERIFY(backend.readingPosition(QUrl("https://example.com/a")).isEmpty());
        QVERIFY(!backend.revealCurrentFile()); // Untitled never launches a process.
        QCOMPARE(Backend::explorerRevealArguments(QStringLiteral("C:/space dir/a,bé.md")),
                 QStringLiteral("/select,\"C:\\space dir\\a,bé.md\""));
        for (int i = 0; i < 105; ++i) {
            const auto path = dir.filePath(QString::number(i) + ".md");
            QFile extra(path); QVERIFY(extra.open(QIODevice::WriteOnly)); extra.close();
            backend.rememberReadingPosition(QUrl::fromLocalFile(path), 0.5, false, i);
        }
        QSettings settings;
        settings.beginGroup("readingPositions");
        QVERIFY(settings.childKeys().size() <= 100);
    }

    void listPreviewMarkersRemainEditableWithStableIndentation() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        Backend backend; backend.m_recoveryPath = dir.filePath("recovery.json");
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        window->show(); QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *source = window->findChild<QObject *>("sourceEditor");
        auto *view = window->findChild<QQuickItem *>("previewEditor");
        auto *preview = window->findChild<LivePreview *>("livePreviewController");
        QVERIFY(source && view && preview);
        const QString text = "- alpha beta gamma\n- second item\n\nafter\n";
        source->setProperty("text", text);
        QVERIFY(QMetaObject::invokeMethod(source, "select", Q_ARG(int, int(text.size())), Q_ARG(int, int(text.size()))));
        QTRY_COMPARE(preview->listMarkers().size(), 2);
        const auto firstMarker = preview->listMarkers().first().toMap();
        const int prefixStart = firstMarker.value("start").toInt();
        const int prefixEnd = firstMarker.value("end").toInt();
        const auto findMarker = [&]() -> QQuickItem * {
            for (auto *child : visualDescendants(view->parentItem())) {
                const auto data = child->property("modelData").toMap();
                if (child->objectName() == "listPreviewMarker"
                    && data.value("start").toInt() == prefixStart
                    && data.value("end").toInt() == prefixEnd) return child;
            }
            return nullptr;
        };
        QTRY_VERIFY(findMarker() && findMarker()->isVisible());
        QCOMPARE(findMarker()->property("text").toString(), QString(QChar(0x2022)));
        window->requestActivate();
        QRectF before;
        QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, before), Q_ARG(int, 2)));
        auto *marker = findMarker(); QVERIFY(marker);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, marker->mapToScene(QPointF(marker->width()/2, marker->height()/2)).toPoint());
        QTRY_VERIFY(source->property("cursorPosition").toInt() >= prefixStart
                    && source->property("cursorPosition").toInt() < prefixEnd);
        // Repeater delegates may be replaced when native font/layout settles.
        QTRY_VERIFY(findMarker() && !findMarker()->isVisible());
        QRectF active;
        QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, active), Q_ARG(int, 2)));
        QCOMPARE(active.x(), before.x());
        QRectF body;
        QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, body), Q_ARG(int, 6)));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, view->mapToScene(body.center()).toPoint());
        QTRY_VERIFY(findMarker() && findMarker()->isVisible());
        QTest::keyClick(window, Qt::Key_X); // Actual visible-editor route, one source undo.
        QTRY_VERIFY(source->property("text").toString().contains('x'));
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(source->property("text").toString(), text);
        window->setProperty("readMode", true);
        QTRY_VERIFY(preview->listMarkers().isEmpty());
    }

    void selectionFooterPositionReopenAndClipboardActions() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Backend backend;
        backend.m_recoveryPath = dir.filePath("recovery.json");
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        auto *window = qobject_cast<QQuickWindow *>(root.data()); QVERIFY(window);
        window->show(); QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *source = window->findChild<QObject *>("sourceEditor");
        auto *view = window->findChild<QObject *>("previewEditor");
        auto *flick = window->findChild<QObject *>("editorScroll");
        QVERIFY(source && view && flick);
        QString text = "**alpha beta**\n\n";
        for (int i = 0; i < 100; ++i) text += QString("## Section %1\n\nA long paragraph of ordinary words.\n\n").arg(i);
        QImage fixture(24, 12, QImage::Format_ARGB32); fixture.fill(Qt::green);
        QVERIFY(fixture.save(dir.filePath("fixture.png")));
        text += "![Local picture](fixture.png)\n";
        const auto first = QUrl::fromLocalFile(dir.filePath("first.md"));
        const auto second = QUrl::fromLocalFile(dir.filePath("second.md"));
        for (const auto &url : {first, second}) {
            QFile file(url.toLocalFile()); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(text.toUtf8());
        }
        backend.open(first);
        QTRY_VERIFY(window->property("positionReady").toBool());
        QVERIFY(QMetaObject::invokeMethod(view, "select", Q_ARG(int, 0), Q_ARG(int, 14)));
        QTRY_COMPARE(backend.selectionWordCount(), 2);
        auto *label = window->findChild<QObject *>("wordCountLabel"); QVERIFY(label);
        QTRY_VERIFY(label->property("text").toString().startsWith("2 selected"));
        auto *preview = window->findChild<LivePreview *>("livePreviewController"); QVERIFY(preview);
        QMetaObject::invokeMethod(view, "forceActiveFocus");
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("**alpha beta**"));
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(QGuiApplication::clipboard()->mimeData()->html().contains("font-weight"));
        QVERIFY(!QGuiApplication::clipboard()->text().contains("Section"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QGuiApplication::clipboard()->mimeData()->html());
        QTextDocument pastedHtml; pastedHtml.setHtml(QGuiApplication::clipboard()->text());
        QCOMPARE(pastedHtml.toPlainText().trimmed(), QStringLiteral("alpha beta"));
        window->setProperty("readMode", true);
        QTRY_VERIFY(!view->property("text").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(view, "select", Q_ARG(int, 0), Q_ARG(int, 10)));
        QTRY_COMPARE(backend.selectionWordCount(), 2);
        auto *quick = qvariant_cast<QQuickTextDocument *>(view->property("textDocument")); QVERIFY(quick);
        QMetaObject::invokeMethod(view, "forceActiveFocus");
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QGuiApplication::clipboard()->mimeData()->html());
        pastedHtml.setHtml(QGuiApplication::clipboard()->text());
        QCOMPARE(pastedHtml.toPlainText().trimmed(), QStringLiteral("alpha beta"));
        QVERIFY(QGuiApplication::clipboard()->mimeData()->html().contains("font-weight"));
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("alpha beta"));
        QVERIFY(QMetaObject::invokeMethod(view, "select", Q_ARG(int, 0), Q_ARG(int, 0)));
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), text);
        auto *copyAction = window->findChild<QObject *>("copyAction"); QVERIFY(copyAction);
        QVERIFY(copyAction->property("enabled").toBool());
        QGuiApplication::clipboard()->setText("sentinel");
        QVERIFY(QMetaObject::invokeMethod(copyAction, "triggered"));
        QCOMPARE(QGuiApplication::clipboard()->text(), text);
        auto *htmlAction = window->findChild<QObject *>("copyHtmlAction"); QVERIFY(htmlAction);
        QGuiApplication::clipboard()->setText("sentinel");
        QVERIFY(QMetaObject::invokeMethod(htmlAction, "triggered"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QGuiApplication::clipboard()->mimeData()->html());
        QVERIFY(QGuiApplication::clipboard()->text().contains("Section 99"));
        const int imagePosition = quick->textDocument()->toPlainText().indexOf(QChar::ObjectReplacementCharacter);
        QVERIFY(imagePosition >= 0);
        QVERIFY(QMetaObject::invokeMethod(view, "select", Q_ARG(int, imagePosition), Q_ARG(int, imagePosition + 1)));
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        const auto imageHtml = QGuiApplication::clipboard()->mimeData()->html();
        QVERIFY(imageHtml.contains("data:image/png;base64,"));
        QVERIFY(!imageHtml.contains("preview:"));
        const qreal maximum = flick->property("contentHeight").toReal() - flick->property("height").toReal();
        QVERIFY(maximum > 0);
        flick->setProperty("contentY", maximum * 0.6);
        QVERIFY(QMetaObject::invokeMethod(window, "rememberPosition"));
        backend.open(second);
        QTRY_VERIFY(window->property("positionReady").toBool());
        QVERIFY(!window->property("readMode").toBool());
        backend.open(first);
        QTRY_VERIFY(window->property("positionReady").toBool());
        QVERIFY(window->property("readMode").toBool());
        QTRY_VERIFY(qAbs(flick->property("contentY").toReal() /
            qMax(qreal(1), flick->property("contentHeight").toReal() - flick->property("height").toReal()) - 0.6) < 0.04);
        QCOMPARE(source->property("text").toString(), text);
        backend.open(first); // Same URL reload also restores, not just fileUrlChanged.
        QTRY_VERIFY(window->property("positionReady").toBool());
        QVERIFY(window->property("readMode").toBool());
        window->setProperty("readMode", false);
        QCoreApplication::processEvents();
        const qreal editMaximum = flick->property("contentHeight").toReal() - flick->property("height").toReal();
        flick->setProperty("contentY", editMaximum * 0.4);
        QVERIFY(QMetaObject::invokeMethod(window, "rememberPosition"));
        const auto saved = backend.readingPosition(first);
        QVERIFY(saved.value("anchor").toInt() > 0);
        backend.open(second);
        QTRY_VERIFY(window->property("positionReady").toBool());
        backend.open(first);
        QTRY_VERIFY(window->property("positionReady").toBool());
        QVERIFY(!window->property("readMode").toBool());
        QTest::qWait(100);
        QVERIFY(qAbs(flick->property("contentY").toReal() /
            qMax(qreal(1), flick->property("contentHeight").toReal() - flick->property("height").toReal()) - 0.4) < 0.04);
        // A failed open must not replace the document or its remembered state.
        backend.open(QUrl::fromLocalFile(dir.filePath("missing.md")));
        QCOMPARE(backend.fileUrl(), first);
        QCOMPARE(source->property("text").toString(), text);
        backend.copyAsHtml();
        QVERIFY(QGuiApplication::clipboard()->text().contains("Section"));
        const auto savedAs = QUrl::fromLocalFile(dir.filePath("renamed.md"));
        const auto beforeSaveAs = flick->property("contentY").toReal();
        backend.saveAs(savedAs);
        QCOMPARE(backend.fileUrl(), savedAs);
        QCOMPARE(flick->property("contentY").toReal(), beforeSaveAs);
        QVERIFY(backend.readingPosition(savedAs).value("anchor").toInt() > 0);
    }

    void pastesScreenshotThroughVisibleEditorAndPreservesSourceInReadMode_data() {
        QTest::addColumn<bool>("contextMenu");
        QTest::newRow("keyboard") << false;
        QTest::newRow("context-menu") << true;
    }

    void pastesScreenshotThroughVisibleEditorAndPreservesSourceInReadMode() {
        QFETCH(bool, contextMenu);
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        QTemporaryDir recovery;
        QVERIFY(recovery.isValid());
        Backend backend;
        backend.m_recoveryPath = recovery.filePath(QStringLiteral("recovery.json"));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *source = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *view = window->findChild<QObject *>(QStringLiteral("previewEditor"));
        QVERIFY(source && view);
        for (bool dark : {false, true}) {
            backend.setDarkMode(dark);
            QCoreApplication::processEvents();
            const QColor body = window->property("bodyTextColor").value<QColor>();
            const QColor strong(backend.themeForeground());
            QCOMPARE(view->property("color").value<QColor>(), body);
            QCOMPARE(source->property("color").value<QColor>(), body);
            QCOMPARE(view->property("previewForeground").value<QColor>(), strong);
            QVERIFY(dark ? qGray(body.rgb()) < qGray(strong.rgb()) : qGray(body.rgb()) > qGray(strong.rgb()));
        }
        auto *toggle = window->findChild<QQuickItem *>(QStringLiteral("readEditToggle"));
        auto *glyph = toggle ? toggle->findChild<QQuickItem *>(QStringLiteral("previewToggleCanvas")) : nullptr;
        QVERIFY(toggle && glyph);
        QCOMPARE(glyph->mapToItem(toggle, QPointF(glyph->width() / 2, glyph->height() / 2)),
                 QPointF(toggle->width() / 2, toggle->height() / 2));
        QImage image(80, 40, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        QGuiApplication::clipboard()->setImage(image);
        if (contextMenu) {
            editorKeyClick(window, Qt::Key_F10, Qt::ShiftModifier);
            QObject *context = window->findChild<QObject *>(QStringLiteral("editorContextMenu"));
            QVERIFY(context);
            QTRY_VERIFY(context->property("visible").toBool());
            QObject *paste = nullptr;
            for (QObject *child : context->findChildren<QObject *>()) {
                if (child->property("text").toString() == QLatin1String("Paste")
                    && child->metaObject()->indexOfSignal("triggered()") >= 0)
                    paste = child;
            }
            QVERIFY(paste);
            QVERIFY(paste->property("enabled").toBool());
            QVERIFY(QMetaObject::invokeMethod(paste, "triggered"));
            QVERIFY(QMetaObject::invokeMethod(context, "close"));
            QTRY_VERIFY(!context->property("visible").toBool());
        } else {
            editorKeyClick(window, Qt::Key_V, Qt::ControlModifier);
        }
        QString markdown = source->property("text").toString();
        QVERIFY(markdown.startsWith(QStringLiteral("![Screenshot](img/")));
        QString path = markdown.mid(14, markdown.size() - 15);
        QVERIFY(backend.resolveImageUrl(path).isLocalFile());
        QVERIFY(source->property("canUndo").toBool());
        editorKeyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(source->property("text").toString(), QString());
        editorKeyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(source->property("text").toString(), markdown);

        source->setProperty("cursorPosition", 5);
        editorKeyClick(window, Qt::Key_F10, Qt::ShiftModifier);
        QObject *menu = window->findChild<QObject *>(QStringLiteral("attachmentMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("visible").toBool());
        QCOMPARE(menu->property("imagePath").toString(), path);
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QObject *rename = window->findChild<QObject *>(QStringLiteral("renameAttachmentAction"));
        QObject *dialog = window->findChild<QObject *>(QStringLiteral("renameAttachmentDialog"));
        QObject *field = window->findChild<QObject *>(QStringLiteral("attachmentPathField"));
        QVERIFY(rename && dialog && field);
        QVERIFY(QMetaObject::invokeMethod(rename, "triggered"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        const QString newPath = QStringLiteral("img/context-menu-test.png");
        field->setProperty("text", newPath);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(!menu->property("visible").toBool());
        const QString renamed = QStringLiteral("![Screenshot](%1)").arg(newPath);
        QTRY_COMPARE(source->property("text").toString(), renamed);
        editorKeyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(source->property("text").toString(), markdown);
        editorKeyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(source->property("text").toString(), renamed);
        markdown = renamed;
        path = newPath;
        QVERIFY(backend.resolveImageUrl(path).isLocalFile());

        backend.replaceRange(markdown.size(), markdown.size(), QStringLiteral("\n\nAfter"));
        source->setProperty("cursorPosition", markdown.size() + 7);
        auto *document = qvariant_cast<QQuickTextDocument *>(view->property("textDocument"));
        QVERIFY(document);
        QTRY_VERIFY(document->textDocument()->toPlainText().contains(QChar::ObjectReplacementCharacter));
        QRectF beforeImage, afterImage;
        QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, beforeImage), Q_ARG(int, 0)));
        QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, afterImage), Q_ARG(int, 1)));
        QVERIFY(afterImage.x() > beforeImage.x());
        auto *viewItem = qobject_cast<QQuickItem *>(view);
        QVERIFY(viewItem);
        for (const qreal fraction : {0.25, 0.75}) {
            const QPointF local(beforeImage.x() + (afterImage.x() - beforeImage.x()) * fraction,
                                beforeImage.y() + beforeImage.height() / 2);
            QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, viewItem->mapToScene(local).toPoint());
            QTRY_VERIFY(menu->property("visible").toBool());
            QCOMPARE(menu->property("imagePath").toString(), path);
            auto *copyImage = window->findChild<QObject *>("copyImageAction"); QVERIFY(copyImage);
            QGuiApplication::clipboard()->setText("sentinel");
            QVERIFY(QMetaObject::invokeMethod(copyImage, "triggered"));
            QCOMPARE(QGuiApplication::clipboard()->image().size(), image.size());
            QCOMPARE(QGuiApplication::clipboard()->image().pixelColor(0, 0), QColor(Qt::blue));
            QSignalSpy closed(menu, SIGNAL(closed()));
            QVERIFY(closed.isValid());
            QVERIFY(QMetaObject::invokeMethod(menu, "close"));
            QTRY_VERIFY(!closed.isEmpty()); // Wait until the native popup releases its grab.
            QVERIFY(!menu->property("visible").toBool());
        }
        viewItem->forceActiveFocus();
        const QString complete = source->property("text").toString();
        const auto toggleMode = [&] {
            // Cocoa may not grant OS activation after synthetic popup dismissal.
            // This attachment test still exercises the real button handler;
            // separate focused tests cover the physical mode shortcut.
            if (!window->isActive() && QGuiApplication::platformName() == QLatin1String("cocoa"))
                return QMetaObject::invokeMethod(window, "toggleReadMode");
            editorKeyClick(window, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
            return true;
        };
        QVERIFY(toggleMode());
        QTRY_VERIFY(window->property("readMode").toBool());
        QTRY_VERIFY(menu->property("readOnly").toBool());
        QVERIFY(!rename->property("enabled").toBool());
        QVERIFY(!rename->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, beforeImage), Q_ARG(int, 0)));
        QVERIFY(QMetaObject::invokeMethod(view, "positionToRectangle", Q_RETURN_ARG(QRectF, afterImage), Q_ARG(int, 1)));
        for (const qreal fraction : {0.25, 0.75}) {
            const QPointF local(beforeImage.x() + (afterImage.x() - beforeImage.x()) * fraction,
                                beforeImage.y() + beforeImage.height() / 2);
            QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, viewItem->mapToScene(local).toPoint());
            QTRY_VERIFY(menu->property("visible").toBool());
            QCOMPARE(menu->property("imagePath").toString(), path);
            auto *copyImage = window->findChild<QObject *>("copyImageAction"); QVERIFY(copyImage);
            QGuiApplication::clipboard()->setText("sentinel");
            QVERIFY(QMetaObject::invokeMethod(copyImage, "triggered"));
            QCOMPARE(QGuiApplication::clipboard()->image().size(), image.size());
            QCOMPARE(QGuiApplication::clipboard()->image().pixelColor(0, 0), QColor(Qt::blue));
            QSignalSpy closed(menu, SIGNAL(closed()));
            QVERIFY(QMetaObject::invokeMethod(menu, "close"));
            QTRY_VERIFY(!closed.isEmpty());
        }
        QTest::keyClick(window, Qt::Key_X);
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(source->property("text").toString(), complete);
        QVERIFY(toggleMode());
        QTRY_VERIFY(!window->property("readMode").toBool());
        QCOMPARE(source->property("text").toString(), complete);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString saved = dir.filePath(QStringLiteral("note.md"));
        backend.saveAs(QUrl::fromLocalFile(saved));
        QCOMPARE(readFile(saved), complete.toUtf8());
        QVERIFY(QFileInfo::exists(dir.filePath(path)));
    }

    void countsWords() {
        QCOMPARE(Backend::countWords(QStringLiteral("one two-three don't 42")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好世界")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好 世界")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("中文 iPad AI")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("foo中bar")), 3);
        QCOMPARE(Backend::countWords(QStringLiteral("日本語テスト")), 6);
        QCOMPARE(Backend::countWords(QStringLiteral("你好，世界。")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("コーヒー")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("か\u3099")), 1); // が, decomposed
        QCOMPARE(Backend::countWords(QStringLiteral("𠀀𠀁 foo")), 3);
        QCOMPARE(Backend::countWords(QStringLiteral("안녕하세요 세계")), 2);
        QCOMPARE(Backend::countWords(QString()), 0);
    }

    void searchIgnoresDiacriticsTheQueryLeavesOut() {
        using Matches = QList<int>;
        const auto find = [](const QString &text, const QString &query) {
            return MarkdownHighlighter::findMatches(text, query);
        };

        // Plain letters find their accented forms, in either case.
        QCOMPARE(find(QStringLiteral("Țară și ŞTIRI"), QStringLiteral("tara")), Matches({0}));
        QCOMPARE(find(QStringLiteral("Țară și ŞTIRI"), QStringLiteral("si")), Matches({5}));
        QCOMPARE(find(QStringLiteral("Țară și ŞTIRI"), QStringLiteral("stiri")), Matches({8}));
        QCOMPARE(find(QStringLiteral("în câmp"), QStringLiteral("in camp")), Matches({0}));
        QCOMPARE(find(QStringLiteral("élève, Łódź, Straße"), QStringLiteral("eleve")), Matches({0}));
        QCOMPARE(find(QStringLiteral("élève, Łódź, Straße"), QStringLiteral("lodz")), Matches({7}));

        // An accented letter in the query matches only itself, though the
        // cedilla and comma-below forms of s and t are one letter.
        QCOMPARE(find(QStringLiteral("tara țară"), QStringLiteral("țară")), Matches({5}));
        QCOMPARE(find(QStringLiteral("tara Ţară"), QStringLiteral("țară")), Matches({5}));
        QCOMPARE(find(QStringLiteral("mama mamă"), QStringLiteral("mamă")), Matches({5}));

        // Scripts whose marks make different letters keep them.
        QCOMPARE(find(QStringLiteral("й и"), QStringLiteral("и")), Matches({2}));
        QCOMPARE(find(QStringLiteral("が か"), QStringLiteral("か")), Matches({2}));
        QCOMPARE(find(QStringLiteral("한 하"), QStringLiteral("하")), Matches({2}));

        // Letters beyond the Basic Multilingual Plane still ignore case.
        QCOMPARE(find(QStringLiteral("x 𐐀𐐁"), QStringLiteral("𐐨𐐩")), Matches({2}));
        QCOMPARE(find(QStringLiteral("x 𐐨𐐩"), QStringLiteral("𐐀𐐁")), Matches({2}));

        // Matches do not overlap, and a near miss does not hide the next one.
        QCOMPARE(find(QStringLiteral("aaa"), QStringLiteral("aa")), Matches({0}));
        QCOMPARE(find(QStringLiteral("tțara"), QStringLiteral("țara")), Matches({1}));
    }

    void restylesTheWholeDocumentWithItsLayoutFinished() {
        // Qt Quick's render thread lays out any block left without a layout,
        // starting the layout's timers from the wrong thread.
        QTextDocument document;
        document.setTextWidth(600);
        QStringList lines;
        for (int i = 0; i < 3000; ++i)
            lines << QStringLiteral("line %1 with **bold** and a [link](u)").arg(i);
        document.setPlainText(lines.join(QLatin1Char('\n')));
        MarkdownHighlighter highlighter(&document);
        document.documentLayout()->documentSize();

        const auto unlaidBlocks = [&document] {
            int count = 0;
            for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
                if (block.layout()->lineCount() == 0)
                    ++count;
            }
            return count;
        };
        highlighter.setSearch(QStringLiteral("line"), -1);
        QCOMPARE(unlaidBlocks(), 0);
        highlighter.setSearch(QString(), -1);
        QCOMPARE(unlaidBlocks(), 0);
        highlighter.setTheme(false, QString(), QString(), QString(), QString());
        QCOMPARE(unlaidBlocks(), 0);
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

    void answersHelpBeforeOpeningAWindow() {
        const QString usage = Cli::usage();
        QVERIFY(usage.contains(QStringLiteral("omawrite [FILE ...]")));
        QVERIFY(usage.contains(QStringLiteral("-h, --help")));
        QVERIFY(usage.contains(QStringLiteral("-v, --version")));

        const QString version = QStringLiteral("1.2.3");
        const auto answer = [&](const QStringList &arguments) {
            return Cli::handleArguments(QStringList{QStringLiteral("omawrite")} + arguments,
                                        version);
        };
        QCOMPARE(answer({QStringLiteral("--help")}), std::optional<int>(0));
        QCOMPARE(answer({QStringLiteral("-h")}), std::optional<int>(0));
        QCOMPARE(answer({QStringLiteral("--version")}), std::optional<int>(0));
        QCOMPARE(answer({QStringLiteral("-v")}), std::optional<int>(0));

        // Everything else is the app's: no arguments, a file, and Qt's own
        // options, which a leading dash must not turn into an error.
        QCOMPARE(answer({}), std::nullopt);
        QCOMPARE(answer({QStringLiteral("draft.md")}), std::nullopt);
        QCOMPARE(answer({QStringLiteral("-platform"), QStringLiteral("offscreen")}),
                 std::nullopt);
        QCOMPARE(answer({QStringLiteral("-notes.md")}), std::nullopt);
    }

    void printsInPointsNotPixels() {
        QFont editorFont;
        editorFont.setPixelSize(20);
        const QFont printed = Backend::printFont(editorFont, 96.0);
        QCOMPARE(printed.pixelSize(), -1);
        QCOMPARE(printed.pointSizeF(), 15.0);
        QCOMPARE(Backend::printFont(editorFont, 0.0).pointSizeF(), 15.0);

        QFont pointFont;
        pointFont.setPointSizeF(11.0);
        QCOMPARE(Backend::printFont(pointFont, 144.0).pointSizeF(), 11.0);
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

    void findsStrikethroughOutsideCode() {
        const auto markup = MarkdownHighlighter::inlineMarkup(QStringLiteral("a ~~gone~~ b"));
        QCOMPARE(markup.size(), 1);
        QCOMPARE(markup.at(0).kind, MarkdownHighlighter::InlineKind::Strikethrough);
        QCOMPARE(markup.at(0).content.start, 4);
        QCOMPARE(markup.at(0).content.length, 4);

        // Tildes in code, beside space, or in longer runs stay literal.
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("`~~x~~`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("a ~~ b ~~ c")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("~~~x~~~")).isEmpty());
    }

    void undoesAnEditInOneStep() {
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

        editor->setProperty("text", QStringLiteral("hello world"));
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 5));
        QMetaObject::invokeMethod(editor, "wrapSelection", Q_ARG(QVariant, QStringLiteral("**")));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("**hello** world"));
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("hello world"));
    }

    void keepsLineHeightAcrossUndoAndPaste() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();
        const auto allLinesSpaced = [document] {
            for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
                if (block.blockFormat().lineHeight() != 140)
                    return false;
            }
            return true;
        };

        // Undoing a deleted line leaves redo working.
        editor->setProperty("text", QStringLiteral("a\nb\nc"));
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 2), Q_ARG(int, 4));
        QTest::keyClick(window, Qt::Key_Delete);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a\nc"));
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a\nb\nc"));
        QVERIFY(document->isRedoAvailable());

        // Lines written over others keep the line height, however many there are.
        editor->setProperty("text", QStringLiteral("aa\nbb\ncc"));
        QGuiApplication::clipboard()->setText(QStringLiteral("X\nY"));
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 5));
        QMetaObject::invokeMethod(editor, "paste");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("X\nY\ncc"));
        QVERIFY(allLinesSpaced());
    }

    void searchFollowsEditsWithoutTakingTheCaret() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *searchField = window->findChild<QObject *>(QStringLiteral("searchField"));
        QVERIFY(editor);
        QVERIFY(searchField);

        editor->setProperty("text", QStringLiteral("foo bar foo"));
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        searchField->setProperty("text", QStringLiteral("foo"));
        QMetaObject::invokeMethod(window, "flushSearch");
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("foo"));

        // Back in the text, typing goes where the caret is.
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        editor->setProperty("cursorPosition", 7);
        QTest::keyClick(window, 'x');
        QTest::keyClick(window, 'y');
        QCOMPARE(editor->property("text").toString(), QStringLiteral("foo barxy foo"));
        QCOMPARE(window->property("searchMatches").toList().size(), 2);

        // The current match's highlight leaves its old block even when text
        // above it is deleted.
        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();
        const auto matchBackground = [document](int blockNumber) {
            const QTextBlock block = document->findBlockByNumber(blockNumber);
            for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
                if (range.start == 0 && range.format.hasProperty(QTextFormat::BackgroundBrush))
                    return range.format.background().color();
            }
            return QColor();
        };
        editor->setProperty("text", QStringLiteral("long text\nfoo\nfoo"));
        searchField->setProperty("text", QStringLiteral("x"));
        QMetaObject::invokeMethod(window, "flushSearch");
        searchField->setProperty("text", QStringLiteral("foo"));
        QMetaObject::invokeMethod(window, "flushSearch");
        QMetaObject::invokeMethod(window, "moveSearch", Q_ARG(QVariant, 1));
        const QColor current = matchBackground(2);
        const QColor other = matchBackground(1);
        QVERIFY(current.isValid() && other.isValid() && current != other);
        QMetaObject::invokeMethod(editor, "remove", Q_ARG(int, 0), Q_ARG(int, 10));
        QCOMPARE(window->property("searchMatches").toList().size(), 2);
        QCOMPARE(window->property("searchMatchIndex").toInt(), 0);
        QCOMPARE(matchBackground(0), current);
        QCOMPARE(matchBackground(1), other);

        // Nor does it stay behind when another match moves into its offset.
        editor->setProperty("text", QStringLiteral("foo\nab\nfoo"));
        QMetaObject::invokeMethod(window, "moveSearch", Q_ARG(QVariant, 1));
        QCOMPARE(matchBackground(2), current);
        QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 0), Q_ARG(QString, QStringLiteral("abcdefg")));
        QCOMPARE(window->property("searchMatches").toList().first().toInt(), 7);
        QCOMPARE(matchBackground(2), other);

        // Matches are found where the highlighter finds them, even past a
        // character whose lowercase is longer.
        editor->setProperty("text", QString::fromUtf8("\xC4\xB0 ab"));
        searchField->setProperty("text", QStringLiteral("ab"));
        QMetaObject::invokeMethod(window, "flushSearch");
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("ab"));
    }

    void replacesOneMatchAfterAnother() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *searchField = window->findChild<QObject *>(QStringLiteral("searchField"));
        QObject *replaceField = window->findChild<QObject *>(QStringLiteral("replaceField"));
        QObject *replaceButton = window->findChild<QObject *>(QStringLiteral("replaceButton"));
        QObject *replaceAllButton = window->findChild<QObject *>(QStringLiteral("replaceAllButton"));
        QVERIFY(editor && searchField && replaceField && replaceButton && replaceAllButton);

        editor->setProperty("text", QStringLiteral("foo foo foo"));
        openReplace(window);
        searchField->setProperty("text", QStringLiteral("foo"));
        replaceField->setProperty("text", QStringLiteral("Foo"));

        // A replacement the query still matches is not found again.
        QMetaObject::invokeMethod(replaceButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("Foo foo foo"));
        QMetaObject::invokeMethod(replaceButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("Foo Foo foo"));
        QMetaObject::invokeMethod(replaceButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("Foo Foo Foo"));

        // Replace All is one edit, so one undo takes it back.
        editor->setProperty("text", QStringLiteral("a foo b foo c"));
        replaceField->setProperty("text", QStringLiteral("x"));
        QMetaObject::invokeMethod(replaceAllButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a x b x c"));
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a foo b foo c"));
    }

    void countsWordsAfterEditsWithoutBlocking() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        editor->setProperty("text", QStringLiteral("one two"));
        QTRY_COMPARE(backend.wordCount(), 2);

        // Hold the thread pool's only thread so a count stays in flight until
        // the gate opens, whatever the timing.
        QThreadPool *pool = QThreadPool::globalInstance();
        const int maxThreads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore gate;
        QSemaphore held;
        auto hold = [&] {
            pool->start([&] {
                held.release();
                gate.acquire();
            });
            held.acquire();
        };
        const auto restore = qScopeGuard([&] {
            gate.release(2);
            pool->waitForDone();
            pool->setMaxThreadCount(maxThreads);
        });
        auto &counting = backend.m_wordCountWatcher;
        QSignalSpy counted(&counting, &QFutureWatcher<int>::finished);

        // An edit made while a count runs gets one more count after it.
        hold();
        QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 7), Q_ARG(QString, QStringLiteral(" three")));
        QTRY_VERIFY(counting.isRunning());
        QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 13), Q_ARG(QString, QStringLiteral(" four")));
        QTRY_VERIFY(!backend.m_wordCountTimer.isActive());
        QCOMPARE(backend.wordCount(), 2);
        gate.release();
        QTRY_COMPARE(counted.count(), 2);
        QCOMPARE(backend.wordCount(), 4);

        // A file loaded while a count runs keeps its own count, and the
        // outdated one is dropped when it finishes.
        hold();
        QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 18), Q_ARG(QString, QStringLiteral(" five")));
        QTRY_VERIFY(counting.isRunning());
        backend.loadDocumentText(QStringLiteral("alpha"));
        QCOMPARE(backend.wordCount(), 1);
        gate.release();
        QTRY_COMPARE(counted.count(), 3);
        QCOMPARE(backend.wordCount(), 1);
        QVERIFY(!counting.isRunning());
        QVERIFY(!backend.m_wordCountTimer.isActive());
    }

    void searchWaitsForTypingToPause() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *searchField = window->findChild<QObject *>(QStringLiteral("searchField"));
        QObject *replaceField = window->findChild<QObject *>(QStringLiteral("replaceField"));
        QObject *replaceAllButton = window->findChild<QObject *>(QStringLiteral("replaceAllButton"));
        QVERIFY(editor && searchField && replaceField && replaceAllButton);

        // Typing does not search until it pauses.
        editor->setProperty("text", QStringLiteral("foo bar foo"));
        openReplace(window);
        searchField->setProperty("text", QStringLiteral("f"));
        searchField->setProperty("text", QStringLiteral("fo"));
        QCOMPARE(window->property("searchMatches").toList().size(), 0);
        QTRY_COMPARE(window->property("searchMatches").toList().size(), 2);
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("fo"));

        // Return before the pause lands on the new query's first match.
        searchField->setProperty("text", QStringLiteral("bar"));
        QMetaObject::invokeMethod(window, "moveSearch", Q_ARG(QVariant, 1));
        QCOMPARE(window->property("searchMatchIndex").toInt(), 0);
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("bar"));

        // Replace All before the pause replaces the new query's matches.
        searchField->setProperty("text", QStringLiteral("foo"));
        replaceField->setProperty("text", QStringLiteral("x"));
        QMetaObject::invokeMethod(replaceAllButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("x bar x"));

        // Closing the bar drops a search still waiting for the pause.
        searchField->setProperty("text", QStringLiteral("bar"));
        QMetaObject::invokeMethod(window, "closeSearch");
        QTest::qWait(300);
        QCOMPARE(editor->property("selectedText").toString(), QString());
    }

    void reopeningTheFindBarSearchesForWhatItHolds() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *searchField = window->findChild<QObject *>(QStringLiteral("searchField"));
        QObject *replaceField = window->findChild<QObject *>(QStringLiteral("replaceField"));
        QObject *replaceAllButton = window->findChild<QObject *>(QStringLiteral("replaceAllButton"));
        QVERIFY(editor && searchField && replaceField && replaceAllButton);

        editor->setProperty("text", QStringLiteral("foo bar foo"));
        openReplace(window);
        searchField->setProperty("text", QStringLiteral("foo"));
        QTRY_COMPARE(window->property("searchMatches").toList().size(), 2);

        // A query changed just before closing the bar is searched on reopening,
        // so Replace All does not cut at the old query's matches.
        searchField->setProperty("text", QStringLiteral("o"));
        QMetaObject::invokeMethod(window, "closeSearch");
        openReplace(window);
        replaceField->setProperty("text", QStringLiteral("x"));
        QMetaObject::invokeMethod(replaceAllButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("fxx bar fxx"));

        // Edits made while the bar was closed are searched on reopening too.
        QMetaObject::invokeMethod(window, "closeSearch");
        editor->setProperty("text", QStringLiteral("a bar"));
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        searchField->setProperty("text", QStringLiteral("bar"));
        QTRY_COMPARE(window->property("searchMatches").toList().size(), 1);
        QMetaObject::invokeMethod(window, "closeSearch");
        editor->setProperty("text", QStringLiteral("bar bar bar"));
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        QCOMPARE(window->property("searchMatches").toList().size(), 3);
    }

    void reachesAndLeavesTheReplaceFieldFromTheKeyboard() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *replaceField = window->findChild<QObject *>(QStringLiteral("replaceField"));
        QVERIFY(editor && replaceField);

        openReplace(window);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(replaceField->property("activeFocus").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("searchOpen").toBool());

        // Escape closes the bar from the Replace field itself too.
        openReplace(window);
        QMetaObject::invokeMethod(replaceField, "forceActiveFocus");
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("searchOpen").toBool());

        // A link inserted from the find bar puts the writer back in the text.
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QObject *view = window->findChild<QObject *>(QStringLiteral("previewEditor"));
        QVERIFY(view && view->property("activeFocus").toBool());
    }

    void keepsShortcutsBehindDialogs() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *dialog = window->findChild<QObject *>(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);

        QMetaObject::invokeMethod(dialog, "open");
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(!window->property("searchOpen").toBool());
        QMetaObject::invokeMethod(dialog, "close");
    }

    void togglesTheFontPickerFromItsButton() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *button = window->findChild<QQuickItem *>(QStringLiteral("fontButton"));
        QObject *picker = window->findChild<QObject *>(QStringLiteral("fontPicker"));
        QVERIFY(button && picker);

        const QPoint center = button->mapToScene(
            QPointF(button->width() / 2, button->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
        QTRY_VERIFY(picker->property("opened").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
        QTRY_VERIFY(!picker->property("visible").toBool());
    }

    void breaksLinesAfterInlineBackticks() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // Backticks that open no fence leave the paragraphs below as prose.
        const QString text = QStringLiteral("```x``` inline\n\npara");
        editor->setProperty("text", text);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        editor->setProperty("cursorPosition", text.length());
        editorKeyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), text + QStringLiteral("\n\n"));

        // Before a closing fence the caret is still in the code; before an
        // opening one it is not yet.
        editor->setProperty("text", QStringLiteral("```\ncode\n```"));
        editor->setProperty("cursorPosition", 9);
        editorKeyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("```\ncode\n\n```"));
        editor->setProperty("text", QStringLiteral("para\n```\ncode\n```"));
        editor->setProperty("cursorPosition", 5);
        editorKeyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("para\n\n\n```\ncode\n```"));

        // Tilde fences hold code the same way.
        editor->setProperty("text", QStringLiteral("~~~\ncode"));
        editor->setProperty("cursorPosition", 8);
        editorKeyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("~~~\ncode\n"));
    }

    void togglesEmphasisAtTheCaret() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");

        const auto type = [window](const QString &keys) {
            for (const QChar key : keys)
                QTest::keyClick(window, key.toLatin1());
        };
        const auto bold = [window] { QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier); };
        const auto text = [editor] { return editor->property("text").toString(); };

        // Ctrl+B, type, Ctrl+B ends the bold instead of stacking markers.
        editor->setProperty("text", QStringLiteral("a "));
        editor->setProperty("cursorPosition", 2);
        bold();
        type(QStringLiteral("hi"));
        bold();
        type(QStringLiteral(" more"));
        QCOMPARE(text(), QStringLiteral("a **hi** more"));

        // Just past the closing marker, Ctrl+B steps back into the bold.
        editor->setProperty("text", QStringLiteral("a **hi**"));
        editor->setProperty("cursorPosition", 8);
        bold();
        type(QStringLiteral("!"));
        QCOMPARE(text(), QStringLiteral("a **hi!**"));

        // Inside the bold, Ctrl+B takes it off and keeps the caret in place.
        editor->setProperty("text", QStringLiteral("a **hello** b"));
        editor->setProperty("cursorPosition", 6);
        bold();
        QCOMPARE(text(), QStringLiteral("a hello b"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 4);

        // At the end of bold italic text, Ctrl+B steps out rather than unbolding.
        editor->setProperty("text", QStringLiteral("***hello***"));
        editor->setProperty("cursorPosition", 8);
        bold();
        QCOMPARE(text(), QStringLiteral("***hello***"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 11);

        // Through several layers of nesting too.
        editor->setProperty("text", QStringLiteral("**~~*hello*~~**"));
        editor->setProperty("cursorPosition", 10);
        bold();
        QCOMPARE(text(), QStringLiteral("**~~*hello*~~**"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 15);

        // A literal underscore at the end is text, not a closing marker.
        editor->setProperty("text", QStringLiteral("**value_**"));
        editor->setProperty("cursorPosition", 7);
        bold();
        QCOMPARE(text(), QStringLiteral("value_"));

        // Italic works the same way.
        editor->setProperty("text", QStringLiteral("a "));
        editor->setProperty("cursorPosition", 2);
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        type(QStringLiteral("hi"));
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        type(QStringLiteral(" x"));
        QCOMPARE(text(), QStringLiteral("a *hi* x"));
    }

    void togglesEmphasisMarkers() {
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

        // Sets the text, selects [from, to), toggles, and returns the text.
        const auto toggle = [editor](const QString &text, int from, int to,
                                     const QString &marker) {
            editor->setProperty("text", text);
            QMetaObject::invokeMethod(editor, "select", Q_ARG(int, from), Q_ARG(int, to));
            QMetaObject::invokeMethod(editor, "wrapSelection", Q_ARG(QVariant, marker));
            return editor->property("text").toString();
        };
        const QString bold = QStringLiteral("**");
        const QString italic = QStringLiteral("*");

        QCOMPARE(toggle(QStringLiteral("a word b"), 2, 6, bold), QStringLiteral("a **word** b"));
        // Off again, whether the markers sit outside the selection or in it.
        QCOMPARE(toggle(QStringLiteral("a **word** b"), 4, 8, bold), QStringLiteral("a word b"));
        QCOMPARE(toggle(QStringLiteral("a **word** b"), 2, 10, bold), QStringLiteral("a word b"));
        QCOMPARE(toggle(QStringLiteral("a ~~word~~ b"), 4, 8, QStringLiteral("~~")),
                 QStringLiteral("a word b"));
        // An empty pair goes away too.
        QCOMPARE(toggle(QStringLiteral("Hello****."), 7, 7, bold), QStringLiteral("Hello."));

        // The "*" of bold is not italic, and "***" holds both.
        QCOMPARE(toggle(QStringLiteral("**word**"), 2, 6, italic), QStringLiteral("***word***"));
        QCOMPARE(toggle(QStringLiteral("***word***"), 3, 7, italic), QStringLiteral("**word**"));
        QCOMPARE(toggle(QStringLiteral("***word***"), 3, 7, bold), QStringLiteral("*word*"));
        // Selecting part of bold's markers adds italic rather than removing bold.
        QCOMPARE(toggle(QStringLiteral("**word**"), 1, 7, italic), QStringLiteral("***word***"));

        // Separate spans are wrapped, not broken apart.
        QCOMPARE(toggle(QStringLiteral("**a** and **b**"), 0, 15, bold),
                 QStringLiteral("****a** and **b****"));
        // Other emphasis nested inside does not stop italic from coming off.
        QCOMPARE(toggle(QStringLiteral("*a **b** c*"), 1, 10, italic),
                 QStringLiteral("a **b** c"));
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

    void findsLinkUrlAtPosition() {
        const QString text = QStringLiteral("Read [site](https://example.com/docs) now");
        const int linkStart = text.indexOf(QStringLiteral("site"));

        QCOMPARE(MarkdownHighlighter::linkUrlAt(text, linkStart),
                 QStringLiteral("https://example.com/docs"));
        QCOMPARE(MarkdownHighlighter::linkUrlAt(text, linkStart + 3),
                 QStringLiteral("https://example.com/docs"));
        QVERIFY(MarkdownHighlighter::linkUrlAt(text, linkStart - 1).isEmpty());
        QVERIFY(MarkdownHighlighter::linkUrlAt(text, text.indexOf(QStringLiteral("https"))).isEmpty());

        // Escaped punctuation, as the editor writes for parentheses, reads as itself.
        const QString escaped = QStringLiteral(R"([wiki](https://example.com/a\(b\)))");
        QCOMPARE(MarkdownHighlighter::linkUrlAt(escaped, 1),
                 QStringLiteral("https://example.com/a(b)"));
    }

    void showsLinkUrlOnlyOverTheLinkText() {
        // Probing a character past the end of the text warns from QTextCursor.
        QTest::failOnWarning(QRegularExpression(QStringLiteral("out of range")));
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("see [label](https://example.com)\nplain"));

        const auto urlUnder = [editor](qreal x, qreal y) {
            QVariant url;
            QMetaObject::invokeMethod(editor, "linkUrlUnder", Q_RETURN_ARG(QVariant, url),
                                      Q_ARG(QVariant, x), Q_ARG(QVariant, y));
            return url.toString();
        };
        const auto boxAt = [editor](int position) {
            QRectF box;
            QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, box),
                                      Q_ARG(int, position));
            return box;
        };

        // Over the label (the hidden "[" is index 4), in the right half of a
        // character too.
        const QRectF first = boxAt(5);
        const QRectF second = boxAt(6);
        const qreal middleY = first.y() + first.height() / 2;
        QCOMPARE(urlUnder(first.x() + 1, middleY), QStringLiteral("https://example.com"));
        QCOMPARE(urlUnder(second.x() - 1, middleY), QStringLiteral("https://example.com"));
        // Over the plain word before it.
        QCOMPARE(urlUnder(boxAt(0).x() + 1, middleY), QString());
        // Past the end of the line the link ends.
        QCOMPARE(urlUnder(editor->property("width").toReal() - 2, middleY), QString());
        // The right half of the label's last character still counts, though
        // the nearest caret position is past the label.
        QCOMPARE(urlUnder(boxAt(10).x() - 1, middleY), QStringLiteral("https://example.com"));

        // Below a last line that is a link, positionAt() snaps up onto it.
        editor->setProperty("text", QStringLiteral("[label](https://example.com)"));
        const QRectF only = boxAt(1);
        QCOMPARE(urlUnder(only.x() + 1, only.y() + only.height() / 2),
                 QStringLiteral("https://example.com"));
        QCOMPARE(urlUnder(only.x() + 1, only.y() + only.height() * 3), QString());

        // A wide character counts across its whole width.
        editor->setProperty("text", QStringLiteral("[\u5bbd](https://example.com)"));
        const QRectF wide = boxAt(1);
        const qreal wideRight = boxAt(2).x();
        const QFont linkFont = editor->property("font").value<QFont>();
        QVERIFY2(wideRight - wide.x() > linkFont.pixelSize() * 0.7,
                 qPrintable(QStringLiteral("Wide glyph advance %1, font %2, pixel size %3, CJK available %4")
                                .arg(wideRight - wide.x()).arg(linkFont.family())
                                .arg(linkFont.pixelSize()).arg(QFontMetricsF(linkFont).inFont(QChar(0x5bbd)))));
        QCOMPARE(urlUnder(wideRight - 1, wide.y() + wide.height() / 2),
                 QStringLiteral("https://example.com"));

        // A right-to-left label runs the other way across the screen.
        editor->setProperty("text", QStringLiteral("[\u05e9\u05dc\u05d5\u05dd](https://example.org)"));
        const QRectF rtlFirst = boxAt(1);
        const QRectF rtlSecond = boxAt(2);
        QVERIFY(rtlSecond.x() < rtlFirst.x());
        const qreal rtlY = rtlFirst.y() + rtlFirst.height() / 2;
        QCOMPARE(urlUnder((rtlFirst.x() + rtlSecond.x()) / 2, rtlY),
                 QStringLiteral("https://example.org"));
        QCOMPARE(urlUnder(rtlFirst.x() - 1, rtlY), QStringLiteral("https://example.org"));

        // Link-like text inside fenced code is not a link.
        editor->setProperty("text", QStringLiteral("```\n[label](https://example.com)\n```"));
        QCOMPARE(backend.linkUrlAt(5), QString());
    }

    void showsLinkTooltipUrlAsPlainText() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *tooltipText =
            window->findChild<QObject *>(QStringLiteral("linkTooltipText"));
        QVERIFY(tooltipText);

        // A destination Qt would mistake for HTML must still read as itself.
        const QString destination =
            QStringLiteral("<img src=\"https://example.com/pixel\">");
        QVERIFY(Qt::mightBeRichText(destination));
        tooltipText->setProperty("text", destination);
        QCOMPARE(tooltipText->property("text").toString(), destination);
        QCOMPARE(tooltipText->property("textFormat").toInt(), int(Qt::PlainText));
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

    void respectsEscapedOpeningBackticks() {
        const auto escaped = MarkdownHighlighter::inlineMarkup(
            QStringLiteral(R"(\` *italic* \`)"));
        QCOMPARE(escaped.size(), 1);
        QCOMPARE(escaped.constFirst().kind, MarkdownHighlighter::InlineKind::Italic);
        QVERIFY(MarkdownHighlighter::codeSpans(QStringLiteral(R"(\` *italic* \`)")).isEmpty());

        const QString mixed = QStringLiteral(R"(\` *italic* `*literal*`)");
        QCOMPARE(MarkdownHighlighter::inlineMarkup(mixed).size(), 1);
        const auto code = MarkdownHighlighter::codeSpans(mixed);
        QCOMPARE(code.size(), 1);
        QCOMPARE(mixed.mid(code.constFirst().start, code.constFirst().length),
                 QStringLiteral("`*literal*`"));

        // An escaped backslash leaves the opening backtick active.
        QVERIFY(MarkdownHighlighter::inlineMarkup(
            QStringLiteral(R"(\\`*literal*`)")).isEmpty());
        QCOMPARE(MarkdownHighlighter::codeSpans(
            QStringLiteral(R"(\\`*literal*`)")).size(), 1);

        // Backslashes inside code do not escape its closing delimiter.
        const auto afterCode = MarkdownHighlighter::inlineMarkup(
            QStringLiteral(R"(`code\` *italic*)"));
        QCOMPARE(afterCode.size(), 1);
        QCOMPARE(afterCode.constFirst().kind, MarkdownHighlighter::InlineKind::Italic);
    }

    void matchesExactBacktickRuns_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("expected");
        QTest::newRow("shortcut") << QStringLiteral("`` C-x ` ``") << QStringLiteral("`` C-x ` ``");
        QTest::newRow("three-ticks") << QStringLiteral("```a `` b ` c```") << QStringLiteral("```a `` b ` c```");
        QTest::newRow("mismatched") << QStringLiteral("``a`") << QString();
        QTest::newRow("backslash-does-not-split-closer") << QStringLiteral(R"(`C-x \``)") << QString();
        QTest::newRow("no-partial-run") << QStringLiteral("`a``") << QString();
        QTest::newRow("unmatched-then-valid") << QStringLiteral("before ``` unmatched ``*literal*``") << QStringLiteral("``*literal*``");
        QTest::newRow("escaped-opener") << QStringLiteral(R"(\`*italic*\`)") << QString();
        QTest::newRow("escaped-first-of-two") << QStringLiteral(R"(\``*literal*`)") << QStringLiteral("`*literal*`");
        QTest::newRow("literal-backslash-before-closer") << QStringLiteral(R"(``code\``)") << QStringLiteral(R"(``code\``)");
        QTest::newRow("even-slashes") << QStringLiteral(R"(\\``*literal*``)") << QStringLiteral("``*literal*``");
        QTest::newRow("space-only") << QStringLiteral("`` ``") << QStringLiteral("`` ``");
    }

    void matchesExactBacktickRuns() {
        QFETCH(QString, text);
        QFETCH(QString, expected);
        const auto spans = MarkdownHighlighter::codeSpans(text);
        QCOMPARE(spans.size(), expected.isEmpty() ? 0 : 1);
        if (!expected.isEmpty()) {
            QCOMPARE(text.mid(spans.first().start, spans.first().length), expected);
            QCOMPARE(spans.first().start, text.indexOf(expected));
        }
    }

    void keepsLongDelimitedCodeLiteralAndSeparate() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(
            QStringLiteral("``*literal* _literal_ [literal](url) ` tick``")).isEmpty());
        const QString first = QStringLiteral("``one ` tick``");
        const QString second = QStringLiteral("```two `` ticks```");
        const QString text = first + QStringLiteral(" and ") + second;
        const auto spans = MarkdownHighlighter::codeSpans(text);
        QCOMPARE(spans.size(), 2);
        QCOMPARE(spans.at(0).start, 0);
        QCOMPARE(spans.at(0).length, first.size());
        QCOMPARE(spans.at(1).start, first.size() + 5);
        QCOMPARE(spans.at(1).length, second.size());
    }

    void rendersMarkdownEscapesAndLiteralBackticks() {
        // CommonMark permits escapes for all ASCII punctuation, not spaces or letters.
        for (ushort ch = 33; ch <= 126; ++ch) {
            if (QChar(ch).isLetterOrNumber())
                continue;
            QTextDocument rendered;
            rendered.setMarkdown(QString(QLatin1Char('\\')) + QChar(ch));
            QCOMPARE(rendered.toPlainText(), QString(QChar(ch)));
        }
        QTextDocument rendered;
        rendered.setMarkdown(QStringLiteral("`` C-x ` ``"));
        QCOMPARE(rendered.toPlainText(), QStringLiteral("C-x `"));
        rendered.setMarkdown(QStringLiteral(R"(`C-x \``)"));
        // The final two adjacent ticks form a run of two, not a single closer.
        QCOMPARE(rendered.toPlainText(), QStringLiteral("`C-x ``"));
        rendered.setMarkdown(QStringLiteral(R"(\a and a\ b)"));
        QCOMPARE(rendered.toPlainText(), QStringLiteral(R"(\a and a\ b)"));
    }

    void keepsEnclosedCodeStyling_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("kind");
        QTest::newRow("italic") << QStringLiteral("*a `code` b*")
                                << int(MarkdownHighlighter::InlineKind::Italic);
        QTest::newRow("bold") << QStringLiteral("**a `code` b**")
                              << int(MarkdownHighlighter::InlineKind::Bold);
        QTest::newRow("link") << QStringLiteral("[a `code` b](https://example.com)")
                              << int(MarkdownHighlighter::InlineKind::Link);
    }

    void keepsEnclosedCodeStyling() {
        QFETCH(QString, text);
        QFETCH(int, kind);
        QTextDocument document;
        document.setPlainText(text);
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();

        const auto formatAt = [&document](int position) {
            for (const auto &range : document.firstBlock().layout()->formats()) {
                if (position >= range.start && position < range.start + range.length)
                    return range.format;
            }
            return QTextCharFormat();
        };
        const auto markup = MarkdownHighlighter::inlineMarkup(text);
        QCOMPARE(markup.size(), 1);
        QCOMPARE(int(markup.constFirst().kind), kind);
        const auto prose = formatAt(text.indexOf(QLatin1Char('a')));
        QCOMPARE(prose.fontItalic(), kind == int(MarkdownHighlighter::InlineKind::Italic));
        QCOMPARE(prose.fontWeight() == QFont::Bold,
                 kind == int(MarkdownHighlighter::InlineKind::Bold));
        QCOMPARE(prose.fontUnderline(), kind == int(MarkdownHighlighter::InlineKind::Link));
        QCOMPARE(prose.background().style(), Qt::NoBrush);

        const int codeStart = text.indexOf(QLatin1Char('`'));
        for (int marker : {codeStart, codeStart + 5})
            QCOMPARE(formatAt(marker).fontPointSize(), 1.0);
        for (int i = codeStart + 1; i < codeStart + 5; ++i) {
            const auto code = formatAt(i);
            QCOMPARE(code.background().color(), QColor("#23372b"));
            QVERIFY(!code.fontItalic());
            QVERIFY(code.fontWeight() != QFont::Bold);
            QVERIFY(!code.fontUnderline());
        }
        for (const auto &marker : markup.constFirst().markers)
            QCOMPARE(formatAt(marker.start).fontPointSize(), 1.0);
    }

    void leavesFencedCodeLiteral() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("prose _italic_\n"
                                            "```ruby\n"
                                            "snake_case_name = *value*\n"
                                            "```\n"
                                            "after _italic_\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();

        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        QCOMPARE(stateOf(0), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(1), int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(2), int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(3), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(4), int(MarkdownHighlighter::Prose));

        // The fenced line sits on the code panel, with nothing italic and no
        // marker hidden.
        const QList<QTextLayout::FormatRange> fenced =
            document.findBlockByNumber(2).layout()->formats();
        QVERIFY(!fenced.isEmpty());
        int covered = 0;
        for (const QTextLayout::FormatRange &range : fenced) {
            QCOMPARE(range.start, covered);
            covered += range.length;
            QCOMPARE(range.format.background().style(), Qt::NoBrush); // Shared block panel, not glyph strips.
            QVERIFY(!range.format.fontItalic());
            QVERIFY(range.format.fontPointSize() != 1.0); // No hidden Markdown markers.
            QVERIFY(range.format.foreground().color() != range.format.background().color());
        }
        QCOMPARE(covered, document.findBlockByNumber(2).text().length());

        const QList<QTextLayout::FormatRange> prose =
            document.findBlockByNumber(4).layout()->formats();
        QVERIFY(std::any_of(prose.cbegin(), prose.cend(),
                            [](const QTextLayout::FormatRange &range) {
                                return range.format.fontItalic();
                            }));
    }

    void opensFencesOnlyWhereMarkdownDoes() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("```inline``` and _italic_\n"
                                            "````md\n"
                                            "```\n"
                                            "still code\n"
                                            "````\n"
                                            "after _italic_\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();

        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        // Backticks that close on the same line are inline code, not a fence.
        QCOMPARE(stateOf(0), int(MarkdownHighlighter::Prose));
        // A four-backtick fence is not closed by three.
        QVERIFY(stateOf(1) >= int(MarkdownHighlighter::InsideFence));
        QVERIFY(stateOf(2) >= int(MarkdownHighlighter::InsideFence));
        QVERIFY(stateOf(3) >= int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(4), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(5), int(MarkdownHighlighter::Prose));

        QCOMPARE(MarkdownHighlighter::openingFenceLength(QStringLiteral("```ruby")), 3);
        QCOMPARE(MarkdownHighlighter::openingFenceLength(QStringLiteral("    ```")), 0);
        QVERIFY(!MarkdownHighlighter::closesFence(QStringLiteral("``` text"), 3));
    }

    void inlineCodeMarkersFollowCaret_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("visible");
        QTest::newRow("single") << QString("see `code` end") << QString("see code end");
        QTest::newRow("literal-tick") << QString("see `` C-x ` `` end") << QString("see C-x ` end");
        QTest::newRow("three-ticks") << QString("see ``` a `` b ` c ``` end") << QString("see a `` b ` c end");
        QTest::newRow("padding") << QString("see ``  a  `` end") << QString("see  a  end");
        QTest::newRow("only-spaces") << QString("see `   ` end") << QString("see     end");
        QTest::newRow("mismatch") << QString("see `bad`` end") << QString("see `bad`` end");
        QTest::newRow("escaped") << QString(R"(see \`literal\` end)") << QString(R"(see \`literal\` end)");
    }

    void inlineCodeMarkersFollowCaret() {
        QFETCH(QString, text);
        QFETCH(QString, visible);
        const auto inactive = highlightedFormats(text, 0);
        QString rendered;
        for (int i = 0; i < text.size(); ++i)
            if (inactive.at(i).fontPointSize() != 1.0) rendered += text.at(i);
        QCOMPARE(rendered, visible);
        for (const auto &code : MarkdownHighlighter::codeSpans(text)) {
            const auto active = highlightedFormats(text, code.start + 1);
            for (int i = code.start; i < code.start + code.length; ++i)
                QVERIFY(active.at(i).fontPointSize() != 1.0);
        }
    }

    void inlineMarkersFollowSelectionOffsetsWithinTheSameBlock() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("a `one` b `two`"));
        MarkdownHighlighter highlighter(&document);
        highlighter.setCaret(0);
        const auto hidden = [&](int offset) {
            for (const auto &range : document.firstBlock().layout()->formats())
                if (offset >= range.start && offset < range.start + range.length)
                    return range.format.fontPointSize() == 1.0;
            return false;
        };
        highlighter.setSelection(1, 7);
        QVERIFY(!hidden(2));
        QVERIFY(hidden(10));
        highlighter.setSelection(9, 15);
        QVERIFY(hidden(2));
        QVERIFY(!hidden(10));
        highlighter.setSelection(0, 0);
        QVERIFY(hidden(2));
        QVERIFY(hidden(10));
        QCOMPARE(document.toPlainText(), QStringLiteral("a `one` b `two`"));
    }

    void underlinePreviewIsCaretAwareAndLiteralInCode() {
        const QString text = QStringLiteral("see <u>word</u> and `<u>literal</u>`");
        const auto formats = highlightedFormats(text, 0);
        QVERIFY(formats.at(7).fontUnderline());
        for (int i : {4, 5, 6, 11, 12, 13, 14})
            QCOMPARE(formats.at(i).fontPointSize(), 1.0);
        const auto active = highlightedFormats(text, 8);
        for (int i = 4; i < 15; ++i)
            QVERIFY(active.at(i).fontPointSize() != 1.0);
        QVERIFY(!formats.at(text.indexOf("literal")).fontUnderline());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral(R"(\<u>literal</u>)")).isEmpty());
        const auto upper = MarkdownHighlighter::inlineMarkup(QStringLiteral("<U>word</U>"));
        QCOMPARE(upper.size(), 1);
        QCOMPARE(upper.first().kind, MarkdownHighlighter::InlineKind::Underline);
        const QString nested = QStringLiteral("<u>outer <u>inner</u> end</u>");
        QCOMPARE(MarkdownHighlighter::inlineMarkup(nested).size(), 2);
        const auto nestedFormats = highlightedFormats(nested);
        QString nestedVisible;
        for (int i = 0; i < nested.size(); ++i)
            if (nestedFormats.at(i).fontPointSize() != 1.0) nestedVisible += nested.at(i);
        QCOMPARE(nestedVisible, QStringLiteral("outer inner end"));
        QTextDocument document;
        document.setPlainText(text);
        document.clearUndoRedoStacks();
        MarkdownHighlighter highlighter(&document);
        highlighter.setCaret(0);
        highlighter.setSelection(0, text.size());
        highlighter.rehighlight();
        for (const auto &range : document.firstBlock().layout()->formats())
            QVERIFY(range.format.fontPointSize() != 1.0);
        QCOMPARE(document.toPlainText(), text);
        QVERIFY(!document.isUndoAvailable());
    }

    void hidesNoMarkersInsideCode() {
        Backend backend;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit {}", QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));
        auto *quickDocument = editor->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quickDocument);
        backend.attachDocument(quickDocument);
        QTextDocument &document = *quickDocument->textDocument();
        auto *highlighter = document.findChild<MarkdownHighlighter *>();
        QVERIFY(highlighter);
        editor->setProperty("text", QStringLiteral("``` **opening**\n"
                                                   "# _literal_ [link](url)\n"
                                                   "```  \n"
                                                   "*prose*\n"
                                                   "`*literal*`\n"
                                                   "```\n"
                                                   "_unterminated_"));
        highlighter->rehighlight();
        const auto hiddenIn = [&](int blockNumber) {
            int hidden = 0;
            for (const auto &range : document.findBlockByNumber(blockNumber).layout()->formats()) {
                if (range.format.fontPointSize() == 1.0)
                    hidden += range.length;
            }
            return hidden;
        };
        for (int blockNumber : {0, 1, 2, 5, 6})
            QCOMPARE(hiddenIn(blockNumber), 0);
        QCOMPARE(hiddenIn(4), 2); // Only the code delimiters, never its literal '*'.
        QCOMPARE(hiddenIn(3), 2);

        // Removing a fence rehighlights the following blocks as prose.
        QTextCursor cursor(&document);
        cursor.setPosition(document.findBlockByNumber(5).position());
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        QTRY_COMPARE(document.findBlockByNumber(6).userState(), int(MarkdownHighlighter::Prose));
        QCOMPARE(hiddenIn(6), 2);
    }

    void showsMarkersAroundTheCaret() {
        const QString text = QStringLiteral("a **b** c");
        // Away from the bold, its markers stay hidden.
        for (int caret : {0, 1, 8, 9}) {
            const auto formats = highlightedFormats(text, caret);
            for (int i : {2, 3, 5, 6})
                QCOMPARE(formats.at(i).fontPointSize(), 1.0);
        }
        // In it or against either end, they show, still in the marker colour.
        for (int caret : {2, 3, 4, 5, 7}) {
            const auto formats = highlightedFormats(text, caret);
            for (int i : {2, 3, 5, 6}) {
                QVERIFY(formats.at(i).fontPointSize() != 1.0);
                QCOMPARE(formats.at(i).foreground().color(), QColor(QStringLiteral("#4f525a")));
            }
            QCOMPARE(formats.at(4).fontWeight(), int(QFont::Bold));
        }

        // Everything the caret sits in shows, and nothing beside it.
        const QString nested = QStringLiteral("*a __b__ c* _d_");
        const auto formats = highlightedFormats(nested, 5);
        for (int i : {0, 3, 4, 6, 7, 10})
            QVERIFY(formats.at(i).fontPointSize() != 1.0);
        for (int i : {12, 14})
            QCOMPARE(formats.at(i).fontPointSize(), 1.0);

        // Moving to another line hides them again there.
        QTextDocument document;
        document.setPlainText(QStringLiteral("**a**\nb"));
        MarkdownHighlighter highlighter(&document);
        highlighter.setCaret(1);
        QVERIFY(document.firstBlock().layout()->formats().constFirst().format.fontPointSize() != 1.0);
        highlighter.setCaret(7);
        QCOMPARE(document.firstBlock().layout()->formats().constFirst().format.fontPointSize(), 1.0);
    }

    void themePathsSupportUnicodeWithoutChangingHome() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("home-\u00e9-\u6f22"));
        QVERIFY(QDir().mkpath(path));
        const QString osHome = QDir::homePath();
        const QString previousHome = qEnvironmentVariable("HOME");
        const bool hadHome = qEnvironmentVariableIsSet("HOME");
#ifdef Q_OS_WIN
        const QString previousProfile = qEnvironmentVariable("USERPROFILE");
        const bool hadProfile = qEnvironmentVariableIsSet("USERPROFILE");
#endif
        const QString themeDirectory = path + QStringLiteral("/.local/state/omarchy/current/theme");
        QVERIFY(QDir().mkpath(themeDirectory));
        const QString colorsPath = themeDirectory + QStringLiteral("/colors.toml");
        writeFile(colorsPath, "mode = \"light\"\nbackground = \"#123456\"\n");
        Backend backend;
        useThemeHome(backend, path);
        QCOMPARE(backend.themeBackground(), QStringLiteral("#123456"));
        QVERIFY(!backend.darkMode());
        QVERIFY(backend.m_themeWatcher.files().contains(colorsPath));
        writeFile(colorsPath, "mode = \"dark\"\nbackground = \"#654321\"\n");
        QTRY_COMPARE(backend.themeBackground(), QStringLiteral("#654321"));
        QVERIFY(backend.darkMode());
        QCOMPARE(QDir::homePath(), osHome);
        QCOMPARE(qEnvironmentVariable("HOME"), previousHome);
        QCOMPARE(qEnvironmentVariableIsSet("HOME"), hadHome);
#ifdef Q_OS_WIN
        QCOMPARE(qEnvironmentVariable("USERPROFILE"), previousProfile);
        QCOMPARE(qEnvironmentVariableIsSet("USERPROFILE"), hadProfile);
#endif
    }

    void takesTheCodePanelFromTheTheme() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QString themeDirectory = homeDirectory.path()
            + QStringLiteral("/.local/state/omarchy/current/theme");
        QVERIFY(QDir().mkpath(themeDirectory));

        Backend backend;
        useThemeHome(backend, homeDirectory.path());
        const auto codePanelFor = [&themeDirectory, &backend](const QByteArray &palette) {
            QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
            if (!colorsFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                return QColor();
            colorsFile.write(palette);
            colorsFile.close();
            // Reload the same backend so a missing key cannot inherit the
            // previous theme's panel color.
            backend.setDarkMode(!backend.darkMode());
            return QColor(backend.themeCodeBackground());
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

        QCOMPARE(codePanelFor("mode = \"light\"\n"
                              "background = \"#ffffff\"\n"
                              "foreground = \"#000000\"\n"
                              "lighter_background = \"garbage\"\n"), mixedLight);

        // A theme whose lighter background is the page itself would leave code
        // with no panel at all, so it falls back to the mix as well.
        QVERIFY(codePanelFor("mode = \"dark\"\n"
                             "background = \"#0c0b0c\"\n"
                             "foreground = \"#fafcfb\"\n"
                             "lighter_background = \"#0c0b0c\"\n")
                != QColor(QStringLiteral("#0c0b0c")));
    }

    void rendersBoldItalicRuns() {
        const QString text = QStringLiteral("a ***word*** b");
        const auto markup = MarkdownHighlighter::inlineMarkup(text);
        QCOMPARE(markup.size(), 2);
        QCOMPARE(markup.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(markup.at(1).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(markup.at(1).content.start, 5);
        QCOMPARE(markup.at(1).content.length, 4);

        const auto formats = highlightedFormats(text);
        QVERIFY(formats.at(5).fontItalic());
        QCOMPARE(formats.at(5).fontWeight(), int(QFont::Bold));
        for (int i : {2, 3, 4, 9, 10, 11})
            QCOMPARE(formats.at(i).fontPointSize(), 1.0);
    }

    // Markers nested inside other markup stay hidden, as the caret skips them.
    void hidesMarkersInsideNestedMarkup() {
        const QString linked = QStringLiteral("[**a**](u)");
        const auto linkFormats = highlightedFormats(linked);
        for (int i : {0, 1, 2, 4, 5, 6})
            QCOMPARE(linkFormats.at(i).fontPointSize(), 1.0);
        QCOMPARE(linkFormats.at(3).fontWeight(), int(QFont::Bold));
        QVERIFY(linkFormats.at(3).fontUnderline());

        const QString nested = QStringLiteral("*a __b__ c*");
        const auto nestedFormats = highlightedFormats(nested);
        for (int i : {0, 3, 4, 6, 7, 10})
            QCOMPARE(nestedFormats.at(i).fontPointSize(), 1.0);
        QVERIFY(nestedFormats.at(5).fontItalic());
        QCOMPARE(nestedFormats.at(5).fontWeight(), int(QFont::Bold));
    }

    void honoursBackslashEscapes() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\*not\\* here")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\[x](y)")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\~~not~~")).isEmpty());
        // An escaped backslash leaves the marker alone.
        QCOMPARE(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\\\*yes*")).size(), 1);
    }

    void leavesThematicBreaksAlone() {
        for (const QString &rule : {QStringLiteral("*****"), QStringLiteral("___"),
                                    QStringLiteral("* * *")}) {
            QVERIFY2(MarkdownHighlighter::inlineMarkup(rule).isEmpty(), qPrintable(rule));
            const auto formats = highlightedFormats(rule);
            for (const QTextCharFormat &format : formats) {
                QVERIFY(format.fontWeight() != QFont::Bold);
                QVERIFY(format.fontPointSize() != 1.0);
            }
        }
    }

    void mergesInlineStylesWithTheBlock() {
        // A heading stays bold under italic, a link, or code.
        const auto heading = highlightedFormats(QStringLiteral("# head *it* [l](u) `c`"));
        QVERIFY(heading.at(8).fontItalic());
        QCOMPARE(heading.at(8).fontWeight(), int(QFont::Bold));
        QCOMPARE(heading.at(13).fontWeight(), int(QFont::Bold));
        QCOMPARE(heading.at(20).fontWeight(), int(QFont::Bold));

        // A quote keeps its colour under bold.
        const auto quote = highlightedFormats(QStringLiteral("> quote **b**"));
        QCOMPARE(quote.at(10).fontWeight(), int(QFont::Bold));
        QCOMPARE(quote.at(10).foreground().color(), quote.at(3).foreground().color());
    }

    void recognisesTildeFences() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("~~~js\n"
                                            "*code*\n"
                                            "```\n"
                                            "~~~\n"
                                            "after *it*\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.rehighlight();
        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        QVERIFY(stateOf(0) >= int(MarkdownHighlighter::InsideFence));
        QVERIFY(stateOf(1) >= int(MarkdownHighlighter::InsideFence));
        // Backticks do not close a tilde fence.
        QVERIFY(stateOf(2) >= int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(3), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(4), int(MarkdownHighlighter::Prose));
    }

    void readsLinkDestinationsWithParentheses() {
        QCOMPARE(MarkdownHighlighter::linkUrlAt(QStringLiteral("[a](b(c))"), 1),
                 QStringLiteral("b(c)"));
        QCOMPARE(MarkdownHighlighter::linkUrlAt(QStringLiteral("[a](b) and (c)"), 1),
                 QStringLiteral("b"));
    }

    void reportsThemeChangesOnlyWhenTheyHappen() {
        ScopedTheme theme("mode = \"dark\"\nbackground = \"#111111\"\n");
        QVERIFY(theme.ok);
        Backend backend;
        useThemeHome(backend, theme.home.path());
        QSignalSpy colorsSpy(&backend, &Backend::themeColorsChanged);
        QSignalSpy modeSpy(&backend, &Backend::darkModeChanged);

        // The theme names its mode, so the desktop's setting changes nothing.
        backend.setDarkMode(false);
        QVERIFY(backend.darkMode());
        QCOMPARE(colorsSpy.count(), 0);
        QCOMPARE(modeSpy.count(), 0);
    }

    void ignoresInlineMarkdownInsideCodeSpans() {
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("`The_brown_fox` jumps")).size(), 0);
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("`a **b** [c](d)` and `e`")).size(), 0);

        const auto mixed = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("`code_span` then *real* emphasis"));
        QCOMPARE(mixed.size(), 1);
        QCOMPARE(mixed.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(mixed.at(0).content.start, 18);
    }

    // A code span makes its own contents literal; it does not stop markup that
    // merely encloses one. Only a delimiter landing inside the code span does.
    void keepsMarkupThatEnclosesACodeSpan() {
        const auto spanning = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("_a `b` c_"));
        QCOMPARE(spanning.size(), 1);
        QCOMPARE(spanning.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(spanning.at(0).content.start, 1);
        QCOMPARE(spanning.at(0).content.length, 7);

        const auto linked = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("[see `code`](url)"));
        QCOMPARE(linked.size(), 1);
        QCOMPARE(linked.at(0).kind, MarkdownHighlighter::InlineKind::Link);
        QCOMPARE(linked.at(0).content.start, 1);
        QCOMPARE(linked.at(0).content.length, 10);

        const auto wrapped = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("*`code`*"));
        QCOMPARE(wrapped.size(), 1);
        QCOMPARE(wrapped.at(0).kind, MarkdownHighlighter::InlineKind::Italic);

        const auto bold = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**a `b` c**"));
        QCOMPARE(bold.size(), 1);
        QCOMPARE(bold.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(bold.at(0).content.start, 2);
        QCOMPARE(bold.at(0).content.length, 7);

        // The closing underscore is inside the code span, so it is not a marker.
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("_a `b_ c` d_")).size(), 0);
    }

    // Emphasis may enclose a code span, so the code pass has to run last for the
    // code span to keep its own styling.
    void stylesAnEnclosedCodeSpanAsCode() {
        QTextDocument document;
        MarkdownHighlighter highlighter(&document);
        document.setPlainText(QStringLiteral("_a `b` c_"));
        // The code panel comes from the theme, as Backend passes it in.
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        // The constructor's own rehighlight is queued, so ask for one directly.
        highlighter.rehighlight();

        const QTextBlock block = document.firstBlock();
        QVERIFY(block.isValid());
        const auto formatAt = [&block](int index) {
            QTextCharFormat found;
            for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
                if (index >= range.start && index < range.start + range.length)
                    found = range.format;
            }
            return found;
        };

        // "a" is italic and carries no code background.
        QVERIFY(formatAt(1).fontItalic());
        QCOMPARE(formatAt(1).background().style(), Qt::NoBrush);

        // The code span keeps the code background and is not italicised.
        QVERIFY(formatAt(4).background().style() != Qt::NoBrush);
        QVERIFY(!formatAt(4).fontItalic());
    }

    // A stray asterisk next to whitespace is not an emphasis delimiter, so it
    // must not swallow a bullet or the operands of arithmetic.
    void keepsWhitespaceDelimitedAsterisksLiteral() {
        // Only `*important*` is markup; the bullet stays a bullet.
        const auto bullet = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("* first *important* point"));
        QCOMPARE(bullet.size(), 1);
        QCOMPARE(bullet.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(bullet.at(0).content.start, 9);
        QCOMPARE(bullet.at(0).content.length, 9);
        QCOMPARE(bullet.at(0).markers[0].start, 8);
        QCOMPARE(bullet.at(0).markers[1].start, 18);

        // Multiplication stays exactly as typed.
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("2 * 3 * 4")).size(), 0);

        // Real emphasis is unaffected.
        const auto shortWord = MarkdownHighlighter::inlineMarkup(QStringLiteral("*a*"));
        QCOMPARE(shortWord.size(), 1);
        QCOMPARE(shortWord.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(shortWord.at(0).content.start, 1);
        QCOMPARE(shortWord.at(0).content.length, 1);

        const auto longPhrase =
            MarkdownHighlighter::inlineMarkup(QStringLiteral("*longer text*"));
        QCOMPARE(longPhrase.size(), 1);
        QCOMPARE(longPhrase.at(0).content.length, 11);

        const auto mixed = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic*"));
        QCOMPARE(mixed.size(), 2);
        QCOMPARE(mixed.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(mixed.at(1).kind, MarkdownHighlighter::InlineKind::Italic);
    }

    void loadsCurrentOmarchyTheme() {
        ScopedTheme theme(
            "mode = \"light\"\n"
            "accent = \"#112233\"\n"
            "selection = \"#445566\"\n"
            "selection_foreground = \"#fafafa\"\n"
            "background = \"#fefefe\"\n"
            "foreground = \"#101010\"\n");
        QVERIFY(theme.ok);

        Backend backend;
        useThemeHome(backend, theme.home.path());
        QCOMPARE(backend.themeBackground(), QStringLiteral("#fefefe"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#101010"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QCOMPARE(backend.themeSelectionForeground(), QStringLiteral("#fafafa"));
        QVERIFY(!backend.darkMode());

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->property("selectionColor").value<QColor>(), QColor("#445566"));
        QCOMPARE(editor->property("selectedTextColor").value<QColor>(), QColor("#fafafa"));
    }

    void fallsBackForSelectionColors_data() {
        QTest::addColumn<QByteArray>("palette");
        QTest::addColumn<QString>("selection");
        QTest::addColumn<QString>("selectionForeground");

        QTest::newRow("bright_foreground")
            << QByteArray("foreground = \"#c0c0c0\"\n"
                          "bright_foreground = \"#ffffff\"\n"
                          "selection = \"#333333\"\n")
            << QStringLiteral("#333333") << QStringLiteral("#ffffff");
        QTest::newRow("foreground")
            << QByteArray("foreground = \"#c0c0c0\"\n"
                          "selection = \"#333333\"\n")
            << QStringLiteral("#333333") << QStringLiteral("#c0c0c0");
        QTest::newRow("selection_background")
            << QByteArray("foreground = \"#c0c0c0\"\n"
                          "selection_background = \"#222222\"\n")
            << QStringLiteral("#222222") << QStringLiteral("#c0c0c0");
    }

    void fallsBackForSelectionColors() {
        QFETCH(QByteArray, palette);
        QFETCH(QString, selection);
        QFETCH(QString, selectionForeground);

        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QString themeDirectory = homeDirectory.path()
            + QStringLiteral("/.local/state/omarchy/current/theme");
        QVERIFY(QDir().mkpath(themeDirectory));

        QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
        QVERIFY(colorsFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(colorsFile.write(palette), qint64(palette.size()));
        colorsFile.close();

        Backend backend;
        useThemeHome(backend, homeDirectory.path());
        QCOMPARE(backend.themeSelection(), selection);
        QCOMPARE(backend.themeSelectionForeground(), selectionForeground);
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
        useThemeHome(backend, theme.home.path());
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
        useThemeHome(backend, theme.home.path());
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
                function replaceText(start, end, replacement) {
                    remove(start, end);
                    insert(start, replacement);
                }

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

    void stepsOverWholeCharactersWhenTypingAndSaving() {
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

        const auto setText = [&](const QString &value) {
            editor->setProperty("text", value);
            editor->setProperty("cursorPosition", 0);
        };
        const auto move = [&](int direction) -> int {
            const bool invoked = QMetaObject::invokeMethod(
                editor, "moveCursorVisibly", Q_ARG(QVariant, direction));
            return invoked ? editor->property("cursorPosition").toInt() : -1;
        };

        // Right steps over the whole emoji, so typing keeps it in the file.
        setText(QStringLiteral("a😀b"));
        editor->setProperty("cursorPosition", 1);
        QCOMPARE(move(1), 3);
        QVERIFY(QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 3),
                                          Q_ARG(QString, QStringLiteral("x"))));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a😀xb"));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString savedPath = directory.filePath(QStringLiteral("emoji.md"));
        backend.saveAs(QUrl::fromLocalFile(savedPath));
        QFile saved(savedPath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(saved.readAll()), QStringLiteral("a😀xb"));

        // Left steps back over the same cluster.
        setText(QStringLiteral("a😀b"));
        editor->setProperty("cursorPosition", 3);
        QCOMPARE(move(-1), 1);

        // Markers show at the caret, so it steps through them like any text.
        setText(QStringLiteral("**bold**"));
        editor->setProperty("cursorPosition", 0);
        QCOMPARE(move(1), 1);
        editor->setProperty("cursorPosition", 8);
        QCOMPARE(move(-1), 7);

        // Markers next to an emoji still step over the whole emoji.
        setText(QStringLiteral("*😀*x"));
        editor->setProperty("cursorPosition", 0);
        QCOMPARE(move(1), 1);
        QCOMPARE(move(1), 3);
        QCOMPARE(move(1), 4);

        // Paragraph separators still move one position at a time.
        setText(QStringLiteral("ab\ncd"));
        editor->setProperty("cursorPosition", 2);
        QCOMPARE(move(1), 3);
        QCOMPARE(move(-1), 2);
    }

    void stepsCursorByWholeGraphemeClusters() {
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

        const auto step = [&](const QString &text, int from, int direction) {
            editor->setProperty("text", text);
            return backend.cursorPositionStep(from, direction);
        };

        // Qt 6 treats surrogate pairs, skin tone modifiers, ZWJ sequences,
        // regional indicators and combining marks as single cursor steps.
        QCOMPARE(step(QStringLiteral("a😀b"), 1, 1), 3);
        QCOMPARE(step(QStringLiteral("a😀b"), 3, -1), 1);
        QCOMPARE(step(QStringLiteral("a👍🏽b"), 1, 1), 5);
        QCOMPARE(step(QStringLiteral("a👍🏽b"), 5, -1), 1);
        QCOMPARE(step(QStringLiteral("a👨‍👩‍👧b"), 1, 1), 9);
        QCOMPARE(step(QStringLiteral("a👨‍👩‍👧b"), 9, -1), 1);
        QCOMPARE(step(QStringLiteral("a🇺🇸b"), 1, 1), 5);
        QCOMPARE(step(QStringLiteral("a1\u20e3b"), 1, 1), 3);
        QCOMPARE(step(QStringLiteral("ae\u0301b"), 1, 1), 3);

        // Document limits and paragraph separators are handled explicitly.
        QCOMPARE(step(QStringLiteral("a😀b"), 0, -1), 0);
        QCOMPARE(step(QStringLiteral("a😀b"), 4, 1), 4);
        QCOMPARE(step(QStringLiteral("ab\ncd"), 2, 1), 3);
        QCOMPARE(step(QStringLiteral("ab\ncd"), 3, -1), 2);
        QCOMPARE(step(QStringLiteral("ab\ncd"), 5, 1), 5);
        QCOMPARE(step(QStringLiteral("😀\nb"), 2, 1), 3);
        QCOMPARE(step(QStringLiteral("😀\nb"), 3, -1), 2);
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

    void copiesRenderedHtml() {
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
        editor->setProperty("text", QStringLiteral("# Title\n\nFirst **bold**.\n\nSecond."));
        backend.copyAsHtml();

        const QMimeData *mimeData = QGuiApplication::clipboard()->mimeData();
        QVERIFY(mimeData);
        QVERIFY(mimeData->hasHtml());
        const QString html = mimeData->html();
        QVERIFY(html.contains(QStringLiteral("<h1>")));
        QVERIFY(html.contains(QStringLiteral("<p>First ")));
        QVERIFY(html.contains(QStringLiteral("<body>")));
        QVERIFY(html.contains(QStringLiteral("font-weight:700;\">bold</span>")));
        QVERIFY(!html.contains(QStringLiteral("font-family")));
        QVERIFY(!html.contains(QStringLiteral("margin")));
        QCOMPARE(mimeData->text(), html);
    }

    void flashesScrollBarOnlyWhenPaging() {
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
        QObject *linger = window->findChild<QObject *>(QStringLiteral("scrollLinger"));
        QVERIFY(editor);
        QVERIFY(linger);

        QStringList lines;
        for (int i = 0; i < 300; ++i)
            lines << QStringLiteral("line %1").arg(i);
        editor->setProperty("text", lines.join(QLatin1Char('\n')));
        QVERIFY(QMetaObject::invokeMethod(editor, "forceActiveFocus"));
        QTRY_VERIFY(editor->property("activeFocus").toBool());

        // Writing past the bottom edge scrolls along, but leaves the bar hidden.
        editor->setProperty("cursorPosition", editor->property("length").toInt());
        QVERIFY(!linger->property("running").toBool());

        editorKeyClick(window, Qt::Key_PageUp);
        QVERIFY(linger->property("running").toBool());

        QMetaObject::invokeMethod(linger, "stop");
        editorKeyClick(window, Qt::Key_Home, Qt::ControlModifier);
        QVERIFY(linger->property("running").toBool());
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);
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
        auto returnKey = [&] { editorKeyClick(window, Qt::Key_Return); };

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

    void nestsListItemsOnTab() {
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
        auto tabKey = [&] { editorKeyClick(window, Qt::Key_Tab); };
        auto backtabKey = [&] { editorKeyClick(window, Qt::Key_Backtab, Qt::ShiftModifier); };

        auto selection = [&] {
            return QStringLiteral("%1-%2@%3").arg(editor->property("selectionStart").toInt())
                                             .arg(editor->property("selectionEnd").toInt())
                                             .arg(caret());
        };

        // An item nests four spaces under the item above, the caret rides
        // along, and Shift+Tab puts it back.
        load(QStringLiteral("- one\n- two"), 11);
        tabKey();
        QCOMPARE(text(), QStringLiteral("- one\n    - two"));
        QCOMPARE(caret(), 15);
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- one\n- two"));
        QCOMPARE(caret(), 11);

        // One undo takes a nesting back.
        tabKey();
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(text(), QStringLiteral("- one\n- two"));

        // The same step nests under ordered markers, the empty one Return
        // leaves included, and Shift+Tab steps back out one level.
        load(QStringLiteral("1. one"), 6);
        QTest::keyClick(window, Qt::Key_Return);
        tabKey();
        QCOMPARE(text(), QStringLiteral("1. one\n    2. "));
        load(QStringLiteral("10. a\n- b"), 9);
        tabKey();
        QCOMPARE(text(), QStringLiteral("10. a\n    - b"));
        load(QStringLiteral("1. a\n    - b\n        - c"), 24);
        backtabKey();
        QCOMPARE(text(), QStringLiteral("1. a\n    - b\n    - c"));

        // The first item nests all the same, an outermost one has nowhere to
        // go, and a tab-indented one comes out like a spaced one.
        load(QStringLiteral("- a"), 3);
        tabKey();
        QCOMPARE(text(), QStringLiteral("    - a"));
        backtabKey();
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a"));
        load(QStringLiteral("- a\n\t- b"), 8);
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a\n- b"));

        // A selection moves the items it touches together, keeping their
        // depth and its own direction, but not a line it only reaches the
        // start of.
        load(QStringLiteral("- a\n- b\n    - c\n- d"), 16);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 4));
        tabKey();
        QCOMPARE(text(), QStringLiteral("- a\n    - b\n        - c\n- d"));
        QCOMPARE(selection(), QStringLiteral("8-24@8"));

        // Shift+Tab lifts every line by one step, no further than the
        // shallowest line goes, so the nesting within the selection is kept.
        load(QStringLiteral("- a\n  - b\n    - c"), 6);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 17));
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a\n- b\n  - c"));
        load(QStringLiteral("- a\n    - b\n  - c"), 6);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 17));
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a\n  - b\n- c"));
        load(QStringLiteral("    - a\n    - b"), 0);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 10));
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a\n- b"));
        QCOMPARE(selection(), QStringLiteral("0-4@4"));
        load(QStringLiteral("- a\n \t- b\n    - c"), 4);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 15));
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a\n- b\n- c"));

        // Indented continuation lines move with their item, but a paragraph
        // at the margin is no item, so Tab leaves the list alone.
        load(QStringLiteral("- a\n- b\n  more"), 4);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 14));
        tabKey();
        QCOMPARE(text(), QStringLiteral("- a\n    - b\n      more"));
        load(QStringLiteral("- a\n- b\n\nParagraph"), 4);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 18));
        tabKey();
        QVERIFY(!text().contains(QStringLiteral("    ")));

        // A fence indented under an item moves with it, but one that starts
        // the selection, or a selection running from a list into a code
        // fence, is left alone.
        load(QStringLiteral("- a\n- b\n  ```\n  code\n  ```"), 4);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 26));
        tabKey();
        QCOMPARE(text(), QStringLiteral("- a\n    - b\n      ```\n      code\n      ```"));
        load(QStringLiteral("  ```\n  - x\n  ```"), 0);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 17));
        tabKey();
        QVERIFY(!text().contains(QStringLiteral("    ")));
        load(QStringLiteral("- a\n  ```\n  - x\n  ```"), 0);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 14));
        tabKey();
        QVERIFY(!text().contains(QStringLiteral("    ")));

        // Outside a list, on a rule, and inside a code fence, Tab types a tab.
        load(QStringLiteral("plain"), 5);
        tabKey();
        QCOMPARE(text(), QStringLiteral("plain\t"));
        load(QStringLiteral("\n- a"), 0);
        tabKey();
        QCOMPARE(text(), QStringLiteral("\t\n- a"));
        load(QStringLiteral("- a\n* * *"), 9);
        tabKey();
        QCOMPARE(text(), QStringLiteral("- a\n* * *\t"));
        load(QStringLiteral("```\n- a\n```"), 7);
        tabKey();
        QCOMPARE(text(), QStringLiteral("```\n- a\t\n```"));
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
        auto returnKey = [&] { editorKeyClick(window, Qt::Key_Return); };
        auto backspaceKey = [&] { editorKeyClick(window, Qt::Key_Backspace); };

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

    void unsavedChangesDialogSupportsAltMnemonics() {
        const QString dialogQmlPath = QFINDTESTDATA("../src/UnsavedChangesDialog.qml");
        QVERIFY(!dialogQmlPath.isEmpty());

        QQmlEngine engine;
        engine.addImportPath(QFileInfo(dialogQmlPath).absolutePath());
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import QtQuick.Controls

            ApplicationWindow {
                width: 640
                height: 420
                visible: true

                UnsavedChangesDialog {
                    id: dialog
                    objectName: "unsavedChangesDialog"
                    containerWidth: parent.width
                    containerHeight: parent.height
                }
            }
        )QML";
        component.setData(harness, QUrl::fromLocalFile(
            QFileInfo(dialogQmlPath).absolutePath() + QStringLiteral("/ShortcutHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        QObject *dialog = root->findChild<QObject *>(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);

        QSignalSpy cancelSpy(dialog, SIGNAL(cancelRequested()));
        QSignalSpy discardSpy(dialog, SIGNAL(discardRequested()));
        QSignalSpy saveSpy(dialog, SIGNAL(saveRequested()));

        // Modified Ctrl chords are inert. Bare-letter choices are covered by
        // the upstream tests and intentionally supported while the modal is up.
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(cancelSpy.count(), 0);
        QCOMPARE(discardSpy.count(), 0);
        QCOMPARE(saveSpy.count(), 0);
        QVERIFY(dialog->property("opened").toBool());

        QTest::keyClick(window, Qt::Key_C, Qt::AltModifier);
        QTRY_COMPARE(cancelSpy.count(), 1);
        QVERIFY(!dialog->property("opened").toBool());

        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_D, Qt::AltModifier);
        QTRY_COMPARE(discardSpy.count(), 1);
        QVERIFY(!dialog->property("opened").toBool());

        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_S, Qt::AltModifier);
        QTRY_COMPARE(saveSpy.count(), 1);
        QVERIFY(!dialog->property("opened").toBool());

        // With the dialog closed, the mnemonics are inert.
        QTest::keyClick(window, Qt::Key_C, Qt::AltModifier);
        QTest::keyClick(window, Qt::Key_D, Qt::AltModifier);
        QTest::keyClick(window, Qt::Key_S, Qt::AltModifier);
        QCOMPARE(cancelSpy.count(), 1);
        QCOMPARE(discardSpy.count(), 1);
        QCOMPARE(saveSpy.count(), 1);
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
        backend.setDesktopTextScale(16.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 27);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 27);

        backend.setDesktopTextScale(9.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 15);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 15);
    }

    void stepsTextSizeOnTopOfTheDesktopScale() {
        Backend backend;
        QSignalSpy textScaleSpy(&backend, &Backend::textScaleChanged);
        backend.setDesktopTextScale(1.5);
        QCOMPARE(backend.textScale(), 1.5);

        backend.increaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 1.1);
        backend.decreaseTextSize();
        backend.decreaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 0.9);
        QVERIFY(backend.status().isEmpty()); // Upstream keeps routine zoom changes quiet.

        // The ends of the ladder hold instead of running away.
        for (int step = 0; step < 8; ++step)
            backend.decreaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 0.5);
        const int settledCount = textScaleSpy.count();
        backend.decreaseTextSize();
        QCOMPARE(textScaleSpy.count(), settledCount);

        // A new window picks the size back up.
        Backend reopened;
        QCOMPARE(reopened.textScale(), 0.5);

        for (int step = 0; step < 20; ++step)
            backend.increaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 3.0);
        QCOMPARE(Backend().textScale(), 3.0);
        const int maximumCount = textScaleSpy.count();
        backend.increaseTextSize();
        QCOMPARE(textScaleSpy.count(), maximumCount);

        backend.resetTextSize();
        QCOMPARE(backend.textScale(), 1.5);
        QCOMPARE(Backend().textScale(), 1.0);
    }

    void banksTouchpadScrollBeforeChangingTextSize() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *scroll = window->findChild<QObject *>(QStringLiteral("editorScroll"));
        QVERIFY(scroll);

        const auto scrollBy = [scroll](int angle, int pixels) {
            const QVariant wheel = QVariantMap{
                {QStringLiteral("angleDelta"), QVariantMap{{QStringLiteral("y"), angle}}},
                {QStringLiteral("pixelDelta"), QVariantMap{{QStringLiteral("y"), pixels}}}};
            QVERIFY(QMetaObject::invokeMethod(scroll, "zoomByWheel", Q_ARG(QVariant, wheel)));
        };

        // A touchpad trickles in pixels: a nudge is not a whole step yet. Its
        // events carry a synthesized angleDelta as well, as Wayland sends them.
        scrollBy(240, 20);
        scrollBy(240, 20);
        QCOMPARE(backend.textScale(), 1.0);
        scrollBy(240, 20);
        QCOMPARE(backend.textScale(), 1.1);

        // A wheel notch is one step, and reversing drops the banked remainder.
        scrollBy(-120, 0);
        QCOMPARE(backend.textScale(), 1.0);

        backend.resetTextSize();
    }

    void growsTextFromEitherPlusKey() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QQuickWindow *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        // Qt resolves a shifted = to Ctrl++ rather than to Ctrl+=, and a layout
        // that puts + on its own key never produces Ctrl+= at all.
        QTest::keyClick(window, Qt::Key_Plus, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.1);
        QTest::keyClick(window, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.25);
        QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.1);

        // Exercise the actual reset binding as well as zooming out below 100%.
        QTest::keyClick(window, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.0);
        QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 0.9);
        QTest::keyClick(window, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.0);
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

        // Exercise a tiling-sized viewport without asking the native Windows
        // window manager to violate the application's production minimum.
        window->setProperty("minimumWidth", 0);
        window->setProperty("width", 394);
        QTRY_COMPARE(window->property("width").toInt(), 394);
        QVERIFY(editor->property("width").toInt()
                <= viewport->property("width").toInt());
        QVERIFY(editor->property("x").toInt() >= 0);
    }

    void shortcutsDialogHasNoBindingLoop() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        // Material's Dialog derives implicitWidth from its content item; a bare
        // Label assigned as contentItem feeds back into that binding and QML
        // reports a binding loop for the dialog on every launch.
        BindingLoopCounter bindingLoops;

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *dialog = window->findChild<QObject *>(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(dialog->property("implicitWidth").toReal() > 0, true);

        QCOMPARE(bindingLoops.count(), 0);
    }

    void shortcutReferenceFitsTheWindow_data() {
        QTest::addColumn<int>("windowWidth");
        QTest::addColumn<qreal>("scale");
        QTest::newRow("standard") << 1280 << qreal(1);
        QTest::newRow("narrow") << 394 << qreal(1);
        QTest::newRow("small-text") << 720 << qreal(0.5);
        QTest::newRow("large-text") << 720 << qreal(3);
        QTest::newRow("narrow-large-text") << 394 << qreal(3);
    }

    void shortcutReferenceFitsTheWindow() {
        QFETCH(int, windowWidth);
        QFETCH(qreal, scale);
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        Backend backend;
        backend.setDesktopTextScale(scale);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->setMinimumWidth(0);
        window->setWidth(windowWidth);
        window->setHeight(520);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        QObject *dialog = window->findChild<QObject *>(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);

        // All aliases open the dialog, and Escape closes it without changing text size.
        for (auto key : {Qt::Key_Slash, Qt::Key_Question, Qt::Key_F1}) {
            QTest::keyClick(window, key, key == Qt::Key_F1 ? Qt::NoModifier : Qt::ControlModifier);
            QTRY_VERIFY(dialog->property("opened").toBool());
            QObject *versionLabel = dialog->findChild<QObject *>(QStringLiteral("versionLabel"));
            QVERIFY(versionLabel);
            QVERIFY(versionLabel->property("visible").toBool());
            QCOMPARE(versionLabel->property("text").toString(),
                     QStringLiteral("Write ") + QCoreApplication::applicationVersion());
            QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
            QCOMPARE(backend.textScale(), scale);
            QVERIFY(dialog->property("x").toReal() >= 0);
            QVERIFY(dialog->property("y").toReal() >= 0);
            QVERIFY(dialog->property("x").toReal() + dialog->property("width").toReal()
                    <= window->width());
            QVERIFY(dialog->property("y").toReal() + dialog->property("height").toReal()
                    <= window->height());
            auto *grid = dialog->findChild<QQuickItem *>(QStringLiteral("shortcutsGrid"));
            QVERIFY(grid);
            QCoreApplication::processEvents();
            for (auto *row : grid->childItems()) {
                if (!row->inherits("QQuickRowLayout")) continue;
                QVERIFY(row->x() + row->width() <= grid->width() + 1);
                for (auto *label : row->childItems())
                    QVERIFY(label->x() + label->width() <= row->width() + 1);
            }
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!dialog->property("visible").toBool());
        }
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

    void autoSavesTitledDocumentsAfterEdits() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("draft.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);

        // Give the document a home on disk, then edit it without saving again.
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);
        editor->setProperty("text", QStringLiteral("auto-saved body"));
        QVERIFY(backend.editorTextChanged());

        // The debounced timer should flush to disk with no manual save.
        QTRY_VERIFY(savedSpy.count() >= 1);
        QVERIFY(!backend.modified());

        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(written.readAll()),
                 QStringLiteral("auto-saved body"));
    }

    void doesNotAutoSaveUntitledDocuments() {
        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);

        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);
        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        editor->setProperty("text", QStringLiteral("nameless draft"));
        QVERIFY(backend.editorTextChanged());

        // Wait past the auto-save debounce: an untitled draft must never auto-save
        // or silently raise the save picker. Crash recovery still covers it.
        QTest::qWait(1800);
        QCOMPARE(savedSpy.count(), 0);
        QCOMPARE(saveDialogSpy.count(), 0);
        QVERIFY(backend.modified());
        // Leave no draft behind for the next test's backend to recover.
        backend.discardRecovery();
    }

    void holdsAutoSaveWhenTheFileChangesElsewhere() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("shared.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        // Another program writes while our auto-save is pending.
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());
        writeFile(path, "theirs");

        // Their version survives the debounce, and further typing.
        QTRY_VERIFY(externalChangeSpy.count() >= 1);
        editor->setProperty("text", QStringLiteral("mine, still typing"));
        QVERIFY(backend.editorTextChanged());
        QTest::qWait(1200);
        QCOMPARE(readFile(path), QByteArray("theirs"));
        QVERIFY(backend.modified());

        // Keeping our version is a choice to overwrite, so auto-save resumes.
        backend.keepExternalVersion();
        QTRY_COMPARE(readFile(path), QByteArray("mine, still typing"));
        QVERIFY(!backend.modified());
    }

    void checksTheFileBeforeEachAutoSave() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("unwatched.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        // Change the file without the watcher seeing it, then let auto-save run.
        writeFileUnwatched(backend, path, "theirs");
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());

        QTRY_VERIFY(externalChangeSpy.count() >= 1);
        QTest::qWait(700);
        QCOMPARE(readFile(path), QByteArray("theirs"));
        QVERIFY(backend.modified());
        backend.discardRecovery();
    }

    void resumesAutoSaveOnlyAfterAnExplicitSave() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("dismissed.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        writeFile(path, "theirs");
        QTRY_VERIFY(externalChangeSpy.count() >= 1);

        // The prompt is dismissed (Escape): neither reload nor keep is chosen.
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());
        QTest::qWait(1200);
        QCOMPARE(readFile(path), QByteArray("theirs"));

        backend.save();
        QCOMPARE(readFile(path), QByteArray("mine"));

        editor->setProperty("text", QStringLiteral("mine, auto-saved again"));
        QVERIFY(backend.editorTextChanged());
        QTRY_COMPARE(readFile(path), QByteArray("mine, auto-saved again"));
    }

    void resumesAutoSaveAfterReloading() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("reloaded.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QVERIFY(backend.m_fileWatcher.files().contains(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        writeFile(path, "theirs");
        QCOMPARE(readFile(path), QByteArray("theirs"));
        // Test the reload/resume state machine independently of OS notification
        // timing. Native delivery remains covered by the watcher tests below.
        backend.checkWatchedFile();
        QTRY_VERIFY(externalChangeSpy.count() >= 1);
        backend.reloadFromDisk();
        QCOMPARE(editor->property("text").toString(), QStringLiteral("theirs"));

        editor->setProperty("text", QStringLiteral("theirs, edited"));
        QVERIFY(backend.editorTextChanged());
        QTRY_COMPARE(readFile(path), QByteArray("theirs, edited"));
    }

    void savedLfUtf8RemainsEligibleForAutoSaveAfterReopening() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("utf8-lf.md"));
        const QString text = QString::fromUtf8("caf\xc3\xa9\n\nlast line\n");
        {
            Backend backend;
            QScopedPointer<QObject> editor(makeEditor(backend)); QVERIFY(editor);
            editor->setProperty("text", text);
            backend.saveAs(QUrl::fromLocalFile(path));
            QCOMPARE(readFile(path), text.toUtf8());
        }
        Backend reopened;
        QScopedPointer<QObject> editor(makeEditor(reopened)); QVERIFY(editor);
        reopened.open(QUrl::fromLocalFile(path));
        QVERIFY(!reopened.m_autoSaveSuspended);
        editor->setProperty("text", text + QStringLiteral("next\n"));
        QVERIFY(reopened.editorTextChanged());
        QTRY_COMPARE(readFile(path), (text + QStringLiteral("next\n")).toUtf8());
    }

    // Saving writes UTF-8 with plain line endings, so a file holding anything
    // else is only rewritten by an explicit save, never by typing.
    void keepsOtherEncodingsAndLineEndingsOutOfAutoSave() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QList<QByteArray> originals{QByteArray("one\r\ntwo\r\n"),
                                          QByteArray("caf\xe9\n")};
        for (const QByteArray &original : originals) {
            const QString path = directory.filePath(QStringLiteral("kept.md"));
            writeFile(path, original);

            Backend backend;
            QScopedPointer<QObject> editor(makeEditor(backend));
            QVERIFY(editor);
            backend.open(QUrl::fromLocalFile(path));
            QVERIFY(backend.status().contains(QStringLiteral("Auto-save is off")));

            editor->setProperty("text", QStringLiteral("changed\n"));
            QVERIFY(backend.editorTextChanged());
            QVERIFY(!backend.m_autoSaveTimer.isActive());
            QTest::qWait(700);
            QCOMPARE(readFile(path), original);

            // Keeping our version over an outside edit does not lift it either.
            backend.keepExternalVersion();
            QVERIFY(!backend.m_autoSaveTimer.isActive());

            // An explicit save still writes.
            backend.save();
            QCOMPARE(readFile(path), QByteArray("changed\n"));
        }
    }

    void holdsAutoSaveWhileTheWriterDecides() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("deciding.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);

        // Closing right after typing opens the unsaved-changes prompt.
        editor->setProperty("text", QStringLiteral("undecided"));
        QVERIFY(backend.editorTextChanged());
        backend.holdAutoSave(true);
        QTest::qWait(1200);
        QCOMPARE(savedSpy.count(), 0);
        QCOMPARE(readFile(path), QByteArray());

        // Cancel lets auto-save carry on.
        backend.holdAutoSave(false);
        QTRY_COMPARE(readFile(path), QByteArray("undecided"));
    }

    void startsANamedFileThatIsNotThereYet() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("new.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);

        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(backend.fileName(), QStringLiteral("new.md"));
        QCOMPARE(backend.status(), QStringLiteral("New file new.md"));
        QVERIFY(!backend.modified());
        QVERIFY(!QFileInfo::exists(path));

        // Typing does not auto-save the file into existence.
        editor->setProperty("text", QStringLiteral("first words"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
        QTest::qWait(800);
        QVERIFY(!QFileInfo::exists(path));

        // The first save lands on the name given, without asking for one.
        QSignalSpy saveAsSpy(&backend, &Backend::saveDialogRequested);
        backend.save();
        QCOMPARE(saveAsSpy.count(), 0);
        QCOMPARE(readFile(path), QByteArray("first words"));
        QVERIFY(!backend.modified());

        // From then on it is an ordinary file, auto-saved like any other.
        editor->setProperty("text", QStringLiteral("more words"));
        QVERIFY(backend.editorTextChanged());
        QTRY_COMPARE(readFile(path), QByteArray("more words"));
        QCOMPARE(externalSpy.count(), 0);
    }

    void refusesANewFileInAMissingFolder() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("missing/new.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(backend.status(), QStringLiteral("Could not open new.md."));
        QVERIFY(backend.fileName() != QStringLiteral("new.md"));
    }

    void namesARelativeFileByItsFullPath() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(directory.path()));

        Backend backend;
        backend.openOrStartFile(QUrl::fromLocalFile(QStringLiteral("relative.md")));
        QDir::setCurrent(previous);

        // A recovery draft written later must not depend on the directory.
        const QFileInfo actual(backend.fileUrl().toLocalFile());
        QVERIFY(actual.isAbsolute());
        QCOMPARE(actual.fileName(), QStringLiteral("relative.md"));
        // macOS aliases /var to /private/var. The new file does not exist
        // yet, so canonicalize its existing parent rather than the file.
        QCOMPARE(QFileInfo(actual.absolutePath()).canonicalFilePath(),
                 QFileInfo(directory.path()).canonicalFilePath());
    }

    void saysWhenARecoveredDraftKeepsAFileFromOpening() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("asked.md"));
        writeFile(path, "on disk");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        // As after restoring a draft: the window holds unsaved text.
        editor->setProperty("text", QStringLiteral("recovered"));
        QVERIFY(backend.editorTextChanged());

        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(backend.status(),
                 QStringLiteral("Recovered unsaved changes; asked.md was not opened."));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("recovered"));
        backend.discardRecovery();
    }

    void dropsAPendingCloseWhenItsSaveFails() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        // The writer chose Save in the close prompt, and the save failed.
        window->setProperty("pendingAction", QStringLiteral("close"));
        window->setProperty("awaitingPendingSave", true);
        QSignalSpy failedSpy(&backend, &Backend::saveFailed);
        backend.saveAs(QUrl::fromLocalFile(QStringLiteral("/nonexistent-dir/x.md")));
        QCOMPARE(failedSpy.count(), 1);

        // A later save, such as an auto-save, must not close the window.
        QCOMPARE(window->property("pendingAction").toString(), QString());
        QVERIFY(!window->property("awaitingPendingSave").toBool());
    }

    void savesInAFolderItCannotWriteNewFilesIn() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("shared.md"));
        writeFile(path, "before");
        QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner);

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("after"));
        QVERIFY(backend.editorTextChanged());
        backend.save();
        QFile::setPermissions(directory.path(),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        QVERIFY(!backend.modified());
        QCOMPARE(readFile(path), QByteArray("after"));
    }

    void removesARecoveryDraftItCannotRead() {
        Backend backend;
        const QString draft = backend.m_recoveryPath;
        QVERIFY(!draft.isEmpty());
        writeFile(draft, "not json");

        // Attaching the document is when a draft is restored.
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        QVERIFY(!QFileInfo::exists(draft));
        QVERIFY(!backend.modified());
    }

    void keepsWatchingAFileReplacedFromOutside_data() {
        QTest::addColumn<bool>("parentOnly");
        QTest::addColumn<bool>("busyParent");
        QTest::newRow("file-and-parent") << false << false;
        QTest::newRow("parent-fallback") << true << false;
        QTest::newRow("busy-parent-fallback") << true << true;
    }

    void keepsWatchingAFileReplacedFromOutside() {
        QFETCH(bool, parentOnly);
        QFETCH(bool, busyParent);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("watched.md"));
        writeFile(path, "first");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);

        QVERIFY(backend.m_fileWatcher.files().contains(path));
        QVERIFY(backend.m_fileWatcher.directories().contains(directory.path()));
        QTimer churn;
        int churnCount = 0;
        const QString sibling = directory.filePath(QStringLiteral("sibling"));
        connect(&churn, &QTimer::timeout, this, [&] {
            if (++churnCount % 2)
                writeFile(sibling, "unrelated");
            else
                QFile::remove(sibling);
        });
        if (busyParent)
            churn.start(20);
        // No yield before replacement: an immediate save must also be noticed.
        const QString replacement = directory.filePath(QStringLiteral("replacement"));
        for (const QByteArray &contents : {QByteArray("second"), QByteArray("third")}) {
            externalSpy.clear();
            if (parentOnly)
                QVERIFY(backend.m_fileWatcher.removePath(path));
            writeFile(replacement, contents);
            QVERIFY(QFile::remove(path));
            QVERIFY(QFile::rename(replacement, path));
            QTRY_VERIFY(externalSpy.count() >= 1);
            if (busyParent) {
                QVERIFY(churn.isActive());
                QVERIFY(churnCount > 0);
            }
            QVERIFY(!externalSpy.first().at(0).toBool());
            QVERIFY(backend.m_autoSaveSuspended);
            QTRY_VERIFY(backend.m_fileWatcher.files().contains(path));
            QVERIFY(backend.m_fileWatcher.directories().contains(directory.path()));
            backend.reloadFromDisk();
            QCOMPARE(backend.currentDocumentText(), QString::fromUtf8(contents));
        }
    }

    void directoryActivityDoesNotRepeatAnUnresolvedConflict() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("watched.md"));
        const QString sibling = directory.filePath(QStringLiteral("sibling"));
        writeFile(path, "first");
        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);
        writeFile(sibling, "unrelated");
        backend.checkWatchedFile();
        QCOMPARE(externalSpy.count(), 0);

        writeFile(path, "second");
        QTRY_COMPARE(externalSpy.count(), 1);
        QCOMPARE(backend.currentDocumentText(), QStringLiteral("first"));
        QVERIFY(backend.m_autoSaveSuspended);
        QVERIFY(!backend.m_hasKnownFileContents);
        QVERIFY(QFile::remove(sibling));
        backend.checkWatchedFile();
        QTest::qWait(250);
        QCOMPARE(externalSpy.count(), 1);

        // A genuinely different conflict must still be reported.
        writeFile(path, "third");
        QTRY_COMPARE(externalSpy.count(), 2);
        backend.reloadFromDisk();
        QVERIFY(!backend.m_autoSaveSuspended);
        writeFile(path, "second");
        QTRY_COMPARE(externalSpy.count(), 3);

        // Moving to another document drops the obsolete file and parent.
        QTemporaryDir other;
        QVERIFY(other.isValid());
        const QString otherPath = other.filePath(QStringLiteral("other.md"));
        writeFile(otherPath, "other");
        backend.open(QUrl::fromLocalFile(otherPath));
        QCOMPARE(backend.m_fileWatcher.files(), QStringList{otherPath});
        QCOMPARE(backend.m_fileWatcher.directories(), QStringList{other.path()});
    }

    void keepsTheWritersTextWhenTheChangeDialogIsDismissed() {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Controls\nApplicationWindow {"
                          " property alias dialog: dialog\n"
                          " ExternalChangeDialog { id: dialog } }",
                          QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY(window);
        QObject *dialog = window->property("dialog").value<QObject *>();
        QVERIFY(dialog);
        QSignalSpy keepSpy(dialog, SIGNAL(keepRequested()));
        QSignalSpy reloadSpy(dialog, SIGNAL(reloadRequested()));

        // Dismissing a changed-file prompt does not resume auto-save.
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(keepSpy.count(), 0);
        QCOMPARE(reloadSpy.count(), 0);
        // A deleted file still becomes an unsaved document when dismissed.
        dialog->setProperty("deleted", true);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_COMPARE(keepSpy.count(), 1);
    }

    void opensAnExistingFileFromTheCommandLine() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("notes.md"));
        writeFile(path, "already here");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("already here"));
        QCOMPARE(backend.status(), QStringLiteral("Opened notes.md"));
    }

    void keepsDiscardedChangesOutOfTheFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("draft.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("original"));
        QVERIFY(backend.editorTextChanged());
        backend.saveAs(QUrl::fromLocalFile(path));

        // The writer edits, then picks Discard to open a file that fails.
        backend.holdAutoSave(true);
        editor->setProperty("text", QStringLiteral("changes to throw away"));
        QVERIFY(backend.editorTextChanged());
        backend.discardRecovery();
        backend.open(QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing.md"))));
        backend.holdAutoSave(false);

        QTest::qWait(800);
        QCOMPARE(readFile(path), QByteArray("original"));
    }

    void forgetsTheSavedTextWhenTheFileChangedUnseen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("draft.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("original"));
        QVERIFY(backend.editorTextChanged());
        backend.saveAs(QUrl::fromLocalFile(path));

        // The file changes without the watcher noticing; the pre-save check
        // catches it.
        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);
        writeFileUnwatched(backend, path, "theirs");
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());
        QTRY_VERIFY(externalSpy.count() >= 1);

        // Undoing back to the old text is not a return to what is on disk.
        editor->setProperty("text", QStringLiteral("original"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
        backend.discardRecovery();
    }

    void doesNotAutoSaveARecoveredDraftOverItsFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("recovered.md"));
        const QUrl url = QUrl::fromLocalFile(path);

        {
            // A session that ends without saving leaves a recovery draft.
            Backend crashed;
            QScopedPointer<QObject> editor(makeEditor(crashed));
            QVERIFY(editor);
            crashed.saveAs(url);
            editor->setProperty("text", QStringLiteral("draft"));
            crashed.m_autoSaveSuspended = true; // stand in for a crash before auto-save
            QVERIFY(crashed.editorTextChanged());
            QTest::qWait(1000);
        }
        QCOMPARE(readFile(path), QByteArray());

        // The file moves on before Write comes back.
        writeFile(path, "newer on disk");

        // Earlier tests may have left drafts of their own; claim until ours.
        for (int attempt = 0; attempt < 100; ++attempt) {
            Backend restored;
            QScopedPointer<QObject> editor(makeEditor(restored));
            QVERIFY(editor);
            if (restored.fileUrl() != url) {
                restored.discardRecovery();
                continue;
            }

            QCOMPARE(editor->property("text").toString(), QStringLiteral("draft"));
            editor->setProperty("text", QStringLiteral("draft, edited"));
            QVERIFY(restored.editorTextChanged());
            QTest::qWait(1200);
            QCOMPARE(readFile(path), QByteArray("newer on disk"));
            QVERIFY(restored.modified());

            restored.save();
            QCOMPARE(readFile(path), QByteArray("draft, edited"));
            return;
        }
        QFAIL("The recovery draft was not restored.");
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

    void treatsAFileRemovedAndWrittenAgainAsChanged() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("replaced.md"));
        writeFile(path, "hello");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        // The watcher sees the file gone before it is written again.
        QVERIFY(QFile::remove(path));
        QTest::qWait(30);
        writeFile(path, "theirs");
        QTRY_COMPARE(externalChangeSpy.count(), 1);
        QCOMPARE(externalChangeSpy.first().at(0).toBool(), false);

        // The new file is watched, so later edits are noticed too.
        writeFile(path, "theirs, again");
        QTRY_COMPARE(externalChangeSpy.count(), 2);
        backend.reloadFromDisk();
        QCOMPARE(editor->property("text").toString(), QStringLiteral("theirs, again"));
    }

    void noticesARemovedFileComingBack() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("back.md"));
        writeFile(path, "hello");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(externalChangeSpy.count(), 1);
        QCOMPARE(externalChangeSpy.last().at(0).toBool(), true);

        writeFile(path, "theirs");
        QTRY_COMPARE(externalChangeSpy.count(), 2);
        QCOMPARE(externalChangeSpy.last().at(0).toBool(), false);

        // And removed again before the writer answers.
        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(externalChangeSpy.count(), 3);
        QCOMPARE(externalChangeSpy.last().at(0).toBool(), true);
    }

    void hidesReloadWhileTheFileIsRemoved() {
        const QString dialogPath = QFINDTESTDATA("../src/ExternalChangeDialog.qml");
        QVERIFY(!dialogPath.isEmpty());
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\n"
                          "Window {\n"
                          "    width: 600; height: 400; visible: true\n"
                          "    property alias dialog: dialog\n"
                          "    ExternalChangeDialog { id: dialog; deleted: true }\n"
                          "}\n",
                          QUrl::fromLocalFile(QFileInfo(dialogPath).dir().filePath(
                              QStringLiteral("Harness.qml"))));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *dialog = window->property("dialog").value<QObject *>();
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QObject *reload = dialog->findChild<QObject *>(QStringLiteral("reloadButton"));
        QVERIFY(reload);

        QVERIFY(!reload->property("visible").toBool());
        dialog->setProperty("deleted", false);
        QVERIFY(reload->property("visible").toBool());

        // Removed while Reload has the focus: Enter keeps the text instead.
        QVERIFY(QMetaObject::invokeMethod(reload, "forceActiveFocus"));
        QTRY_VERIFY(reload->property("activeFocus").toBool());
        dialog->setProperty("deleted", true);
        QObject *keep = dialog->findChild<QObject *>(QStringLiteral("keepButton"));
        QVERIFY(keep);
        QTRY_VERIFY(keep->property("activeFocus").toBool());
    }

private:
    // The character formats the highlighter gives a single line of text.
    static QList<QTextCharFormat> highlightedFormats(const QString &text, int caret = -1) {
        QTextDocument document;
        document.setPlainText(text);
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();
        if (caret >= 0)
            highlighter.setCaret(caret);
        QList<QTextCharFormat> formats(text.length());
        for (const auto &range : document.firstBlock().layout()->formats()) {
            for (int i = range.start; i < range.start + range.length; ++i)
                formats[i] = range.format;
        }
        return formats;
    }

    // Windows Qt resolves the OS account's profile before HOME/USERPROFILE.
    // Redirect only our private theme path, never the process/user environment.
    static void useThemeHome(Backend &backend, const QString &path) {
        backend.m_themeHome = path;
        backend.loadOmarchyTheme();
        backend.watchOmarchyTheme();
    }

    // Builds a scratch theme tree; each backend opts into it explicitly.
    struct ScopedTheme {
        explicit ScopedTheme(const QByteArray &palette) {
            const QString themeDirectory = home.path()
                + QStringLiteral("/.local/state/omarchy/current/theme");
            QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
            ok = home.isValid() && QDir().mkpath(themeDirectory)
                && colorsFile.open(QIODevice::WriteOnly | QIODevice::Text)
                && colorsFile.write(palette) == qint64(palette.size());
        }
        QTemporaryDir home;
        bool ok = false;
    };

    // Builds a minimal TextEdit and attaches its document to the backend, so
    // editorTextChanged() sees real edits. The engine outlives the returned
    // editor because it is a data member.
    QObject *makeEditor(Backend &backend) {
        QQmlComponent component(&m_engine);
        component.setData(
            "import QtQuick\nTextEdit { property var doc: textDocument }", QUrl());
        if (!component.isReady()) {
            qWarning() << component.errorString();
            return nullptr;
        }
        QObject *editor = component.create();
        if (editor)
            backend.attachDocument(editor->property("doc").value<QObject *>());
        return editor;
    }

    static void writeFile(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(contents), qint64(contents.size()));
    }

    // Writes while the backend is not watching, as when an event is missed.
    static void writeFileUnwatched(Backend &backend, const QString &path,
                                   const QByteArray &contents) {
        backend.unwatchFiles();
        writeFile(path, contents);
    }

    static QByteArray readFile(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
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

    QQmlEngine m_engine;
    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteEditorTest)
#include "tst_editor.moc"

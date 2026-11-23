#include <QtTest>
#include <QThread>
#include <atomic>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickTextDocument>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextLayout>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QAbstractTextDocumentLayout>
#include <QTextTable>
#include <QInputMethodEvent>
#include <QTemporaryDir>
#include <QImage>
#include <QClipboard>
#include <QMimeData>
#include <QSignalSpy>
#include <QSignalBlocker>
#include <QScopeGuard>
#include "../../src/mermaidrenderer.h"
#include "../../src/livepreview.h"
#include "../../src/markdowntablemodel.h"
#include "../../src/codehighlighter.h"
#include "../../src/markdownhighlighter.h"

class ImageResolver : public QObject {
    Q_OBJECT
public:
    QString root;
    Q_INVOKABLE QUrl resolveImageUrl(const QString &path) {
        return path.startsWith("https:") ? QUrl() : QUrl::fromLocalFile(root + "/" + path);
    }
};

class LivePreviewTest : public QObject {
    Q_OBJECT
private:
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    std::unique_ptr<LivePreview> controller;
    QObject *sourceItem = nullptr, *viewItem = nullptr;
    QTextDocument *source = nullptr, *view = nullptr;
    const QString markdown = QString::fromUtf8("😀 before\n\n| A | B |\n| --- | --- |\n| one | two |\n\nafter\n");
    void flush() { QCoreApplication::processEvents(); QCoreApplication::processEvents(); }
    void select(QObject *item, int anchor, int caret) {
        QVERIFY(QMetaObject::invokeMethod(item, "select", Q_ARG(int, anchor), Q_ARG(int, caret)));
        flush();
    }
    void type(const QString &text) {
        for (const QChar c : text) {
            QKeyEvent event(QEvent::KeyPress, c.unicode(), Qt::NoModifier, QString(c));
            QCoreApplication::sendEvent(viewItem, &event);
            flush();
        }
    }
private slots:
    void init() {
        QQmlComponent component(&engine);
        component.setData(
            "import QtQuick\n"
            "Item { width: 600; height: 800\n"
            "TextEdit { objectName: 'source'; visible: false; textFormat: TextEdit.PlainText }\n"
            "TextEdit { objectName: 'view'; width: 550; height: 800\n"
            "textFormat: TextEdit.RichText; wrapMode: TextEdit.Wrap\n"
            "font.pixelSize: 20; color: '#222222'\n"
            "property color previewBackground: 'white'\n"
            "property color previewAccent: 'blue'\n"
            "property bool previewDark: false\n"
            "} }", QUrl());
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        sourceItem = root->findChild<QObject *>("source");
        viewItem = root->findChild<QObject *>("view");
        source = qvariant_cast<QQuickTextDocument *>(sourceItem->property("textDocument"))->textDocument();
        view = qvariant_cast<QQuickTextDocument *>(viewItem->property("textDocument"))->textDocument();
        source->setPlainText(markdown);
        source->clearUndoRedoStacks();
        controller = std::make_unique<LivePreview>();
        auto *quick = qvariant_cast<QQuickTextDocument *>(viewItem->property("textDocument"));
        connect(quick, &QQuickTextDocument::textDocumentChanged, root.get(), [this, quick] {
            view = quick->textDocument(); // Modes now retain distinct native documents.
        });
        controller->attach(sourceItem, viewItem);
        view = qvariant_cast<QQuickTextDocument *>(viewItem->property("textDocument"))->textDocument();
        flush();
    }
    void cleanup() { controller.reset(); root.reset(); }
    void footnoteNumbersStayConsistentAroundTables() {
        const QString text = QStringLiteral("# Heading\n\n| A | B |\n| --- | --- |\n| note[^a] | cell |\n\nProse[^b]\n\n[^a]: Table note\n[^b]: Prose note\n");
        source->setPlainText(text); sourceItem->setProperty("cursorPosition", 0); flush();
        source->clearUndoRedoStacks();
        const MarkdownReferences::Index index(text);
        QCOMPARE(index.numbers.value("b"), 2);
        const int marker = text.indexOf("[^b]");
        QCOMPARE(view->characterAt(controller->viewPosition(marker)), QChar::ObjectReplacementCharacter);
        controller->setReadMode(true); flush();
        bool found = false;
        for (auto block = view->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().anchorHref() == MarkdownReferences::noteUrl("b")) {
                    QCOMPARE(it.fragment().text(), QStringLiteral("2")); found = true;
                }
        QVERIFY(found);
        QVERIFY(view->toPlainText().contains("1. Table note"));
        QVERIFY(view->toPlainText().contains("2. Prose note"));
        QCOMPARE(source->toPlainText(), text); QVERIFY(!source->isUndoAvailable());
    }
    void editFootnotesCollapseRevealRenumberAndPreserveUndo() {
        const QString text = QStringLiteral("A[^b] B[^a] C[^b]\n\n[^a]: Alpha\n[^b]: Beta\n");
        source->setPlainText(text);
        sourceItem->setProperty("cursorPosition", 0); flush();
        source->clearUndoRedoStacks();
        const int first = text.indexOf("[^b]"), last = text.indexOf("C[^b]") + 1;
        const auto imageAt = [&](int position) {
            QTextCursor cursor(view); cursor.setPosition(controller->viewPosition(position));
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            return cursor.charFormat().toImageFormat();
        };
        QCOMPARE(view->characterAt(controller->viewPosition(first)), QChar::ObjectReplacementCharacter);
        const auto initial = imageAt(last);
        QVERIFY(initial.isImageFormat());
        QCOMPARE(initial.verticalAlignment(), QTextCharFormat::AlignSuperScript);
        QVERIFY(initial.height() < QFontMetricsF(view->defaultFont()).height());
        sourceItem->setProperty("cursorPosition", first + 2); flush();
        QVERIFY(view->toPlainText().contains("A[^b]"));
        QCOMPARE(source->toPlainText(), text); QVERIFY(!source->isUndoAvailable());
        sourceItem->setProperty("cursorPosition", 0); flush();
        QCOMPARE(view->characterAt(controller->viewPosition(first)), QChar::ObjectReplacementCharacter);
        controller->setSearchHighlight(QStringLiteral("A[^b]"), 0); flush();
        QVERIFY(view->toPlainText().contains("A[^b]"));
        controller->setSearchHighlight({}, -1); flush();
        QCOMPARE(view->characterAt(controller->viewPosition(first)), QChar::ObjectReplacementCharacter);
        // Renumber a different, unchanged range without changing marker lengths.
        QTextCursor edit(source); edit.setPosition(first + 2);
        edit.setPosition(first + 3, QTextCursor::KeepAnchor); edit.insertText("a"); flush();
        QVERIFY(imageAt(last).name() != initial.name());
        source->undo(); flush(); QCOMPARE(source->toPlainText(), text);
        select(viewItem, controller->viewPosition(first + 4), controller->viewPosition(first + 4));
        QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QCoreApplication::sendEvent(viewItem, &backspace); flush();
        QString deleted = text; deleted.remove(first + 3, 1);
        QCOMPARE(source->toPlainText(), deleted);
        source->undo(); flush(); QCOMPARE(source->toPlainText(), text);
    }
    void tableModelEditsAreSignalOnlyBoundedAndSafe() {
        MarkdownTableModel model;
        QString table = "| A | B |\n| :--- | ---: |\n| x | y |";
        model.setSource(table, QFont());
        QCOMPARE(model.rowCount(), 2); QCOMPARE(model.columnCount(), 2);
        QSignalSpy request(&model, &MarkdownTableModel::replaceRequested);
        bool accept = true;
        connect(&model, &MarkdownTableModel::replaceRequested, &model, [&] { if (accept) model.acknowledgeReplacement(); });
        QVERIFY(model.setCell(1, 1, QString::fromUtf8("😀 | literal")));
        QCOMPARE(request.size(), 1);
        QCOMPARE(model.data(model.index(1, 1), MarkdownTableModel::MarkdownRole).toString(), QString("y"));
        auto apply = [&] {
            const auto args = request.takeLast(); request.clear();
            table.replace(args[0].toInt(), args[1].toInt(), args[2].toString());
            model.setSource(table, QFont());
        };
        apply(); QVERIFY(table.contains(QString::fromUtf8("😀 \\| literal")));
        QCOMPARE(model.columnAlignment(1), int(Qt::AlignRight));
        QVERIFY(model.setColumnAlignment(1, "center")); apply();
        QCOMPARE(model.columnAlignment(1), int(Qt::AlignHCenter)); QVERIFY(table.contains(":---:"));
        QVERIFY(model.setColumnAlignment(1, "left")); apply(); QCOMPARE(model.columnAlignment(1), int(Qt::AlignLeft));
        QVERIFY(!model.setColumnAlignment(-1, "left")); QVERIFY(!model.setColumnAlignment(0, "bogus"));
        QVERIFY(model.setColumnAlignment(1, "right")); apply();
        accept = false; QVERIFY(!model.setCell(1, 1, "stale")); request.clear(); accept = true;
        QVERIFY(!model.setCell(0, 0, "line\nbreak"));
        QVERIFY(!model.removeRow(0));
        QVERIFY(model.insertRow(1)); apply(); QCOMPARE(model.rowCount(), 3);
        QVERIFY(model.insertColumn(0)); apply(); QCOMPARE(model.columnCount(), 3);
        QVERIFY(table.contains(":---")); QVERIFY(table.contains("---:"));
        QVERIFY(model.removeColumn(1)); apply(); QCOMPARE(model.columnCount(), 2);
        QVERIFY(model.removeRow(2)); apply(); QCOMPARE(model.rowCount(), 2);
        model.setSource("| A |\n| --- |\n| ![x](https://invalid.example/a.png) <script>x</script> |", QFont());
        const auto html = model.data(model.index(1, 0), MarkdownTableModel::HtmlRole).toString();
        QVERIFY(!html.contains("<img")); QVERIFY(!html.contains("<script>"));
        QVERIFY(!model.removeColumn(0));
        model.setProperty("drafts", QVariantMap{{"1:0", QVariantMap{{"text", "uncommitted"}, {"original", ""}}}});
        const auto revision = model.draftRevision();
        model.setSource("| A |\n| --- |\n| different |", QFont());
        QVERIFY(model.draftRevision() > revision);
        const auto recovered = model.property("drafts").toMap();
        QVERIFY(!recovered.contains("1:0")); QVERIFY(recovered.firstKey().startsWith("recovered/"));
        QCOMPARE(recovered.first().toMap()["text"].toString(), QString("uncommitted"));
        model.setSource(QString(4 * 1024 * 1024 + 1, 'x'), QFont()); QCOMPARE(model.rowCount(), 0);
    }
    void rectangularTableFillIsAtomicBoundedAndRevisionChecked() {
        const QString original = "before\n\n| A | B | C |\n| :--- | ---: | --- |\n| a | b | untouched |\n| c | d | retained |\n| e | f | stable |\n\nafter";
        source->setPlainText(original); select(sourceItem, 0, 0); flush();
        auto *model = qobject_cast<MarkdownTableModel *>(qvariant_cast<QObject *>(controller->tablePanels().first().toMap()["model"])); QVERIFY(model);
        source->clearUndoRedoStacks(); QSignalSpy requests(model, &MarkdownTableModel::replaceRequested);
        const auto revision = model->sourceRevision();
        QVERIFY(model->fillCells(1, 0, 3, 1, QString::fromUtf8("😀 | fill"), revision)); flush();
        QCOMPARE(requests.count(), 1);
        for (int r = 1; r <= 3; ++r) for (int c = 0; c < 2; ++c)
            QCOMPARE(model->data(model->index(r, c), MarkdownTableModel::MarkdownRole).toString(), QString::fromUtf8("😀 \\| fill"));
        QVERIFY(source->toPlainText().contains("| :--- | ---: | --- |"));
        QVERIFY(source->toPlainText().contains("| retained |"));
        QVERIFY(!model->fillCells(1, 0, 3, 1, "stale", revision));
        source->undo(); flush(); QCOMPARE(source->toPlainText(), original); QVERIFY(!source->isUndoAvailable());
        source->redo(); flush(); QVERIFY(model->fillCells(1, 0, 3, 1, "", model->sourceRevision())); flush();
        QCOMPARE(model->rowCount(), 4); QCOMPARE(model->columnCount(), 3);
        const QString beforeReject = source->toPlainText();
        QVERIFY(!model->fillCells(0, 0, 3, 2, QString(65537, 'x'), model->sourceRevision()));
        QVERIFY(!model->fillCells(0, 0, 4, 2, "bad", model->sourceRevision()));
        QVERIFY(!model->fillCells(0, 0, 3, 2, "new\nline", model->sourceRevision()));
        QCOMPARE(source->toPlainText(), beforeReject);
        MarkdownTableModel bounded; bounded.setSource("| A |\n| --- |\n" + QString("| x |\n").repeated(70), QFont());
        QSignalSpy oversized(&bounded, &MarkdownTableModel::replaceRequested);
        QVERIFY(!bounded.fillCells(0, 0, 70, 0, QString(65536, 'x'), bounded.sourceRevision())); QCOMPARE(oversized.count(), 0);
        controller->setReadMode(true); flush();
        QVERIFY(!model->fillCells(0, 0, 3, 2, "Read", model->sourceRevision()));
        controller->setReadMode(false); flush();
        MarkdownTableModel unbordered; QString text = "A | B\r\n:--- | ---:\r\na | b\r\nc | d";
        unbordered.setSource(text, QFont());
        QSignalSpy borderRequests(&unbordered, &MarkdownTableModel::replaceRequested);
        QVERIFY(!unbordered.fillCells(0, 0, 2, 1, "", unbordered.sourceRevision())); // No canonical ACK.
        connect(&unbordered, &MarkdownTableModel::replaceRequested, &unbordered, [&](int at, int length, const QString &replacement) {
            text.replace(at, length, replacement); unbordered.acknowledgeReplacement(); unbordered.setSource(text, QFont());
        });
        QVERIFY(unbordered.fillCells(0, 0, 2, 1, "", unbordered.sourceRevision()));
        QCOMPARE(unbordered.rowCount(), 3); QCOMPARE(unbordered.columnCount(), 2);
        QVERIFY(text.contains(":--- | ---:")); QCOMPARE(text.count("\r\n"), 3);
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 2; ++c)
            QVERIFY(unbordered.data(unbordered.index(r, c), MarkdownTableModel::MarkdownRole).toString().isEmpty());
        MarkdownTableModel exact; QString atLimit = "|A|\n|---|\n" + ("|" + QString(65500, 'x') + "|\n").repeated(64);
        atLimit += "|" + QString(4 * 1024 * 1024 - atLimit.size() - 3, 'y') + "|\n";
        exact.setSource(atLimit, QFont()); QCOMPARE(exact.rowCount(), 66);
        QSignalSpy exactRequests(&exact, &MarkdownTableModel::replaceRequested);
        QVERIFY(exact.fillCells(1, 0, 1, 0, QString(65500, 'x'), exact.sourceRevision())); QCOMPARE(exactRequests.count(), 0);
        connect(&exact, &MarkdownTableModel::replaceRequested, &exact, [&](int at, int length, const QString &replacement) {
            atLimit.replace(at, length, replacement); exact.acknowledgeReplacement(); exact.setSource(atLimit, QFont());
        });
        QVERIFY(exact.fillCells(1, 0, 1, 0, QString(65500, 'z'), exact.sourceRevision())); QCOMPARE(exactRequests.count(), 1);
        QCOMPARE(atLimit.size(), 4 * 1024 * 1024);
    }
    void tableGridUsesCanonicalUndoAndSourceEscape() {
        const auto panel = controller->tablePanels().first().toMap();
        auto *model = qobject_cast<MarkdownTableModel *>(qvariant_cast<QObject *>(panel["model"])); QVERIFY(model);
        const QString original = source->toPlainText(); source->clearUndoRedoStacks();
        QVERIFY(model->setCell(1, 1, "**changed**")); flush();
        QVERIFY(source->toPlainText().contains("**changed**"));
        QCOMPARE(controller->tablePanels().first().toMap()["id"], panel["id"]);
        source->undo(); flush(); QCOMPARE(source->toPlainText(), original); QVERIFY(!source->isUndoAvailable());
        controller->setTableSource(panel["id"].toInt(), true); flush();
        QVERIFY(controller->tablePanels().isEmpty()); QVERIFY(view->toPlainText().contains("| A | B |"));
        const auto raw = controller->codeBlockPanels().first().toMap(); QCOMPARE(raw["tableId"].toInt(), panel["id"].toInt());
        for (auto b = view->findBlockByNumber(raw["firstBlock"].toInt()); b.blockNumber() <= raw["lastBlock"].toInt(); b = b.next())
            QVERIFY(b.blockFormat().nonBreakableLines());
        select(sourceItem, source->characterCount() - 1, source->characterCount() - 1);
        QVERIFY(!controller->tablePanels().isEmpty());
        controller->setReadMode(true); flush(); QCOMPARE(controller->tablePanels().size(), 1);
        QVERIFY(controller->tableScrollPanels().isEmpty());
        QCOMPARE(qvariant_cast<QObject *>(controller->tablePanels().first().toMap()["model"]), static_cast<QObject *>(model));
        QCOMPARE(model->data(model->index(1, 0), MarkdownTableModel::MarkdownRole).toString(), QString("one"));
        QVERIFY(!model->setCell(1, 0, "Read must not edit"));
        QCOMPARE(source->toPlainText(), original);
    }
    void tableSizingAndAcknowledgedParsingStayCached() {
        MarkdownTableModel model; const QFont font("monospace", 18);
        QString text = "| A | B |\n| --- | --- |\n| a deliberately wide maximum | plain |\n| x | **rich** |";
        model.setSource(text, font); const qreal width = model.columnWidth(0); model.columnWidth(1);
        const auto measurements = model.m_widthMeasurements;
        model.columnWidth(0); model.columnWidth(1); QCOMPARE(model.m_widthMeasurements, measurements);
        model.data(model.index(1, 0), MarkdownTableModel::HtmlRole); QCOMPARE(model.m_richRenders, quint64(0));
        model.data(model.index(2, 1), MarkdownTableModel::HtmlRole); QCOMPARE(model.m_richRenders, quint64(1));
        QSignalSpy widthSignals(&model, &MarkdownTableModel::columnWidthsChanged);
        connect(&model, &MarkdownTableModel::replaceRequested, &model, [&](int at, int length, const QString &replacement) {
            text.replace(at, length, replacement); model.acknowledgeReplacement();
        });
        const auto parses = model.m_parseCount;
        QVERIFY(model.setCell(2, 0, "xx")); QCOMPARE(model.m_parseCount, parses + 1);
        QVERIFY(!model.m_validatedRows.isEmpty()); model.setSource(text, font);
        QCOMPARE(model.m_parseCount, parses + 1); QVERIFY(model.m_validatedRows.isEmpty()); // Exact ACKed candidate consumed, no second parse.
        QCOMPARE(model.m_widthMeasurements, measurements + 1); QCOMPARE(model.columnWidth(0), width); QCOMPARE(widthSignals.count(), 0);
        QVERIFY(model.setCell(1, 0, QString(150, 'W'))); model.setSource(text, font);
        QCOMPARE(widthSignals.count(), 1); QVERIFY(model.columnWidth(0) > width);
        // Foreign edits must never consume a previously validated proposal.
        QVERIFY(model.setCell(2, 0, "xxx")); text.replace("plain", "foreign");
        const auto beforeForeign = model.m_parseCount; model.setSource(text, font);
        QCOMPARE(model.m_parseCount, beforeForeign + 1); QVERIFY(model.m_validatedRows.isEmpty());
        QCOMPARE(model.data(model.index(1, 1), MarkdownTableModel::MarkdownRole).toString(), QString("foreign"));
        const auto renders = model.m_richRenders;
        model.setSource(text, QFont("monospace", 24)); model.columnWidth(0);
        QVERIFY(model.m_widthMeasurements > measurements + 2); QVERIFY(widthSignals.count() >= 2);
        model.data(model.index(1, 1), MarkdownTableModel::HtmlRole); QCOMPARE(model.m_richRenders, renders);
        // Markdown/autolinks/entities/escapes retain the resource-free parser.
        for (const QString &value : {QString("https://example.com"), QString("&amp;"), QString("<img src='file:///no'>"), QString("\\\\*")}) {
            MarkdownTableModel rich; rich.setSource("| A |\n| --- |\n| " + value + " |", font);
            const QString html = rich.data(rich.index(1, 0), MarkdownTableModel::HtmlRole).toString();
            QVERIFY(!html.contains("<img", Qt::CaseInsensitive)); QCOMPARE(rich.m_richRenders, quint64(1));
        }
    }
    void readTableSlotsUseExactSessionsAndReplacementInvalidatesEditors() {
        const QString table = "| A | B |\n| --- | --- |\n| one | two |";
        const QString text = "![forged](preview:table/not-internal/1)\n\n" + table
            + "\n\n```mermaid\n%%{init: {}}%%\ngraph TD; a-->b\n```\n\n" + table + "\n\nafter";
        source->setPlainText(text); select(sourceItem, 0, 0);
        QCOMPARE(controller->tablePanels().size(), 2);
        const auto first = controller->tablePanels().first().toMap();
        QPointer<MarkdownTableModel> oldModel = qobject_cast<MarkdownTableModel *>(first["model"].value<QObject *>()); QVERIFY(oldModel);
        controller->setReadMode(true); flush(); QCOMPARE(controller->tablePanels().size(), 2);
        QCOMPARE(controller->m_readTablePositions.size(), 2);
        QSet<int> positions;
        for (const auto &entry : controller->tablePanels()) {
            const auto panel = entry.toMap(); const int id = panel["id"].toInt();
            const int position = controller->m_readTablePositions.value(id, -1);
            QVERIFY(position >= 0); QVERIFY(!positions.contains(position)); positions.insert(position);
            QCOMPARE(view->characterAt(position), QChar::ObjectReplacementCharacter);
            QCOMPARE(controller->imagePathAt(position), QString()); // Slots never masquerade as user images.
            auto *model = qobject_cast<MarkdownTableModel *>(panel["model"].value<QObject *>()); QVERIFY(model);
            QCOMPARE(model->data(model->index(1, 0), MarkdownTableModel::MarkdownRole).toString(), QString("one"));
            QVERIFY(!model->setCell(1, 0, "rejected"));
        }
        for (auto *frame : view->rootFrame()->childFrames()) QVERIFY(!qobject_cast<QTextTable *>(frame));
        QCOMPARE(source->toPlainText(), text);
        viewItem->setProperty("width", 300); controller->refresh(); flush(); QCOMPARE(controller->tablePanels().size(), 2);
        controller->setReadMode(false); flush();
        oldModel->setProperty("drafts", QVariantMap{{"1:0", QVariantMap{{"text", "recover me"}, {"original", "one"}}}});
        controller->resetTableSessions();
        QVERIFY(!oldModel->setCell(1, 0, "must not edit a new document")); // Still alive until deleteLater, but session already revoked.
        flush(); QCOMPARE(source->toPlainText(), text); QCOMPARE(controller->tablePanels().size(), 2);
        QVERIFY(controller->tablePanels().first().toMap()["id"] != first["id"]);
        QVERIFY(controller->tableDraftsPending()); QVERIFY(controller->copyTableDrafts(0));
        QVERIFY(QGuiApplication::clipboard()->text().contains("recover me")); controller->discardTableDrafts();
        QVERIFY(!controller->tableDraftsPending());
    }
    void codeOverflowKeepsNativeSelectionAndRestoresProseWrap() {
        const QString longLine = QString("long_identifier ").repeated(100);
        const QString text = "before\n\n```cpp\n" + longLine + "\n```\n\nafter";
        source->setPlainText(text); select(sourceItem, 0, 0);
        auto panel = controller->codeBlockPanels().first().toMap();
        QVERIFY(panel["contentWidth"].toReal() > panel["viewportWidth"].toReal());
        auto first = view->findBlockByNumber(panel["firstBlock"].toInt());
        auto body = first.next(); QCOMPARE(body.layout()->lineCount(), 1);
        controller->scrollBlock(panel["id"].toInt(), 150); flush();
        QCOMPARE(controller->blockOffsets().value(QString::number(panel["id"].toInt())).toReal(), qreal(150));
        QVERIFY(body.blockFormat().textIndent() < 0); QCOMPARE(source->toPlainText(), text);
        select(viewItem, body.position(), body.position());
        const auto native = viewItem->property("cursorRectangle").toRectF();
        QVERIFY(controller->visualCursorRectangle(native).x() < -100); // Not clamped to the left edge.
        QInputMethodEvent preedit(QString::fromUtf8("かな"), {}); QCoreApplication::sendEvent(viewItem, &preedit); flush();
        QVERIFY(controller->m_composing);
        const auto composingRect = viewItem->property("cursorRectangle").toRectF();
        QCOMPARE(controller->visualCursorRectangle(composingRect), composingRect); // Includes native IME cursor/preedit.
        QInputMethodEvent cancel; QCoreApplication::sendEvent(viewItem, &cancel); flush();
        QSignalSpy redraw(view->documentLayout(), &QAbstractTextDocumentLayout::update);
        controller->scrollBlock(panel["id"].toInt(), 100); flush(); QVERIFY(redraw.size() > 0);
        select(viewItem, body.position() + 5, body.position() + 15);
        QCOMPARE(viewItem->property("selectedText").toString(), longLine.mid(5, 10));
        for (bool read : {true, false}) {
            controller->setReadMode(read); flush(); panel = controller->codeBlockPanels().first().toMap();
            controller->scrollBlock(panel["id"].toInt(), 150); flush();
            viewItem->setProperty("width", 30000); controller->refresh(); flush();
            QCOMPARE(controller->blockOffsets().value(QString::number(panel["id"].toInt())).toReal(), qreal(0));
            viewItem->setProperty("width", 550); controller->refresh(); flush();
        }
        source->setPlainText("ordinary " + longLine); flush();
        QVERIFY(!view->begin().blockFormat().nonBreakableLines());
        QVERIFY(!view->begin().blockFormat().hasProperty(QTextFormat::UserProperty + 120));
    }
    void escapesAndBacktickRunsSurviveModeSwitches() {
        const QString text = QStringLiteral(R"(\*literal\* and `` C-x ` ``

\# not a heading)");
        source->setPlainText(text); source->clearUndoRedoStacks(); flush();
        for (int n = 0; n < 3; ++n) {
            controller->setReadMode(true); flush();
            QCOMPARE(view->toPlainText(), QStringLiteral("*literal* and C-x `\n# not a heading"));
            controller->setReadMode(false); flush();
            QCOMPARE(view->toPlainText(), text);
            QCOMPARE(source->toPlainText(), text);
            QVERIFY(!source->isUndoAvailable());
        }
        QTextCursor change(source); change.movePosition(QTextCursor::End);
        change.insertText(QStringLiteral(" changed")); flush();
        source->undo(); flush();
        QCOMPARE(source->toPlainText(), text);
        QVERIFY(!source->isUndoAvailable());
    }
    void modeSwitchesRetainEditBlocksHighlighterAndCachedRead() {
        const QString text = QString("Ordinary paragraph with **strong** text.\n\n").repeated(800)
            + "```cpp\nint answer = 42;\n```\n\nlast";
        source->setPlainText(text); select(sourceItem, text.size(), text.size());
        auto *edit = view;
        const QTextBlock stable = view->findBlockByNumber(600);
        const auto resets = controller->m_fullResets;
        controller->setReadMode(true); flush();
        auto *read = view; QVERIFY(read != edit);
        QCOMPARE(controller->m_highlighter->document(), edit);
        const int readRevision = read->revision();
        for (int n = 0; n < 4; ++n) {
            controller->setReadMode(false); flush();
            QCOMPARE(view, edit); QCOMPARE(view->findBlockByNumber(600), stable);
            QCOMPARE(controller->m_fullResets, resets);
            QCOMPARE(view->toPlainText(), text);
            controller->setReadMode(true); flush();
            QCOMPARE(view, read); QCOMPARE(read->revision(), readRevision);
        }
        QTextCursor change(source); change.movePosition(QTextCursor::End); change.insertText(" changed"); flush();
        controller->setReadMode(false); flush();
        QCOMPARE(view, edit); QCOMPARE(view->toPlainText(), text + " changed");
        QCOMPARE(controller->m_fullResets, resets);
        source->undo(); flush(); QCOMPARE(view->toPlainText(), text);
    }
    void retainedEditImagesRefreshAfterReadChanges() {
        QTemporaryDir directory;
        QImage image(600, 200, QImage::Format_RGB32); image.fill(Qt::red);
        QVERIFY(image.save(directory.filePath("image.png")));
        ImageResolver resolver; resolver.root = directory.path(); controller->setResolver(&resolver);
        const QString text = "first\n\n![a](image.png)\n\nlast";
        source->setPlainText(text); select(sourceItem, text.size(), text.size());
        auto pixelAndWidth = [&] {
            const int at = view->toPlainText().indexOf(QChar::ObjectReplacementCharacter);
            QTextCursor cursor(view); cursor.setPosition(at);
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            const auto format = cursor.charFormat().toImageFormat();
            const QImage native = qvariant_cast<QImage>(view->resource(QTextDocument::ImageResource, QUrl(format.name())));
            return qMakePair(native.pixelColor(0, 0), format.width());
        };
        QCOMPARE(pixelAndWidth().first, QColor(Qt::red));
        controller->setReadMode(true); flush();
        viewItem->setProperty("width", 250);
        image.fill(Qt::green); QVERIFY(image.save(directory.filePath("image.png")));
        controller->refresh(); flush(); controller->setReadMode(false); flush();
        QCOMPARE(pixelAndWidth().first, QColor(Qt::green));
        QVERIFY(pixelAndWidth().second <= 250);
        QCOMPARE(source->toPlainText(), text);
    }
    void autoPairsUseOneCanonicalUndoAndRespectImeAndWords() {
        auto reset = [&](const QString &text) {
            source->setPlainText(text); select(sourceItem, text.size(), text.size()); source->clearUndoRedoStacks();
        };
        reset("plain"); type("["); QCOMPARE(source->toPlainText(), QString("plain[]"));
        type("{"); QCOMPARE(source->toPlainText(), QString("plain[{}]"));
        QKeyEvent back(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QCoreApplication::sendEvent(viewItem, &back); flush();
        QCOMPARE(source->toPlainText(), QString("plain[]"));
        source->undo(); flush(); QCOMPARE(source->toPlainText(), QString("plain[{}]"));
        select(sourceItem, 7, 7);
        type("}"); QCOMPARE(source->toPlainText(), QString("plain[{}]"));
        QCOMPARE(sourceItem->property("cursorPosition").toInt(), 8);
        reset("hello"); select(sourceItem, 0, 5); type("\"");
        QCOMPARE(source->toPlainText(), QString("\"hello\""));
        QCOMPARE(sourceItem->property("selectedText").toString(), QString("hello"));
        source->undo(); flush(); QCOMPARE(source->toPlainText(), QString("hello")); QVERIFY(!source->isUndoAvailable());
        reset("don"); type("'"); QCOMPARE(source->toPlainText(), QString("don'"));
        reset("escaped \\"); type("\""); QCOMPARE(source->toPlainText(), QString("escaped \\\""));
        reset("");
        QInputMethodEvent preedit("x", {{QInputMethodEvent::Cursor, 0, 0, {}}});
        QCoreApplication::sendEvent(viewItem, &preedit); flush(); QVERIFY(!controller->imeCursorVisible());
        QInputMethodEvent commit; commit.setCommitString("[");
        QCoreApplication::sendEvent(viewItem, &commit); flush();
        QVERIFY(controller->imeCursorVisible()); QCOMPARE(source->toPlainText(), QString("["));
        QCoreApplication::sendEvent(viewItem, &preedit); flush(); QVERIFY(!controller->imeCursorVisible());
        controller->setReadMode(true); flush();
        QVERIFY(!controller->m_composing); QVERIFY(controller->imeCursorVisible());
        controller->setReadMode(false); flush();
        QVERIFY(view->begin().layout()->preeditAreaText().isEmpty());
        QCOMPARE(source->toPlainText(), QString("["));
    }
    void nativeViewportFlagTracksDirectDocumentEdits() {
        auto *item = qobject_cast<QQuickItem *>(viewItem); QVERIFY(item);
        const QString text = QString("ordinary words\n").repeated(800).left(10000);
        source->setPlainText(text); select(sourceItem, text.size(), text.size());
        QVERIFY(!item->flags().testFlag(QQuickItem::ItemObservesViewport));
        type("x");
        QVERIFY(item->flags().testFlag(QQuickItem::ItemObservesViewport));
        QCOMPARE(view->toPlainText(), text + "x");
        controller->setReadMode(true); flush();
        QVERIFY(item->flags().testFlag(QQuickItem::ItemObservesViewport));
        controller->setReadMode(false); flush();
        source->setPlainText("small again"); flush();
        QVERIFY(!item->flags().testFlag(QQuickItem::ItemObservesViewport));
    }
    void tableBoundaryTypingConvergesAndRawWidthsStayExact() {
        QString text = "distant prose\n\nadjacent prose\n\n| A | B |\n| --- | --- |\n";
        for (int r = 0; r < 1200; ++r) text += QString("| row%1 | value |\n").arg(r);
        text += "| widest | " + QString(300, 'W') + " |\n\nafter";
        source->setPlainText(text); select(sourceItem, text.indexOf("| A") - 1, text.indexOf("| A") - 1); flush();
        const auto patches = controller->m_projectionPatches, resets = controller->m_fullResets;
        auto *model = controller->m_tables.first().model.data(); QVERIFY(model);
        const auto parses = model->m_parseCount;
        type("abc");
        QCOMPARE(controller->m_projectionPatches, patches); QCOMPARE(controller->m_fullResets, resets);
        QVERIFY(controller->m_blockParser.parsedLines() < 10); QCOMPARE(model->m_parseCount, parses);
        QVERIFY(!controller->m_tables.first().source);
        const int id = controller->m_tables.first().id; controller->setTableSource(id, true); flush();
        QVERIFY(controller->m_tables.first().source); QVERIFY(!controller->codeBlockPanels().isEmpty());
        auto panelWidth = [&] { return controller->codeBlockPanels().first().toMap()["contentWidth"].toReal(); };
        auto exhaustive = [&] {
            qreal maximum = viewItem->property("width").toReal() - 8;
            const auto group = controller->m_editCodeGroups.first();
            for (auto b = view->findBlockByNumber(group.first); b.isValid() && b.blockNumber() <= group.second; b = b.next()) {
                view->documentLayout()->blockBoundingRect(b);
                if (b.layout()) for (int i = 0; i < b.layout()->lineCount(); ++i) maximum = qMax(maximum, b.layout()->lineAt(i).naturalTextWidth() + 8);
            }
            return maximum;
        };
        QCOMPARE(panelWidth(), exhaustive());
        const auto visits = controller->m_overflowStyleVisits, measurements = controller->m_overflowMeasurements;
        const int row = source->toPlainText().indexOf("row10"); select(sourceItem, row, row); type("x");
        QCOMPARE(controller->m_overflowStyleVisits, visits);
        QVERIFY(controller->m_overflowMeasurements - measurements <= 3); QCOMPARE(panelWidth(), exhaustive());
        const qreal wide = panelWidth();
        const int widest = source->toPlainText().indexOf(QString(300, 'W'));
        select(sourceItem, widest, widest + 300); type("narrow");
        QVERIFY(panelWidth() < wide); QCOMPARE(panelWidth(), exhaustive());
        source->undo(); flush(); QCOMPARE(panelWidth(), exhaustive());
        // Newline joins/splits must normalize inherited no-wrap properties and
        // rebuild row identities, not reuse stale rows after an interior splice.
        const int split = source->toPlainText().indexOf("row100"); select(sourceItem, split, split); type("\n");
        PreviewBlocks::Parser fresh; fresh.reset(source->toPlainText());
        QCOMPARE(controller->m_blockParser.blocks().size(), fresh.blocks().size());
        source->undo(); flush();
        controller->setReadMode(true); flush(); controller->setReadMode(false); flush();
        QVERIFY(!controller->m_tables.isEmpty()); controller->setTableSource(controller->m_tables.first().id, true); flush();
        QCOMPARE(panelWidth(), exhaustive());
    }
    void incrementalEditsKeepNativeBlocksAndConverge() {
        QString text;
        for (int n = 0; n < 2000; ++n) text += QString("Paragraph %1 with words.\n\n").arg(n);
        text += "```cpp\n" + QString("int value = 1;\n").repeated(1200) + "```\n\nlast\n";
        source->setPlainText(text); select(sourceItem, 4, 4); source->clearUndoRedoStacks();
        const auto preserved = view->findBlockByNumber(3000);
        const auto resets = controller->m_fullResets;
        const auto patches = controller->m_projectionPatches;
        type("abc");
        QCOMPARE(controller->m_fullResets, resets); QCOMPARE(controller->m_projectionPatches, patches);
        QCOMPARE(preserved, view->findBlockByNumber(3000));
        QVERIFY(controller->m_blockParser.parsedLines() < 10);
        QVERIFY(controller->m_listParser.parsedLines < 10);
        QCOMPARE(view->toPlainText(), source->toPlainText());
        source->undo(); flush(); QCOMPARE(source->toPlainText(), text); QCOMPARE(view->toPlainText(), text);
        QCOMPARE(controller->m_fullResets, resets);
        const auto patchesAfterUndo = controller->m_projectionPatches;
        const int code = text.indexOf("value"); select(sourceItem, code, code); type("x");
        QCOMPARE(controller->m_blockParser.parsedLines(), 1);
        QVERIFY(controller->m_listParser.parsedLines < 10);
        QCOMPARE(controller->m_fullResets, resets); QCOMPARE(controller->m_projectionPatches, patchesAfterUndo);
        QCOMPARE(view->toPlainText(), source->toPlainText());
    }
    void incrementalListsMatchFreshParsingAndViewportIsBounded() {
        QString text;
        for (int n = 0; n < 1000; ++n) text += QString("1. item %1\n   continuation\n").arg(n);
        text += "\nlast\n";
        source->setPlainText(text); select(sourceItem, 4, 4);
        const QStringList edits = {"x", "\n- nested\n", "\n\n", "```\n", "<!--\n", "-->\n", "1. "};
        for (int n = 0; n < edits.size(); ++n) {
            const int at = qMin(4 + n * 19, int(source->toPlainText().size()));
            select(sourceItem, at, at); type(edits[n]);
            const auto fresh = ListPreview::parse(view->toPlainText());
            QCOMPARE(controller->m_listParser.items.size(), fresh.size());
            for (int i = 0; i < fresh.size(); ++i) {
                const auto a = controller->m_listParser.items[i], b = fresh[i];
                QCOMPARE(a.block, b.block); QCOMPARE(a.prefix, b.prefix); QCOMPARE(a.depth, b.depth); QCOMPARE(a.marker, b.marker);
            }
        }
        source->setPlainText(text); select(sourceItem, 4, 4);
        controller->setViewport(0, 300); flush();
        QVERIFY(controller->listMarkers().size() < 30);
        QVERIFY(!controller->listMarkers().isEmpty());
        const auto first = controller->listMarkers().first().toMap().value("start").toInt();
        controller->setViewport(view->documentLayout()->documentSize().height() - 600, 300); flush();
        QVERIFY(controller->listMarkers().size() < 30);
        QVERIFY(!controller->listMarkers().isEmpty());
        QVERIFY(controller->listMarkers().first().toMap().value("start").toInt() > first);
    }
    void quotePreviewRevealsWholeParagraphWithoutChangingGeometryOrUndo() {
        viewItem->setProperty("width", 270);
        const QString text = "😀 before\n\n| A | B |\n| --- | --- |\n| one | two |\n\n> quote "
            + QString("words ").repeated(25) + "\n> > nested body\n\nafter\n";
        source->setPlainText(text); source->clearUndoRedoStacks();
        select(sourceItem, text.size(), text.size());
        const int at = text.indexOf("> quote");
        auto block = view->findBlock(controller->viewPosition(at));
        QVERIFY(block.text().startsWith("> quote"));
        QCOMPARE(block.blockFormat().intProperty(MarkdownHighlighter::QuotePrefixProperty), 2);
        QCOMPARE(block.blockFormat().alignment(), Qt::Alignment(Qt::AlignJustify));
        QCOMPARE(controller->listMarkers().size(), 0); // No border/replacement marker.
        view->documentLayout()->documentSize();
        QVERIFY(block.layout()->lineCount() > 1);
        // Justification may stretch prefix spaces depending on font metrics.
        // Verify the hanging indent separately from that expanded advance.
        QVERIFY(qAbs(block.layout()->lineAt(1).x() - block.layout()->lineAt(0).x()
                     + block.blockFormat().textIndent()) < 1);
        const auto geometry = [&] {
            auto *layout = block.layout();
            QList<QPointF> result{layout->position()};
            for (int n = 0; n < layout->lineCount(); ++n) {
                const auto line = layout->lineAt(n);
                result.append(line.position());
                for (int pos = line.textStart(); pos <= line.textStart() + line.textLength(); ++pos)
                    result.append(QPointF(line.cursorToX(pos), line.y()));
            }
            return result;
        };
        const auto hiddenGeometry = geometry();
        const auto prefixColor = [&] {
            for (const auto &range : block.layout()->formats()) if (range.start == 0) return range.format.foreground().color();
            return QColor();
        };
        QCOMPARE(prefixColor().alpha(), 0);
        const auto resets = controller->m_fullResets, patches = controller->m_projectionPatches;
        select(sourceItem, at + 8, at + 8); // Anywhere in the body reveals >.
        QVERIFY(prefixColor().alpha() > 0); QCOMPARE(geometry(), hiddenGeometry);
        select(sourceItem, text.size(), text.size()); QCOMPARE(prefixColor().alpha(), 0);
        QCOMPARE(geometry(), hiddenGeometry);
        select(sourceItem, at + 9, at + 4); QVERIFY(prefixColor().alpha() > 0);
        QCOMPARE(geometry(), hiddenGeometry);
        select(sourceItem, text.size(), text.size());
        controller->setSearchHighlight("> q", at); flush(); QVERIFY(prefixColor().alpha() > 0);
        QCOMPARE(geometry(), hiddenGeometry);
        controller->setSearchHighlight("", -1); flush(); QCOMPARE(prefixColor().alpha(), 0);
        QCOMPARE(geometry(), hiddenGeometry);
        QCOMPARE(controller->m_fullResets, resets); QCOMPARE(controller->m_projectionPatches, patches);
        QCOMPARE(source->toPlainText(), text); QVERIFY(!source->isUndoAvailable());
        select(sourceItem, at + 8, at + 8); type("x");
        QString edited = text; edited.insert(at + 8, 'x'); QCOMPARE(source->toPlainText(), edited);
        source->undo(); flush(); QCOMPARE(source->toPlainText(), text);
        // Removing the revealed > restores ordinary alignment; one source undo restores quote styling.
        QTextCursor remove(view); remove.setPosition(controller->viewPosition(at));
        remove.setPosition(controller->viewPosition(at + 1), QTextCursor::KeepAnchor); remove.removeSelectedText(); flush();
        QCOMPARE(source->toPlainText(), QString(text).remove(at, 1));
        QCOMPARE(view->findBlock(controller->viewPosition(at)).blockFormat().alignment(), Qt::Alignment(Qt::AlignLeft));
        source->undo(); flush(); QCOMPARE(source->toPlainText(), text);
        QCOMPARE(view->findBlock(controller->viewPosition(at)).blockFormat().alignment(), Qt::Alignment(Qt::AlignJustify));
        const auto nested = view->findBlock(controller->viewPosition(text.indexOf("> > nested")));
        QCOMPARE(nested.blockFormat().intProperty(MarkdownHighlighter::QuotePrefixProperty), 4);
        QVERIFY(nested.blockFormat().leftMargin() > block.blockFormat().leftMargin());
    }
    void readUsesStandardMarkdownLineBreaks_data() {
        QTest::addColumn<QString>("markdown");
        QTest::addColumn<QString>("expectedRaw");
        const QString hard = QString(QChar::LineSeparator);
        const QString paragraph = QString(QChar::ParagraphSeparator);
        QTest::newRow("soft") << QString("first\nsecond") << QString("first second");
        QTest::newRow("backslash") << QString("first\\\nsecond") << QString("first" + hard + "second");
        QTest::newRow("two-spaces") << QString("first  \nsecond") << QString("first" + hard + "second");
        QTest::newRow("paragraph") << QString("first\n\nsecond") << QString("first" + paragraph + "second");
        QTest::newRow("indented-continuation") << QString("first\\\n   second") << QString("first" + hard + "second");
        QTest::newRow("consecutive-hard-breaks") << QString("first\\\nsecond  \nthird") << QString("first" + hard + "second" + hard + "third");
        QTest::newRow("one-space-is-soft") << QString("first \nsecond") << QString("first second");
        QTest::newRow("hard-break-before-blank") << QString("first\\\n\nsecond") << QString("first\\" + paragraph + "second");
        QTest::newRow("quote-soft") << QString("> first\n> second") << QString("first second");
        QTest::newRow("quote-backslash") << QString("> first\\\n> second") << QString("first" + hard + "second");
        QTest::newRow("quote-two-spaces") << QString("> first  \n> second") << QString("first" + hard + "second");
        QTest::newRow("quote-paragraph") << QString("> first\n>\n> second") << QString("first" + paragraph + "second");
        QTest::newRow("escaped-backslash") << QString("first\\\\\nsecond") << QString("first\\ second");
        QTest::newRow("inline-code") << QString("``first\nsecond``") << QString("first second");
        QTest::newRow("fenced-code") << QString("```text\nfirst\nsecond\n```") << QString("first" + paragraph + "second");
        QTest::newRow("literal-break-in-code") << QString("```text\nfirst\\\nsecond  \nthird\n```") << QString("first\\" + paragraph + "second  " + paragraph + "third");
        QTest::newRow("nested-quote") << QString("> > first\\\n> > second") << QString("first" + hard + "second");
        QTest::newRow("reference-link-after-break") << QString("first\\\n[second][id]\n\n[id]: https://example.invalid") << QString("first" + hard + "second");
        QTest::newRow("reference-link-before-break") << QString("[first][id]\\\nsecond\n\n[id]: https://example.invalid") << QString("first" + hard + "second");
        QTest::newRow("reference-link-before-space-break") << QString("[first][id]  \nsecond\n\n[id]: https://example.invalid") << QString("first" + hard + "second");
        QTest::newRow("multiline-code-after-break") << QString("first\\\n`second\nthird`") << QString("first" + hard + "second third");
    }
    void readUsesStandardMarkdownLineBreaks() {
        QFETCH(QString, markdown);
        QFETCH(QString, expectedRaw);
        source->setPlainText(markdown); source->clearUndoRedoStacks(); flush();
        for (int n = 0; n < 2; ++n) {
            controller->setReadMode(true); flush();
            QCOMPARE(view->toRawText(), expectedRaw);
            QCOMPARE(view->blockCount(), expectedRaw.count(QChar::ParagraphSeparator) + 1);
            if (expectedRaw.contains(QChar::LineSeparator)) {
                view->documentLayout()->documentSize();
                QVERIFY(view->begin().layout()->lineCount() >= 2);
            }
            if (QTest::currentDataTag() == QByteArrayLiteral("paragraph"))
                QVERIFY(view->begin().blockFormat().bottomMargin() > 0);
            controller->setReadMode(false); flush();
            QCOMPARE(view->toPlainText(), markdown);
            QCOMPARE(source->toPlainText(), markdown);
            QVERIFY(!source->isUndoAvailable());
        }
    }
    void readQuoteBreaksAndJustificationSurviveHtmlRoundTrip() {
        const QString text = "ordinary first\nordinary second\n\n> quote first\n> quote second\n>\n> next paragraph\n> last line  \n> existing break\n\nafter\n";
        source->setPlainText(text); source->clearUndoRedoStacks(); select(sourceItem, text.size(), text.size());
        controller->setReadMode(true); flush();
        const QString raw = view->toRawText();
        QVERIFY2(raw.contains(QString("quote first quote second")), qPrintable(raw));
        QVERIFY(raw.contains("ordinary first ordinary second"));
        QVERIFY(raw.contains(QString("next paragraph last line") + QChar::LineSeparator + "existing break"));
        int quotes = 0;
        for (auto block = view->begin(); block.isValid(); block = block.next()) {
            if (block.text().startsWith("quote first") || block.text().startsWith("next paragraph")) {
                ++quotes; QCOMPARE(block.blockFormat().alignment(), Qt::Alignment(Qt::AlignJustify));
            } else QVERIFY(block.blockFormat().alignment() != Qt::AlignJustify);
        }
        QCOMPARE(quotes, 2); QCOMPARE(source->toPlainText(), text); QVERIFY(!source->isUndoAvailable());
        controller->setReadMode(false); flush(); QCOMPARE(view->toPlainText(), text);
        // Angle autolinks and literal comparisons are prose, not HTML blocks.
        source->setPlainText("> first\n> one < two\n> visit <https://example.invalid>\n> last\n"); flush();
        controller->setReadMode(true); flush();
        QVERIFY(view->toRawText().contains(QString("first one < two visit https://example.invalid last")));
        bool anchor = false;
        for (auto block = view->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it) anchor |= it.fragment().charFormat().anchorHref() == "https://example.invalid";
        QVERIFY(anchor); QVERIFY(!view->toPlainText().contains("PWHARDBREAK"));
    }
    void quoteClassificationAndReadPreparationStayConservative() {
        const QString prose = "> first\n> second\n> > nested first\n> > nested second\n";
        QCOMPARE(ListPreview::readHardBreaks(prose), prose);
        const QStringList untouched = {
            "```text\n> literal\n> also literal\n```\n",
            "    > indented\n    > literal\n",
            "<!--\n> literal\n> also literal\n-->\n",
            "<div>\n> literal\n> also literal\n</div>\n",
            "> ```text\n> > literal\n> still code\n> ```\n",
            "> <pre>\n> > literal\n> still literal\n> </pre>\n",
            "> <!--\n> > literal\n> still hidden\n> -->\n",
            "> a | b\n> --- | ---\n> x | y\n> z | q\n",
            "> - item\n> continuation\n> still item\n",
            "> heading\n> ===\n",
            "> heading\n> ---\n",
            "> inline `first\n> second` last\n> after\n",
            "> [ref]: https://example.invalid\n> continuation\n",
            "> [label](\n> https://example.invalid)\n",
            "> ![image](\n> image.png)\n",
            "> [label](https://example.invalid\n> \"title )\n> still title\")\n",
            "> [multi\n> line](https://example.invalid)\n",
            "\\> escaped\n\\> literal\n"
        };
        for (const auto &text : untouched) {
            QCOMPARE(ListPreview::readHardBreaks(text), text);
            QStringList markers;
            const auto prepared = ListPreview::readHardBreaks(text, &markers);
            QTextDocument original, restored; original.setMarkdown(text); restored.setMarkdown(prepared);
            QVERIFY(ListPreview::restoreReadHardBreaks(restored, markers));
            QCOMPARE(restored.toHtml(), original.toHtml()); // Includes multiline link/image identity.
        }
        source->setPlainText(prose + "\nafter\n"); flush();
        const QStringList edits = {"x", "\n> next\n", "\n", "```\n", "> > "};
        for (const auto &edit : edits) {
            select(sourceItem, 3, 3); type(edit);
            const auto fresh = ListPreview::parse(view->toPlainText());
            QCOMPARE(controller->m_listParser.items.size(), fresh.size());
            for (int i = 0; i < fresh.size(); ++i) {
                const auto a = controller->m_listParser.items[i], b = fresh[i];
                QCOMPARE(a.block, b.block); QCOMPARE(a.prefix, b.prefix); QCOMPARE(a.depth, b.depth);
                QCOMPARE(a.quote, b.quote); QCOMPARE(a.soft, b.soft);
            }
        }
    }
    void listMarkersKeepHangingAlignmentAndSource() {
        viewItem->setProperty("width", 260);
        const QString text = "- alpha " + QString("words ").repeated(18)
            + "\n  - nested beta\n- gamma\n\n8. eight\n1. nine\n1. ten\n\nafter\n";
        source->setPlainText(text); source->clearUndoRedoStacks();
        select(sourceItem, text.size(), text.size());
        QCOMPARE(view->toPlainText(), text);
        QCOMPARE(controller->listMarkers().size(), 6);
        QCOMPARE(controller->listMarkers()[4].toMap().value("text").toString(), QString("9."));
        QCOMPARE(controller->listMarkers()[5].toMap().value("text").toString(), QString("10."));
        const auto bodyX = [&] { auto *layout = view->begin().layout(); return layout->position().x() + layout->lineAt(0).cursorToX(2); };
        view->documentLayout()->documentSize();
        const qreal x = bodyX();
        QVERIFY(view->begin().layout()->lineCount() > 1);
        const auto *layout = view->begin().layout();
        QVERIFY(qAbs(x - layout->position().x() - layout->lineAt(1).x()) < 1);
        const auto prefixColor = [&] {
            for (const auto &range : view->begin().layout()->formats())
                if (range.start == 0) return range.format.foreground().color();
            return QColor();
        };
        QCOMPARE(prefixColor().alpha(), 0);
        const auto markers = controller->listMarkers();
        select(sourceItem, 0, 0);
        QCOMPARE(controller->listMarkers(), markers); // Caret visibility never resets all delegates.
        QVERIFY(prefixColor().alpha() > 0);
        QCOMPARE(bodyX(), x);
        select(sourceItem, 5, 5);
        QCOMPARE(prefixColor().alpha(), 0);
        QCOMPARE(bodyX(), x);
        controller->setSearchHighlight("- a", 0); flush(); // Match crosses the prefix/body boundary.
        QVERIFY(prefixColor().alpha() > 0);
        QVERIFY(controller->listMarkers().first().toMap().value("searchReveal").toBool());
        controller->setSearchHighlight("", -1); flush();
        QCOMPARE(prefixColor().alpha(), 0);
        select(sourceItem, 4, 0); // Selection reveals literal prefixes, in either direction.
        QVERIFY(prefixColor().alpha() > 0);
        QCOMPARE(source->toPlainText(), text);
        QVERIFY(!source->isUndoAvailable());
        controller->setReadMode(true); flush();
        QVERIFY(controller->listMarkers().isEmpty());
        layout = view->begin().layout();
        QVERIFY(qAbs(layout->position().x() + layout->lineAt(0).cursorToX(0) - x) < 1);
        controller->setReadMode(false); flush();
        QCOMPARE(view->toPlainText(), text);
        QCOMPARE(bodyX(), x);
        const auto excluded = ListPreview::parse("    - code\n\n```\n- code\n```\n\n<!--\n- hidden\n-->\n\n<div>\n- hidden\n</div>\n\na | b\n--- | ---\n- literal | row\n");
        QVERIFY(excluded.isEmpty());
        QVERIFY(ListPreview::parse("Paragraph\n2. literal continuation\n").isEmpty());
        QVERIFY(ListPreview::parse("<details>\n- literal\n</details>\n").isEmpty());
        QVERIFY(ListPreview::parse("<div>\n</div>\n- literal\n\n").isEmpty());
        const auto dedented = ListPreview::parse("- outer\n  - inner\n\n  outer continuation\n");
        QCOMPARE(dedented.last().depth, 1);
        const auto separate = ListPreview::parse("8. first\n1) second\n");
        QCOMPARE(separate.size(), 2);
        QCOMPARE(separate.last().marker, QString("1)"));
        const auto siblings = ListPreview::parse("- outer\n - sibling\n  - sibling\n");
        QCOMPARE(siblings.size(), 3);
        for (const auto &item : siblings) QCOMPARE(item.depth, 1);
        const auto outsideFence = ListPreview::parse("8. old\n\n```\ncode\n```\n\n1. new\n");
        QCOMPARE(outsideFence.last().marker, QString("1."));
        const auto nestedFence = ListPreview::parse("8. old\n\n   ```\n   - code\n   ```\n\n1. next\n");
        QCOMPARE(nestedFence.last().marker, QString("9."));
        const auto outerFence = ListPreview::parse("8. outer\n   - inner\n\n   ```\n   code\n   ```\n\n1. next\n");
        QCOMPARE(outerFence.last().marker, QString("9."));
        const auto afterRule = ListPreview::parse("Paragraph\n***\n2. item\n");
        QCOMPARE(afterRule.size(), 1); QCOMPARE(afterRule.first().marker, QString("2."));
        QVERIFY(ListPreview::parse("<custom>\n- literal\n</custom>\n\n").isEmpty());
        controller->setReadMode(false); flush();
        select(sourceItem, 0, 7);
        QInputMethodEvent preedit("漢", {});
        QCoreApplication::sendEvent(viewItem, &preedit); flush();
        QVERIFY(controller->listMarkers().isEmpty());
        QInputMethodEvent cancel;
        QCoreApplication::sendEvent(viewItem, &cancel); flush();
        // Native IME removes an initial selection even when preedit is cancelled.
        QCOMPARE(source->toPlainText(), text.mid(7));
        QVERIFY(source->isUndoAvailable()); source->undo(); flush();
        QCOMPARE(source->toPlainText(), text);
    }
    void listLayoutCachesCaretWorkAndPreservesPrefixUndo() {
        QString text;
        for (int i = 0; i < 1000; ++i) text += QString("- item %1 body words\n").arg(i);
        text += "\nafter";
        source->setPlainText(text); source->clearUndoRedoStacks();
        select(sourceItem, text.size(), text.size());
        QCOMPARE(controller->listMarkers().size(), 1000);
        QSignalSpy modelChanges(controller.get(), &LivePreview::listMarkersChanged);
        QElapsedTimer timer; timer.start();
        for (int i = 0; i < 32; ++i) select(sourceItem, 4 + i % 10, 4 + i % 10);
        QVERIFY2(timer.elapsed() < 5000, "Caret movement should not reparse/reformat the full list or recreate its QML delegates");
        QCOMPARE(modelChanges.size(), 0);
        QCOMPARE(source->toPlainText(), text); QVERIFY(!source->isUndoAvailable());
        select(viewItem, 0, 1); type("+");
        QCOMPARE(source->toPlainText(), "+" + text.mid(1));
        QVERIFY(source->isUndoAvailable()); source->undo(); flush();
        QCOMPARE(source->toPlainText(), text);
        QFont font = view->defaultFont(); font.setPixelSize(28);
        viewItem->setProperty("font", font); viewItem->setProperty("width", 280);
        controller->refresh(); flush();
        const auto *layout = view->begin().layout();
        const qreal body = layout->position().x() + layout->lineAt(0).cursorToX(2);
        QVERIFY(qAbs(body - view->documentMargin() - view->indentWidth()) < 1);
        QCOMPARE(source->toPlainText(), text); QVERIFY(!source->isUndoAvailable());
    }
    void inactiveHorizontalRulesRevealSourceAndKeepUndo() {
        const QString text = "before\n\n---\n\nafter\n\n```\n---\n```\n";
        source->setPlainText(text); source->clearUndoRedoStacks();
        select(sourceItem, text.size(), text.size());
        QCOMPARE(view->toPlainText().count(QChar::ObjectReplacementCharacter), 1);
        const int object = view->toPlainText().indexOf(QChar::ObjectReplacementCharacter);
        QTextCursor cursor(view); cursor.setPosition(object); cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        QVERIFY(cursor.charFormat().isImageFormat());
        QCOMPARE(cursor.charFormat().toImageFormat().width(), viewItem->property("width").toReal() - 8);
        select(viewItem, object, object); // Same source activation used by pointer/keyboard navigation.
        QVERIFY(!view->toPlainText().contains(QChar::ObjectReplacementCharacter));
        QVERIFY(view->toPlainText().contains("---"));
        select(sourceItem, text.size(), text.size());
        QCOMPARE(view->toPlainText().count(QChar::ObjectReplacementCharacter), 1);
        const int rule = text.indexOf("---");
        select(sourceItem, rule - 1, rule + 4);
        QVERIFY(!view->toPlainText().contains(QChar::ObjectReplacementCharacter));
        controller->setReadMode(true); flush();
        controller->setReadMode(false); flush();
        QCOMPARE(source->toPlainText(), text);
        QVERIFY(!source->isUndoAvailable());
    }
    void codePanelCopyUsesOnlyNativeBodiesAndCarriesLanguage() {
        const QString text = "```cpp extra\n  int value = 1;\n\n// 🙂\n```\n"
                             "```\nplain\n```\n~~~custom-tag\nunknown\n~~~~\n"
                             "```json\n{\"a\":1}\n```\n```json\n{\"b\":2}\n```\n";
        const QStringList bodies{"  int value = 1;\n\n// 🙂", "plain", "unknown", "{\"a\":1}", "{\"b\":2}"};
        const QStringList languages{"cpp", "text", "custom-tag", "json", "json"};
        source->setPlainText(text); source->clearUndoRedoStacks();
        select(sourceItem, text.size(), text.size());
        for (bool read : {false, true}) {
            controller->setReadMode(read); controller->refresh(); flush();
            QCOMPARE(controller->codeBlockPanels().size(), bodies.size());
            for (int i = 0; i < bodies.size(); ++i) {
                QCOMPARE(controller->codeBlockPanels()[i].toMap().value("language").toString(), languages[i]);
                QVERIFY(controller->copyCodeBlock(i));
                QCOMPARE(QGuiApplication::clipboard()->text(), bodies[i]);
            }
            QCOMPARE(source->toPlainText(), text);
            QVERIFY(!source->isUndoAvailable());
        }
        source->setPlainText("<!--\n```wrong\nphantom\n```\n-->\n\n```json\nactual\n```\n");
        controller->setReadMode(true); controller->refresh(); flush();
        QCOMPARE(controller->codeBlockPanels().size(), 1);
        QCOMPARE(controller->codeBlockPanels().first().toMap().value("language").toString(), QString("json"));
        QVERIFY(controller->copyCodeBlock(0));
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("actual")); // Never source-parser phantom code.
        source->setPlainText("```text\nunclosed\n"); flush();
        for (bool read : {false, true}) {
            controller->setReadMode(read); controller->refresh(); flush();
            QVERIFY(controller->copyCodeBlock(0));
            QCOMPARE(QGuiApplication::clipboard()->text(), read ? QString("unclosed") : QString("unclosed\n"));
        }
        controller->setReadMode(false); source->setPlainText("```\n```\n"); flush();
        QVERIFY(controller->copyCodeBlock(0));
        QCOMPARE(QGuiApplication::clipboard()->text(), QString());
        QGuiApplication::clipboard()->setText("unchanged");
        QVERIFY(!controller->copyCodeBlock(-1)); QVERIFY(!controller->copyCodeBlock(99));
        const auto page = view->pageSize(); view->setPageSize(QSizeF(0, 0));
        QVERIFY(!controller->copyCodeBlock(0));
        view->setPageSize(page); flush();
        QInputMethodEvent preedit(QString::fromUtf8("に"), {});
        QCoreApplication::sendEvent(viewItem, &preedit);
        QVERIFY(!controller->copyCodeBlock(0));
        QInputMethodEvent cancel(QString(), {}); QCoreApplication::sendEvent(viewItem, &cancel); flush();
        { // A stale presentation must not copy unrelated text even if source is unchanged.
            QSignalBlocker viewSignals(view);
            QTextCursor edit(view); edit.insertText("unexpected");
        }
        QVERIFY(!controller->copyCodeBlock(0));
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("unchanged"));
        controller->refresh(); flush();
        QTextCursor cursor(source); cursor.insertText("Before\n"); // Projection has not caught up yet.
        QVERIFY(!controller->copyCodeBlock(0));
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("unchanged"));
    }
    void manyCodePanelsBatchTheirSpacingUpdates() {
        QString text = "Before\n";
        for (int i = 0; i < 300; ++i) text += "```text\nvalue\n```\n";
        text += "After\n";
        source->setPlainText(text); source->clearUndoRedoStacks();
        select(sourceItem, text.size(), text.size()); controller->refresh(); flush();
        QCOMPARE(controller->codeBlockPanels().size(), 300);
        auto *layout = view->documentLayout();
        { // Invalidate presentation-only spacing, without measuring the fixture setup.
            QSignalBlocker layoutSignals(layout);
            QTextCursor batch(view); batch.beginEditBlock();
            for (auto block = view->begin(); block.isValid(); block = block.next()) {
                auto format = block.blockFormat(); format.setTopMargin(0); format.setBottomMargin(0);
                QTextCursor cursor(block); cursor.setBlockFormat(format);
            }
            batch.endEditBlock();
        }
        QSignalSpy updates(layout, &QAbstractTextDocumentLayout::update);
        layout->update(QRectF()); flush();
        QVERIFY(updates.count() > 0);
        QVERIFY2(updates.count() < 12, qPrintable(QString::number(updates.count())));
        QCOMPARE(controller->codeBlockPanels().size(), 300);
        const auto panels = controller->codeBlockPanels();
        updates.clear(); select(sourceItem, text.size() - 2, text.size() - 2); flush();
        QCOMPARE(controller->codeBlockPanels(), panels);
        QVERIFY2(updates.count() < 12, qPrintable(QString("caret refresh updates: %1").arg(updates.count())));
        QCOMPARE(source->toPlainText(), text);
        QVERIFY(!source->isUndoAvailable());
    }
    void paddedCodePanelsLeaveRoomForProseAndAdjacentBlocks() {
        const QString text = "Before\n```json\n{\"x\":true}\n```\n```text\nnext\n```\nAfter\n";
        source->setPlainText(text); source->clearUndoRedoStacks();
        select(sourceItem, text.size(), text.size());
        for (int pixels : {16, 28}) {
            QFont font = view->defaultFont(); font.setPixelSize(pixels);
            viewItem->setProperty("font", font);
            for (bool read : {false, true}) {
                controller->setReadMode(read); controller->refresh(); flush();
                const auto panels = controller->codeBlockPanels();
                QCOMPARE(panels.size(), 2);
                const auto a = panels[0].toMap(), b = panels[1].toMap();
                QVERIFY(a.value("y").toReal() + a.value("height").toReal() < b.value("y").toReal());
                QRectF before, after;
                QVERIFY(QMetaObject::invokeMethod(viewItem, "positionToRectangle", Q_RETURN_ARG(QRectF, before), Q_ARG(int, 0)));
                QVERIFY(QMetaObject::invokeMethod(viewItem, "positionToRectangle", Q_RETURN_ARG(QRectF, after), Q_ARG(int, view->find("After").position() - 1)));
                QVERIFY(before.bottom() < a.value("y").toReal());
                QVERIFY(b.value("y").toReal() + b.value("height").toReal() < after.top());
                const auto code = view->find("\"x\"").block();
                QRectF line;
                QVERIFY(QMetaObject::invokeMethod(viewItem, "positionToRectangle", Q_RETURN_ARG(QRectF, line), Q_ARG(int, code.position())));
                QVERIFY(line.top() - a.value("y").toReal() >= pixels / 2);
                QVERIFY(a.value("width").toReal() > viewItem->property("width").toReal() + 12);
                QCOMPARE(source->toPlainText(), text);
                QVERIFY(!source->isUndoAvailable());
            }
        }
        source->setPlainText("```text\nedge\n```");
        select(sourceItem, source->characterCount() - 1, source->characterCount() - 1);
        for (bool read : {false, true}) {
            controller->setReadMode(read); controller->refresh(); flush();
            const auto panel = controller->codeBlockPanels().first().toMap();
            QVERIFY(panel.value("y").toReal() >= 0);
            QVERIFY(panel.value("y").toReal() + panel.value("height").toReal()
                    <= viewItem->property("contentHeight").toReal());
        }
        source->setPlainText("Only prose"); flush();
        for (bool read : {false, true}) {
            controller->setReadMode(read); controller->refresh(); flush();
            QVERIFY(controller->codeBlockPanels().isEmpty());
            QCOMPARE(view->rootFrame()->frameFormat().topMargin(), view->documentMargin());
            QCOMPARE(view->rootFrame()->frameFormat().bottomMargin(), view->documentMargin());
        }
    }
    void codePanelsCoverNativeFontBoundsAcrossMarginsAndZoom() {
        const int id = QFontDatabase::addApplicationFont(QFINDTESTDATA("../../fonts/IBMPlexMono-Regular.ttf"));
        QVERIFY(id >= 0);
        const QString text = "Testing background\n\n```json\n{\"this\":\"that\", \"jgy\":true}\n```\n";
        source->setPlainText(text); source->clearUndoRedoStacks();
        select(sourceItem, text.size(), text.size());
        for (int pixels : {12, 20, 34}) {
            QFont font(QFontDatabase::applicationFontFamilies(id).first()); font.setPixelSize(pixels);
            viewItem->setProperty("font", font);
            for (bool read : {false, true}) {
                controller->setReadMode(read); controller->refresh(); flush();
                for (int margin : {0, 4, 9}) {
                    view->setDocumentMargin(margin); flush();
                    QCOMPARE(controller->codeBlockPanels().size(), 1);
                    const auto panel = controller->codeBlockPanels().first().toMap();
                    const auto block = view->find("\"this\"").block();
                    QVERIFY(block.isValid());
                    QRectF cursor;
                    QVERIFY(QMetaObject::invokeMethod(viewItem, "positionToRectangle", Q_RETURN_ARG(QRectF, cursor), Q_ARG(int, block.position())));
                    const qreal top = panel.value("y").toReal(), bottom = top + panel.value("height").toReal();
                    QVERIFY2(top <= cursor.top() && bottom >= cursor.bottom(),
                        qPrintable(QString("pixels=%1 read=%2 margin=%3 panel=%4..%5 cursor=%6..%7")
                            .arg(pixels).arg(read).arg(margin).arg(top).arg(bottom).arg(cursor.top()).arg(cursor.bottom())));
                    const auto *layout = block.layout();
                    const qreal origin = cursor.y() - layout->lineAt(0).y();
                    for (const auto &run : layout->glyphRuns()) {
                        const auto glyphs = run.glyphIndexes();
                        const auto positions = run.positions();
                        for (int i = 0; i < glyphs.size(); ++i) {
                            const auto ink = run.rawFont().boundingRect(glyphs.at(i)).translated(positions.at(i));
                            QVERIFY(top <= origin + ink.top());
                            QVERIFY(bottom >= origin + ink.bottom());
                        }
                    }
                }
            }
        }
        QCOMPARE(source->toPlainText(), text);
        QVERIFY(!source->isUndoAvailable());
    }
    void codePanelsCoverBlankAndWrappedLinesWithoutChangingSource() {
        const QString text = "before `inline`\n\n```cpp\nint value = 1;\n\n// " + QString(180, 'x')
            + "\n```\n```unknown-language\nplain\n```\n\nafter\n\n~~~\nunfinished\n";
        source->setPlainText(text);
        source->clearUndoRedoStacks();
        select(sourceItem, text.size(), text.size());
        for (bool read : {false, true, false}) {
            controller->setReadMode(read);
            controller->refresh(); flush();
            const auto panels = controller->codeBlockPanels();
            QCOMPARE(panels.size(), 3);
            for (const auto &panel : panels) {
                const auto r = panel.toMap();
                QVERIFY(r.value("width").toReal() >= viewItem->property("width").toReal());
                QVERIFY(r.value("height").toReal() > 0);
            }
            const auto first = panels.first().toMap();
            const auto blank = view->find("int value").block().next();
            QCOMPARE(blank.text(), QString());
            view->documentLayout()->blockBoundingRect(blank);
            const qreal blankY = blank.layout()->position().y();
            QVERIFY(first.value("y").toReal() <= blankY);
            QVERIFY(first.value("y").toReal() + first.value("height").toReal()
                    >= blankY + blank.layout()->lineAt(0).height());
            QVERIFY(first.value("height").toReal() > 4 * 20); // Includes blank + wrapped lines.
            viewItem->setProperty("width", 220); controller->refresh(); flush();
            QCOMPARE(controller->codeBlockPanels().first().toMap().value("height").toReal(), first.value("height").toReal());
            QVERIFY(controller->codeBlockPanels().first().toMap().value("contentWidth").toReal() > 220);
            viewItem->setProperty("width", 550); controller->refresh(); flush();
            QCOMPARE(source->toPlainText(), text);
            QVERIFY(!source->isUndoAvailable());
        }
        source->setPlainText("ordinary prose"); controller->refresh(); flush();
        QVERIFY(controller->codeBlockPanels().isEmpty());
    }
    void codePanelsDoNotDependOnTransientSyntaxStates() {
        const QString text = "before\n\n```cpp\nint value = 1;\n\n// code\n```\n\nafter\n";
        source->setPlainText(text); source->clearUndoRedoStacks();
        select(sourceItem, text.size(), text.size()); controller->refresh(); flush();
        QCOMPARE(controller->codeBlockPanels().size(), 1);
        const auto before = controller->codeBlockPanels();
        // Syntax state is an asynchronous implementation detail, not whether
        // the unchanged visible Markdown still contains a fenced block.
        for (auto block = view->begin(); block.isValid(); block = block.next())
            block.setUserState(-1);
        view->documentLayout()->update(QRectF()); flush();
        QCOMPARE(controller->codeBlockPanels(), before);
        const QSizeF page = view->pageSize();
        view->setPageSize(QSizeF(0, 0));
        view->documentLayout()->update(QRectF()); flush();
        QCOMPARE(view->findBlockByNumber(2).layout()->lineCount(), 0);
        QCOMPARE(controller->codeBlockPanels(), before); // Never publish partial geometry.
        view->setPageSize(page);
        view->markContentsDirty(0, view->characterCount());
        view->documentLayout()->update(QRectF()); flush();
        QCOMPARE(controller->codeBlockPanels(), before);
        QCOMPARE(source->toPlainText(), text);
        QVERIFY(!source->isUndoAvailable());
        source->setPlainText("plain prose"); flush();
        QVERIFY(controller->codeBlockPanels().isEmpty()); // Genuine removal still clears it.
    }
    void codePanelsFollowWrappedPreeditWithoutRebuilding() {
        const QString text = "```text\nx\n```\n\n```text\nsecond\n```\n";
        source->setPlainText(text); source->clearUndoRedoStacks();
        select(sourceItem, text.indexOf("\nx\n") + 2, text.indexOf("\nx\n") + 2);
        controller->refresh(); flush();
        QCOMPARE(controller->codeBlockPanels().size(), 2);
        const auto before = controller->codeBlockPanels();
        QSignalSpy rebuilds(controller.get(), &LivePreview::projectionChanged);
        QInputMethodEvent preedit(QString(200, 'a'), {});
        QCoreApplication::sendEvent(viewItem, &preedit); flush();
        QTRY_VERIFY(controller->codeBlockPanels().first().toMap().value("contentWidth").toReal()
                    > before.first().toMap().value("contentWidth").toReal());
        QCOMPARE(controller->codeBlockPanels().first().toMap().value("height").toReal(), before.first().toMap().value("height").toReal());
        QCOMPARE(controller->codeBlockPanels().last().toMap().value("y").toReal(), before.last().toMap().value("y").toReal());
        QCOMPARE(rebuilds.count(), 0);
        QCOMPARE(source->toPlainText(), text);
        QInputMethodEvent cancel(QString(), {});
        QCoreApplication::sendEvent(viewItem, &cancel); flush();
        QTRY_COMPARE(controller->codeBlockPanels(), before);
        QCOMPARE(source->toPlainText(), text);
        QVERIFY(!source->isUndoAvailable());
    }
    void readPanelsKeepContainerAndAdjacentFenceBoundaries() {
        const QString text = "- item\n\n    ```cpp\n    int nested = 1;\n    ```\n\n```py\nfirst = 1\n```\n```py\nsecond = 2\n```\n";
        source->setPlainText(text); select(sourceItem, text.size(), text.size());
        controller->setReadMode(true); controller->refresh(); flush();
        QCOMPARE(controller->codeBlockPanels().size(), 3);
        const QStringList bodies{"int nested = 1;", "first = 1", "second = 2"};
        const QStringList languages{"cpp", "py", "py"};
        for (int i = 0; i < bodies.size(); ++i) {
            QCOMPARE(controller->codeBlockPanels()[i].toMap().value("language").toString(), languages[i]);
            QVERIFY(controller->copyCodeBlock(i));
            QCOMPARE(QGuiApplication::clipboard()->text(), bodies[i]);
        }
        QCOMPARE(source->toPlainText(), text);
    }
    void richImageRestylesCoalesceLayoutNotifications() {
        QTemporaryDir directory;
        QImage image(160, 100, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::darkCyan);
        QVERIFY(image.save(directory.filePath("image.png")));
        ImageResolver resolver;
        resolver.root = directory.path();
        controller->setResolver(&resolver);
        QString text = QStringLiteral("![Picture](image.png)\n\n");
        for (int i = 0; i < 1000; ++i)
            text += QStringLiteral("# Heading %1\n\n").arg(i);
        text += QStringLiteral("After.\n");
        source->setPlainText(text);
        select(sourceItem, text.size(), text.size());
        controller->refresh();
        flush();
        auto *highlighter = viewItem->findChild<MarkdownHighlighter *>();
        QVERIFY(highlighter);
        QSignalSpy updates(view->documentLayout(), &QAbstractTextDocumentLayout::update);
        QElapsedTimer elapsed;
        elapsed.start();
        highlighter->setTheme(true, "#111111", "#eeeeee", "#5584aa", "#252525");
        qInfo() << "1000-heading rich restyle:" << elapsed.elapsed() << "ms; layout updates:" << updates.count();
        // TextEdit must not synchronously relayout the document per heading.
        QVERIFY(updates.count() > 0);
        QVERIFY(updates.count() < 20);
        QCOMPARE(source->toPlainText(), text);
    }

    void imageLayoutsAreCompleteWhenProjectionIsPublished() {
        QTemporaryDir directory;
        QImage image(640, 400, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::darkCyan);
        QVERIFY(image.save(directory.filePath("image.png")));
        ImageResolver resolver;
        resolver.root = directory.path();
        controller->setResolver(&resolver);
        const QString text = QStringLiteral("# Heading\n\n![Picture](image.png)\n\n| A | B |\n| --- | --- |\n| one | two |\n\n```cpp\n/* open\nclose */\n```\n\nAfter.\n");
        source->setPlainText(text);
        select(sourceItem, text.size(), text.size());
        int checked = 0;
        const auto connection = connect(controller.get(), &LivePreview::projectionChanged, this, [&] {
            int imageCount = 0;
            for (QTextBlock block = view->begin(); block.isValid(); block = block.next()) {
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fragment = it.fragment();
                    if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) continue;
                    QVERIFY(block.layout());
                    // Do not force layout here: it must already be ready when published.
                    QVERIFY(block.layout()->lineForTextPosition(fragment.position() - block.position()).isValid());
                    ++checked;
                    ++imageCount;
                }
            }
            QCOMPARE(imageCount, 2); // User image plus the shared grid's native image slot, in both modes.
        });
        const auto disconnect = qScopeGuard([connection] { QObject::disconnect(connection); });
        for (int i = 0; i < 12; ++i) {
            viewItem->setProperty("width", 320 + (i % 3) * 110);
            controller->setReadMode(i % 2 == 0);
            controller->refresh();
            flush();
            QCOMPARE(source->toPlainText(), text);
        }
        QVERIFY(checked >= 12);
    }

    void sceneGraphModeSwitchWithImages() {
        if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
            QSKIP("Run this rendered-window regression with QT_QPA_PLATFORM=cocoa (or the native desktop platform).");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QImage image(160, 100, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::darkCyan);
        QVERIFY(image.save(directory.filePath("image.png")));
        ImageResolver resolver;
        resolver.root = directory.path();
        controller->setResolver(&resolver);
        const QString text = QStringLiteral("# Heading\n\n![First](image.png)\n\nBefore **bold**.\n\n| A | B |\n| --- | --- |\n| one | two |\n\n```cpp\n/* comment\n still comment */\nint value = 42;\n```\n\n![Last](image.png)\n\nAfter.\n");
        source->setPlainText(text);
        select(sourceItem, text.size(), text.size());
        controller->refresh();
        flush();
        QQuickWindow window;
        auto threaded = std::make_shared<std::atomic_bool>(false);
        connect(&window, &QQuickWindow::beforeRendering, &window, [threaded] {
            if (QThread::currentThread() != QCoreApplication::instance()->thread())
                threaded->store(true);
        }, Qt::DirectConnection);
        window.resize(600, 800);
        auto *item = qobject_cast<QQuickItem *>(root.get());
        item->setParentItem(window.contentItem());
        const auto detach = qScopeGuard([item] { item->setParentItem(nullptr); });
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
        for (int i = 0; i < 40; ++i) {
            const bool read = i % 2 == 0;
            viewItem->setProperty("readOnly", read);
            controller->setReadMode(read);
            if (!read) {
                const int at = i % 4 ? text.indexOf("Before") : text.size();
                select(sourceItem, at, at);
            }
            window.requestUpdate();
            QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 0, 5000);
            frames.clear();
            QVERIFY(!window.grabWindow().isNull());
            QCOMPARE(source->toPlainText(), text);
        }
        QVERIFY2(threaded->load(), "Use QSG_RENDER_LOOP=threaded and a native hardware rendering backend.");
    }

    void languageCodeTokensSurviveReadImportAndModeSwitches() {
        const QList<QPair<QString, QStringList>> fences = {
            {QStringLiteral("python"), {QStringLiteral("def greet(name):"), QStringLiteral("    return 'hello' # comment")}},
            {QStringLiteral("cpp"), {QStringLiteral("int answer = 42; // comment")}},
            {QStringLiteral("cpp"), {QStringLiteral("/*"), QStringLiteral("int value;"), QStringLiteral("*/")}},
            {QStringLiteral("js"), {QStringLiteral("const value = true;")}},
            {QStringLiteral("json"), {QStringLiteral("{\"key\": true, \"n\": 12}")}},
            {QStringLiteral("yaml"), {QStringLiteral("enabled: true # comment")}},
            {QStringLiteral("sh"), {QStringLiteral("echo \"hello\" # comment")}},
            {QStringLiteral("python"), {QStringLiteral("'''unfinished string")}},
            // Immediately adjacent, same language: MUST reset string state.
            {QStringLiteral("python"), {QStringLiteral("return 123")}}
        };
        // Indented literal fence must not claim the prefix of the real C++ comment.
        QString markdown = QStringLiteral("before\n\n    ```cpp\n    /*\n    ```\n\n");
        for (const auto &fence : fences)
            markdown += QStringLiteral("```%1\n%2\n```\n").arg(fence.first, fence.second.join(QLatin1Char('\n')));
        markdown += QStringLiteral("\n> ```js\n> let quoted = false;\n> ```\n\n- item\n\n  ```cpp\n  int nested = 17;\n  ```\n\n```unknown-omawrite-language\nplain _code_\n```\n\n```\nuntyped_var = 99\n```\n\n```cpp\n```\n\nafter\n\n```js\nconst unfinished = 19;\n");
        for (bool dark : {false, true}) {
            viewItem->setProperty("previewDark", dark);
            sourceItem->setProperty("text", markdown);
            source->clearUndoRedoStacks();
            controller->setReadMode(true);
            controller->refresh();
            flush();
            CodeHighlighter lexer(dark);
            QTextBlock nextBlock = view->begin();
            auto checkFence = [&](const QString &language, const QStringList &lines) {
                KSyntaxHighlighting::State state;
                const auto definition = CodeHighlighter::resolveLanguage(language);
                for (const QString &line : lines) {
                    QTextBlock block = nextBlock;
                    while (block.isValid() && block.text() != line) block = block.next();
                    QVERIFY2(block.isValid(), qPrintable(line));
                    nextBlock = block.next();
                    const auto expected = lexer.tokenize(line, definition, state);
                    state = expected.state;
                    QVERIFY(!expected.tokens.isEmpty());
                    for (const auto &token : expected.tokens) {
                        QTextCursor cursor(view);
                        cursor.setPosition(block.position() + token.offset);
                        cursor.setPosition(block.position() + token.offset + 1, QTextCursor::KeepAnchor);
                        if (token.format.hasProperty(QTextFormat::ForegroundBrush))
                            QCOMPARE(cursor.charFormat().foreground(), token.format.foreground());
                        else
                            QCOMPARE(cursor.charFormat().foreground().color(), viewItem->property("color").value<QColor>());
                    }
                }
            };
            for (const auto &fence : fences) checkFence(fence.first, fence.second);
            checkFence(QStringLiteral("js"), {QStringLiteral("let quoted = false;")});
            checkFence(QStringLiteral("cpp"), {QStringLiteral("int nested = 17;")});
            checkFence(QStringLiteral("js"), {QStringLiteral("const unfinished = 19;")});
            for (const auto &plain : {QStringLiteral("plain _code_"), QStringLiteral("untyped_var = 99")}) {
                const auto cursor = view->find(plain);
                QVERIFY(!cursor.isNull());
                QVERIFY(!cursor.charFormat().fontItalic());
                QCOMPARE(cursor.charFormat().foreground().color(), viewItem->property("color").value<QColor>());
            }
            QCOMPARE(source->toPlainText(), markdown);
            QVERIFY(!source->isUndoAvailable());
            controller->setReadMode(false);
            flush();
            QCOMPARE(source->toPlainText(), markdown);
            QVERIFY(!source->isUndoAvailable());
        }
    }

    void cumulativeTableAndImageBudgets() {
        viewItem->setProperty("width", 4096);
        QString tables = QStringLiteral("before\n\n");
        for (int n = 0; n < 8; ++n) {
            tables += QStringLiteral("| Name | Value |\n| --- | --- |\n");
            for (int row = 0; row < 32; ++row)
                tables += QStringLiteral("| table %1 row %2 | value |\n").arg(n).arg(row);
            tables += QLatin1Char('\n');
        }
        sourceItem->setProperty("text", tables);
        flush();
        auto resourceCount = [&] {
            qsizetype bytes = 0;
            int count = 0;
            for (QTextBlock block = view->begin(); block.isValid(); block = block.next()) {
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    if (!it.fragment().charFormat().isImageFormat()) continue;
                    const auto format = it.fragment().charFormat().toImageFormat();
                    bytes += view->resource(QTextDocument::ImageResource, QUrl(format.name()))
                                 .value<QImage>().sizeInBytes();
                    ++count;
                }
            }
            return qMakePair(count, bytes);
        };
        const auto tablesRendered = resourceCount();
        QVERIFY(tablesRendered.first > 0 && tablesRendered.first < 8);
        QVERIFY(tablesRendered.second <= 32 * 1024 * 1024);
        QCOMPARE(source->toPlainText(), tables);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage fixture(512, 512, QImage::Format_RGB32);
        fixture.fill(Qt::blue);
        QVERIFY(fixture.save(dir.filePath("test.png")));
        ImageResolver resolver;
        resolver.root = dir.path();
        controller->setResolver(&resolver);
        QString images = QStringLiteral("before\n\n");
        for (int n = 0; n < 100; ++n)
            images += QStringLiteral("![Image %1](test.png)\n\n").arg(n);
        sourceItem->setProperty("text", images);
        flush();
        const auto editImages = resourceCount();
        QVERIFY(editImages.first > 0 && editImages.first <= 32);
        QVERIFY(editImages.second <= 32 * 1024 * 1024);
        controller->setReadMode(true);
        const auto readImages = resourceCount();
        QVERIFY(readImages.first > 0 && readImages.first <= 32);
        QVERIFY(readImages.second <= 32 * 1024 * 1024);
        QCOMPARE(source->toPlainText(), images);
        controller->setReadMode(false);
        sourceItem->setProperty("text", QStringLiteral("after"));
        flush();
        QVERIFY(view->resource(QTextDocument::ImageResource, QUrl("preview:read/31"))
                    .value<QImage>().isNull());
        QVERIFY(view->resource(QTextDocument::ImageResource, QUrl("preview:edit/31"))
                    .value<QImage>().isNull());
    }

    void inlineCodeAndUnderlinePreviewPreserveSource() {
        const QString text = QStringLiteral("outside\n\nA `code` and `` C-x ` `` with <u>underlined</u>.");
        sourceItem->setProperty("text", text);
        sourceItem->setProperty("cursorPosition", 0);
        source->clearUndoRedoStacks();
        flush();
        const auto visibleEditText = [&] {
            QString visible;
            for (QTextBlock block = view->begin(); block.isValid(); block = block.next()) {
                if (!visible.isEmpty()) visible += QLatin1Char('\n');
                for (int i = 0; i < block.text().size(); ++i) {
                    bool hidden = false;
                    for (const auto &range : block.layout()->formats())
                        if (i >= range.start && i < range.start + range.length)
                            hidden = range.format.fontPointSize() == 1.0;
                    if (!hidden) visible += block.text().at(i);
                }
            }
            return visible;
        };
        QVERIFY(visibleEditText().contains(QStringLiteral("A code and C-x ` with underlined.")));
        const auto underlined = view->find(QStringLiteral("underlined"));
        bool hasUnderline = false;
        const int offset = underlined.selectionStart() - underlined.block().position();
        for (const auto &range : underlined.block().layout()->formats())
            if (offset >= range.start && offset < range.start + range.length)
                hasUnderline = range.format.fontUnderline();
        QVERIFY(hasUnderline);
        controller->setReadMode(true);
        QVERIFY(view->toPlainText().contains(QStringLiteral("A code and C-x ` with underlined.")));
        QVERIFY(view->find(QStringLiteral("underlined")).charFormat().fontUnderline());
        controller->setReadMode(false);
        sourceItem->setProperty("cursorPosition", text.indexOf(QStringLiteral("code")) + 1);
        flush();
        QVERIFY(visibleEditText().contains(QStringLiteral("`code` and C-x `")));
        QCOMPARE(source->toPlainText(), text);
        QVERIFY(!source->isUndoAvailable());
    }

    void readSpacingAndInlineStylesMatchEdit() {
        const QString text = QStringLiteral("before\n\n# Header\n\nA **bold** *italic* [link](https://example.com) `code`.\n\nAnother paragraph.\n\n| A | B |\n| --- | --- |\n| cell | y |\n\nAfter table.\n\n```text\nfirst\nsecond\n```\n\n- one\n- two\n");
        sourceItem->setProperty("text", text);
        sourceItem->setProperty("cursorPosition", 0);
        auto find = [&] (const QString &word) {
            for (QTextBlock block = view->begin(); block.isValid(); block = block.next())
                if (block.text().contains(word)) return block;
            return QTextBlock();
        };
        auto gap = [&] (const QString &left, const QString &right) {
            view->size();
            return view->documentLayout()->blockBoundingRect(find(right)).top()
                 - view->documentLayout()->blockBoundingRect(find(left)).bottom();
        };
        auto format = [&] (const QString &word, bool edit) {
            const QTextBlock block = find(word);
            const int start = block.text().indexOf(word);
            if (edit) {
                for (const auto &range : block.layout()->formats())
                    if (start >= range.start && start < range.start + range.length) return range.format;
            }
            QTextCursor cursor(block);
            cursor.setPosition(block.position() + start);
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            return cursor.charFormat();
        };
        for (bool dark : {false, true}) {
            viewItem->setProperty("previewDark", dark);
            viewItem->setProperty("color", QColor(dark ? "#eeeeee" : "#222324"));
            viewItem->setProperty("previewAccent", QColor("#c98545"));
            for (int pixels : {16, 28}) {
                QFont font = qvariant_cast<QFont>(viewItem->property("font"));
                font.setPixelSize(pixels);
                viewItem->setProperty("font", font);
                controller->refresh();
                flush();
                const qreal headingGap = gap("Header", "bold");
                const qreal paragraphGap = gap("bold", "Another");
                const auto bold = format("bold", true);
                const auto italic = format("italic", true);
                const auto link = format("link", true);
                const auto code = format("code", true);
                controller->setReadMode(true);
                QVERIFY(qAbs(gap("Header", "bold") - headingGap) < 1);
                QVERIFY(qAbs(gap("bold", "Another") - paragraphGap) < 1);
                QCOMPARE(format("bold", false).fontWeight(), bold.fontWeight());
                QCOMPARE(format("italic", false).fontItalic(), italic.fontItalic());
                QCOMPARE(format("link", false).foreground(), link.foreground());
                QCOMPARE(format("link", false).fontUnderline(), link.fontUnderline());
                QCOMPARE(format("link", false).anchorHref(), QStringLiteral("https://example.com"));
                QCOMPARE(format("code", false).background(), code.background());
                QCOMPARE(format("code", false).foreground(), code.foreground());
                const QTextBlock first = find("first"), second = find("second");
                QCOMPARE(first.blockFormat().bottomMargin(), 0.0);
                QCOMPARE(second.blockFormat().topMargin(), 0.0);
                QCOMPARE(find("one").blockFormat().bottomMargin(), 0.0);
                QCOMPARE(controller->tablePanels().size(), 1);
                const auto tablePanel = controller->tablePanels().first().toMap();
                auto *model = qobject_cast<MarkdownTableModel *>(qvariant_cast<QObject *>(tablePanel["model"]));
                QVERIFY(model);
                QCOMPARE(model->data(model->index(1, 0), MarkdownTableModel::MarkdownRole).toString(), QString("cell"));
                const int position = controller->m_readTablePositions.value(tablePanel["id"].toInt(), -1);
                QVERIFY(position >= 0);
                QVERIFY(view->documentLayout()->blockBoundingRect(view->findBlock(position)).height() >= tablePanel["height"].toReal());
                QCOMPARE(source->toPlainText(), text);
                controller->setReadMode(false);
            }
        }
    }

    void normalTextStaysRegularAcrossModeToggles() {
        const QString text = QStringLiteral("# Header\n\nParagraph with **bold** text.\n\nAnother paragraph.");
        sourceItem->setProperty("text", text);
        sourceItem->setProperty("cursorPosition", text.size());
        for (bool dark : {false, true}) {
            const QColor strong(dark ? "#eeeeee" : "#222324");
            const QColor body(dark ? "#b8b8b8" : "#575859");
            viewItem->setProperty("previewDark", dark);
            viewItem->setProperty("previewForeground", strong);
            viewItem->setProperty("color", body);
            controller->refresh();
            flush();
            for (bool read : {false, true, false, true, false}) {
                controller->setReadMode(read);
                for (auto block = view->begin(); block.isValid(); block = block.next()) {
                    QTextCursor cursor(block);
                    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                    const auto format = cursor.charFormat();
                    if (block.text().startsWith("Header") && read)
                        QCOMPARE(format.foreground().color(), strong);
                    if (!block.text().startsWith("Paragraph") && !block.text().startsWith("Another")) continue;
                    QCOMPARE(format.fontWeight(), int(QFont::Normal));
                    QVERIFY(!format.fontItalic());
                    QCOMPARE(block.blockFormat().headingLevel(), 0);
                    if (read) QCOMPARE(format.foreground().color(), body);
                    else QCOMPARE(format.foreground().style(), Qt::NoBrush); // uses the TextEdit body color
                }
                QCOMPARE(source->toPlainText(), text);
            }
        }
    }

    void nestedAndLooseListsKeepSourceSpacing() {
        for (const QString &items : {QStringLiteral("- parent\n  - child\n- sibling"),
                                    QStringLiteral("- parent\n\n- child\n\n- sibling")}) {
            sourceItem->setProperty("text", "before\n\n" + items + "\n\nafter");
            sourceItem->setProperty("cursorPosition", 0);
            flush();
            auto gap = [&] (const QString &left, const QString &right) {
                view->size();
                QRectF a, b;
                for (auto block = view->begin(); block.isValid(); block = block.next()) {
                    if (block.text().contains(left)) a = view->documentLayout()->blockBoundingRect(block);
                    if (block.text().contains(right)) b = view->documentLayout()->blockBoundingRect(block);
                }
                return b.top() - a.bottom();
            };
            const qreal firstGap = gap("parent", "child"), secondGap = gap("child", "sibling");
            controller->setReadMode(true);
            QVERIFY(qAbs(gap("parent", "child") - firstGap) < 1);
            QVERIFY(qAbs(gap("child", "sibling") - secondGap) < 1);
            controller->setReadMode(false);
        }
    }

    void headingSizesSurviveModeToggleAndZoom() {
        QString text = QStringLiteral("before\n\n");
        for (int level = 1; level <= 6; ++level)
            text += QString(level, QLatin1Char('#')) + QStringLiteral(" Title %1\n\n").arg(level);
        sourceItem->setProperty("text", text);
        sourceItem->setProperty("cursorPosition", 0);
        for (int pixels : {16, 28}) {
            QFont font = qvariant_cast<QFont>(viewItem->property("font"));
            font.setPixelSize(pixels);
            viewItem->setProperty("font", font);
            controller->refresh();
            flush();
            auto sizes = [&] {
                view->size();
                QList<qreal> result;
                for (QTextBlock block = view->begin(); block.isValid(); block = block.next()) {
                    const int start = block.text().indexOf(QStringLiteral("Title"));
                    if (start < 0) continue;
                    const auto runs = block.layout()->glyphRuns(start, 5);
                    result.append(runs.isEmpty() ? -1 : runs.first().rawFont().pixelSize());
                }
                return result;
            };
            const auto editSizes = sizes();
            QCOMPARE(editSizes.size(), 6);
            QVERIFY(editSizes.first() > pixels);
            QVERIFY(editSizes.last() > 0 && editSizes.last() < pixels);
            controller->setReadMode(true);
            QCOMPARE(sizes(), editSizes);
            controller->setReadMode(false);
            QCOMPARE(sizes(), editSizes);
            QCOMPARE(source->toPlainText(), text);
        }
    }

    void mappingUtf16() {
        const QList<LivePreview::Segment> segments{{0, 5, 0, 5, false}, {5, 25, 5, 6, true}, {25, 31, 6, 12, false}};
        QCOMPARE(LivePreview::toSource(segments, 5), 5);
        QCOMPARE(LivePreview::toSource(segments, 6), 25);
        QCOMPARE(LivePreview::toSource(segments, 10), 29);
        QCOMPARE(LivePreview::toView(segments, 18), 5);
        QCOMPARE(LivePreview::toView(segments, 25), 6);
        QCOMPARE(LivePreview::toView(segments, 29), 10);
        QCOMPARE(QString::fromUtf8("😀").size(), 2);
    }
    void nativeObjectAndSourcePreservation() {
        QVERIFY(view->toPlainText().contains(QChar::ObjectReplacementCharacter));
        QCOMPARE(source->toPlainText(), markdown);
        QVERIFY(!source->isUndoAvailable());
        for (int n = 0; n < 4; ++n) {
            controller->setReadMode(true);
            QVERIFY(!view->toPlainText().contains("| --- |"));
            controller->setReadMode(false);
            viewItem->setProperty("width", 400 + n * 20);
            controller->refresh();
            flush();
        }
        QCOMPARE(source->toPlainText(), markdown);
        QVERIFY(!source->isUndoAvailable());
    }
    void activeBlockAndBackwardsSelection() {
        const int tableStart = markdown.indexOf('|');
        select(sourceItem, tableStart + 4, tableStart + 4);
        controller->setTableSource(controller->m_tables.first().id, true); flush();
        QVERIFY(!view->toPlainText().contains(QChar::ObjectReplacementCharacter));
        QCOMPARE(view->toPlainText(), markdown);
        select(sourceItem, markdown.size() - 1, 2);
        QCOMPARE(viewItem->property("cursorPosition").toInt(), 2);
        QCOMPARE(viewItem->property("selectionEnd").toInt(), int(markdown.size()) - 1);
        QCOMPARE(source->toPlainText(), markdown);
    }
    void nativeTypingUndoRedo() {
        select(sourceItem, markdown.size() - 1, markdown.size() - 1);
        type("XYZ");
        QCOMPARE(source->toPlainText(), QString(markdown).insert(markdown.size() - 1, "XYZ"));
        QVERIFY(source->isUndoAvailable());
        QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
        QCoreApplication::sendEvent(viewItem, &undo); flush();
        QCOMPARE(source->toPlainText(), markdown);
        QKeyEvent redo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCoreApplication::sendEvent(viewItem, &redo); flush();
        QCOMPARE(source->toPlainText(), QString(markdown).insert(markdown.size() - 1, "XYZ"));
        controller->setReadMode(true);
        controller->setReadMode(false);
        source->undo(); flush();
        QCOMPARE(source->toPlainText(), markdown);
    }
    void boundaryDeleteIsOneSourceCharacter() {
        const int after = markdown.indexOf("\n\nafter");
        select(sourceItem, after + 1, after + 1);
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QCoreApplication::sendEvent(viewItem, &key); flush();
        QCOMPARE(source->toPlainText(), QString(markdown).remove(after, 1));
        source->undo(); flush();
        QCOMPARE(source->toPlainText(), markdown);
    }
    void nativeSelectionReplacement() {
        const int tableStart = markdown.indexOf('|');
        select(viewItem, view->toPlainText().size() - 1, 2);
        QVERIFY(!view->toPlainText().contains(QChar::ObjectReplacementCharacter));
        type("X");
        QCOMPARE(source->toPlainText(), markdown.left(2) + "X\n");
        Q_UNUSED(tableStart);
        source->undo(); flush();
        QCOMPARE(source->toPlainText(), markdown);
    }
    void imePreservesSource() {
        select(sourceItem, markdown.size() - 1, markdown.size() - 1);
        QInputMethodEvent preedit(QString::fromUtf8("に"), {});
        QCoreApplication::sendEvent(viewItem, &preedit); flush();
        QCOMPARE(source->toPlainText(), markdown);
        QInputMethodEvent commit;
        commit.setCommitString(QString::fromUtf8("日本"));
        QCoreApplication::sendEvent(viewItem, &commit); flush();
        QCOMPARE(source->toPlainText(), QString(markdown).insert(markdown.size() - 1, QString::fromUtf8("日本")));
        source->undo(); flush();
        QCOMPARE(source->toPlainText(), markdown);
    }
    void localAndRemoteImages() {
        QTemporaryDir directory;
        QImage image(300, 200, QImage::Format_RGB32); image.fill(Qt::red);
        QVERIFY(image.save(directory.filePath("my image.png")));
        ImageResolver resolver; resolver.root = directory.path();
        controller->setResolver(&resolver);
        const QString text = "start\n\n![alt](<my image.png>)\n\n![remote](https://invalid.example/a.png)\n\nend";
        source->setPlainText(text); flush();
        controller->refresh(); flush();
        QCOMPARE(view->toPlainText().count(QChar::ObjectReplacementCharacter), 1);
        QCOMPARE(source->toPlainText(), text);
        controller->setReadMode(true);
        QVERIFY(!view->toHtml().contains("https://invalid.example/a.png"));
        controller->setReadMode(false);
        QCOMPARE(source->toPlainText(), text);
    }
    void readImagesKeepOriginalDestinationsForContextCopy() {
        QTemporaryDir directory;
        QImage image(100, 80, QImage::Format_RGB32); image.fill(Qt::blue);
        QVERIFY(image.save(directory.filePath("my image.png")));
        ImageResolver resolver; resolver.root = directory.path();
        controller->setResolver(&resolver);
        const QString text = "Inline ![a](<my image.png>) text\n\n![b][ref]\n\n[ref]: <my image.png>\n\n![bad](https://invalid.example/a.png)";
        source->setPlainText(text); flush();
        controller->setReadMode(true); flush();
        int count = 0;
        for (int i = 0; i < view->characterCount() - 1; ++i) {
            if (view->characterAt(i) == QChar::ObjectReplacementCharacter) {
                QCOMPARE(controller->imagePathAt(i), QString("my image.png"));
                ++count;
            } else QVERIFY(controller->imagePathAt(i).isEmpty());
        }
        QCOMPARE(count, 2);
        QVERIFY(controller->imagePathAt(-1).isEmpty());
        QVERIFY(controller->imagePathAt(view->characterCount()).isEmpty());
        QCOMPARE(source->toPlainText(), text);
        viewItem->setProperty("width", 180);
        source->setPlainText("a ![wrapped](<my image.png>)" + QString(80, 'x')); flush();
        const int object = view->toPlainText().indexOf(QChar::ObjectReplacementCharacter);
        QVERIFY(object >= 0);
        QRectF before, after;
        QVERIFY(QMetaObject::invokeMethod(viewItem, "positionToRectangle", Q_RETURN_ARG(QRectF, before), Q_ARG(int, object)));
        QVERIFY(QMetaObject::invokeMethod(viewItem, "positionToRectangle", Q_RETURN_ARG(QRectF, after), Q_ARG(int, object + 1)));
        QVERIFY(after.y() > before.y()); // Old insertion-point bounds reject the actual image here.
        const auto block = view->findBlock(object);
        const auto *layout = block.layout();
        const auto line = layout->lineForTextPosition(object - block.position());
        const qreal x = viewItem->property("leftPadding").toReal() + layout->position().x()
            + line.cursorToX(object - block.position());
        const qreal y = viewItem->property("topPadding").toReal() + layout->position().y() + line.y() + line.height() / 2;
        for (qreal fraction : {0.25, 0.75})
            QCOMPARE(controller->imagePathAtPoint(x + image.width() * fraction, y), QString("my image.png"));
        QVERIFY(controller->imagePathAtPoint(-10, y).isEmpty());
        QVERIFY(controller->imagePathAtPoint(x + image.width() + 10, y).isEmpty());
        source->setPlainText("no images"); flush();
        QVERIFY(controller->imagePathAt(0).isEmpty());
        controller->setReadMode(false); flush();
        QVERIFY(controller->imagePathAt(0).isEmpty());
    }
    void repeatedTextUsesActualNativeEditRange() {
        source->setPlainText("aaaa"); flush();
        select(sourceItem, 0, 0);
        type("a");
        QCOMPARE(sourceItem->property("cursorPosition").toInt(), 1);
        QCOMPARE(source->toRawText(), QString("aaaaa"));
        source->setPlainText(QString(QChar::Nbsp) + QLatin1Char(' ')); flush();
        select(sourceItem, 0, 0);
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QCoreApplication::sendEvent(viewItem, &key); flush();
        QCOMPARE(source->toRawText(), QString(" "));
        source->undo(); flush();
        QCOMPARE(source->toRawText(), QString(QChar::Nbsp) + QLatin1Char(' '));
    }
    void implicitResourcesAreDeniedAndRichPasteCannotChangeSource() {
        QTemporaryDir directory;
        QImage image(40, 20, QImage::Format_ARGB32); image.fill(Qt::green);
        const QUrl url = QUrl::fromLocalFile(directory.filePath("private.png"));
        QVERIFY(image.save(url.toLocalFile()));
        QVERIFY(!view->resource(QTextDocument::ImageResource, url).isValid());
        auto *mime = new QMimeData;
        mime->setHtml("<p><img src='https://invalid.example/private.png'></p>");
        QGuiApplication::clipboard()->setMimeData(mime);
        QVERIFY(QMetaObject::invokeMethod(viewItem, "paste"));
        flush();
        QCOMPARE(source->toPlainText(), markdown);
        QVERIFY(!view->toHtml().contains("https://invalid.example/private.png"));
        QVERIFY(!view->resource(QTextDocument::ImageResource,
                               QUrl("https://invalid.example/private.png")).isValid());
    }
    void ambiguousObjectRemovalIsRejected() {
        int object = view->toPlainText().indexOf(QChar::ObjectReplacementCharacter);
        QVERIFY(object >= 0);
        QTextCursor cursor(view);
        cursor.setPosition(object);
        cursor.deleteChar(); flush();
        QCOMPARE(source->toPlainText(), markdown);
        QVERIFY(view->toPlainText().contains(QChar::ObjectReplacementCharacter));
    }
    void constrainedMermaidSyntax() {
        QVERIFY(MermaidRenderer::supported("flowchart LR\nA-->B"));
        QVERIFY(MermaidRenderer::supported("sequenceDiagram\nAlice->>Bob: Hello"));
        QVERIFY(!MermaidRenderer::supported("flowchart LR\nclick A 'https://example.com'"));
        QVERIFY(!MermaidRenderer::supported("flowchart LR\n%%{init:{}}%%"));
        QVERIFY(!MermaidRenderer::supported("flowchart LR\nA[<img src='x'>]"));
    }
    void mermaidGenerationIsBoundedAndCannotRerenderForever() {
#ifndef Q_OS_UNIX
        QSKIP("Fake mmdc uses a POSIX script");
#else
        const QByteArray previousPath = qgetenv("PATH");
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", previousPath); });
        QTemporaryDir directory;
        QImage image(40, 20, QImage::Format_ARGB32); image.fill(Qt::red);
        const QString png = directory.filePath("fixture.png");
        QVERIFY(image.save(png));
        QFile script(directory.filePath("mmdc"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(("#!/bin/sh\n/bin/cp '" + png + "' \"$4\"\n").toUtf8());
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", directory.path().toUtf8());
        MermaidRenderer renderer;
        renderer.setGeneration("document");
        QSignalSpy ready(&renderer, &MermaidRenderer::ready);
        auto requestAll = [&] {
            for (int i = 0; i < 20; ++i)
                renderer.request(QString("```mermaid\nflowchart LR\nA%1-->B\n```").arg(i), false, 500);
        };
        connect(&renderer, &MermaidRenderer::ready, this, requestAll);
        requestAll();
        QTRY_COMPARE(ready.count(), 16);
        QTest::qWait(100);
        QCOMPARE(ready.count(), 16);
        renderer.setGeneration("next document");
        disconnect(&renderer, &MermaidRenderer::ready, this, nullptr);
        const QString newCode = "```mermaid\nflowchart LR\nNew-->B\n```";
        QVERIFY(renderer.request(newCode, false, 500).isNull());
        QTRY_COMPARE(ready.count(), 17);
        QCOMPARE(renderer.request(newCode, false, 500).size(), QSize(40, 20));
        QVERIFY(renderer.request("```mermaid\nflowchart LR\nA-->B", false, 500).isNull());
#endif
    }
    void readFootnotesKeepSeparateBlocksAndSource() {
        const QString text = QStringLiteral("A[^a] and B[^b] and repeated[^a].\n\n"
            "[^a]: One **bold** note.\n[^b]: Another note.\n\n    A second paragraph.\n\nOutside.\n");
        source->setPlainText(text); source->clearUndoRedoStacks(); flush();
        controller->setReadMode(true); flush();
        const QString plain = view->toPlainText();
        QVERIFY2(!plain.contains(QStringLiteral("[^")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("One bold note.")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("A second paragraph.")), qPrintable(plain));
        const int first = controller->footnoteViewPosition(MarkdownReferences::noteUrl(QStringLiteral("a")));
        const int second = controller->footnoteViewPosition(MarkdownReferences::noteUrl(QStringLiteral("b")));
        QVERIFY(first >= 0 && second > first);
        QVERIFY(view->findBlock(first) != view->findBlock(second));
        for (int i = 0; i < 3; ++i) { controller->setReadMode(false); flush(); controller->setReadMode(true); flush(); }
        QCOMPARE(view->toPlainText(), plain);
        QCOMPARE(source->toPlainText(), text);
        QVERIFY(!source->isUndoAvailable());
    }

    void readLinks() {
        source->setPlainText("[Example](https://example.com)\n\n**bold**"); flush();
        controller->setReadMode(true);
        QVERIFY(view->toHtml().contains("href=\"https://example.com\""));
        QVERIFY(!view->toPlainText().contains("**"));
        controller->setReadMode(false);
        QCOMPARE(source->toPlainText(), QString("[Example](https://example.com)\n\n**bold**"));
    }
};
QTEST_MAIN(LivePreviewTest)
#include "tst_livepreview.moc"

#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QAbstractTextDocumentLayout>
#include <QInputMethodEvent>
#include <QTemporaryDir>
#include <QImage>
#include <memory>
#include "../../src/livepreview.h"
#include "../../src/markdownhighlighter.h"
#include "../../src/spellingcontroller.h"

namespace {
class FakeDictionary final : public SpellingDictionary {
public:
    QMap<QString, QString> dictionaries() const override { return {{"English", "en_US"}}; }
    bool selectLanguage(const QString &language) override { return language == "en_US"; }
    bool isCorrect(const QString &word) override { return word != "typo"; }
    QStringList suggestions(const QString &) override { return {"fixed"}; }
    bool learn(const QString &) override { return true; }
};

QTextCharFormat formatAt(QTextDocument *document, int position) {
    const QTextBlock block = document->findBlock(position);
    QTextCharFormat result;
    if (block.isValid() && block.layout()) {
        for (const auto &range : block.layout()->formats()) {
            if (position - block.position() >= range.start
                    && position - block.position() < range.start + range.length)
                result.merge(range.format);
        }
    }
    return result;
}

bool squiggleAt(QTextDocument *document, int position) {
    return formatAt(document, position).underlineStyle() == QTextCharFormat::SpellCheckUnderline;
}

class CountingHighlighter final : public MarkdownHighlighter {
public:
    explicit CountingHighlighter(QTextDocument *document) : MarkdownHighlighter(document) {}
    QList<int> visited;
protected:
    void highlightBlock(const QString &text) override {
        visited.append(currentBlock().blockNumber());
        MarkdownHighlighter::highlightBlock(text);
    }
};
}

class IntegrationImageResolver : public QObject {
    Q_OBJECT
public:
    QString root;
    Q_INVOKABLE QUrl resolveImageUrl(const QString &path) {
        return QUrl::fromLocalFile(root + "/" + path);
    }
};

class SpellingIntegrationTest : public QObject {
    Q_OBJECT
private:
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    std::unique_ptr<SpellingController> spelling;
    std::unique_ptr<LivePreview> preview;
    std::unique_ptr<MarkdownHighlighter> sourceHighlighter;
    QObject *sourceItem = nullptr;
    QObject *viewItem = nullptr;
    QTextDocument *source = nullptr;
    QTextDocument *view = nullptr;

    void flush() { QCoreApplication::processEvents(); QCoreApplication::processEvents(); }
    void setText(const QString &text) {
        source->setPlainText(text);
        // Keep the caret away from rendered blocks so they stay collapsed.
        QMetaObject::invokeMethod(sourceItem, "select", Q_ARG(int, text.size()), Q_ARG(int, text.size()));
        preview->refresh();
        flush();
        source->clearUndoRedoStacks();
    }
    void enableSpelling() { spelling->setEnabled(true); }

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
        QVERIFY(sourceItem);
        QVERIFY(viewItem);
        source = qvariant_cast<QQuickTextDocument *>(sourceItem->property("textDocument"))->textDocument();
        spelling = std::make_unique<SpellingController>(std::make_unique<FakeDictionary>());
        spelling->setLanguage("en_US");
        spelling->setEnabled(false);
        spelling->attach(source);
        sourceHighlighter = std::make_unique<MarkdownHighlighter>(source);
        connect(spelling.get(), &SpellingController::rangesChanged, this, [this] {
            QList<MarkdownHighlighter::Span> ranges;
            for (const auto &range : spelling->ranges())
                ranges.append({range.start, range.length});
            sourceHighlighter->setSpellingRanges(ranges);
        });
        preview = std::make_unique<LivePreview>();
        auto *quick = qvariant_cast<QQuickTextDocument *>(viewItem->property("textDocument"));
        connect(quick, &QQuickTextDocument::textDocumentChanged, root.get(), [this, quick] { view = quick->textDocument(); });
        preview->setSpelling(spelling.get());
        preview->attach(sourceItem, viewItem);
        view = qvariant_cast<QQuickTextDocument *>(viewItem->property("textDocument"))->textDocument();
        flush();
    }
    void cleanup() {
        preview.reset();
        spelling.reset();
        sourceHighlighter.reset();
        root.reset();
    }

    void pointerHitsOnlyActualGlyphsIncludingWrappedAndBidiText() {
        viewItem->setProperty("padding", 8);
        viewItem->setProperty("width", 160);
        for (const QString &prefix : {QString(), QString::fromUtf8("שלום ")}) {
            for (int pixels : {20, 80}) {
                QFont font = viewItem->property("font").value<QFont>();
                font.setPixelSize(pixels);
                viewItem->setProperty("font", font);
                setText(prefix + "typo   good\n\n");
                enableSpelling();
                QTRY_COMPARE(spelling->ranges().size(), 1);
                flush();
                view->documentLayout()->documentSize();
                const auto block = view->firstBlock();
                const auto *layout = block.layout();
                QVERIFY(layout);
                layout->glyphRuns(); // Shape fallback/bidi runs before measuring caret geometry.
                for (int i = prefix.size(); i < prefix.size() + 4; ++i) {
                    const QTextLine line = layout->lineForTextPosition(i);
                    QVERIFY(line.isValid());
                    const auto runs = layout->glyphRuns(i, 1);
                    QVERIFY(!runs.isEmpty() && !runs.first().positions().isEmpty());
                    const auto &run = runs.first();
                    const qreal a = run.positions().first().x();
                    const qreal b = a + run.rawFont().advancesForGlyphIndexes(run.glyphIndexes()).first().x();
                    for (qreal fraction : {0.25, 0.75}) {
                        const QPointF point = layout->position() + QPointF(a + (b - a) * fraction,
                            line.y() + line.height() / 2) + QPointF(8, 8);
                        const auto token = preview->spellingWordAtPoint(point.x(), point.y());
                        if (token.isEmpty())
                            qInfo() << "caret x" << a << b << "layout" << layout->position()
                                << "hit" << view->documentLayout()->hitTest(point - QPointF(8, 8), Qt::ExactHit)
                                << "onchar" << line.xToCursor(point.x() - 8, QTextLine::CursorOnCharacter)
                                << "view" << view->toPlainText();
                        QVERIFY2(token.value("word").toString() == QString("typo"),
                            qPrintable(QString("prefix=%1 pixels=%2 i=%3 fraction=%4 point=%5,%6 line=%7,%8,%9,%10")
                                .arg(prefix).arg(pixels).arg(i).arg(fraction).arg(point.x()).arg(point.y())
                                .arg(line.naturalTextRect().x()).arg(line.naturalTextRect().y())
                                .arg(line.naturalTextRect().width()).arg(line.naturalTextRect().height())));
                        QCOMPARE(token.value("start").toInt(), prefix.size());
                    }
                }
                QVERIFY(preview->spellingWordAtPoint(210, 15).isEmpty());
                QVERIFY(preview->spellingWordAtPoint(20, -10).isEmpty());
            }
        }
    }

    void asyncArrivalPreservesProjectionSourceAndUndo() {
        const QString original = "good\n\n";
        setText(original);
        QTextCursor cursor(source);
        cursor.movePosition(QTextCursor::End);
        cursor.insertText("typo");
        flush();
        const QString text = source->toPlainText();
        const QString projected = view->toPlainText();
        const int undoSteps = source->availableUndoSteps();
        const bool modified = source->isModified();
        QVERIFY(undoSteps > 0);
        QSignalSpy projections(preview.get(), &LivePreview::projectionChanged);
        QSignalSpy changes(source, &QTextDocument::contentsChanged);
        QSignalSpy arrivals(spelling.get(), &SpellingController::rangesChanged);
        enableSpelling();
        QVERIFY(spelling->ranges().isEmpty());
        QTRY_COMPARE(spelling->ranges().size(), 1);
        QTRY_VERIFY(squiggleAt(view, preview->viewPosition(text.indexOf("typo"))));
        QVERIFY(!arrivals.isEmpty());
        QCOMPARE(projections.count(), 0);
        QCOMPARE(changes.count(), 0);
        QCOMPARE(source->toPlainText(), text);
        QCOMPARE(view->toPlainText(), projected);
        // QSyntaxHighlighter may advance QTextDocument's formatting revision,
        // but must not change modified state or the source undo commands.
        QCOMPARE(source->isModified(), modified);
        QCOMPARE(source->availableUndoSteps(), undoSteps);
        QVERIFY(!view->isUndoAvailable());
        source->undo();
        flush();
        QCOMPARE(source->toPlainText(), original);
        QVERIFY(!source->isUndoAvailable());
        source->redo();
        flush();
        QCOMPARE(source->toPlainText(), text);
    }

    void collapsedObjectsAreSkippedAndFollowingRangesMapExactly() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QImage image(80, 50, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QVERIFY(image.save(directory.filePath("image.png")));
        IntegrationImageResolver resolver;
        resolver.root = directory.path();
        preview->setResolver(&resolver);
        const QString text = QString::fromUtf8(
            "😀 typo\n\n| typo | good |\n| --- | --- |\n| good | good |\n\n"
            "typo\n\n![typo](image.png)\n\ntypo\n\ngood\n");
        setText(text);
        int objects = 0;
        for (const auto &segment : preview->segments())
            objects += segment.object;
        QCOMPARE(objects, 2);
        enableSpelling();
        // Table-cell prose is checked, but image alt text is excluded by SpellingProse.
        QTRY_COMPARE(spelling->ranges().size(), 4);
        QTRY_VERIFY(squiggleAt(view, preview->viewPosition(text.lastIndexOf("typo"))));
        QSet<int> expected;
        for (int start : {text.indexOf("typo"), text.indexOf("\n\ntypo") + 2, text.lastIndexOf("typo")}) {
            const int mapped = preview->viewPosition(start);
            QCOMPARE(view->toPlainText().mid(mapped, 4), QString("typo"));
            for (int i = 0; i < 4; ++i)
                expected.insert(mapped + i);
        }
        for (int i = 0; i < view->toPlainText().size(); ++i)
            QCOMPARE(squiggleAt(view, i), expected.contains(i));
        for (const auto &segment : preview->segments()) {
            if (segment.object) {
                for (int i = segment.viewStart; i < segment.viewEnd; ++i) {
                    QVERIFY(!squiggleAt(view, i));
                    QVERIFY(preview->spellingWordAt(i).isEmpty());
                }
            }
        }
        QCOMPARE(source->toPlainText(), text);
        QVERIFY(!source->isUndoAvailable());
    }

    void readModeSuppressesSpellingAndEditModeRestoresIt() {
        setText("good **typo**\n\ngood\n");
        enableSpelling();
        QTRY_COMPARE(spelling->ranges().size(), 1);
        const int start = source->toPlainText().indexOf("typo");
        QTRY_VERIFY(squiggleAt(view, preview->viewPosition(start)));
        preview->setReadMode(true);
        flush();
        QVERIFY(view->toPlainText().contains("typo"));
        QVERIFY(preview->spellingWordAt(view->toPlainText().indexOf("typo")).isEmpty());
        for (int i = 0; i < view->toPlainText().size(); ++i)
            QVERIFY(!squiggleAt(view, i));
        // Also exercise arrivals while already in read mode.
        spelling->setEnabled(false);
        enableSpelling();
        QTRY_COMPARE(spelling->ranges().size(), 1);
        for (int i = 0; i < view->toPlainText().size(); ++i)
            QVERIFY(!squiggleAt(view, i));
        preview->setReadMode(false);
        flush();
        QTRY_VERIFY(squiggleAt(view, preview->viewPosition(start)));
        QVERIFY(!source->isUndoAvailable());
    }

    void imeDefersPendingSpellingUntilPreeditEnds() {
        setText("typo good\n");
        QSignalSpy arrivals(spelling.get(), &SpellingController::rangesChanged);
        enableSpelling();
        QInputMethodEvent preedit(QString::fromUtf8("に"), {});
        QCoreApplication::sendEvent(viewItem, &preedit);
        flush();
        QSignalSpy projections(preview.get(), &LivePreview::projectionChanged);
        // Longer than the controller debounce; pending results must not paint.
        QTest::qWait(500);
        QCOMPARE(arrivals.count(), 0);
        QVERIFY(spelling->ranges().isEmpty());
        QVERIFY(!squiggleAt(view, 0));
        QCOMPARE(projections.count(), 0);
        QCOMPARE(source->toPlainText(), QString("typo good\n"));
        QVERIFY(!source->isUndoAvailable());
        QInputMethodEvent endPreedit;
        QCoreApplication::sendEvent(viewItem, &endPreedit);
        QTRY_COMPARE(spelling->ranges().size(), 1);
        QTRY_VERIFY(squiggleAt(view, 0));
        QCOMPARE(source->toPlainText(), QString("typo good\n"));
        QVERIFY(!source->isUndoAvailable());
    }

    void spellingMergesWithBoldForegroundAndSearch() {
        setText("good **[typo](https://example.test)** good\n");
        const int start = source->toPlainText().indexOf("typo");
        const int mapped = preview->viewPosition(start);
        const QTextCharFormat before = formatAt(view, mapped);
        QVERIFY(before.fontWeight() >= QFont::Bold);
        QVERIFY(before.hasProperty(QTextFormat::ForegroundBrush));
        preview->setSearchHighlight("typo", start);
        const QTextCharFormat search = formatAt(view, mapped);
        QVERIFY(search.hasProperty(QTextFormat::BackgroundBrush));
        enableSpelling();
        QTRY_VERIFY(squiggleAt(view, mapped));
        for (int i = 0; i < 4; ++i) {
            const auto merged = formatAt(view, mapped + i);
            QCOMPARE(merged.fontWeight(), before.fontWeight());
            QCOMPARE(merged.foreground(), before.foreground());
            QCOMPARE(merged.background(), search.background());
        }
        QSignalSpy projections(preview.get(), &LivePreview::projectionChanged);
        spelling->setEnabled(false);
        QVERIFY(!squiggleAt(view, mapped));
        QCOMPARE(formatAt(view, mapped).fontWeight(), before.fontWeight());
        QCOMPARE(formatAt(view, mapped).foreground(), before.foreground());
        QCOMPARE(formatAt(view, mapped).background(), search.background());
        QCOMPARE(projections.count(), 0);
        QVERIFY(!source->isUndoAvailable());
    }

    void clearingRangesRehighlightsOnlyAffectedBlocks() {
        QTextDocument document;
        document.setPlainText("typo\ngood\ntypo\ngood\n");
        CountingHighlighter highlighter(&document);
        highlighter.rehighlight();
        highlighter.setSpellingRanges({{0, 4}, {10, 4}});
        QVERIFY(squiggleAt(&document, 0));
        QVERIFY(squiggleAt(&document, 10));
        highlighter.visited.clear();
        highlighter.setSpellingRanges({{10, 4}});
        QCOMPARE(highlighter.visited, QList<int>{0});
        QVERIFY(!squiggleAt(&document, 0));
        QVERIFY(squiggleAt(&document, 10));
        highlighter.visited.clear();
        highlighter.setSpellingRanges({});
        QCOMPARE(highlighter.visited, QList<int>{2});
        QVERIFY(!squiggleAt(&document, 10));
        highlighter.visited.clear();
        highlighter.setSpellingRanges({});
        QVERIFY(highlighter.visited.isEmpty());
    }
};

QTEST_MAIN(SpellingIntegrationTest)
#include "tst_integration.moc"

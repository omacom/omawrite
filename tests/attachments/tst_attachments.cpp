#include <QtTest>
#include <QApplication>
#include <QMessageBox>
#include <QTimer>
#include <QClipboard>
#include <QImage>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextDocument>
#include "backend.h"
#include "attachmenthelper.h"
#include "printdocument.h"
#include <QTextBlock>
#include <QTextImageFormat>

class OmawriteAttachmentsTest : public QObject {
    Q_OBJECT
    static void bind(Backend &backend, QTextDocument &document) {
        backend.m_document = &document;
        backend.m_autoSaveHeld = true;
    }
    static QByteArray bytes(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
    static void put(const QString &path, const QByteArray &value) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(value), value.size());
    }
    static void clipboardImage() {
        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QGuiApplication::clipboard()->setImage(image);
    }
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("OmawriteAttachmentTests"));
        QCoreApplication::setApplicationName(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    void cleanupTestCase() {
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    }
    void exactLiveReferencesAfterLocate() {
        QTemporaryDir dir;
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        clipboardImage();
        document.setPlainText(backend.pasteClipboardImage("img/lost.png"));
        const QString stored = backend.resolveImageUrl("img/lost.png").toLocalFile();
        QVERIFY(QFile::remove(stored));
        const QString replacement = dir.filePath("replacement.png");
        put(replacement, "replacement image bytes");
        QVERIFY(backend.locateAttachment("img/lost.png", QUrl::fromLocalFile(replacement),
                                         "img/lost.png.recovered.png"));
        document.setPlainText(document.toPlainText() + "\n`![example](img/lost.png)`\n");
        backend.setModified(true);
        const QString saved = dir.filePath("note.md");
        backend.saveAs(QUrl::fromLocalFile(saved));
        QCOMPARE(bytes(saved), document.toPlainText().toUtf8());
        QCOMPARE(bytes(dir.filePath("img/lost.png.recovered.png")), QByteArray("replacement image bytes"));
    }
    void encodedMissingReferenceBlocksSave() {
        QTemporaryDir dir;
        const QString saved = dir.filePath("note.md");
        put(saved, "start");
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        backend.open(QUrl::fromLocalFile(saved));
        clipboardImage();
        QVERIFY(!backend.pasteClipboardImage("img/lost-encoded.png").isEmpty());
        const QString stored = backend.resolveImageUrl("img/lost-encoded.png").toLocalFile();
        QVERIFY(QFile::remove(stored));
        document.setPlainText("![x](img/lost%2Dencoded.png)");
        backend.setModified(true);
        backend.save();
        QCOMPARE(bytes(saved), QByteArray("start"));
        QVERIFY(backend.status().contains("missing"));
    }
    void absoluteFilesystemReferencesStayAbsolute() {
        QTemporaryDir dir, destination;
        const QString image = dir.filePath("absolute.png");
        put(image, "existing image bytes");
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        const QString markdown = QString("![x](%1)").arg(image);
        document.setPlainText(markdown);
        QCOMPARE(backend.resolveImageUrl(image), QUrl::fromLocalFile(image));
        backend.setModified(true);
        const QString saved = destination.filePath("note.md");
        backend.saveAs(QUrl::fromLocalFile(saved));
        QCOMPARE(bytes(saved), markdown.toUtf8());
        backend.saveAs(QUrl::fromLocalFile(image));
        QCOMPARE(bytes(image), QByteArray("existing image bytes"));
        QVERIFY(backend.status().contains("overwrite"));
    }
    void pathsAndReferences() {
        QVERIFY(Attachments::safeRelativePath("img/a.png"));
        for (const QString &path : {"../a.png", "/a.png", "img/../a.png", "C:/a.png", "img\\a.png", "img//a.png"})
            QVERIFY2(!Attachments::safeRelativePath(path), qPrintable(path));
        const QString text = "![a [nested] label](img/a.png)\n![b](<img/a%20b(1).png> \"title\")\n"
                             "![c][id]\n![id][]\n![id]\n[id]: img/ref.png \"title\"\n"
                             "`![code](not.png)`\n```md\n![fence](no.png)\n```\n    ![indent](no.png)\n";
        const auto refs = Attachments::references(text);
        QCOMPARE(refs.size(), 4); // candidates include indented syntax; backend confirms with Qt
        QCOMPARE(refs[0].path, QString("img/a.png"));
        QCOMPARE(refs[1].path, QString("img/a%20b(1).png"));
        QCOMPARE(refs[2].path, QString("no.png"));
        QCOMPARE(refs[3].path, QString("img/ref.png"));
        QCOMPARE(Attachments::references("![x](img/photo(1).png)")[0].path, QString("img/photo(1).png"));
        QCOMPARE(Attachments::decodedPath("img/a\\(1\\)%20b.png"), QString("img/a(1) b.png"));
    }
    void printingFailuresAreVisibleWithoutChangingTheDocument() {
        Backend backend; QTextDocument document; bind(backend, document);
        document.setPlainText(QStringLiteral("untouched"));
        bool found = false;
        QTimer::singleShot(0, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) if (auto *box = qobject_cast<QMessageBox *>(widget)) {
                // macOS intentionally ignores QMessageBox window titles.
                found = box->isVisible() && box->icon() == QMessageBox::Critical
                    && box->text() == QStringLiteral("Synthetic failure") && box->textFormat() == Qt::PlainText;
                box->accept();
            }
        });
        backend.reportPrintFailure(QStringLiteral("Synthetic failure"));
        QVERIFY(found); QCOMPARE(document.toPlainText(), QStringLiteral("untouched"));
        QCOMPARE(backend.status(), QStringLiteral("Synthetic failure"));
    }
    void printingResolvesStagedRecoveredSavedAndSaveAsImagesWithoutMutation() {
        QTemporaryDir dir, destination;
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        backend.m_recoveryPath = dir.filePath("recovery.json");
        clipboardImage();
        const auto pasted = backend.pasteClipboardImage("img/print.png");
        const QString markdown = pasted + " " + pasted + "\n\n![ref][image]\n\n[image]: img/print.png\n";
        document.setPlainText(markdown); backend.setModified(true); backend.writeRecovery();
        const auto snapshot = bytes(backend.m_recoveryPath);
        const auto staged = backend.m_stagedAttachments;
        const bool undo = document.isUndoAvailable();
        const QPageLayout layout(QPageSize(QPageSize::A4), QPageLayout::Portrait, QMarginsF(15,15,15,15), QPageLayout::Millimeter);
        auto firstImage = [](Printing::Job &job) {
            if (!job.error.isEmpty() || job.sections.empty()) return QImage();
            auto &doc = *job.sections.front().document;
            for (auto block = doc.begin(); block.isValid(); block = block.next()) for (auto it = block.begin(); !it.atEnd(); ++it) {
                auto f = it.fragment().charFormat();
                if (f.isImageFormat()) return qvariant_cast<QImage>(doc.resource(QTextDocument::ImageResource, QUrl(f.toImageFormat().name())));
            }
            return QImage();
        };
        auto job = backend.printJob(layout);
        QVERIFY2(job.error.isEmpty(), qPrintable(job.error)); QCOMPARE(firstImage(job).pixelColor(0,0), QColor(Qt::red));
        QCOMPARE(document.toPlainText(), markdown); QCOMPARE(document.isUndoAvailable(), undo);
        QCOMPARE(backend.m_stagedAttachments, staged); QVERIFY(backend.m_materializedAttachments.isEmpty());
        QCOMPARE(bytes(backend.m_recoveryPath), snapshot); QVERIFY(!QFileInfo::exists(dir.filePath("img/print.png")));
        Backend restored; QTextDocument restoredDocument; bind(restored, restoredDocument);
        restored.m_recoveryPath = backend.m_recoveryPath; restored.restoreRecovery();
        auto recovered = restored.printJob(layout); QCOMPARE(firstImage(recovered).pixelColor(0,0), QColor(Qt::red));
        QCOMPARE(restoredDocument.toPlainText(), markdown); QCOMPARE(bytes(restored.m_recoveryPath), snapshot);
        backend.saveAs(QUrl::fromLocalFile(dir.filePath("note.md")));
        QImage changed(8,8,QImage::Format_RGB32); changed.fill(Qt::green); QVERIFY(changed.save(dir.filePath("img/print.png")));
        const auto savedBefore = bytes(dir.filePath("note.md"));
        auto saved = backend.printJob(layout); QCOMPARE(firstImage(saved).pixelColor(0,0), QColor(Qt::green));
        QCOMPARE(bytes(dir.filePath("note.md")), savedBefore); QCOMPARE(document.toPlainText(), markdown);
        backend.saveAs(QUrl::fromLocalFile(destination.filePath("moved.md")));
        Backend reopened; QTextDocument reopenedDocument; bind(reopened, reopenedDocument);
        reopened.open(QUrl::fromLocalFile(destination.filePath("moved.md")));
        auto moved = reopened.printJob(layout); QCOMPARE(firstImage(moved).pixelColor(0,0), QColor(Qt::green));
        QCOMPARE(reopenedDocument.toPlainText(), markdown);
    }
    void copyImageKeepsOriginalPixelsAndNeverChangesTheDocument() {
        QTemporaryDir dir;
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        QImage image(80, 2001, QImage::Format_ARGB32);
        image.fill(Qt::red);
        image.setPixelColor(79, 2000, Qt::blue);
        QGuiApplication::clipboard()->setImage(image);
        const QString markdown = backend.pasteClipboardImage("img/copy.png");
        QVERIFY(!markdown.isEmpty());
        document.setPlainText(markdown);
        const bool undo = document.isUndoAvailable();
        QGuiApplication::clipboard()->setText("sentinel");
        QVERIFY(backend.copyImage("img/copy.png"));
        QCOMPARE(QGuiApplication::clipboard()->image().size(), image.size());
        QCOMPARE(QGuiApplication::clipboard()->image().pixelColor(79, 2000), QColor(Qt::blue));
        QCOMPARE(document.toPlainText(), markdown);
        QCOMPARE(document.isUndoAvailable(), undo);
        backend.saveAs(QUrl::fromLocalFile(dir.filePath("note.md")));
        image.fill(Qt::green);
        QVERIFY(image.save(dir.filePath("img/copy.png")));
        QVERIFY(backend.copyImage("img/copy.png"));
        QCOMPARE(QGuiApplication::clipboard()->image().pixelColor(0, 0), QColor(Qt::green));
        QFile large(dir.filePath("large.png"));
        QVERIFY(large.open(QIODevice::WriteOnly));
        QVERIFY(large.resize(Attachments::MaxImageFileBytes + 1));
        large.close();
        for (const QString &path : {QString("https://invalid.example/a.png"), QString("data:image/png;base64,AAAA"),
                                   QString("../outside.png"), QString("missing.png"), QString("note.md"), QString("large.png")}) {
            QGuiApplication::clipboard()->setText("sentinel");
            QVERIFY(!backend.copyImage(path));
            QCOMPARE(QGuiApplication::clipboard()->text(), QString("sentinel"));
        }
        QCOMPARE(document.toPlainText(), markdown);
    }
    void clipboardAndRecovery() {
        QTemporaryDir dir;
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        backend.m_recoveryPath = dir.filePath("recovery.json");
        QGuiApplication::clipboard()->setText("not an image");
        QVERIFY(backend.pasteClipboardImage().isEmpty());
        clipboardImage();
        QVERIFY(backend.pasteClipboardImage("../evil.png").isEmpty());
        const QString markdown = backend.pasteClipboardImage("img/test.png");
        QCOMPARE(markdown, QString("![Screenshot](img/test.png)"));
        const QUrl staged = backend.resolveImageUrl("img/test.png");
        QVERIFY(staged.isLocalFile());
        QVERIFY(!bytes(staged.toLocalFile()).isEmpty());
        QVERIFY(backend.resolveImageUrl("https://example.com/a.png").isEmpty());
        QVERIFY(backend.resolveImageUrl("%2e%2e/evil.png").isEmpty());
        QVERIFY(backend.pasteClipboardImage("img/test.png").isEmpty());
        document.setPlainText(markdown);
        backend.setModified(true);
        backend.writeRecovery();
        Backend restored;
        QTextDocument restoredDocument;
        bind(restored, restoredDocument);
        restored.m_recoveryPath = backend.m_recoveryPath;
        restored.restoreRecovery();
        QCOMPARE(restoredDocument.toPlainText(), markdown);
        QCOMPARE(restored.resolveImageUrl("img/test.png"), staged);
        restored.saveAs(QUrl::fromLocalFile(dir.filePath("saved.md")));
        QCOMPARE(bytes(dir.filePath("saved.md")), markdown.toUtf8());
        QCOMPARE(bytes(dir.filePath("img/test.png")), bytes(staged.toLocalFile()));
        QVERIFY(!QFileInfo::exists(backend.m_recoveryPath));
    }
    void autoSaveAndSaveAs() {
        QTemporaryDir dir;
        const QString original = dir.filePath("original/note.md");
        put(original, "start");
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        backend.open(QUrl::fromLocalFile(original));
        clipboardImage();
        const QString markdown = backend.pasteClipboardImage("img/auto.png");
        document.setPlainText(markdown);
        backend.setModified(true);
        backend.m_autoSaveHeld = false;
        backend.autoSave();
        QCOMPARE(bytes(original), markdown.toUtf8());
        QVERIFY(QFileInfo::exists(dir.filePath("original/img/auto.png")));
        // Disk copies take precedence once committed; outside image edits are
        // not overwritten by stale recovery PNGs on the next Markdown save.
        put(dir.filePath("original/img/auto.png"), "external replacement");
        QCOMPARE(backend.resolveImageUrl("img/auto.png").toLocalFile(), dir.filePath("original/img/auto.png"));
        backend.save();
        const QUrl originalUrl = backend.fileUrl();
        backend.saveAs(QUrl::fromLocalFile(dir.filePath("original/img/auto.png")));
        QCOMPARE(backend.fileUrl(), originalUrl);
        QCOMPARE(bytes(dir.filePath("original/img/auto.png")), QByteArray("external replacement"));
        QDir().mkpath(dir.filePath("destination"));
        const QString dest = dir.filePath("destination/note.md");
        backend.saveAs(QUrl::fromLocalFile(dest));
        QCOMPARE(bytes(dest), markdown.toUtf8());
        QCOMPARE(bytes(dir.filePath("destination/img/auto.png")), QByteArray("external replacement"));
        QDir().mkpath(dir.filePath("collision"));
        put(dir.filePath("collision/img/auto.png"), "other image");
        backend.saveAs(QUrl::fromLocalFile(dir.filePath("collision/note.md")));
        QVERIFY(!QFileInfo::exists(dir.filePath("collision/note.md")));
        QCOMPARE(bytes(dir.filePath("collision/img/auto.png")), QByteArray("other image"));
        QCOMPARE(backend.fileUrl().toLocalFile(), dest);
    }
    void saveAsExistingImagesAndMissingRecovery() {
        QTemporaryDir dir;
        QDir().mkpath(dir.filePath("old"));
        QDir().mkpath(dir.filePath("new"));
        put(dir.filePath("old/img/a (1).png"), "image bytes");
        const QString markdown = "- list\n    - ![nested](<img/a%20(1).png>)\n\n<img src=\"img/a%20(1).png\">\n";
        put(dir.filePath("old/doc.md"), markdown.toUtf8());
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        backend.open(QUrl::fromLocalFile(dir.filePath("old/doc.md")));
        backend.saveAs(QUrl::fromLocalFile(dir.filePath("new/doc.md")));
        QCOMPARE(bytes(dir.filePath("new/doc.md")), markdown.toUtf8());
        QCOMPARE(bytes(dir.filePath("new/img/a (1).png")), QByteArray("image bytes"));
        clipboardImage();
        const QString pasted = backend.pasteClipboardImage("img/missing.png");
        const QString stagedPath = backend.resolveImageUrl("img/missing.png").toLocalFile();
        QVERIFY(QFile::remove(stagedPath));
        document.setPlainText(pasted);
        backend.setModified(true);
        backend.m_recoveryPath = dir.filePath("snapshot.json");
        backend.writeRecovery();
        const QByteArray oldMarkdown = bytes(dir.filePath("new/doc.md"));
        backend.save();
        QCOMPARE(bytes(dir.filePath("new/doc.md")), oldMarkdown);
        QVERIFY(QFileInfo::exists(backend.m_recoveryPath));
        QVERIFY(backend.locateAttachment("img/missing.png", QUrl::fromLocalFile(dir.filePath("old/img/a (1).png")), "img/found.png"));
        backend.save();
        QCOMPARE(bytes(dir.filePath("new/doc.md")), document.toPlainText().toUtf8());
        QCOMPARE(bytes(dir.filePath("new/img/found.png")), QByteArray("image bytes"));
    }
    void renameUndoAndLocate() {
        QTemporaryDir dir;
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        clipboardImage();
        backend.pasteClipboardImage("img/old.png");
        document.setPlainText("![a](img/old.png)\n![b][ref]\n\n[ref]: img/old.png\n\n`![code](img/old.png)`\n\n"
                              "    ![indented code](img/old.png)\n\n- list\n    - ![nested](img/old.png)\n\n"
                              "> ```\n> ![quoted code](img/old.png)\n> ```\n");
        const QString original = document.toPlainText();
        QVERIFY(backend.renameAttachment("img/old.png", "img/new.png"));
        QVERIFY(document.toPlainText().contains("![a](img/new.png)"));
        QVERIFY(document.toPlainText().contains("[ref]: img/new.png"));
        QVERIFY(document.toPlainText().contains("`![code](img/old.png)`"));
        QVERIFY(document.toPlainText().contains("    ![indented code](img/old.png)"));
        QVERIFY(document.toPlainText().contains("    - ![nested](img/new.png)"));
        QVERIFY(document.toPlainText().contains("> ![quoted code](img/old.png)"));
        document.undo();
        QCOMPARE(document.toPlainText(), original);
        QVERIFY(backend.resolveImageUrl("img/new.png").isLocalFile());
        QVERIFY(backend.resolveImageUrl("img/old.png").isLocalFile());
        put(dir.filePath("found.png"), "replacement");
        document.setPlainText("![missing](img/lost.png)");
        QVERIFY(backend.locateAttachment("img/lost.png", QUrl::fromLocalFile(dir.filePath("found.png")), "img/found.png"));
        QCOMPARE(document.toPlainText(), QString("![missing](img/found.png)"));
        document.undo();
        QCOMPARE(document.toPlainText(), QString("![missing](img/lost.png)"));
    }
    void diskRenameSaveAsUndo() {
        QTemporaryDir dir;
        const QString markdown = "![old](img/old.png)";
        put(dir.filePath("old/doc.md"), markdown.toUtf8());
        put(dir.filePath("old/img/old.png"), "original image");
        QDir().mkpath(dir.filePath("new"));
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        backend.open(QUrl::fromLocalFile(dir.filePath("old/doc.md")));
        QVERIFY(backend.renameAttachment("img/old.png", "img/new.png"));
        backend.saveAs(QUrl::fromLocalFile(dir.filePath("new/doc.md")));
        QCOMPARE(bytes(dir.filePath("new/img/new.png")), QByteArray("original image"));
        QCOMPARE(bytes(dir.filePath("new/img/old.png")), QByteArray("original image"));
        document.undo();
        QCOMPARE(document.toPlainText(), markdown);
        QCOMPARE(backend.resolveImageUrl("img/old.png").toLocalFile(), dir.filePath("new/img/old.png"));
        backend.save();
        QCOMPARE(bytes(dir.filePath("new/doc.md")), markdown.toUtf8());
    }
    void destinationCollisionAndCopyFailures() {
        QTemporaryDir dir;
        Backend backend;
        QTextDocument document;
        bind(backend, document);
        clipboardImage();
        document.setPlainText(backend.pasteClipboardImage("same.png"));
        backend.saveAs(QUrl::fromLocalFile(dir.filePath("same.png")));
        QVERIFY(!QFileInfo::exists(dir.filePath("same.png")));
        QVERIFY(backend.fileUrl().isEmpty());
        backend.saveAs(QUrl::fromLocalFile(dir.filePath("SAME.PNG")));
        QVERIFY(!QFileInfo::exists(dir.filePath("SAME.PNG")));
        QVERIFY(backend.fileUrl().isEmpty());
        const QString source = dir.filePath("source");
        put(source, "image");
        QString error;
        QVERIFY(!Attachments::copyInto(source, dir.path(), "../outside", &error));
        put(dir.filePath("blocked"), "not a directory");
        QVERIFY(!Attachments::copyInto(source, dir.path(), "blocked/image.png", &error));
        QDir().mkpath(dir.filePath("outside"));
        QVERIFY(QFile::link(dir.filePath("outside"), dir.filePath("link")));
        QVERIFY(!Attachments::copyInto(source, dir.path(), "link/image.png", &error));
        QVERIFY(!QFileInfo::exists(dir.filePath("outside/image.png")));
    }
};
QTEST_MAIN(OmawriteAttachmentsTest)
#include "tst_attachments.moc"

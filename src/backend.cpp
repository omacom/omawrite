#include "backend.h"
#include "spellingcontroller.h"
#include "attachmenthelper.h"
#include "printdocument.h"
#include <QCheckBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QMessageBox>

#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif
#include <cmath>
#include <QtConcurrent/QtConcurrentRun>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QBuffer>
#include <QTextImageFormat>
#include <QUuid>
#include <QMimeData>
#include <QProcess>
#include <QPrintDialog>
#include <QPrinter>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include "profile.h"
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLockFile>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextLayout>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "markdownhighlighter.h"

constexpr qreal typoraLineHeightPercent = 140;

static QTextBlockFormat lineHeightFormat() {
    QTextBlockFormat format;
    format.setLineHeight(typoraLineHeightPercent, QTextBlockFormat::ProportionalHeight);
    return format;
}
const QString lastSaveDirectorySetting = QStringLiteral("file/lastSaveDirectory");
const QString textZoomSetting = QStringLiteral("text/zoom");

// The user's own text size, multiplied onto the desktop's text scale.
// Browser-sized steps, with a wider range for small and large displays.
constexpr qreal textZoomSteps[] = {0.5, 0.67, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
constexpr int textZoomStepCount = int(std::size(textZoomSteps));

const QString editorFontSetting = QStringLiteral("editor/font");

// Noto ships a separate family per script, which buries every other font in
// the picker. Offer only the core Latin families; the rest still serve as
// fallbacks for whatever scripts a document contains.
const QStringList keptNotoFamilies = {QStringLiteral("Noto Sans"),
                                      QStringLiteral("Noto Sans Mono"),
                                      QStringLiteral("Noto Serif")};

// Fontconfig's generic aliases just point at another listed family.
const QStringList genericFontAliases = {QStringLiteral("Monospace"), QStringLiteral("Mono"),
                                        QStringLiteral("Sans Serif"), QStringLiteral("Sans"),
                                        QStringLiteral("Serif"), QStringLiteral("System-ui")};

// Symbol fonts often claim Latin coverage because they map their glyphs onto
// ASCII, so they can only be told apart by name. D050000L is URW's Dingbats.
const QRegularExpression symbolFontName(
    QStringLiteral("symbol|dingbat|emoji|icon|awesome|^D050000L$"),
    QRegularExpression::CaseInsensitiveOption);

namespace {

// A shade of `from` carried part of the way towards `to`.
QColor blend(const QColor &from, const QColor &to, qreal amount) {
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

}

QString Backend::normalizedLinkUrl(const QString &clipboardText) {
    QString candidate = clipboardText.trimmed();
    static const QRegularExpression lineBreakRe(QStringLiteral("[\\r\\n]"));
    const int lineBreak = candidate.indexOf(lineBreakRe);
    if (lineBreak >= 0)
        candidate = candidate.left(lineBreak).trimmed();

    if (candidate.isEmpty())
        return {};

    if (candidate.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
        candidate.prepend(QStringLiteral("https://"));

    static const QRegularExpression schemeRe(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    if (!schemeRe.match(candidate).hasMatch())
        return {};

    const QUrl url(candidate);
    if (!url.isValid() || url.scheme().isEmpty())
        return {};

    const QString scheme = url.scheme().toLower();
    const bool webUrl = scheme == QStringLiteral("http")
        || scheme == QStringLiteral("https")
        || scheme == QStringLiteral("ftp");
    if (webUrl && url.host().isEmpty())
        return {};

    if (!webUrl && scheme != QStringLiteral("mailto"))
        return {};

    return url.toString();
}

Backend::Backend(QObject *parent, bool recoverDraft)
    : Backend(std::unique_ptr<SpellingDictionary>(), parent, recoverDraft) {}

Backend::Backend(std::unique_ptr<SpellingDictionary> dictionary, QObject *parent, bool recoverDraft)
    : QObject(parent), m_recoverDraft(recoverDraft) {
    m_themeHome = QDir::homePath();
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &Backend::clipboardChanged);
    m_spelling = dictionary ? new SpellingController(std::move(dictionary), this)
                            : new SpellingController(this);
    connect(m_spelling, &SpellingController::rangesChanged, this, [this] {
        if (!m_highlighter)
            return;
        QList<MarkdownHighlighter::Span> spans;
        for (const auto &range : m_spelling->ranges())
            spans.append({range.start, range.length});
        m_highlighter->setSpellingRanges(spans);
    });
    connect(m_spelling, &SpellingController::operationFailed, this, &Backend::setStatus);
    // A settings file truncated by a crash reads back as an empty value, and
    // both that and a NaN would clamp to the smallest size rather than fall
    // back to the default.
    bool zoomOk = false;
    const qreal storedZoom = QSettings().value(textZoomSetting, 1.0).toDouble(&zoomOk);
    m_textZoom = zoomOk && storedZoom > 0 ? qBound(textZoomSteps[0], storedZoom,
                                                   textZoomSteps[textZoomStepCount - 1])
                                          : 1.0;

    const QString savedFont = QSettings().value(editorFontSetting).toString();
    m_editorFont = !savedFont.isEmpty() && QFontDatabase::hasFamily(savedFont)
        ? savedFont
        : defaultEditorFont();

    const QString stateDirectory = Profile::dataLocation();
    QDir().mkpath(stateDirectory);
    // Claim an orphaned snapshot before taking an empty slot. This ensures a
    // crash in window 2 is still recovered even if window 1 exited normally.
    for (int pass = recoverDraft ? 0 : 1; pass < 2 && !m_recoveryLock; ++pass) {
        for (int slot = 0; slot < 100; ++slot) {
            const QString base = QDir(stateDirectory).filePath(
                QStringLiteral("recovery-%1").arg(slot));
            const bool snapshotExists = QFileInfo::exists(base + QStringLiteral(".json"));
            if ((pass == 0) != snapshotExists)
                continue;
            auto lock = std::make_unique<QLockFile>(base + QStringLiteral(".lock"));
            if (lock->tryLock()) {
                m_recoveryPath = base + QStringLiteral(".json");
                m_recoveryLock = std::move(lock);
                break;
            }
        }
    }
    connect(&m_selectionCountWatcher, &QFutureWatcher<int>::finished, this, [this] {
        m_selectionCounting = false;
        if (m_selectionJobGeneration == m_selectionGeneration) {
            const int count = m_selectionCountWatcher.result();
            if (count != m_selectionWordCount) {
                m_selectionWordCount = count;
                emit selectionWordCountChanged();
            }
        } else if (!m_selectionText.isEmpty()) {
            refreshSelectionWordCount();
        }
    });
    m_wordCountTimer.setSingleShot(true);
    m_wordCountTimer.setInterval(120);
    connect(&m_wordCountTimer, &QTimer::timeout, this, &Backend::refreshWordCount);
    connect(&m_wordCountWatcher, &QFutureWatcher<int>::finished, this, [this] {
        if (!m_wordCountStale)
            setWordCount(m_wordCountWatcher.result());
        if (m_wordCountAgain) {
            m_wordCountAgain = false;
            refreshWordCount();
        }
    });
    m_recoveryTimer.setSingleShot(true);
    m_recoveryTimer.setInterval(750);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &Backend::writeRecovery);
    m_autoSaveTimer.setSingleShot(true);
    m_autoSaveTimer.setInterval(500);
    connect(&m_autoSaveTimer, &QTimer::timeout, this, &Backend::autoSave);
    // Tools that replace a file by removing it and writing it again leave it
    // briefly missing, so let the burst settle before deciding what happened.
    m_fileCheckTimer.setSingleShot(true);
    m_fileCheckTimer.setInterval(100);
    connect(&m_fileCheckTimer, &QTimer::timeout, this, &Backend::checkWatchedFile);
    // Continuous activity beside the document must not postpone its check
    // indefinitely. Coalesce a burst without restarting the pending deadline.
    const auto scheduleFileCheck = [this] {
        if (!m_fileCheckTimer.isActive())
            m_fileCheckTimer.start();
    };
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, this, scheduleFileCheck);
    connect(&m_fileWatcher, &QFileSystemWatcher::directoryChanged, this, scheduleFileCheck);

    loadOmarchyTheme();
    watchOmarchyTheme();
    // Switching themes touches several watched paths in a row, and the theme
    // folder can be briefly missing, so reload once the burst has settled.
    m_themeReloadTimer.setSingleShot(true);
    m_themeReloadTimer.setInterval(100);
    connect(&m_themeReloadTimer, &QTimer::timeout, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, &m_themeReloadTimer,
            qOverload<>(&QTimer::start));
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, &m_themeReloadTimer,
            qOverload<>(&QTimer::start));
}

Backend::~Backend() = default;

void Backend::setParentWindow(QWindow *window) {
    m_parentWindow = window;
}

// The name an untitled document goes by, which a save suggests too.
QString Backend::untitledName() {
    return tr("Untitled") + QStringLiteral(".md");
}

void Backend::retranslate() {
    emit fileUrlChanged();
}

QString Backend::fileName() const {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty())
        return untitledName();

    if (m_fileUrl.isLocalFile()) {
        const QFileInfo info(m_fileUrl.toLocalFile());
        if (!info.fileName().isEmpty())
            return info.fileName();
    }

    const QString name = m_fileUrl.fileName();
    return name.isEmpty() ? untitledName() : name;
}

void Backend::setDarkMode(bool darkMode) {
    m_systemDarkMode = darkMode;
    loadOmarchyTheme();
}

void Backend::setDesktopTextScale(qreal textScale) {
    if (qFuzzyCompare(m_desktopTextScale, textScale))
        return;

    m_desktopTextScale = textScale;
    emit textScaleChanged();
}

void Backend::increaseTextSize() {
    setTextZoom(steppedTextZoom(1));
}

void Backend::decreaseTextSize() {
    setTextZoom(steppedTextZoom(-1));
}

void Backend::resetTextSize() {
    setTextZoom(1.0);
}

// The neighbouring rung, starting from whichever rung the current zoom sits
// closest to; the ends of the ladder hold.
qreal Backend::steppedTextZoom(int direction) const {
    int closest = 0;
    for (int step = 1; step < textZoomStepCount; ++step) {
        if (qAbs(textZoomSteps[step] - m_textZoom) < qAbs(textZoomSteps[closest] - m_textZoom))
            closest = step;
    }

    return textZoomSteps[qBound(0, closest + direction, textZoomStepCount - 1)];
}

void Backend::setTextZoom(qreal zoom) {
    if (qFuzzyCompare(m_textZoom, zoom))
        return;

    m_textZoom = zoom;
    QSettings().setValue(textZoomSetting, zoom);
    emit textScaleChanged();
}

QObject *Backend::spelling() const { return m_spelling; }

bool Backend::replaceSpelling(const QVariantMap &token, const QString &replacement) {
    if (!m_spelling->validate(token) || replacement.isEmpty())
        return false;
    const int start = token.value(QStringLiteral("start")).toInt();
    replaceRange(start, start + token.value(QStringLiteral("length")).toInt(), replacement);
    return true;
}

void Backend::attachDocument(QObject *textDocument) {
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    if (!quickDocument || !quickDocument->textDocument()) {
        setStatus(tr("Could not attach the Markdown renderer."));
        return;
    }

    if (m_highlighter)
        delete m_highlighter.data();

    // Qt Quick draws only the visible part of a long text, but turns that on
    // only for text set through TextEdit.text, never through its document.
    if (auto *editor = qobject_cast<QQuickItem *>(quickDocument->parent()))
        editor->setFlag(QQuickItem::ItemObservesViewport);

    m_document = quickDocument->textDocument();
    m_lastDocumentText = m_document->toPlainText();
    m_highlighter = new MarkdownHighlighter(m_document);
    m_highlighter->setTheme(m_darkMode, m_themeBackground, m_themeForeground, m_themeAccent,
                            m_themePanelBackground);
    m_highlighter->setCaret(m_caretPosition);
    m_spelling->attach(m_document);

    connect(m_document, &QTextDocument::contentsChange, this,
            [this](int position, int, int charsAdded) {
                if (m_formattingTypography || m_loading)
                    return;
                m_lastChangePos = position;
                m_lastChangeAdded = charsAdded;
            });

    applyDocumentTypography();
    if (m_recoverDraft) restoreRecovery();
}

void Backend::openDialog() {
    emit openDialogRequested();
}

void Backend::open(const QUrl &url) {
    if (m_openRouter && !m_bypassRouting) { m_openRouter(url); return; }
    if (!url.isLocalFile()) {
        setStatus(tr("Only local files can be opened."));
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(tr("Could not open %1.").arg(targetName));
        return;
    }

    const QByteArray raw = file.readAll();
    // Text mode drops carriage returns, matching how the file is compared
    // and saved later.
    file.setTextModeEnabled(true);
    file.seek(0);
    const QByteArray contents = file.readAll();
    emit documentAboutToReplace();
    loadDocumentText(QString::fromUtf8(contents));
    m_stagedAttachments.clear();
    m_materializedAttachments.clear();
    emit attachmentUrlsChanged();
    clearRecovery();
    setKnownFileContents(contents, true);
    m_autoSaveSuspended = false;
    setFileUrl(url);
    watchCurrentFile();
    setModified(false);
    if (savesUnchanged(raw)) {
        setStatus(tr("Opened %1").arg(fileName()));
    } else {
        // Saving writes UTF-8 with plain line endings, which would change
        // bytes this file holds. Leave that to an explicit save.
        suspendAutoSave();
        setStatus(tr("Opened %1. Auto-save is off to keep its encoding and line endings.")
                      .arg(fileName()));
    }
    emit documentLoaded();
}

void Backend::save() {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty()) {
        saveAsDialog();
        return;
    }

    saveTo(m_fileUrl);
}

void Backend::saveAsDialog() {
    emit saveDialogRequested(suggestedSaveUrl());
}

void Backend::saveAs(const QUrl &url) {
    saveTo(url);
}

void Backend::discardRecovery() {
    clearRecovery();
    // Discarded changes must never reach the file, even if the open or close
    // that follows fails and leaves them in the editor. Opening a file or an
    // explicit save lifts this.
    suspendAutoSave();
}

// A file named on the command line need not exist yet: `omawrite new.md`
// starts a blank document that the first save writes to that path. Opening
// from the file picker or reloading still requires the file to be there.
void Backend::openOrStartFile(const QUrl &requestedUrl) {
    QScopedValueRollback<bool> bypass(m_bypassRouting, true);
    // A relative path names a file in the directory Write was started
    // from; a recovery draft must not resolve it against another one later.
    const QFileInfo info(requestedUrl.toLocalFile());
    const QUrl url = requestedUrl.isLocalFile()
        ? QUrl::fromLocalFile(info.absoluteFilePath()) : requestedUrl;
    if (m_modified) {
        // A recovered draft fills this window; opening over it would lose it.
        setStatus(tr("Recovered unsaved changes; %1 was not opened.")
                      .arg(info.fileName()));
        return;
    }
    if (!url.isLocalFile() || info.exists() || !info.absoluteDir().exists()) {
        open(url);
        return;
    }

    emit documentAboutToReplace();
    loadDocumentText(QString());
    m_stagedAttachments.clear();
    m_materializedAttachments.clear();
    emit attachmentUrlsChanged();
    clearRecovery();
    // Nothing has been read, so no text counts as saved, and auto-save holds
    // off until an explicit save creates the file (see autoSave()).
    setKnownFileContents(QByteArray(), false);
    setFileUrl(url);
    watchCurrentFile();
    setModified(false);
    setStatus(tr("New file %1").arg(fileName()));
    emit documentLoaded();
}

void Backend::reloadFromDisk() {
    QScopedValueRollback<bool> bypass(m_bypassRouting, true);
    if (m_fileUrl.isLocalFile())
        open(m_fileUrl);
}

void Backend::keepExternalVersion() {
    QFile file(m_fileUrl.toLocalFile());
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray raw = file.readAll();
        file.setTextModeEnabled(true);
        file.seek(0);
        setKnownFileContents(file.readAll(), true);
        // Keeping our text over an edited file is a deliberate choice to
        // overwrite it, so auto-save may resume, unless saving would change
        // the file's encoding or line endings. A removed file stays removed
        // until the writer saves it again.
        m_autoSaveSuspended = !savesUnchanged(raw);
    } else {
        setKnownFileContents(QByteArray(), false);
    }
    setModified(true);
    scheduleRecovery();
    scheduleAutoSave();
    watchCurrentFile();
    setStatus(tr("Kept your version"));
}

QFont Backend::printFont(const QFont &editorFont, qreal screenDpi) {
    // The editor sizes its font in pixels, which a printer takes as device dots.
    // Convert to points at the screen's DPI so the page matches the editor.
    QFont font = editorFont;
    if (font.pixelSize() <= 0)
        return font;

    const qreal dpi = screenDpi > 0.0 ? screenDpi : 96.0;
    font.setPointSizeF(font.pixelSize() * 72.0 / dpi);
    return font;
}

Printing::Job Backend::printJob(const QPageLayout &layout, bool appendices) const {
    if (!m_document) { Printing::Job job; job.error = QStringLiteral("There is no document to print."); return job; }
    Printing::Options options;
    options.pageLayout = layout;
    options.font = QFont(m_editorFont, 11); // Physical print size, independent of editor zoom/theme.
    options.wideTablesToAppendix = appendices;
    return Printing::build(currentDocumentText(), options, [this](const QString &path) { return resolveImageUrl(path); });
}

void Backend::reportPrintFailure(const QString &message) {
    setStatus(message);
    QMessageBox error(QMessageBox::Critical, QStringLiteral("Print failed"), message, QMessageBox::Ok);
    error.setTextFormat(Qt::PlainText);
    error.winId();
    if (error.windowHandle() && m_parentWindow) error.windowHandle()->setTransientParent(m_parentWindow);
    error.exec();
}

void Backend::printDocument() {
    if (!m_document) { reportPrintFailure(tr("There is no document to print.")); return; }
    QDialog settings;
    settings.setWindowTitle(QStringLiteral("Print layout"));
    QVBoxLayout layout(&settings);
    QCheckBox appendices(QStringLiteral("Move wide tables to appendix"));
    appendices.setChecked(QSettings().value(QStringLiteral("print/wideTablesToAppendix"), true).toBool());
    QLabel explanation(QStringLiteral("Long, narrow tables stay inline. Wide tables move to landscape\nappendices; extra-wide tables repeat column 1 across column groups."));
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons.button(QDialogButtonBox::Ok)->setText(QStringLiteral("Continue…"));
    layout.addWidget(&appendices); layout.addWidget(&explanation); layout.addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &settings, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &settings, &QDialog::reject);
    settings.winId();
    if (settings.windowHandle() && m_parentWindow) settings.windowHandle()->setTransientParent(m_parentWindow);
    if (settings.exec() != QDialog::Accepted) return;

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(fileName());
    printer.setPageMargins(QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);
    QPrintDialog dialog(&printer);
    dialog.setWindowTitle(tr("Print %1").arg(fileName()));
    dialog.setMinMax(1, 2000);
    dialog.setOption(QAbstractPrintDialog::PrintSelection, false);
    dialog.setOption(QAbstractPrintDialog::PrintCurrentPage, false);
    dialog.winId();
    if (dialog.windowHandle() && m_parentWindow) dialog.windowHandle()->setTransientParent(m_parentWindow);
    if (dialog.exec() != QDialog::Accepted) return;
    auto job = printJob(printer.pageLayout(), appendices.isChecked());
    if (!job.error.isEmpty()) { reportPrintFailure(job.error); return; }
    if (!job.warnings.isEmpty() && QMessageBox::warning(nullptr, QStringLiteral("Print images"),
        job.warnings.join(QLatin1Char('\n')) + QStringLiteral("\nPrint with explicit placeholders?"),
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) return;
    QSettings().setValue(QStringLiteral("print/wideTablesToAppendix"), appendices.isChecked());
    QString failure;
    if (!job.print(printer, &failure)) reportPrintFailure(failure);
    else setStatus(QStringLiteral("Print job sent."));
}

void Backend::newWindow() { emit newWindowRequested(); }

void Backend::setOpenRouter(std::function<void(const QUrl &)> router) {
    m_openRouter = std::move(router);
    emit routesFileOpenChanged();
}

bool Backend::hasRecoverySnapshot() const { return QFileInfo::exists(m_recoveryPath); }

void Backend::finishClosing() {
    m_autoSaveHeld = true;
    m_autoSaveTimer.stop();
    m_fileCheckTimer.stop();
    clearRecovery();
}

void Backend::copyAsHtml(int start, int end, QObject *renderedDocument, bool plainMarkup) {
    QTextDocument rendered;
    const bool selected = start >= 0 && end > start;
    auto *quick = qobject_cast<QQuickTextDocument *>(renderedDocument);
    auto *view = qobject_cast<QTextDocument *>(renderedDocument);
    if (!view && quick) view = quick->textDocument();
    if (selected && view) {
        QTextCursor selection(view);
        selection.setPosition(qBound(0, start, view->characterCount() - 1));
        selection.setPosition(qBound(0, end, view->characterCount() - 1), QTextCursor::KeepAnchor);
        QTextCursor(&rendered).insertFragment(selection.selection());
        // Snapshot ranges before changing formats (adjacent fragments may merge).
        QList<QTextLayout::FormatRange> formats;
        for (QTextBlock block = rendered.begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (!fragment.isValid()) continue;
                auto format = fragment.charFormat();
                if (format.isImageFormat()) {
                    auto imageFormat = format.toImageFormat();
                    const auto image = qvariant_cast<QImage>(view->resource(
                        QTextDocument::ImageResource, QUrl(imageFormat.name())));
                    QByteArray png;
                    QBuffer buffer(&png);
                    buffer.open(QIODevice::WriteOnly);
                    if (!image.isNull() && image.save(&buffer, "PNG")) {
                        imageFormat.setName(QStringLiteral("data:image/png;base64,")
                                            + QString::fromLatin1(png.toBase64()));
                        format = imageFormat;
                    } else if (QUrl(imageFormat.name()).scheme() == QStringLiteral("preview")) {
                        format.setObjectType(QTextFormat::NoObject); // Never export private resource URLs.
                    }
                }
                for (int property : {QTextFormat::ForegroundBrush, QTextFormat::BackgroundBrush,
                         QTextFormat::FontFamilies, QTextFormat::FontPointSize, QTextFormat::FontPixelSize})
                    format.clearProperty(property);
                formats.append({fragment.position(), fragment.length(), format});
            }
        }
        for (const auto &range : formats) {
            QTextCursor cursor(&rendered);
            cursor.setPosition(range.start);
            cursor.setPosition(range.start + range.length, QTextCursor::KeepAnchor);
            cursor.setCharFormat(range.format);
        }
    } else {
        QString markdown = currentDocumentText();
        if (selected) markdown = markdown.mid(start, end - start);
        rendered.setMarkdown(markdown);
    }

    // Drop Qt's fixed fonts and margins from the body and blocks, so the pasted
    // text takes on the destination's own typeface and paragraph spacing.
    QString html = rendered.toHtml();
    static const QRegularExpression blockStyle(
        QStringLiteral("<(body|p|h[1-6]|ul|ol|li)\\s+style=\"[^\"]*\""));
    html.replace(blockStyle, QStringLiteral("<\\1"));

    auto *mimeData = new QMimeData;
    mimeData->setHtml(html);
    // Rich destinations use text/html; code/plain-text destinations must get
    // the exported markup too, not silently fall back to rendered words.
    mimeData->setText(plainMarkup ? html : rendered.toPlainText());
    QGuiApplication::clipboard()->setMimeData(mimeData);
    if (plainMarkup) setStatus(tr("Copied as HTML."));
}

QString Backend::attachmentStoragePath() const {
    return QDir(Profile::dataLocation())
        .filePath(QStringLiteral("attachments"));
}

QString Backend::pasteClipboardImage(const QString &proposedRelativePath) {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime || !mime->hasImage()) return QString();
    const QImage image = qvariant_cast<QImage>(mime->imageData());
    if (image.isNull()) {
        setStatus(QStringLiteral("Could not read the clipboard image."));
        return QString();
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString path = proposedRelativePath.isEmpty()
        ? QStringLiteral("img/screenshot-%1.png").arg(id) : proposedRelativePath;
    if (!Attachments::safeRelativePath(path) || !path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)
        || m_stagedAttachments.contains(path)
        || (m_fileUrl.isLocalFile() && QFileInfo::exists(QFileInfo(m_fileUrl.toLocalFile()).dir().filePath(path)))) {
        setStatus(QStringLiteral("Choose an unused relative PNG attachment path (for example img/screenshot.png)."));
        return QString();
    }
    const QString root = attachmentStoragePath();
    if (!QDir().mkpath(root)) {
        setStatus(QStringLiteral("Could not create attachment recovery storage."));
        return QString();
    }
    // QSaveFile never exposes a partially encoded PNG to recovery or preview.
    const QString storageId = id + QStringLiteral(".png");
    QSaveFile file(QDir(root).filePath(storageId));
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit()) {
        setStatus(QStringLiteral("Could not store the clipboard image."));
        return QString();
    }
    m_stagedAttachments.insert(path, storageId);
    emit attachmentUrlsChanged();
    return QStringLiteral("![Screenshot](%1)").arg(path);
}

QUrl Backend::resolveImageUrl(const QString &path) const {
    // Never hand a remote URL to the preview. Decode before validating so an
    // encoded traversal or separator cannot bypass the relative-path policy.
    const QUrl url(path);
    QString local;
    if (url.isLocalFile()) {
        if (!url.host().isEmpty()) return QUrl();
        local = url.toLocalFile();
    } else {
        if (!url.isRelative() || !url.host().isEmpty() || url.hasQuery() || url.hasFragment()) return QUrl();
        const QString relative = Attachments::decodedPath(path);
        if (QDir::isAbsolutePath(relative)) {
            local = relative;
        } else {
            if (!Attachments::safeLocalPath(relative)) return QUrl();
            if (m_stagedAttachments.contains(relative) && !m_materializedAttachments.contains(relative))
                local = QDir(attachmentStoragePath()).filePath(m_stagedAttachments.value(relative));
            else if (m_fileUrl.isLocalFile())
                local = QFileInfo(m_fileUrl.toLocalFile()).dir().filePath(relative);
        }
    }
    return QFileInfo(local).isFile() ? QUrl::fromLocalFile(QFileInfo(local).absoluteFilePath()) : QUrl();
}

bool Backend::copyImage(const QString &path) {
    const QImage image = Attachments::loadLocalImage(resolveImageUrl(path));
    if (image.isNull()) {
        setStatus(QStringLiteral("Could not copy the image. Only readable local images within the safety limits are supported."));
        return false;
    }
    QGuiApplication::clipboard()->setImage(image);
    setStatus(QStringLiteral("Copied image."));
    return true;
}

bool Backend::materializeAttachments(const QUrl &destination, QString *error) const {
    const QString root = QFileInfo(destination.toLocalFile()).absolutePath();
    const bool moving = !m_fileUrl.isLocalFile()
        || QFileInfo(m_fileUrl.toLocalFile()).absolutePath() != root;
    QMap<QString, QString> paths;
    const QString documentPath = QFileInfo(destination.toLocalFile()).absoluteFilePath();
    const auto collidesWithDocument = [&documentPath](const QString &path) {
        if (path.isEmpty()) return false;
        const QFileInfo candidate(path), document(documentPath);
        return QDir::cleanPath(candidate.absoluteFilePath()).compare(QDir::cleanPath(documentPath), Qt::CaseInsensitive) == 0
            || (!candidate.canonicalFilePath().isEmpty()
                && candidate.canonicalFilePath() == document.canonicalFilePath());
    };
    const QStringList referencedImages = Attachments::imagePaths(currentDocumentText());
    QSet<QString> referencedPaths;
    for (const QString &reference : referencedImages)
        referencedPaths.insert(QDir::cleanPath(Attachments::decodedPath(reference)));
    QStringList dependencies = referencedImages;
    dependencies.append(m_stagedAttachments.keys());
    // Collision protection is independent of whether an image needs copying:
    // same-folder Save As must not replace an already-materialized image, nor
    // may a new Markdown target overwrite an image in the original folder.
    for (const QString &reference : dependencies) {
        const QUrl url(reference);
        if (!url.isLocalFile() && !url.isRelative()) continue;
        const QString path = Attachments::decodedPath(reference);
        QString target;
        QString source;
        if (url.isLocalFile() && url.host().isEmpty()) {
            source = url.toLocalFile();
        } else if (url.isRelative() && url.host().isEmpty() && QDir::isAbsolutePath(path)) {
            source = path;
        } else if (url.isRelative() && Attachments::safeLocalPath(path)) {
            target = QDir(root).filePath(path);
            if (m_fileUrl.isLocalFile()) source = QFileInfo(m_fileUrl.toLocalFile()).dir().filePath(path);
        }
        if (collidesWithDocument(target) || collidesWithDocument(source)) {
            *error = QStringLiteral("The document destination would overwrite an attachment: %1").arg(reference);
            return false;
        }
    }
    if (moving) {
        for (const QString &reference : referencedImages) {
            const QUrl url(reference);
            const QString path = Attachments::decodedPath(reference);
            if (!url.isRelative() || QDir::isAbsolutePath(path))
                continue; // absolute filesystem paths and file: URLs stay unchanged
            if (!Attachments::safeLocalPath(path) || url.hasQuery() || url.hasFragment()) {
                *error = QStringLiteral("Cannot safely copy attachment path: %1").arg(reference);
                return false;
            }
            paths.insert(path, reference);
        }
    }
    // Also preserve staged images no longer referenced after undo, or whose
    // syntax the conservative source scanner cannot recognize. No image is
    // deleted by undo, and a later redo remains valid after Save As.
    for (auto it = m_stagedAttachments.cbegin(); it != m_stagedAttachments.cend(); ++it) {
        if (!moving && m_materializedAttachments.contains(it.key())) continue;
        // Exact decoded references only: substrings and code examples do not
        // make a missing undo alias live; percent encoding must not hide it.
        if (!resolveImageUrl(it.key()).isEmpty() || referencedPaths.contains(it.key()))
            paths.insert(it.key(), it.key());
    }
    // Preflight every source/collision before creating any attachment files.
    for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
        const QString target = QDir(root).filePath(it.key());
        const QFileInfo targetInfo(target), documentInfo(documentPath);
        if (QDir::cleanPath(target).compare(QDir::cleanPath(documentPath), Qt::CaseInsensitive) == 0
            || (!targetInfo.canonicalFilePath().isEmpty()
                && targetInfo.canonicalFilePath() == documentInfo.canonicalFilePath())) {
            *error = QStringLiteral("The document and an attachment have the same destination: %1").arg(it.key());
            return false;
        }
        const QUrl source = resolveImageUrl(it.value());
        if (source.isEmpty()) {
            *error = QStringLiteral("Attachment is missing: %1. Locate it before saving.").arg(it.key());
            return false;
        }
        if (targetInfo.exists() && (targetInfo.isSymLink() || !targetInfo.isFile()
            || !Attachments::sameFileContents(source.toLocalFile(), target))) {
            *error = QStringLiteral("Attachment already exists: %1").arg(it.key());
            return false;
        }
    }
    for (auto it = paths.cbegin(); it != paths.cend(); ++it)
        if (!Attachments::copyInto(resolveImageUrl(it.value()).toLocalFile(), root, it.key(), error)) return false;
    return true;
}

bool Backend::updateAttachment(const QString &oldPath, const QString &newPath, const QString &source) {
    if (!m_document || !Attachments::safeRelativePath(newPath) || oldPath == newPath
        || m_stagedAttachments.contains(newPath)
        || (m_fileUrl.isLocalFile() && QFileInfo::exists(QFileInfo(m_fileUrl.toLocalFile()).dir().filePath(newPath)))) {
        setStatus(QStringLiteral("Choose a different, unused relative attachment path."));
        return false;
    }
    QVector<Attachments::Reference> matches;
    for (const auto &ref : Attachments::references(currentDocumentText()))
        if (Attachments::decodedPath(ref.path) == Attachments::decodedPath(oldPath)) matches.append(ref);
    std::sort(matches.begin(), matches.end(), [](const auto &a, const auto &b) { return a.start > b.start; });
    // Confirm source spans with Qt's Markdown parser. In particular, a matching
    // string in indented/container code or image alt text is not a reference.
    QString probedText = currentDocumentText();
    QStringList probeNames;
    const QString probeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (qsizetype i = 0; i < matches.size(); ++i) {
        const QString probe = QStringLiteral("omawrite-probe-%1-%2.png").arg(probeId).arg(i);
        probeNames.append(probe);
        probedText.replace(matches[i].start, matches[i].length, probe);
    }
    const QStringList renderedProbes = Attachments::imagePaths(probedText);
    QVector<Attachments::Reference> confirmed;
    for (qsizetype i = 0; i < matches.size(); ++i)
        if (renderedProbes.contains(probeNames[i])) confirmed.append(matches[i]);
    matches = confirmed;
    if (matches.isEmpty()) {
        setStatus(QStringLiteral("No supported matching image references in this document."));
        return false;
    }
    QString proposedText = currentDocumentText();
    for (const auto &ref : matches) proposedText.replace(ref.start, ref.length, newPath);
    for (const QString &reference : Attachments::imagePaths(proposedText)) {
        if (Attachments::decodedPath(reference) == Attachments::decodedPath(oldPath)) {
            setStatus(QStringLiteral("This image uses unsupported source syntax; edit its references manually."));
            return false;
        }
    }
    const QString root = attachmentStoragePath();
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".asset");
    QString error;
    if (!QDir().mkpath(root) || !Attachments::copyInto(source, root, id, &error)) {
        setStatus(error.isEmpty() ? QStringLiteral("Could not create attachment recovery storage.") : error);
        return false;
    }
    // Retain the old local asset as an undo alias too, even if it was never
    // pasted in this session. Save As then preserves an undo back to its path.
    const QString oldRelative = Attachments::decodedPath(oldPath);
    if (Attachments::safeLocalPath(oldRelative) && !m_stagedAttachments.contains(oldRelative)) {
        const QUrl oldSource = resolveImageUrl(oldPath);
        if (!oldSource.isEmpty()) {
            QString oldId = id;
            if (!Attachments::sameFileContents(oldSource.toLocalFile(), source)) {
                oldId = QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".asset");
                if (!Attachments::copyInto(oldSource.toLocalFile(), root, oldId, &error)) {
                    setStatus(error);
                    return false;
                }
            }
            m_stagedAttachments.insert(oldRelative, oldId);
            if (m_fileUrl.isLocalFile()) m_materializedAttachments.insert(oldRelative);
        }
    }
    // Retain both old and new staged entries: undo must not remove images.
    m_stagedAttachments.insert(newPath, id);
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    for (const auto &ref : matches) {
        cursor.setPosition(ref.start);
        cursor.setPosition(ref.start + ref.length, QTextCursor::KeepAnchor);
        cursor.insertText(newPath);
    }
    cursor.endEditBlock();
    emit attachmentUrlsChanged();
    scheduleRecovery();
    return true;
}

bool Backend::renameAttachment(const QString &oldPath, const QString &newRelativePath) {
    const QUrl source = resolveImageUrl(oldPath);
    if (source.isEmpty()) {
        setStatus(QStringLiteral("Attachment is missing; locate a local file first."));
        return false;
    }
    return updateAttachment(oldPath, newRelativePath, source.toLocalFile());
}

bool Backend::locateAttachment(const QString &oldPath, const QUrl &localFile, const QString &newRelativePath) {
    if (!localFile.isLocalFile() || !localFile.host().isEmpty()
        || !QFileInfo(localFile.toLocalFile()).isFile()) {
        setStatus(QStringLiteral("Choose an existing local attachment file."));
        return false;
    }
    QString path = newRelativePath;
    if (path.isEmpty()) {
        QString suffix = QFileInfo(localFile.toLocalFile()).suffix();
        if (suffix.isEmpty() || !Attachments::safeRelativePath(suffix)) suffix = QStringLiteral("img");
        path = QStringLiteral("img/attachment-%1.%2")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces), suffix);
    }
    return updateAttachment(oldPath, path, localFile.toLocalFile());
}

QString Backend::clipboardUrl() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    if (!mimeData)
        return {};

    if (mimeData->hasUrls()) {
        const QList<QUrl> urls = mimeData->urls();
        for (const QUrl &url : urls) {
            const QString normalized = normalizedLinkUrl(url.toString());
            if (!normalized.isEmpty())
                return normalized;
        }
    }

    if (!mimeData->hasText())
        return {};

    return normalizedLinkUrl(mimeData->text());
}

bool Backend::clipboardImageAvailable() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    return mime && mime->hasImage();
}

QString Backend::clipboardText() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    return mimeData && mimeData->hasText() ? mimeData->text() : QString();
}

bool Backend::editorTextChanged() {
    if (m_loading || m_formattingTypography)
        return false;

    const QString text = currentDocumentText();
    if (text == m_lastDocumentText)
        return false;
    m_lastDocumentText = text;

    reapplyTypographyToChange();

    scheduleWordCount();

    const QString &baseline = m_lastKnownFileText;
    // An empty baseline means a pristine untitled document, but only when there
    // is no file behind it. Once there is one, an unknown baseline is unknown
    // rather than empty, and emptying the editor is a change like any other.
    const bool baselineKnown = m_hasKnownFileContents
        || !m_fileUrl.isValid() || m_fileUrl.isEmpty();

    if (baselineKnown && text == baseline) {
        setModified(false);
        clearRecovery();
        if (m_hasKnownFileContents)
            setStatus(tr("Saved %1").arg(fileName()));
        else
            setStatus(QString());
    } else {
        setModified(true);
        setStatus(QString());
        scheduleRecovery();
        scheduleAutoSave();
    }

    return true;
}

void Backend::setCaretPosition(int position) {
    m_caretPosition = position;
    if (m_highlighter)
        m_highlighter->setCaret(position);
}

// The emphasis span of the marker's kind that the caret sits in or against,
// in document positions, so a toggle with nothing selected can act on it.
QVariantMap Backend::emphasisSpanAt(int position, const QString &marker) const {
    if (!m_document)
        return {};

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid() || block.userState() >= MarkdownHighlighter::InsideFence
            || block.previous().userState() >= MarkdownHighlighter::InsideFence)
        return {};

    using Kind = MarkdownHighlighter::InlineKind;
    const Kind kind = marker == QStringLiteral("**") ? Kind::Bold
        : marker == QStringLiteral("~~")              ? Kind::Strikethrough
        : marker == QStringLiteral("<u>")             ? Kind::Underline
                                                      : Kind::Italic;
    const int offset = position - block.position();
    const QList<MarkdownHighlighter::InlineMarkup> markup =
        MarkdownHighlighter::inlineMarkup(block.text());
    for (const auto &item : markup) {
        const int start = item.markers[0].start;
        const int end = item.markers[1].start + item.markers[1].length;
        if (item.kind != kind || offset < start || offset > end)
            continue;
        // The visible text ends before the chain of nested emphasis closing
        // with it, as the italic "*" inside "***word***".
        const int contentEnd = item.content.start + item.content.length;
        int textEnd = contentEnd;
        for (bool moved = true; moved;) {
            moved = false;
            for (const auto &nested : markup) {
                const auto &closing = nested.markers[1];
                if (closing.start >= item.content.start && closing.length > 0
                        && closing.start + closing.length == textEnd) {
                    textEnd = closing.start;
                    moved = true;
                }
            }
        }
        const int base = block.position();
        return {{QStringLiteral("start"), base + start},
                {QStringLiteral("end"), base + end},
                {QStringLiteral("contentStart"), base + item.content.start},
                {QStringLiteral("contentEnd"), base + contentEnd},
                {QStringLiteral("textEnd"), base + textEnd}};
    }
    return {};
}

QString Backend::linkUrlAt(int position) const {
    if (!m_document)
        return {};

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid())
        return {};

    // Fenced code is literal, so nothing in it is a link.
    if (block.userState() >= MarkdownHighlighter::InsideFence
            || block.previous().userState() >= MarkdownHighlighter::InsideFence)
        return {};

    const QString reference = referenceIndex().urlAt(position);
    if (!reference.isEmpty()) return reference;
    return MarkdownHighlighter::linkUrlAt(block.text(), position - block.position());
}

const MarkdownReferences::Index &Backend::referenceIndex() const {
    const int revision = m_document ? m_document->revision() : -1;
    if (m_referenceDocument != m_document || revision != m_referenceRevision) {
        m_referenceDocument = m_document;
        m_referenceRevision = revision;
        m_referenceIndex = MarkdownReferences::Index(currentDocumentText());
    }
    return m_referenceIndex;
}

QString Backend::linkHoverText(const QString &url) const {
    if (!url.startsWith(QStringLiteral("write-footnote:"))) return url;
    const auto &index = referenceIndex();
    const auto note = index.notes.constFind(MarkdownReferences::noteKey(url));
    if (note == index.notes.cend()) return {};
    QString markdown = note->text;
    for (auto it = index.links.cbegin(); it != index.links.cend(); ++it) {
        QString label = it.key();
        label.replace('\\', QStringLiteral("\\\\")).replace('[', QStringLiteral("\\[")).replace(']', QStringLiteral("\\]"));
        QString destination = it->text;
        destination.replace('<', QStringLiteral("%3C")).replace('>', QStringLiteral("%3E"));
        markdown += QStringLiteral("\n\n[%1]: <%2>\n").arg(label, destination);
    }
    // Plain, multiline preview. Do not interpret HTML or load note images.
    class NoteDocument : public QTextDocument {
        QVariant loadResource(int, const QUrl &) override { return {}; }
    } rendered;
    rendered.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    return rendered.toPlainText();
}

int Backend::footnoteSourcePosition(const QString &url) const {
    const auto &notes = referenceIndex().notes;
    const auto note = notes.constFind(MarkdownReferences::noteKey(url));
    return note == notes.cend() ? -1 : note->start;
}

QString Backend::nextReferenceLabel(bool footnote) const {
    return referenceIndex().nextLabel(footnote);
}

int Backend::cursorPositionStep(int position, int direction) const
{
    if (!m_document || direction == 0)
        return position;

    const int documentLength = m_document->characterCount() - 1;
    position = qBound(0, position, documentLength);

    if (direction > 0) {
        if (position >= documentLength)
            return documentLength;
    } else if (position <= 0) {
        return 0;
    }

    const QTextBlock block = m_document->findBlock(position);
    if (!block.isValid())
        return position;

    const int blockStart = block.position();
    const int blockTextLength = block.text().size();
    // A block's text excludes its paragraph separator, so this is the last
    // cursor position inside the block, just before the separator.
    const int blockEnd = blockStart + blockTextLength;

    if (direction > 0) {
        // The separator is the next valid cursor position past a block; the
        // layout only knows the block's own characters.
        if (position >= blockEnd)
            return position + 1;
    } else if (position <= blockStart) {
        return position - 1;
    }

    // A fresh layout over the block's own text is always in sync with the
    // document, unlike the document's display layout, whose text() cannot be
    // checked for staleness. Building lines is unnecessary for cursor
    // stepping and dominates the cost, so the layout is left unformatted.
    // QTextLayout steps by whole grapheme clusters, so surrogate pairs, skin
    // tone modifiers and ZWJ sequences are crossed in a single move.
    QTextLayout layout(block.text());
    const int local = position - blockStart;
    const int stepped = direction > 0
        ? layout.nextCursorPosition(local, QTextLayout::SkipCharacters)
        : layout.previousCursorPosition(local, QTextLayout::SkipCharacters);
    return blockStart + qBound(0, stepped, blockTextLength);
}

void Backend::setSearchHighlight(const QString &query, int currentMatchStart) {
    if (m_highlighter)
        m_highlighter->setSearch(query, currentMatchStart);
}

// Found by the highlighter's own matcher, so the offsets agree with what it
// paints, diacritics and all.
QVariantList Backend::searchMatches(const QString &query) const {
    QVariantList matches;
    if (query.isEmpty())
        return matches;
    for (const int start : MarkdownHighlighter::findMatches(currentDocumentText(), query))
        matches.append(start);
    return matches;
}

// One edit command, so a single undo takes the whole replacement back.
void Backend::replaceRange(int start, int end, const QString &text) {
    if (!m_document)
        return;
    const int last = m_document->characterCount() - 1;
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    cursor.setPosition(qBound(0, start, last));
    cursor.setPosition(qBound(0, end, last), QTextCursor::KeepAnchor);
    cursor.insertText(text);
    cursor.endEditBlock();
}

// A block's state says what follows its line, so a fence line itself is only
// code on its inner side: past an opening fence's backticks or tildes, or anywhere
// before the end of a closing one.
bool Backend::insideFence(int position) const {
    if (!m_document)
        return false;
    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid())
        return false;
    const bool opensOrContinues = block.userState() >= MarkdownHighlighter::InsideFence;
    const bool afterOpening = block.previous().userState() >= MarkdownHighlighter::InsideFence;
    const int offset = position - block.position();
    if (opensOrContinues && !afterOpening) {
        static const QRegularExpression fenceMark(QStringLiteral("[`~]"));
        return offset >= block.text().indexOf(fenceMark) + 3;
    }
    if (afterOpening && !opensOrContinues)
        return offset < block.text().length();
    return opensOrContinues;
}

QVariantMap Backend::windowGeometry() const {
    QSettings settings;
    return {{QStringLiteral("x"), settings.value(QStringLiteral("window/x"), -1)},
            {QStringLiteral("y"), settings.value(QStringLiteral("window/y"), -1)},
            {QStringLiteral("width"), settings.value(QStringLiteral("window/width"), 1280)},
            {QStringLiteral("height"), settings.value(QStringLiteral("window/height"), 820)},
            {QStringLiteral("maximized"), settings.value(QStringLiteral("window/maximized"), false)}};
}

void Backend::saveWindowGeometry(int x, int y, int width, int height, bool maximized) {
    QSettings settings;
    if (!maximized) {
        settings.setValue(QStringLiteral("window/x"), x);
        settings.setValue(QStringLiteral("window/y"), y);
        settings.setValue(QStringLiteral("window/width"), width);
        settings.setValue(QStringLiteral("window/height"), height);
    }
    settings.setValue(QStringLiteral("window/maximized"), maximized);
}

QString Backend::defaultEditorFont() {
    return QStringLiteral("IBM Plex Mono");
}

void Backend::setEditorFont(const QString &family) {
    if (family.isEmpty() || family == m_editorFont)
        return;

    m_editorFont = family;
    QSettings().setValue(editorFontSetting, family);
    emit editorFontChanged();
}

QStringList Backend::availableFonts() const {
    // Leave out icon, emoji, and other symbol fonts, along with anything that
    // cannot set the Latin text the editor is written in.
    QStringList families;
    for (const QString &family : QFontDatabase::families(QFontDatabase::Latin)) {
        if (QFontDatabase::isPrivateFamily(family)
                || QFontDatabase::writingSystems(family).contains(QFontDatabase::Symbol))
            continue;
        families.append(family);
    }
    return selectableFontFamilies(families);
}

QStringList Backend::selectableFontFamilies(const QStringList &families) {
    QStringList selectable;
    static const QRegularExpression foundrySuffix(QStringLiteral("\\s*\\[[^\\]]*\\]$"));
    for (QString family : families) {
        // The same family from two foundries is listed as "Name [Foundry]".
        family.remove(foundrySuffix);
        if (family.startsWith(QStringLiteral("Noto ")) && !keptNotoFamilies.contains(family))
            continue;
        if (genericFontAliases.contains(family, Qt::CaseInsensitive)
                || family.contains(symbolFontName))
            continue;
        if (family == defaultEditorFont() || selectable.contains(family, Qt::CaseInsensitive))
            continue;
        selectable.append(family);
    }
    std::sort(selectable.begin(), selectable.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    selectable.prepend(defaultEditorFont());
    return selectable;
}

void Backend::documentFontChanged() {
    if (m_highlighter)
        m_highlighter->refreshFont();
}

void Backend::loadDocumentText(const QString &text) {
    if (!m_document) {
        setStatus(tr("Could not attach the Markdown renderer."));
        return;
    }

    // Inserting under the line height already set styles the text once, where
    // setPlainText and a typography pass after it would restyle it twice.
    m_loading = true;
    const bool undoEnabled = m_document->isUndoRedoEnabled();
    m_document->setUndoRedoEnabled(false);
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.removeSelectedText();
    cursor.setBlockFormat(lineHeightFormat());
    cursor.insertText(text);
    cursor.endEditBlock();
    m_document->setUndoRedoEnabled(undoEnabled);
    m_lastDocumentText = text;
    m_loading = false;

    m_wordCountTimer.stop();
    m_wordCountStale = true;
    m_wordCountAgain = false;
    setWordCount(countWords(text));
}

void Backend::setFileUrl(const QUrl &url) {
    if (m_fileUrl == url)
        return;

    m_fileUrl = url;
    emit fileUrlChanged();
}

void Backend::setModified(bool modified) {
    if (m_modified == modified)
        return;

    m_modified = modified;
    emit modifiedChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;

    m_status = status;
    emit statusChanged();
}

void Backend::saveTo(const QUrl &url) {
    if (!url.isLocalFile()) {
        setStatus(tr("Only local files can be saved."));
        emit saveFailed();
        return;
    }

    if (m_writeGuard && !m_writeGuard(url)) {
        setStatus(QStringLiteral("This file is already open in another window. Choose a different Save As name."));
        emit saveFailed();
        return;
    }
    QString attachmentError;
    if (!materializeAttachments(url, &attachmentError)) {
        setStatus(attachmentError);
        emit saveFailed();
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QSaveFile file(url.toLocalFile());
    // Without a temporary file beside it, as in a folder only the file itself
    // may be written in, write the file in place rather than fail.
    file.setDirectWriteFallback(true);
    // Binary writes preserve the UTF-8/LF baseline on Windows too.
    if (!file.open(QIODevice::WriteOnly)) {
        setStatus(tr("Could not save %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    const QByteArray contents = currentDocumentText().toUtf8();
    if (file.write(contents) != contents.size()) {
        setStatus(QStringLiteral("Could not write %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    // QSaveFile commits by replacing the target. Stop watching the old inode
    // before that replacement so our own write is not classified as external.
    unwatchFiles();

    // commit() flushes, fsyncs, and atomically renames the temp file into place,
    // returning false (and leaving the original untouched) on any write error.
    if (!file.commit()) {
        watchCurrentFile();
        setStatus(tr("Could not write %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    setKnownFileContents(contents, true);
    m_autoSaveSuspended = false;
    m_autoSaveTimer.stop();
    setFileUrl(url);
    for (auto it = m_stagedAttachments.cbegin(); it != m_stagedAttachments.cend(); ++it)
        m_materializedAttachments.insert(it.key());
    emit attachmentUrlsChanged();
    watchCurrentFile();
    QSettings().setValue(lastSaveDirectorySetting,
                         QFileInfo(url.toLocalFile()).absolutePath());
    setModified(false);
    setStatus(tr("Saved %1").arg(fileName()));
    clearRecovery();
    emit saveSucceeded();
}

void Backend::scheduleRecovery() {
    m_recoveryTimer.start();
}

void Backend::scheduleAutoSave() {
    // Only auto-save documents with a file name. Untitled drafts are left to
    // manual save so auto-save never silently opens a file picker or creates a
    // file the writer did not name; crash recovery still covers them. A named
    // file that does not exist yet waits for an explicit save (see autoSave()).
    if (m_fileUrl.isLocalFile() && !m_autoSaveSuspended && !m_autoSaveHeld)
        m_autoSaveTimer.start();
}

void Backend::holdAutoSave(bool held) {
    m_autoSaveHeld = held;
    if (held)
        m_autoSaveTimer.stop();
    else if (m_modified)
        scheduleAutoSave();
}

void Backend::autoSave() {
    if (!m_modified || !m_fileUrl.isLocalFile() || m_autoSaveSuspended || m_autoSaveHeld)
        return;

    // Never write over a version we have not seen. The watcher normally
    // catches outside edits first, but it can miss some (or fire late), so
    // check the file itself right before replacing it.
    const bool exists = QFileInfo::exists(m_fileUrl.toLocalFile());
    if (!m_hasKnownFileContents) {
        // A removed file, a new one not yet saved, or a recovered draft whose
        // file was gone waits for an explicit save rather than being created.
        suspendAutoSave();
        if (exists)
            emit externalChangeDetected(false, m_modified);
        return;
    }
    if (!diskMatchesKnownContents()) {
        reportExternalChange(!exists);
        return;
    }

    saveTo(m_fileUrl);
}

void Backend::suspendAutoSave() {
    // Held until the writer reloads, keeps their version, or saves explicitly.
    m_autoSaveTimer.stop();
    m_autoSaveSuspended = true;
}

// Whether the file still holds what we last read or wrote, read as open()
// reads it so carriage returns compare equal.
bool Backend::diskMatchesKnownContents() const {
    if (!m_hasKnownFileContents)
        return false;
    QFile file(m_fileUrl.toLocalFile());
    return file.open(QIODevice::ReadOnly | QIODevice::Text)
        && file.readAll() == m_lastKnownFileContents;
}

void Backend::reportExternalChange(bool deleted) {
    // The parent watch also sees unrelated sibling changes. Do not reopen a
    // dismissed conflict for the same disk state, but still report a later edit.
    QFile file(m_fileUrl.toLocalFile());
    const bool readable = !deleted && file.open(QIODevice::ReadOnly | QIODevice::Text);
    QByteArray state = deleted ? QByteArray("missing") : QByteArray("unreadable");
    if (readable)
        state = "contents:" + QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    if (state == m_lastReportedFileState)
        return;

    // Whatever is on disk now, it is not what we last read, so the baseline is
    // unknown until the writer picks a version. A deletion counts: the old
    // text is not saved anywhere either.
    setKnownFileContents(QByteArray(), false);
    m_lastReportedFileState = state;
    suspendAutoSave();
    emit externalChangeDetected(deleted, m_modified);
}

// Saving writes UTF-8 with plain line endings; anything else on disk would be
// rewritten by it.
bool Backend::savesUnchanged(const QByteArray &raw) {
    return !raw.contains('\r') && QString::fromUtf8(raw).toUtf8() == raw;
}

void Backend::setKnownFileContents(const QByteArray &contents, bool known) {
    m_lastReportedFileState.clear();
    m_lastKnownFileContents = contents;
    m_hasKnownFileContents = known;
    // Every caller reads in text mode or writes plain line endings, so there
    // are no carriage returns to strip.
    m_lastKnownFileText = known ? QString::fromUtf8(contents) : QString();
}

void Backend::writeRecovery() {
    if (!m_modified)
        return;
    const QString path = m_recoveryPath;
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        setStatus(QStringLiteral("Recovery storage is unavailable; save your document before closing."));
        return;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setStatus(QStringLiteral("Could not open recovery storage; save your document before closing."));
        return;
    }
    QJsonObject attachments;
    for (auto it = m_stagedAttachments.cbegin(); it != m_stagedAttachments.cend(); ++it)
        attachments.insert(it.key(), it.value());
    QJsonArray materialized;
    for (const QString &path : m_materializedAttachments) materialized.append(path);
    const QJsonObject recovery{{QStringLiteral("fileUrl"), m_fileUrl.toString()},
                               {QStringLiteral("text"), currentDocumentText()},
                               {QStringLiteral("attachments"), attachments},
                               {QStringLiteral("materializedAttachments"), materialized}};
    const QByteArray bytes = QJsonDocument(recovery).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit())
        setStatus(QStringLiteral("Could not write recovery draft; keep this window open and save your document."));
}

void Backend::restoreRecovery() {
    QFile file(m_recoveryPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    if (!json.isObject() || !json.object().contains(QStringLiteral("text"))) {
        // Preserve malformed bytes for manual recovery without claiming the
        // same broken snapshot on every launch.
        file.rename(m_recoveryPath + QStringLiteral(".invalid-")
                    + QUuid::createUuid().toString(QUuid::WithoutBraces));
        return;
    }
    const QJsonObject recovery = json.object();
    m_stagedAttachments.clear();
    m_materializedAttachments.clear();
    const QJsonObject attachments = recovery.value(QStringLiteral("attachments")).toObject();
    const QRegularExpression storageId(QStringLiteral("^[a-f0-9-]{36}\\.(png|asset)$"));
    bool missingAttachment = false;
    for (auto it = attachments.begin(); it != attachments.end(); ++it) {
        const QString id = it.value().toString();
        if (!Attachments::safeLocalPath(it.key()) || !storageId.match(id).hasMatch()) {
            missingAttachment = true;
            continue;
        }
        // Keep missing IDs too: saving must fail, not silently drop the image.
        m_stagedAttachments.insert(it.key(), id);
        missingAttachment |= !QFileInfo::exists(QDir(attachmentStoragePath()).filePath(id));
    }
    for (const auto &value : recovery.value(QStringLiteral("materializedAttachments")).toArray())
        if (m_stagedAttachments.contains(value.toString())) m_materializedAttachments.insert(value.toString());
    emit attachmentUrlsChanged();
    emit documentAboutToReplace();
    loadDocumentText(recovery.value(QStringLiteral("text")).toString());
    const QUrl recoveredUrl(recovery.value(QStringLiteral("fileUrl")).toString());
    QFile diskFile(recoveredUrl.toLocalFile());
    if (recoveredUrl.isLocalFile() && diskFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setKnownFileContents(diskFile.readAll(), true);
    } else {
        setKnownFileContents(QByteArray(), false);
    }
    setFileUrl(recoveredUrl);
    // The file may have moved on since this draft was written, and the draft
    // alone cannot tell. Leave overwriting it to an explicit save.
    m_autoSaveSuspended = recoveredUrl.isLocalFile();
    watchCurrentFile();
    setModified(true);
    setStatus(missingAttachment
        ? tr("Recovered unsaved changes; some staged attachments are missing or invalid.")
        : tr("Recovered unsaved changes"));
    emit documentLoaded();
}

void Backend::clearRecovery() {
    m_recoveryTimer.stop();
    QFile::remove(m_recoveryPath);
}

void Backend::unwatchFiles() {
    m_fileCheckTimer.stop();
    const QStringList watched = m_fileWatcher.files() + m_fileWatcher.directories();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);
}

// Keep the parent subscribed even while the file exists: replacement can
// invalidate the file watch without delivering a useful fileChanged event.
// Reconcile incrementally so rearming never tears down the healthy parent.
void Backend::watchCurrentFile() {
    QString filePath;
    QString directoryPath;
    if (m_fileUrl.isLocalFile()) {
        const QFileInfo info(m_fileUrl.toLocalFile());
        m_fileMissing = !info.exists();
        if (!m_fileMissing)
            filePath = info.absoluteFilePath();
        if (info.absoluteDir().exists())
            directoryPath = info.absolutePath();
    }
    for (const QString &path : m_fileWatcher.files())
        if (path != filePath)
            m_fileWatcher.removePath(path);
    for (const QString &path : m_fileWatcher.directories())
        if (path != directoryPath)
            m_fileWatcher.removePath(path);
    if (!directoryPath.isEmpty() && !m_fileWatcher.directories().contains(directoryPath))
        m_fileWatcher.addPath(directoryPath);
    if (!filePath.isEmpty() && !m_fileWatcher.files().contains(filePath))
        m_fileWatcher.addPath(filePath);
}

void Backend::checkWatchedFile() {
    // Atomic saves replace the watched inode, which drops it from the
    // watcher, so re-arm it either way. Our own save is not an outside edit.
    const bool wasMissing = m_fileMissing;
    watchCurrentFile();
    if (diskMatchesKnownContents())
        return;
    // A file that was already missing has been reported, or was never there.
    // One that turns up again is an outside edit like any other, even while
    // the writer is still looking at the removal.
    if (m_fileMissing && wasMissing)
        return;
    reportExternalChange(m_fileMissing);
}

// Black or white, whichever reads on the color (WCAG relative luminance).
QString Backend::readableOn(const QString &color) {
    const QColor c(color);
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? QStringLiteral("black") : QStringLiteral("white");
}

// A quoted TOML value ends at its closing quote; whatever trails it is an inline comment.
QString Backend::tomlValue(const QString &text) {
    const QString value = text.trimmed();
    if (value.isEmpty())
        return value;

    const QChar quote = value.front();
    if (quote != QLatin1Char('"') && quote != QLatin1Char('\''))
        return value;

    const int close = value.indexOf(quote, 1);
    return close < 0 ? value : value.mid(1, close - 1);
}

void Backend::loadOmarchyTheme() {
    const QString colorsPath = m_themeHome
        + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");
    QString themeMode;
    QString background;
    QString foreground;
    QString accent;
    QString selection;
    QString selectionBackground;
    QString selectionForeground;
    QString brightForeground;
    QString lighterBackground;
    QFile file(colorsPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;

            const int equals = line.indexOf(QLatin1Char('='));
            if (equals < 0)
                continue;

            const QString key = line.left(equals).trimmed();
            const QString value = tomlValue(line.mid(equals + 1));

            if (key == QStringLiteral("mode"))
                themeMode = value;
            else if (key == QStringLiteral("background"))
                background = value;
            else if (key == QStringLiteral("foreground"))
                foreground = value;
            else if (key == QStringLiteral("accent"))
                accent = value;
            else if (key == QStringLiteral("selection"))
                selection = value;
            else if (key == QStringLiteral("selection_background"))
                selectionBackground = value;
            else if (key == QStringLiteral("selection_foreground"))
                selectionForeground = value;
            else if (key == QStringLiteral("bright_foreground"))
                brightForeground = value;
            else if (key == QStringLiteral("lighter_background"))
                lighterBackground = value;
        }
    }

    const bool wasDark = m_darkMode;
    const QStringList previousColors{m_themeBackground, m_themeForeground, m_themeAccent,
                                     m_themeSelection, m_themeSelectionForeground,
                                     m_themePanelBackground};

    bool themeIsDark = m_systemDarkMode;
    if (themeMode == QStringLiteral("dark")) {
        themeIsDark = true;
    } else if (themeMode == QStringLiteral("light")) {
        themeIsDark = false;
    } else {
        const QColor parsedBackground(background);
        if (parsedBackground.isValid()) {
            const double luminance = 0.299 * parsedBackground.redF()
                + 0.587 * parsedBackground.greenF() + 0.114 * parsedBackground.blueF();
            themeIsDark = luminance < 0.5;
        }
    }
    m_darkMode = themeIsDark;

    // The defaults follow the mode the theme resolved to, so a colour we cannot
    // read costs that one colour rather than the contrast of the whole editor.
    m_themeBackground = m_darkMode ? QStringLiteral("#101010") : QStringLiteral("#ffffff");
    m_themeForeground = m_darkMode ? QStringLiteral("#eeeeee") : QStringLiteral("#222324");
    m_themeAccent = m_darkMode ? QStringLiteral("#5584aa") : QStringLiteral("#2077b2");
    m_themeSelection = m_darkMode ? QStringLiteral("#186a9a") : QStringLiteral("#2077b2");

    // An unparseable colour reaches QML as black, so keep the default instead.
    // Later calls win, so each fallback is applied before what it falls back from.
    const auto applyColor = [](QString &target, const QString &value) {
        if (QColor(value).isValid())
            target = value;
    };
    applyColor(m_themeBackground, background);
    applyColor(m_themeForeground, foreground);
    applyColor(m_themeAccent, accent);
    applyColor(m_themeSelection, selectionBackground);
    applyColor(m_themeSelection, selection);
    m_themeSelectionForeground = m_themeForeground;
    applyColor(m_themeSelectionForeground, brightForeground);
    applyColor(m_themeSelectionForeground, selectionForeground);

    // Omarchy themes name a lighter background for panels like the one code
    // sits on, so use the shade the theme chose. Not every theme sets the key,
    // and a few set it to the page background, which would leave code with no
    // panel at all: mix a shade of the page towards the text for those. Mixing,
    // unlike lightening, still moves on a pure black background.
    const QColor page(m_themeBackground);
    const QColor lighter(lighterBackground);
    m_themePanelBackground = (lighter.isValid() && lighter != page
                                 ? lighter
                                 : blend(page, QColor(m_themeForeground), 0.06)).name();

    if (m_highlighter) {
        m_highlighter->setTheme(m_darkMode, m_themeBackground, m_themeForeground,
                                m_themeAccent, m_themePanelBackground);
    }

    if (m_darkMode != wasDark)
        emit darkModeChanged();
    const QStringList colors{m_themeBackground, m_themeForeground, m_themeAccent,
                             m_themeSelection, m_themeSelectionForeground,
                             m_themePanelBackground};
    if (colors != previousColors)
        emit themeColorsChanged();
}

void Backend::watchOmarchyTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const QString currentDir = m_themeHome
        + QStringLiteral("/.local/state/omarchy/current");
    const QString themeDir = currentDir + QStringLiteral("/theme");
    const QString colorsPath = themeDir + QStringLiteral("/colors.toml");

    if (QDir(currentDir).exists())
        m_themeWatcher.addPath(currentDir);
    if (QDir(themeDir).exists())
        m_themeWatcher.addPath(themeDir);
    if (QFile::exists(colorsPath))
        m_themeWatcher.addPath(colorsPath);
}

QUrl Backend::suggestedSaveUrl() const {
    if (m_fileUrl.isLocalFile())
        return m_fileUrl;

    const QString savedDirectory = QSettings().value(lastSaveDirectorySetting).toString();
    const QDir directory = savedDirectory.isEmpty() || !QDir(savedDirectory).exists()
        ? QDir::home()
        : QDir(savedDirectory);
    return QUrl::fromLocalFile(
        directory.filePath(suggestedFileName(currentDocumentText())));
}

// toPlainText() turns non-breaking spaces into spaces and line separators
// into newlines, which saving would write over the file's own characters.
QString Backend::currentDocumentText() const {
    if (!m_document)
        return {};
    QString text = m_document->toRawText();
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return text;
}

int Backend::countWords(const QString &text) {
    // Only letters and numbers: PCRE2 reads \p{Han} as Script_Extensions, which
    // puts 。 、 「 and the CJK radicals in Han and would count each as a word.
    // One per thread, as loading a file counts here while a worker may too.
    thread_local const QRegularExpression cjkRe(
        QStringLiteral("(?=[\\p{L}\\p{N}])[\\p{Han}\\p{Hiragana}\\p{Katakana}]"));
    thread_local const QRegularExpression wordRe(
        QStringLiteral("[\\p{L}\\p{N}]+(?:['-][\\p{L}\\p{N}]+)*"));

    // Count unspaced Han and kana characters individually, then replace them
    // so they remain boundaries between surrounding space-delimited words.
    int count = 0;

    QRegularExpressionMatchIterator cjkIt = cjkRe.globalMatch(text);
    while (cjkIt.hasNext()) {
        cjkIt.next();
        ++count;
    }

    QString nonCjkText = text;
    nonCjkText.replace(cjkRe, QStringLiteral(" "));

    QRegularExpressionMatchIterator it = wordRe.globalMatch(nonCjkText);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

void Backend::countSelectedText(const QString &text) {
    if (text == m_selectionText) return;
    m_selectionText = text;
    ++m_selectionGeneration;
    if (text.isEmpty()) {
        if (m_selectionWordCount) {
            m_selectionWordCount = 0;
            emit selectionWordCountChanged();
        }
    } else if (!m_selectionCounting) {
        refreshSelectionWordCount();
    }
}

void Backend::refreshSelectionWordCount() {
    m_selectionCounting = true;
    m_selectionJobGeneration = m_selectionGeneration;
    m_selectionCountWatcher.setFuture(QtConcurrent::run(&Backend::countWords, m_selectionText));
}

QString Backend::positionKey(const QUrl &url) {
    if (!url.isLocalFile()) return {};
    const QFileInfo info(url.toLocalFile());
    if (!info.isFile()) return {};
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) return {};
    return QString::fromLatin1(QCryptographicHash::hash(
        QUrl::fromLocalFile(canonical).toEncoded(), QCryptographicHash::Sha256).toHex());
}

QVariantMap Backend::readingPosition(const QUrl &url) const {
    const QString key = positionKey(url);
    return key.isEmpty() ? QVariantMap{} : QSettings().value(QStringLiteral("readingPositions/") + key).toMap();
}

void Backend::rememberReadingPosition(const QUrl &url, qreal ratio, bool readMode, int caret,
                                     int anchor, qreal offset) {
    const QString key = positionKey(url);
    if (key.isEmpty() || !std::isfinite(ratio) || !std::isfinite(offset)) return;
    QSettings settings;
    settings.beginGroup(QStringLiteral("readingPositions"));
    settings.setValue(key, QVariantMap{{QStringLiteral("ratio"), qBound(qreal(0), ratio, qreal(1))},
        {QStringLiteral("readMode"), readMode}, {QStringLiteral("caret"), qMax(0, caret)},
        {QStringLiteral("anchor"), qMax(-1, anchor)}, {QStringLiteral("offset"), offset},
        {QStringLiteral("updated"), QDateTime::currentMSecsSinceEpoch()}});
    auto keys = settings.childKeys();
    if (keys.size() > 100) {
        std::sort(keys.begin(), keys.end(), [&settings](const QString &a, const QString &b) {
            return settings.value(a).toMap().value(QStringLiteral("updated")).toLongLong()
                < settings.value(b).toMap().value(QStringLiteral("updated")).toLongLong();
        });
        for (int i = 0; i < keys.size() - 100; ++i) settings.remove(keys[i]);
    }
}

QString Backend::explorerRevealArguments(QString path) {
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return QStringLiteral("/select,\"%1\"").arg(path);
}

bool Backend::revealCurrentFile() {
    const QFileInfo info(m_fileUrl.toLocalFile());
    if (!m_fileUrl.isLocalFile() || !info.isFile()) {
        setStatus(QStringLiteral("Save this file before revealing it in the file manager."));
        return false;
    }
    const QString path = info.absoluteFilePath();
#ifdef Q_OS_MACOS
    const bool opened = QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), path});
#elif defined(Q_OS_WIN)
    QProcess explorer;
    explorer.setProgram(QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows")))
                            .filePath(QStringLiteral("explorer.exe")));
    explorer.setNativeArguments(explorerRevealArguments(path));
    const bool opened = explorer.startDetached();
#elif defined(Q_OS_LINUX)
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("/org/freedesktop/FileManager1"), QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("ShowItems"));
    message << QStringList{QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)} << QString();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 1500), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, path](QDBusPendingCallWatcher *call) {
        const QDBusPendingReply<> reply = *call;
        if (reply.isError() && !QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())))
            setStatus(QStringLiteral("Could not open the file manager."));
        call->deleteLater();
    });
    const bool opened = true; // Asynchronous; fallback opens the folder, not necessarily selecting.
#else
    const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
#endif
    if (!opened) setStatus(QStringLiteral("Could not open the file manager."));
    return opened;
}

QString Backend::suggestedFileName(const QString &text) {
    QString name = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\x00-\\x1f\\x7f]")),
                 QStringLiteral("-"));
    name = name.left(120).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = tr("Untitled");
    if (!name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        name += QStringLiteral(".md");
    return name;
}

void Backend::setWordCount(int words) {
    if (m_wordCount == words)
        return;

    m_wordCount = words;
    emit wordCountChanged();
}

void Backend::refreshWordCount() {
    // One count at a time; edits made while it runs get one more after it.
    if (m_wordCountWatcher.isRunning()) {
        m_wordCountAgain = true;
        return;
    }
    // editorTextChanged keeps m_lastDocumentText current, and handing the
    // worker that shared copy saves reading the whole document out again.
    m_wordCountStale = false;
    m_wordCountWatcher.setFuture(QtConcurrent::run(&Backend::countWords, m_lastDocumentText));
}

void Backend::scheduleWordCount() {
    m_wordCountTimer.start();
}

void Backend::applyDocumentTypography() {
    if (!m_document)
        return;

    // A full pass is only used for a freshly attached document, so it is safe
    // to drop undo history here (re-enabling clears the stack anyway). A loaded
    // file gets the line height as it is inserted, in loadDocumentText.
    const bool undoEnabled = m_document->isUndoRedoEnabled();
    m_document->setUndoRedoEnabled(false);

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(lineHeightFormat());
    m_formattingTypography = false;

    m_document->setUndoRedoEnabled(undoEnabled);
}

void Backend::reapplyTypographyToChange() {
    if (!m_document)
        return;

    // Format only the block(s) touched by the last edit instead of the whole
    // document, and fold the change into the preceding edit command so a single
    // undo reverts both the text and its formatting.
    const int maxPos = m_document->characterCount() - 1;
    const int start = qBound(0, m_lastChangePos, maxPos);
    const int end = qBound(start, m_lastChangePos + m_lastChangeAdded, maxPos);

    // Blocks that already have it are left alone: an undo brings their format
    // back with the text, and merging anyway would push a command that clears
    // the redo stack.
    bool needed = false;
    for (QTextBlock block = m_document->findBlock(start);
         block.isValid() && block.position() <= end; block = block.next()) {
        const QTextBlockFormat format = block.blockFormat();
        if (format.lineHeightType() != QTextBlockFormat::ProportionalHeight
                || format.lineHeight() != typoraLineHeightPercent) {
            needed = true;
            break;
        }
    }
    if (!needed)
        return;

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    cursor.joinPreviousEditBlock();
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.mergeBlockFormat(lineHeightFormat());
    cursor.endEditBlock();
    m_formattingTypography = false;
}

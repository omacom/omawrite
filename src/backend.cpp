#include "backend.h"

#include <QClipboard>
#include <QCollator>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMarginsF>
#include <QPageLayout>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QPrintDialog>
#include <QPrinter>
#include <QQuickTextDocument>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QScreen>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>

#include "markdownhighlighter.h"

namespace {
QList<Backend *> g_liveWindows;
bool g_sessionWritesFrozen = false;
}

constexpr qreal typoraLineHeightPercent = 140;
const QString lastSaveDirectorySetting = QStringLiteral("file/lastSaveDirectory");
const QString editorFontSizeSetting = QStringLiteral("editor/fontSize");
constexpr int defaultEditorFontSize = 20;
constexpr int minimumEditorFontSize = 10;
constexpr int maximumEditorFontSize = 48;
const QString editorFontFamilySetting = QStringLiteral("editor/fontFamily");
const QString caretStyleSetting = QStringLiteral("editor/caretStyle");
const QString caretBlinkSetting = QStringLiteral("editor/caretBlink");
const QString editorMeasureCharsSetting = QStringLiteral("editor/measureChars");
const QString printMarginMmSetting = QStringLiteral("print/marginMm");
// Shipped with the app, so these are the families that are always there.
// Quattro leads: four character widths, and the face iA Writer defaults to.
const QString bundledFontFamily = QStringLiteral("iA Writer Quattro S");
constexpr int defaultEditorMeasureChars = 65;
constexpr int minimumEditorMeasureChars = 20;
constexpr int maximumEditorMeasureChars = 200;
constexpr qreal defaultPrintMarginMm = 15.0;
const QString printFontPointSizeSetting = QStringLiteral("print/fontPointSize");
// Ordinary book and report body text. The editor's 20px means nothing here.
constexpr qreal defaultPrintFontPointSize = 11.0;
constexpr qreal maximumPrintMarginMm = 60.0;
const QString wordTargetSetting = QStringLiteral("editor/wordTarget");
const QString paragraphOnReturnSetting = QStringLiteral("editor/paragraphOnReturn");
const QString autosaveSetting = QStringLiteral("editor/autosave");
const QString autosaveDelaySetting = QStringLiteral("editor/autosaveDelayMs");
constexpr int defaultAutosaveDelayMs = 750;
// Below a fifth of a second the debounce fires mid-word; above a minute it is
// no longer the safety net it is here to be.
constexpr int minimumAutosaveDelayMs = 200;
constexpr int maximumAutosaveDelayMs = 60000;

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

Backend::Backend(QObject *parent) : QObject(parent) {
    m_editorFontSize = qBound(minimumEditorFontSize,
                              QSettings().value(editorFontSizeSetting,
                                                defaultEditorFontSize).toInt(),
                              maximumEditorFontSize);

    const QString stateDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(stateDirectory);
    m_recoveryPath = sessionPath();
    auto lock = std::make_unique<QLockFile>(
        QDir(stateDirectory).filePath(QStringLiteral("session.lock")));
    lock->tryLock();
    m_recoveryLock = std::move(lock);
    ensureTab();
    g_liveWindows.append(this);
    m_wordCountTimer.setSingleShot(true);
    m_wordCountTimer.setInterval(120);
    connect(&m_wordCountTimer, &QTimer::timeout, this, &Backend::refreshWordCount);
    const QString requestedFamily =
        QSettings().value(editorFontFamilySetting).toString();
    m_editorFontFamily = requestedFamily.isEmpty()
        ? bundledFontFamily
        : resolveFontFamily(requestedFamily, availableFontFamilies());
    m_caretStyle = QSettings().value(caretStyleSetting,
                                     QStringLiteral("line")).toString()
                   == QStringLiteral("block")
        ? QStringLiteral("block")
        : QStringLiteral("line");
    m_caretBlink = QSettings().value(caretBlinkSetting, true).toBool();
    m_editorMeasureChars = qBound(minimumEditorMeasureChars,
                                  QSettings().value(editorMeasureCharsSetting,
                                                    defaultEditorMeasureChars).toInt(),
                                  maximumEditorMeasureChars);
    m_printMarginMm = qBound(qreal(0), QSettings().value(printMarginMmSetting,
                                                         defaultPrintMarginMm).toDouble(),
                             maximumPrintMarginMm);
    m_printFontPointSize = qBound(qreal(6),
                                  QSettings().value(printFontPointSizeSetting,
                                                    defaultPrintFontPointSize).toDouble(),
                                  qreal(32));

    m_wordTarget = qMax(0, QSettings().value(wordTargetSetting, 0).toInt());
    m_paragraphOnReturn =
        QSettings().value(paragraphOnReturnSetting, false).toBool();
    m_autosave = QSettings().value(autosaveSetting, true).toBool();
    m_autosaveDelayMs = qBound(minimumAutosaveDelayMs,
                               QSettings().value(autosaveDelaySetting,
                                                 defaultAutosaveDelayMs).toInt(),
                               maximumAutosaveDelayMs);
    m_recoveryTimer.setSingleShot(true);
    m_recoveryTimer.setInterval(m_autosaveDelayMs);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &Backend::persistDocument);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, this,
            [this](const QString &path) {
                const int tab = indexOfLocalPath(path);
                if (tab < 0)
                    return;
                if (tab != m_activeTab)
                    setActiveTab(tab);

                const bool deleted = !QFileInfo::exists(path);
                if (!deleted && m_hasKnownFileContents) {
                    QFile file(path);
                    if (file.open(QIODevice::ReadOnly)
                            && file.readAll() == m_lastKnownFileContents) {
                        // Atomic saves can replace the watched inode. Re-arm the
                        // watcher, but do not report our own save as an outside edit.
                        watchCurrentFile();
                        return;
                    }
                }

                // Whatever is on disk now, it is not what we last read, so the
                // baseline is unknown until the writer picks a version. A
                // deletion counts: the old text is not saved anywhere either.
                setKnownFileContents(QByteArray(), false);
                m_externalChangeUnanswered = true;
                emit externalChangeDetected(deleted, m_modified);
            });

    loadOmarchyTheme();
    watchOmarchyTheme();
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
}

Backend::~Backend() {
    if (!g_sessionWritesFrozen)
        persistSession();
    g_liveWindows.removeAll(this);
}

void Backend::setParentWindow(QWindow *window) {
    m_parentWindow = window;
    syncRepresentedFile();
}

void Backend::setNativeMacChrome(bool nativeMacChrome) {
    if (m_nativeMacChrome == nativeMacChrome)
        return;
    m_nativeMacChrome = nativeMacChrome;
    emit nativeMacChromeChanged();
}

void Backend::syncRepresentedFile() {
    if (!m_parentWindow)
        return;
    if (m_fileUrl.isLocalFile() && !m_fileUrl.toLocalFile().isEmpty())
        m_parentWindow->setFilePath(m_fileUrl.toLocalFile());
    else
        m_parentWindow->setFilePath(QString());
}

QWindow *Backend::parentWindow() const {
    return m_parentWindow;
}

QString Backend::fileName() const {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty())
        return QStringLiteral("Untitled.md");

    if (m_fileUrl.isLocalFile()) {
        const QFileInfo info(m_fileUrl.toLocalFile());
        if (!info.fileName().isEmpty())
            return info.fileName();
    }

    const QString name = m_fileUrl.fileName();
    return name.isEmpty() ? QStringLiteral("Untitled.md") : name;
}

QString Backend::sessionPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("session.json"));
}

QList<Backend *> Backend::liveWindows() {
    return g_liveWindows;
}

int Backend::storedWindowCount() {
    QFile sessionFile(sessionPath());
    if (!sessionFile.open(QIODevice::ReadOnly))
        return 0;
    return QJsonDocument::fromJson(sessionFile.readAll())
        .object()
        .value(QStringLiteral("windows"))
        .toArray()
        .size();
}

void Backend::prepareToQuit() {
    if (!g_liveWindows.isEmpty())
        g_liveWindows.constFirst()->persistSession();
    g_sessionWritesFrozen = true;
}

void Backend::allowSessionWrites() {
    g_sessionWritesFrozen = false;
}

bool Backend::isBlankUntitled() const {
    return m_tabs.size() == 1 && !m_modified
        && (!m_fileUrl.isValid() || m_fileUrl.isEmpty())
        && currentDocumentText().trimmed().isEmpty();
}

void Backend::ensureTab() {
    if (!m_tabs.isEmpty())
        return;
    DocumentTab tab;
    tab.untitledNumber = 1;
    m_tabs.append(tab);
    m_activeTab = 0;
}

int Backend::tabCount() const {
    return m_tabs.size();
}

QString Backend::tabTitle() const {
    if (m_activeTab < 0 || m_activeTab >= m_tabs.size())
        return QStringLiteral("Untitled");
    const DocumentTab &tab = m_tabs.at(m_activeTab);
    if (tab.untitledNumber > 0) {
        if (tab.untitledNumber == 1)
            return QStringLiteral("Untitled");
        return QStringLiteral("Untitled %1").arg(tab.untitledNumber);
    }
    return fileName();
}

QVariantList Backend::tabs() const {
    QVariantList list;
    for (int i = 0; i < m_tabs.size(); ++i) {
        const DocumentTab &tab = m_tabs.at(i);
        QString title;
        if (tab.untitledNumber > 1)
            title = QStringLiteral("Untitled %1").arg(tab.untitledNumber);
        else if (tab.untitledNumber == 1 || !tab.fileUrl.isLocalFile())
            title = QStringLiteral("Untitled");
        else
            title = QFileInfo(tab.fileUrl.toLocalFile()).fileName();
        if (title.isEmpty())
            title = QStringLiteral("Untitled");
        list.append(QVariantMap{
            {QStringLiteral("title"), title},
            {QStringLiteral("dirty"), tab.modified || (i == m_activeTab && m_modified)},
            {QStringLiteral("active"), i == m_activeTab},
        });
    }
    return list;
}

int Backend::nextUntitledNumber() const {
    int highest = 0;
    for (Backend *window : g_liveWindows) {
        for (const DocumentTab &tab : window->m_tabs)
            highest = qMax(highest, tab.untitledNumber);
    }
    return highest + 1;
}

int Backend::indexOfLocalPath(const QString &path) const {
    const QString canonical = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (!m_tabs.at(i).fileUrl.isLocalFile())
            continue;
        if (QFileInfo(m_tabs.at(i).fileUrl.toLocalFile()).absoluteFilePath() == canonical)
            return i;
    }
    return -1;
}

void Backend::storeTabFields(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;
    DocumentTab &tab = m_tabs[index];
    tab.fileUrl = m_fileUrl;
    tab.cachedText = currentDocumentText();
    tab.modified = m_modified;
    tab.pathNeverRead = m_pathNeverRead;
    tab.lastKnownFileContents = m_lastKnownFileContents;
    tab.lastKnownFileText = m_lastKnownFileText;
    tab.hasKnownFileContents = m_hasKnownFileContents;
    tab.externalChangeUnanswered = m_externalChangeUnanswered;
    if (tab.fileUrl.isLocalFile() && !tab.fileUrl.toLocalFile().isEmpty())
        tab.untitledNumber = 0;
}

void Backend::loadTabFields(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;
    const DocumentTab &tab = m_tabs.at(index);
    m_pathNeverRead = tab.pathNeverRead;
    m_externalChangeUnanswered = tab.externalChangeUnanswered;
    setKnownFileContents(tab.lastKnownFileContents, tab.hasKnownFileContents);
    m_lastKnownFileText = tab.lastKnownFileText;
    setFileUrl(tab.fileUrl);
    loadDocumentText(tab.cachedText);
    setModified(tab.modified);
}

void Backend::newTab() {
    storeTabFields(m_activeTab);
    DocumentTab tab;
    tab.untitledNumber = nextUntitledNumber();
    m_tabs.append(tab);
    m_activeTab = m_tabs.size() - 1;
    loadTabFields(m_activeTab);
    watchCurrentFile();
    persistSession();
    emit tabsChanged();
}

bool Backend::closeTab(int index) {
    if (index < 0 || index >= m_tabs.size())
        return false;
    storeTabFields(m_activeTab);
    if (m_tabs.size() == 1) {
        m_tabs.clear();
        g_liveWindows.removeAll(this);
        persistSession();
        emit closeWindowRequested();
        emit tabsChanged();
        return true;
    }
    m_tabs.removeAt(index);
    if (m_activeTab >= m_tabs.size())
        m_activeTab = m_tabs.size() - 1;
    else if (m_activeTab > index)
        --m_activeTab;
    loadTabFields(m_activeTab);
    watchCurrentFile();
    persistSession();
    emit tabsChanged();
    return false;
}

void Backend::setActiveTab(int index) {
    if (index < 0 || index >= m_tabs.size() || index == m_activeTab)
        return;
    storeTabFields(m_activeTab);
    m_activeTab = index;
    loadTabFields(m_activeTab);
    watchCurrentFile();
    emit tabsChanged();
}

void Backend::adoptTabFrom(QObject *sourceWindow, int index) {
    auto *source = qobject_cast<Backend *>(sourceWindow);
    if (!source || source == this)
        return;
    if (index < 0 || index >= source->m_tabs.size())
        return;
    source->storeTabFields(source->m_activeTab);
    const DocumentTab tab = source->m_tabs.at(index);
    if (tab.fileUrl.isLocalFile() && indexOfLocalPath(tab.fileUrl.toLocalFile()) >= 0) {
        setActiveTab(indexOfLocalPath(tab.fileUrl.toLocalFile()));
        source->closeTab(index);
        return;
    }
    storeTabFields(m_activeTab);
    m_tabs.append(tab);
    source->m_tabs.removeAt(index);
    if (source->m_tabs.isEmpty()) {
        g_liveWindows.removeAll(source);
        emit source->closeWindowRequested();
    } else {
        if (source->m_activeTab >= source->m_tabs.size())
            source->m_activeTab = source->m_tabs.size() - 1;
        else if (source->m_activeTab > index)
            --source->m_activeTab;
        source->loadTabFields(source->m_activeTab);
        emit source->tabsChanged();
    }
    m_activeTab = m_tabs.size() - 1;
    loadTabFields(m_activeTab);
    watchCurrentFile();
    persistSession();
    emit tabsChanged();
}

void Backend::takeDetachedTab(QObject *sourceWindow, int index) {
    if (isBlankUntitled())
        m_tabs.clear();
    adoptTabFrom(sourceWindow, index);
}

void Backend::moveTab(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= m_tabs.size() || to >= m_tabs.size())
        return;
    storeTabFields(m_activeTab);
    m_tabs.move(from, to);
    if (m_activeTab == from)
        m_activeTab = to;
    else if (from < m_activeTab && to >= m_activeTab)
        --m_activeTab;
    else if (from > m_activeTab && to <= m_activeTab)
        ++m_activeTab;
    persistSession();
    emit tabsChanged();
}

void Backend::finishTabDrag(int index, qreal globalX, qreal globalY) {
    if (index < 0 || index >= m_tabs.size())
        return;
    const QPoint point(qRound(globalX), qRound(globalY));
    Backend *hit = nullptr;
    for (Backend *window : g_liveWindows) {
        if (!window->m_parentWindow)
            continue;
        if (window->m_parentWindow->geometry().contains(point)) {
            hit = window;
            break;
        }
    }
    if (!hit) {
        emit detachTabRequested(index);
        return;
    }
    if (hit == this)
        return;
    hit->adoptTabFrom(this, index);
}

void Backend::setDarkMode(bool darkMode) {
    if (m_darkMode == darkMode)
        return;

    m_darkMode = darkMode;
    loadOmarchyTheme();
    emit darkModeChanged();
}

void Backend::setTextScale(qreal textScale) {
    if (qFuzzyCompare(m_textScale, textScale))
        return;

    m_textScale = textScale;
    emit textScaleChanged();
}

void Backend::setEditorFontSize(int editorFontSize) {
    const int boundedSize = qBound(minimumEditorFontSize, editorFontSize,
                                   maximumEditorFontSize);
    if (m_editorFontSize == boundedSize)
        return;

    m_editorFontSize = boundedSize;
    QSettings().setValue(editorFontSizeSetting, m_editorFontSize);
    emit editorFontSizeChanged();
}

namespace {
// Anything larger is not something to inline into a document being read.
constexpr qint64 maximumEmbeddedBytes = 512 * 1024;

const QStringList &imageExtensions() {
    static const QStringList extensions{
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("bmp"),
        QStringLiteral("svg")};
    return extensions;
}

const QStringList &proseExtensions() {
    static const QStringList extensions{QStringLiteral("txt"), QStringLiteral("md"),
                                        QStringLiteral("markdown"), QStringLiteral("text")};
    return extensions;
}

const QStringList &tableExtensions() {
    static const QStringList extensions{QStringLiteral("csv"), QStringLiteral("tsv")};
    return extensions;
}

// One row of delimited text, honouring quotes so a field may hold the
// delimiter. Deliberately small: a spreadsheet is not a document.
QStringList splitDelimited(const QString &line, QChar delimiter) {
    QStringList fields;
    QString field;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar character = line.at(i);
        if (quoted) {
            if (character == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    field.append(QLatin1Char('"'));
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.append(character);
            }
        } else if (character == QLatin1Char('"')) {
            quoted = true;
        } else if (character == delimiter) {
            fields << field;
            field.clear();
        } else {
            field.append(character);
        }
    }
    fields << field;
    return fields;
}

QString tableFrom(const QString &contents, QChar delimiter) {
    const QStringList lines = contents.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty())
        return {};

    QStringList rendered;
    for (int row = 0; row < lines.size(); ++row) {
        QStringList fields = splitDelimited(lines.at(row).trimmed(), delimiter);
        for (QString &field : fields)
            field = field.trimmed().replace(QLatin1Char('|'), QStringLiteral("\\|"));
        rendered << QStringLiteral("| ") + fields.join(QStringLiteral(" | "))
                        + QStringLiteral(" |");
        if (row == 0) {
            QStringList rule;
            for (int i = 0; i < fields.size(); ++i)
                rule << QStringLiteral("---");
            rendered << QStringLiteral("| ") + rule.join(QStringLiteral(" | "))
                            + QStringLiteral(" |");
        }
    }
    return rendered.join(QLatin1Char('\n'));
}
}

bool Backend::contentBlockOnLine(const QString &line, ContentBlock *block) {
    // A path, alone, optionally followed by a caption in quotes or brackets.
    static const QRegularExpression blockRe(QStringLiteral(
        "^\\s*([^\"'()\\s][^\"'()]*\\.[A-Za-z0-9]{1,10})"
        "(?:\\s+(?:\"([^\"]*)\"|'([^']*)'|\\(([^)]*)\\)))?\\s*$"));

    const QRegularExpressionMatch match = blockRe.match(line);
    if (!match.hasMatch())
        return false;

    const QString path = match.captured(1).trimmed();
    // A URL is not a file, and an absolute path is not inside the document's
    // folder, which is the only place a block is allowed to reach.
    if (path.contains(QStringLiteral("://")) || path.startsWith(QLatin1Char('/'))
            || path.startsWith(QLatin1Char('~'))) {
        return false;
    }

    if (block) {
        block->path = path;
        block->caption = match.captured(2) + match.captured(3) + match.captured(4);
    }
    return true;
}

namespace {

bool walkContentBlockLines(const QStringList &lines, int i, bool *insideFence) {
    const QString trimmed = lines.at(i).trimmed();
    if (trimmed.startsWith(QStringLiteral("```"))
            || trimmed.startsWith(QStringLiteral("~~~"))) {
        *insideFence = !*insideFence;
        return false;
    }
    if (*insideFence)
        return false;
    const bool aloneAbove = i == 0 || lines.at(i - 1).trimmed().isEmpty();
    const bool aloneBelow = i + 1 >= lines.size()
        || lines.at(i + 1).trimmed().isEmpty();
    return aloneAbove && aloneBelow;
}

QString liveOrFileText(const QString &canonicalPath, const QString &absolutePath,
                        const QVariantMap &liveTexts) {
    if (liveTexts.contains(canonicalPath))
        return liveTexts.value(canonicalPath).toString();
    if (!absolutePath.isEmpty() && liveTexts.contains(absolutePath))
        return liveTexts.value(absolutePath).toString();
    return {};
}

QStringList contentBlockIncludePaths(const QString &markdown, const QDir &directory,
                                      const QString &root) {
    QStringList names;
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    bool insideFence = false;
    for (int i = 0; i < lines.size(); ++i) {
        if (!walkContentBlockLines(lines, i, &insideFence))
            continue;
        Backend::ContentBlock block;
        if (!Backend::contentBlockOnLine(lines.at(i), &block))
            continue;
        const QString candidate =
            QFileInfo(directory.filePath(block.path)).canonicalFilePath();
        if (candidate.isEmpty()
                || !(candidate == root || candidate.startsWith(root + QLatin1Char('/')))) {
            continue;
        }
        if (!names.contains(block.path))
            names.append(block.path);
    }
    return names;
}

QStringList contentBlockIncludeCanonicals(const QString &markdown, const QDir &directory,
                                           const QString &root) {
    QStringList canonicals;
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    bool insideFence = false;
    for (int i = 0; i < lines.size(); ++i) {
        if (!walkContentBlockLines(lines, i, &insideFence))
            continue;
        Backend::ContentBlock block;
        if (!Backend::contentBlockOnLine(lines.at(i), &block))
            continue;
        const QString candidate =
            QFileInfo(directory.filePath(block.path)).canonicalFilePath();
        if (candidate.isEmpty()
                || !(candidate == root || candidate.startsWith(root + QLatin1Char('/')))) {
            continue;
        }
        if (!canonicals.contains(candidate))
            canonicals.append(candidate);
    }
    return canonicals;
}

} // namespace

QString Backend::expandContentBlocks(const QString &markdown,
                                     const QString &documentDirectory,
                                     const QVariantMap &liveTexts) {
    if (documentDirectory.isEmpty() || !markdown.contains(QLatin1Char('.')))
        return markdown;

    const QDir directory(documentDirectory);
    const QString root = QDir(directory.absolutePath()).canonicalPath();
    if (root.isEmpty())
        return markdown;

    QStringList lines = markdown.split(QLatin1Char('\n'));
    bool insideFence = false;
    for (int i = 0; i < lines.size(); ++i) {
        if (!walkContentBlockLines(lines, i, &insideFence))
            continue;

        ContentBlock block;
        if (!contentBlockOnLine(lines.at(i), &block))
            continue;

        const QFileInfo relative(directory.filePath(block.path));
        const QString candidate = relative.canonicalFilePath();
        // Outside the document's folder, or not there at all: leave the line
        // exactly as the writer typed it.
        if (candidate.isEmpty()
                || !(candidate == root || candidate.startsWith(root + QLatin1Char('/')))) {
            continue;
        }

        const QFileInfo info(candidate);
        const bool hasLive = liveTexts.contains(candidate)
            || liveTexts.contains(relative.absoluteFilePath());
        const QString live = liveOrFileText(candidate, relative.absoluteFilePath(), liveTexts);
        if (!hasLive && (!info.isFile() || info.size() > maximumEmbeddedBytes))
            continue;

        const QString suffix = info.suffix().toLower();
        if (imageExtensions().contains(suffix)) {
            lines[i] = QStringLiteral("![%1](%2)")
                           .arg(block.caption, QUrl::fromLocalFile(candidate).toString());
            continue;
        }

        QString contents = live;
        if (!hasLive) {
            QFile file(candidate);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            contents = QString::fromUtf8(file.readAll());
        }

        QString replacement;
        if (proseExtensions().contains(suffix)) {
            replacement = contents;
        } else if (tableExtensions().contains(suffix)) {
            replacement = tableFrom(contents, suffix == QStringLiteral("tsv")
                                                  ? QLatin1Char('\t')
                                                  : QLatin1Char(','));
        } else {
            // Everything else is code, fenced under its own extension so the
            // highlighter can read it.
            replacement = QStringLiteral("```%1\n%2\n```").arg(suffix, contents.trimmed());
        }

        if (replacement.isEmpty())
            continue;

        if (!block.caption.isEmpty() && !proseExtensions().contains(suffix))
            replacement += QStringLiteral("\n\n*%1*").arg(block.caption);

        lines[i] = replacement;
    }

    return lines.join(QLatin1Char('\n'));
}

bool Backend::canAssembleThisFolder() const {
    if (!m_fileUrl.isLocalFile())
        return false;
    return QFileInfo(QFileInfo(m_fileUrl.toLocalFile()).absolutePath()).isDir();
}

QVariantMap Backend::liveDocumentTexts() const {
    QVariantMap texts;
    for (Backend *window : g_liveWindows) {
        for (int i = 0; i < window->m_tabs.size(); ++i) {
            const DocumentTab &tab = window->m_tabs.at(i);
            if (!tab.fileUrl.isLocalFile())
                continue;
            QString key = QFileInfo(tab.fileUrl.toLocalFile()).canonicalFilePath();
            if (key.isEmpty())
                key = QFileInfo(tab.fileUrl.toLocalFile()).absoluteFilePath();
            if (key.isEmpty())
                continue;
            const QString text = i == window->m_activeTab
                ? window->currentDocumentText()
                : tab.cachedText;
            texts.insert(key, text);
        }
    }
    return texts;
}

QVariantMap Backend::assembleFolder(const QString &directoryPath,
                                      const QVariantMap &liveTexts) {
    QVariantMap result{{QStringLiteral("ok"), false},
                       {QStringLiteral("markdown"), QString()},
                       {QStringLiteral("wordCount"), 0},
                       {QStringLiteral("usesUnsavedWork"), false},
                       {QStringLiteral("roots"), QVariantList()},
                       {QStringLiteral("suggestedSaveUrl"), QUrl()},
                       {QStringLiteral("suggestedPdfUrl"), QUrl()}};

    const QDir directory(directoryPath);
    const QString root = QDir(directory.absolutePath()).canonicalPath();
    if (root.isEmpty()) {
        result.insert(QStringLiteral("error"), QStringLiteral("Could not read the folder."));
        return result;
    }

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    QFileInfoList entries = directory.entryInfoList(
        {QStringLiteral("*.md"), QStringLiteral("*.markdown")}, QDir::Files, QDir::NoSort);
    std::sort(entries.begin(), entries.end(), [&](const QFileInfo &a, const QFileInfo &b) {
        return collator.compare(a.fileName(), b.fileName()) < 0;
    });

    struct MarkdownFile {
        QString fileName;
        QString canonical;
        QString text;
        QStringList includeNames;
        QStringList includeCanonical;
        bool fromLive = false;
    };
    QVector<MarkdownFile> files;

    for (const QFileInfo &info : entries) {
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty())
            continue;

        const QString live = liveOrFileText(canonical, info.absoluteFilePath(), liveTexts);
        const bool fromLive = liveTexts.contains(canonical)
            || liveTexts.contains(info.absoluteFilePath());
        if (!fromLive && info.size() > maximumEmbeddedBytes) {
            result.insert(QStringLiteral("error"),
                          QStringLiteral("%1 is too large to assemble.").arg(info.fileName()));
            return result;
        }

        QString text = live;
        if (!fromLive) {
            QFile file(canonical);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                result.insert(QStringLiteral("error"),
                              QStringLiteral("Could not read %1.").arg(info.fileName()));
                return result;
            }
            text = QString::fromUtf8(file.readAll());
        }

        MarkdownFile item;
        item.fileName = info.fileName();
        item.canonical = canonical;
        item.text = text;
        item.includeNames = contentBlockIncludePaths(text, directory, root);
        item.includeCanonical = contentBlockIncludeCanonicals(text, directory, root);
        item.fromLive = fromLive;
        files.append(item);
    }

    QSet<QString> included;
    for (const MarkdownFile &item : files) {
        for (const QString &canonical : item.includeCanonical)
            included.insert(canonical);
    }

    QVector<MarkdownFile> roots;
    for (const MarkdownFile &item : files) {
        if (!included.contains(item.canonical))
            roots.append(item);
    }
    if (roots.isEmpty())
        roots = files;

    QStringList pieces;
    QVariantList rootMaps;
    bool usesUnsavedWork = false;

    for (const MarkdownFile &item : roots) {
        pieces << expandContentBlocks(item.text, directory.absolutePath(), liveTexts).trimmed();
        QVariantMap row{{QStringLiteral("fileName"), item.fileName},
                        {QStringLiteral("includes"), item.includeNames}};
        rootMaps.append(row);
        if (item.fromLive) {
            QFile disk(item.canonical);
            QString diskText;
            if (disk.open(QIODevice::ReadOnly | QIODevice::Text))
                diskText = QString::fromUtf8(disk.readAll());
            if (diskText != item.text)
                usesUnsavedWork = true;
        }
        for (const QString &canonical : item.includeCanonical) {
            if (!liveTexts.contains(canonical))
                continue;
            QFile disk(canonical);
            QString diskText;
            if (disk.open(QIODevice::ReadOnly | QIODevice::Text))
                diskText = QString::fromUtf8(disk.readAll());
            if (diskText != liveTexts.value(canonical).toString())
                usesUnsavedWork = true;
        }
    }

    const QString markdown = pieces.join(QStringLiteral("\n\n"));
    const QString folderName = QFileInfo(root).fileName();
    const QString parent = QFileInfo(root).absolutePath();
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("markdown"), markdown);
    result.insert(QStringLiteral("wordCount"), countWords(markdown));
    result.insert(QStringLiteral("usesUnsavedWork"), usesUnsavedWork);
    result.insert(QStringLiteral("roots"), rootMaps);
    result.insert(QStringLiteral("directoryName"), folderName);
    result.insert(QStringLiteral("suggestedSaveUrl"),
                 QUrl::fromLocalFile(QDir(parent).filePath(folderName + QStringLiteral(".md"))));
    result.insert(QStringLiteral("suggestedPdfUrl"),
                 QUrl::fromLocalFile(QDir(parent).filePath(folderName + QStringLiteral(".pdf"))));
    return result;
}

QVariantMap Backend::assembleThisFolder() {
    if (!canAssembleThisFolder()) {
        setStatus(QStringLiteral("Save the document first so it has a folder."));
        return QVariantMap{{QStringLiteral("ok"), false}};
    }

    QVariantMap result = assembleFolder(
        QFileInfo(m_fileUrl.toLocalFile()).absolutePath(), liveDocumentTexts());
    if (!result.value(QStringLiteral("ok")).toBool()) {
        const QString error = result.value(QStringLiteral("error")).toString();
        if (!error.isEmpty())
            setStatus(error);
    }
    return result;
}

bool Backend::saveAssembledMarkdown(const QUrl &url, const QString &markdown) {
    if (!url.isLocalFile())
        return false;
    const QString path = url.toLocalFile();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(markdown.toUtf8());
    if (!file.commit())
        return false;
    setStatus(QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()));
    return true;
}

bool Backend::saveAssembledPdf(const QUrl &url, const QString &markdown) {
    if (!url.isLocalFile())
        return false;
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(url.toLocalFile());
    printer.setDocName(printJobName(QFileInfo(url.toLocalFile()).fileName()));
    printRenderedMarkdown(&printer, markdown);
    if (!QFileInfo::exists(url.toLocalFile()) || QFileInfo(url.toLocalFile()).size() == 0)
        return false;
    setStatus(QStringLiteral("Saved %1").arg(QFileInfo(url.toLocalFile()).fileName()));
    return true;
}

void Backend::printAssembledMarkdown(const QString &markdown) {
    if (markdown.isEmpty()) {
        setStatus(QStringLiteral("There is nothing to print."));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    const QString job = canAssembleThisFolder()
        ? QFileInfo(QFileInfo(m_fileUrl.toLocalFile()).absolutePath()).fileName() + QStringLiteral(".md")
        : QStringLiteral("Omawrite.md");
    printer.setDocName(printJobName(job));
    QPrintDialog dialog(&printer);
    dialog.setWindowTitle(QStringLiteral("Print assembled folder"));
    dialog.winId();
    if (dialog.windowHandle() && m_parentWindow)
        dialog.windowHandle()->setTransientParent(m_parentWindow);
    if (dialog.exec() == QDialog::Accepted)
        printRenderedMarkdown(&printer, markdown);
}

void Backend::printRenderedMarkdown(QPrinter *printer, const QString &markdown) const {
    QPageLayout layout = printer->pageLayout();
    layout.setUnits(QPageLayout::Millimeter);
    layout.setMargins(QMarginsF(m_printMarginMm, m_printMarginMm,
                                m_printMarginMm, m_printMarginMm));
    printer->setPageLayout(layout);

    QTextDocument rendered;
    QFont printed(m_editorFontFamily);
    printed.setPointSizeF(m_printFontPointSize);
    rendered.setDefaultFont(printed);
    rendered.setMarkdown(markdown);
    styleRenderedDocument(&rendered, true);
    rendered.print(printer);
}

QStringList Backend::bundledFontFamilies() {
    return {QStringLiteral("iA Writer Quattro S"),
            QStringLiteral("iA Writer Duo S"),
            QStringLiteral("iA Writer Mono S")};
}

QStringList Backend::availableFontFamilies() {
    QStringList families = QFontDatabase::families();
    // The app's own faces lead the list. Otherwise they sit somewhere in the
    // middle of a few hundred system families and read as unavailable.
    for (const QString &bundled : bundledFontFamilies())
        families.removeAll(bundled);
    return bundledFontFamilies() + families;
}

QString Backend::resolveFontFamily(const QString &requested,
                                   const QStringList &availableFamilies) {
    if (!requested.isEmpty() && availableFamilies.contains(requested))
        return requested;

    return bundledFontFamily;
}

void Backend::setEditorFontFamily(const QString &family) {
    const QString resolved = resolveFontFamily(family, QFontDatabase::families());
    if (m_editorFontFamily == resolved)
        return;

    m_editorFontFamily = resolved;
    QSettings().setValue(editorFontFamilySetting, m_editorFontFamily);
    if (m_document) {
        QFont font = m_document->defaultFont();
        font.setFamily(m_editorFontFamily);
        m_document->setDefaultFont(font);
    }
    emit editorFontFamilyChanged();
}

void Backend::setCaretStyle(const QString &caretStyle) {
    const QString normalized = caretStyle == QStringLiteral("block")
        ? QStringLiteral("block")
        : QStringLiteral("line");
    if (m_caretStyle == normalized)
        return;

    m_caretStyle = normalized;
    QSettings().setValue(caretStyleSetting, m_caretStyle);
    emit caretStyleChanged();
}

void Backend::setCaretBlink(bool caretBlink) {
    if (m_caretBlink == caretBlink)
        return;

    m_caretBlink = caretBlink;
    QSettings().setValue(caretBlinkSetting, m_caretBlink);
    emit caretBlinkChanged();
}

void Backend::setEditorMeasureChars(int measureChars) {
    const int bounded = qBound(minimumEditorMeasureChars, measureChars,
                               maximumEditorMeasureChars);
    if (m_editorMeasureChars == bounded)
        return;

    m_editorMeasureChars = bounded;
    QSettings().setValue(editorMeasureCharsSetting, m_editorMeasureChars);
    emit editorMeasureCharsChanged();
}

void Backend::setPrintMarginMm(qreal marginMm) {
    const qreal bounded = qBound(qreal(0), marginMm, maximumPrintMarginMm);
    if (qFuzzyCompare(m_printMarginMm, bounded))
        return;

    m_printMarginMm = bounded;
    QSettings().setValue(printMarginMmSetting, m_printMarginMm);
    emit printMarginMmChanged();
}

QString Backend::appVersion() {
#ifdef OMAWRITE_VERSION
    return QStringLiteral(OMAWRITE_VERSION);
#else
    return QStringLiteral("unknown");
#endif
}

QString Backend::appCommit() {
#ifdef OMAWRITE_COMMIT
    return QStringLiteral(OMAWRITE_COMMIT);
#else
    return QStringLiteral("unknown");
#endif
}

QUrl Backend::appCommitUrl() {
#ifdef OMAWRITE_REMOTE
    const QString commit = appCommit();
    const QString remote = QStringLiteral(OMAWRITE_REMOTE);
    // A modified tree is marked with a trailing +. Linking it would point at a
    // page whose code is not what is running, which is worse than not linking.
    if (remote.isEmpty() || commit.isEmpty()
            || commit == QStringLiteral("unknown")
            || commit.endsWith(QLatin1Char('+'))) {
        return {};
    }

    return QUrl(remote + QStringLiteral("/commit/") + commit);
#else
    return {};
#endif
}

int Backend::draftTargetFor(int wordTarget) {
    if (wordTarget <= 0)
        return 0;

    // A first draft wants a quarter more than the finished piece, so there is
    // something to throw away rather than something to pad.
    return qRound(wordTarget * 1.25);
}

void Backend::setWordTarget(int wordTarget) {
    const int bounded = qMax(0, wordTarget);
    if (m_wordTarget == bounded)
        return;

    m_wordTarget = bounded;
    QSettings().setValue(wordTargetSetting, m_wordTarget);
    emit wordTargetChanged();
}

void Backend::setParagraphOnReturn(bool paragraphOnReturn) {
    if (m_paragraphOnReturn == paragraphOnReturn)
        return;

    m_paragraphOnReturn = paragraphOnReturn;
    QSettings().setValue(paragraphOnReturnSetting, m_paragraphOnReturn);
    emit paragraphOnReturnChanged();
}

void Backend::setPrintFontPointSize(qreal pointSize) {
    const qreal bounded = qBound(qreal(6), pointSize, qreal(32));
    if (qFuzzyCompare(m_printFontPointSize, bounded))
        return;

    m_printFontPointSize = bounded;
    QSettings().setValue(printFontPointSizeSetting, m_printFontPointSize);
    emit printFontPointSizeChanged();
}

void Backend::setAutosave(bool autosave) {
    if (m_autosave == autosave)
        return;

    m_autosave = autosave;
    QSettings().setValue(autosaveSetting, m_autosave);
    emit autosaveChanged();
}

void Backend::setAutosaveDelayMs(int autosaveDelayMs) {
    const int bounded = qBound(minimumAutosaveDelayMs, autosaveDelayMs,
                               maximumAutosaveDelayMs);
    if (m_autosaveDelayMs == bounded)
        return;

    m_autosaveDelayMs = bounded;
    m_recoveryTimer.setInterval(m_autosaveDelayMs);
    QSettings().setValue(autosaveDelaySetting, m_autosaveDelayMs);
    emit autosaveDelayMsChanged();
}

void Backend::resetEditorFontSize() {
    setEditorFontSize(defaultEditorFontSize);
}

void Backend::attachDocument(QObject *textDocument) {
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    if (!quickDocument || !quickDocument->textDocument()) {
        setStatus(QStringLiteral("Could not attach the Markdown renderer."));
        return;
    }

    if (m_highlighter)
        delete m_highlighter.data();

    m_document = quickDocument->textDocument();
    m_lastDocumentText = m_document->toPlainText();
    m_highlighter = new MarkdownHighlighter(m_document);
    m_highlighter->setDarkMode(m_darkMode);
    m_highlighter->setColors(m_themeBackground, m_themeForeground, m_themeAccent,
                             m_themeCodeBackground);

    connect(m_document, &QTextDocument::contentsChange, this,
            [this](int position, int, int charsAdded) {
                if (m_formattingTypography || m_loading)
                    return;
                m_lastChangePos = position;
                m_lastChangeAdded = charsAdded;
            });

    applyDocumentTypography();
    restoreRecovery();
}

void Backend::openDialog() {
    emit openDialogRequested();
}

void Backend::open(const QUrl &url) {
    if (url.isLocalFile()) {
        const QString path = url.toLocalFile();
        const int here = indexOfLocalPath(path);
        if (here >= 0) {
            setActiveTab(here);
            return;
        }
        for (Backend *window : g_liveWindows) {
            if (window == this)
                continue;
            const int there = window->indexOfLocalPath(path);
            if (there >= 0) {
                window->setActiveTab(there);
                if (window->m_parentWindow)
                    window->m_parentWindow->requestActivate();
                return;
            }
        }
        const bool replaceBlank = m_tabs.size() == 1 && !m_modified
            && (!m_fileUrl.isValid() || m_fileUrl.isEmpty())
            && currentDocumentText().trimmed().isEmpty();
        const int previousTab = m_activeTab;
        if (!replaceBlank) {
            storeTabFields(m_activeTab);
            DocumentTab tab;
            tab.untitledNumber = nextUntitledNumber();
            m_tabs.append(tab);
            m_activeTab = m_tabs.size() - 1;
            loadTabFields(m_activeTab);
        }
        openPath(url, true);
        if (m_status.startsWith(QStringLiteral("Could not")) && !replaceBlank) {
            const QString error = m_status;
            m_tabs.removeLast();
            m_activeTab = previousTab;
            loadTabFields(m_activeTab);
            setStatus(error);
        }
    } else {
        openPath(url, true);
    }
    storeTabFields(m_activeTab);
    persistSession();
    emit tabsChanged();
}

void Backend::openPath(const QUrl &url, bool mayStartNewFile) {
    if (!url.isLocalFile()) {
        setStatus(QStringLiteral("Only local files can be opened."));
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QFile file(url.toLocalFile());
    // A path that is not there yet is a file the writer means to start, so
    // take the name for a blank document. The first save then lands where
    // they said it should, instead of asking them again.
    if (mayStartNewFile && !file.exists()) {
        // Only where it could be written: a name under a directory that is not
        // there leaves the first save with nowhere to land and no dialog.
        const QFileInfo parentDirectory(QFileInfo(url.toLocalFile()).absolutePath());
        if (!parentDirectory.isDir() || !parentDirectory.isWritable()) {
            setStatus(QStringLiteral("Could not open %1.").arg(targetName));
            return;
        }

        loadDocumentText(QString());
        clearRecovery();
        m_lastKnownFileContents.clear();
        m_hasKnownFileContents = false;
        m_pathNeverRead = true;
        m_externalChangeUnanswered = false;
        setFileUrl(url);
        setModified(false);
        setStatus(QStringLiteral("New file %1").arg(fileName()));
        return;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // A reload with nothing left to read leaves this document holding a
        // name and no file, and the watcher let the path go when it went.
        // That is the state a new file starts in, so say so: if the file
        // comes back, the next save asks rather than replacing it unseen.
        if (!mayStartNewFile && !file.exists())
            m_pathNeverRead = true;

        setStatus(QStringLiteral("Could not open %1.").arg(targetName));
        return;
    }

    const QByteArray contents = file.readAll();
    loadDocumentText(QString::fromUtf8(contents));
    clearRecovery();
    setKnownFileContents(contents, true);
    m_pathNeverRead = false;
    m_externalChangeUnanswered = false;
    setFileUrl(url);
    watchCurrentFile();
    setModified(false);
    setStatus(QStringLiteral("Opened %1").arg(fileName()));
}

void Backend::save() {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty()) {
        saveAsDialog();
        return;
    }

    // Nothing can watch a file that is not there, so a name taken for a file
    // that has yet to be written is unguarded until this save: a `git pull` or
    // a sync client can put something on that path in the meantime and
    // QSaveFile::commit() would replace it without a word. Ask once, and only
    // once -- the flag is cleared by every answer the dialog can give, so a
    // file that turns out to be unreadable cannot leave the writer trapped in
    // a question they have already answered.
    if (m_pathNeverRead && m_fileUrl.isLocalFile()
            && QFileInfo::exists(m_fileUrl.toLocalFile())) {
        m_closeAfterSave = false;
        emit externalFileAppeared(m_modified);
        return;
    }

    saveTo(m_fileUrl);
}

void Backend::saveForClose() {
    if (!m_modified) {
        emit closeAfterSave();
        return;
    }

    m_closeAfterSave = true;
    save();
}

void Backend::saveAsDialog() {
    emit saveDialogRequested(suggestedSaveUrl());
}

void Backend::saveAs(const QUrl &url) {
    saveTo(url);
}

void Backend::fileDialogCanceled() {
    m_closeAfterSave = false;
}

void Backend::discardRecovery() {
    clearRecovery();
}

void Backend::reloadFromDisk() {
    // Reload is asked for a file we already have, so it must not go down the
    // path that takes an absent name for a new document: if the file goes away
    // between the "File changed" dialog opening and the click, blanking the
    // editor and clearing recovery would throw away the only copy left. Say
    // it could not be opened and leave the text where it is.
    if (m_fileUrl.isLocalFile())
        openPath(m_fileUrl, false);
}

void Backend::keepExternalVersion() {
    m_externalChangeUnanswered = false;
    QFile file(m_fileUrl.toLocalFile());
    if (file.open(QIODevice::ReadOnly)) {
        setKnownFileContents(file.readAll(), true);
    } else {
        setKnownFileContents(QByteArray(), false);
    }
    // Answered, whether or not the file could be read. Failing to read it is
    // not a reason to ask again: the writer said to keep their version, and
    // the next save must be allowed to try, so the filesystem gets to give
    // the answer instead of the dialog asking the same question forever.
    m_pathNeverRead = false;
    setModified(true);
    scheduleRecovery();
    watchCurrentFile();
    setStatus(QStringLiteral("Kept your version"));
}

QFont Backend::printFont(const QFont &editorFont, qreal screenDpi) {
    QFont font = editorFont;
    if (font.pixelSize() <= 0)
        return font;

    // A pixel size is a screen measurement. Handing it to a HighResolution
    // QPrinter makes the printer read it as 1200-dpi dots, so an 18px font
    // prints 18/1200 of an inch tall. Points are resolution-independent.
    const qreal dpi = screenDpi > 0.0 ? screenDpi : 96.0;
    font.setPointSizeF(font.pixelSize() * 72.0 / dpi);
    return font;
}

namespace {
bool isCodeBlock(const QTextBlock &block) {
    if (!block.isValid())
        return false;

    const QTextBlockFormat format = block.blockFormat();
    return format.hasProperty(QTextFormat::BlockCodeLanguage)
        || format.hasProperty(QTextFormat::BlockCodeFence);
}
}

void Backend::styleRenderedDocument(QTextDocument *document, bool forPrint) const {
    if (!document)
        return;

    // Qt's Markdown reader packs every block flush against the next, so the
    // blank lines that separate paragraphs in the source disappear from the
    // render. Put that breathing room back as real block spacing, sized from
    // the font so it tracks the desktop text scale.
    //
    // The gap is one line of the editor's own leading, because that is exactly
    // what a blank line between two paragraphs is in the source.
    const qreal gap = QFontMetricsF(document->defaultFont()).height()
        * typoraLineHeightPercent / 100.0;

    // Paper is white however the app is themed, so a printed page takes the
    // light palette throughout -- panel and highlighting both. On screen the
    // preview follows the editor.
    const bool darkPalette = forPrint ? false : m_darkMode;
    const QColor panel = forPrint ? QColor(QStringLiteral("#edece8"))
                                  : QColor(m_themeCodeBackground);
    // A printed accent has to hold up on white; on screen it is the theme's.
    const QColor accent = forPrint ? QColor(QStringLiteral("#0b5aa6"))
                                   : QColor(m_themeAccent);
    const QColor muted = forPrint ? QColor(QStringLiteral("#6b7280"))
                                  : QColor(m_themeForeground).lighter(160);

    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        const bool heading = format.headingLevel() > 0;
        const bool listItem = block.textList() != nullptr;
        const bool quote = format.intProperty(QTextFormat::BlockQuoteLevel) > 0;
        const bool code = format.hasProperty(QTextFormat::BlockCodeLanguage)
            || format.hasProperty(QTextFormat::BlockCodeFence);

        if (code) {
            // Every line of a fence is its own block, so the panel is only
            // continuous if the blocks touch: no margins between them, and
            // none of the prose leading, which leaves a bare stripe.
            const bool opensRun = !isCodeBlock(block.previous());
            const bool closesRun = !isCodeBlock(block.next());
            format.setTopMargin(opensRun ? gap : 0);
            format.setBottomMargin(closesRun ? gap : 0);
            format.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
            // Qt marks code non-breakable, which on a page means a long line
            // runs past the measure and off the paper: the text is cut and the
            // panel, which only ever spans the measure, stops short of it.
            // Wrapping keeps the line on the page and inside the panel.
            format.setNonBreakableLines(false);
            if (panel.isValid())
                format.setBackground(panel);
        } else {
            // Headings lead their section, list items stay tight together, and
            // ordinary paragraphs get a full blank line's worth beneath them.
            // Quotes need the leading gap too or they read as another list row.
            format.setTopMargin(heading ? gap * 1.5 : (quote ? gap : 0));
            format.setBottomMargin(listItem ? gap * 0.2 : gap);
            format.setLineHeight(typoraLineHeightPercent,
                                 QTextBlockFormat::ProportionalHeight);
            format.clearBackground();
        }

        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);

        if (code) {
            // Code wants a monospaced face whatever the prose is set in, and
            // the app carries one, so it is there on any machine.
            QTextCharFormat monospaced;
            monospaced.setFontFamilies({QStringLiteral("iA Writer Mono S")});
            cursor.setPosition(block.position());
            cursor.setPosition(block.position() + block.length() - 1,
                               QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(monospaced);

            // And the same highlighting the editor gives it. Reading the same
            // code in two palettes, or in one and not the other, reads as two
            // different documents.
            const QString language = MarkdownHighlighter::languageForFence(
                QStringLiteral("```")
                + block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage));
            for (const auto &token :
                     MarkdownHighlighter::codeTokens(block.text(), language)) {
                QTextCharFormat coloured;
                coloured.setForeground(MarkdownHighlighter::codeTokenColor(
                    token.token, darkPalette, accent, muted));
                if (token.token == MarkdownHighlighter::CodeToken::Comment)
                    coloured.setFontItalic(true);
                if (token.token == MarkdownHighlighter::CodeToken::Keyword)
                    coloured.setFontWeight(QFont::DemiBold);

                cursor.setPosition(block.position() + token.start);
                cursor.setPosition(block.position() + token.start + token.length,
                                   QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(coloured);
            }
        }
    }
    cursor.endEditBlock();
}

QString Backend::printJobName(const QString &documentFileName) {
    const QString trimmed = documentFileName.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("Untitled");

    const QFileInfo info(trimmed);
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown"))
        return info.completeBaseName();

    return trimmed;
}

void Backend::renderPreview(QObject *textDocument) {
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    if (!quickDocument || !quickDocument->textDocument())
        return;

    QTextDocument *preview = quickDocument->textDocument();
    preview->setDefaultFont(m_document ? m_document->defaultFont() : preview->defaultFont());
    preview->setMarkdown(renderableDocumentText());

    styleRenderedDocument(preview, false);
}

namespace {
// A paragraph is a run of non-blank lines. Returns the [start, end) offsets of
// the paragraph containing `cursor`, clamped to the text.
struct ParagraphRange { int start; int end; };

ParagraphRange paragraphAround(const QStringList &lines, int lineIndex) {
    int start = lineIndex;
    while (start > 0 && !lines.at(start - 1).trimmed().isEmpty())
        --start;
    int end = lineIndex;
    while (end + 1 < lines.size() && !lines.at(end + 1).trimmed().isEmpty())
        ++end;
    return {start, end};
}

int lineIndexForOffset(const QStringList &lines, int cursor) {
    int consumed = 0;
    for (int i = 0; i < lines.size(); ++i) {
        consumed += lines.at(i).size() + 1;  // the newline
        if (cursor < consumed)
            return i;
    }
    return qMax(0, lines.size() - 1);
}

int offsetForLineIndex(const QStringList &lines, int lineIndex) {
    int offset = 0;
    for (int i = 0; i < lineIndex && i < lines.size(); ++i)
        offset += lines.at(i).size() + 1;
    return offset;
}

QVariantMap result(const QString &text, int cursor) {
    return QVariantMap{{QStringLiteral("text"), text},
                       {QStringLiteral("cursor"), qBound(0, cursor, text.size())}};
}

// Abbreviations that end in a full stop without ending a sentence. Deliberately
// short: a longer list is a dictionary, and the cost of a wrong split here is
// one keystroke to undo, not lost work.
bool endsWithAbbreviation(const QString &sentence) {
    static const QStringList abbreviations{
        QStringLiteral("mr."),  QStringLiteral("mrs."), QStringLiteral("ms."),
        QStringLiteral("dr."),  QStringLiteral("prof."), QStringLiteral("st."),
        QStringLiteral("e.g."), QStringLiteral("i.e."), QStringLiteral("etc."),
        QStringLiteral("vs."),  QStringLiteral("cf."),  QStringLiteral("al.")};

    const QString tail = sentence.trimmed().toLower();
    for (const QString &abbreviation : abbreviations) {
        if (tail.endsWith(abbreviation))
            return true;
    }

    // A single initial, as in "J. B. Peterson".
    if (tail.size() >= 2 && tail.endsWith(QLatin1Char('.'))
            && tail.at(tail.size() - 2).isLetter()
            && (tail.size() == 2 || !tail.at(tail.size() - 3).isLetter())) {
        return true;
    }

    return false;
}
}

QStringList Backend::splitSentences(const QString &paragraph) {
    QStringList sentences;
    const QString text = paragraph.simplified();
    if (text.isEmpty())
        return sentences;

    int start = 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar character = text.at(i);
        if (character != QLatin1Char('.') && character != QLatin1Char('!')
                && character != QLatin1Char('?')) {
            continue;
        }

        // Run on through ?!. and any closing quote or bracket.
        int end = i;
        while (end + 1 < text.size()
               && QStringLiteral(".!?\"')]").contains(text.at(end + 1))) {
            ++end;
        }

        if (end + 1 >= text.size()) {
            sentences << text.mid(start).trimmed();
            start = text.size();
            break;
        }

        if (text.at(end + 1) != QLatin1Char(' '))
            continue;

        const QString candidate = text.mid(start, end - start + 1);
        if (endsWithAbbreviation(candidate))
            continue;

        // A decimal such as "3.5" is not a boundary.
        if (character == QLatin1Char('.') && i > 0 && text.at(i - 1).isDigit()
                && end + 2 < text.size() && text.at(end + 2).isDigit()) {
            continue;
        }

        sentences << candidate.trimmed();
        start = end + 2;
        i = end + 1;
    }

    if (start < text.size()) {
        const QString rest = text.mid(start).trimmed();
        if (!rest.isEmpty())
            sentences << rest;
    }

    return sentences;
}

QVariantMap Backend::moveParagraph(const QString &text, int cursor, int delta) {
    if (delta == 0)
        return result(text, cursor);

    QStringList lines = text.split(QLatin1Char('\n'));
    const int lineIndex = lineIndexForOffset(lines, cursor);
    const ParagraphRange here = paragraphAround(lines, lineIndex);
    if (lines.at(here.start).trimmed().isEmpty())
        return result(text, cursor);  // the cursor is between paragraphs

    // Find the neighbouring paragraph in the direction asked for.
    int probe = delta < 0 ? here.start - 1 : here.end + 1;
    while (probe >= 0 && probe < lines.size() && lines.at(probe).trimmed().isEmpty())
        probe += delta < 0 ? -1 : 1;
    if (probe < 0 || probe >= lines.size())
        return result(text, cursor);  // nothing to trade places with

    const ParagraphRange there = paragraphAround(lines, probe);
    const int cursorWithin = cursor - offsetForLineIndex(lines, here.start);

    const QStringList moving = lines.mid(here.start, here.end - here.start + 1);
    const QStringList other = lines.mid(there.start, there.end - there.start + 1);

    QStringList rebuilt;
    if (delta < 0) {
        rebuilt = lines.mid(0, there.start) + moving + QStringList{QString()} + other
                + lines.mid(here.end + 1);
    } else {
        rebuilt = lines.mid(0, here.start) + other + QStringList{QString()} + moving
                + lines.mid(there.end + 1);
    }

    const QString rebuiltText = rebuilt.join(QLatin1Char('\n'));
    const int newStartLine = delta < 0 ? there.start
                                       : there.start - (here.end - here.start + 1);
    const int newCursor = offsetForLineIndex(rebuilt, qMax(0, newStartLine)) + cursorWithin;
    return result(rebuiltText, newCursor);
}

QVariantMap Backend::explodeSentences(const QString &text, int cursor) {
    QStringList lines = text.split(QLatin1Char('\n'));
    const int lineIndex = lineIndexForOffset(lines, cursor);
    const ParagraphRange range = paragraphAround(lines, lineIndex);
    if (lines.at(range.start).trimmed().isEmpty())
        return result(text, cursor);

    const QString paragraph =
        lines.mid(range.start, range.end - range.start + 1).join(QLatin1Char(' '));
    const QStringList sentences = splitSentences(paragraph);
    if (sentences.size() < 2)
        return result(text, cursor);

    const QStringList rebuilt =
        lines.mid(0, range.start) + sentences + lines.mid(range.end + 1);
    const QString rebuiltText = rebuilt.join(QLatin1Char('\n'));
    return result(rebuiltText, offsetForLineIndex(rebuilt, range.start));
}

QVariantMap Backend::collapseSentences(const QString &text, int cursor) {
    QStringList lines = text.split(QLatin1Char('\n'));
    const int lineIndex = lineIndexForOffset(lines, cursor);
    const ParagraphRange range = paragraphAround(lines, lineIndex);
    if (lines.at(range.start).trimmed().isEmpty())
        return result(text, cursor);

    QStringList parts;
    for (int i = range.start; i <= range.end; ++i) {
        const QString trimmed = lines.at(i).trimmed();
        if (!trimmed.isEmpty())
            parts << trimmed;
    }

    const QStringList rebuilt = lines.mid(0, range.start)
        + QStringList{parts.join(QLatin1Char(' '))} + lines.mid(range.end + 1);
    const QString rebuiltText = rebuilt.join(QLatin1Char('\n'));
    return result(rebuiltText, offsetForLineIndex(rebuilt, range.start));
}

QVariantList Backend::outlineFor(const QString &text) {
    QVariantList outline;
    static const QRegularExpression headingRe(
        QStringLiteral("^(#{1,6})\\s+(.*\\S)\\s*$"));

    const QStringList lines = text.split(QLatin1Char('\n'));
    int offset = 0;
    bool inFence = false;
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("```"))
                || trimmed.startsWith(QStringLiteral("~~~"))) {
            inFence = !inFence;
        } else if (!inFence) {
            const QRegularExpressionMatch match = headingRe.match(line);
            if (match.hasMatch()) {
                outline.append(QVariantMap{
                    {QStringLiteral("level"), match.captured(1).size()},
                    {QStringLiteral("title"), match.captured(2)},
                    {QStringLiteral("position"), offset}});
            }
        }
        offset += line.size() + 1;
    }

    return outline;
}

void Backend::printDocument() {
    if (!m_document) {
        setStatus(QStringLiteral("There is no document to print."));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    // Drives the default filename macOS puts in Save as PDF.
    printer.setDocName(printJobName(fileName()));
    QPrintDialog dialog(&printer);
    dialog.setWindowTitle(QStringLiteral("Print %1").arg(fileName()));
    dialog.winId();
    if (dialog.windowHandle() && m_parentWindow)
        dialog.windowHandle()->setTransientParent(m_parentWindow);

    if (dialog.exec() == QDialog::Accepted)
        printRenderedMarkdown(&printer, renderableDocumentText());
}

QString Backend::enclosingBundlePath(const QString &executableDirPath) {
    // Walked as text rather than with QDir::cdUp(), which needs the directory
    // to exist and so could not be tested without building a bundle.
    const QStringList parts = QDir::cleanPath(executableDirPath).split(QLatin1Char('/'));
    if (parts.size() < 3)
        return {};

    if (parts.at(parts.size() - 1) != QStringLiteral("MacOS")
        || parts.at(parts.size() - 2) != QStringLiteral("Contents")
        || !parts.at(parts.size() - 3).endsWith(QStringLiteral(".app"))) {
        return {};
    }

    return QStringList(parts.mid(0, parts.size() - 2)).join(QLatin1Char('/'));
}

bool Backend::launchNewInstance(const QString &filePath) {
    QStringList arguments;

#ifdef Q_OS_MACOS
    // Running the executable inside the bundle directly gives the new instance
    // its own Dock tile and leaves it unknown to Launch Services. `open -n`
    // asks macOS for a second instance of the application itself.
    const QString bundle = enclosingBundlePath(QCoreApplication::applicationDirPath());
    if (!bundle.isEmpty()) {
        arguments << QStringLiteral("-n") << QStringLiteral("-a") << bundle;
        if (!filePath.isEmpty())
            arguments << QStringLiteral("--args") << filePath;

        return QProcess::startDetached(QStringLiteral("open"), arguments);
    }
#endif

    if (!filePath.isEmpty())
        arguments << filePath;

    return QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments);
}

void Backend::newWindow() {
    emit newWindowRequested();
}

void Backend::bringAllWindowsToFront() {
    for (Backend *window : g_liveWindows) {
        if (window->m_parentWindow)
            window->m_parentWindow->raise();
    }
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

    if (m_document) {
        const int blockCount = m_document->blockCount();
        if (blockCount > m_formattedBlockCount)
            reapplyTypographyToChange();
        m_formattedBlockCount = blockCount;
    }

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
            setStatus(QStringLiteral("Saved %1").arg(fileName()));
        else
            setStatus(QString());
    } else {
        setModified(true);
        setStatus(QStringLiteral("Unsaved"));
        scheduleRecovery();
    }

    return true;
}

QVariantList Backend::hiddenRangesAt(int position) const {
    QVariantList ranges;
    if (!m_document)
        return ranges;

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid())
        return ranges;

    // The highlighter leaves fenced code alone, so it hides nothing there.
    if (MarkdownHighlighter::isInsideFence(block.userState()))
        return ranges;

    const int lineStart = block.position();
    QList<QPair<int, int>> spans;
    const QList<MarkdownHighlighter::InlineMarkup> markup =
        MarkdownHighlighter::inlineMarkup(block.text());
    for (const MarkdownHighlighter::InlineMarkup &item : markup) {
        for (const MarkdownHighlighter::Span &marker : item.markers) {
            spans.append({lineStart + marker.start,
                          lineStart + marker.start + marker.length});
        }
    }
    std::sort(spans.begin(), spans.end());

    for (const auto &span : spans) {
        ranges.append(QVariantMap{{QStringLiteral("start"), span.first},
                                  {QStringLiteral("end"), span.second}});
    }
    return ranges;
}

void Backend::setSearchHighlight(const QString &query, int currentMatchStart) {
    if (m_highlighter)
        m_highlighter->setSearch(query, currentMatchStart);
}

void Backend::openExternalUrl(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https")
            || scheme == QStringLiteral("mailto"))
        QDesktopServices::openUrl(url);
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

void Backend::loadDocumentText(const QString &text) {
    if (!m_document) {
        setStatus(QStringLiteral("Could not attach the Markdown renderer."));
        return;
    }

    m_loading = true;
    m_document->setPlainText(text);
    m_lastDocumentText = text;
    m_loading = false;

    applyDocumentTypography();
    m_wordCountTimer.stop();
    setWordCount(countWords(text));
}

void Backend::setFileUrl(const QUrl &url) {
    if (m_fileUrl != url) {
        m_fileUrl = url;
        emit fileUrlChanged();
    }
    if (m_activeTab >= 0 && m_activeTab < m_tabs.size()) {
        m_tabs[m_activeTab].fileUrl = m_fileUrl;
        if (m_fileUrl.isLocalFile() && !m_fileUrl.toLocalFile().isEmpty())
            m_tabs[m_activeTab].untitledNumber = 0;
    }
    syncRepresentedFile();
    watchCurrentFile();
    emit tabsChanged();
}

void Backend::setModified(bool modified) {
    if (m_modified != modified) {
        m_modified = modified;
        emit modifiedChanged();
    }
    if (m_activeTab >= 0 && m_activeTab < m_tabs.size())
        m_tabs[m_activeTab].modified = m_modified;
    emit tabsChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;

    m_status = status;
    emit statusChanged();
}

void Backend::saveTo(const QUrl &url) {
    if (!url.isLocalFile()) {
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Only local files can be saved."));
        emit saveFailed();
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QSaveFile file(url.toLocalFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Could not save %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    const QByteArray contents = currentDocumentText().toUtf8();
    file.write(contents);

    // QSaveFile commits by replacing the target. Stop watching the old inode
    // before that replacement so our own write is not classified as external.
    const QStringList watched = m_fileWatcher.files();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);

    // commit() flushes, fsyncs, and atomically renames the temp file into place,
    // returning false (and leaving the original untouched) on any write error.
    if (!file.commit()) {
        watchCurrentFile();
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Could not write %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    const bool shouldClose = m_closeAfterSave;
    m_closeAfterSave = false;
    setKnownFileContents(contents, true);
    m_pathNeverRead = false;
    setFileUrl(url);
    watchCurrentFile();
    QSettings().setValue(lastSaveDirectorySetting,
                         QFileInfo(url.toLocalFile()).absolutePath());
    setModified(false);
    setStatus(QStringLiteral("Saved %1").arg(fileName()));
    clearRecovery();
    emit saveSucceeded();

    if (shouldClose)
        emit closeAfterSave();
}

void Backend::scheduleRecovery() {
    m_recoveryTimer.start();
}

bool Backend::canAutosaveToFile() const {
    if (!m_autosave || !m_fileUrl.isLocalFile())
        return false;

    // An outside edit nobody has answered yet. The writer picks the version
    // that survives; autosave does not get to pick it for them.
    if (m_externalChangeUnanswered)
        return false;

    // A name taken for a file that was never read is unguarded: nothing can
    // watch a path with no file, so something can arrive on it between the
    // naming and the write. The first save is the one that asks about that,
    // so leave it to the writer -- until they have saved once, this document
    // keeps getting the snapshot instead.
    if (m_pathNeverRead)
        return false;

    return true;
}

// The debounce that used to only ever write the crash snapshot. A document
// with a name it is safe to write goes to its own file; everything else --
// untitled drafts, and anything the guardrails hold back -- keeps getting the
// snapshot, so no edit is ever only in memory.
void Backend::persistDocument() {
    if (m_modified && canAutosaveToFile()) {
        saveTo(m_fileUrl);
        persistSession();
        return;
    }
    if (m_modified)
        persistSession();
}

QString Backend::recoveryPath() const {
    return sessionPath();
}

void Backend::setKnownFileContents(const QByteArray &contents, bool known) {
    m_lastKnownFileContents = contents;
    m_hasKnownFileContents = known;
    m_lastKnownFileText = known
        ? QString::fromUtf8(contents).replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
        : QString();
}

void Backend::persistSession() {
    if (g_sessionWritesFrozen)
        return;
    storeTabFields(m_activeTab);
    QJsonArray windows;
    for (Backend *window : g_liveWindows) {
        if (window->m_tabs.isEmpty())
            continue;
        windows.append(window->sessionObject());
    }
    writeSessionFile(windows);
}

QJsonObject Backend::sessionObject() const {
    QJsonArray tabs;
    for (int i = 0; i < m_tabs.size(); ++i) {
        const DocumentTab &tab = m_tabs.at(i);
        QJsonObject item{{QStringLiteral("fileUrl"), tab.fileUrl.toString()},
                         {QStringLiteral("pathNeverRead"), tab.pathNeverRead},
                         {QStringLiteral("untitledNumber"), tab.untitledNumber},
                         {QStringLiteral("modified"),
                          tab.modified || (i == m_activeTab && m_modified)}};
        const bool unsaved = tab.modified || (i == m_activeTab && m_modified)
            || tab.untitledNumber > 0;
        if (unsaved)
            item.insert(QStringLiteral("text"),
                        i == m_activeTab ? currentDocumentText() : tab.cachedText);
        tabs.append(item);
    }
    const QVariantMap geometry = windowGeometry();
    return QJsonObject{
        {QStringLiteral("activeTab"), m_activeTab},
        {QStringLiteral("tabs"), tabs},
        {QStringLiteral("x"), geometry.value(QStringLiteral("x")).toInt()},
        {QStringLiteral("y"), geometry.value(QStringLiteral("y")).toInt()},
        {QStringLiteral("width"), geometry.value(QStringLiteral("width")).toInt()},
        {QStringLiteral("height"), geometry.value(QStringLiteral("height")).toInt()},
        {QStringLiteral("maximized"), geometry.value(QStringLiteral("maximized")).toBool()},
    };
}

void Backend::writeSessionFile(const QJsonArray &windows) {
    const QString path = sessionPath();
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("windows"), windows}})
                   .toJson(QJsonDocument::Compact));
    file.commit();
}

void Backend::writeRecovery() {
    persistSession();
}

void Backend::restoreRecovery() {
    const QString stateDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QFileInfo::exists(sessionPath())) {
        QDir dir(stateDirectory);
        const QStringList legacy = dir.entryList({QStringLiteral("recovery-*.json")},
                                                  QDir::Files);
        QJsonArray importedTabs;
        for (const QString &name : legacy) {
            QFile old(dir.filePath(name));
            if (!old.open(QIODevice::ReadOnly))
                continue;
            const QJsonObject recovery = QJsonDocument::fromJson(old.readAll()).object();
            old.close();
            if (!recovery.contains(QStringLiteral("text")))
                continue;
            QJsonObject tab{{QStringLiteral("fileUrl"), recovery.value(QStringLiteral("fileUrl"))},
                            {QStringLiteral("pathNeverRead"),
                             recovery.value(QStringLiteral("pathNeverRead"))},
                            {QStringLiteral("text"), recovery.value(QStringLiteral("text"))},
                            {QStringLiteral("modified"), true},
                            {QStringLiteral("untitledNumber"),
                             recovery.value(QStringLiteral("fileUrl")).toString().isEmpty()
                                 ? 1
                                 : 0}};
            importedTabs.append(tab);
            QFile::remove(dir.filePath(name));
        }
        if (!importedTabs.isEmpty()) {
            writeSessionFile(QJsonArray{QJsonObject{
                {QStringLiteral("activeTab"), 0},
                {QStringLiteral("tabs"), importedTabs},
            }});
        }
    }

    QFile sessionFile(sessionPath());
    if (!sessionFile.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(sessionFile.readAll()).object();
    QJsonArray windows = root.value(QStringLiteral("windows")).toArray();
    if (windows.isEmpty() && root.contains(QStringLiteral("text"))) {
        windows.append(QJsonObject{
            {QStringLiteral("activeTab"), 0},
            {QStringLiteral("tabs"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("fileUrl"), root.value(QStringLiteral("fileUrl"))},
                 {QStringLiteral("pathNeverRead"), root.value(QStringLiteral("pathNeverRead"))},
                 {QStringLiteral("text"), root.value(QStringLiteral("text"))},
                 {QStringLiteral("modified"), true},
             }}},
        });
    }
    const int windowIndex = qMax(0, g_liveWindows.indexOf(this));
    if (windowIndex >= windows.size())
        return;
    const QJsonObject window = windows.at(windowIndex).toObject();
    const QJsonArray tabs = window.value(QStringLiteral("tabs")).toArray();
    if (tabs.isEmpty())
        return;

    m_tabs.clear();
    for (const QJsonValue &value : tabs) {
        const QJsonObject item = value.toObject();
        DocumentTab tab;
        tab.fileUrl = QUrl(item.value(QStringLiteral("fileUrl")).toString());
        tab.cachedText = item.value(QStringLiteral("text")).toString();
        tab.pathNeverRead = item.value(QStringLiteral("pathNeverRead")).toBool();
        tab.untitledNumber = item.value(QStringLiteral("untitledNumber")).toInt();
        tab.modified = item.value(QStringLiteral("modified")).toBool(true);
        if (tab.fileUrl.isEmpty() && tab.untitledNumber == 0)
            tab.untitledNumber = 1;
        if (tab.fileUrl.isLocalFile()) {
            QFile disk(tab.fileUrl.toLocalFile());
            if (disk.open(QIODevice::ReadOnly)) {
                tab.lastKnownFileContents = disk.readAll();
                tab.hasKnownFileContents = true;
                tab.lastKnownFileText = QString::fromUtf8(tab.lastKnownFileContents)
                                           .replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
                if (!item.contains(QStringLiteral("text"))) {
                    tab.cachedText = tab.lastKnownFileText;
                    tab.modified = false;
                }
            }
        }
        m_tabs.append(tab);
    }
    m_activeTab = qBound(0, window.value(QStringLiteral("activeTab")).toInt(),
                         m_tabs.size() - 1);
    loadTabFields(m_activeTab);
    m_pathNeverRead = m_tabs[m_activeTab].pathNeverRead;
    if (m_modified)
        setStatus(QStringLiteral("Recovered unsaved changes"));
    watchCurrentFile();
    emit tabsChanged();
}

void Backend::clearRecovery() {
    m_recoveryTimer.stop();
    persistSession();
}

void Backend::watchCurrentFile() {
    const QStringList watched = m_fileWatcher.files();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);
    QSet<QString> paths;
    for (const DocumentTab &tab : m_tabs) {
        if (tab.fileUrl.isLocalFile() && QFileInfo::exists(tab.fileUrl.toLocalFile()))
            paths.insert(tab.fileUrl.toLocalFile());
    }
    if (m_fileUrl.isLocalFile() && QFileInfo::exists(m_fileUrl.toLocalFile()))
        paths.insert(m_fileUrl.toLocalFile());
    if (!paths.isEmpty())
        m_fileWatcher.addPaths(QStringList(paths.begin(), paths.end()));
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
    const QString colorsPath = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");
    QString themeMode;
    QString background;
    QString foreground;
    QString accent;
    QString selection;
    // Left empty when the theme sets no lighter background; the highlighter
    // mixes its own shade of the page in that case.
    m_themeLighterBackground.clear();
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
            else if (key == QStringLiteral("lighter_background"))
                m_themeLighterBackground = value;
        }
    }

    bool themeIsDark = m_darkMode;
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
    const bool darkModeFlipped = themeIsDark != m_darkMode;
    m_darkMode = themeIsDark;

    // The defaults follow the mode the theme resolved to, so a colour we cannot
    // read costs that one colour rather than the contrast of the whole editor.
    m_themeBackground = m_darkMode ? QStringLiteral("#101010") : QStringLiteral("#ffffff");
    m_themeForeground = m_darkMode ? QStringLiteral("#eeeeee") : QStringLiteral("#222324");
    m_themeAccent = m_darkMode ? QStringLiteral("#5584aa") : QStringLiteral("#2077b2");
    m_themeSelection = m_darkMode ? QStringLiteral("#186a9a") : QStringLiteral("#2077b2");

    // An unparseable colour reaches QML as black, so keep the default instead.
    const auto applyColor = [](QString &target, const QString &value) {
        if (QColor(value).isValid())
            target = value;
    };
    applyColor(m_themeBackground, background);
    applyColor(m_themeForeground, foreground);
    applyColor(m_themeAccent, accent);
    applyColor(m_themeSelection, selection);

    // Omarchy themes name a lighter background for panels like the one code
    // sits on, so use the shade the theme chose. Not every theme sets the key,
    // and a few set it to the page background, which would leave code with no
    // panel at all: mix a shade of the page towards the text for those. Mixing,
    // unlike lightening, still moves on a pure black background.
    const QColor page(m_themeBackground);
    const QColor lighter(m_themeLighterBackground);
    m_themeCodeBackground = (lighter.isValid() && lighter != page
                                 ? lighter
                                 : blend(page, QColor(m_themeForeground), 0.10)).name();

    if (m_highlighter) {
        m_highlighter->setDarkMode(m_darkMode);
        m_highlighter->setColors(m_themeBackground, m_themeForeground, m_themeAccent,
                                 m_themeCodeBackground);
    }

    if (darkModeFlipped)
        emit darkModeChanged();
    emit themeColorsChanged();
}

void Backend::watchOmarchyTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const QString currentDir = QDir::homePath()
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

QString Backend::renderableDocumentText() const {
    const QString text = currentDocumentText();
    if (!m_fileUrl.isLocalFile())
        return text;  // an untitled draft has no folder to resolve against

    return expandContentBlocks(
        text, QFileInfo(m_fileUrl.toLocalFile()).absolutePath());
}

QString Backend::currentDocumentText() const {
    return m_document ? m_document->toPlainText() : QString();
}

int Backend::countWords(const QString &text) {
    static const QRegularExpression wordRe(
        QStringLiteral("[\\p{L}\\p{N}]+(?:['-][\\p{L}\\p{N}]+)*"));
    int count = 0;
    QRegularExpressionMatchIterator it = wordRe.globalMatch(text);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

QString Backend::suggestedFileName(const QString &text) {
    QString name = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\x00-\\x1f\\x7f]")),
                 QStringLiteral("-"));
    name = name.left(120).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("Untitled");
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
    setWordCount(countWords(currentDocumentText()));
}

void Backend::scheduleWordCount() {
    m_wordCountTimer.start();
}

void Backend::applyDocumentTypography() {
    if (!m_document)
        return;

    QTextBlockFormat blockFormat;
    blockFormat.setLineHeight(typoraLineHeightPercent, QTextBlockFormat::ProportionalHeight);

    // A full pass is only used for freshly loaded/attached documents, so it is
    // safe to drop undo history here (re-enabling clears the stack anyway).
    const bool undoEnabled = m_document->isUndoRedoEnabled();
    m_document->setUndoRedoEnabled(false);

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(blockFormat);
    m_formattingTypography = false;

    m_document->setUndoRedoEnabled(undoEnabled);

    m_formattedBlockCount = m_document->blockCount();
}

QVariantList Backend::fencedRanges() const {
    QVariantList ranges;
    if (!m_document)
        return ranges;

    int openedAt = -1;
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        const QString trimmed = block.text().trimmed();
        if (!trimmed.startsWith(QStringLiteral("```"))
                && !trimmed.startsWith(QStringLiteral("~~~"))) {
            continue;
        }

        if (openedAt < 0) {
            openedAt = block.position();
        } else {
            ranges.append(QVariantMap{
                {QStringLiteral("start"), openedAt},
                {QStringLiteral("end"), block.position() + block.length() - 1}});
            openedAt = -1;
        }
    }

    // A fence left open still wants its panel, to the end of the document.
    if (openedAt >= 0) {
        ranges.append(QVariantMap{
            {QStringLiteral("start"), openedAt},
            {QStringLiteral("end"), m_document->characterCount() - 1}});
    }

    return ranges;
}

void Backend::reapplyTypographyToChange() {
    if (!m_document)
        return;

    QTextBlockFormat blockFormat;
    blockFormat.setLineHeight(typoraLineHeightPercent, QTextBlockFormat::ProportionalHeight);

    // Format only the block(s) touched by the last edit instead of the whole
    // document, and fold the change into the preceding edit command so a single
    // undo reverts both the text and its formatting.
    const int maxPos = m_document->characterCount() - 1;
    const int start = qBound(0, m_lastChangePos, maxPos);
    const int end = qBound(start, m_lastChangePos + m_lastChangeAdded, maxPos);

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    cursor.joinPreviousEditBlock();
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.mergeBlockFormat(blockFormat);
    cursor.endEditBlock();
    m_formattingTypography = false;
}

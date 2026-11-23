#pragma once

#include <QObject>
#include <QPointer>
#include <QTextDocument>
#include <QTextLayout>
#include <QVariant>
#include "previewblocks.h"
#include "markdownreferences.h"
#include "listpreview.h"

class MarkdownHighlighter;
class MermaidRenderer;
class SpellingController;
class QKeyEvent;
class MarkdownTableModel;
class QTextTable;

// The backend owns source and its undo stack. This document is disposable UI.
class LivePreview : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool readMode READ readMode WRITE setReadMode NOTIFY readModeChanged)
    Q_PROPERTY(bool imeCursorVisible READ imeCursorVisible NOTIFY imeCursorVisibleChanged)
    Q_PROPERTY(QObject *resolver READ resolver WRITE setResolver)
    Q_PROPERTY(QObject *spelling READ spelling WRITE setSpelling)
    Q_PROPERTY(QVariantList codeBlockPanels READ codeBlockPanels NOTIFY codeBlockPanelsChanged)
    Q_PROPERTY(QVariantList listMarkers READ listMarkers NOTIFY listMarkersChanged)
    Q_PROPERTY(QVariantList tablePanels READ tablePanels NOTIFY tablePanelsChanged)
    Q_PROPERTY(QVariantList tableScrollPanels READ tableScrollPanels NOTIFY tablePanelsChanged)
    Q_PROPERTY(QVariantMap blockOffsets READ blockOffsets NOTIFY blockOffsetsChanged)
public:
    struct Segment {
        int sourceStart, sourceEnd, viewStart, viewEnd;
        bool object;
    };
    explicit LivePreview(QObject *parent = nullptr);
    bool readMode() const { return m_readMode; }
    bool imeCursorVisible() const { return m_imeCursorVisible; }
    void setReadMode(bool read);
    QObject *resolver() const { return m_resolver; }
    void setResolver(QObject *resolver) { m_resolver = resolver; }
    QObject *spelling() const;
    void setSpelling(QObject *spelling);
    Q_INVOKABLE void attach(QObject *sourceItem, QObject *viewItem);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setViewport(qreal top, qreal height);
    Q_PROPERTY(qreal viewportBottom READ viewportBottom NOTIFY viewportChanged)
    qreal viewportBottom() const { return m_viewportHeight < 0 ? -1 : m_viewportTop + m_viewportHeight; }
    Q_INVOKABLE QString linkAtPoint(qreal x, qreal y) const;
    Q_INVOKABLE int footnoteViewPosition(const QString &url) const;
    Q_INVOKABLE void copySource();
    Q_INVOKABLE bool copyReadSelection(bool asHtml = false);
    Q_INVOKABLE bool copyCodeBlock(int panelIndex);
    Q_INVOKABLE void prepareEdit(bool revealBoundary = true);
    Q_INVOKABLE void watchTableInput(QObject *table, bool enabled);
    Q_INVOKABLE void setTableSource(int id, bool source);
    Q_INVOKABLE void resetTableSessions();
    Q_INVOKABLE bool copyTableDrafts(int id = 0);
    Q_INVOKABLE void discardTableDrafts();
    Q_PROPERTY(bool tableDraftsPending READ tableDraftsPending NOTIFY tableDraftsChanged)
    bool tableDraftsPending() const;
    Q_INVOKABLE void scrollBlock(int id, qreal offset);
    Q_INVOKABLE QRectF visualCursorRectangle(const QRectF &native) const;
    Q_INVOKABLE void prepareFormatting();
    Q_INVOKABLE void setSearchHighlight(const QString &query, int currentMatchStart);
    Q_INVOKABLE QString imagePathAt(int viewPosition) const;
    Q_INVOKABLE QString imagePathAtPoint(qreal x, qreal y) const;
    Q_INVOKABLE QVariantMap spellingWordAt(int viewPosition);
    Q_INVOKABLE QVariantMap spellingWordAtPoint(qreal x, qreal y);
    Q_INVOKABLE int sourcePosition(int viewPosition) const;
    Q_INVOKABLE int viewPosition(int sourcePosition) const;
    // Public pure mapping helpers are also used by the isolated tests.
    static int toSource(const QList<Segment> &segments, int position);
    static int toView(const QList<Segment> &segments, int position);
    const QList<Segment> &segments() const { return m_segments; }
    QVariantList codeBlockPanels() const { return m_codeBlockPanels; }
    QVariantList listMarkers() const { return m_listMarkers; }
    QVariantList tablePanels() const { return m_tablePanels; }
    QVariantList tableScrollPanels() const { return m_tableScrollPanels; }
    QVariantMap blockOffsets() const { return m_blockOffsets; }
signals:
    void readModeChanged();
    void imeCursorVisibleChanged();
    void copyHtmlRequested();
    void toggleReadModeRequested();
    void formatCodeRequested(bool fenced);
    void codeBlockPanelsChanged();
    void listMarkersChanged();
    void tablePanelsChanged();
    void tableDraftsChanged();
    void viewportChanged();
    void blockOffsetsChanged();
    void projectionAboutToChange();
    void projectionChanged();
    void projectionReady();
private slots:
    void sourceSelectionChanged();
    void viewSelectionChanged();
    void viewContentsChanged(int position, int removed, int added);
    void sourceTextChanged();
    void sourceGeometryChanged();
private:
    bool eventFilter(QObject *object, QEvent *event) override;
    void schedule();
    void rebuild();
    void refreshSpelling();
    void select(QObject *item, int anchor, int caret);
    QUrl resolve(const QString &path) const;
    QImage imageFor(const PreviewBlocks::Block &block);
    void buildReadDocument(const QString &source);
    void switchModeDocument(bool read);
    void clearViewResources();
    void completeProjectionLayout();
    void recordViewChange(int position, int removed, const QString &added);
    void updateSourceSnapshot();
    void syncEditProjection(bool revealBoundary = false, bool preparingEdit = false);
    void styleProjection(bool fontChanged);
    PreviewBlocks::Parser m_blockParser;
    MarkdownReferences::Index m_noteReferences;
    quint64 m_noteGeneration = quint64(-1);
    ListPreview::Parser m_listParser;
    int m_listDirtyFirst = 0;
    bool m_sourceDirty = true, m_viewSyncedSource = false, m_forceReset = true;
    bool m_styleDirty = true, m_renderDirty = true, m_renderedRead = false;
    qreal m_viewportTop = 0, m_viewportHeight = -1;
    QList<QPair<QTextBlock, QPair<qreal, qreal>>> m_panelMargins;
    quint64 m_projectionPatches = 0, m_fullResets = 0; // Deterministic regression counters.
    friend class LivePreviewTest;
    void updateCodeBlockPanels();
    void styleCodeBlocks();
    void syncTables();
    void shiftTables(int position, int removed, int added);
    void updateTablePanels();
    void styleOverflow();
    struct TableSession {
        int id, start, end;
        QString markdown;
        bool source = false;
        QPointer<MarkdownTableModel> model;
    };
    struct RowWidth {
        QTextBlock block;
        int revision = -1;
        QTextBlockFormat blockFormat;
        QTextCharFormat charFormat;
        QTextOption option;
        QList<QTextLayout::FormatRange> formats;
        qreal width = 0;
    };
    struct Overflow {
        int id;
        QTextBlock first, last;
        QPointer<QTextTable> table;
        qreal offset = 0, width = 0;
        QFont widthFont;
        QList<RowWidth> rows;
    };
    QList<TableSession> m_tables;
    QVariantMap m_recoveredTableDrafts;
    QList<Overflow> m_overflows;
    QList<QTextBlock> m_unwrapped;
    QList<QTextBlock> m_overflowEndpoints;
    bool m_overflowStyleDirty = true;
    quint64 m_overflowStyleVisits = 0, m_overflowMeasurements = 0; // Regression counters, not telemetry.
    QVariantList m_tablePanels, m_tableScrollPanels;
    QVariantMap m_blockOffsets;
    int m_nextPanelId = 1;
    bool m_overflowScrolling = false, m_revealOverflowCaret = true;
    Overflow &overflowFor(const QTextBlock &first, const QTextBlock &last, QTextTable *table = nullptr);
    void finishProjectionChange();
    bool m_projectionChanging = false;
    QPair<int, int> visibleBlocks() const;
    QList<QPair<int, int>> m_editCodeGroups;
    QList<QTextBlock> m_codeEndpoints;
    qreal m_codeLineHeight = -1;
    bool m_codeSpacingDirty = true;
    QList<int> m_invalidImageSlots;
    void styleEditLists();
    void updateListMarkers();
    int characterAtPoint(qreal x, qreal y) const;
    bool autoPair(QKeyEvent *event);
    QVariantList m_listMarkers;
    quint64 m_sourceGeneration = 0;
    QFont m_listFont;
    qreal m_listIndentWidth = 0;
    bool m_listStyleValid = false;
    void scheduleCodeBlockPanels();
    QVariantList m_codeBlockPanels;
    QList<QPair<int, int>> m_readCodeBlocks;
    QHash<QString, QString> m_readImagePaths; // Sanitized native resource -> original local destination.
    QHash<int, int> m_readTablePositions; // Exact generated resource -> final Read slot position, keyed by session.
    QStringList m_readCodeLanguages;
    bool m_panelsPending = false;
    QPointer<QObject> m_sourceItem, m_viewItem, m_resolver;
    QPointer<QTextDocument> m_source, m_view, m_editDocument, m_readDocument;
    struct EditPresentation {
        QString text, source;
        QList<Segment> segments;
        QList<QUrl> resources;
        QList<QPair<QTextBlock, QPair<qreal, qreal>>> margins;
        QList<QTextBlock> endpoints;
        QList<QPair<int, int>> groups;
        QList<int> invalidImages;
        qreal lineHeight = -1;
        bool spacingDirty = true;
    } m_savedEdit, m_savedRead;
    QString m_readStyleKey;
    quint64 m_resourceGeneration = 0, m_readGeneration = 0;
    MarkdownHighlighter *m_highlighter = nullptr;
    QPointer<SpellingController> m_spelling;
    MermaidRenderer *m_mermaid = nullptr;
    QList<Segment> m_segments;
    QList<QUrl> m_resourceNames;
    QString m_viewText;
    QString m_projectionSource, m_panelsSource, m_panelsView;
    QString m_projectionKey;
    QString m_searchQuery;
    int m_currentMatch = -1;
    int m_anchor = 0, m_caret = 0;
    bool m_guard = false, m_pending = false, m_readMode = false;
    bool m_composing = false, m_imeCursorVisible = true;
    // Disposable, byte- and count-bounded raster cache.
    QHash<QString, QImage> m_images;
};

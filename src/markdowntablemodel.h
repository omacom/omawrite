#pragma once

#include <QAbstractTableModel>
#include <QFont>
#include <QString>
#include <QVector>

// A projection of one complete table, not an owner of its source or undo stack.
// All offsets (including replaceRequested) are QString/UTF-16 offsets.
class MarkdownTableModel : public QAbstractTableModel
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap drafts MEMBER m_drafts NOTIFY draftsChanged)
    Q_PROPERTY(quint64 draftRevision READ draftRevision NOTIFY draftsChanged)
    Q_PROPERTY(quint64 sourceRevision READ sourceRevision NOTIFY sourceRevisionChanged)
    Q_PROPERTY(qreal scrollX MEMBER m_scrollX NOTIFY scrollPositionChanged)
    Q_PROPERTY(qreal scrollY MEMBER m_scrollY NOTIFY scrollPositionChanged)

public:
    enum Role { MarkdownRole = Qt::UserRole + 1, HtmlRole, HeaderRole, AlignmentRole };
    Q_ENUM(Role)

    explicit MarkdownTableModel(QObject *parent = nullptr);
    void setSource(const QString &markdown, const QFont &font);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override {
        return index.isValid() ? QAbstractTableModel::flags(index) | Qt::ItemIsEditable : Qt::NoItemFlags;
    }

    Q_INVOKABLE qreal columnWidth(int column) const;
    Q_INVOKABLE int columnAlignment(int column) const;
    Q_INVOKABLE bool setColumnAlignment(int column, const QString &alignment);
    void acknowledgeReplacement() { m_acknowledged = true; }
    quint64 draftRevision() const { return m_draftRevision; }
    quint64 sourceRevision() const { return m_sourceRevision; }
    // value is raw inline Markdown. Existing escapes are retained; unescaped
    // pipes and an odd trailing backslash are protected against row syntax.
    Q_INVOKABLE bool setCell(int row, int column, const QString &value);
    Q_INVOKABLE bool fillCells(int top, int left, int bottom, int right,
                               const QString &value, quint64 sourceRevision);
    // Rows can only be inserted after the header or an existing body row.
    Q_INVOKABLE bool insertRow(int afterRow);
    Q_INVOKABLE bool removeRow(int row);
    // -1 inserts before the first column.
    Q_INVOKABLE bool insertColumn(int afterColumn);
    Q_INVOKABLE bool removeColumn(int column);

signals:
    void replaceRequested(int offset, int length, const QString &replacement);
    void columnWidthsChanged();
    void sourceRevisionChanged();
    void scrollPositionChanged();
    void draftsChanged();

private:
    friend class LivePreviewTest;
    struct Cell {
        int offset = 0;
        int length = 0;
        QString markdown;
        mutable QString html;
        mutable qreal width = -1;
        mutable bool rendered = false;
    };
    struct Range { int offset = 0; int length = 0; };
    struct Line {
        int offset = 0;
        int length = 0;
        QString ending;
        QVector<Range> fields;
    };

    static bool parse(const QString &source, QVector<Line> &lines,
                      QVector<QVector<Cell>> &rows);
    static bool splitLine(const QString &source, Line &line);
    static bool protectCell(const QString &value, QString &result);
    void render(const Cell &cell) const;
    bool requestTable(const QString &replacement);
    bool requestReplacement(int offset, int length, const QString &replacement,
                            QVector<Line> lines = {}, QVector<QVector<Cell>> rows = {});

    QString m_source;
    QFont m_font;
    QVector<Line> m_lines; // Header, alignment separator, then body.
    QVector<QVector<Cell>> m_rows; // Header and body only.
    mutable QVector<qreal> m_widths;
    bool m_initialized = false, m_acknowledged = false;
    QVariantMap m_drafts;
    QString m_expectedSource;
    QVector<Line> m_validatedLines;
    QVector<QVector<Cell>> m_validatedRows;
    quint64 m_parseCount = 0;
    mutable quint64 m_widthMeasurements = 0, m_richRenders = 0;
    quint64 m_draftRevision = 0, m_sourceRevision = 0;
    qreal m_scrollX = 0, m_scrollY = 0;
};

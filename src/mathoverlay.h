#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>

#include "markdownhighlighter.h"

class Backend;

// Draws the typeset formulas over the editor text. The highlighter hides each
// formula's source and holds its space; this item paints the formula there.
// It sits in the editor's coordinates, so it scrolls along with the text.
class MathOverlay : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(QColor selectionColor READ selectionColor WRITE setSelectionColor NOTIFY selectionColorChanged)
    Q_PROPERTY(int selectionStart READ selectionStart WRITE setSelectionStart NOTIFY selectionStartChanged)
    Q_PROPERTY(int selectionEnd READ selectionEnd WRITE setSelectionEnd NOTIFY selectionEndChanged)

public:
    explicit MathOverlay(QQuickItem *parent = nullptr);

    QObject *source() const;
    void setSource(QObject *source);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    QColor selectionColor() const { return m_selectionColor; }
    void setSelectionColor(const QColor &color);
    int selectionStart() const { return m_selectionStart; }
    void setSelectionStart(int position);
    int selectionEnd() const { return m_selectionEnd; }
    void setSelectionEnd(int position);

signals:
    void sourceChanged();
    void colorChanged();
    void selectionColorChanged();
    void selectionStartChanged();
    void selectionEndChanged();

protected:
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    QPointer<Backend> m_source;
    QColor m_color = Qt::black;
    QColor m_selectionColor = Qt::transparent;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;
    QList<MarkdownHighlighter::MathPlacement> m_placements;
};

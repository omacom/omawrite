#include "mathoverlay.h"

#include "backend.h"

#include <QHash>
#include <QImage>
#include <QPainter>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGRectangleNode>
#include <QSGTexture>
#include <QSvgRenderer>

namespace {
// Owns the formula textures, so they are released on the render thread
// together with the node tree. A transform node rather than a plain one: the
// software renderer only positions children added later under a node whose
// transform it has recorded, and it records none for plain nodes.
class MathNode : public QSGTransformNode {
public:
    ~MathNode() override { qDeleteAll(textures); }
    QHash<QByteArray, QSGTexture *> textures;
};

QByteArray textureKey(const QByteArray &svg, const QSize &pixels, const QColor &color) {
    return QByteArray::number(quint64(qHash(svg))) + ' ' + QByteArray::number(pixels.width())
           + 'x' + QByteArray::number(pixels.height()) + ' ' + color.name().toLatin1();
}

QImage rasterize(const QByteArray &svg, const QSize &pixels, const QColor &color) {
    QByteArray colored = svg;
    colored.replace("currentColor", color.name().toLatin1());
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QSvgRenderer renderer(colored);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(pixels)));
    return image;
}

// Whole device pixels, so a formula's texture maps one to one onto the screen.
QRectF snapped(const QRectF &rect, qreal dpr) {
    const QPointF topLeft(qRound(rect.x() * dpr) / dpr, qRound(rect.y() * dpr) / dpr);
    return QRectF(topLeft, QSizeF(qRound(rect.width() * dpr) / dpr,
                                  qRound(rect.height() * dpr) / dpr));
}
}

MathOverlay::MathOverlay(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
}

QObject *MathOverlay::source() const {
    return m_source.data();
}

void MathOverlay::setSource(QObject *source) {
    Backend *backend = qobject_cast<Backend *>(source);
    if (m_source == backend)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = backend;
    // Placements are read in updatePolish, right before the frame is synced,
    // so formulas never trail a frame behind the text they sit in.
    if (backend)
        connect(backend, &Backend::mathPlacementsChanged, this, &QQuickItem::polish);
    polish();
    emit sourceChanged();
}

void MathOverlay::setColor(const QColor &color) {
    if (m_color == color)
        return;
    m_color = color;
    update();
    emit colorChanged();
}

void MathOverlay::setSelectionColor(const QColor &color) {
    if (m_selectionColor == color)
        return;
    m_selectionColor = color;
    update();
    emit selectionColorChanged();
}

void MathOverlay::setSelectionStart(int position) {
    if (m_selectionStart == position)
        return;
    m_selectionStart = position;
    update();
    emit selectionStartChanged();
}

void MathOverlay::setSelectionEnd(int position) {
    if (m_selectionEnd == position)
        return;
    m_selectionEnd = position;
    update();
    emit selectionEndChanged();
}

void MathOverlay::updatePolish() {
    m_placements = m_source ? m_source->mathPlacements()
                            : QList<MarkdownHighlighter::MathPlacement>();
    update();
}

void MathOverlay::itemChange(ItemChange change, const ItemChangeData &value) {
    if (change == ItemDevicePixelRatioHasChanged)
        update();
    QQuickItem::itemChange(change, value);
}

QSGNode *MathOverlay::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
    auto *root = static_cast<MathNode *>(oldNode);
    if (!root)
        root = new MathNode;
    while (QSGNode *child = root->firstChild()) {
        root->removeChildNode(child);
        delete child;
    }

    const qreal dpr = window()->effectiveDevicePixelRatio();
    QHash<QByteArray, QSGTexture *> used;
    for (const MarkdownHighlighter::MathPlacement &placement : std::as_const(m_placements)) {
        // A selected formula's source character is drawn in the selected
        // text colour, whatever its format says; cover it with the selection.
        const bool selected = m_selectionStart <= placement.position
                              && placement.position < m_selectionEnd;
        if (selected && m_selectionColor.alpha() > 0) {
            QSGRectangleNode *backing = window()->createRectangleNode();
            backing->setRect(snapped(placement.backing, dpr));
            backing->setColor(m_selectionColor);
            root->appendChildNode(backing);
        }

        const QRectF rect = snapped(placement.image, dpr);
        const QSize pixels = (rect.size() * dpr).toSize();
        if (pixels.isEmpty())
            continue;
        const QByteArray key = textureKey(placement.svg, pixels, m_color);
        QSGTexture *texture = used.value(key);
        if (!texture)
            texture = root->textures.take(key);
        if (!texture)
            texture = window()->createTextureFromImage(rasterize(placement.svg, pixels, m_color));
        used.insert(key, texture);

        QSGImageNode *image = window()->createImageNode();
        image->setTexture(texture);
        image->setOwnsTexture(false);
        image->setFiltering(QSGTexture::Linear);
        image->setRect(rect);
        root->appendChildNode(image);
    }
    qDeleteAll(root->textures);
    root->textures = used;
    return root;
}

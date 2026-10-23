#include "render/scenerenderer.h"

#include "core/scene.h"
#include "render/textlayout.h"
#include "core/imageasset.h"
#include "core/mediaasset.h"
#include "core/shape.h"
#include "core/table.h"
#include "core/chart.h"
#include "render/shapeeffects.h"
#include "render/fillpaint.h"
#include "core/fillstyle.h"

#include <QPainterPath>

#include <QFont>
#include <QPainter>

namespace {

void paintObject(QPainter &painter, const SceneObject &object, qreal slideHeight) {
    if (object.hidden || object.opacity <= 0.0)
        return;

    painter.save();
    painter.setOpacity(object.opacity);

    if (!qFuzzyIsNull(object.rotation) || !qFuzzyCompare(object.paintScale, 1.0)) {
        const QPointF centre = object.rect.center();
        painter.translate(centre);
        painter.rotate(object.rotation);
        painter.scale(object.paintScale, object.paintScale);
        painter.translate(-centre);
    }
    if (!object.paintClip.isNull()) {
        // ANIM-001 Wipe: draw only the part of the object's box shown so far.
        const QRectF r = object.rect.normalized();
        painter.setClipRect(QRectF(r.left() + r.width() * object.paintClip.left(), r.top() + r.height() * object.paintClip.top(),
                                   r.width() * object.paintClip.width(), r.height() * object.paintClip.height()), Qt::IntersectClip);
    }

    switch (object.type) {
    case ObjectType::Chart: Chart::paint(painter,object); break;
    case ObjectType::Table: Table::paint(painter,object); break;
    case ObjectType::Rect:
        Shape::paint(painter,object);
        break;

    case ObjectType::Media: {
        auto picture=object;
        picture.image=MediaAsset::frameAt(object,object.mediaPosition);
        ImageAsset::paint(painter,picture);
        break;
    }
    case ObjectType::Image: {
        // EFFECTS-UI-1: a picture's glow and shadows follow its frame (or
        // mask), as PowerPoint draws them.
        const bool effects = (object.glowRadius > 0 && object.glowColor.alpha() > 0) || object.shadowEnabled;
        QPainterPath frame;
        if (effects) {
            SceneObject mask;
            mask.rect = object.rect;
            mask.shapeKind = object.imageMask == 1 ? 1 : object.imageMask == 2 ? 0 : object.imageMask == 3 ? 5 : object.imageMask == 4 ? 7 : 0;
            mask.cornerRadius = object.imageMask == 2 ? (object.cornerRadius > 0 ? object.cornerRadius : qMin(object.rect.width(), object.rect.height()) * .1) : 0;
            frame = Shape::path(mask);
            ShapeEffects::paintBehind(painter, object, frame, QPainterPath());
        }
        ImageAsset::paint(painter,object);
        if (effects) ShapeEffects::paintInside(painter, object, frame);
        break;
    }
    case ObjectType::Text: {
        // FILL-UI-1: a text box with a fill or an outline of its own draws
        // that box first, with the box's glow and shadow about it (as
        // PowerPoint does once the box has something to carry them); the
        // words keep their own effects. An outline with no fill casts its
        // shadow from the words as well (BOX-SHADOW-2).
        if (FillStyle::boxVisible(object)) {
            const auto pieces = FillStyle::split(object);
            Shape::paint(painter, pieces.first());
            TextLayout::paint(painter, pieces.last(), slideHeight);
        } else
            TextLayout::paint(painter, object, slideHeight);
        break;
    }
    }

    painter.restore();
}

} // namespace

void SceneRenderer::paint(QPainter &painter, const QVector<SceneObject> &states, qreal slideHeight) {
    FillPaint::setUnitsPerPoint(slideHeight / 540.0);   // FILL-UI-1: PowerPoint's slides are 540 pt high
    for (const SceneObject &state : states)
        paintObject(painter, state, slideHeight);
}

QImage SceneRenderer::render(const QVector<SceneObject> &states, const QSizeF &documentSize,
                             const QSize &pixelSize, const QColor &background, qreal slideHeight) {
    // A deck's shape decides the pixel size; never let one ask for more
    // memory than a 16K picture. Callers treat a null image as a failure.
    if (pixelSize.isEmpty() || qint64(pixelSize.width()) * pixelSize.height() > kMaxPixels)
        return QImage();
    QImage image(pixelSize, QImage::Format_RGBA8888);
    image.fill(background);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (documentSize.width() > 0.0 && documentSize.height() > 0.0) {
        painter.scale(pixelSize.width() / documentSize.width(),
                      pixelSize.height() / documentSize.height());
    }

    paint(painter, states, slideHeight > 0 ? slideHeight : documentSize.height());
    painter.end();
    return image;
}

#include "render/scenerenderer.h"

#include "core/scene.h"
#include "render/textlayout.h"
#include "core/imageasset.h"
#include "core/mediaasset.h"
#include "core/shape.h"
#include "core/table.h"
#include "core/chart.h"
#include "render/effectmodel.h"
#include "render/shapeeffects.h"
#include "render/fillpaint.h"
#include "core/fillstyle.h"
#include "core/design.h"
#include "render/layereffects.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>

#include <QPainterPath>

#include <QFont>
#include <QPainter>

namespace {

// What names a layer picture: everything about the object.
QString layerKey(const SceneObject &o, const char *part) {
    QVariantMap all = Design::properties(o);
    all.insert(QStringLiteral("mediaPosition"), o.mediaPosition);
    const QByteArray json = QJsonDocument(QJsonObject::fromVariantMap(all)).toJson(QJsonDocument::Compact);
    return QLatin1String(part) + QLatin1Char(':') + QString::fromLatin1(QCryptographicHash::hash(json, QCryptographicHash::Sha1).toHex());
}

// A shape (or a text box's box) through its soft edges, reflection and 3-D.
void paintShape(QPainter &painter, const SceneObject &o) {
    const auto look = LayerEffects::shapeLook(o);
    if (!LayerEffects::needed(look)) { Shape::paint(painter, o); return; }
    // A turned shape's outer shadow and glow come from the
    // turned picture; the flat drawing behind it leaves them out.
    SceneObject flat = o;
    if (LayerEffects::castFollows(look)) LayerEffects::dropFollowed(flat, false);
    LayerEffects::paint(painter, o.rect, look, layerKey(o, "shape"),
                        [&](QPainter &p) { Shape::paint(p, o); }, [&](QPainter &p) { Shape::paint(p, flat); });
}

// Words through their reflection and 3-D (and, with no box to carry them,
// the box's 3-D rotation).
void paintWords(QPainter &painter, const SceneObject &o, qreal slideHeight, bool boxToo) {
    auto look = LayerEffects::textLook(o);
    QRectF frame = o.rect;
    // A box turned in 3-D turns its words with it (approximately: as one flat
    // picture about the box's middle; PowerPoint's "keep text flat" is kept).
    // Not with "Keep text flat".
    if (!boxToo && !look.scene.on && o.scene3d.on && !o.textFlat) { look.scene = o.scene3d; frame = FillStyle::boxRect(o); }
    if (boxToo) {
        // A box with no fill and no outline: PowerPoint draws none of its
        // reflection, soft edges or bevel (measured); its 3-D rotation still
        // turns the words.
        const auto box = LayerEffects::shapeLook(o);
        if (!look.scene.on && !o.textFlat) look.scene = box.scene;
    }
    look.words = TextLayout::linesRect(o);
    if (!LayerEffects::needed(look)) { TextLayout::paint(painter, o, slideHeight); return; }
    // Turned words cast their (box-wide) shadow and glow from
    // the turned picture; a stretch's own effects stay flat.
    // Each stretch of words casts its own, from its own letters.
    const auto looks = TextLayout::effectLooks(o);
    look.cast = LayerEffects::castOfWords(looks.first());
    for (int i = 1; i < looks.size(); ++i) look.casts.append(LayerEffects::castOfWords(looks.at(i)));
    look.letters = [&o, slideHeight](QPainter &p, int i) { TextLayout::paintLetters(p, o, i, slideHeight); };
    SceneObject flat = o;
    if (LayerEffects::castFollows(look)) LayerEffects::dropFollowed(flat, true);
    LayerEffects::paint(painter, frame, look, layerKey(o, "words") + QString::number(slideHeight),
                        [&](QPainter &p) { TextLayout::paint(p, o, slideHeight); },
                        [&](QPainter &p) { TextLayout::paint(p, flat, slideHeight); });
}

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

    switch (object.type) {
    case ObjectType::Chart: Chart::paint(painter,object); break;
    case ObjectType::Table: Table::paint(painter,object); break;
    case ObjectType::Rect:
        paintShape(painter, object);
        break;

    case ObjectType::Media: {
        auto picture=object;
        picture.image=MediaAsset::frameAt(object,object.mediaPosition);
        ImageAsset::paint(painter,picture);
        break;
    }
    case ObjectType::Image: {
        // A picture's glow and shadows follow its frame (or
        // mask), as PowerPoint draws them.
        const bool effects = (object.glowRadius > 0 && object.glowColor.alpha() > 0) || object.shadowEnabled;
        QPainterPath frame;
        if (effects) {
            SceneObject mask;
            mask.rect = object.rect;
            mask.shapeKind = object.imageMask == 1 ? 1 : object.imageMask == 2 ? 0 : object.imageMask == 3 ? 5 : object.imageMask == 4 ? 7 : 0;
            mask.cornerRadius = object.imageMask == 2 ? (object.cornerRadius > 0 ? object.cornerRadius : qMin(object.rect.width(), object.rect.height()) * .1) : 0;
            frame = Shape::path(mask);
        }
        // A turned picture's outer shadow and glow come from the turned picture.
        const auto look = LayerEffects::shapeLook(object);
        SceneObject flat = object;
        if (LayerEffects::needed(look) && LayerEffects::castFollows(look)) LayerEffects::dropFollowed(flat, false);
        // The shadow is cast by the picture as drawn (its own transparency too).
        auto behind = [&](QPainter &p) {
            if (effects) ShapeEffects::paintBehind(p, flat, frame, QPainterPath(), [&](QPainter &q) { ImageAsset::paint(q, object); });
        };
        auto face = [&](QPainter &p) {
            ImageAsset::paint(p, object);
            if (effects) ShapeEffects::paintInside(p, object, frame);
        };
        // A picture's soft edges, reflection and 3-D.
        if (LayerEffects::needed(look)) LayerEffects::paint(painter, object.rect, look, layerKey(object, "picture"), face, behind);
        else { behind(painter); face(painter); }
        break;
    }
    case ObjectType::Text: {
        // A text box with a fill or an outline of its own draws
        // that box first, with the box's glow and shadow about it (as
        // PowerPoint does once the box has something to carry them); the
        // words keep their own effects. An outline with no fill casts its
        // shadow from the words as well.
        if (FillStyle::boxVisible(object)) {
            const auto pieces = FillStyle::split(object);
            paintShape(painter, pieces.first());
            paintWords(painter, pieces.last(), slideHeight, false);
        } else
            paintWords(painter, object, slideHeight, true);
        break;
    }
    }

    painter.restore();
}

} // namespace

void SceneRenderer::paint(QPainter &painter, const QVector<SceneObject> &states, qreal slideHeight) {
    FillPaint::setUnitsPerPoint(slideHeight / 540.0);   // PowerPoint's slides are 540 pt high
    EffectModel::setUnitsPerPoint(slideHeight / 540.0);
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

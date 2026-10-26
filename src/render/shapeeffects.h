#pragma once

// PowerPoint's glow and shadows on a shape or picture
// (p:spPr/a:effectLst: a:glow, a:outerShdw, a:innerShdw).
//
// The effects are worked out on the shape's silhouette: its fill area and its
// outline (just the outline when it has no fill, so a box with only a border
// glows along the border, inside and out, as PowerPoint draws it).
//
//   glow          the colour, at its own transparency, about the silhouette
//                 grown and softened (render/effectmodel.h: one model for
//                 every glow and shadow, measured from PowerPoint)
//   outer shadow  the silhouette resized (sx/sy) and slanted (kx/ky) about the
//                 shadowAlign point of the box, moved by the distance in its
//                 direction, softened by the blur; behind the shape
//   inner shadow  the outside of the silhouette, moved and softened, seen only
//                 inside it; over the fill, under the outline
//
// The pictures are made at the size they are drawn and kept, so a slide that
// doesn't change isn't worked out again. A plain offset shadow with no blur and
// nothing else is drawn the way OmaShow always drew it (a solid copy of the
// shape), so older decks look exactly as they did.

#include <QPainterPath>

#include <functional>

class QPainter;
struct SceneObject;

namespace ShapeEffects {

// Anything here that needs the softened (picture) path.
bool soft(const SceneObject &o);

// Glow and outer shadow; call before drawing the shape. `area` is what the
// shape fills (empty when it has no fill), `outline` the outline's own area
// (empty when it has none). `drawn`, when given, draws the object as it is
// drawn (fill and outline, or the picture), and the shadow is cast by that,
// its transparency included; the glow is the silhouette's.
void paintBehind(QPainter &painter, const SceneObject &o, const QPainterPath &area, const QPainterPath &outline,
                 const std::function<void(QPainter &)> &drawn = {});

// The inner shadow; call after the fill, before the outline.
void paintInside(QPainter &painter, const SceneObject &o, const QPainterPath &area);

// The point of the box a shadow is resized and slanted about (shadowAlign).
QPointF anchor(const QRectF &box, int align);

} // namespace ShapeEffects

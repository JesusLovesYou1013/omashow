#pragma once

// STYLE3D: the effects PowerPoint works out on the drawn object as a whole
// (soft edges, reflection, bevel, contour, depth and 3-D rotation), done on a
// picture of the object made at the size it is shown.
//
//   soft edges   EXACT IN KIND, APPROXIMATE IN SIZE: the edge fades from clear
//                at the outline to solid at the soft edge's size in (the
//                faded copy of the shape, eroded by half and blurred, as
//                PowerPoint's looks; its curve is not measured)
//   reflection   the object turned over about its alignment point (sy/sx,
//                kx/ky), moved by the distance in its direction, faded from
//                start to end transparency between the two positions along
//                the fade direction and blurred. Close to PowerPoint; rotate
//                with shape is kept but not drawn (the reflection turns with
//                the shape)
//   bevel        PROTOTYPE: a height picture of the bevel's profile from the
//                outline in (12 presets' profiles drawn from PowerPoint's
//                gallery by eye), lit by the light rig's lights
//                ([MS-OI29500]) with the material's shine, as a 2-D shading
//                of the face; the bottom bevel only shows on turned shapes
//   contour      a band of the contour colour round the outline (outside it)
//   depth        PROTOTYPE: the silhouette stacked back from the face, shaded
//                by which way its sides face the lights
//   3-D rotation PROTOTYPE: the face (and its depth) turned by the camera's
//                angles as [MS-OI29500] composes them and projected flat
//                (parallel cameras) or in perspective (field of view at
//                PowerPoint's 159.76 mm viewing scale); oblique cameras slant
//                the depth; legacy cameras as their modern near relations
//
// Everything a deck says is kept and written back as it was, whatever of it
// is drawn.

#include "core/effects3d.h"

#include <QRectF>
#include <QString>
#include <functional>

class QPainter;
struct SceneObject;

namespace LayerEffects {

// Which part of an object's drawing the shape and text painters do (for
// drawing the face apart from what goes behind it). Thread-local.
enum class Pass { All, Behind, Face };
Pass pass();
struct PassScope {
    explicit PassScope(Pass p);
    ~PassScope();
    Pass was;
};

struct Look {
    qreal softEdge = 0;
    Effects3D::Reflection reflection;
    Effects3D::Format format;
    Effects3D::Scene scene;
    QColor face = QColor(68, 114, 196);     // the depth's colour when it has none of its own
    QColor line;                            // the contour's when it has none (invalid: white)
};
bool needed(const Look &look);

// Draws an object through its look: `behind` (glow and shadow) as it is, then
// the face (`face`, drawn in slide units in the object's frame) with the
// look's effects. `box` is the object's own box; `key` names what is drawn so
// a picture already made is used again.
void paint(QPainter &painter, const QRectF &box, const Look &look, const QString &key,
           const std::function<void(QPainter &)> &face, const std::function<void(QPainter &)> &behind);

// The look an object carries: on its box (p:spPr) or on its words.
Look shapeLook(const SceneObject &o);
Look textLook(const SceneObject &o);

} // namespace LayerEffects

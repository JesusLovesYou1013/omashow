#pragma once

// The effects PowerPoint works out on the drawn object as a whole
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
//   bevel        Approximation: a height picture of the bevel's profile from the
//                outline in (12 presets' profiles drawn from PowerPoint's
//                gallery by eye), lit by the light rig's lights
//                ([MS-OI29500]) with the material's shine, as a 2-D shading
//                of the face; the bottom bevel only shows on turned shapes
//   contour      a band of the contour colour round the outline (outside it)
//   depth        Approximation: the silhouette stacked back from the face, shaded
//                by which way its sides face the lights
//   3-D rotation Approximation: the face (and its depth) turned by the camera's
//                angles as [MS-OI29500] composes them and projected flat
//                (parallel cameras) or in perspective (field of view at
//                PowerPoint's 159.76 mm viewing scale); oblique cameras slant
//                the depth; legacy cameras as their modern near relations
//
// On a turned object (3-D rotation) PowerPoint works its outer
// shadow, glow and reflection out on the turned picture: the shadow is the
// turned silhouette (front, sides and all) moved, resized and slanted in the
// slide's own directions, the reflection that picture turned over below its
// drawn bounds. OmaShow does the same while effects3dFollow is on (the
// default); off, they are the flat object's, as before.
//   shadow, glow  EXACT IN KIND (same silhouette rule as a flat shape's, on
//                 the turned picture; on words, each stretch's from its own
//                 turned letters); as approximate as the turned picture
//   reflection    in the object's own plane: the flat face's reflection,
//                 turned with it (measured: it runs along the turned foot);
//                 off: the flat face's
//
// Everything a deck says is kept and written back as it was, whatever of it
// is drawn.

#include "core/effects3d.h"

#include <QRectF>
#include <QVector>
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

// The outer shadow and glow worked out on the turned picture.
struct Cast {
    bool shadow = false;
    QColor shadowColor;
    qreal dx = 0, dy = 0, blur = 0, sx = 100, sy = 100, kx = 0, ky = 0;   // slide units, percent, degrees
    int align = 0;
    qreal glow = 0;
    QColor glowColor;
    qreal k = 1;   // every length times this (EffectModel::textScale for the words' own effects)
    bool any() const { return (shadow && shadowColor.alpha() > 0) || (glow > 0 && glowColor.alpha() > 0); }
};

struct Look {
    qreal softEdge = 0;
    Effects3D::Reflection reflection;
    Effects3D::Format format;
    Effects3D::Scene scene;
    QColor face = QColor(68, 114, 196);     // the depth's colour when it has none of its own
    QColor line;                            // the contour's when it has none (invalid: white)
    Cast cast;
    bool follow = true;                     // effects3dFollow
    QRectF words;                           // words: their lines (a reflection's foot); else invalid
    // Words: each stretch with effects of its own casts its own (after `cast`,
    // which the rest of the letters cast), and `letters(painter, i)` draws only
    // the letters cast i comes from (0: `cast`, i: casts[i - 1]), as the face is.
    QVector<Cast> casts;
    std::function<void(QPainter &, int)> letters;
};
bool needed(const Look &look);
// The look turns (3-D rotation drawn) and its effects follow.
bool turned(const Look &look);
// The cast is drawn from the turned picture (so the flat object's isn't).
bool castFollows(const Look &look);
// What an object's own drawing leaves out of its flat shadow and glow when
// the look draws them turned: outer shadow off, glow off (`textGlow` too for words).
void dropFollowed(SceneObject &o, bool words);

// Draws an object through its look: `behind` (glow and shadow) as it is, then
// the face (`face`, drawn in slide units in the object's frame) with the
// look's effects. `box` is the object's own box; `key` names what is drawn so
// a picture already made is used again.
void paint(QPainter &painter, const QRectF &box, const Look &look, const QString &key,
           const std::function<void(QPainter &)> &face, const std::function<void(QPainter &)> &behind);

// The look an object carries: on its box (p:spPr) or on its words.
Look shapeLook(const SceneObject &o);
Look textLook(const SceneObject &o);
// The outer shadow and glow a stretch of words (or the words) carries.
Cast castOfWords(const SceneObject &o);

} // namespace LayerEffects

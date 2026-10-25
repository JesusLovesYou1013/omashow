#pragma once

// One model of PowerPoint's glow and shadows, shared by every object that
// casts them (shapes, pictures, text boxes, the words themselves, and turned
// objects), worked out from PowerPoint's own pictures of test slides (black-box
// measurement of what it draws; nothing else). Lengths are slide units unless
// said otherwise.
//
//   softening   a Gaussian whose standard deviation is a third of the blur
//               radius (fits PowerPoint's shadows to 0.0005 strength, any
//               colour, transparency, distance and direction)
//   strength    exactly the colour's own opacity, no gamma: the silhouette's
//               own opacity times the colour's (a half-transparent fill casts
//               a half-strength shadow)
//   shadow      the silhouette slanted, then resized about its alignment point,
//               moved by the distance in its direction, then softened
//   inner       what lies outside the silhouette, moved against the direction,
//               softened, seen only inside
//   glow        the silhouette grown by glowGrow(radius) and softened by a
//               sixth of the radius (a glow is a blur of half its radius)
//   words       effects set on the words (a:rPr) are all those lengths times
//               textScale(size): PowerPoint draws them smaller on smaller text
//               (offset, blur and glow alike); an inner shadow on words is
//               0.6 of that again; a perspective shadow on words turns about
//               the laid-out lines' point (not the text box's, not
//               the letters' ink)
//
// The pictures this makes are in pixels; `scale` is pixels per slide unit.

#include <QRectF>
#include <QTransform>
#include <QVector>

namespace EffectModel {

// Slide units per point for the slide being drawn (SceneRenderer::paint sets
// it; a 1080-high slide's 2 otherwise).
void setUnitsPerPoint(qreal units);
qreal unitsPerPoint();

// The deviation, slide units, of a blur radius.
qreal sigma(qreal blur);
// How far a glow of this radius grows the silhouette before softening, and the
// softening (slide units). `point` is slide units per point.
qreal glowGrow(qreal radius, qreal point);
qreal glowSigma(qreal radius);
// What the words' own effects are multiplied by at this text size (points),
// and their inner shadow's (smaller again: 0.6 of that, measured).
qreal textScale(qreal sizePoints);
qreal textInnerScale(qreal sizePoints);

// `region` (slide units) grown so its corner falls on a whole pixel of the
// painter (when it only scales and moves) and its size is whole pixels at
// `scale`, so a picture made for it is drawn exactly where it was worked out,
// not half a pixel off.
QRectF onPixels(const QTransform &painter, const QRectF &region, qreal scale);

// Blurs a plane of strengths 0-1 in place: a Gaussian of this deviation
// (pixels). Past its edges the plane goes on at `outside`.
void gaussian(QVector<float> &plane, int w, int h, qreal sigmaPixels, float outside = 0.f);
// Distance from every pixel to the nearest marked one, in pixels (exact
// Euclidean, Felzenszwalb & Huttenlocher's two passes).
QVector<float> distanceTo(const QVector<bool> &marked, int w, int h);
// The glow's strengths from a silhouette plane: grown, softened, at
// `strength` (the glow colour's opacity). Pixels.
QVector<float> glow(const QVector<float> &silhouette, int w, int h, qreal growPixels, qreal sigmaPixels, float strength);

} // namespace EffectModel

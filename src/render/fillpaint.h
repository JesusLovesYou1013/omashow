#pragma once

// Drawing PowerPoint's fills and lines (Format Shape > Fill, Line)
// for what core/fillstyle.h calls modern: gradients with their own stops and
// circle, rectangle and shape paths; the 54 preset patterns; tiled, inset and
// see-through pictures; preset and custom dashes, compound and gradient lines.
// Anything else is drawn by Shape::paint as it always was, so older decks look
// exactly the same.
//
// How each is drawn:
//   linear     a:lin: the stops along the angle, from the corner of the box
//              behind the line through the middle to the corner ahead of it;
//              scaled works in the unit square, so the box stretches it
//   circle     a:path circle: from the focus box (a:fillToRect) out to the box's
//              farthest corner
//   rectangle  a:path rect: from the focus box out to each side of the box
//   shape      a:path shape: the outline shrunk towards the focus, step by step
//   pattern    8 by 8 cells of a point each, fixed to the slide, not the shape
//              (as PowerPoint's own PDFs show them)
//   picture    tiles at the picture's own size (its dots per inch, 96 when it
//              has none) times the scale, from the aligned corner plus offset
//   lines      the outline's area, cut into the compound's parallel lines and
//              then into dashes, filled with the colour or gradient
// "Rotate with shape" off turns the gradient or tiles back against the
// shape's rotation, so they keep to the page.

#include <QPainterPath>

class QPainter;
struct SceneObject;

namespace FillPaint {

// Slide units per point, for patterns and tiles: set by SceneRenderer::paint
// from the slide's height (540 pt for PowerPoint's slides).
void setUnitsPerPoint(qreal units);
qreal unitsPerPoint();

// Fills `area` (the shape's path, in the shape's own frame) with o's fill.
void paintFill(QPainter &painter, const SceneObject &o, const QPainterPath &area);
// The outline's own area (compound lines and dashes cut out of it).
QPainterPath lineArea(const SceneObject &o, const QPainterPath &path);
// Draws the outline of `path` with o's line.
void paintLine(QPainter &painter, const SceneObject &o, const QPainterPath &path);

} // namespace FillPaint

#pragma once

// The one place scene objects become pixels.
//
// The live canvas and the offscreen shot harness both come through here, which
// is what makes "what you rehearsed is what you export" testable rather than
// hopeful. This is also the seam a Skia painter slots behind later — the
// signature takes a QPainter today because QPainter is the reference
// implementation, not because it is the only one.

#include <QColor>
#include <QImage>
#include <QSizeF>
#include <QVector>

class QPainter;
struct SceneObject;

namespace SceneRenderer {

// Paints into a painter already scaled so that one unit == one document unit.
// `slideHeight` is the slide's height in those units, for effects sized in
// points (TextLayout::paint); 1080 is a 16:9 slide.
void paint(QPainter &painter, const QVector<SceneObject> &states, qreal slideHeight = 1080);

// The most pixels one render may ask for: a 16K picture (16384 x 8192).
constexpr qint64 kMaxPixels = 16384LL * 8192;

// Returns a null image when pixelSize is empty or larger than kMaxPixels.
// `slideHeight` as for paint(); left out, documentSize is taken to be the slide.
QImage render(const QVector<SceneObject> &states, const QSizeF &documentSize,
              const QSize &pixelSize, const QColor &background, qreal slideHeight = 0);

} // namespace SceneRenderer

#pragma once

// Windows drawings — the WMF and EMF pictures of old clip art and Office
// decks — played back into SVG, which the deck keeps as a drawing.
//
// The readers (`ppt.*`, `pptx.*`, `odp.*`) hand a metafile here instead of
// leaving the picture out. Every GDI drawing call clip art uses is played:
// lines, shapes, polygons, curves, paths, pens and brushes, fonts and text,
// bitmaps, clipping, the window/viewport mapping and the world transform. What
// SVG cannot do (raster operations, hatch spacing) is drawn in the nearest
// form. EMF+ records riding inside an EMF are skipped for the EMF drawing
// every such file also carries.

#include <QByteArray>
#include <QString>

namespace Metafile {

// Whether the bytes are a WMF (placeable or not) or an EMF.
bool recognise(const QByteArray &bytes);

// The drawing as an SVG document, or empty with `error` set.
QByteArray toSvg(const QByteArray &bytes, QString *error = nullptr);

} // namespace Metafile

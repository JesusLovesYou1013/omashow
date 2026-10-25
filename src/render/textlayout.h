#pragma once
#include "core/scene.h"
#include <QVector>
class QPainter;
namespace TextLayout {
struct Metrics {
    qreal naturalHeight = 0, renderedHeight = 0, effectiveSize = 0;
    bool overflow = false;
};
QString editorHtml(const SceneObject &object);
Metrics measure(const SceneObject &object);
// Where the lines sit in the box (its width; their height, aligned as drawn).
QRectF linesRect(const SceneObject &object);
// `slideHeight` (slide units) says how big a point is: PowerPoint's slides are
// 7.5 inches tall at both 16:9 and 4:3. Only effects measured in points
// (text glow and shadow) use it; 1080 is a 16:9 slide.
void paint(QPainter &painter, const SceneObject &object, qreal slideHeight = 1080);
// The words' own effects as they are drawn: the box's (on the letters with
// none of their own) first, then each stretch's with effects of its own.
QVector<SceneObject> effectLooks(const SceneObject &object);
// Only the letters that carry effectLooks(object)[index] (what those effects
// are cast from), drawn where paint() draws them.
void paintLetters(QPainter &painter, const SceneObject &object, int index, qreal slideHeight = 1080);
} // namespace TextLayout

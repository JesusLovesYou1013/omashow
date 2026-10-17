#pragma once
#include "core/scene.h"
class QPainter;
namespace TextLayout {
struct Metrics {
    qreal naturalHeight = 0, renderedHeight = 0, effectiveSize = 0;
    bool overflow = false;
};
QString editorHtml(const SceneObject &object);
Metrics measure(const SceneObject &object);
// What a kept shrink-to-fit box (SceneObject::fitKept) is checked against: its
// words, its box and its size before any font swap factor.
QString keptFitKey(const SceneObject &object);
// `slideHeight` (slide units) says how big a point is: PowerPoint's slides are
// 7.5 inches tall at both 16:9 and 4:3. Only effects measured in points
// (text glow and shadow) use it; 1080 is a 16:9 slide.
void paint(QPainter &painter, const SceneObject &object, qreal slideHeight = 1080);
} // namespace TextLayout

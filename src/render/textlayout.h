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
void paint(QPainter &painter, const SceneObject &object);
} // namespace TextLayout

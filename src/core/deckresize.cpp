#include "core/deckresize.h"
#include "core/starter.h"
#include "core/table.h"
bool DeckResize::apply(Document &document, const QSizeF &size,
                       bool scaleContent) {
  if (!Starter::validSize(size) || document.size.isEmpty() ||
      document.size == size)
    return false;
  if (scaleContent) {
    const qreal scale = qMin(size.width() / document.size.width(),
                             size.height() / document.size.height());
    const QPointF offset((size.width() - document.size.width() * scale) / 2,
                         (size.height() - document.size.height() * scale) / 2);
    const auto transform = [&](SceneObject &o) {
      scaleObject(o, scale);
      o.rect.translate(offset);
      o.connectorStart += offset;
      o.connectorEnd += offset;
    };
    for (auto &master : document.masters)
      for (auto &o : master.objects)
        transform(o);
    for (auto &layout : document.layouts)
      for (auto &o : layout.placeholders)
        transform(o);
    for (auto &slide : document.slides)
      for (auto &o : slide.objects)
        transform(o);
  }
  document.size = size;
  return true;
}

void DeckResize::scaleObject(SceneObject &o, qreal scale, const QPointF &origin) {
  o.rect = QRectF(origin + (o.rect.topLeft() - origin) * scale, o.rect.size() * scale);
  Table::scale(o.table, scale);
  o.fontSize *= scale;
  o.origFontSize *= scale;
  o.textGlowRadius *= scale;   // a swapped Office font's size before its size factor
  o.letterSpacing *= scale;
  o.paragraphSpacing *= scale;
  o.textIndent *= scale;
  o.cornerRadius *= scale;
  o.connectorStart = origin + (o.connectorStart - origin) * scale;
  o.connectorEnd = origin + (o.connectorEnd - origin) * scale;
  o.strokeWidth *= scale;
  o.shadowX *= scale;
  o.shadowY *= scale;
  o.shadowBlur *= scale;
  o.glowRadius *= scale;
  o.boxShadowX *= scale;   // BOTH-EFFECTS-1
  o.boxShadowY *= scale;
  o.boxShadowBlur *= scale;
  o.pictureTileX *= scale;   // FILL-UI-1
  o.pictureTileY *= scale;
  o.boxInsetL *= scale;
  o.boxInsetT *= scale;
  o.boxInsetR *= scale;
  o.boxInsetB *= scale;
  // STYLE3D: soft edges, reflections, bevels and depth; text outlines and a
  // stretch's own effects.
  o.softEdge *= scale;
  Effects3D::scale(o.reflection, scale);
  Effects3D::scale(o.textReflection, scale);
  Effects3D::scale(o.format3d, scale);
  Effects3D::scale(o.textFormat3d, scale);
  o.textFlatZ *= scale;
  auto scaleKeys = [scale](QVariantMap &m, std::initializer_list<const char *> keys) {
    for (const char *key : keys)
      if (m.contains(QLatin1String(key))) m.insert(QLatin1String(key), m.value(QLatin1String(key)).toDouble() * scale);
  };
  scaleKeys(o.textOutline, {"strokeWidth"});
  for (auto &run : o.runs) {
    scaleKeys(run.outline, {"strokeWidth"});
    scaleKeys(run.effects, {"textGlowRadius", "shadowX", "shadowY", "shadowBlur"});
    if (run.effects.contains(QStringLiteral("textReflection"))) {
      Effects3D::Reflection r;
      if (Effects3D::fromMap(run.effects.value(QStringLiteral("textReflection")), &r)) {
        Effects3D::scale(r, scale);
        run.effects.insert(QStringLiteral("textReflection"), Effects3D::toMap(r));
      }
    }
  }
}

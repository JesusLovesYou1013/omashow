#pragma once

// EFFECTS-UI-1: the small pictures in the Style tab's glow and shadow presets,
// drawn by the slide renderer itself so each one shows what the preset will
// look like on the slide. The image id is "<text|shape>?<settings>", the
// settings in points (one point a slide unit here):
//   g  glow size     gc glow colour (#aarrggbb)
//   s  shadow on     sc shadow colour   sb blur   sx/sy offset
//   si inner         ssx/ssy size (%)   skx/sky slant (degrees)   sa anchor (0-8)
//   f  the sample's own colour
// e.g. image://effects/shape?g=8&gc=%23664472c4

#include <QQuickImageProvider>

class EffectPreviewProvider : public QQuickImageProvider {
public:
    EffectPreviewProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

#include "render/effectpreview.h"

#include "core/design.h"
#include "core/fillstyle.h"
#include "core/scene.h"
#include "render/scenerenderer.h"

#include <QJsonDocument>
#include <QUrlQuery>

EffectPreviewProvider::EffectPreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage EffectPreviewProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    const int q = id.indexOf(QLatin1Char('?'));
    const QString kind = q < 0 ? id : id.left(q);
    const QUrlQuery query(q < 0 ? QString() : id.mid(q + 1));
    const auto number = [&](const char *key, qreal fallback) {
        bool ok = false;
        const qreal v = query.queryItemValue(QLatin1String(key), QUrl::FullyDecoded).toDouble(&ok);
        return ok && std::isfinite(v) ? v : fallback;
    };
    const auto colour = [&](const char *key, const QColor &fallback) {
        const QColor c = QColor::fromString(query.queryItemValue(QLatin1String(key), QUrl::FullyDecoded));
        return c.isValid() ? c : fallback;
    };
    // A 64 pt square: the sample in the middle, room around it for the effect.
    constexpr qreal tile = 64;
    SceneObject o;
    o.id = QStringLiteral("preview");
    // BOTH-EFFECTS-1 (lit=1): a light card behind a dark sample, so a soft
    // black shadow (the perspective ones) reads on the dark panel.
    const bool lit = number("lit", 0) != 0;
    const QColor own = colour("f", lit ? QColor(0x3a, 0x41, 0x50) : QColor(0xd8, 0xdc, 0xe2));
    if (kind == QLatin1String("fill")) {
        // FILL-UI-1: a fill or line sample (gallery and pattern swatches):
        // p is the shape's fill and line keys as JSON. Unknown keys are ignored.
        o.type = ObjectType::Rect;
        o.rect = QRectF(6, 6, 52, 52);
        o.fillStyle = 5;
        o.strokeWidth = 0;
        const auto values = QJsonDocument::fromJson(query.queryItemValue(QStringLiteral("p"), QUrl::FullyDecoded).toUtf8()).toVariant().toMap();
        const bool line = values.contains(QStringLiteral("strokeWidth")) && !values.contains(QStringLiteral("fillStyle"));
        if (line) o.rect = QRectF(4, 20, 56, 24);
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            if (it.key().startsWith(QLatin1String("fill")) || it.key().startsWith(QLatin1String("gradient")) || it.key().startsWith(QLatin1String("pattern")) || it.key().startsWith(QLatin1String("stroke")))
                Design::setProperty(o, it.key(), it.value());
        FillStyle::syncLegacy(o);
        const QSize px = requestedSize.isValid() && !requestedSize.isEmpty() ? requestedSize.boundedTo(QSize(512, 512)) : QSize(96, 96);
        const QImage image = SceneRenderer::render({o}, QSizeF(tile, tile), px, QColor(Qt::transparent), 540);
        if (size) *size = image.size();
        return image;
    }
    if (kind == QLatin1String("text")) {
        o.type = ObjectType::Text;
        o.text = QStringLiteral("A");
        o.fontSize = 40;
        o.fontWeight = 700;
        o.textColor = own;
        o.textAlign = 1;
        o.verticalAlign = 1;
        o.textFit = 2;
        o.rect = QRectF(12, 10, 40, 44);
        o.textGlowRadius = qBound<qreal>(0, number("g", 0), 100);
        o.textGlowColor = colour("gc", QColor(0, 0, 0, 0));
    } else {
        o.type = ObjectType::Rect;
        o.rect = QRectF(18, 18, 28, 28);
        o.cornerRadius = 4;
        o.fill = own;
        o.fillStyle = 0;
        o.glowRadius = qBound<qreal>(0, number("g", 0), 100);
        o.glowColor = colour("gc", QColor(0, 0, 0, 0));
    }
    o.shadowEnabled = number("s", 0) != 0;
    o.shadowColor = colour("sc", QColor(0, 0, 0, 102));
    o.shadowBlur = qBound<qreal>(0, number("sb", 0), 100);
    o.shadowX = qBound<qreal>(-100, number("sx", 0), 100);
    o.shadowY = qBound<qreal>(-100, number("sy", 0), 100);
    o.shadowInner = number("si", 0) != 0;
    o.shadowScaleX = qBound<qreal>(-200, number("ssx", 100), 200);
    o.shadowScaleY = qBound<qreal>(-200, number("ssy", 100), 200);
    o.shadowSkewX = qBound<qreal>(-89, number("skx", 0), 89);
    o.shadowSkewY = qBound<qreal>(-89, number("sky", 0), 89);
    o.shadowAlign = qBound(0, int(number("sa", 0)), 8);
    // STYLE3D (x): soft edge, reflection and 3-D as JSON of their keys, in
    // points (softEdge, reflection, format3d, scene3d; the text sample takes
    // them on its words). Unknown keys and bad values are ignored.
    if (query.hasQueryItem(QStringLiteral("x"))) {
        const auto values = QJsonDocument::fromJson(query.queryItemValue(QStringLiteral("x"), QUrl::FullyDecoded).toUtf8()).toVariant().toMap();
        const bool text = o.type == ObjectType::Text;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            QString key = it.key();
            if (key != QLatin1String("softEdge") && key != QLatin1String("reflection") && key != QLatin1String("format3d") && key != QLatin1String("scene3d")) continue;
            if (text && key == QLatin1String("softEdge")) continue;
            if (text) key = QStringLiteral("text") + key.left(1).toUpper() + key.mid(1);
            Design::setProperty(o, key, it.value());
        }
        // Room for a reflection below the sample.
        if (values.contains(QStringLiteral("reflection"))) o.rect.translate(0, text ? -8 : -12);
    }
    const QSize px = requestedSize.isValid() && !requestedSize.isEmpty() ? requestedSize.boundedTo(QSize(512, 512)) : QSize(96, 96);
    // Slide height 540: one slide unit is a point, as the settings are.
    const QImage image = SceneRenderer::render({o}, QSizeF(tile, tile), px, lit ? QColor(0xee, 0xf1, 0xf5) : QColor(Qt::transparent), 540);
    if (size) *size = image.size();
    return image;
}

#include "render/shapeeffects.h"

#include "core/scene.h"
#include "render/effectmodel.h"

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QtMath>

#include <cmath>

namespace {

// A path's coverage as strengths 0-1, over `region` (slide units) at `scale`
// pixels per unit, moved by `t` first.
QVector<float> coverage(const QPainterPath &path, const QTransform &t, const QRectF &region, qreal scale, int w, int h) {
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(scale, scale);
        p.translate(-region.topLeft());
        p.setTransform(t, true);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawPath(path);
    }
    QVector<float> plane(w * h);
    for (int y = 0; y < h; ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; ++x) plane[y * w + x] = qAlpha(row[x]) / 255.f;
    }
    return plane;
}

QString signature(const QPainterPath &path) {
    size_t h = 0;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto e = path.elementAt(i);
        h = qHashMulti(h, int(e.type), qRound(e.x * 64), qRound(e.y * 64));
    }
    return QString::number(h, 16) + QLatin1Char(':') + QString::number(path.elementCount());
}

// The shadow's own move: slanted, then resized, about the anchor, then moved.
// PowerPoint slants the whole shape before resizing it (measured: the top of an
// "upper left" perspective shadow moves tan(20°) x the full height).
QTransform shadowMove(const SceneObject &o) {
    const QPointF a = ShapeEffects::anchor(o.rect, o.shadowAlign);
    QTransform t;
    t.translate(o.shadowX, o.shadowY);
    t.translate(a.x(), a.y());
    t.scale(o.shadowScaleX / 100.0, o.shadowScaleY / 100.0);
    t.shear(std::tan(qDegreesToRadians(o.shadowSkewX)), std::tan(qDegreesToRadians(o.shadowSkewY)));
    t.translate(-a.x(), -a.y());
    return t;
}

// What the object casts, as drawn by `drawn` (its own transparency, so a
// half-transparent fill casts a half-strength shadow, as PowerPoint does), over
// `region` at `scale`, moved by `t` first.
QVector<float> drawnCoverage(const std::function<void(QPainter &)> &drawn, const QTransform &t, const QRectF &region, qreal scale, int w, int h) {
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.scale(scale, scale);
        p.translate(-region.topLeft());
        p.setTransform(t, true);
        drawn(p);
    }
    QVector<float> plane(w * h);
    for (int y = 0; y < h; ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; ++x) plane[y * w + x] = qAlpha(row[x]) / 255.f;
    }
    return plane;
}

QMutex lock;
QCache<QString, QImage> &cache() {
    static QCache<QString, QImage> kept(48 * 1024 * 1024);   // bytes
    return kept;
}

QImage cached(const QString &key) {
    QMutexLocker locked(&lock);
    if (const QImage *kept = cache().object(key)) return *kept;
    return QImage();
}
void keep(const QString &key, const QImage &image) {
    QMutexLocker locked(&lock);
    cache().insert(key, new QImage(image), int(qMin<qint64>(INT_MAX, qint64(image.sizeInBytes()))));
}

qreal drawScale(const QPainter &painter, const QSizeF &full) {
    const QTransform &t = painter.worldTransform();
    const qreal scale = std::sqrt(t.m11() * t.m11() + t.m12() * t.m12());
    return qBound(0.02, scale, 4096.0 / qMax(1.0, qMax(full.width(), full.height())));
}

} // namespace

QPointF ShapeEffects::anchor(const QRectF &box, int align) {
    const int column = qBound(0, align, 8) % 3, row = qBound(0, align, 8) / 3;
    return QPointF(box.left() + box.width() * column / 2.0, box.top() + box.height() * row / 2.0);
}

bool ShapeEffects::soft(const SceneObject &o) {
    const bool glow = o.glowRadius > 0 && o.glowColor.alpha() > 0;
    const bool shadow = o.shadowEnabled && o.shadowColor.alpha() > 0;
    if (glow) return true;
    if (!shadow) return false;
    // A see-through fill or outline casts a fainter shadow (render/effectmodel.h).
    const bool seeThrough = (o.fillStyle != 5 && o.fill.alpha() < 255) || (o.strokeWidth > 0 && o.strokeColor.alpha() < 255);
    return o.shadowInner || o.shadowBlur > 0 || seeThrough || !qFuzzyCompare(o.shadowScaleX, 100.0) || !qFuzzyCompare(o.shadowScaleY, 100.0) ||
           !qFuzzyIsNull(o.shadowSkewX) || !qFuzzyIsNull(o.shadowSkewY);
}

void ShapeEffects::paintBehind(QPainter &painter, const SceneObject &o, const QPainterPath &area, const QPainterPath &outline,
                               const std::function<void(QPainter &)> &drawn) {
    const bool glow = o.glowRadius > 0 && o.glowColor.alpha() > 0;
    const bool shadow = o.shadowEnabled && !o.shadowInner && o.shadowColor.alpha() > 0;
    if (!glow && !shadow) return;
    QPainterPath shape = area.united(outline);
    shape.setFillRule(Qt::WindingFill);
    if (shape.isEmpty()) return;
    const QRectF bounds = shape.boundingRect();
    const QTransform move = shadow ? shadowMove(o) : QTransform();
    const qreal sigma = shadow ? EffectModel::sigma(o.shadowBlur) : 0;
    const qreal grow = glow ? EffectModel::glowGrow(o.glowRadius, EffectModel::unitsPerPoint()) : 0;
    const qreal glowSigma = glow ? EffectModel::glowSigma(o.glowRadius) : 0;
    const qreal reach = grow + 4 * glowSigma;
    QRectF region = bounds.adjusted(-reach, -reach, reach, reach);
    if (shadow) region = region.united(move.map(shape).boundingRect().adjusted(-4 * sigma, -4 * sigma, 4 * sigma, 4 * sigma));
    region.adjust(-1, -1, 1, 1);
    const qreal scale = drawScale(painter, region.size());
    region = EffectModel::onPixels(painter.worldTransform(), region, scale);
    const int w = qMax(1, qRound(region.width() * scale)), h = qMax(1, qRound(region.height() * scale));
    // What the object casts: as drawn when the caller can draw it, else its
    // silhouette at full strength.
    QVector<float> cast;
    QString castKey = QStringLiteral("solid");
    if (shadow && drawn) {
        cast = drawnCoverage(drawn, move, region, scale, w, h);
        castKey = QString::number(qHashBits(cast.constData(), size_t(cast.size()) * sizeof(float)), 16);
    }
    const QString key = QStringList{QStringLiteral("behind"), signature(shape), QString::number(w), QString::number(h),
                                    QString::number(region.x()), QString::number(region.y()),
                                    QString::number(glow ? o.glowRadius : 0), o.glowColor.name(QColor::HexArgb),
                                    QString::number(shadow), o.shadowColor.name(QColor::HexArgb), QString::number(sigma),
                                    QString::number(move.m11()), QString::number(move.m12()), QString::number(move.m21()),
                                    QString::number(move.m22()), QString::number(move.dx()), QString::number(move.dy()),
                                    QString::number(grow), castKey}
                            .join(QLatin1Char('|'));
    QImage picture = cached(key);
    if (picture.isNull()) {
        QVector<float> halo(w * h, 0.f);
        if (glow)
            halo = EffectModel::glow(coverage(shape, QTransform(), region, scale, w, h), w, h, grow * scale, glowSigma * scale,
                                     float(o.glowColor.alphaF()));
        if (shadow) {
            if (cast.isEmpty()) cast = coverage(shape, move, region, scale, w, h);
            EffectModel::gaussian(cast, w, h, sigma * scale, 0.f);
            const float strength = float(o.shadowColor.alphaF());
            for (auto &v : cast) v *= strength;
        }
        QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
        const QColor g = o.glowColor, c = o.shadowColor;
        for (int y = 0; y < h; ++y) {
            auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                const float ga = qBound(0.f, halo[i], 1.f), sa = shadow ? qBound(0.f, cast[i], 1.f) * (1 - ga) : 0.f;
                row[x] = qRgba(qRound(g.red() * ga + c.red() * sa), qRound(g.green() * ga + c.green() * sa),
                               qRound(g.blue() * ga + c.blue() * sa), qRound((ga + sa) * 255));
            }
        }
        picture = image;
        keep(key, picture);
    }
    painter.drawImage(QRectF(region.topLeft(), QSizeF(w / scale, h / scale)), picture);
}

void ShapeEffects::paintInside(QPainter &painter, const SceneObject &o, const QPainterPath &area) {
    if (!o.shadowEnabled || !o.shadowInner || o.shadowColor.alpha() == 0 || area.isEmpty()) return;
    const qreal sigma = EffectModel::sigma(o.shadowBlur);
    // The outside moves against the shadow's direction, so the edge the shadow
    // points to darkens (as PowerPoint draws it: dir 45° darkens bottom and right).
    const QTransform move = QTransform::fromTranslate(-o.shadowX, -o.shadowY);
    // What casts it is the shape with its outline (as PowerPoint, measured: the
    // band is narrower by the outline), and the picture is made wide enough to
    // hold all of it moved and softened, so nothing past its edge counts as
    // outside.
    QPainterPath cast = area;
    if (o.strokeWidth > 0) {
        QPainterPathStroker stroker;
        stroker.setWidth(o.strokeWidth);
        stroker.setJoinStyle(Qt::MiterJoin);
        cast = cast.united(stroker.createStroke(area));
        cast.setFillRule(Qt::WindingFill);
    }
    const qreal reach = 4 * sigma + 1;
    QRectF region = area.boundingRect().united(move.map(cast).boundingRect()).adjusted(-reach, -reach, reach, reach);
    const qreal scale = drawScale(painter, region.size());
    region = EffectModel::onPixels(painter.worldTransform(), region, scale);
    const int w = qMax(1, qRound(region.width() * scale)), h = qMax(1, qRound(region.height() * scale));
    const QString key = QStringList{QStringLiteral("inside"), signature(area), QString::number(w), QString::number(h),
                                    QString::number(region.x()), QString::number(region.y()), o.shadowColor.name(QColor::HexArgb),
                                    QString::number(sigma), QString::number(o.shadowX), QString::number(o.shadowY),
                                    QString::number(o.strokeWidth)}
                            .join(QLatin1Char('|'));
    QImage picture = cached(key);
    if (picture.isNull()) {
        const QVector<float> body = coverage(area, QTransform(), region, scale, w, h);
        // What lies outside the shape, moved back by the shadow's offset and
        // softened, seen through the shape.
        QVector<float> outside = coverage(cast, move, region, scale, w, h);
        for (auto &v : outside) v = 1 - v;
        EffectModel::gaussian(outside, w, h, sigma * scale, 1.f);
        const QColor c = o.shadowColor;
        const float strength = float(c.alphaF());
        QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h; ++y) {
            auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                const float a = qBound(0.f, outside[i], 1.f) * body[i] * strength;
                row[x] = qRgba(qRound(c.red() * a), qRound(c.green() * a), qRound(c.blue() * a), qRound(a * 255));
            }
        }
        picture = image;
        keep(key, picture);
    }
    painter.drawImage(QRectF(region.topLeft(), QSizeF(w / scale, h / scale)), picture);
}

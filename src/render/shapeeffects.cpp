#include "render/shapeeffects.h"

#include "core/scene.h"

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QtMath>

#include <cmath>

namespace {

// Blurs a plane of strengths in place: a Gaussian of this standard deviation
// (pixels), one direction then the other. Past its edges the plane is taken to
// go on at `outside` (0 for a shadow; 1 for what lies outside an inner one).
void gaussian(QVector<float> &plane, int w, int h, qreal sigma, float outside) {
    if (sigma < 0.3) return;
    const int r = qCeil(sigma * 3);
    QVector<float> k(2 * r + 1);
    float sum = 0;
    for (int i = -r; i <= r; ++i) sum += (k[i + r] = float(std::exp(-i * i / (2 * sigma * sigma))));
    for (auto &v : k) v /= sum;
    QVector<float> line(qMax(w, h));
    auto pass = [&](int count, int stride, float *start) {
        for (int i = 0; i < count; ++i) line[i] = start[i * stride];
        for (int i = 0; i < count; ++i) {
            float v = 0;
            for (int j = -r; j <= r; ++j) {
                const int at = i + j;
                v += (at < 0 || at >= count ? outside : line[at]) * k[j + r];
            }
            start[i * stride] = v;
        }
    };
    for (int y = 0; y < h; ++y) pass(w, 1, plane.data() + y * w);
    for (int x = 0; x < w; ++x) pass(h, w, plane.data() + x);
}

// Distance from every pixel to the nearest marked one, in pixels (exact
// Euclidean, Felzenszwalb & Huttenlocher's two passes).
QVector<float> distanceTo(const QVector<bool> &marked, int w, int h) {
    const float far = 1e20f;
    QVector<float> d(w * h);
    for (int i = 0; i < w * h; ++i) d[i] = marked[i] ? 0 : far;
    const int n = qMax(w, h);
    QVector<float> f(n), out(n), z(n + 1);
    QVector<int> v(n);
    auto pass = [&](int count) {
        int k = 0;
        v[0] = 0; z[0] = -far; z[1] = far;
        for (int q = 1; q < count; ++q) {
            auto meet = [&](int r) { return ((f[q] + q * q) - (f[r] + r * r)) / (2.0f * (q - r)); };
            float s = meet(v[k]);
            while (s <= z[k]) s = meet(v[--k]);
            ++k; v[k] = q; z[k] = s; z[k + 1] = far;
        }
        k = 0;
        for (int q = 0; q < count; ++q) {
            while (z[k + 1] < q) ++k;
            out[q] = (q - v[k]) * (q - v[k]) + f[v[k]];
        }
    };
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) f[y] = d[y * w + x];
        pass(h);
        for (int y = 0; y < h; ++y) d[y * w + x] = out[y];
    }
    for (int y = 0; y < h; ++y) {
        std::copy(d.cbegin() + y * w, d.cbegin() + (y + 1) * w, f.begin());
        pass(w);
        for (int x = 0; x < w; ++x) d[y * w + x] = std::sqrt(out[x]);
    }
    return d;
}

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

// The shadow's own move: resized and slanted about the anchor, then moved.
QTransform shadowMove(const SceneObject &o) {
    const QPointF a = ShapeEffects::anchor(o.rect, o.shadowAlign);
    QTransform t;
    t.translate(o.shadowX, o.shadowY);
    t.translate(a.x(), a.y());
    t.shear(std::tan(qDegreesToRadians(o.shadowSkewX)), std::tan(qDegreesToRadians(o.shadowSkewY)));
    t.scale(o.shadowScaleX / 100.0, o.shadowScaleY / 100.0);
    t.translate(-a.x(), -a.y());
    return t;
}

// How soft a PowerPoint blur radius is, as a Gaussian's standard deviation.
// (A blur radius is taken as reaching about 2.5 deviations: unmeasured.)
qreal sigmaOf(qreal blur) { return qMax<qreal>(0, blur) / 2.5; }

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
    return o.shadowInner || o.shadowBlur > 0 || !qFuzzyCompare(o.shadowScaleX, 100.0) || !qFuzzyCompare(o.shadowScaleY, 100.0) ||
           !qFuzzyIsNull(o.shadowSkewX) || !qFuzzyIsNull(o.shadowSkewY);
}

void ShapeEffects::paintBehind(QPainter &painter, const SceneObject &o, const QPainterPath &area, const QPainterPath &outline) {
    const bool glow = o.glowRadius > 0 && o.glowColor.alpha() > 0;
    const bool shadow = o.shadowEnabled && !o.shadowInner && o.shadowColor.alpha() > 0;
    if (!glow && !shadow) return;
    QPainterPath shape = area.united(outline);
    shape.setFillRule(Qt::WindingFill);
    if (shape.isEmpty()) return;
    const QRectF bounds = shape.boundingRect();
    const QTransform move = shadow ? shadowMove(o) : QTransform();
    const qreal sigma = shadow ? sigmaOf(o.shadowBlur) : 0;
    QRectF region = bounds.adjusted(-o.glowRadius * 1.2, -o.glowRadius * 1.2, o.glowRadius * 1.2, o.glowRadius * 1.2);
    if (shadow) region = region.united(move.map(shape).boundingRect().adjusted(-3 * sigma, -3 * sigma, 3 * sigma, 3 * sigma));
    region.adjust(-1, -1, 1, 1);
    const qreal scale = drawScale(painter, region.size());
    const int w = qMax(1, qCeil(region.width() * scale)), h = qMax(1, qCeil(region.height() * scale));
    const QString key = QStringList{QStringLiteral("behind"), signature(shape), QString::number(w), QString::number(h),
                                    QString::number(region.x()), QString::number(region.y()),
                                    QString::number(glow ? o.glowRadius : 0), o.glowColor.name(QColor::HexArgb),
                                    QString::number(shadow), o.shadowColor.name(QColor::HexArgb), QString::number(sigma),
                                    QString::number(move.m11()), QString::number(move.m12()), QString::number(move.m21()),
                                    QString::number(move.m22()), QString::number(move.dx()), QString::number(move.dy())}
                            .join(QLatin1Char('|'));
    QImage picture = cached(key);
    if (picture.isNull()) {
        const QVector<float> body = coverage(shape, QTransform(), region, scale, w, h);
        QVector<float> halo(w * h, 0.f), cast;
        if (glow) {
            // Half strength half-way out, all but gone at the glow's size
            // (unmeasured against PowerPoint for shapes; text glow is measured).
            QVector<bool> inside(w * h);
            for (int i = 0; i < w * h; ++i) inside[i] = body[i] >= 0.5f;
            const QVector<float> d = distanceTo(inside, w, h);
            const qreal at = o.glowRadius * 0.5 * scale, fade = qMax<qreal>(0.5, o.glowRadius * 0.2 * scale);
            const float strength = float(o.glowColor.alphaF());
            for (int i = 0; i < w * h; ++i)
                halo[i] = qMax(body[i], float(0.5 * std::erfc((d[i] - at) / (fade * M_SQRT2)))) * strength;
        }
        if (shadow) {
            cast = coverage(shape, move, region, scale, w, h);
            gaussian(cast, w, h, sigma * scale, 0.f);
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
    const qreal sigma = sigmaOf(o.shadowBlur);
    const QRectF region = area.boundingRect().adjusted(-1, -1, 1, 1);
    const qreal scale = drawScale(painter, region.size());
    const int w = qMax(1, qCeil(region.width() * scale)), h = qMax(1, qCeil(region.height() * scale));
    const QTransform move = QTransform::fromTranslate(o.shadowX, o.shadowY);
    const QString key = QStringList{QStringLiteral("inside"), signature(area), QString::number(w), QString::number(h),
                                    QString::number(region.x()), QString::number(region.y()), o.shadowColor.name(QColor::HexArgb),
                                    QString::number(sigma), QString::number(o.shadowX), QString::number(o.shadowY)}
                            .join(QLatin1Char('|'));
    QImage picture = cached(key);
    if (picture.isNull()) {
        const QVector<float> body = coverage(area, QTransform(), region, scale, w, h);
        // What lies outside the shape, moved in by the shadow's offset and
        // softened, seen through the shape.
        QVector<float> outside = coverage(area, move, region, scale, w, h);
        for (auto &v : outside) v = 1 - v;
        gaussian(outside, w, h, sigma * scale, 1.f);
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

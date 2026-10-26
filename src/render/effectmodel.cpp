#include "render/effectmodel.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace EffectModel {

namespace {
thread_local qreal tUnitsPerPoint = 2;   // a 1080-high slide
}

void setUnitsPerPoint(qreal units) { tUnitsPerPoint = units > 0 ? units : 2; }
qreal unitsPerPoint() { return tUnitsPerPoint; }

// Measured: a Gaussian of a third of the blur radius fits PowerPoint's shadow
// edges over blur 2-20 pt (RMS 0.0004 of full strength); a wider or a box
// kernel does not.
qreal sigma(qreal blur) { return qMax<qreal>(0, blur) / 3; }

// Measured on glows of 3-20 pt: the edge's half-strength line sits half the
// radius out up to 5 pt (2.5 pt), and 0.45 of it beyond, softened by a sixth
// of the radius throughout.
qreal glowGrow(qreal radius, qreal point) {
    const qreal r = qMax<qreal>(0, radius);
    return qMax(0.45 * r, qMin(0.5 * r, 2.5 * point));
}
qreal glowSigma(qreal radius) { return qMax<qreal>(0, radius) / 6; }

// Measured on words of 12-88 pt: one factor moves, blurs and grows their
// shadows and glows alike, 0.0495 x size^0.697 (1 at about 75 pt).
qreal textScale(qreal sizePoints) { return 0.0495 * std::pow(qMax<qreal>(1, sizePoints), 0.697); }
// Measured on words of 20-88 pt: their inner shadow's distance and blur are
// both 0.6 of the outer shadow's.
qreal textInnerScale(qreal sizePoints) { return 0.6 * textScale(sizePoints); }

QRectF onPixels(const QTransform &painter, const QRectF &region, qreal scale) {
    QRectF out = region;
    if (painter.type() <= QTransform::TxScale && painter.isInvertible()) {
        const QPointF d = painter.map(region.topLeft());
        out.setTopLeft(painter.inverted().map(QPointF(std::floor(d.x() + 1e-6), std::floor(d.y() + 1e-6))));
    }
    if (scale > 0) out.setSize(QSizeF(qMax(1, qCeil(out.width() * scale - 1e-6)) / scale, qMax(1, qCeil(out.height() * scale - 1e-6)) / scale));
    return out;
}

void gaussian(QVector<float> &plane, int w, int h, qreal sigmaPixels, float outside) {
    if (sigmaPixels < 0.3 || w <= 0 || h <= 0) return;
    const int r = qCeil(sigmaPixels * 4);
    QVector<float> k(2 * r + 1);
    float sum = 0;
    for (int i = -r; i <= r; ++i) sum += (k[i + r] = float(std::exp(-i * i / (2 * sigmaPixels * sigmaPixels))));
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

QVector<float> glow(const QVector<float> &silhouette, int w, int h, qreal growPixels, qreal sigmaPixels, float strength) {
    QVector<bool> inside(w * h);
    for (int i = 0; i < w * h; ++i) inside[i] = silhouette[i] >= 0.5f;
    const QVector<float> d = distanceTo(inside, w, h);
    // The outline lies half a pixel past the centres of the pixels inside it.
    QVector<float> grown(w * h);
    for (int i = 0; i < w * h; ++i)
        grown[i] = qMax(silhouette[i], qBound(0.f, float(growPixels + 1 - d[i]), 1.f));
    gaussian(grown, w, h, sigmaPixels, 0.f);
    for (auto &v : grown) v = qBound(0.f, v, 1.f) * strength;
    return grown;
}

} // namespace EffectModel

#include "render/layereffects.h"

#include "core/scene.h"
#include "render/fillpaint.h"
#include "render/shapeeffects.h"

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QPolygonF>
#include <QTransform>
#include <QtMath>

#include <cmath>

namespace LayerEffects {

namespace {
thread_local Pass tPass = Pass::All;

// Distance from every pixel to the nearest marked one, in pixels (exact
// Euclidean, Felzenszwalb & Huttenlocher), as render/textlayout.cpp.
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

void gaussian(QVector<float> &plane, int w, int h, qreal sigma) {
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
            for (int j = qMax(-r, -i); j <= qMin(r, count - 1 - i); ++j) v += line[i + j] * k[j + r];
            start[i * stride] = v;
        }
    };
    for (int y = 0; y < h; ++y) pass(w, 1, plane.data() + y * w);
    for (int x = 0; x < w; ++x) pass(h, w, plane.data() + x);
}

// Blurs a premultiplied picture, every channel alike.
void blur(QImage &image, qreal sigma) {
    if (sigma < 0.3) return;
    const int w = image.width(), h = image.height();
    QVector<float> planes[4];
    for (auto &p : planes) p.resize(w * h);
    for (int y = 0; y < h; ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            planes[0][y * w + x] = qRed(row[x]); planes[1][y * w + x] = qGreen(row[x]);
            planes[2][y * w + x] = qBlue(row[x]); planes[3][y * w + x] = qAlpha(row[x]);
        }
    }
    for (auto &p : planes) gaussian(p, w, h, sigma);
    for (int y = 0; y < h; ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int a = qBound(0, qRound(planes[3][y * w + x]), 255);
            auto c = [&](int i) { return qBound(0, qRound(planes[i][y * w + x]), a); };
            row[x] = qRgba(c(0), c(1), c(2), a);
        }
    }
}

// A bevel's height from the outline in (t 0) to its inner edge (t 1), 0-1.
// PROTOTYPE: drawn from PowerPoint's gallery by eye, not measured.
qreal profile(const QString &preset, qreal t) {
    t = qBound<qreal>(0, t, 1);
    if (preset == QLatin1String("relaxedInset")) return 0.5 - 0.5 * std::cos(M_PI * t);
    if (preset == QLatin1String("cross")) return t < 0.5 ? std::sqrt(1 - (1 - 2 * t) * (1 - 2 * t)) : 1 - 0.5 * (t - 0.5);
    if (preset == QLatin1String("coolSlant")) return std::pow(t, 0.6);
    if (preset == QLatin1String("angle")) return t;
    if (preset == QLatin1String("softRound")) return t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
    if (preset == QLatin1String("convex")) return 1 - std::pow(1 - t, 3);
    if (preset == QLatin1String("slope")) return qMin<qreal>(1, t * 1.25);
    if (preset == QLatin1String("divot")) return 1 - 0.7 * std::sin(M_PI * t);
    if (preset == QLatin1String("riblet")) return t + 0.18 * std::sin(4 * M_PI * t);
    if (preset == QLatin1String("hardEdge")) return t < 0.2 ? t / 0.2 : 1;
    if (preset == QLatin1String("artDeco")) { const qreal s = t * 3; return (s - std::sin(2 * M_PI * s) / (2 * M_PI)) / 3; }
    return std::sqrt(qMax<qreal>(0, 1 - (1 - t) * (1 - t)));   // circle
}

struct V3 { qreal x, y, z; };
// The camera's turn as [MS-OI29500] composes it: about y by lon, then x by
// lat, then z by rev (y axis down, z toward the viewer).
struct Turn {
    qreal m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Turn() = default;
    Turn(qreal lat, qreal lon, qreal rev) {
        const qreal a = qDegreesToRadians(lat), b = qDegreesToRadians(lon), c = qDegreesToRadians(rev);
        const qreal X[3][3] = {{1, 0, 0}, {0, std::cos(a), std::sin(a)}, {0, -std::sin(a), std::cos(a)}};
        const qreal Y[3][3] = {{std::cos(b), 0, -std::sin(b)}, {0, 1, 0}, {std::sin(b), 0, std::cos(b)}};
        const qreal Z[3][3] = {{std::cos(c), std::sin(c), 0}, {-std::sin(c), std::cos(c), 0}, {0, 0, 1}};
        qreal XY[3][3] = {};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) XY[i][j] += X[i][k] * Y[k][j];
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { m[i][j] = 0; for (int k = 0; k < 3; ++k) m[i][j] += Z[i][k] * XY[k][j]; }
    }
    V3 operator()(const V3 &p) const {
        return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z, m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z,
                m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z};
    }
};

V3 normalised(const V3 &v) {
    const qreal l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 0 ? V3{v.x / l, v.y / l, v.z / l} : V3{0, 0, 1};
}
qreal dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// The light rig's lighting of a surface facing `n`, against a flat face
// toward the viewer (so an unturned, unbevelled face keeps its colour).
struct Lighting {
    Effects3D::Rig rig;
    Effects3D::Material material;
    qreal flat = 1, flatShine = 0;
    explicit Lighting(const Effects3D::Scene &s, const QString &materialName)
        : rig(Effects3D::rig(s)), material(Effects3D::material(materialName)) {
        flat = qMax<qreal>(1e-3, diffuse({0, 0, 1}));
        flatShine = shine({0, 0, 1});
    }
    qreal diffuse(const V3 &n) const {
        qreal sum = rig.ambient;
        for (const auto &l : rig.lights) {
            if (!l.diffuse) continue;
            const V3 to = normalised({-l.x, -l.y, -l.z});
            sum += (l.r + l.g + l.b) / 3 * qMax<qreal>(0, dot(n, to));
        }
        return sum;
    }
    qreal shine(const V3 &n) const {
        if (material.specular <= 0) return 0;
        qreal sum = 0;
        for (const auto &l : rig.lights) {
            if (!l.specular) continue;
            const V3 to = normalised({-l.x, -l.y, -l.z});
            const V3 half = normalised({to.x, to.y, to.z + 1});
            sum += (l.r + l.g + l.b) / 3 * std::pow(qMax<qreal>(0, dot(n, half)), material.power * 2);
        }
        return sum * material.specular * 0.6;
    }
    qreal shade(const V3 &n) const {
        const qreal relative = diffuse(n) / flat;
        return qBound<qreal>(0, 1 + (relative - 1) * material.diffuse, 2.5);
    }
};

struct Made { QImage image; QRectF rect; };

QCache<QString, Made> &cache() { static QCache<QString, Made> c(96 * 1024); return c; }   // KiB
QMutex &cacheLock() { static QMutex m; return m; }

Made make(const QRectF &box, const Look &look, qreal scale, qreal margin,
          const std::function<void(QPainter &)> &face) {
    const QSizeF full(box.width() + 2 * margin, box.height() + 2 * margin);
    const QSize px(qMax(1, qCeil(full.width() * scale)), qMax(1, qCeil(full.height() * scale)));
    QImage layer(px, QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    {
        QPainter p(&layer);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.scale(scale, scale);
        p.translate(margin - box.left(), margin - box.top());
        PassScope only(Pass::Face);
        face(p);
    }
    const int w = px.width(), h = px.height();
    QVector<bool> inside(w * h), outside(w * h);
    for (int y = 0; y < h; ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
        for (int x = 0; x < w; ++x) { inside[y * w + x] = qAlpha(row[x]) >= 128; outside[y * w + x] = !inside[y * w + x]; }
    }
    const bool bevelled = look.format.on && look.format.top.on() && look.format.top.w * scale > 0.5;
    const bool turned = Effects3D::drawn(look.scene);
    const bool deep = look.format.on && look.format.depth * scale >= 0.5;
    const qreal soft = look.softEdge * scale;
    QVector<float> in;   // distance from inside pixels to the outline
    if (soft > 0.5 || bevelled || deep) in = distanceTo(outside, w, h);

    // Soft edges: clear at the outline, solid the edge's size in.
    if (soft > 0.5) {
        const qreal half = soft / 2, sigma = qMax<qreal>(0.5, soft / 4);
        for (int y = 0; y < h; ++y) {
            auto *row = reinterpret_cast<QRgb *>(layer.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const qreal d = in[y * w + x] - 0.5;
                const qreal f = 0.5 * std::erfc((half - d) / (sigma * M_SQRT2));
                const QRgb c = row[x];
                row[x] = qRgba(qRound(qRed(c) * f), qRound(qGreen(c) * f), qRound(qBlue(c) * f), qRound(qAlpha(c) * f));
            }
        }
    }

    qreal lat = 0, lon = 0, rev = 0;
    if (turned) Effects3D::rotation(look.scene, &lat, &lon, &rev);
    const Effects3D::Camera cam = Effects3D::camera(look.scene.camera);
    const bool oblique = turned && cam.skew > 0;
    const Turn turn = oblique ? Turn() : Turn(lat, lon, rev);
    const Lighting light(look.scene.on ? look.scene : Effects3D::Scene(), look.format.material);

    // Bevel (and a turned face's light): the face shaded by its slope.
    if (bevelled || (turned && !oblique)) {
        const qreal bw = bevelled ? look.format.top.w * scale : 0, bh = bevelled ? look.format.top.h * scale : 0;
        QVector<float> height(w * h, 0.f);
        if (bevelled)
            for (int i = 0; i < w * h; ++i)
                height[i] = inside[i] ? float(bh * profile(look.format.top.preset, (in[i] - 0.5) / bw)) : 0.f;
        const bool metal = light.material.metal;
        for (int y = 0; y < h; ++y) {
            auto *row = reinterpret_cast<QRgb *>(layer.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb c = row[x];
                if (!qAlpha(c)) continue;
                qreal gx = 0, gy = 0;
                if (bevelled) {
                    const int x0 = qMax(0, x - 1), x1 = qMin(w - 1, x + 1), y0 = qMax(0, y - 1), y1 = qMin(h - 1, y + 1);
                    gx = (height[y * w + x1] - height[y * w + x0]) / qMax(1, x1 - x0);
                    gy = (height[y1 * w + x] - height[y0 * w + x]) / qMax(1, y1 - y0);
                }
                const V3 n = turn(normalised({-gx, -gy, 1}));
                const qreal s = light.shade(n);
                const qreal shine = qMax<qreal>(0, light.shine(n) - light.flatShine);
                const qreal a = qAlpha(c);
                auto channel = [&](int v) {
                    // Shine is white on most materials, the colour itself on metals.
                    const qreal lit = v * s + (metal ? shine * v * 1.5 : shine * a);
                    return qBound(0, qRound(lit), int(a));
                };
                row[x] = qRgba(channel(qRed(c)), channel(qGreen(c)), channel(qBlue(c)), int(a));
            }
        }
    }

    // Contour: a band round the outline, outside it.
    const qreal contour = look.format.on ? look.format.contour * scale : 0;
    if (contour >= 0.5) {
        const QVector<float> out = distanceTo(inside, w, h);
        const QColor cc = look.format.contourColor.isValid() ? look.format.contourColor : look.line.isValid() ? look.line : QColor(Qt::white);
        QImage band(w, h, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h; ++y) {
            auto *row = reinterpret_cast<QRgb *>(band.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const qreal a = qBound<qreal>(0, contour - out[y * w + x] + 0.5, 1) * cc.alphaF();
                row[x] = qRgba(qRound(cc.red() * a), qRound(cc.green() * a), qRound(cc.blue() * a), qRound(a * 255));
            }
        }
        QPainter p(&band);
        p.drawImage(0, 0, layer);
        p.end();
        layer = band;
        for (int y = 0; y < h; ++y) {
            const auto *row = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
            for (int x = 0; x < w; ++x) { inside[y * w + x] = qAlpha(row[x]) >= 128; outside[y * w + x] = !inside[y * w + x]; }
        }
        if (deep) in = distanceTo(outside, w, h);
    }

    // The box in layer pixels; everything after works about its middle.
    const QRectF boxPx(margin * scale, margin * scale, box.width() * scale, box.height() * scale);
    const QPointF middle = boxPx.center();
    QImage result = layer;
    QPointF resultAt(0, 0);   // where `result` sits in layer pixels
    QRectF faceBox = boxPx;   // the box as drawn (turned), for the reflection

    if (turned || deep) {
        const qreal depth = deep ? look.format.depth * scale : 0;
        const qreal front = look.format.on ? look.format.z * scale : 0;
        const bool persp = turned && !cam.parallel && !look.scene.camera.startsWith(QLatin1String("legacy"));
        const qreal fov = persp ? Effects3D::fieldOfView(look.scene) : 0;
        // PowerPoint's viewing scale: 159.76 mm at half the field of view.
        const qreal distance = fov > 0 ? 452.9 * FillPaint::unitsPerPoint() * scale / std::tan(qDegreesToRadians(fov / 2)) : 0;
        const qreal zoom = look.scene.on ? qMax<qreal>(0.01, look.scene.zoom / 100) : 1;
        const qreal skew = oblique ? cam.skew / 100 : 0;
        const qreal angle = qDegreesToRadians(cam.skewAngle);
        auto project = [&](const QPointF &p, qreal z) {
            V3 q = turn({p.x() - middle.x(), p.y() - middle.y(), z});
            qreal f = 1;
            if (distance > 0) f = distance / qMax(distance * 0.05, distance - q.z);
            QPointF s(q.x * f, q.y * f);
            if (oblique) s += QPointF(std::cos(angle), -std::sin(angle)) * (-z * skew);
            return middle + s * zoom;
        };
        const QRectF src(0, 0, w, h);
        auto quadAt = [&](qreal z) {
            return QPolygonF{project(src.topLeft(), z), project(src.topRight(), z), project(src.bottomRight(), z), project(src.bottomLeft(), z)};
        };
        const QPolygonF frontQuad = quadAt(front);
        const QPolygonF backQuad = quadAt(front - depth);
        QRectF bounds = frontQuad.boundingRect().united(backQuad.boundingRect()).adjusted(-2, -2, 2, 2);
        const qreal most = 6000;
        bounds = bounds.intersected(QRectF(middle.x() - most, middle.y() - most, 2 * most, 2 * most));
        QImage out(qMax(1, qCeil(bounds.width())), qMax(1, qCeil(bounds.height())), QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setRenderHint(QPainter::Antialiasing);
        const QPolygonF srcQuad{src.topLeft(), src.topRight(), src.bottomRight(), src.bottomLeft()};
        auto drawAt = [&](const QImage &image, const QPolygonF &quad) {
            QTransform t;
            QPolygonF moved = quad.translated(-bounds.topLeft());
            if (!QTransform::quadToQuad(srcQuad, moved, t)) return;
            p.setTransform(t);
            p.drawImage(QPointF(), image);
        };
        if (depth > 0) {
            // The sides: the silhouette in the depth's colour, each pixel lit
            // as the side nearest it faces.
            const QColor ec = look.format.extrusionColor.isValid() ? look.format.extrusionColor : look.face;
            QImage sides(w, h, QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < h; ++y) {
                const auto *src = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
                auto *row = reinterpret_cast<QRgb *>(sides.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    const int a = qAlpha(src[x]);
                    if (!a) { row[x] = 0; continue; }
                    const int x0 = qMax(0, x - 1), x1 = qMin(w - 1, x + 1), y0 = qMax(0, y - 1), y1 = qMin(h - 1, y + 1);
                    const qreal gx = (in[y * w + x1] - in[y * w + x0]) / qMax(1, x1 - x0);
                    const qreal gy = (in[y1 * w + x] - in[y0 * w + x]) / qMax(1, y1 - y0);
                    const V3 n = turn(normalised({-gx, -gy, 0.0001}));
                    const qreal s = qBound<qreal>(0.15, light.diffuse(n) / light.flat, 1.4) * 0.85;
                    const qreal f = a / 255.0 * ec.alphaF();
                    row[x] = qRgba(qBound(0, qRound(ec.red() * s * f), 255), qBound(0, qRound(ec.green() * s * f), 255),
                                   qBound(0, qRound(ec.blue() * s * f), 255), qRound(f * 255));
                }
            }
            const QPointF a = project(middle, front), b = project(middle, front - depth);
            const int slices = qBound(1, qCeil(QLineF(a, b).length() * 1.5) + 1, 160);
            for (int k = slices; k >= 1; --k) drawAt(sides, quadAt(front - depth * k / slices));
        }
        drawAt(layer, frontQuad);
        p.end();
        result = out;
        resultAt = bounds.topLeft();
        faceBox = QPolygonF{project(boxPx.topLeft(), front), project(boxPx.topRight(), front), project(boxPx.bottomRight(), front),
                            project(boxPx.bottomLeft(), front)}.boundingRect();
    }

    // Reflection: turned over about its point of the box, moved, faded, blurred.
    if (Effects3D::drawn(look.reflection)) {
        const auto &r = look.reflection;
        const QPointF anchor = ShapeEffects::anchor(faceBox, r.align);
        QTransform t;
        const qreal dist = r.dist * scale;
        t.translate(dist * std::cos(qDegreesToRadians(r.dir)), dist * std::sin(qDegreesToRadians(r.dir)));
        t.translate(anchor.x(), anchor.y());
        t.shear(std::tan(qDegreesToRadians(qBound(-89.0, r.kx, 89.0))), std::tan(qDegreesToRadians(qBound(-89.0, r.ky, 89.0))));
        t.scale(r.sx / 100, r.sy / 100);
        t.translate(-anchor.x(), -anchor.y());
        const QRectF resultRect(resultAt, result.size());
        const qreal sigma = r.blur * scale / 2;
        const QRectF mirrored = t.mapRect(resultRect).adjusted(-3 * sigma - 1, -3 * sigma - 1, 3 * sigma + 1, 3 * sigma + 1);
        QRectF bounds = mirrored.united(resultRect);
        const qreal most = 8000;
        bounds = bounds.intersected(QRectF(middle.x() - most, middle.y() - most, 2 * most, 2 * most));
        QImage turnedOver(qMax(1, qCeil(bounds.width())), qMax(1, qCeil(bounds.height())), QImage::Format_ARGB32_Premultiplied);
        turnedOver.fill(Qt::transparent);
        {
            QPainter p(&turnedOver);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.translate(-bounds.topLeft());
            p.setTransform(t, true);
            p.drawImage(resultAt, result);
        }
        // Fade along fadeDir across the reflected box.
        const QRectF rb = t.mapRect(faceBox).translated(-bounds.topLeft());
        const QPointF dir(std::cos(qDegreesToRadians(r.fadeDir)), std::sin(qDegreesToRadians(r.fadeDir)));
        const QPointF corners[4] = {rb.topLeft(), rb.topRight(), rb.bottomRight(), rb.bottomLeft()};
        qreal lo = 1e18, hi = -1e18;
        for (const auto &c : corners) { const qreal v = c.x() * dir.x() + c.y() * dir.y(); lo = qMin(lo, v); hi = qMax(hi, v); }
        const qreal extent = qMax<qreal>(1e-6, hi - lo);
        const qreal sa = r.startAlpha / 100, ea = r.endAlpha / 100, sp = r.startPos / 100, ep = r.endPos / 100;
        for (int y = 0; y < turnedOver.height(); ++y) {
            auto *row = reinterpret_cast<QRgb *>(turnedOver.scanLine(y));
            for (int x = 0; x < turnedOver.width(); ++x) {
                if (!qAlpha(row[x])) continue;
                const qreal at = ((x + .5) * dir.x() + (y + .5) * dir.y() - lo) / extent;
                const qreal f = at <= sp ? sa : at >= ep ? ea : sa + (ea - sa) * (at - sp) / qMax<qreal>(1e-6, ep - sp);
                const QRgb c = row[x];
                row[x] = qRgba(qRound(qRed(c) * f), qRound(qGreen(c) * f), qRound(qBlue(c) * f), qRound(qAlpha(c) * f));
            }
        }
        blur(turnedOver, sigma);
        {
            QPainter p(&turnedOver);
            p.drawImage(resultAt - bounds.topLeft(), result);
        }
        result = turnedOver;
        resultAt = bounds.topLeft();
    }

    const QPointF origin(box.left() - margin, box.top() - margin);
    return {result, QRectF(origin + resultAt / scale, QSizeF(result.width() / scale, result.height() / scale))};
}
} // namespace

Pass pass() { return tPass; }
PassScope::PassScope(Pass p) : was(tPass) { tPass = p; }
PassScope::~PassScope() { tPass = was; }

bool needed(const Look &look) {
    return look.softEdge > 0 || Effects3D::drawn(look.reflection) || Effects3D::drawn(look.format) || Effects3D::drawn(look.scene);
}

void paint(QPainter &painter, const QRectF &box, const Look &look, const QString &key,
           const std::function<void(QPainter &)> &face, const std::function<void(QPainter &)> &behind) {
    if (behind) {
        PassScope only(Pass::Behind);
        behind(painter);
    }
    const QTransform &t = painter.worldTransform();
    qreal scale = qMax(std::hypot(t.m11(), t.m12()), std::hypot(t.m21(), t.m22()));
    const qreal unit = FillPaint::unitsPerPoint();
    qreal margin = qMax(box.width(), box.height()) * 0.12 + 8 * unit;
    if (look.format.on) margin += look.format.contour + look.format.top.w * 0.2;
    const QSizeF full(box.width() + 2 * margin, box.height() + 2 * margin);
    scale = qBound(0.02, scale, qMin(4.0 * qMax(1.0, scale), 2200.0 / qMax(1.0, qMax(full.width(), full.height()))));
    const QString name = key + QLatin1Char('|') + QString::number(scale, 'g', 6) + QLatin1Char('|') + QString::number(unit, 'g', 6);
    Made made;
    bool have = false;
    {
        QMutexLocker locked(&cacheLock());
        if (const Made *kept = cache().object(name)) { made = *kept; have = true; }
    }
    if (!have) {
        made = make(box, look, scale, margin, face);
        QMutexLocker locked(&cacheLock());
        cache().insert(name, new Made(made), qMax(1, int(made.image.sizeInBytes() / 1024)));
    }
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(made.rect, made.image);
    painter.restore();
}

Look shapeLook(const SceneObject &o) {
    Look l;
    l.softEdge = o.softEdge;
    l.reflection = o.reflection;
    l.format = o.format3d;
    l.scene = o.scene3d;
    l.face = o.fillStyle == 5 ? o.strokeColor : o.fill;
    if (o.type == ObjectType::Image) l.face = QColor(128, 128, 128);
    if (o.strokeWidth > 0) l.line = o.strokeColor;
    return l;
}

Look textLook(const SceneObject &o) {
    Look l;
    l.reflection = o.textReflection;
    if (!l.reflection.on)
        for (const auto &run : o.runs) {
            Effects3D::Reflection r;
            if (Effects3D::fromMap(run.effects.value(QStringLiteral("textReflection")), &r) && r.on) { l.reflection = r; break; }
        }
    l.format = o.textFormat3d;
    l.scene = o.textScene3d;
    l.face = o.textColor;
    return l;
}

} // namespace LayerEffects

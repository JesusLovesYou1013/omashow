#include "render/fillpaint.h"

#include "core/fillstyle.h"
#include "core/imageasset.h"
#include "core/scene.h"

#include <QCache>
#include <QCryptographicHash>
#include <QImage>
#include <QLinearGradient>
#include <QMutex>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRadialGradient>
#include <QtMath>

#include <cmath>

namespace {

thread_local qreal tUnitsPerPoint = 2;   // a 1080-high slide

struct Spec {
    QVector<GradientStop> stops;   // sorted
    int kind = 0;                  // 0 linear, 1 circle, 2 rectangle, 3 shape
    qreal angle = 0;
    bool scaled = false;
    bool rotate = true;
    qreal l = .5, t = .5, r = .5, b = .5;   // the focus box, as a:fillToRect
};

Spec fillSpec(const SceneObject &o) {
    Spec s;
    s.stops = FillStyle::fillStops(o);
    s.kind = o.fillStyle == 1 ? 0 : 1 + qBound(0, o.gradientPath, 2);
    s.angle = o.fillAngle;
    s.scaled = o.gradientScaled;
    s.rotate = o.gradientRotate;
    s.l = o.gradientFocusL; s.t = o.gradientFocusT; s.r = o.gradientFocusR; s.b = o.gradientFocusB;
    return s;
}

Spec lineSpec(const SceneObject &o) {
    Spec s;
    s.stops = FillStyle::lineStops(o);
    s.kind = qBound(0, o.strokeGradientPath, 3);
    s.angle = o.strokeAngle;
    return s;
}

// Turns brush space back against the shape's rotation (rotate with shape off).
QTransform unrotate(const SceneObject &o, const QPointF &pivot) {
    QTransform t;
    if (qFuzzyIsNull(o.rotation)) return t;
    t.translate(pivot.x(), pivot.y());
    t.rotate(-o.rotation);
    t.translate(-pivot.x(), -pivot.y());
    return t;
}

QRectF focusBox(const Spec &s, const QRectF &box) {
    const qreal w = box.width(), h = box.height();
    qreal l = box.left() + s.l * w, r = box.right() - s.r * w, t = box.top() + s.t * h, b = box.bottom() - s.b * h;
    if (l > r) l = r = (l + r) / 2;
    if (t > b) t = b = (t + b) / 2;
    return QRectF(QPointF(l, t), QPointF(r, b));
}

QCache<QString, QImage> &textures() { static QCache<QString, QImage> cache(48 * 1024); return cache; }   // in KiB
QMutex &texturesLock() { static QMutex m; return m; }

QString stopsKey(const QVector<GradientStop> &stops) {
    QString k;
    for (const auto &s : stops) k += QStringLiteral("%1:%2:%3;").arg(s.position).arg(s.color.rgba()).arg(s.brightness);
    return k;
}

// Rectangle and shape gradients have no Qt brush of their own: a picture of
// the gradient over the box, at about the size it is drawn.
QImage gradientTexture(const Spec &s, const QRectF &box, const QSize &px, const QPainterPath &shape) {
    QString key = QStringLiteral("%1|%2x%3|%4,%5,%6,%7|%8|").arg(s.kind).arg(px.width()).arg(px.height())
                      .arg(s.l).arg(s.t).arg(s.r).arg(s.b).arg(stopsKey(s.stops));
    if (s.kind == 3) {
        QByteArray bytes;
        for (int i = 0; i < shape.elementCount(); ++i) {
            const auto e = shape.elementAt(i);
            bytes += QByteArray::number((e.x - box.left()) / qMax(1e-9, box.width()), 'g', 5) + ',' +
                     QByteArray::number((e.y - box.top()) / qMax(1e-9, box.height()), 'g', 5) + ';';
        }
        key += QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex());
    }
    {
        QMutexLocker lock(&texturesLock());
        if (const QImage *hit = textures().object(key)) return *hit;
    }
    QImage image(px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const qreal sx = px.width() / qMax(1e-9, box.width()), sy = px.height() / qMax(1e-9, box.height());
    const QRectF focus = focusBox(s, box);
    if (s.kind == 2) {
        // How far out from the focus box, side by side: 0 on it, 1 at the box.
        const qreal fl = (focus.left() - box.left()) * sx, fr = (focus.right() - box.left()) * sx;
        const qreal ft = (focus.top() - box.top()) * sy, fb = (focus.bottom() - box.top()) * sy;
        const qreal W = px.width(), H = px.height();
        for (int y = 0; y < px.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            const qreal cy = y + .5;
            const qreal ty = cy < ft ? (ft - cy) / qMax(1e-9, ft) : cy > fb ? (cy - fb) / qMax(1e-9, H - fb) : 0;
            for (int x = 0; x < px.width(); ++x) {
                const qreal cx = x + .5;
                const qreal tx = cx < fl ? (fl - cx) / qMax(1e-9, fl) : cx > fr ? (cx - fr) / qMax(1e-9, W - fr) : 0;
                const QColor c = FillStyle::colorAt(s.stops, qBound(0.0, qMax(tx, ty), 1.0));
                line[x] = qPremultiply(c.rgba());
            }
        }
    } else {
        // The outline drawn smaller and smaller towards the focus, each its colour.
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.scale(sx, sy);
        p.translate(-box.topLeft());
        const QPointF c = focus.center();
        const int steps = qBound(16, int(qMax(px.width(), px.height()) / 3), 160);
        // The outer step covers the whole box, so nothing is left unfilled.
        p.fillRect(box, FillStyle::colorAt(s.stops, 1));
        for (int i = steps; i >= 0; --i) {
            const qreal u = qreal(i) / steps;
            const qreal fx = focus.width() / qMax(1e-9, box.width()), fy = focus.height() / qMax(1e-9, box.height());
            QTransform t;
            t.translate(c.x(), c.y());
            t.scale(fx + (1 - fx) * u, fy + (1 - fy) * u);
            t.translate(-c.x(), -c.y());
            p.setBrush(FillStyle::colorAt(s.stops, u));
            p.drawPath(t.map(shape));
        }
    }
    QMutexLocker lock(&texturesLock());
    textures().insert(key, new QImage(image), qMax<qsizetype>(1, image.sizeInBytes() / 1024));
    return image;
}

QBrush gradientBrush(QPainter &painter, const Spec &s, const QRectF &box, const QPainterPath &shape, const SceneObject &o) {
    const QTransform keep = s.rotate ? QTransform() : unrotate(o, o.rect.center());
    const qreal w = qMax(1e-6, box.width()), h = qMax(1e-6, box.height());
    if (s.kind == 0) {
        const qreal a = qDegreesToRadians(s.angle);
        const QPointF d(std::cos(a), std::sin(a));
        QLinearGradient g;
        QTransform t;
        if (s.scaled) {
            const qreal L = (std::abs(d.x()) + std::abs(d.y())) / 2;
            g.setStart(QPointF(.5, .5) - d * L);
            g.setFinalStop(QPointF(.5, .5) + d * L);
            t.translate(box.left(), box.top());
            t.scale(w, h);
        } else {
            const QPointF c = box.center();
            const qreal L = std::abs(w / 2 * d.x()) + std::abs(h / 2 * d.y());
            g.setStart(c - d * L);
            g.setFinalStop(c + d * L);
        }
        for (const auto &stop : s.stops) g.setColorAt(qBound(0.0, stop.position, 1.0), FillStyle::stopColor(stop));
        QBrush brush(g);
        brush.setTransform(t * keep);
        return brush;
    }
    const QRectF focus = focusBox(s, box);
    if (s.kind == 1) {
        const QPointF c = focus.center();
        qreal radius = 0;
        for (const auto &corner : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()})
            radius = qMax(radius, std::hypot(corner.x() - c.x(), corner.y() - c.y()));
        radius = qMax(radius, 1e-6);
        // A focus box, not a point: the first stop covers it.
        const qreal start = qMin(0.95, std::hypot(focus.width(), focus.height()) / 2 / radius);
        QRadialGradient g(c, radius);
        for (const auto &stop : s.stops) g.setColorAt(qBound(0.0, start + (1 - start) * stop.position, 1.0), FillStyle::stopColor(stop));
        if (start > 0) g.setColorAt(0, FillStyle::stopColor(s.stops.first()));
        QBrush brush(g);
        brush.setTransform(keep);
        return brush;
    }
    // Rectangle and shape: a picture of the gradient at the size it is drawn.
    const QTransform device = painter.combinedTransform();
    const qreal scale = std::sqrt(std::abs(device.determinant()));
    const QSize px(qBound(8, int(std::ceil(w * scale)), 1024), qBound(8, int(std::ceil(h * scale)), 1024));
    const QImage image = gradientTexture(s, box, px, shape);
    QBrush brush(image);
    QTransform t;
    t.translate(box.left(), box.top());
    t.scale(w / px.width(), h / px.height());
    brush.setTransform(t * keep);
    return brush;
}

QBrush patternBrush(const SceneObject &o) {
    const QByteArray cells = FillStyle::patternCells(o.patternPreset);
    QImage tile(8, 8, QImage::Format_ARGB32_Premultiplied);
    const QColor fg = o.fill, bg = o.fillSecondary;
    const QColor mid = QColor::fromRgbF((fg.redF() + bg.redF()) / 2, (fg.greenF() + bg.greenF()) / 2, (fg.blueF() + bg.blueF()) / 2,
                                        (fg.alphaF() + bg.alphaF()) / 2);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const char c = cells.size() == 64 ? cells.at(y * 8 + x) : '#';
            tile.setPixel(x, y, qPremultiply((c == '#' ? fg : c == '+' ? mid : bg).rgba()));
        }
    QBrush brush(tile);
    // A cell is a point, on the slide's own grid (brush space is the slide).
    QTransform t;
    t.scale(tUnitsPerPoint, tUnitsPerPoint);
    brush.setTransform(t * unrotate(o, o.rect.center()));
    return brush;
}

void paintPicture(QPainter &painter, const SceneObject &o, const QPainterPath &area) {
    painter.save();
    if (o.pictureTransparency > 0) painter.setOpacity(painter.opacity() * qBound(0.0, 1 - o.pictureTransparency, 1.0));
    const bool raster = o.imageFormat != QLatin1String("svg") && !o.image.isNull();
    const bool placed = o.pictureTile || (o.imageMode == 2 && (!qFuzzyIsNull(o.pictureInsetL) || !qFuzzyIsNull(o.pictureInsetT) ||
                                                               !qFuzzyIsNull(o.pictureInsetR) || !qFuzzyIsNull(o.pictureInsetB)));
    if (!raster || !placed) {
        painter.setClipPath(area, Qt::IntersectClip);
        ImageAsset::paint(painter, o);
        painter.restore();
        return;
    }
    QImage image = ImageAsset::displayImage(o);
    const QRectF source = ImageAsset::sourceRect(o);
    if (!source.isEmpty() && source.toAlignedRect() != image.rect()) image = image.copy(source.toAlignedRect());
    if (image.isNull()) { painter.restore(); return; }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRectF box = o.rect;
    if (o.pictureTile) {
        const int flip = qBound(0, o.pictureTileFlip, 3);
        if (flip) {
            QImage doubled(image.width() * ((flip & 1) ? 2 : 1), image.height() * ((flip & 2) ? 2 : 1), QImage::Format_ARGB32_Premultiplied);
            doubled.fill(Qt::transparent);
            QPainter p(&doubled);
            p.drawImage(0, 0, image);
            if (flip & 1) p.drawImage(image.width(), 0, image.flipped(Qt::Horizontal));
            if (flip & 2) p.drawImage(0, image.height(), image.flipped(Qt::Vertical));
            if (flip == 3) p.drawImage(image.width(), image.height(), image.flipped(Qt::Horizontal | Qt::Vertical));
            p.end();
            image = doubled;
        }
        const qreal dpiX = image.dotsPerMeterX() > 0 ? image.dotsPerMeterX() * 0.0254 : 96;
        const qreal dpiY = image.dotsPerMeterY() > 0 ? image.dotsPerMeterY() * 0.0254 : 96;
        const qreal tileW = qMax(0.5, image.width() / dpiX * 72 * tUnitsPerPoint * o.pictureTileScaleX / 100);
        const qreal tileH = qMax(0.5, image.height() / dpiY * 72 * tUnitsPerPoint * o.pictureTileScaleY / 100);
        const int al = qBound(0, o.pictureTileAlign, 8);
        const qreal fx = (al % 3) / 2.0, fy = (al / 3) / 2.0;
        // The tile's own point that matches the box's aligned point.
        const QPointF origin(box.left() + box.width() * fx - tileW * fx + o.pictureTileX,
                             box.top() + box.height() * fy - tileH * fy + o.pictureTileY);
        QBrush brush(image);
        QTransform t;
        t.translate(origin.x(), origin.y());
        t.scale(tileW / image.width(), tileH / image.height());
        brush.setTransform(t * (o.pictureRotate ? QTransform() : unrotate(o, o.rect.center())));
        painter.setPen(Qt::NoPen);
        painter.fillPath(area, brush);
    } else {
        // Stretched into the box less its insets (a:fillRect).
        const QRectF dest = box.adjusted(o.pictureInsetL * box.width(), o.pictureInsetT * box.height(),
                                         -o.pictureInsetR * box.width(), -o.pictureInsetB * box.height());
        if (dest.width() > 0 && dest.height() > 0) {
            QBrush brush(image);
            QTransform t;
            t.translate(dest.left(), dest.top());
            t.scale(dest.width() / image.width(), dest.height() / image.height());
            brush.setTransform(t);
            QPainterPath clip;
            clip.addRect(dest);
            painter.fillPath(area.intersected(clip), brush);
        }
    }
    painter.restore();
}

QPainterPath stroked(const SceneObject &o, qreal width, const QVector<qreal> &dashes, const QPainterPath &path) {
    QPainterPathStroker s;
    s.setWidth(qMax(0.01, width));
    s.setCapStyle(Qt::PenCapStyle(qBound(0, o.strokeCap, 2) * 16));
    s.setJoinStyle(Qt::PenJoinStyle(qBound(0, o.strokeJoin, 2) * 64));
    s.setMiterLimit(8);
    if (!dashes.isEmpty()) {
        // Dash lengths are in line widths of the whole line.
        QVector<qreal> scaled;
        for (const qreal d : dashes) scaled.append(d * o.strokeWidth / qMax(0.01, width));
        s.setDashPattern(scaled);
    }
    return s.createStroke(path);
}

bool closedPath(const QPainterPath &p) {
    if (p.elementCount() < 3) return false;
    const auto first = p.elementAt(0), last = p.elementAt(p.elementCount() - 1);
    return std::hypot(first.x - last.x, first.y - last.y) < 1e-3 || p.fillRule() == Qt::WindingFill;
}

// Everything within signed distance d of the outline: d > 0 outwards.
QPainterPath within(const SceneObject &o, const QPainterPath &path, qreal d) {
    QPainterPath inside = path;
    inside.closeSubpath();
    if (qFuzzyIsNull(d)) return inside;
    const QPainterPath band = stroked(o, std::abs(d) * 2, {}, path);
    return d > 0 ? inside.united(band) : inside.subtracted(band);
}

} // namespace

void FillPaint::setUnitsPerPoint(qreal units) { if (units > 0 && std::isfinite(units)) tUnitsPerPoint = units; }
qreal FillPaint::unitsPerPoint() { return tUnitsPerPoint; }

void FillPaint::paintFill(QPainter &painter, const SceneObject &o, const QPainterPath &area) {
    painter.save();
    painter.setPen(Qt::NoPen);
    switch (o.fillStyle) {
    case 0: painter.fillPath(area, o.fill); break;
    case 1: case 2: painter.fillPath(area, gradientBrush(painter, fillSpec(o), o.rect, area, o)); break;
    case 3:
        if (o.patternPreset.isEmpty()) {
            painter.fillPath(area, o.fillSecondary);
            painter.fillPath(area, QBrush(o.fill, Qt::BrushStyle(Qt::Dense1Pattern + o.patternStyle)));
        } else {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.fillPath(area, patternBrush(o));
        }
        break;
    case 4: paintPicture(painter, o, area); break;
    default: break;
    }
    painter.restore();
}

QPainterPath FillPaint::lineArea(const SceneObject &o, const QPainterPath &path) {
    const qreal W = o.strokeWidth;
    if (W <= 0) return {};
    QVector<qreal> dashes = FillStyle::dashPattern(o);
    // PowerPoint counts round and square caps inside a preset dash's length
    // (a custom dash: round inside, square added), as LibreOffice measured;
    // Qt adds them to every dash, so they come off the dashes here.
    const bool custom = o.strokeDash == QLatin1String("custom");
    if (!dashes.isEmpty() && (o.strokeCap == 2 || (o.strokeCap == 1 && !custom)))
        for (int i = 0; i + 1 < dashes.size(); i += 2) {
            const qreal cut = qMin(1.0, dashes[i] - 0.01);
            dashes[i] -= cut;
            dashes[i + 1] += cut;
        }
    const int compound = qBound(0, o.strokeCompound, 4);
    if (compound == 0) return stroked(o, W, dashes, path);
    // The parallel lines, as offsets across the line (outside +, inside -),
    // in line widths: double 1:1:1, thick-thin 3:1:1, thin-thick 1:1:3, triple 1:1:1:1:1.
    QVector<QPair<qreal, qreal>> bands;
    switch (compound) {
    case 1: bands = {{1 / 6.0, .5}, {-.5, -1 / 6.0}}; break;
    case 2: bands = {{-.1, .5}, {-.5, -.3}}; break;
    case 3: bands = {{.3, .5}, {-.5, .1}}; break;
    default: bands = {{.3, .5}, {-.1, .1}, {-.5, -.3}}; break;
    }
    QPainterPath area;
    if (closedPath(path)) {
        for (const auto &[a, b] : bands) area = area.united(within(o, path, b * W).subtracted(within(o, path, a * W)));
    } else {
        // An open line has no inside: its lines are laid either side alike.
        for (const auto &[a, b] : bands) {
            const qreal hi = qMax(std::abs(a), std::abs(b)), lo = (a < 0 && b > 0) ? 0 : qMin(std::abs(a), std::abs(b));
            QPainterPath band = stroked(o, hi * 2 * W, {}, path);
            if (lo > 0) band = band.subtracted(stroked(o, lo * 2 * W, {}, path));
            area = area.united(band);
        }
    }
    if (!dashes.isEmpty()) area = area.intersected(stroked(o, W * 1.02, dashes, path));
    return area;
}

void FillPaint::paintLine(QPainter &painter, const SceneObject &o, const QPainterPath &path) {
    const QPainterPath area = lineArea(o, path);
    if (area.isEmpty()) return;
    painter.save();
    painter.setPen(Qt::NoPen);
    if (o.strokeGradient) painter.fillPath(area, gradientBrush(painter, lineSpec(o), o.rect, path, o));
    else painter.fillPath(area, o.strokeColor);
    painter.restore();
}

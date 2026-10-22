#include "io/metafile.h"

#include <QBuffer>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPicture>
#include <QRegion>
#include <QStringDecoder>
#include <QSvgGenerator>
#include <QtEndian>
#include <QtMath>

namespace Metafile {

namespace {

// ---- bytes

quint8 u8(const QByteArray &b, qint64 at) { return at >= 0 && at < b.size() ? quint8(b.at(at)) : 0; }
quint16 u16(const QByteArray &b, qint64 at) { return quint16(u8(b, at) | (u8(b, at + 1) << 8)); }
qint16 i16(const QByteArray &b, qint64 at) { return qint16(u16(b, at)); }
quint32 u32(const QByteArray &b, qint64 at) { return quint32(u16(b, at)) | (quint32(u16(b, at + 2)) << 16); }
qint32 i32(const QByteArray &b, qint64 at) { return qint32(u32(b, at)); }
float f32(const QByteArray &b, qint64 at) {
    const quint32 v = u32(b, at);
    float f;
    memcpy(&f, &v, 4);
    return std::isfinite(f) ? f : 0.0f;
}
QColor colorref(quint32 c) { return QColor(int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF)); }

constexpr quint32 kPlaceable = 0x9AC6CDD7;
constexpr quint32 kEmfSignature = 0x464D4520;   // " EMF"

bool isEmf(const QByteArray &b) { return b.size() >= 88 && u32(b, 0) == 1 && u32(b, 40) == kEmfSignature; }
bool isWmfHeader(const QByteArray &b, qint64 at) {
    const quint16 type = u16(b, at), size = u16(b, at + 2), version = u16(b, at + 4);
    return (type == 1 || type == 2) && size == 9 && (version == 0x0100 || version == 0x0300);
}

// ---- bitmaps

// A device-independent bitmap: its header (with colours) and its bits, as an
// image. `pal` says the colours are palette indexes, which have no palette here.
QImage dibImage(QByteArray bmi, const QByteArray &bits, bool pal = false) {
    if (bmi.size() < 12) return {};
    const quint32 headerSize = u32(bmi, 0);
    if (headerSize < 12 || headerSize > quint32(bmi.size())) return {};
    const bool core = headerSize == 12;
    const int bitCount = core ? u16(bmi, 10) : u16(bmi, 14);
    const quint32 compression = core ? 0 : u32(bmi, 16);
    if (compression == 4 || compression == 5) return QImage::fromData(bits.isEmpty() ? bmi.mid(headerSize) : bits);
    const quint32 used = core ? 0 : u32(bmi, 32);
    const int colours = bitCount <= 8 ? int(used ? qMin<quint32>(used, 256) : (1u << bitCount)) : int(qMin<quint32>(used, 256));
    int paletteBytes = colours * (core ? 3 : 4);
    if (pal && bitCount <= 8) {
        // Palette indexes: grey steps stand in for a palette the file doesn't carry.
        QByteArray grey;
        for (int i = 0; i < colours; ++i) {
            const char v = char(colours > 1 ? 255 * i / (colours - 1) : 0);
            grey.append(v).append(v).append(v);
            if (!core) grey.append('\0');
        }
        bmi = bmi.left(headerSize) + grey + bmi.mid(headerSize + colours * 2);
    }
    if (compression == 3 && headerSize == 40) paletteBytes += 12;
    QByteArray file("BM");
    file.append(QByteArray(12, '\0'));
    QByteArray body = bits.isEmpty() ? bmi : bmi.left(headerSize + paletteBytes) + bits;
    qToLittleEndian<quint32>(quint32(14 + body.size()), file.data() + 2);
    qToLittleEndian<quint32>(quint32(14 + headerSize + paletteBytes), file.data() + 10);
    QImage image = QImage::fromData(file + body, "BMP");
    return image.isNull() ? image : image.convertToFormat(QImage::Format_ARGB32);
}

QColor averageColour(const QImage &image) {
    if (image.isNull()) return Qt::gray;
    return image.scaled(1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).pixelColor(0, 0);
}

// What SVG can't do of a raster operation, done to the picture instead:
// AND drops white (the mask of the classic two-step transparency), OR and XOR
// drop black (the picture of it), NOTSRCCOPY inverts.
QImage applyRop(QImage image, quint32 rop) {
    const auto clear = [&image](QRgb key) {
        for (int y = 0; y < image.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
                if ((line[x] & 0xFFFFFF) == (key & 0xFFFFFF)) line[x] = 0;
        }
    };
    switch (rop) {
    case 0x008800C6: clear(0xFFFFFF); break;                     // SRCAND
    case 0x00EE0086: case 0x00660046: clear(0x000000); break;    // SRCPAINT, SRCINVERT
    case 0x00330008: image.invertPixels(); break;                 // NOTSRCCOPY
    default: break;
    }
    return image;
}

// ---- the device context

struct Pen {
    bool none = false;
    QColor color = Qt::black;
    qreal width = 0;           // logical units; 0 = one device pixel
    bool cosmetic = true;
    int style = 0;
    Qt::PenCapStyle cap = Qt::RoundCap;
    Qt::PenJoinStyle join = Qt::RoundJoin;
};
struct Brush {
    bool none = false;
    QColor color = Qt::white;
    int hatch = -1;
};
struct Font {
    QString face = QStringLiteral("Arial");
    qreal height = -12;
    int weight = 400;
    bool italic = false, underline = false, strike = false;
    qreal escapement = 0;
};
struct Obj {
    enum Kind { Empty, PenKind, BrushKind, FontKind, Other } kind = Empty;
    Pen pen;
    Brush brush;
    Font font;
};

struct State {
    Pen pen;
    Brush brush;
    Font font;
    QColor textColor = Qt::black, bkColor = Qt::white;
    int bkMode = 2, fillMode = 1, textAlign = 0, mapMode = 1, rop2 = 13;
    QPointF winOrg, winExt{1, 1}, vpOrg, vpExt{1, 1};
    bool winSet = false, vpSet = false;
    QTransform world;
    QPointF cur;
    bool clockwise = false;
    bool clipped = false;
    QPainterPath clip;         // in device units
};

Obj stockObject(quint32 n) {
    Obj o;
    switch (n) {
    case 0: case 1: case 2: case 3: case 4: {
        static const int grey[] = {255, 192, 128, 64, 0};
        o.kind = Obj::BrushKind;
        o.brush.color = QColor(grey[n], grey[n], grey[n]);
        break;
    }
    case 5: o.kind = Obj::BrushKind; o.brush.none = true; break;
    case 6: o.kind = Obj::PenKind; o.pen.color = Qt::white; break;
    case 7: o.kind = Obj::PenKind; break;
    case 8: o.kind = Obj::PenKind; o.pen.none = true; break;
    case 18: o.kind = Obj::BrushKind; break;
    case 19: o.kind = Obj::PenKind; break;
    case 10: case 11: case 16: o.kind = Obj::FontKind; o.font.face = QStringLiteral("Courier New"); break;
    case 12: case 13: case 14: case 17: o.kind = Obj::FontKind; break;
    default: o.kind = Obj::Other; break;
    }
    return o;
}

class Player {
public:
    explicit Player(const QByteArray &bytes) : b(bytes) {}

    // Reads the header: the kind, where the records start, and the picture's frame.
    bool begin(QString *error) {
        if (isEmf(b)) {
            emf = true;
            const QRectF bounds(QPointF(i32(b, 8), i32(b, 12)), QPointF(i32(b, 16) + 1, i32(b, 20) + 1));
            const QRectF frame(QPointF(i32(b, 24), i32(b, 28)), QPointF(i32(b, 32), i32(b, 36)));
            const qreal devW = i32(b, 72), devH = i32(b, 76), mmW = i32(b, 80), mmH = i32(b, 84);
            if (devW > 0 && devH > 0 && mmW > 0 && mmH > 0) {
                pxPerMm = QPointF(devW / mmW, devH / mmH);
                if (frame.width() > 0 && frame.height() > 0) {
                    canvas = QRectF(QPointF(frame.left() * pxPerMm.x() / 100.0, frame.top() * pxPerMm.y() / 100.0),
                                    QPointF(frame.right() * pxPerMm.x() / 100.0, frame.bottom() * pxPerMm.y() / 100.0));
                    natural = QSizeF(frame.width() / 100.0 * 96 / 25.4, frame.height() / 100.0 * 96 / 25.4);
                }
            }
            if (canvas.width() <= 0 || canvas.height() <= 0) canvas = bounds;
            start = u32(b, 4);
            return true;
        }
        qint64 at = 0;
        if (u32(b, 0) == kPlaceable) {
            const QRectF box(QPointF(i16(b, 6), i16(b, 8)), QPointF(i16(b, 10), i16(b, 12)));
            const int inch = u16(b, 14);
            canvas = box.normalized();
            if (inch > 0) natural = QSizeF(canvas.width() / inch * 96, canvas.height() / inch * 96);
            at = 22;
        }
        if (!isWmfHeader(b, at)) {
            if (error) *error = QStringLiteral("not a Windows drawing");
            return false;
        }
        objects.resize(qMin<int>(u16(b, at + 10), 4096));
        start = at + u16(b, at + 2) * 2;
        if (canvas.isEmpty()) {
            // No placeable header: the first window the drawing sets is its frame.
            for (qint64 r = start; r + 6 <= b.size();) {
                const qint64 size = qint64(u32(b, r)) * 2;
                const quint16 fn = u16(b, r + 4);
                if (size < 6 || fn == 0) break;
                if (fn == 0x020B) firstOrg = QPointF(i16(b, r + 8), i16(b, r + 6));
                if (fn == 0x020C) {
                    canvas = QRectF(firstOrg, QSizeF(i16(b, r + 8), i16(b, r + 6))).normalized();
                    break;
                }
                r += size;
            }
        }
        return true;
    }

    void play(QPainter *painter) {
        p = painter;
        s = State();
        stack.clear();
        handles.clear();
        for (auto &o : objects) o = Obj();
        inPath = false;
        path = QPainterPath();
        applyClip();
        p->setRenderHint(QPainter::Antialiasing);
        if (emf) playEmf();
        else playWmf();
    }

    QRectF canvas;
    QSizeF natural;

private:
    const QByteArray &b;
    bool emf = false;
    qint64 start = 0;
    QPointF firstOrg;
    QPointF pxPerMm{96 / 25.4, 96 / 25.4};
    QPainter *p = nullptr;
    State s;
    QVector<State> stack;
    QVector<Obj> objects;               // WMF: the object table
    QHash<quint32, Obj> handles;        // EMF: objects by handle
    bool inPath = false;
    QPainterPath path;
    QRectF clipBox;
    bool clipIsRect = false;

    // ---- mapping

    // Logical units to the picture's own (device) units.
    QTransform mapping() const {
        QTransform page;
        if (emf) {
            qreal sx = 1, sy = 1;
            switch (s.mapMode) {
            case 2: sx = pxPerMm.x() * 0.1; sy = -pxPerMm.y() * 0.1; break;
            case 3: sx = pxPerMm.x() * 0.01; sy = -pxPerMm.y() * 0.01; break;
            case 4: sx = pxPerMm.x() * 0.254; sy = -pxPerMm.y() * 0.254; break;
            case 5: sx = pxPerMm.x() * 0.0254; sy = -pxPerMm.y() * 0.0254; break;
            case 6: sx = pxPerMm.x() * 25.4 / 1440; sy = -pxPerMm.y() * 25.4 / 1440; break;
            case 7: case 8:
                sx = s.winExt.x() ? s.vpExt.x() / s.winExt.x() : 1;
                sy = s.winExt.y() ? s.vpExt.y() / s.winExt.y() : 1;
                if (s.mapMode == 7) {
                    const qreal m = qMin(qAbs(sx), qAbs(sy));
                    sx = sx < 0 ? -m : m;
                    sy = sy < 0 ? -m : m;
                }
                break;
            default: break;
            }
            page = QTransform(sx, 0, 0, sy, s.vpOrg.x() - s.winOrg.x() * sx, s.vpOrg.y() - s.winOrg.y() * sy);
            return s.world * page;
        }
        if (!s.winSet || !s.winExt.x() || !s.winExt.y() || canvas.isEmpty()) return {};
        // A WMF plays its window onto its frame: the window's origin at the
        // frame's top left, its extent across the frame (negative turns it).
        const qreal sx = canvas.width() / s.winExt.x(), sy = canvas.height() / s.winExt.y();
        return QTransform(sx, 0, 0, sy, canvas.left() - s.winOrg.x() * sx, canvas.top() - s.winOrg.y() * sy);
    }

    qreal deviceScale(const QTransform &m) const { return qSqrt(qAbs(m.determinant())); }

    // ---- painting
    //
    // Everything is painted in device units with the clip already cut out of
    // it: OmaShow's SVG renderer ignores clip paths, so the SVG carries none.

    // A one-pixel ("cosmetic") line, as wide as one would be on a screen
    // showing the picture: a 600th of it across.
    qreal hairline() const { return qMax(canvas.width(), canvas.height()) / 600.0; }

    // The pen in device units.
    QPen pen(const QTransform &m) const {
        if (s.pen.none || s.pen.style == 5) return Qt::NoPen;
        QPen out(s.pen.color);
        const bool thin = s.pen.cosmetic || s.pen.width <= 0;
        out.setWidthF(qMax(thin ? hairline() : s.pen.width * deviceScale(m), hairline()));
        switch (s.pen.style) {
        case 1: out.setStyle(Qt::DashLine); break;
        case 2: out.setStyle(Qt::DotLine); break;
        case 3: out.setStyle(Qt::DashDotLine); break;
        case 4: out.setStyle(Qt::DashDotDotLine); break;
        default: out.setStyle(Qt::SolidLine); break;
        }
        out.setCapStyle(s.pen.cap);
        out.setJoinStyle(s.pen.join);
        return out;
    }

    // Whether something covering `box` (device units) must be cut to the clip:
    // not when there is none, or when a rectangular one holds it whole.
    bool cutNeeded(const QRectF &box) const {
        if (!s.clipped) return false;
        return !(clipIsRect && clipBox.contains(box));
    }
    bool outsideClip(const QRectF &box) const { return s.clipped && !clipBox.intersects(box) && !box.isEmpty(); }

    void fillDevice(const QPainterPath &device, const QColor &color) {
        if (outsideClip(device.boundingRect())) return;
        const QPainterPath shape = cutNeeded(device.boundingRect()) ? device.intersected(s.clip) : device;
        if (shape.isEmpty()) return;
        p->setWorldTransform(QTransform());
        p->fillPath(shape, color);
    }
    void strokeDevice(const QPainterPath &device, const QPen &line) {
        if (line.style() == Qt::NoPen) return;
        const qreal w = line.widthF();
        const QRectF box = device.boundingRect().adjusted(-w, -w, w, w);
        if (outsideClip(box)) return;
        p->setWorldTransform(QTransform());
        if (!cutNeeded(box)) {
            p->strokePath(device, line);
            return;
        }
        QPainterPathStroker stroker(line);
        const QPainterPath outline = stroker.createStroke(device).intersected(s.clip);
        if (!outline.isEmpty()) p->fillPath(outline, line.color());
    }

    void fillAndStroke(QPainterPath shape, bool fill = true, bool stroke = true) {
        if (inPath) {
            path.addPath(shape);
            return;
        }
        shape.setFillRule(s.fillMode == 2 ? Qt::WindingFill : Qt::OddEvenFill);
        const QTransform m = mapping();
        const QPainterPath device = m.map(shape);
        if (fill && !s.brush.none) {
            if (s.brush.hatch >= 0) hatch(device);
            else fillDevice(device, s.brush.color);
        }
        if (stroke && s.rop2 != 11) strokeDevice(device, pen(m));
    }

    // A hatched brush, drawn as its lines (SVG has no hatches), a hundredth of the picture apart.
    void hatch(const QPainterPath &device) {
        if (s.bkMode == 2) fillDevice(device, s.bkColor);
        const QRectF box = device.boundingRect();
        const qreal step = hairline() * 6;
        const int h = s.brush.hatch;
        QPainterPath lines;
        const auto line = [&lines](QPointF a, QPointF b) { lines.moveTo(a); lines.lineTo(b); };
        if (h == 0 || h == 4)
            for (qreal y = box.top(); y <= box.bottom(); y += step) line(QPointF(box.left(), y), QPointF(box.right(), y));
        if (h == 1 || h == 4)
            for (qreal x = box.left(); x <= box.right(); x += step) line(QPointF(x, box.top()), QPointF(x, box.bottom()));
        const qreal span = box.width() + box.height();
        if (h == 2 || h == 5)
            for (qreal d = -span; d <= span; d += step)
                line(QPointF(box.left() + d, box.top()), QPointF(box.left() + d + box.height(), box.bottom()));
        if (h == 3 || h == 5)
            for (qreal d = -span; d <= span; d += step)
                line(QPointF(box.left() + d, box.bottom()), QPointF(box.left() + d + box.height(), box.top()));
        QPainterPathStroker stroker(QPen(s.brush.color, hairline(), Qt::SolidLine, Qt::FlatCap));
        QPainterPath area = device;
        if (s.clipped) area = area.intersected(s.clip);
        const QPainterPath strokes = stroker.createStroke(lines).intersected(area);
        if (strokes.isEmpty()) return;
        p->setWorldTransform(QTransform());
        p->fillPath(strokes, s.brush.color);
    }

    void polygon(const QPolygonF &points) {
        if (points.size() < 2) return;
        QPainterPath shape;
        shape.addPolygon(points);
        shape.closeSubpath();
        fillAndStroke(shape);
    }
    void polyline(const QPolygonF &points, bool fromCurrent = false) {
        if (points.isEmpty()) return;
        QPainterPath shape;
        shape.moveTo(fromCurrent ? s.cur : points.first());
        for (int i = fromCurrent ? 0 : 1; i < points.size(); ++i) shape.lineTo(points.at(i));
        if (inPath) {
            if (fromCurrent && path.elementCount()) for (const auto &pt : points) path.lineTo(pt);
            else path.addPath(shape);
        } else {
            fillAndStroke(shape, false, true);
        }
        if (fromCurrent) s.cur = points.last();
    }
    void beziers(const QPolygonF &points, bool fromCurrent) {
        QPainterPath shape;
        int i = 0;
        if (fromCurrent) shape.moveTo(s.cur);
        else if (!points.isEmpty()) shape.moveTo(points.at(i++));
        else return;
        for (; i + 2 < points.size(); i += 3) shape.cubicTo(points.at(i), points.at(i + 1), points.at(i + 2));
        if (inPath) {
            if (fromCurrent && path.elementCount()) {
                for (int k = 0; k + 2 < points.size(); k += 3) path.cubicTo(points.at(k), points.at(k + 1), points.at(k + 2));
            } else path.addPath(shape);
        } else fillAndStroke(shape, false, true);
        if (fromCurrent && !points.isEmpty()) s.cur = points.last();
    }
    void lineTo(const QPointF &to) {
        if (inPath) {
            if (!path.elementCount()) path.moveTo(s.cur);
            path.lineTo(to);
        } else {
            QPainterPath shape;
            shape.moveTo(s.cur);
            shape.lineTo(to);
            fillAndStroke(shape, false, true);
        }
        s.cur = to;
    }
    void moveTo(const QPointF &to) {
        s.cur = to;
        if (inPath) path.moveTo(to);
    }
    void rectangle(const QRectF &r) {
        QPainterPath shape;
        shape.addRect(r.normalized());
        fillAndStroke(shape);
    }
    void roundRect(const QRectF &r, const QSizeF &corner) {
        QPainterPath shape;
        shape.addRoundedRect(r.normalized(), qAbs(corner.width()) / 2, qAbs(corner.height()) / 2);
        fillAndStroke(shape);
    }
    void ellipse(const QRectF &r) {
        QPainterPath shape;
        shape.addEllipse(r.normalized());
        fillAndStroke(shape);
    }
    // Arc, chord (kind 1) or pie (kind 2), from the start radial to the end
    // radial, counter-clockwise unless the arc direction says otherwise.
    void arc(const QRectF &rect, const QPointF &from, const QPointF &to, int kind, bool connect = false) {
        const QRectF r = rect.normalized();
        if (r.width() <= 0 || r.height() <= 0) return;
        const QPointF c = r.center();
        const auto angleOf = [&](const QPointF &pt) {
            return qRadiansToDegrees(qAtan2(-(pt.y() - c.y()) * r.width() / r.height(), pt.x() - c.x()));
        };
        const qreal a0 = angleOf(from);
        qreal sweep = angleOf(to) - a0;
        if (s.clockwise) { while (sweep >= 0) sweep -= 360; }
        else { while (sweep <= 0) sweep += 360; }
        QPainterPath shape;
        if (kind == 2) {
            shape.moveTo(c);
            shape.arcTo(r, a0, sweep);
            shape.closeSubpath();
        } else {
            shape.arcMoveTo(r, a0);
            if (connect) {
                QPainterPath lead;
                lead.moveTo(s.cur);
                lead.lineTo(shape.currentPosition());
                shape = lead;
            }
            shape.arcTo(r, a0, sweep);
            if (kind == 1) shape.closeSubpath();
        }
        if (connect) s.cur = shape.currentPosition();
        fillAndStroke(shape, kind != 0, true);
    }

    // Text at a logical point, in the current font, laid out on the device so
    // a flipped mapping doesn't mirror it.
    void text(QPointF at, const QString &str, const QVector<qreal> &dx, quint32 options = 0, const QRectF &box = {}) {
        if (s.textAlign & 1) at = s.cur;                // TA_UPDATECP
        const QTransform m = mapping();
        if ((options & 2) && box.isValid()) {           // ETO_OPAQUE
            QPainterPath r;
            r.addRect(box.normalized());
            fillDevice(m.map(r), s.bkColor);
        }
        if (str.isEmpty()) return;
        const qreal scaleY = qSqrt(m.m21() * m.m21() + m.m22() * m.m22());
        const qreal scaleX = qSqrt(m.m11() * m.m11() + m.m12() * m.m12());
        const qreal height = s.font.height ? s.font.height : -12;
        const qreal em = height < 0 ? -height * scaleY : height * scaleY / 1.15;
        if (em <= 0) return;
        QFont font(s.font.face);
        font.setPixelSize(100);
        font.setWeight(QFont::Weight(qBound(100, s.font.weight ? s.font.weight : 400, 900)));
        font.setItalic(s.font.italic);
        font.setUnderline(s.font.underline);
        font.setStrikeOut(s.font.strike);
        const qreal k = em / 100.0;
        const QFontMetricsF metrics(font);
        qreal width = 0;
        QVector<qreal> steps;
        if (dx.size() >= str.size()) {
            for (int i = 0; i < str.size(); ++i) { steps.append(dx.at(i) * scaleX / k); width += steps.last(); }
        } else {
            width = metrics.horizontalAdvance(str);
        }
        qreal x = 0, y = 0;
        const int horizontal = s.textAlign & 6, vertical = s.textAlign & 24;
        if (horizontal == 6) x = -width / 2;
        else if (horizontal == 2) x = -width;
        if (vertical == 0) y = metrics.ascent();
        else if (vertical == 8) y = -metrics.descent();
        const QPointF origin = m.map(at);
        const qreal angle = s.font.escapement / 10.0;
        const QTransform local = QTransform::fromScale(k, k) * QTransform().rotate(-angle) * QTransform::fromTranslate(origin.x(), origin.y());
        if (s.bkMode == 2 && !(options & 2)) {
            QPainterPath back;
            back.addRect(QRectF(x, y - metrics.ascent(), width, metrics.height()));
            fillDevice(local.map(back), s.bkColor);
        }
        const QRectF bounds = local.mapRect(QRectF(x, y - metrics.ascent(), width, metrics.height()));
        if (outsideClip(bounds)) return;
        if (cutNeeded(bounds)) {
            // Cut by the clip: the letters as outlines.
            QPainterPath letters;
            if (steps.isEmpty()) letters.addText(QPointF(x, y), font, str);
            else {
                qreal pos = x;
                for (int i = 0; i < str.size(); ++i) { letters.addText(QPointF(pos, y), font, str.mid(i, 1)); pos += steps.at(i); }
            }
            fillDevice(local.map(letters), s.textColor);
        } else {
            p->setWorldTransform(local);
            p->setFont(font);
            p->setPen(s.textColor);
            if (steps.isEmpty()) {
                p->drawText(QPointF(x, y), str);
            } else {
                qreal pos = x;
                for (int i = 0; i < str.size(); ++i) {
                    p->drawText(QPointF(pos, y), str.mid(i, 1));
                    pos += steps.at(i);
                }
            }
        }
        if (s.textAlign & 1) {
            const QTransform back = m.inverted();
            s.cur = back.map(origin + QPointF(width * k * qCos(qDegreesToRadians(angle)), -width * k * qSin(qDegreesToRadians(angle))));
        }
    }

    // A bitmap into a logical rectangle (negative sizes mirror it).
    void bitmap(const QRectF &dest, QImage image, QRectF src, quint32 rop) {
        if (image.isNull()) { patBlt(dest, rop); return; }
        image = applyRop(image, rop);
        const QRect whole = image.rect();
        if (src.width() <= 0 || src.height() <= 0 || !QRectF(whole).contains(src.normalized())) src = whole;
        bool flipX = dest.width() < 0, flipY = dest.height() < 0;
        if (flipX || flipY) image = image.flipped((flipX ? Qt::Horizontal : Qt::Orientations()) | (flipY ? Qt::Vertical : Qt::Orientations()));
        if (flipX) src.moveLeft(whole.width() - src.right());
        if (flipY) src.moveTop(whole.height() - src.bottom());
        const QTransform m = mapping();
        const QRectF target = dest.normalized();
        const QRectF box = m.mapRect(target);
        if (outsideClip(box)) return;
        if (cutNeeded(box)) {
            // Clear what the clip hides from the picture itself.
            image = image.copy(src.toAlignedRect()).convertToFormat(QImage::Format_ARGB32);
            src = image.rect();
            const QTransform toImage = (QTransform::fromScale(target.width() / src.width(), target.height() / src.height()) *
                                        QTransform::fromTranslate(target.left(), target.top()) * m).inverted();
            QPainterPath hidden;
            hidden.addRect(image.rect());
            hidden = hidden.subtracted(toImage.map(s.clip));
            QPainter cut(&image);
            cut.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            cut.fillPath(hidden, Qt::black);
        }
        p->setWorldTransform(m);
        p->drawImage(target, image, src);
    }
    void patBlt(const QRectF &dest, quint32 rop) {
        QPainterPath r;
        r.addRect(dest.normalized());
        const QPainterPath device = mapping().map(r);
        switch (rop) {
        case 0x00FF0062: fillDevice(device, Qt::white); break;   // WHITENESS
        case 0x00000042: fillDevice(device, Qt::black); break;   // BLACKNESS
        case 0x00F00021: case 0x005A0049:                        // PATCOPY, PATINVERT
            if (!s.brush.none) fillDevice(device, s.brush.color);
            break;
        default: break;
        }
    }

    // ---- clipping, in device units

    // The clip's bounds, and whether it is a plain rectangle, for the quick tests.
    void applyClip() {
        clipBox = s.clipped ? s.clip.boundingRect() : QRectF();
        QPainterPath box;
        box.addRect(clipBox);
        clipIsRect = s.clipped && s.clip.simplified() == box.simplified();
    }
    QPainterPath everything() const {
        QPainterPath all;
        all.addRect(canvas.adjusted(-1e6, -1e6, 1e6, 1e6));
        return all;
    }
    // RGN_AND 1, OR 2, XOR 3, DIFF 4, COPY 5.
    void clip(const QPainterPath &device, int mode) {
        const QPainterPath current = s.clipped ? s.clip : everything();
        switch (mode) {
        case 1: s.clip = current.intersected(device); break;
        case 2: s.clip = current.united(device); break;
        case 3: s.clip = current.united(device).subtracted(current.intersected(device)); break;
        case 4: s.clip = current.subtracted(device); break;
        default: s.clip = device; break;
        }
        s.clipped = true;
        applyClip();
    }
    void clipRect(const QRectF &logical, bool exclude) {
        QPainterPath r;
        r.addRect(logical.normalized());
        clip(mapping().map(r), exclude ? 4 : 1);
    }
    void resetClip() {
        s.clipped = false;
        s.clip = QPainterPath();
        applyClip();
    }

    void save() {
        if (stack.size() < 1024) stack.append(s);
    }
    // RestoreDC: a negative n counts back from the last save, a positive one
    // names a save from the first.
    void restore(int n) {
        const int depth = int(stack.size());
        const int count = n < 0 ? -n : n > 0 ? depth - n + 1 : 0;
        if (count <= 0 || count > depth) return;
        stack.resize(depth - count + 1);
        s = stack.takeLast();
        applyClip();
    }

    void select(const Obj &o) {
        switch (o.kind) {
        case Obj::PenKind: s.pen = o.pen; break;
        case Obj::BrushKind: s.brush = o.brush; break;
        case Obj::FontKind: s.font = o.font; break;
        default: break;
        }
    }

    // ---- WMF

    QPointF wpt(qint64 at) const { return QPointF(i16(b, at), i16(b, at + 2)); }      // x, y
    QPointF wptYX(qint64 at) const { return QPointF(i16(b, at + 2), i16(b, at)); }    // y, x (reversed)
    QRectF wrect(qint64 at) const {    // bottom, right, top, left
        return QRectF(QPointF(i16(b, at + 6), i16(b, at + 4)), QPointF(i16(b, at + 2), i16(b, at)));
    }
    QPolygonF wpoints(qint64 at, int count, qint64 end) const {
        QPolygonF out;
        for (int i = 0; i < count && at + 4 * i + 4 <= end; ++i) out.append(wpt(at + 4 * i));
        return out;
    }
    void addObject(const Obj &o) {
        for (auto &slot : objects)
            if (slot.kind == Obj::Empty) { slot = o; return; }
    }
    QString ansi(const QByteArray &bytes) const {
        QByteArray t = bytes;
        const int zero = t.indexOf('\0');
        if (zero >= 0) t.truncate(zero);
        auto decode = QStringDecoder("windows-1252");
        return decode.isValid() ? QString(decode(t)) : QString::fromLatin1(t);
    }

    void playWmf() {
        int guard = 0;
        for (qint64 r = start; r + 6 <= b.size() && ++guard < 2000000;) {
            const qint64 size = qint64(u32(b, r)) * 2;
            const quint16 fn = u16(b, r + 4);
            if (fn == 0 || size < 6) break;
            const qint64 end = qMin<qint64>(r + size, b.size());
            const qint64 a = r + 6;
            switch (fn) {
            case 0x001E: save(); break;
            case 0x0127: restore(i16(b, a)); break;
            case 0x0102: s.bkMode = u16(b, a); break;
            case 0x0103: s.mapMode = u16(b, a); break;
            case 0x0104: s.rop2 = u16(b, a); break;
            case 0x0106: s.fillMode = u16(b, a); break;
            case 0x012E: s.textAlign = u16(b, a); break;
            case 0x0201: s.bkColor = colorref(u32(b, a)); break;
            case 0x0209: s.textColor = colorref(u32(b, a)); break;
            case 0x020B: s.winOrg = wptYX(a); s.winSet = true; break;
            case 0x020C: s.winExt = wptYX(a); s.winSet = true; break;
            case 0x020D: s.vpOrg = wptYX(a); break;
            case 0x020E: s.vpExt = wptYX(a); s.vpSet = true; break;
            case 0x020F: s.winOrg += wptYX(a); break;
            case 0x0410: {
                const qreal yd = i16(b, a), yn = i16(b, a + 2), xd = i16(b, a + 4), xn = i16(b, a + 6);
                if (xd && yd) s.winExt = QPointF(s.winExt.x() * xn / xd, s.winExt.y() * yn / yd);
                break;
            }
            case 0x0213: lineTo(wptYX(a)); break;
            case 0x0214: moveTo(wptYX(a)); break;
            case 0x0415: clipRect(wrect(a), true); break;
            case 0x0416: clipRect(wrect(a), false); break;
            case 0x012C: if (u16(b, a) == 0) resetClip(); break;
            case 0x0418: ellipse(wrect(a)); break;
            case 0x041B: rectangle(wrect(a)); break;
            case 0x061C: roundRect(wrect(a + 4), QSizeF(i16(b, a + 2), i16(b, a))); break;
            case 0x0817: case 0x0830: case 0x081A:
                arc(wrect(a + 8), wptYX(a + 4), wptYX(a), fn == 0x0817 ? 0 : fn == 0x0830 ? 1 : 2);
                break;
            case 0x0324: polygon(wpoints(a + 2, i16(b, a), end)); break;
            case 0x0325: polyline(wpoints(a + 2, i16(b, a), end)); break;
            case 0x0538: {
                const int n = u16(b, a);
                QPainterPath shape;
                qint64 at = a + 2 + 2 * n;
                for (int i = 0; i < n && at < end; ++i) {
                    const int count = u16(b, a + 2 + 2 * i);
                    const QPolygonF pts = wpoints(at, count, end);
                    if (pts.size() > 1) { shape.addPolygon(pts); shape.closeSubpath(); }
                    at += 4 * count;
                }
                fillAndStroke(shape);
                break;
            }
            case 0x0521: {
                const int count = i16(b, a);
                const qint64 at = a + 2 + ((count + 1) & ~1);
                text(wptYX(at), ansi(b.mid(a + 2, count)), {});
                break;
            }
            case 0x0A32: {
                const QPointF at = wptYX(a);
                const int count = i16(b, a + 4);
                const quint32 options = u16(b, a + 6);
                qint64 str = a + 8;
                QRectF box;
                if (options & 6) {
                    box = QRectF(QPointF(i16(b, str), i16(b, str + 2)), QPointF(i16(b, str + 4), i16(b, str + 6)));
                    str += 8;
                }
                QVector<qreal> dx;
                const qint64 dxAt = str + ((count + 1) & ~1);
                if (dxAt + 2 * count <= end)
                    for (int i = 0; i < count; ++i) dx.append(i16(b, dxAt + 2 * i));
                text(at, ansi(b.mid(str, count)), dx, options, box);
                break;
            }
            case 0x02FA: {
                Obj o;
                o.kind = Obj::PenKind;
                const int style = u16(b, a);
                o.pen.style = style & 0xF;
                o.pen.none = o.pen.style == 5;
                o.pen.width = i16(b, a + 2);
                o.pen.cosmetic = o.pen.width <= 1;
                o.pen.cap = (style & 0xF00) == 0x100 ? Qt::SquareCap : (style & 0xF00) == 0x200 ? Qt::FlatCap : Qt::RoundCap;
                o.pen.join = (style & 0xF000) == 0x1000 ? Qt::BevelJoin : (style & 0xF000) == 0x2000 ? Qt::MiterJoin : Qt::RoundJoin;
                o.pen.color = colorref(u32(b, a + 6));
                addObject(o);
                break;
            }
            case 0x02FC: {
                Obj o;
                o.kind = Obj::BrushKind;
                const int style = u16(b, a);
                o.brush.none = style == 1;
                o.brush.color = colorref(u32(b, a + 2));
                if (style == 2) o.brush.hatch = u16(b, a + 6);
                addObject(o);
                break;
            }
            case 0x0142: {
                Obj o;
                o.kind = Obj::BrushKind;
                o.brush.color = averageColour(dibImage(b.mid(a + 4, end - a - 4), {}, u16(b, a + 2) == 1));
                addObject(o);
                break;
            }
            case 0x01F9: {
                Obj o;
                o.kind = Obj::BrushKind;
                o.brush.color = Qt::gray;
                addObject(o);
                break;
            }
            case 0x02FB: {
                Obj o;
                o.kind = Obj::FontKind;
                o.font.height = i16(b, a);
                o.font.escapement = i16(b, a + 4);
                o.font.weight = i16(b, a + 8);
                o.font.italic = u8(b, a + 10);
                o.font.underline = u8(b, a + 11);
                o.font.strike = u8(b, a + 12);
                const QString face = ansi(b.mid(a + 18, qMin<qint64>(32, end - a - 18)));
                if (!face.isEmpty()) o.font.face = face;
                addObject(o);
                break;
            }
            case 0x00F7: case 0x06FF: {
                Obj o;
                o.kind = Obj::Other;
                addObject(o);
                break;
            }
            case 0x012D: {
                const int i = u16(b, a);
                if (i < objects.size()) select(objects.at(i));
                break;
            }
            case 0x01F0: {
                const int i = u16(b, a);
                if (i < objects.size()) objects[i] = Obj();
                break;
            }
            case 0x061D: {
                const quint32 rop = u32(b, a);
                patBlt(QRectF(QPointF(i16(b, a + 10), i16(b, a + 8)), QSizeF(i16(b, a + 6), i16(b, a + 4))), rop);
                break;
            }
            case 0x0F43: {      // STRETCHDIB
                const quint32 rop = u32(b, a);
                const QRectF src(i16(b, a + 12), i16(b, a + 10), i16(b, a + 8), i16(b, a + 6));
                const QRectF dest(i16(b, a + 20), i16(b, a + 18), i16(b, a + 16), i16(b, a + 14));
                bitmap(dest, dibImage(b.mid(a + 22, end - a - 22), {}, u16(b, a + 4) == 1), dibSource(src, a + 22), rop);
                break;
            }
            case 0x0B41: case 0x0940: {      // DIBSTRETCHBLT, DIBBITBLT
                const bool stretch = fn == 0x0B41;
                const quint32 rop = u32(b, a);
                const int words = int((end - a) / 2);
                const bool hasBitmap = words > (stretch ? 11 : 9);
                if (!hasBitmap) {
                    const qint64 e = end;
                    patBlt(QRectF(QPointF(i16(b, e - 2), i16(b, e - 4)), QSizeF(i16(b, e - 6), i16(b, e - 8))), rop);
                    break;
                }
                QRectF dest, src;
                qint64 dib;
                if (stretch) {
                    src = QRectF(i16(b, a + 10), i16(b, a + 8), i16(b, a + 6), i16(b, a + 4));
                    dest = QRectF(i16(b, a + 18), i16(b, a + 16), i16(b, a + 14), i16(b, a + 12));
                    dib = a + 20;
                } else {
                    dest = QRectF(i16(b, a + 14), i16(b, a + 12), i16(b, a + 10), i16(b, a + 8));
                    src = QRectF(i16(b, a + 6), i16(b, a + 4), dest.width(), dest.height());
                    dib = a + 16;
                }
                bitmap(dest, dibImage(b.mid(dib, end - dib), {}), dibSource(src, dib), rop);
                break;
            }
            case 0x0D33: {      // SETDIBTODEV
                const QRectF dest(i16(b, a + 16), i16(b, a + 14), i16(b, a + 12), i16(b, a + 10));
                bitmap(dest, dibImage(b.mid(a + 18, end - a - 18), {}, u16(b, a) == 1), {}, 0x00CC0020);
                break;
            }
            default: break;
            }
            r += size;
        }
    }

    // A source rectangle given from a bottom-up DIB's bottom, turned to the image's top.
    QRectF dibSource(QRectF src, qint64 dibAt) const {
        const quint32 headerSize = u32(b, dibAt);
        const qint32 height = headerSize == 12 ? i16(b, dibAt + 6) : i32(b, dibAt + 8);
        if (height > 0 && src.height() > 0) src.moveTop(height - src.top() - src.height());
        return src;
    }

    // ---- EMF

    QPointF ept(qint64 at) const { return QPointF(i32(b, at), i32(b, at + 4)); }
    QRectF erect(qint64 at) const { return QRectF(QPointF(i32(b, at), i32(b, at + 4)), QPointF(i32(b, at + 8), i32(b, at + 12))); }
    QPolygonF epoints(qint64 at, quint32 count, qint64 end, bool small) const {
        QPolygonF out;
        const int each = small ? 4 : 8;
        for (quint32 i = 0; i < count && at + each * qint64(i) + each <= end; ++i) {
            const qint64 q = at + each * qint64(i);
            out.append(small ? QPointF(i16(b, q), i16(b, q + 2)) : ept(q));
        }
        return out;
    }
    QImage ebitmap(qint64 r, qint64 offBmi, qint64 cbBmi, qint64 offBits, qint64 cbBits, quint32 usage) const {
        if (!cbBmi || offBmi <= 0) return {};
        return dibImage(b.mid(r + offBmi, cbBmi), cbBits ? b.mid(r + offBits, cbBits) : QByteArray(), usage == 1);
    }
    QRectF ebitmapSource(qint64 r, qint64 offBmi, QRectF src) const { return dibSource(src, r + offBmi); }
    QPainterPath region(qint64 at, qint64 end) const {
        const quint32 count = u32(b, at + 8);
        QRegion rgn;
        qint64 q = at + 32;
        for (quint32 i = 0; i < count && q + 16 <= end; ++i, q += 16)
            rgn += QRect(QPoint(i32(b, q), i32(b, q + 4)), QPoint(i32(b, q + 8) - 1, i32(b, q + 12) - 1));
        QPainterPath out;
        out.addRegion(rgn);
        return out;
    }
    void ecreate(quint32 handle, const Obj &o) { if (handles.size() < 65536) handles.insert(handle, o); }

    void playEmf() {
        int guard = 0;
        for (qint64 r = start; r + 8 <= b.size() && ++guard < 2000000;) {
            const quint32 type = u32(b, r);
            const qint64 size = u32(b, r + 4);
            if (size < 8 || (size & 3)) break;
            const qint64 end = qMin<qint64>(r + size, b.size());
            const qint64 a = r + 8;
            switch (type) {
            case 14: return;   // EOF
            case 9: s.winExt = ept(a); s.winSet = true; break;
            case 10: s.winOrg = ept(a); s.winSet = true; break;
            case 11: s.vpExt = ept(a); s.vpSet = true; break;
            case 12: s.vpOrg = ept(a); break;
            case 17: s.mapMode = int(u32(b, a)); break;
            case 18: s.bkMode = int(u32(b, a)); break;
            case 19: s.fillMode = int(u32(b, a)); break;
            case 20: s.rop2 = int(u32(b, a)); break;
            case 22: s.textAlign = int(u32(b, a)); break;
            case 24: s.textColor = colorref(u32(b, a)); break;
            case 25: s.bkColor = colorref(u32(b, a)); break;
            case 57: s.clockwise = u32(b, a) == 2; break;
            case 27: moveTo(ept(a)); break;
            case 54: lineTo(ept(a)); break;
            case 33: save(); break;
            case 34: restore(i32(b, a)); break;
            case 35: s.world = QTransform(f32(b, a), f32(b, a + 4), f32(b, a + 8), f32(b, a + 12), f32(b, a + 16), f32(b, a + 20)); break;
            case 36: {
                const QTransform x(f32(b, a), f32(b, a + 4), f32(b, a + 8), f32(b, a + 12), f32(b, a + 16), f32(b, a + 20));
                switch (u32(b, a + 24)) {
                case 1: s.world = QTransform(); break;
                case 2: s.world = x * s.world; break;
                case 3: s.world = s.world * x; break;
                case 4: s.world = x; break;
                default: break;
                }
                break;
            }
            case 37: {
                const quint32 h = u32(b, a);
                if (h & 0x80000000u) select(stockObject(h & 0x7FFFFFFF));
                else if (handles.contains(h)) select(handles.value(h));
                break;
            }
            case 40: handles.remove(u32(b, a)); break;
            case 38: {
                Obj o;
                o.kind = Obj::PenKind;
                const quint32 style = u32(b, a + 4);
                o.pen.style = int(style & 0xF);
                o.pen.none = o.pen.style == 5;
                o.pen.width = i32(b, a + 8);
                o.pen.cosmetic = o.pen.width <= 1 && !(style & 0x10000);
                o.pen.color = colorref(u32(b, a + 16));
                ecreate(u32(b, a), o);
                break;
            }
            case 95: {
                Obj o;
                o.kind = Obj::PenKind;
                const quint32 style = u32(b, a + 20);
                o.pen.style = int(style & 0xF);
                o.pen.none = o.pen.style == 5 || u32(b, a + 28) == 1;
                o.pen.width = u32(b, a + 24);
                o.pen.cosmetic = !(style & 0x10000);
                o.pen.cap = (style & 0xF00) == 0x100 ? Qt::SquareCap : (style & 0xF00) == 0x200 ? Qt::FlatCap : Qt::RoundCap;
                o.pen.join = (style & 0xF000) == 0x1000 ? Qt::BevelJoin : (style & 0xF000) == 0x2000 ? Qt::MiterJoin : Qt::RoundJoin;
                o.pen.color = colorref(u32(b, a + 32));
                ecreate(u32(b, a), o);
                break;
            }
            case 39: {
                Obj o;
                o.kind = Obj::BrushKind;
                const quint32 style = u32(b, a + 4);
                o.brush.none = style == 1;
                o.brush.color = colorref(u32(b, a + 8));
                if (style == 2) o.brush.hatch = int(u32(b, a + 12));
                ecreate(u32(b, a), o);
                break;
            }
            case 93: case 94: {
                Obj o;
                o.kind = Obj::BrushKind;
                o.brush.color = averageColour(ebitmap(r, u32(b, a + 8), u32(b, a + 12), u32(b, a + 16), u32(b, a + 20), u32(b, a + 4)));
                ecreate(u32(b, a), o);
                break;
            }
            case 82: {
                Obj o;
                o.kind = Obj::FontKind;
                const qint64 lf = a + 4;
                o.font.height = i32(b, lf);
                o.font.escapement = i32(b, lf + 8);
                o.font.weight = i32(b, lf + 16);
                o.font.italic = u8(b, lf + 20);
                o.font.underline = u8(b, lf + 21);
                o.font.strike = u8(b, lf + 22);
                QString face;
                for (int i = 0; i < 32; ++i) {
                    const quint16 c = u16(b, lf + 28 + 2 * i);
                    if (!c) break;
                    face.append(QChar(c));
                }
                if (!face.isEmpty()) o.font.face = face;
                ecreate(u32(b, a), o);
                break;
            }
            case 48: case 49: case 50: break;    // palettes
            case 42: ellipse(erect(a)); break;
            case 43: rectangle(erect(a)); break;
            case 44: roundRect(erect(a), QSizeF(i32(b, a + 16), i32(b, a + 20))); break;
            case 45: case 46: case 47: case 55:
                arc(erect(a), ept(a + 16), ept(a + 24), type == 46 ? 1 : type == 47 ? 2 : 0, type == 55);
                break;
            case 41: {   // ANGLEARC
                const QPointF c = ept(a);
                const qreal radius = u32(b, a + 8);
                const QRectF box(c - QPointF(radius, radius), c + QPointF(radius, radius));
                QPainterPath shape;
                shape.moveTo(s.cur);
                shape.arcTo(box, f32(b, a + 12), f32(b, a + 16));
                s.cur = shape.currentPosition();
                if (inPath) path.addPath(shape);
                else fillAndStroke(shape, false, true);
                break;
            }
            case 3: polygon(epoints(a + 20, u32(b, a + 16), end, false)); break;
            case 86: polygon(epoints(a + 20, u32(b, a + 16), end, true)); break;
            case 4: polyline(epoints(a + 20, u32(b, a + 16), end, false)); break;
            case 87: polyline(epoints(a + 20, u32(b, a + 16), end, true)); break;
            case 6: polyline(epoints(a + 20, u32(b, a + 16), end, false), true); break;
            case 89: polyline(epoints(a + 20, u32(b, a + 16), end, true), true); break;
            case 2: beziers(epoints(a + 20, u32(b, a + 16), end, false), false); break;
            case 85: beziers(epoints(a + 20, u32(b, a + 16), end, true), false); break;
            case 5: beziers(epoints(a + 20, u32(b, a + 16), end, false), true); break;
            case 88: beziers(epoints(a + 20, u32(b, a + 16), end, true), true); break;
            case 7: case 8: case 90: case 91: {
                const bool small = type >= 90;
                const bool closed = type == 8 || type == 91;
                const quint32 n = qMin<quint32>(u32(b, a + 16), 100000);
                qint64 at = a + 24 + 4 * qint64(n);
                QPainterPath shape;
                for (quint32 i = 0; i < n && at < end; ++i) {
                    const quint32 count = u32(b, a + 24 + 4 * i);
                    const QPolygonF pts = epoints(at, count, end, small);
                    if (pts.size() > 1) {
                        if (closed) { shape.addPolygon(pts); shape.closeSubpath(); }
                        else { shape.moveTo(pts.first()); for (int k = 1; k < pts.size(); ++k) shape.lineTo(pts.at(k)); }
                    }
                    at += qint64(count) * (small ? 4 : 8);
                }
                fillAndStroke(shape, closed, true);
                break;
            }
            case 59: inPath = true; path = QPainterPath(); break;
            case 60: inPath = false; break;
            case 61: if (inPath) path.closeSubpath(); break;
            case 68: inPath = false; path = QPainterPath(); break;
            case 62: case 63: case 64: {
                inPath = false;
                QPainterPath shape = path;
                path = QPainterPath();
                fillAndStroke(shape, type != 64, type != 62);
                break;
            }
            case 67: {
                inPath = false;
                clip(mapping().map(path), int(u32(b, a)));
                path = QPainterPath();
                break;
            }
            case 30: clipRect(erect(a), false); break;
            case 29: clipRect(erect(a), true); break;
            case 75: {
                const quint32 cb = u32(b, a), mode = u32(b, a + 4);
                if (!cb) { if (mode == 5) resetClip(); }
                else clip(region(a + 8, end), int(mode));
                break;
            }
            case 71: case 74: {    // FILLRGN, PAINTRGN
                const bool fill = type == 71;
                const QPainterPath shape = region(a + (fill ? 24 : 20), end);
                const Brush keep = s.brush;
                if (fill) {
                    const quint32 h = u32(b, a + 20);
                    const Obj o = (h & 0x80000000u) ? stockObject(h & 0x7FFFFFFF) : handles.value(h);
                    if (o.kind == Obj::BrushKind) s.brush = o.brush;
                }
                fillAndStroke(shape, true, false);
                s.brush = keep;
                break;
            }
            case 76: case 77: {    // BITBLT, STRETCHBLT
                const QRectF dest(i32(b, a + 16), i32(b, a + 20), i32(b, a + 24), i32(b, a + 28));
                const quint32 rop = u32(b, a + 32);
                const qint64 offBmi = u32(b, a + 72), cbBmi = u32(b, a + 76), offBits = u32(b, a + 80), cbBits = u32(b, a + 84);
                QRectF src(i32(b, a + 36), i32(b, a + 40), dest.width(), dest.height());
                if (type == 77) src.setSize(QSizeF(i32(b, a + 88), i32(b, a + 92)));
                const QImage image = ebitmap(r, offBmi, cbBmi, offBits, cbBits, u32(b, a + 68));
                if (image.isNull()) patBlt(dest, rop);
                else bitmap(dest, image, ebitmapSource(r, offBmi, src), rop);
                break;
            }
            case 81: {    // STRETCHDIBITS
                const QRectF dest(i32(b, a + 16), i32(b, a + 20), i32(b, a + 64), i32(b, a + 68));
                const QRectF src(i32(b, a + 24), i32(b, a + 28), i32(b, a + 32), i32(b, a + 36));
                const qint64 offBmi = u32(b, a + 40);
                bitmap(dest, ebitmap(r, offBmi, u32(b, a + 44), u32(b, a + 48), u32(b, a + 52), u32(b, a + 56)),
                       ebitmapSource(r, offBmi, src), u32(b, a + 60));
                break;
            }
            case 80: {    // SETDIBITSTODEVICE
                const QRectF dest(i32(b, a + 16), i32(b, a + 20), i32(b, a + 32), i32(b, a + 36));
                bitmap(dest, ebitmap(r, u32(b, a + 40), u32(b, a + 44), u32(b, a + 48), u32(b, a + 52), u32(b, a + 56)), {}, 0x00CC0020);
                break;
            }
            case 114: case 116: {    // ALPHABLEND, TRANSPARENTBLT
                const QRectF dest(i32(b, a + 16), i32(b, a + 20), i32(b, a + 24), i32(b, a + 28));
                const quint32 how = u32(b, a + 32);
                const qint64 offBmi = u32(b, a + 72);
                QImage image = ebitmap(r, offBmi, u32(b, a + 76), u32(b, a + 80), u32(b, a + 84), u32(b, a + 68));
                const QRectF src(i32(b, a + 36), i32(b, a + 40), i32(b, a + 88), i32(b, a + 92));
                if (image.isNull()) break;
                if (type == 116) {
                    const QRgb key = colorref(how).rgb();
                    for (int y = 0; y < image.height(); ++y) {
                        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
                        for (int x = 0; x < image.width(); ++x)
                            if ((line[x] & 0xFFFFFF) == (key & 0xFFFFFF)) line[x] = 0;
                    }
                    bitmap(dest, image, ebitmapSource(r, offBmi, src), 0x00CC0020);
                } else {
                    // AC_SRC_ALPHA: the bitmap's own alpha (premultiplied); else none.
                    const bool own = (how >> 24) & 1;
                    if (!own) image = image.convertToFormat(QImage::Format_RGB32).convertToFormat(QImage::Format_ARGB32);
                    else image.reinterpretAsFormat(QImage::Format_ARGB32_Premultiplied);
                    const qreal alpha = ((how >> 16) & 0xFF) / 255.0;
                    p->save();
                    p->setOpacity(alpha);
                    bitmap(dest, image, ebitmapSource(r, offBmi, src), 0x00CC0020);
                    p->restore();
                }
                break;
            }
            case 83: case 84: {    // EXTTEXTOUTA, EXTTEXTOUTW
                const qint64 t = a + 28;
                const QPointF at = ept(t);
                const quint32 count = qMin<quint32>(u32(b, t + 8), 65536);
                const qint64 offString = u32(b, t + 12);
                const quint32 options = u32(b, t + 16);
                const QRectF box = erect(t + 20);
                const qint64 offDx = u32(b, t + 36);
                QString str;
                if (type == 84) for (quint32 i = 0; i < count; ++i) str.append(QChar(u16(b, r + offString + 2 * i)));
                else str = ansi(b.mid(r + offString, count));
                QVector<qreal> dx;
                if (offDx && r + offDx + 4 * qint64(count) <= end)
                    for (quint32 i = 0; i < count; ++i) dx.append(i32(b, r + offDx + 4 * i));
                if (options & 0x2000) dx.clear();     // ETO_PDY: pairs, not one per letter
                text(at, str, dx, options, box);
                break;
            }
            case 108: {    // SMALLTEXTOUT
                const QPointF at = ept(a);
                const quint32 count = qMin<quint32>(u32(b, a + 8), 65536);
                const quint32 options = u32(b, a + 12);
                qint64 str = a + 28;
                QRectF box;
                if (!(options & 0x100)) { box = erect(str); str += 16; }
                QString text8;
                if (options & 0x200) text8 = ansi(b.mid(str, count));
                else for (quint32 i = 0; i < count; ++i) text8.append(QChar(u16(b, str + 2 * i)));
                text(at, text8, {}, options, box);
                break;
            }
            default: break;   // comments (EMF+ among them) and what draws nothing
            }
            r += size;
        }
    }
};

} // namespace

bool recognise(const QByteArray &bytes) {
    return isEmf(bytes) || u32(bytes, 0) == kPlaceable || isWmfHeader(bytes, 0);
}

QByteArray toSvg(const QByteArray &bytes, QString *error) {
    Player player(bytes);
    if (!player.begin(error)) return {};
    QRectF canvas = player.canvas;
    if (canvas.width() <= 0 || canvas.height() <= 0) {
        // No frame anywhere: the frame is what the drawing covers.
        QPicture picture;
        QPainter measure(&picture);
        player.play(&measure);
        measure.end();
        canvas = picture.boundingRect();
        player.canvas = canvas;
    }
    if (canvas.width() <= 0 || canvas.height() <= 0) {
        if (error) *error = QStringLiteral("the drawing is empty");
        return {};
    }
    QSizeF size = player.natural;
    if (size.width() < 1 || size.height() < 1) size = canvas.size().scaled(1000, 1000, Qt::KeepAspectRatio);
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QSvgGenerator generator(QSvgGenerator::SvgVersion::Svg11);
    generator.setOutputDevice(&buffer);
    generator.setViewBox(canvas);
    generator.setSize(QSize(qMax(1, qRound(size.width())), qMax(1, qRound(size.height()))));
    QPainter painter(&generator);
    player.play(&painter);
    painter.end();
    return buffer.data();
}

} // namespace Metafile

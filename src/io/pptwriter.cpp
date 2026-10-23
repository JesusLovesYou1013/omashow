#include "io/pptwriter.h"

#include "anim/presentation.h"
#include "core/fillstyle.h"
#include "core/design.h"
#include "core/fontswap.h"
#include "core/imageasset.h"
#include "core/mediaasset.h"
#include "core/shape.h"
#include "io/cfb.h"
#include "render/scenerenderer.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QtEndian>
#include <QtMath>

#include <cmath>
#include <functional>

namespace {

// ---- records

struct Bytes {
    QByteArray b;
    Bytes &u8(int v) { b.append(char(quint8(v))); return *this; }
    Bytes &u16(int v) { char d[2]; qToLittleEndian<quint16>(quint16(v), d); b.append(d, 2); return *this; }
    Bytes &u32(quint32 v) { char d[4]; qToLittleEndian<quint32>(v, d); b.append(d, 4); return *this; }
    Bytes &i32(qint32 v) { return u32(quint32(v)); }
    Bytes &raw(const QByteArray &d) { b.append(d); return *this; }
};

QByteArray record(int type, int instance, const QByteArray &data, int ver = 0) {
    Bytes h;
    h.u16((ver & 0xF) | (instance << 4)).u16(type).u32(quint32(data.size()));
    return h.b + data;
}
QByteArray container(int type, const QByteArray &children, int instance = 0) { return record(type, instance, children, 0xF); }

QByteArray utf16(const QString &s) {
    QByteArray out;
    for (const QChar c : s) { char d[2]; qToLittleEndian<quint16>(c.unicode(), d); out.append(d, 2); }
    return out;
}
QByteArray cstring(const QString &s, int instance) { return record(0x0FBA, instance, utf16(s)); }

quint32 colorref(const QColor &c) { return quint32(c.red()) | (quint32(c.green()) << 8) | (quint32(c.blue()) << 16); }
quint32 fixed(qreal v) { return quint32(qint32(qRound(v * 65536.0))); }

// "\005DocumentSummaryInformation": the property set where Office keeps each
// link's target and place ("_PID_HLINKS"), which LibreOffice reads to know
// where a link to a slide goes. Two sections — the document summary, and the
// user's own properties with their names in a dictionary.
QByteArray summaryInformation(const QVector<QPair<QString, QString>> &links) {
    const auto pad = [](Bytes &b) { while (b.b.size() % 4) b.u8(0); };
    const auto section = [&](const QVector<QPair<quint32, QByteArray>> &props) {
        Bytes body;
        const int head = 8 + 8 * props.size();
        QByteArray values;
        Bytes table;
        for (const auto &p : props) {
            table.u32(p.first).u32(quint32(head + values.size()));
            values += p.second;
        }
        body.u32(quint32(head + values.size())).u32(quint32(props.size())).raw(table.b).raw(values);
        return body.b;
    };
    Bytes codepage;
    codepage.u32(2).u16(1252);
    pad(codepage);

    Bytes dictionary;
    const QByteArray name = "_PID_HLINKS";
    dictionary.u32(1).u32(2).u32(quint32(name.size() + 1)).raw(name).u8(0);
    pad(dictionary);

    const auto text = [&](Bytes &b, const QString &s) {
        b.u32(0x1F).u32(quint32(s.size() + 1)).raw(utf16(s)).u16(0);
        pad(b);
    };
    Bytes blob;
    blob.u32(quint32(links.size() * 6));
    for (const auto &l : links) {
        blob.u32(3).i32(7).u32(3).i32(6).u32(3).i32(0).u32(3).i32(4); // as Office writes them; 4: on a shape
        text(blob, l.first);
        text(blob, l.second);
    }
    Bytes hlinks;
    hlinks.u32(0x41).u32(quint32(blob.b.size())).raw(blob.b);

    const QByteArray first = section({{1, codepage.b}});
    QByteArray second = section({{0, dictionary.b}, {1, codepage.b}, {2, hlinks.b}});
    // LibreOffice cuts every property short to the section's size less the
    // section's place in the stream, so the section carries that much room
    // at its end, or only the first link would survive.
    const int start = 28 + 2 * 20;
    const int room = start + first.size();
    second += QByteArray(room, 0);
    qToLittleEndian<quint32>(quint32(second.size()), second.data());

    static const unsigned char summary[16] = {0x02, 0xD5, 0xCD, 0xD5, 0x9C, 0x2E, 0x1B, 0x10, 0x93, 0x97, 0x08, 0x00, 0x2B, 0x2C, 0xF9, 0xAE};
    static const unsigned char own[16] = {0x05, 0xD5, 0xCD, 0xD5, 0x9C, 0x2E, 0x1B, 0x10, 0x93, 0x97, 0x08, 0x00, 0x2B, 0x2C, 0xF9, 0xAE};
    Bytes out;
    out.u16(0xFFFE).u16(0).u32(0x00020006).raw(QByteArray(16, 0)).u32(2);
    out.raw(QByteArray(reinterpret_cast<const char *>(summary), 16)).u32(quint32(start));
    out.raw(QByteArray(reinterpret_cast<const char *>(own), 16)).u32(quint32(start + first.size()));
    out.raw(first).raw(second);
    return out.b;
}

// Office Art properties, written in order of their number.
struct Opt {
    QMap<int, quint32> value;
    QMap<int, QByteArray> complex;
    QSet<int> blips;
    Opt &set(int pid, quint32 v) { value.insert(pid, v); return *this; }
    Opt &blip(int pid, int index) { value.insert(pid, quint32(index)); blips.insert(pid); return *this; }
    Opt &setComplex(int pid, const QByteArray &d) { value.insert(pid, quint32(d.size())); complex.insert(pid, d); return *this; }
    // A set of switches: each on or off, with its "use" bit set so it counts.
    Opt &flags(int pid, quint32 bit, bool on) {
        quint32 v = value.value(pid, 0);
        v |= bit << 16;
        if (on) v |= bit; else v &= ~bit;
        value.insert(pid, v);
        return *this;
    }
    QByteArray record(int type = 0xF00B) const {
        Bytes table, extra;
        for (auto it = value.cbegin(); it != value.cend(); ++it) {
            const bool isComplex = complex.contains(it.key());
            table.u16(it.key() | (isComplex ? 0x8000 : 0) | (blips.contains(it.key()) ? 0x4000 : 0)).u32(it.value());
            if (isComplex) extra.raw(complex.value(it.key()));
        }
        return ::record(type, int(value.size()), table.b + extra.b, 3);
    }
};

struct Warnings {
    QMap<QString, QList<int>> where;
    void add(const QString &message, int slide) {
        auto &list = where[message];
        if (!list.contains(slide)) list.append(slide);
    }
    QStringList lines() const {
        QStringList out;
        for (auto it = where.cbegin(); it != where.cend(); ++it) {
            QStringList numbers;
            for (int n : it.value()) if (n > 0) numbers.append(QString::number(n));
            if (numbers.isEmpty()) out.append(it.key());
            else out.append(QStringLiteral("%1 (slide%2 %3)").arg(it.key(), numbers.size() > 1 ? QStringLiteral("s") : QString(), numbers.join(QStringLiteral(", "))));
        }
        return out;
    }
};

QByteArray encoded(const QImage &image, const char *format, int quality = -1) {
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format, quality);
    return out;
}

QString roman(int n, bool upper) {
    static const struct { int v; const char *s; } table[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
                                                             {50, "l"}, {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
    QString out;
    for (const auto &t : table) while (n >= t.v) { out += QLatin1String(t.s); n -= t.v; }
    return upper ? out.toUpper() : out;
}
QString letters(int n, bool upper) {
    QString out;
    for (; n > 0; n = (n - 1) / 26) out.prepend(QChar(char16_t((upper ? 'A' : 'a') + (n - 1) % 26)));
    return out;
}

// ---- the writer

struct Writer {
    const Document &doc;
    std::shared_ptr<Workers::Job> job;
    Warnings warnings;
    qreal k = 4;                 // master units (576 an inch) per slide unit
    QSize slideSize;             // in master units
    QStringList fonts;
    QByteArray pictures;         // the "Pictures" stream
    QByteArray store;            // FBSE records
    QHash<QByteArray, int> pictureIndex;   // by MD5: 1-based
    int pictureCount = 0;
    struct Link { QString target, location; };
    QVector<Link> links;         // ExHyperlink 1…n
    QHash<QString, quint32> slideIdOf;      // OmaShow slide id → the file's
    QHash<QString, int> slideNumberOf;
    int slide = 0;               // 1-based while a slide is written

    // Drawings and their shape ids: each drawing gets its own run of 1024.
    struct Cluster { quint32 dg, used; };
    QVector<Cluster> clusters;
    int drawings = 0, shapesSaved = 0;
    quint32 spid = 0, spidLimit = 0;
    int dg = 0, dgShapes = 0;

    explicit Writer(const Document &d) : doc(d) {
        const qreal w = qMax(1.0, doc.size.width()), h = qMax(1.0, doc.size.height());
        k = (w >= h ? 4320.0 / h : 4320.0 / w);
        slideSize = QSize(qRound(w * k), qRound(h * k));
    }

    bool canceled() const { return job && job->canceled; }
    qint32 mu(qreal units) const { return qint32(qRound(units * k)); }
    quint32 emu(qreal units) const { return quint32(qMax<qint64>(0, qRound64(units * k * 1587.5))); }
    qreal points(qreal units) const { return units * k / 8.0; }

    int font(const QString &family) {
        const QString name = family.isEmpty() ? QStringLiteral("Arial") : family.left(31);
        const int at = fonts.indexOf(name);
        if (at >= 0) return at;
        fonts.append(name);
        return fonts.size() - 1;
    }

    // ---- shape ids

    void beginDrawing() {
        dg = ++drawings;
        dgShapes = 0;
        nextCluster();
    }
    void nextCluster() {
        clusters.append({quint32(dg), 0});
        spid = quint32(clusters.size()) * 1024;
        spidLimit = spid + 1023;
    }
    quint32 newSpid() {
        if (spid >= spidLimit) nextCluster();
        ++clusters.last().used;
        ++dgShapes;
        ++shapesSaved;
        return spid++;
    }

    // ---- pictures

    // A picture in the store: its 1-based index.
    int addPicture(const QByteArray &data, bool jpeg) {
        const QByteArray uid = QCryptographicHash::hash(data, QCryptographicHash::Md5);
        if (pictureIndex.contains(uid)) return pictureIndex.value(uid);
        const int type = jpeg ? 0xF01D : 0xF01E;
        const int instance = jpeg ? 0x46A : 0x6E0;
        const QByteArray blip = record(type, instance, uid + QByteArray(1, char(0xFF)) + data);
        Bytes fbse;
        fbse.u8(jpeg ? 5 : 6).u8(jpeg ? 5 : 6).raw(uid).u16(0xFF).u32(quint32(blip.size())).u32(1).u32(quint32(pictures.size()))
            .u8(0).u8(0).u8(0).u8(0);
        store += record(0xF007, jpeg ? 5 : 6, fbse.b, 2);
        pictures += blip;
        pictureIndex.insert(uid, ++pictureCount);
        return pictureCount;
    }

    int addImage(const QImage &image) { return addPicture(encoded(image, "PNG"), false); }

    // The picture as OmaShow draws it into its box.
    QImage drawn(const SceneObject &o, qreal density) const {
        const QSize size(qMax(1, qRound(o.rect.width() * density)), qMax(1, qRound(o.rect.height() * density)));
        QImage out(size, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter painter(&out);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.scale(density, density);
        painter.translate(-o.rect.topLeft());
        SceneObject flat = o;
        flat.rotation = 0;
        flat.opacity = 1;
        flat.shadowEnabled = false;
        flat.strokeWidth = 0;
        ImageAsset::paint(painter, flat);
        return out;
    }

    // Anything OmaShow draws itself (a table, a chart, an equation), as a picture.
    QImage snapshot(const SceneObject &o) const {
        const qreal density = qBound(1.0, 2.0, 4096.0 / qMax(1.0, qMax(o.rect.width(), o.rect.height())));
        QImage image(qMax(1, qRound(o.rect.width() * density)), qMax(1, qRound(o.rect.height() * density)), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.scale(density, density);
        painter.translate(-o.rect.topLeft());
        SceneObject flat = o;
        flat.rotation = 0;
        flat.opacity = 1;
        SceneRenderer::paint(painter, {flat}, doc.size.height());
        return image;
    }

    // ---- shapes

    // A box in master units, kept as the format keeps a turned one: turned
    // near upright, its sides swapped about its middle.
    QByteArray anchor(const QRectF &r, qreal rotation) const {
        QRectF box = r;
        const qreal turn = std::fmod(std::fmod(rotation, 360.0) + 360.0, 360.0);
        if ((turn >= 45 && turn < 135) || (turn >= 225 && turn < 315)) {
            const QPointF c = r.center();
            box = QRectF(c.x() - r.height() / 2, c.y() - r.width() / 2, r.height(), r.width());
        }
        const auto clamp = [](qint32 v) { return qBound(-32768, v, 32767); };
        Bytes a;
        a.u16(clamp(mu(box.top()))).u16(clamp(mu(box.left()))).u16(clamp(mu(box.right()))).u16(clamp(mu(box.bottom())));
        return record(0xF010, 0, a.b);
    }

    QByteArray spContainer(int type, quint32 flags, const Opt &opt, const QByteArray &anchorRecord,
                           const QByteArray &clientData = {}, const QByteArray &textbox = {}) {
        Bytes fsp;
        fsp.u32(newSpid()).u32(flags | 0x800 | (anchorRecord.isEmpty() ? 0 : 0x200));
        QByteArray inside = record(0xF00A, type, fsp.b, 2) + opt.record() + anchorRecord;
        if (!clientData.isEmpty()) inside += container(0xF011, clientData);
        if (!textbox.isEmpty()) inside += container(0xF00D, textbox);
        return container(0xF004, inside);
    }

    void common(Opt &opt, const SceneObject &o) const {
        if (!qFuzzyIsNull(o.rotation)) opt.set(0x04, fixed(o.rotation));
        if (!o.altTitle.isEmpty()) opt.setComplex(0x380, utf16(o.altTitle) + QByteArray(2, '\0'));
        if (!o.altText.isEmpty()) opt.setComplex(0x381, utf16(o.altText) + QByteArray(2, '\0'));
        if (o.hidden) opt.flags(0x3BF, 0x2, true);
    }

    void noFill(Opt &opt) const { opt.flags(0x1BF, 0x10, false); }
    void noLine(Opt &opt) const { opt.flags(0x1FF, 0x8, false); }

    void fill(Opt &opt, const SceneObject &o) {
        const qreal alpha = o.opacity;
        const auto opacity = [&](const QColor &c) { return fixed(qBound(0.0, c.alphaF() * alpha, 1.0)); };
        switch (o.fillStyle) {
        case 5:
            noFill(opt);
            return;
        case 1:   // a line of shading: the angle turns anticlockwise from left-to-right
            opt.set(0x180, 7).set(0x181, colorref(o.fillSecondary)).set(0x182, opacity(o.fillSecondary))
                .set(0x183, colorref(o.fill)).set(0x184, opacity(o.fill))
                .set(0x18B, fixed(std::remainder(90.0 - o.fillAngle, 360.0)))
                .set(0x18C, 0);
            break;
        case 2:   // out from the middle
            opt.set(0x180, 5).set(0x181, colorref(o.fillSecondary)).set(0x182, opacity(o.fillSecondary))
                .set(0x183, colorref(o.fill)).set(0x184, opacity(o.fill))
                .set(0x18D, fixed(0.5)).set(0x18E, fixed(0.5)).set(0x18F, fixed(0.5)).set(0x190, fixed(0.5));
            break;
        case 4: {
            SceneObject pic = o;
            pic.type = ObjectType::Image;
            int index = 0;
            if (!o.image.isNull() || !o.vectorPicture.isNull()) {
                const qreal density = qBound(1.0, 2.0, 4096.0 / qMax(1.0, qMax(o.rect.width(), o.rect.height())));
                index = addImage(drawn(pic, density));
            }
            if (index > 0) { opt.set(0x180, 3).blip(0x186, index); break; }
            opt.set(0x180, 0).set(0x181, colorref(o.fill)).set(0x182, opacity(o.fill));
            break;
        }
        case 3:
            warnings.add(QStringLiteral("A pattern fill was written in its main colour"), slide);
            Q_FALLTHROUGH();
        default:
            opt.set(0x180, 0).set(0x181, colorref(o.fill)).set(0x182, opacity(o.fill));
            break;
        }
        opt.flags(0x1BF, 0x10, true);
    }

    void stroke(Opt &opt, const SceneObject &o, bool arrows = false) const {
        if (o.strokeWidth <= 0 || o.strokeColor.alpha() == 0) { noLine(opt); return; }
        opt.set(0x1C0, colorref(o.strokeColor)).set(0x1C1, fixed(qBound(0.0, o.strokeColor.alphaF() * o.opacity, 1.0)))
            .set(0x1CB, emu(o.strokeWidth));
        if (o.strokeStyle > 0 && o.strokeStyle <= 4) opt.set(0x1CE, quint32(o.strokeStyle));
        opt.set(0x1D6, o.strokeJoin == 1 ? 0 : o.strokeJoin == 0 ? 1 : 2);
        opt.set(0x1D7, o.strokeCap == 2 ? 0 : o.strokeCap == 1 ? 1 : 2);
        if (arrows) {
            if (o.connectorArrowStart) opt.set(0x1D0, 1);
            if (o.connectorArrowEnd) opt.set(0x1D1, 1);
        }
        opt.flags(0x1FF, 0x8, true);
    }

    void shadow(Opt &opt, const SceneObject &o) const {
        if (!o.shadowEnabled || o.shadowColor.alpha() == 0) return;
        opt.set(0x201, colorref(o.shadowColor)).set(0x204, fixed(qBound(0.0, o.shadowColor.alphaF() * o.opacity, 1.0)))
            .set(0x205, quint32(qint32(qRound(o.shadowX * k * 1587.5)))).set(0x206, quint32(qint32(qRound(o.shadowY * k * 1587.5))));
        opt.flags(0x23F, 0x2, true);
    }

    // A link on click: ClientData's InteractiveInfo.
    QByteArray click(const SceneObject &o) {
        if (!o.linkKind || o.linkTarget.isEmpty()) return {};
        Link link;
        int kind = 8;
        if (o.linkKind == 1) link.target = o.linkTarget;
        else if (o.linkKind == 2) link.target = QStringLiteral("mailto:") + o.linkTarget;
        else if (o.linkKind == 3 && slideIdOf.contains(o.linkTarget)) {
            link.location = QStringLiteral("%1,%2,Slide %2").arg(slideIdOf.value(o.linkTarget)).arg(slideNumberOf.value(o.linkTarget));
            kind = 7;
        } else return {};
        links.append(link);
        Bytes atom;
        atom.u32(0).u32(quint32(links.size())).u8(4).u8(0).u8(0).u8(0).u8(kind).u8(0).u8(0).u8(0);
        return container(0x0FF2, record(0x0FF3, 0, atom.b));
    }

    // An outline in its own 21600 square: points and segments.
    void geometry(Opt &opt, const QPainterPath &path, const QRectF &box, bool closed) const {
        const qreal sx = 21600.0 / qMax(0.001, box.width()), sy = 21600.0 / qMax(0.001, box.height());
        const auto pt = [&](const QPointF &p) { return QPoint(qBound(-32768, qRound((p.x() - box.left()) * sx), 32767), qBound(-32768, qRound((p.y() - box.top()) * sy), 32767)); };
        QVector<QPoint> points;
        QVector<quint16> segments;
        QPointF start;
        int i = 0;
        const int n = path.elementCount();
        const auto finish = [&]() {
            if (closed && !segments.isEmpty()) segments.append(0x6001);
        };
        while (i < n) {
            const auto e = path.elementAt(i);
            if (e.type == QPainterPath::MoveToElement) {
                if (!segments.isEmpty()) finish();
                points.append(pt(e));
                segments.append(0x4000);
                start = e;
                ++i;
            } else if (e.type == QPainterPath::LineToElement) {
                points.append(pt(e));
                if (!segments.isEmpty() && (segments.last() & 0xE000) == 0x0000 && (segments.last() & 0x1FFF) < 0x1FFF) ++segments.last();
                else segments.append(0x0001);
                ++i;
            } else if (e.type == QPainterPath::CurveToElement && i + 2 < n) {
                points.append(pt(e));
                points.append(pt(path.elementAt(i + 1)));
                points.append(pt(path.elementAt(i + 2)));
                if (!segments.isEmpty() && (segments.last() & 0xE000) == 0x2000 && (segments.last() & 0x1FFF) < 0x1FFF) ++segments.last();
                else segments.append(0x2001);
                i += 3;
            } else ++i;
        }
        finish();
        segments.append(0x8000);
        Bytes v, s;
        v.u16(points.size()).u16(points.size()).u16(0xFFF0);
        for (const auto &p : points) v.u16(p.x()).u16(p.y());
        s.u16(segments.size()).u16(segments.size()).u16(2);
        for (quint16 seg : segments) s.u16(seg);
        opt.set(0x140, 0).set(0x141, 0).set(0x142, 21600).set(0x143, 21600).set(0x144, 4);
        opt.setComplex(0x145, v.b).setComplex(0x146, s.b);
        if (!closed) opt.flags(0x17F, 0x1, false);   // fFillOK: an open line holds no fill
    }

    QByteArray writeShape(const SceneObject &o) {
        Opt opt;
        common(opt, o);
        shadow(opt, o);
        const QByteArray clickData = click(o);
        const bool line = o.connector || (o.shapeKind == 13);
        if (line) {
            stroke(opt, o, true);
            noFill(opt);
            const QPainterPath path = Shape::path(o);
            if (o.connector && o.connectorRoute == 0 && path.elementCount() == 2) {
                // A straight line: its box, mirrored to run the right way.
                const QPointF a = o.connectorStart, b = o.connectorEnd;
                const QRectF box = QRectF(a, b).normalized();
                quint32 flags = 0;
                if (b.x() < a.x()) flags |= 0x40;
                if (b.y() < a.y()) flags |= 0x80;
                opt.value.remove(0x04);
                return spContainer(20, flags, opt, anchor(box, 0), clickData);
            }
            QRectF box = path.boundingRect();
            if (box.width() < 1) box.setWidth(1);
            if (box.height() < 1) box.setHeight(1);
            geometry(opt, path, box, false);
            return spContainer(0, 0, opt, anchor(box, o.rotation), clickData);
        }
        fill(opt, o);
        stroke(opt, o);
        int type = 0;
        switch (o.shapeKind) {
        case 0: type = o.cornerRadius > 0.5 ? 2 : 1; break;
        case 1: type = 3; break;
        case 3: type = 4; break;
        default: break;
        }
        if (type == 2) opt.set(0x147, quint32(qBound(0, qRound(o.cornerRadius / qMax(1.0, qMin(o.rect.width(), o.rect.height())) * 21600), 10800)));
        if (type == 0) geometry(opt, Shape::path(o), o.rect, true);
        return spContainer(type, 0, opt, anchor(o.rect, o.rotation), clickData);
    }

    QByteArray writePicture(const SceneObject &o) {
        if ((o.image.isNull() && o.vectorPicture.isNull()) || o.rect.isEmpty()) {
            warnings.add(QStringLiteral("A picture had no data to write"), slide);
            return {};
        }
        const QString format = o.imageFormat.toLower();
        const QSizeF natural = ImageAsset::size(o);
        const QRectF source = ImageAsset::sourceRect(o);
        QSizeF target = o.rect.size();
        if (o.imageMode == 0) { target = source.size(); target.scale(o.rect.size(), Qt::KeepAspectRatio); }
        const bool whole = qAbs(source.x()) < 0.5 && qAbs(source.y()) < 0.5 && qAbs(source.width() - natural.width()) < 0.5 &&
                           qAbs(source.height() - natural.height()) < 0.5;
        const bool fills = qAbs(target.width() - o.rect.width()) < 0.5 && qAbs(target.height() - o.rect.height()) < 0.5;
        const bool adjusted = !qFuzzyIsNull(o.imageBrightness) || !qFuzzyCompare(o.imageContrast, 1.0) ||
                              !qFuzzyCompare(o.imageSaturation, 1.0) || !qFuzzyIsNull(o.imageTintAmount);
        const bool plain = whole && fills && !adjusted && o.imageMask == 0;
        int index = 0;
        if (plain && !o.imageData.isEmpty() && (format == QLatin1String("png") || format == QLatin1String("jpg") || format == QLatin1String("jpeg")))
            index = addPicture(o.imageData, format != QLatin1String("png"));
        else if (plain && !o.image.isNull() && format != QLatin1String("svg"))
            index = addImage(o.image);
        else {
            const bool svg = format == QLatin1String("svg");
            const qreal perUnit = svg ? 2.0 : source.width() / qMax(1.0, target.width());
            const qreal density = qBound(1.0, perUnit, 4096.0 / qMax(1.0, qMax(o.rect.width(), o.rect.height())));
            const QImage image = drawn(o, density);
            const bool photo = (format == QLatin1String("jpg") || format == QLatin1String("jpeg")) && o.imageMask == 0 && fills;
            index = photo ? addPicture(encoded(image.convertToFormat(QImage::Format_RGB32), "JPEG", 92), true) : addImage(image);
        }
        Opt opt;
        common(opt, o);
        shadow(opt, o);
        opt.blip(0x104, index);
        noFill(opt);
        SceneObject outline = o;
        if (o.imageMask != 0 && o.strokeWidth > 0) warnings.add(QStringLiteral("A shaped picture's outline was written as a rectangle"), slide);
        stroke(opt, outline);
        if (o.opacity < 0.999) warnings.add(QStringLiteral("A see-through picture was written solid"), slide);
        return spContainer(75, 0, opt, anchor(o.rect, o.rotation), click(o));
    }

    QByteArray writeSnapshot(const SceneObject &o, const QString &what) {
        if (o.rect.isEmpty()) return {};
        warnings.add(what, slide);
        Opt opt;
        common(opt, o);
        opt.blip(0x104, addImage(snapshot(o)));
        noFill(opt);
        noLine(opt);
        return spContainer(75, 0, opt, anchor(o.rect, o.rotation), click(o));
    }

    // ---- text

    struct Look {
        int font = 0, size = 18, color = 0;
        bool bold = false, italic = false, underline = false, shadow = false;
        int baseline = 0;
        bool operator==(const Look &o) const {
            return font == o.font && size == o.size && color == o.color && bold == o.bold && italic == o.italic &&
                   underline == o.underline && shadow == o.shadow && baseline == o.baseline;
        }
    };

    QByteArray pf(const SceneObject &o, int level, bool bullet, QChar marker, qreal lineGap) const {
        const qreal em = o.fontSize * 1.2;
        Bytes d;
        quint32 mask = 0x1 | 0x800 | 0x1000 | 0x4000 | 0x100 | 0x400;
        if (bullet) mask |= 0x80;
        d.u32(mask).u16(bullet ? 0x1 : 0);
        if (bullet) d.u16(marker.unicode());
        d.u16(qBound(0, o.textAlign, 3));
        d.u16(quint16(qint16(qBound(50, qRound(o.lineHeight), 400))));
        d.u16(quint16(qint16(-qBound(0, qRound(points(lineGap) * 8), 32767))));
        const int depth = bullet ? level + 1 : 0;
        d.u16(quint16(qBound(0, mu(em * depth), 32767)));
        d.u16(quint16(qBound(0, mu(em * qMax(0, depth - 1)), 32767)));
        return d.b;
    }

    QByteArray cf(const Look &l) const {
        Bytes d;
        d.u32(0x1 | 0x2 | 0x4 | 0x10 | 0x10000 | 0x20000 | 0x40000 | 0x80000);
        d.u16((l.bold ? 0x1 : 0) | (l.italic ? 0x2 : 0) | (l.underline ? 0x4 : 0) | (l.shadow ? 0x10 : 0));
        d.u16(l.font).u16(qBound(1, l.size, 4000)).u32(quint32(l.color)).u16(quint16(qint16(l.baseline == 1 ? 30 : l.baseline == 2 ? -25 : 0)));
        return d.b;
    }

    // The words of a box, as a text header, the characters and their styles.
    QByteArray textbox(const SceneObject &o, int textType, const QString &override = {}) {
        QString source = override.isNull() ? o.text : override;
        if (o.uppercase) source = source.toUpper();
        if (!qFuzzyIsNull(o.letterSpacing)) warnings.add(QStringLiteral("Letter spacing was left out"), slide);
        if (o.columns > 1) warnings.add(QStringLiteral("Text in columns was written as one column"), slide);
        if (o.textKind == 2) warnings.add(QStringLiteral("Words on a path were written as an ordinary text box"), slide);
        const QStringList paragraphs = source.split(QLatin1Char('\n'));
        const bool listed = o.listStyle > 0;
        const bool numbers = listed && (o.listStyle == 2 || o.listStyle >= 5);
        if (numbers) warnings.add(QStringLiteral("Numbered lists were written with their numbers typed in"), slide);
        Look base;
        base.font = font(o.fontFamily);
        base.size = qMax(1, qRound(points(o.fontSize)));
        base.color = int(colorref(o.textColor) | 0xFE000000);
        base.bold = o.fontWeight >= 600;
        base.italic = o.italic;
        base.underline = o.underline;
        base.shadow = o.shadowEnabled && o.shadowColor.alpha() > 0;
        bool struck = false;

        QString chars;
        QVector<Look> looks;
        QByteArray pfRuns;
        QVector<int> counters(10, 0);
        int offset = 0;   // where this paragraph starts in `source`
        for (int p = 0; p < paragraphs.size(); ++p) {
            QString line = paragraphs[p];
            int level = 0, skip = 0;
            if (listed) while (skip < line.size() && line.at(skip) == QLatin1Char('\t')) { ++level; ++skip; }
            level = qMin(level, 4);
            const QString words = line.mid(skip);
            const int paraStart = chars.size();
            QChar marker;
            bool bullet = false;
            if (listed && !words.trimmed().isEmpty()) {
                if (numbers) {
                    for (int deeper = level + 1; deeper < counters.size(); ++deeper) counters[deeper] = 0;
                    const int n = ++counters[level] + (level == 0 ? o.listStart - 1 : 0);
                    const QString label = o.listStyle == 5 ? letters(n, false) : o.listStyle == 6 ? letters(n, true)
                                        : o.listStyle == 7 ? roman(n, false) : o.listStyle == 8 ? roman(n, true) : QString::number(n);
                    chars += label + QStringLiteral(".\t");
                    for (int i = 0; i < label.size() + 2; ++i) looks.append(base);
                } else {
                    bullet = true;
                    marker = o.listStyle == 3 ? QChar(0x25CB) : o.listStyle == 4 ? QChar(0x25A0)
                           : level == 0 ? QChar(0x2022) : level == 1 ? QChar(0x25CB) : QChar(0x25A0);
                }
            }
            for (int i = 0; i < words.size(); ++i) {
                const int at = offset + skip + i;
                Look l = base;
                for (const auto &run : o.runs) {
                    if (at < run.start || at >= run.start + run.length) continue;
                    if (!run.fontFamily.isEmpty()) l.font = font(run.fontFamily);
                    if (run.fontSize > 0) l.size = qMax(1, qRound(points(run.fontSize)));
                    if (run.weight) l.bold = run.weight >= 600;
                    if (run.italic) l.italic = run.italic == 1;
                    if (run.underline) l.underline = run.underline == 1;
                    if (run.strike == 1) struck = true;
                    if (run.baseline) l.baseline = run.baseline;
                    if (run.color.isValid()) l.color = int(colorref(run.color) | 0xFE000000);
                }
                QChar c = words.at(i);
                if (c == QChar(0x2028)) c = QChar(0x0B);
                chars += c;
                looks.append(l);
            }
            const bool last = p == paragraphs.size() - 1;
            if (!last) { chars += QLatin1Char('\r'); looks.append(looks.isEmpty() ? base : looks.last()); }
            const int count = chars.size() - paraStart + (last ? 1 : 0);
            Bytes run;
            run.u32(quint32(count)).u16(level);
            pfRuns += run.b + pf(o, level, bullet, marker, last ? 0 : o.paragraphSpacing);
            offset += line.size() + 1;
        }
        if (struck) warnings.add(QStringLiteral("Struck-through words were written plain"), slide);
        // Character runs: the text and one more, as the format counts them.
        QByteArray cfRuns;
        looks.append(looks.isEmpty() ? base : looks.last());
        for (int i = 0; i < looks.size();) {
            int j = i + 1;
            while (j < looks.size() && looks[j] == looks[i]) ++j;
            Bytes run;
            run.u32(quint32(j - i));
            cfRuns += run.b + cf(looks[i]);
            i = j;
        }
        Bytes header;
        header.u32(quint32(textType));
        return record(0x0F9F, 0, header.b) + record(0x0FA0, 0, utf16(chars)) + record(0x0FA1, 0, pfRuns + cfRuns);
    }

    QByteArray writeText(const SceneObject &o, const QByteArray &placeholder = {}, int textType = 4, const QString &override = {}) {
        if (o.textKind == 1) return writeSnapshot(o, QStringLiteral("An equation was written as a picture of itself"));
        // OmaShow draws a text box's words only: no fill, outline or box shadow.
        Opt opt;
        common(opt, o);
        noFill(opt);
        noLine(opt);
        opt.set(0x81, 0).set(0x82, 0).set(0x83, 0).set(0x84, 0).set(0x85, 0)
            .set(0x87, quint32(o.verticalAlign == 1 ? 1 : o.verticalAlign == 2 ? 2 : 0));
        opt.flags(0xBF, 0x2, o.textFit == 3);
        if (o.textFit == 1) warnings.add(QStringLiteral("Text shrunk to fit its box was written at its full size"), slide);
        if (o.textGlowRadius > 0 && o.textGlowColor.alpha() > 0) warnings.add(QStringLiteral("Glowing text was written without its glow"), slide);
        QByteArray data = placeholder + click(o);
        return spContainer(placeholder.isEmpty() ? 202 : 1, 0, opt, anchor(o.rect, o.rotation), data, textbox(o, textType, override));
    }

    QByteArray writeMedia(const SceneObject &o) {
        warnings.add(QStringLiteral("A film or sound was written as its first picture"), slide);
        QImage poster = MediaAsset::frameAt(o, qMax(0.0, o.mediaTrimStart));
        if (poster.isNull() || o.rect.isEmpty()) return {};
        Opt opt;
        common(opt, o);
        opt.blip(0x104, addImage(poster));
        noFill(opt);
        noLine(opt);
        return spContainer(75, 0, opt, anchor(o.rect, o.rotation), click(o));
    }

    QByteArray writeObjects(const QVector<SceneObject> &objects) {
        QByteArray out;
        for (const auto &o : objects) {
            if (canceled()) return {};
            switch (o.type) {
            case ObjectType::Rect: out += writeShape(o); break;
            case ObjectType::Text:
                // FILL-UI-1: a text box's own box goes as a shape under its words.
                for (const auto &part : FillStyle::split(o)) out += part.type == ObjectType::Rect ? writeShape(part) : writeText(part);
                break;
            case ObjectType::Image: out += writePicture(o); break;
            case ObjectType::Media: out += writeMedia(o); break;
            case ObjectType::Table: out += writeSnapshot(o, QStringLiteral("A table was written as a picture of itself")); break;
            case ObjectType::Chart: out += writeSnapshot(o, QStringLiteral("A chart was written as a picture of itself")); break;
            }
        }
        return out;
    }

    QByteArray background(const QColor &c) {
        Opt opt;
        opt.set(0x180, 0).set(0x181, colorref(c));
        opt.flags(0x1BF, 0x10, true);
        noLine(opt);
        return spContainer(1, 0x400, opt, {});
    }

    // A drawing: the group every shape sits in, its shapes, then the background.
    QByteArray ppDrawing(const std::function<QByteArray()> &shapes, const std::function<QByteArray()> &backgroundShape) {
        beginDrawing();
        Bytes fspgr;
        fspgr.u32(0).u32(0).u32(0).u32(0);
        Bytes fsp;
        fsp.u32(newSpid()).u32(0x5);
        const QByteArray group = container(0xF004, record(0xF009, 0, fspgr.b, 1) + record(0xF00A, 0, fsp.b, 2));
        const QByteArray inside = shapes();
        const QByteArray back = backgroundShape ? backgroundShape() : QByteArray();
        Bytes fdg;
        fdg.u32(quint32(dgShapes)).u32(spid - 1);
        const QByteArray dgc = container(0xF002, record(0xF008, dg, fdg.b) + container(0xF003, group + inside) + back);
        return container(0x040C, dgc);
    }

    // ---- masters

    // The look a master gives new text of one kind.
    QByteArray masterStyle(int instance, const SceneObject *sample, int levels) {
        Bytes d;
        d.u16(levels);
        SceneObject o;
        if (sample) o = *sample;
        else { o.fontFamily = doc.theme.fonts.value(QStringLiteral("body"), QStringLiteral("Arial")); o.fontSize = (instance == 0 ? 44 : 32) * 8 / k; }
        for (int level = 0; level < levels; ++level) {
            Look l;
            l.font = font(o.fontFamily);
            const qreal pt = points(o.fontSize) * (level == 0 ? 1.0 : qMax(0.6, 1.0 - 0.1 * level));
            l.size = qMax(1, qRound(pt));
            l.color = int(colorref(o.textColor.isValid() ? o.textColor : QColor(Qt::black)) | 0xFE000000);
            l.bold = o.fontWeight >= 600;
            l.italic = o.italic;
            const bool bullet = instance == 1 && (sample ? sample->listStyle > 0 : true);
            SceneObject para = o;
            para.fontSize = o.fontSize;
            d.raw(pf(para, level, bullet, level == 0 ? QChar(0x2022) : level == 1 ? QChar(0x2013) : QChar(0x2022), 0));
            d.raw(cf(l));
        }
        return record(0x0FA3, instance, d.b);
    }

    QByteArray colorScheme(const QColor &background, const QColor &text) const {
        Bytes d;
        const QColor accent = doc.theme.colors.value(QStringLiteral("accent"), QColor(0x00, 0x99, 0xCC));
        for (const QColor &c : {background, text, QColor(0x80, 0x80, 0x80), text, QColor(0xBB, 0xE0, 0xE3), accent, QColor(0x33, 0x99, 0xFF), QColor(0x99, 0x33, 0xCC)})
            d.u32(colorref(c));
        return record(0x07F0, 1, d.b);
    }

    QByteArray placeholderAtom(int position, int placement) const {
        Bytes d;
        d.u32(quint32(position)).u8(placement).u8(0).u16(0);
        return record(0x0BC3, 0, d.b);
    }

    QByteArray writeMaster(const Master &m) {
        slide = 0;
        const QColor back = doc.theme.colors.value(m.backgroundToken, m.background);
        const QColor fore = doc.theme.colors.value(QStringLiteral("foreground"), QColor(Qt::black));
        // Title and body as the master's first layout has them.
        const SceneObject *title = nullptr, *body = nullptr;
        for (const auto &layout : doc.layouts) {
            if (layout.masterId != m.id) continue;
            for (const auto &p : layout.placeholders) {
                if (p.id == QLatin1String("title") && !title) title = &p;
                if (p.id == QLatin1String("body") && !body) body = &p;
            }
            if (title && body) break;
        }
        Bytes atom;
        atom.u32(1).u8(1).u8(2).u8(0).u8(0).u8(0).u8(0).u8(0).u8(0).u32(0).u32(0).u16(0).u16(0);
        QByteArray out = record(0x03EF, 0, atom.b, 2);
        out += colorScheme(back, fore);
        out += masterStyle(0, title, 1) + masterStyle(1, body, 5) + masterStyle(2, nullptr, 5) + masterStyle(4, nullptr, 5);
        QVector<SceneObject> objects;
        for (const auto &o : m.objects) objects.append(Design::themed(doc.theme, o));
        const auto shapes = [&]() {
            QByteArray s = writeObjects(objects);
            const QRectF page(QPointF(0, 0), doc.size);
            SceneObject t;
            if (title) t = *title;
            else { t.rect = QRectF(page.width() * .05, page.height() * .04, page.width() * .9, page.height() * .17); t.fontSize = 44 * 8 / k; t.textColor = fore; }
            t.fillStyle = 5; t.strokeWidth = 0; t.shadowEnabled = false; t.listStyle = 0; t.linkKind = 0; t.hidden = false;
            s += writeText(t, placeholderAtom(0, 1), 0, QStringLiteral("Click to edit the title text format"));
            SceneObject b;
            if (body) b = *body;
            else { b.rect = QRectF(page.width() * .05, page.height() * .25, page.width() * .9, page.height() * .65); b.fontSize = 32 * 8 / k; b.textColor = fore; b.listStyle = 1; }
            b.fillStyle = 5; b.strokeWidth = 0; b.shadowEnabled = false; b.linkKind = 0; b.hidden = false;
            if (b.listStyle == 0) b.listStyle = 1;
            s += writeText(b, placeholderAtom(1, 2), 1, QStringLiteral("Click to edit the outline text format\n\tSecond Outline Level\n\t\tThird Outline Level"));
            return s;
        };
        out += ppDrawing(shapes, [&]() { return background(back); });
        return container(0x03F8, out);
    }

    // ---- slides

    QByteArray transition(int index, const Slide &s) {
        const int kind = Presentation::transitionKind(doc, index);
        const qreal seconds = Presentation::transitionSeconds(doc, index);
        int effect = 0, direction = 0;
        const int toward = s.transitionDirection == 2 ? 1 : s.transitionDirection == 1 ? 2 : s.transitionDirection == 3 ? 3 : 0;
        switch (kind) {
        case Presentation::Cut: effect = 0; break;
        case Presentation::Push: effect = 20; direction = toward; break;
        case Presentation::Cover: effect = 4; direction = toward; break;
        case Presentation::Uncover: effect = 7; direction = toward; break;
        case Presentation::FadeThroughBlack: effect = 6; direction = 1; break;
        case Presentation::Zoom: effect = 11; break;
        case Presentation::Morph:
        case Presentation::Whirl:
            warnings.add(QStringLiteral("A transition PowerPoint 97–2003 doesn't have was written as a fade"), index + 1);
            effect = 6;
            break;
        default: effect = 6; break;
        }
        quint16 flags = 0x1;
        if (s.skipped) flags |= 0x4;
        if (s.advanceAfter >= 0) flags |= 0x400;
        Bytes d;
        d.i32(s.advanceAfter >= 0 ? qint32(qRound(s.advanceAfter * 1000)) : 0).u32(0).u8(direction).u8(effect).u16(flags)
            .u8(seconds < 0.6 ? 2 : seconds < 0.9 ? 1 : 0).u8(0).u8(0).u8(0);
        return record(0x03F9, 0, d.b);
    }

    QByteArray comments(const Slide &s) {
        QByteArray all;
        int n = 0;
        for (const auto &c : doc.comments) {
            if (c.slideId != s.id) continue;
            QPointF where(10, 10);
            if (const auto *o = s.find(c.objectId)) where = o->rect.topLeft();
            QString text = c.text;
            if (!c.parentId.isEmpty()) text = QStringLiteral("Re: ") + text;
            if (c.resolved) text += QStringLiteral(" [resolved]");
            text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
            const QString author = c.author.isEmpty() ? QStringLiteral("Unknown") : c.author;
            QString initials;
            for (const auto &part : author.split(QLatin1Char(' '), Qt::SkipEmptyParts)) initials += part.at(0).toUpper();
            QDateTime when = QDateTime::fromString(c.created, Qt::ISODate);
            if (!when.isValid()) when = QDateTime(QDate(2000, 1, 1), QTime(0, 0));
            Bytes atom;
            atom.u32(quint32(n++)).u16(when.date().year()).u16(when.date().month()).u16(when.date().dayOfWeek() % 7).u16(when.date().day())
                .u16(when.time().hour()).u16(when.time().minute()).u16(when.time().second()).u16(when.time().msec())
                .i32(mu(where.x())).i32(mu(where.y()));
            all += container(0x2EE0, cstring(author, 0) + cstring(text, 1) + cstring(initials.left(8), 2) + record(0x2EE1, 0, atom.b));
        }
        if (all.isEmpty()) return {};
        return container(0x1388, container(0x138A, cstring(QStringLiteral("___PPT10"), 0) + record(0x138B, 0, all)));
    }

    QByteArray writeSlide(int index, quint32 masterRef, quint32 notesRef) {
        const Slide &s = doc.slides[index];
        slide = index + 1;
        const Slide resolved = Design::resolve(doc, index);
        const auto *layout = Design::layout(doc, s.layoutId);
        QColor masterBack = Qt::white;
        for (const auto &m : doc.masters)
            if (layout && m.id == layout->masterId) masterBack = doc.theme.colors.value(m.backgroundToken, m.background);
        const bool ownBackground = s.backgroundOverride || resolved.background != masterBack;
        quint16 flags = 0x2 | (s.showMasterObjects ? 0x1 : 0) | (ownBackground ? 0 : 0x4);
        Bytes atom;
        atom.u32(0x10).u32(0).u32(0).u32(masterRef).u32(notesRef).u16(flags).u16(0);
        QByteArray out = record(0x03EF, 0, atom.b, 2);
        out += transition(index, s);
        QVector<SceneObject> objects;
        for (const auto &o : resolved.objects) if (!o.id.startsWith(QLatin1String("@master/"))) objects.append(o);
        out += ppDrawing([&]() { return writeObjects(objects); }, [&]() { return background(resolved.background); });
        out += colorScheme(resolved.background, doc.theme.colors.value(QStringLiteral("foreground"), QColor(Qt::black)));
        out += comments(s);
        if (!s.timeline.steps.isEmpty() || !s.pptxInteractive.isEmpty())
            warnings.add(QStringLiteral("Builds (animations) were left out for now"), slide);
        return container(0x03EE, out);
    }

    // Speaker notes: a page with the slide's picture and the words under it.
    QByteArray writeNotes(quint32 slideIdRef, const QString &notes) {
        Bytes atom;
        atom.u32(slideIdRef).u16(slideIdRef ? 0x7 : 0).u16(0);
        QByteArray out = record(0x03F1, 0, atom.b, 1);
        const auto shapes = [&]() {
            // The notes page is 7.5 by 10 inches, in slide units for anchor().
            const qreal page = 5760.0 / k, side = 4320.0 / k;
            QByteArray s;
            Opt image;
            noFill(image);
            image.flags(0x1FF, 0x8, true);
            image.set(0x1C0, 0);
            const qreal imageW = side * 0.8, imageH = imageW * doc.size.height() / qMax(1.0, doc.size.width());
            s += spContainer(1, 0, image, anchor(QRectF((side - imageW) / 2, page * 0.08, imageW, qMin(imageH, page * 0.4)), 0),
                             placeholderAtom(0, 11));
            SceneObject words;
            words.text = notes;
            words.rect = QRectF(side * 0.1, page * 0.52, side * 0.8, page * 0.4);
            words.fontFamily = doc.theme.fonts.value(QStringLiteral("body"), QStringLiteral("Arial"));
            words.fontSize = 12 * 8 / k;
            words.textColor = Qt::black;
            words.fillStyle = 5;
            words.strokeWidth = 0;
            words.verticalAlign = 0;
            words.textFit = 2;
            s += writeText(words, placeholderAtom(1, 12), 2);
            return s;
        };
        out += ppDrawing(shapes, [&]() { return background(Qt::white); });
        out += colorScheme(Qt::white, Qt::black);
        return container(0x03F0, out);
    }

    // ---- the document

    QByteArray run() {
        if (doc.slides.isEmpty()) return {};
        QVector<Master> masters = doc.masters;
        if (masters.isEmpty()) {
            Master plain;
            plain.id = QStringLiteral("default");
            plain.name = QStringLiteral("Default");
            plain.background = doc.theme.colors.value(QStringLiteral("background"), QColor(Qt::white));
            masters.append(plain);
        }
        const int masterCount = masters.size(), slideCount = doc.slides.size();
        for (int i = 0; i < slideCount; ++i) { slideIdOf.insert(doc.slides[i].id, quint32(256 + i)); slideNumberOf.insert(doc.slides[i].id, i + 1); }
        // Persist ids: the document, its masters, the notes master, slides, then notes.
        const quint32 notesMasterRef = 2 + masterCount;
        const auto masterRef = [&](int i) { return quint32(2 + i); };
        const auto slideRef = [&](int i) { return quint32(3 + masterCount + i); };
        QVector<int> withNotes;
        for (int i = 0; i < slideCount; ++i) if (!doc.slides[i].notes.trimmed().isEmpty()) withNotes.append(i);
        const auto notesRef = [&](int n) { return quint32(3 + masterCount + slideCount + n); };

        // Bodies first, so the document knows its fonts, pictures and links.
        QVector<QByteArray> masterBodies, slideBodies, notesBodies;
        for (int i = 0; i < masterCount; ++i) masterBodies.append(writeMaster(masters[i]));
        const QByteArray notesMaster = writeNotes(0, QStringLiteral("Click to edit the notes"));
        for (int i = 0; i < slideCount; ++i) {
            if (canceled()) return {};
            const auto *layout = Design::layout(doc, doc.slides[i].layoutId);
            int m = 0;
            for (int j = 0; j < masterCount; ++j) if (layout && masters[j].id == layout->masterId) m = j;
            const int n = withNotes.indexOf(i);
            slideBodies.append(writeSlide(i, 0x80000000u + quint32(m), n >= 0 ? quint32(0x100 + n) : 0));
        }
        for (int n = 0; n < withNotes.size(); ++n) {
            slide = withNotes[n] + 1;
            notesBodies.append(writeNotes(slideIdOf.value(doc.slides[withNotes[n]].id), doc.slides[withNotes[n]].notes));
        }
        if (!doc.sections.isEmpty()) warnings.add(QStringLiteral("Sections have no PowerPoint 97–2003 form and were left out"), 0);
        if (fonts.isEmpty()) font(QStringLiteral("Arial"));

        // The document itself.
        Bytes da;
        da.i32(slideSize.width()).i32(slideSize.height()).i32(4320).i32(5760).i32(1).i32(2)
            .u32(notesMasterRef).u32(0).u16(1).u16(slideSize == QSize(5760, 4320) ? 0 : 6).u8(0).u8(0).u8(0).u8(1);
        QByteArray body = record(0x03E9, 0, da.b, 1);
        if (!links.isEmpty()) {
            Bytes seed;
            seed.u32(quint32(links.size() + 1));
            QByteArray list = record(0x040A, 0, seed.b);
            for (int i = 0; i < links.size(); ++i) {
                Bytes id;
                id.u32(quint32(i + 1));
                QByteArray one = record(0x0FD3, 0, id.b) + cstring(links[i].target.isEmpty() ? links[i].location : links[i].target, 0);
                if (!links[i].target.isEmpty()) one += cstring(links[i].target, 1);
                if (!links[i].location.isEmpty()) one += cstring(links[i].location, 3);
                list += container(0x0FD7, one);
            }
            body += container(0x0409, list);
        }
        QByteArray fontRecords;
        for (int i = 0; i < fonts.size(); ++i) {
            QByteArray face = utf16(fonts[i]);
            face.resize(64, '\0');
            Bytes rest;
            rest.u8(0).u8(0).u8(4).u8(0x22);
            fontRecords += record(0x0FB7, i, face + rest.b);
        }
        Look plain;
        Bytes cfDefault;
        cfDefault.raw(cf(plain));
        SceneObject para;
        para.fontSize = 18 * 8 / k;
        Bytes pfDefault;
        pfDefault.u16(0).raw(pf(para, 0, false, QChar(), 0));
        Bytes siDefault;
        siDefault.u32(0);
        body += container(0x03F2, container(0x07D5, fontRecords) + record(0x0FA4, 0, cfDefault.b) + record(0x0FA5, 0, pfDefault.b) +
                                      record(0x0FA9, 0, siDefault.b) + masterStyle(4, nullptr, 5));

        // Pictures and shape ids for every drawing.
        Bytes dggBlock;
        dggBlock.u32(quint32(clusters.size() + 1) * 1024).u32(quint32(clusters.size() + 1)).u32(quint32(shapesSaved)).u32(quint32(drawings));
        for (const auto &c : clusters) dggBlock.u32(c.dg).u32(c.used);
        QByteArray dgg = record(0xF006, 0, dggBlock.b);
        if (pictureCount > 0) dgg += container(0xF001, store, pictureCount);
        Opt defaults;
        defaults.set(0x181, 0xFFFFFF).set(0x1C0, 0).flags(0x1BF, 0x10, true).flags(0x1FF, 0x8, true);
        dgg += defaults.record();
        body += container(0x040B, container(0xF000, dgg));

        const auto persistAtom = [](quint32 ref, quint32 id) {
            Bytes d;
            d.u32(ref).u32(0).i32(0).u32(id).u32(0);
            return record(0x03F3, 0, d.b);
        };
        QByteArray list;
        for (int i = 0; i < masterCount; ++i) list += persistAtom(masterRef(i), 0x80000000u + quint32(i));
        body += container(0x0FF0, list, 1);
        list.clear();
        for (int i = 0; i < slideCount; ++i) list += persistAtom(slideRef(i), quint32(256 + i));
        body += container(0x0FF0, list, 0);
        if (!withNotes.isEmpty()) {
            list.clear();
            for (int n = 0; n < withNotes.size(); ++n) list += persistAtom(notesRef(n), quint32(0x100 + n));
            body += container(0x0FF0, list, 2);
        }
        if (!doc.shows.isEmpty()) {
            QByteArray shows;
            for (const auto &show : doc.shows) {
                Bytes ids;
                for (const auto &id : show.slideIds) if (slideIdOf.contains(id)) ids.u32(slideIdOf.value(id));
                shows += container(0x0411, cstring(show.name, 0) + record(0x0412, 0, ids.b));
            }
            body += container(0x0410, shows);
        }
        body += record(0x03EA, 0, {});

        // The stream: every part, where it starts, then the edit that names them.
        QByteArray stream;
        QMap<quint32, quint32> offsets;
        const auto put = [&](quint32 ref, const QByteArray &part) { offsets.insert(ref, quint32(stream.size())); stream += part; };
        put(1, container(0x03E8, body));
        for (int i = 0; i < masterCount; ++i) put(masterRef(i), masterBodies[i]);
        put(notesMasterRef, notesMaster);
        for (int i = 0; i < slideCount; ++i) put(slideRef(i), slideBodies[i]);
        for (int n = 0; n < notesBodies.size(); ++n) put(notesRef(n), notesBodies[n]);
        const quint32 last = offsets.lastKey();
        Bytes dir;
        dir.u32(1 | (quint32(offsets.size()) << 20));
        for (auto it = offsets.cbegin(); it != offsets.cend(); ++it) dir.u32(it.value());
        const quint32 dirAt = quint32(stream.size());
        stream += record(0x1772, 0, dir.b);
        const quint32 editAt = quint32(stream.size());
        Bytes edit;
        edit.u32(256).u16(0).u8(0).u8(3).u32(0).u32(dirAt).u32(1).u32(last + 1).u16(1).u16(0);
        stream += record(0x0FF5, 0, edit.b);

        const QByteArray name = "OmaShow";
        Bytes user;
        user.u32(0x14).u32(0xE391C05F).u32(editAt).u16(name.size()).u16(0x03F4).u8(3).u8(0).u16(0).raw(name).u32(8).raw(utf16(QString::fromLatin1(name)));
        const QByteArray currentUser = record(0x0FF6, 0, user.b);

        QVector<Cfb::Entry> streams;
        streams.append({QStringLiteral("PowerPoint Document"), stream});
        streams.append({QStringLiteral("Current User"), currentUser});
        if (!pictures.isEmpty()) streams.append({QStringLiteral("Pictures"), pictures});
        if (!links.isEmpty()) {
            QVector<QPair<QString, QString>> places;
            for (const auto &l : links) places.append({l.target, l.location});
            streams.append({QStringLiteral("\005DocumentSummaryInformation"), summaryInformation(places)});
        }
        static const unsigned char clsid[16] = {0x10, 0x8D, 0x81, 0x64, 0x9B, 0x4F, 0xCF, 0x11, 0x86, 0xEA, 0x00, 0xAA, 0x00, 0xB9, 0x29, 0xE8};
        return Cfb::write(streams, QByteArray(reinterpret_cast<const char *>(clsid), 16));
    }
};

} // namespace

namespace PptWriter {

QByteArray bytes(const Document &document, QStringList *warnings, const std::shared_ptr<Workers::Job> &job) {
    // Office fonts swapped for free matches on import are named again, as on PowerPoint save.
    Document deck = document;
    FontSwap::revert(deck);
    Writer writer(deck);
    writer.job = job;
    const auto out = writer.run();
    if (warnings) *warnings = writer.warnings.lines();
    return out;
}

} // namespace PptWriter

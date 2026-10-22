#include "io/keynotewriter.h"

#include "anim/presentation.h"
#include "core/design.h"
#include "core/fontswap.h"
#include "core/imageasset.h"
#include "core/mediaasset.h"
#include "core/shape.h"
#include "io/zip.h"
#include "render/scenerenderer.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QUuid>
#include <QtEndian>
#include <QtMath>

#include <cmath>
#include <cstring>
#include <functional>

namespace {

// ---- protocol buffers

// What a message refers to: other objects (strongly or weakly) and data files.
struct Refs {
    QVector<quint64> objects;
    QSet<quint64> strong;
    QVector<quint64> datas;
};

// A message being written. Embedding one message in another carries its
// references up, so an object knows everything it points at when it is put.
struct Pb {
    QByteArray b;
    std::shared_ptr<Refs> refs = std::make_shared<Refs>();
    int count = 0;   // fields written at this level: a style's override count

    void varint(quint64 v) {
        while (v >= 0x80) { b.append(char(quint8(v | 0x80))); v >>= 7; }
        b.append(char(quint8(v)));
    }
    void tag(int field, int wire) { varint((quint64(field) << 3) | quint64(wire)); ++count; }
    Pb &u(int f, quint64 v) { tag(f, 0); varint(v); return *this; }
    Pb &e(int f, int v) { tag(f, 0); varint(quint64(qint64(v))); return *this; }   // int32 and enums
    Pb &flag(int f, bool v) { return u(f, v ? 1 : 0); }
    Pb &f32(int f, double v) {
        tag(f, 5);
        const float x = float(v);
        quint32 bits;
        std::memcpy(&bits, &x, 4);
        char d[4];
        qToLittleEndian<quint32>(bits, d);
        b.append(d, 4);
        return *this;
    }
    Pb &f64(int f, double v) {
        tag(f, 1);
        quint64 bits;
        std::memcpy(&bits, &v, 8);
        char d[8];
        qToLittleEndian<quint64>(bits, d);
        b.append(d, 8);
        return *this;
    }
    Pb &bytes(int f, const QByteArray &d) { tag(f, 2); varint(quint64(d.size())); b += d; return *this; }
    Pb &str(int f, const QString &s) { return bytes(f, s.toUtf8()); }
    void absorb(const Pb &m) {
        if (m.refs == refs) return;
        refs->objects += m.refs->objects;
        refs->strong.unite(m.refs->strong);
        refs->datas += m.refs->datas;
    }
    Pb &msg(int f, const Pb &m) { absorb(m); return bytes(f, m.b); }
    // Another message's fields at this level, as if written here.
    Pb &merge(const Pb &m) { absorb(m); b += m.b; count += m.count; return *this; }
    Pb &ref(int f, quint64 id, bool weak = false) {
        refs->objects.append(id);
        if (!weak) refs->strong.insert(id);
        Pb r;
        r.u(1, id);
        return bytes(f, r.b);
    }
    Pb &data(int f, quint64 id) {
        refs->datas.append(id);
        Pb r;
        r.u(1, id);
        return bytes(f, r.b);
    }
    Pb &packed(int f, const QVector<quint64> &values) {
        if (values.isEmpty()) return *this;
        Pb p;
        for (quint64 v : values) p.varint(v);
        return bytes(f, p.b);
    }
};

Pb color(const QColor &c) {
    const QColor rgb = c.toRgb();
    Pb p;
    p.e(1, 1).f32(3, rgb.redF()).f32(4, rgb.greenF()).f32(5, rgb.blueF()).f32(6, rgb.alphaF()).e(12, 1);
    return p;
}
Pb point(const QPointF &at) { Pb p; p.f32(1, at.x()).f32(2, at.y()); return p; }
Pb size(const QSizeF &s) { Pb p; p.f32(1, s.width()).f32(2, s.height()); return p; }
Pb solidFill(const QColor &c) { Pb p; p.msg(1, color(c)); return p; }

// An outline as Keynote keeps it: in its own box, from `origin`.
Pb path(const QPainterPath &p, const QPointF &origin, bool closed) {
    Pb out;
    const auto element = [&](int type, const QVector<QPointF> &points) {
        Pb e;
        e.e(1, type);
        for (const auto &pt : points) e.msg(2, point(pt - origin));
        out.msg(1, e);
    };
    bool open = false;
    QPointF start;
    const int n = p.elementCount();
    for (int i = 0; i < n;) {
        const auto el = p.elementAt(i);
        if (el.isMoveTo()) {
            if (open && closed) element(5, {});
            element(1, {QPointF(el)});
            start = QPointF(el);
            open = true;
            ++i;
        } else if (el.isLineTo()) {
            // Closing does the last line back, as Keynote writes it.
            const bool last = i + 1 == n || p.elementAt(i + 1).isMoveTo();
            if (!(closed && last && QPointF(el) == start)) element(2, {QPointF(el)});
            ++i;
        } else if (el.isCurveTo() && i + 2 < n) {
            element(4, {QPointF(el), QPointF(p.elementAt(i + 1)), QPointF(p.elementAt(i + 2))});
            i += 3;
        } else ++i;
    }
    if (open && closed) element(5, {});
    return out;
}

Pb pathSource(const QPainterPath &p, const QRectF &box, bool closed) {
    Pb bezier;
    bezier.msg(2, size(box.size())).msg(3, path(p, box.topLeft(), closed));
    Pb source;
    source.flag(1, false).flag(2, false).msg(5, bezier);
    return source;
}

QPainterPath rectPath(const QRectF &r) { QPainterPath p; p.addRect(r); return p; }

// The no-shadow, no-outline forms a full style carries.
Pb noShadow() {
    Pb s;
    s.msg(1, color(Qt::black)).f32(2, 315).f32(3, 5).e(4, 1).f32(5, 1).flag(6, false).e(7, 0);
    return s;
}
Pb strokePattern(int type, const QVector<qreal> &dashes = {}) {
    Pb p;
    p.e(1, type).f32(2, 0).u(3, quint64(dashes.size()));
    for (int i = 0; i < qMax(6, int(dashes.size())); ++i) p.f32(4, i < dashes.size() ? dashes[i] : 0.0);
    return p;
}
Pb noStroke() {
    Pb s;
    s.msg(1, color(Qt::black)).f32(2, 1).e(3, 0).e(4, 0).f32(5, 4).msg(6, strokePattern(2));
    return s;
}

QByteArray encoded(const QImage &image, const char *format, int quality = -1) {
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format, quality);
    return out;
}

// A small binary property list: a dictionary of strings and booleans, as
// Keynote reads Metadata/Properties.plist.
QByteArray binaryPlist(const QVector<QPair<QString, QVariant>> &entries) {
    QVector<QByteArray> objects;
    const auto string = [](const QByteArray &s) {
        QByteArray o;
        if (s.size() < 15) o.append(char(0x50 | s.size()));
        else {
            o.append(char(0x5F));
            if (s.size() < 256) { o.append(char(0x10)); o.append(char(s.size())); }
            else { o.append(char(0x11)); o.append(char(s.size() >> 8)); o.append(char(s.size() & 0xFF)); }
        }
        return o + s;
    };
    const int n = entries.size();
    QByteArray dict;
    dict.append(char(0xD0 | n));
    for (int i = 0; i < n; ++i) dict.append(char(1 + i));
    for (int i = 0; i < n; ++i) dict.append(char(1 + n + i));
    objects.append(dict);
    for (const auto &e : entries) objects.append(string(e.first.toLatin1()));
    for (const auto &e : entries)
        objects.append(e.second.typeId() == QMetaType::Bool ? QByteArray(1, e.second.toBool() ? char(0x09) : char(0x08))
                                                            : string(e.second.toString().toLatin1()));
    QByteArray out = "bplist00";
    QVector<int> offsets;
    for (const auto &o : objects) { offsets.append(out.size()); out += o; }
    const int table = out.size();
    for (int at : offsets) { out.append(char(at >> 8)); out.append(char(at & 0xFF)); }
    QByteArray trailer(32, '\0');
    trailer[6] = 2;   // offsets are two bytes
    trailer[7] = 1;   // object references one
    qToBigEndian<quint64>(quint64(objects.size()), trailer.data() + 8);
    qToBigEndian<quint64>(0, trailer.data() + 16);
    qToBigEndian<quint64>(quint64(table), trailer.data() + 24);
    return out + trailer;
}

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

// Object types, as Keynote's registry numbers them.
enum Type {
    KNDocument = 1, KNShow = 2, KNSlideNode = 4, KNSlide = 5, KNPlaceholder = 7, KNSlideStyle = 9, KNTheme = 10,
    KNNote = 15, KNClassicStylesheetRecord = 19, KNSoundtrack = 21, TSKAnnotationAuthorStorage = 213,
    TSKCustomFormatList = 222, TSSStylesheet = 401, TSAFunctionBrowserState = 601, TSWPStorage = 2001,
    TSWPShapeInfo = 2011, TSWPCharacterStyle = 2021, TSWPParagraphStyle = 2022, TSWPListStyle = 2023,
    TSWPShapeStyle = 2025, TSDImage = 3005, TSDMask = 3006, TSDMediaStyle = 3016,
    TSDGuideStorage = 3047, TSDStandinCaption = 3097, TSCECalculationEngine = 4000,
    TSCENamedReferenceManager = 4003, TSCETrackedReferenceStore = 4004, TSPPackageMetadata = 11006,
    TSPDocumentMetadata = 11011,
};

// ---- the writer

struct Writer {
    const Document &doc;
    std::shared_ptr<Workers::Job> job;
    Warnings warnings;
    int slide = 0;   // 1-based while a slide is written

    struct Obj { quint64 id = 0; int type = 0; QVector<quint64> version; Pb body; };
    struct Part { QString preferred, locator; quint64 root = 0; QVector<Obj> objects; };
    QVector<Part> parts;
    QHash<quint64, int> home;   // object → the part it is in
    quint64 next = 100;

    struct DataFile { quint64 id = 0; QString preferred, file; QByteArray bytes, digest; QSize pixels; };
    QVector<DataFile> datas;
    QHash<QByteArray, quint64> dataByDigest;
    QByteArray seed;   // makes the deck's UUIDs its own and the same each save

    // The stylesheet and its named styles.
    int sheetPart = -1;
    quint64 sheet = 0;
    QVector<quint64> styles;
    QVector<QPair<QString, quint64>> named;
    QMap<quint64, QVector<quint64>> variations;
    QHash<QByteArray, quint64> styleCache;
    QHash<QByteArray, quint64> partStyleCache;   // character looks, kept in each slide's own part
    quint64 textboxStyle = 0, shapeStyle = 0, lineStyle = 0, drawingLineStyle = 0, imageStyle = 0, movieStyle = 0;
    quint64 titlePara = 0, bodyPara = 0, numberPara = 0, notesPara = 0, defaultPara = 0;
    quint64 noneList = 0, bulletList = 0, numberList = 0, notesList = 0, charNull = 0, charLink = 0;

    struct KeyMaster { QString layoutId; bool objects = true; quint64 id = 0, node = 0, style = 0; QColor background; };
    QVector<KeyMaster> keyMasters;

    explicit Writer(const Document &d) : doc(d) {
        QByteArray identity = "OmaShow";
        for (const auto &s : doc.slides) identity += s.id.toUtf8();
        seed = QCryptographicHash::hash(identity, QCryptographicHash::Sha1);
    }

    bool canceled() const { return job && job->canceled; }
    quint64 newId() { return next++; }

    int newPart(const QString &preferred, const QString &locator = {}) {
        Part p;
        p.preferred = preferred;
        p.locator = locator;
        parts.append(p);
        return int(parts.size()) - 1;
    }
    void put(int part, quint64 id, int type, const Pb &body, const QVector<quint64> &version = {1, 0, 5}) {
        parts[part].objects.append({id, type, version, body});
        home.insert(id, part);
    }
    // The object a part opens with, which other parts name by the part alone.
    void putRoot(int part, quint64 id, int type, const Pb &body) {
        parts[part].objects.prepend({id, type, {1, 0, 5}, body});
        parts[part].root = id;
        home.insert(id, part);
    }

    QUuid uuid(quint64 id) const {
        return QUuid::createUuidV5(QUuid(QStringLiteral("{9b1d3f0e-5a2c-4c7e-8d31-6f0a2b4e7c91}")), seed + QByteArray::number(id));
    }

    quint64 addData(const QByteArray &bytes, const QString &stem, const QString &suffix, const QSize &pixels = {}) {
        const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1);
        if (auto it = dataByDigest.find(digest); it != dataByDigest.end()) return *it;
        DataFile f;
        f.id = newId();
        f.preferred = stem + QLatin1Char('.') + suffix;
        f.file = stem + QLatin1Char('-') + QString::number(f.id) + QLatin1Char('.') + suffix;
        f.bytes = bytes;
        f.digest = digest;
        f.pixels = pixels;
        datas.append(f);
        dataByDigest.insert(digest, f.id);
        return f.id;
    }
    quint64 addImage(const QImage &image) {
        return addData(encoded(image, "PNG"), QStringLiteral("image"), QStringLiteral("png"), image.size());
    }

    // ---- styles

    // A style's own StyleArchive: what it is called, or what it varies.
    Pb styleHead(const QString &name, const QString &identifier, quint64 parent) {
        Pb head;
        if (!name.isEmpty()) head.str(1, name);
        if (!identifier.isEmpty()) head.str(2, identifier);
        if (parent) head.ref(3, parent).flag(4, true);
        head.ref(5, sheet, true);
        return head;
    }

    // A style in the stylesheet. Variations of one parent that come out the
    // same are one style.
    quint64 addStyle(int type, const std::function<Pb(const Pb &head)> &build, const QByteArray &look, quint64 parent,
                     const QString &name = {}, const QString &identifier = {}) {
        QByteArray key;
        if (parent && identifier.isEmpty()) {
            key = QByteArray::number(type) + ':' + QByteArray::number(parent) + ':' + look;
            if (auto it = styleCache.find(key); it != styleCache.end()) return *it;
        }
        const quint64 id = newId();
        put(sheetPart, id, type, build(styleHead(name, identifier, parent)));
        styles.append(id);
        if (!identifier.isEmpty()) named.append({identifier, id});
        if (parent) variations[parent].append(id);
        if (!key.isEmpty()) styleCache.insert(key, id);
        return id;
    }

    quint64 paragraphStyle(const Pb &chars, const Pb &para, quint64 parent, const QString &name = {}, const QString &identifier = {}) {
        return addStyle(TSWPParagraphStyle, [&](const Pb &head) {
            Pb s;
            s.msg(1, head).u(10, quint64(chars.count + para.count)).msg(11, chars).msg(12, para);
            return s;
        }, chars.b + '|' + para.b, parent, name, identifier);
    }

    // A shape style: Keynote's drawing properties under its text ones.
    quint64 shapeStyleOf(const Pb &drawing, const Pb &text, quint64 parent, const QString &identifier = {}) {
        const int total = drawing.count + text.count;
        return addStyle(TSWPShapeStyle, [&](const Pb &head) {
            Pb inner;
            inner.msg(1, head).u(10, quint64(total)).msg(11, drawing);
            Pb s;
            s.msg(1, inner).u(10, quint64(total)).msg(11, text);
            return s;
        }, drawing.b + '|' + text.b, parent, {}, identifier);
    }

    quint64 mediaStyleOf(const Pb &props, quint64 parent, const QString &identifier = {}) {
        return addStyle(TSDMediaStyle, [&](const Pb &head) {
            Pb s;
            s.msg(1, head).u(10, quint64(props.count)).msg(11, props);
            return s;
        }, props.b, parent, {}, identifier);
    }

    // A list style; `levels` fills each of its nine levels.
    quint64 listStyleOf(int label, const QString &marker, int numbers, qreal step, qreal gap, qreal scale, quint64 parent,
                        const QString &name = {}, const QString &identifier = {}, bool nullFonts = false) {
        Pb props;
        int fields = 0;
        for (int i = 0; i < 9; ++i) props.e(11, label);
        ++fields;
        for (int i = 0; i < 9; ++i) props.f32(12, gap);
        for (int i = 0; i < 9; ++i) props.f32(13, step * i);
        fields += 2;
        for (int i = 0; i < 9; ++i) { Pb g; g.f32(1, scale).f32(2, 0).flag(3, true); props.msg(14, g); }
        ++fields;
        if (label == 3) { for (int i = 0; i < 9; ++i) props.e(15, numbers); for (int i = 0; i < 9; ++i) props.flag(25, false); fields += 2; }
        if (label == 2) { for (int i = 0; i < 9; ++i) props.str(16, marker.isEmpty() ? QStringLiteral("•") : marker); ++fields; }
        if (nullFonts) { props.flag(20, true).flag(22, true); fields += 2; }
        return addStyle(TSWPListStyle, [&](const Pb &head) {
            Pb s;
            s.msg(1, head).u(10, quint64(fields)).merge(props);
            return s;
        }, props.b, parent, name, identifier);
    }

    // Every property of a character style, as a theme's own styles carry them.
    Pb fullChars(const QString &font, qreal sizePt, bool bold, const QColor &c, const QString &language) const {
        Pb p;
        p.flag(25, true).f32(14, 0).flag(1, bold).e(13, 0).flag(41, false).flag(38, true).flag(36, true).msg(7, color(c))
            .flag(33, true).str(5, font).f32(3, sizePt).flag(40, false).flag(2, false).f32(15, 0);
        if (language.isEmpty()) p.flag(8, true); else p.str(9, language);
        p.e(16, 1).flag(20, true).e(12, 0).flag(22, true).f32(24, 1).e(10, 0).f32(27, 0).msg(46, solidFill(c)).flag(47, false)
            .flag(43, true).e(11, 0).flag(28, true).f32(30, 1).flag(31, false).flag(32, false).e(35, -1);
        return p;
    }
    Pb fullPara(int alignment, qreal tabs, qreal spacing, qreal before, quint64 list, bool outline) const {
        Pb lines;
        if (spacing > 0) lines.f32(2, spacing);
        Pb p;
        p.e(1, alignment).str(3, QStringLiteral(".")).f32(4, tabs).e(15, 0).flag(5, true).f32(7, 0).flag(41, true)
            .msg(17, point({0, 0})).flag(8, true).flag(9, false).flag(10, false).f32(11, 0).msg(13, lines).ref(40, list)
            .u(27, outline ? 0 : 0xFFFFFFFFu).e(28, 0).flag(14, false).f32(19, 0).f32(18, 1).flag(43, outline).flag(44, false)
            .f32(20, 0).f32(21, before).flag(31, true).msg(25, Pb()).flag(26, true);
        return p;
    }
    Pb fullShape(const Pb &fill, const Pb &stroke, const Pb &shadow) const {
        Pb p;
        p.msg(1, fill).msg(6, Pb()).f32(3, 1).msg(5, Pb()).msg(4, shadow).msg(2, stroke).msg(7, Pb());
        return p;
    }
    Pb fullText(quint64 para) const {
        Pb columns, equal;
        equal.u(1, 1);
        columns.msg(1, equal);
        Pb padding;
        padding.f32(1, 4).f32(2, 4).f32(3, 4).f32(4, 4);
        Pb p;
        p.msg(4, columns).u(7, 0).msg(6, padding).ref(10, para).flag(1, false).e(2, 1).flag(11, false);
        return p;
    }

    QString font(const QString &family) const { return family.isEmpty() ? QStringLiteral("Helvetica Neue") : family; }

    // The theme's own styles, which every object varies.
    void theme(const QSizeF &page) {
        const QColor fore = doc.theme.colors.value(QStringLiteral("foreground"), QColor(Qt::black));
        const QColor accent = doc.theme.colors.value(QStringLiteral("accent"), QColor(0x27, 0xC2, 0xFF));
        const QString heading = font(doc.theme.fonts.value(QStringLiteral("heading")));
        const QString body = font(doc.theme.fonts.value(QStringLiteral("body")));
        const QString language = doc.language;
        const qreal unit = page.height() / 1080.0;

        noneList = listStyleOf(0, {}, 0, 0, 0, 1, 0, QStringLiteral("None"), QStringLiteral("text-0-liststyle-None"));
        bulletList = listStyleOf(2, QStringLiteral("•"), 0, 48 * unit, 1, 1.23, 0, QStringLiteral("Bullet"), QStringLiteral("text-1-liststyle-Bullet"), true);
        numberList = listStyleOf(3, {}, 0, 48 * unit, 1, 1, 0, QStringLiteral("Numbered"), QStringLiteral("text-4-liststyle-Numbered"), true);
        notesList = listStyleOf(0, {}, 0, 18, 0, 1, 0, {}, QStringLiteral("note-0-liststyle"));

        defaultPara = paragraphStyle(fullChars(body, 12 * unit, false, fore, {}), fullPara(4, 36 * unit, 0, 0, noneList, false), 0,
                                     QStringLiteral("Free Form"), QStringLiteral("paragraph-style-default"));
        titlePara = paragraphStyle(fullChars(heading, 96 * unit, true, fore, language), fullPara(4, 192 * unit, 0.9, 0, noneList, true), 0,
                                   QStringLiteral("Title"), QStringLiteral("text-0-paragraphstyle-Title"));
        bodyPara = paragraphStyle(fullChars(body, 48 * unit, false, fore, language), fullPara(4, 192 * unit, 0, 0, noneList, true), 0,
                                  QStringLiteral("Body"), QStringLiteral("text-8-paragraphstyle-Body"));
        numberPara = paragraphStyle(fullChars(body, 18 * unit, false, fore, {}), fullPara(2, 46 * unit, 0, 0, noneList, false), 0,
                                    QStringLiteral("Slide Number"), QStringLiteral("text-15-paragraphstyle-Slide Number"));
        notesPara = paragraphStyle(fullChars(QStringLiteral("Helvetica Neue"), 22, false, Qt::black, {}), fullPara(4, 36, 1.18, 0, notesList, false), 0,
                                   QStringLiteral("Presenter Notes"), QStringLiteral("note-0-paragraphstyle"));

        Pb none;
        none.msg(1, Pb());
        charNull = addStyle(TSWPCharacterStyle, [&](const Pb &head) { Pb s; s.msg(1, head); return s; }, {}, 0,
                            QStringLiteral("None"), QStringLiteral("character-style-null"));
        charLink = addStyle(TSWPCharacterStyle, [&](const Pb &head) {
            Pb props;
            props.e(11, 1);
            Pb s;
            s.msg(1, head).u(10, 1).msg(11, props);
            return s;
        }, {}, 0, QStringLiteral("Link"), QStringLiteral("character-style-hyperlink"));

        Pb noFill;
        textboxStyle = shapeStyleOf(fullShape(noFill, noStroke(), noShadow()), fullText(bodyPara), 0, QStringLiteral("textbox-0-shapestyle"));
        shapeStyle = shapeStyleOf(fullShape(solidFill(accent), noStroke(), noShadow()), fullText(bodyPara), 0, QStringLiteral("shape-0-shapestyle"));
        Pb line;
        line.msg(1, color(fore)).f32(2, 2 * unit).e(3, 0).e(4, 0).f32(5, 4).msg(6, strokePattern(1));
        lineStyle = shapeStyleOf(fullShape(noFill, line, noShadow()), fullText(bodyPara), 0, QStringLiteral("line-0-shapestyle"));
        drawingLineStyle = shapeStyleOf(fullShape(noFill, line, noShadow()), Pb(), 0, QStringLiteral("drawingline-0-drawinglineStyle"));
        Pb media;
        media.f32(2, 1).msg(4, Pb()).msg(3, noShadow()).msg(1, noStroke());
        imageStyle = mediaStyleOf(media, 0, QStringLiteral("image-0-imageStyle"));
        Pb movie;
        movie.msg(1, noStroke());
        movieStyle = mediaStyleOf(movie, 0, QStringLiteral("movie-0-movieStyle"));
    }

    // ---- looks

    // Keynote turns objects the other way: anticlockwise, in whole turns.
    static qreal angle(qreal rotation) {
        const qreal a = std::fmod(-rotation, 360.0);
        return a < 0 ? a + 360 : a;
    }

    Pb shadowOf(const QColor &c, qreal dx, qreal dy, qreal blur) const {
        Pb s;
        QColor solid = c;
        solid.setAlphaF(1);
        qreal a = qRadiansToDegrees(std::atan2(-dy, dx));
        if (a < 0) a += 360;
        s.msg(1, color(solid)).f32(2, a).f32(3, std::hypot(dx, dy)).e(4, qMax(0, qRound(blur))).f32(5, c.alphaF()).flag(6, true).e(7, 0);
        return s;
    }

    Pb strokeOf(const SceneObject &o) const {
        if (o.strokeWidth <= 0 || o.strokeColor.alpha() == 0) return noStroke();
        Pb s;
        s.msg(1, color(o.strokeColor)).f32(2, o.strokeWidth).e(3, o.strokeCap == 2 ? 1 : o.strokeCap == 1 ? 2 : 0)
            .e(4, o.strokeJoin == 2 ? 1 : o.strokeJoin == 1 ? 2 : 0).f32(5, 4);
        // Qt's dash patterns, in widths of the line.
        static const QVector<QVector<qreal>> dashes = {{}, {4, 2}, {1, 2}, {4, 2, 1, 2}, {4, 2, 1, 2, 1, 2}};
        if (o.strokeStyle > 0 && o.strokeStyle < dashes.size()) s.msg(6, strokePattern(0, dashes[o.strokeStyle]));
        else s.msg(6, strokePattern(1));
        return s;
    }

    Pb fillOf(const SceneObject &o) {
        switch (o.fillStyle) {
        case 5:
            return Pb();
        case 1:
        case 2: {
            Pb g;
            g.e(1, o.fillStyle == 1 ? 0 : 1);
            const QColor stops[2] = {o.fill, o.fillSecondary};
            for (int i = 0; i < 2; ++i) { Pb stop; stop.msg(1, color(stops[i])).f32(2, i).f32(3, 0.5); g.msg(2, stop); }
            g.f32(3, 1).flag(4, false);
            if (o.fillStyle == 1) {
                qreal a = std::fmod(-qDegreesToRadians(o.fillAngle), 2 * M_PI);
                if (a < 0) a += 2 * M_PI;
                Pb angled;
                angled.f32(2, a);
                g.msg(5, angled);
            }
            Pb f;
            f.msg(2, g);
            return f;
        }
        case 4: {
            SceneObject pic = o;
            pic.type = ObjectType::Image;
            if (o.image.isNull() && o.vectorPicture.isNull()) return solidFill(o.fill);
            const qreal density = qBound(1.0, 2.0, 4096.0 / qMax(1.0, qMax(o.rect.width(), o.rect.height())));
            const QImage image = drawnPicture(pic, density);
            Pb picture;
            picture.data(6, addImage(image)).e(2, 1).msg(4, size(o.rect.size())).flag(8, false);
            Pb f;
            f.msg(3, picture);
            return f;
        }
        case 3:
            warnings.add(QStringLiteral("A pattern fill was written in its main colour"), slide);
            Q_FALLTHROUGH();
        default:
            return solidFill(o.fill);
        }
    }

    // The picture as OmaShow draws it into its box.
    QImage drawnPicture(const SceneObject &o, qreal density) const {
        const QSize px(qMax(1, qRound(o.rect.width() * density)), qMax(1, qRound(o.rect.height() * density)));
        QImage out(px, QImage::Format_ARGB32_Premultiplied);
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

    // ---- drawables

    quint64 standin(int part) {
        const quint64 id = newId();
        put(part, id, TSDStandinCaption, Pb(), {10, 1, 0});
        return id;
    }

    QString link(const SceneObject &o) {
        if (!o.linkKind || o.linkTarget.isEmpty()) return {};
        if (o.linkKind == 1) return o.linkTarget;
        if (o.linkKind == 2) return o.linkTarget.startsWith(QLatin1String("mailto:")) ? o.linkTarget : QStringLiteral("mailto:") + o.linkTarget;
        warnings.add(QStringLiteral("Links to other slides were left out for now"), slide);
        return {};
    }

    // What every object starts with: where it is, what holds it, its link.
    Pb drawable(int part, quint64 parent, const QRectF &r, qreal rotation, const SceneObject *o, bool captions = true, bool aspect = false) {
        Pb geometry;
        geometry.msg(1, point(r.topLeft())).msg(2, size(r.size())).u(3, 3).f32(4, angle(rotation));
        Pb wrap;
        wrap.u(1, 4).u(2, 2).u(3, 1).f32(4, 12).f32(5, 0.5).flag(6, false);
        Pb d;
        d.msg(1, geometry).ref(2, parent, true).msg(3, wrap);
        if (o) {
            const QString url = link(*o);
            if (!url.isEmpty()) d.str(4, url);
        }
        d.flag(5, o && o->locked).flag(7, aspect);
        if (o && !o->altText.isEmpty()) d.str(8, o->altText);
        else if (o && !o->altTitle.isEmpty()) d.str(8, o->altTitle);
        if (captions) d.ref(10, standin(part)).ref(11, standin(part)).flag(12, false).flag(13, false);
        return d;
    }

    // ---- text

    // How a box's words look, as a variation of `base`: what the box says
    // for every paragraph of one kind.
    quint64 paragraphLook(const SceneObject &o, quint64 base, quint64 list, bool direction) {
        Pb c;
        c.str(5, font(o.fontFamily)).f32(3, o.fontSize).flag(1, o.fontWeight >= 600).flag(2, o.italic)
            .msg(7, color(o.textColor)).msg(46, solidFill(o.textColor)).e(11, o.underline ? 1 : 0).e(12, o.strike ? 1 : 0)
            .e(13, o.uppercase ? 1 : 0).f32(27, o.fontSize > 0 ? o.letterSpacing / o.fontSize : 0);
        const QString language = o.language.isEmpty() ? doc.language : o.language;
        if (!language.isEmpty()) c.flag(8, false).str(9, language);
        if (o.textGlowRadius > 0 && o.textGlowColor.alpha() > 0) {
            // Keynote has no glow: a shadow all round in the glow's colour.
            c.flag(20, false).msg(21, shadowOf(o.textGlowColor, 0, 0, o.textGlowRadius));
            warnings.add(QStringLiteral("Glowing text was written as a soft shadow all round (Keynote has no glow)"), slide);
        } else if (o.shadowEnabled && o.shadowColor.alpha() > 0) {
            c.flag(20, false).msg(21, shadowOf(o.shadowColor, o.shadowX, o.shadowY, o.shadowBlur));
        }
        Pb p;
        static const int align[] = {0, 2, 1, 3};
        Pb lines;
        lines.f32(2, qBound(0.1, o.lineHeight / 100.0, 10.0));
        p.e(1, align[qBound(0, o.textAlign, 3)]).msg(13, lines).f32(20, o.paragraphSpacing).f32(21, 0).f32(11, o.textIndent)
            .f32(7, 0).f32(4, o.tabStop > 0 ? o.tabStop : o.fontSize * 4).ref(40, list);
        if (direction && o.direction > 0) p.e(38, o.direction == 1 ? 0 : 1);
        return paragraphStyle(c, p, base);
    }

    // The list a box's paragraphs follow, at OmaShow's indents.
    quint64 listLook(const SceneObject &o) {
        if (o.listStyle <= 0) return noneList;
        const qreal em = o.fontSize * 1.2;
        const bool numbers = o.listStyle == 2 || o.listStyle >= 5;
        if (numbers) {
            const int kind = o.listStyle == 5 ? 12 : o.listStyle == 6 ? 9 : o.listStyle == 7 ? 6 : o.listStyle == 8 ? 3 : 0;
            if (o.listStart != 1) warnings.add(QStringLiteral("A numbered list was written starting from 1"), slide);
            return listStyleOf(3, {}, kind, em, em * 0.4, 1, numberList);
        }
        const QString marker = o.listStyle == 3 ? QStringLiteral("○") : o.listStyle == 4 ? QStringLiteral("■") : QStringLiteral("•");
        return listStyleOf(2, marker, 0, em, em * 0.4, 1, bulletList);
    }

    // A stretch that looks unlike its box: a character style of only what differs.
    quint64 charLook(int part, const SceneObject &o, const TextRun &run) {
        Pb c;
        if (!run.fontFamily.isEmpty() && run.fontFamily != o.fontFamily) c.str(5, font(run.fontFamily));
        if (run.fontSize > 0 && !qFuzzyCompare(run.fontSize, o.fontSize)) c.f32(3, run.fontSize);
        if (run.weight && (run.weight >= 600) != (o.fontWeight >= 600)) c.flag(1, run.weight >= 600);
        if (run.italic && (run.italic == 1) != o.italic) c.flag(2, run.italic == 1);
        if (run.underline && (run.underline == 1) != o.underline) c.e(11, run.underline == 1 ? 1 : 0);
        if (run.strike && (run.strike == 1) != o.strike) c.e(12, run.strike == 1 ? 1 : 0);
        if (run.baseline) c.e(10, run.baseline == 1 ? 1 : 2);
        if (run.color.isValid() && run.color != o.textColor) c.msg(7, color(run.color)).msg(46, solidFill(run.color));
        if (!run.language.isEmpty()) c.flag(8, false).str(9, run.language);
        if (c.count == 0) return 0;
        const QByteArray key = QByteArray::number(part) + ':' + c.b;
        if (auto it = partStyleCache.find(key); it != partStyleCache.end()) return *it;
        const quint64 id = newId();
        Pb s;
        s.msg(1, styleHead({}, {}, 0)).u(10, quint64(c.count)).msg(11, c);
        put(part, id, TSWPCharacterStyle, s);
        partStyleCache.insert(key, id);
        return id;
    }

    // The words of a box: one string, a paragraph style and list for each
    // paragraph, its level, and where a stretch looks different.
    quint64 storage(int part, const SceneObject &o, quint64 base, int kind = 3) {
        const QStringList paragraphs = o.text.split(QLatin1Char('\n'));
        const bool listed = o.listStyle > 0;
        const quint64 list = kind == 4 ? notesList : listLook(o);
        const quint64 para = kind == 4 ? notesPara : paragraphLook(o, base, list, true);
        QString text;
        Pb paraStyles, paraData, lists, chars;
        quint64 lastChar = quint64(-1);
        int offset = 0;
        for (int p = 0; p < paragraphs.size(); ++p) {
            const QString &line = paragraphs[p];
            int level = 0, skip = 0;
            if (listed) while (skip < line.size() && line.at(skip) == QLatin1Char('\t')) { ++level; ++skip; }
            level = qMin(level, 8);
            const int start = int(text.size());
            Pb entry;
            entry.u(1, quint64(start)).ref(2, para);
            paraStyles.msg(1, entry);
            Pb data;
            data.u(1, quint64(start)).u(2, quint64(level)).u(3, 0);
            paraData.msg(1, data);
            Pb listEntry;
            listEntry.u(1, quint64(start)).ref(2, list);
            lists.msg(1, listEntry);
            for (int i = skip; i < line.size(); ++i) {
                const int at = offset + i;
                if (kind != 4) {
                    TextRun look;
                    for (const auto &run : o.runs) {
                        if (at < run.start || at >= run.start + run.length) continue;
                        if (!run.fontFamily.isEmpty()) look.fontFamily = run.fontFamily;
                        if (run.fontSize > 0) look.fontSize = run.fontSize;
                        if (run.weight) look.weight = run.weight;
                        if (run.italic) look.italic = run.italic;
                        if (run.underline) look.underline = run.underline;
                        if (run.strike) look.strike = run.strike;
                        if (run.baseline) look.baseline = run.baseline;
                        if (run.color.isValid()) look.color = run.color;
                        if (!run.language.isEmpty()) look.language = run.language;
                    }
                    const quint64 style = o.runs.isEmpty() ? 0 : charLook(part, o, look);
                    if (style != lastChar) {
                        Pb c;
                        c.u(1, quint64(text.size()));
                        if (style) c.ref(2, style);
                        chars.msg(1, c);
                        lastChar = style;
                    }
                }
                text += line.at(i);
            }
            if (p + 1 < paragraphs.size()) text += QLatin1Char('\n');
            offset += int(line.size()) + 1;
        }
        Pb single;
        single.u(1, 0).u(2, 0).u(3, 0);
        Pb starts, bidi, dropCap, noDropCap;
        starts.msg(1, single);
        bidi.msg(1, single);
        noDropCap.u(1, 0);
        dropCap.msg(1, noDropCap);
        Pb s;
        if (kind != 3) s.e(1, kind);
        s.ref(2, sheet, true);
        if (!text.isEmpty()) s.str(3, text);
        s.flag(10, true).msg(5, paraStyles).msg(6, paraData).msg(7, lists);
        if (lastChar != quint64(-1) && !chars.b.isEmpty()) s.msg(8, chars);
        s.msg(14, starts).msg(24, bidi);
        if (kind != 4) s.msg(28, dropCap);
        const quint64 id = newId();
        put(part, id, TSWPStorage, s);
        return id;
    }

    // A shape that holds text, as text boxes and placeholders are.
    Pb shapeInfo(int part, quint64 parent, const SceneObject &o, quint64 style, quint64 words, bool textBox) {
        Pb shape;
        shape.msg(1, drawable(part, parent, o.rect, o.rotation, &o)).ref(2, style)
            .msg(3, pathSource(rectPath(QRectF(QPointF(0, 0), o.rect.size())), QRectF(QPointF(0, 0), o.rect.size()), true)).f32(6, 0);
        Pb info;
        info.msg(1, shape).ref(2, words).ref(4, words);
        if (textBox) info.flag(6, true);
        return info;
    }

    // How a text box sits: its padding, its alignment, fitting and columns.
    quint64 textBoxLook(const SceneObject &o) {
        Pb drawing;
        if (o.opacity < 0.999) drawing.f32(3, o.opacity);
        Pb padding;
        padding.f32(1, 0).f32(2, 0).f32(3, 0).f32(4, 0);
        Pb text;
        text.flag(1, o.textFit == 1).e(2, qBound(0, o.verticalAlign, 2)).msg(6, padding);
        if (o.columns > 1) {
            Pb columns, equal;
            equal.u(1, quint64(o.columns)).f32(2, o.columnGap > 0 ? o.columnGap : o.fontSize);
            columns.msg(1, equal);
            text.msg(4, columns);
        }
        return shapeStyleOf(drawing, text, textboxStyle);
    }

    void writeText(int part, quint64 parent, const SceneObject &o, QVector<quint64> &drawn) {
        if (o.textKind == 1) { writeSnapshot(part, parent, o, QStringLiteral("An equation was written as a picture of itself"), drawn); return; }
        if (o.textKind == 2) warnings.add(QStringLiteral("Words on a path were written as an ordinary text box"), slide);
        const quint64 words = storage(part, o, bodyPara);
        const quint64 style = textBoxLook(o);
        const quint64 id = newId();
        put(part, id, TSWPShapeInfo, shapeInfo(part, parent, o, style, words, true));
        drawn.append(id);
    }

    // ---- shapes

    void writeShape(int part, quint64 parent, const SceneObject &o, QVector<quint64> &drawn) {
        const bool line = o.connector || o.shapeKind == 13;
        QPainterPath outline = Shape::path(o);
        QRectF box = line ? outline.boundingRect() : o.rect;
        if (box.width() < 1) box.setWidth(1);
        if (box.height() < 1) box.setHeight(1);
        Pb drawing;
        drawing.msg(1, line ? Pb() : fillOf(o)).msg(2, strokeOf(o));
        if (o.opacity < 0.999) drawing.f32(3, o.opacity);
        if (o.shadowEnabled && o.shadowColor.alpha() > 0) drawing.msg(4, shadowOf(o.shadowColor, o.shadowX, o.shadowY, o.shadowBlur));
        if (line) {
            // Arrowheads: Keynote's filled arrow, drawn from where the line ends.
            const auto arrow = [] {
                QPainterPath tip;
                tip.moveTo(0, 0);
                tip.lineTo(6, 0);
                tip.lineTo(3, 7);
                tip.closeSubpath();
                Pb end;
                end.msg(1, path(tip, {0, 0}, true)).e(2, 0).msg(3, point({3, 7})).flag(4, true).str(5, QStringLiteral("filled arrow"));
                return end;
            };
            if (o.connectorArrowEnd) drawing.msg(6, arrow());
            if (o.connectorArrowStart) drawing.msg(7, arrow());
        }
        const quint64 style = shapeStyleOf(drawing, Pb(), line ? lineStyle : shapeStyle);
        // Lines too are open shapes, as Keynote's pen draws them, which every
        // Keynote reader takes.
        Pb shape;
        shape.msg(1, drawable(part, parent, box, line ? 0 : o.rotation, &o)).ref(2, style).msg(3, pathSource(outline, box, !line)).f32(6, 0);
        // A shape holds words in Keynote, even when it has none.
        SceneObject empty;
        empty.text.clear();
        empty.fontFamily = doc.theme.fonts.value(QStringLiteral("body"));
        empty.fontSize = 48 * doc.size.height() / 1080.0;
        empty.textColor = doc.theme.colors.value(QStringLiteral("foreground"), QColor(Qt::black));
        empty.textAlign = 1;
        empty.textFit = 0;
        const quint64 words = storage(part, empty, bodyPara);
        Pb info;
        info.msg(1, shape).ref(2, words).ref(4, words);
        const quint64 id = newId();
        put(part, id, TSWPShapeInfo, info);
        drawn.append(id);
    }

    // ---- pictures

    // A picture: its bytes, where the whole of it sits, and the part shown.
    void picture(int part, quint64 parent, const SceneObject &o, quint64 data, const QSize &pixels, const QRectF &whole,
                 const QRectF &shown, int mask, QVector<quint64> &drawn) {
        const quint64 id = newId();
        Pb props;
        if (o.strokeWidth > 0 && o.strokeColor.alpha() > 0) props.msg(1, strokeOf(o));
        if (o.opacity < 0.999) props.f32(2, o.opacity);
        if (o.shadowEnabled && o.shadowColor.alpha() > 0) props.msg(3, shadowOf(o.shadowColor, o.shadowX, o.shadowY, o.shadowBlur));
        const quint64 style = props.count ? mediaStyleOf(props, imageStyle) : imageStyle;
        const QSizeF natural = pixels.isValid() ? QSizeF(pixels) : whole.size();
        Pb image;
        image.msg(1, drawable(part, parent, whole, o.rotation, &o, true, true)).data(11, data).ref(3, style)
            .msg(4, size(whole.size())).msg(9, size(natural)).u(7, 0).flag(18, false)
            .msg(19, path(rectPath(QRectF(QPointF(0, 0), natural)), {0, 0}, true));
        if (mask >= 0) {
            const quint64 maskId = newId();
            const QRectF local(shown.topLeft() - whole.topLeft(), shown.size());
            QPainterPath outline;
            if (mask == 1) outline.addEllipse(QRectF(QPointF(0, 0), shown.size()));
            else outline.addRect(QRectF(QPointF(0, 0), shown.size()));
            Pb m;
            m.msg(1, drawable(part, id, local, 0, nullptr, false)).msg(2, pathSource(outline, QRectF(QPointF(0, 0), shown.size()), true));
            put(part, maskId, TSDMask, m);
            image.ref(5, maskId);
        }
        put(part, id, TSDImage, image);
        drawn.append(id);
    }

    void writePicture(int part, quint64 parent, const SceneObject &o, QVector<quint64> &drawn) {
        if ((o.image.isNull() && o.vectorPicture.isNull()) || o.rect.isEmpty()) {
            warnings.add(QStringLiteral("A picture had no data to write"), slide);
            return;
        }
        const QString format = o.imageFormat.toLower();
        const QSizeF natural = ImageAsset::size(o);
        const QRectF source = ImageAsset::sourceRect(o);
        QRectF target = o.rect;
        if (o.imageMode == 0) {
            QSizeF fitted = source.size();
            fitted.scale(o.rect.size(), Qt::KeepAspectRatio);
            target = QRectF(o.rect.center() - QPointF(fitted.width() / 2, fitted.height() / 2), fitted);
        }
        const bool whole = qAbs(source.x()) < 0.5 && qAbs(source.y()) < 0.5 && qAbs(source.width() - natural.width()) < 0.5 &&
                           qAbs(source.height() - natural.height()) < 0.5;
        const bool adjusted = !qFuzzyIsNull(o.imageBrightness) || !qFuzzyCompare(o.imageContrast, 1.0) ||
                              !qFuzzyCompare(o.imageSaturation, 1.0) || !qFuzzyIsNull(o.imageTintAmount);
        const bool kept = format == QLatin1String("png") || format == QLatin1String("jpg") || format == QLatin1String("jpeg") ||
                          format == QLatin1String("gif") || format == QLatin1String("tif") || format == QLatin1String("tiff");
        const bool fills = qAbs(target.width() - o.rect.width()) < 0.5 && qAbs(target.height() - o.rect.height()) < 0.5;
        const bool shaped = o.imageMask == 1 ? fills : o.imageMask == 0;
        const bool masked = !whole || o.imageMask == 1;
        if (!adjusted && !o.image.isNull() && o.vectorPicture.isNull() && shaped && !(masked && !qFuzzyIsNull(o.rotation)) &&
            !source.isEmpty()) {
            // The original picture, placed so the part shown lands on the box.
            quint64 data;
            if (kept && !o.imageData.isEmpty())
                data = addData(o.imageData, QStringLiteral("image"), format == QLatin1String("jpeg") ? QStringLiteral("jpg") : format == QLatin1String("tif") ? QStringLiteral("tiff") : format,
                               o.image.size());
            else
                data = addImage(o.image);
            const qreal sx = target.width() / source.width(), sy = target.height() / source.height();
            const QRectF full(target.left() - source.left() * sx, target.top() - source.top() * sy, natural.width() * sx, natural.height() * sy);
            picture(part, parent, o, data, o.image.size(), full, target, masked ? o.imageMask : -1, drawn);
            return;
        }
        // Anything else as OmaShow draws it.
        const bool svg = format == QLatin1String("svg") || !o.vectorPicture.isNull();
        const qreal perUnit = svg ? 2.0 : source.width() / qMax(1.0, target.width());
        const qreal density = qBound(1.0, perUnit, 4096.0 / qMax(1.0, qMax(o.rect.width(), o.rect.height())));
        const QImage image = drawnPicture(o, density);
        const bool photo = (format == QLatin1String("jpg") || format == QLatin1String("jpeg")) && o.imageMask == 0 && fills;
        const quint64 data = photo ? addData(encoded(image.convertToFormat(QImage::Format_RGB32), "JPEG", 92), QStringLiteral("image"), QStringLiteral("jpg"), image.size())
                                   : addImage(image);
        if (adjusted) warnings.add(QStringLiteral("A picture's colour changes were written into the picture"), slide);
        picture(part, parent, o, data, image.size(), o.rect, o.rect, -1, drawn);
    }

    void writeSnapshot(int part, quint64 parent, const SceneObject &o, const QString &what, QVector<quint64> &drawn) {
        if (o.rect.isEmpty()) return;
        warnings.add(what, slide);
        const QImage image = snapshot(o);
        SceneObject plain = o;
        plain.strokeWidth = 0;
        plain.shadowEnabled = false;
        picture(part, parent, plain, addImage(image), image.size(), o.rect, o.rect, -1, drawn);
    }

    void writeMedia(int part, quint64 parent, const SceneObject &o, QVector<quint64> &drawn) {
        warnings.add(QStringLiteral("A film or sound was written as its first picture"), slide);
        const QImage poster = MediaAsset::frameAt(o, qMax(0.0, o.mediaTrimStart));
        if (poster.isNull() || o.rect.isEmpty()) return;
        picture(part, parent, o, addImage(poster), poster.size(), o.rect, o.rect, -1, drawn);
    }

    void writeObjects(int part, quint64 parent, const QVector<SceneObject> &objects, QVector<quint64> &drawn) {
        for (const auto &o : objects) {
            if (canceled()) return;
            if (o.hidden) { warnings.add(QStringLiteral("Hidden objects were left out"), slide); continue; }
            if (!o.groups.isEmpty()) warnings.add(QStringLiteral("Groups were written as their separate objects"), slide);
            switch (o.type) {
            case ObjectType::Rect: writeShape(part, parent, o, drawn); break;
            case ObjectType::Text: writeText(part, parent, o, drawn); break;
            case ObjectType::Image: writePicture(part, parent, o, drawn); break;
            case ObjectType::Media: writeMedia(part, parent, o, drawn); break;
            case ObjectType::Table: writeSnapshot(part, parent, o, QStringLiteral("A table was written as a picture of itself"), drawn); break;
            case ObjectType::Chart: writeSnapshot(part, parent, o, QStringLiteral("A chart was written as a picture of itself"), drawn); break;
            }
        }
    }

    // ---- slides

    // The title, body and slide number every Keynote slide has. On a master
    // they show the layout's own; on a slide they are kept empty and hidden,
    // as OmaShow's words are written as text boxes.
    struct Placeholders { quint64 title = 0, body = 0, number = 0; };
    Placeholders placeholders(int part, quint64 parent, const SlideLayout *layout, bool master) {
        const QRectF page(QPointF(0, 0), doc.size);
        const QColor fore = doc.theme.colors.value(QStringLiteral("foreground"), QColor(Qt::black));
        SceneObject title, body;
        bool haveTitle = false, haveBody = false;
        if (layout)
            for (const auto &p : layout->placeholders) {
                if (p.id == QLatin1String("title") && !haveTitle) { title = Design::themed(doc.theme, p); haveTitle = true; }
                if (p.id == QLatin1String("body") && !haveBody) { body = Design::themed(doc.theme, p); haveBody = true; }
            }
        if (!haveTitle) {
            title.rect = QRectF(page.width() * .05, page.height() * .04, page.width() * .9, page.height() * .17);
            title.fontSize = 96 * page.height() / 1080;
            title.fontFamily = doc.theme.fonts.value(QStringLiteral("heading"));
            title.textColor = fore;
            title.fontWeight = 700;
        }
        if (!haveBody) {
            body.rect = QRectF(page.width() * .05, page.height() * .25, page.width() * .9, page.height() * .65);
            body.fontSize = 48 * page.height() / 1080;
            body.fontFamily = doc.theme.fonts.value(QStringLiteral("body"));
            body.textColor = fore;
            body.listStyle = 1;
            body.verticalAlign = 0;
        }
        SceneObject number;
        number.rect = QRectF(page.width() / 2 - 40, page.height() - 60, 80, 40);
        number.fontSize = 18 * page.height() / 1080;
        number.fontFamily = doc.theme.fonts.value(QStringLiteral("body"));
        number.textColor = fore;
        number.textAlign = 1;
        const auto one = [&](SceneObject o, quint64 base, int kind) {
            o.rotation = 0;
            o.linkKind = 0;
            o.locked = false;
            o.altText.clear();
            o.altTitle.clear();
            if (!master) o.text.clear();
            else if (kind == 2) o.text = QStringLiteral("Title");
            else if (kind == 3) o.text = o.listStyle > 0 ? QStringLiteral("Body Level One\n\tBody Level Two\n\t\tBody Level Three") : QStringLiteral("Body");
            else o.text.clear();
            const quint64 words = storage(part, o, base);
            const quint64 style = textBoxLook(o);
            Pb ph;
            ph.msg(1, shapeInfo(part, parent, o, style, words, true)).e(2, kind);
            const quint64 id = newId();
            put(part, id, KNPlaceholder, ph);
            return id;
        };
        Placeholders out;
        out.title = one(title, titlePara, 2);
        out.body = one(body, bodyPara, 3);
        out.number = one(number, numberPara, 1);
        return out;
    }

    Pb transition(int index, const Slide &s) {
        const int kind = index >= 0 ? Presentation::transitionKind(doc, index) : Presentation::Cut;
        const qreal seconds = index >= 0 ? Presentation::transitionSeconds(doc, index) : 1.0;
        QString effect = QStringLiteral("apple:dissolve");
        switch (kind) {
        case Presentation::Cut: effect = QStringLiteral("none"); break;
        case Presentation::Fade:
        case Presentation::FadeThroughBlack: break;
        case Presentation::Push: effect = QStringLiteral("apple:push"); break;
        default:
            warnings.add(QStringLiteral("A transition was written as a dissolve"), index + 1);
            break;
        }
        Pb animation;
        animation.str(1, QStringLiteral("Transition")).str(2, effect).f64(3, qMax(0.1, seconds)).u(4, 0)
            .f64(5, s.advanceAfter >= 0 ? s.advanceAfter : 0).flag(6, s.advanceAfter >= 0)
            .u(11, quint64(qFromLittleEndian<quint32>(seed.constData()) ^ quint32(index + 7)) & 0x7FFFFFFF).flag(16, false);
        Pb attributes;
        attributes.msg(8, animation);
        Pb t;
        t.msg(2, attributes);
        return t;
    }

    // A slide style: the master's, or a variation with this slide's background.
    quint64 slideLook(const KeyMaster &m, const QColor &background) {
        Pb props;
        if (background != m.background) props.msg(1, solidFill(background));
        props.flag(4, false).flag(5, false).flag(6, false).flag(7, false);
        return addStyle(KNSlideStyle, [&](const Pb &head) {
            Pb s;
            s.msg(1, head).u(10, quint64(props.count)).msg(11, props);
            return s;
        }, props.b, m.style);
    }

    QImage thumbnail(const QVector<SceneObject> &objects, const QColor &background, int width) const {
        const qreal w = qMax(1.0, doc.size.width()), h = qMax(1.0, doc.size.height());
        const QSize px(width, qMax(1, qRound(width * h / w)));
        QImage image = SceneRenderer::render(objects, doc.size, px, background);
        if (image.isNull()) { image = QImage(px, QImage::Format_RGB32); image.fill(background); }
        return image.convertToFormat(QImage::Format_RGB32);
    }
    quint64 thumbnailData(const QImage &image) {
        return addData(encoded(image, "JPEG", 85), QStringLiteral("st-") + QUuid::fromRfc4122(seed.left(16)).toString(QUuid::WithoutBraces).toUpper(),
                       QStringLiteral("jpg"), image.size());
    }

    Pb slideNode(quint64 slideId, quint64 thumb, const QSize &thumbSize, bool skipped, bool transition, bool note) {
        Pb n;
        n.ref(2, slideId).u(21, 1).data(16, thumb).msg(10, size(thumbSize)).flag(14, false).flag(4, skipped).flag(6, false)
            .flag(7, transition).flag(8, note).flag(18, false).u(26, 0xFFFFFFFFu).flag(20, false).u(27, 1).flag(28, false);
        return n;
    }

    QVector<quint64> masterNodes;
    int documentPart = -1;
    QImage firstSlide;

    void writeMaster(KeyMaster &m, int number) {
        const auto *layout = Design::layout(doc, m.layoutId);
        const auto *base = layout ? Design::master(doc, layout->masterId) : nullptr;
        if (!base && !doc.masters.isEmpty()) base = &doc.masters.first();
        m.background = base ? doc.theme.colors.value(base->backgroundToken, base->background)
                            : doc.theme.colors.value(QStringLiteral("background"), QColor(Qt::white));
        m.id = newId();
        const int part = newPart(QStringLiteral("MasterSlide"), QStringLiteral("MasterSlide-%1").arg(m.id));
        slide = 0;

        Pb props;
        props.msg(1, solidFill(m.background)).flag(4, true).flag(5, true).flag(6, false).flag(7, false);
        m.style = addStyle(KNSlideStyle, [&](const Pb &head) {
            Pb s;
            s.msg(1, head).u(10, quint64(props.count)).msg(11, props);
            return s;
        }, {}, 0, QStringLiteral("master slide style"), QStringLiteral("slide-%1-slidestyle").arg(number));

        QVector<SceneObject> objects;
        if (base && m.objects)
            for (const auto &o : base->objects) objects.append(Design::themed(doc.theme, o));
        QVector<quint64> drawn;
        writeObjects(part, m.id, objects, drawn);
        const Placeholders ph = placeholders(part, m.id, layout, true);

        const quint64 guides = newId();
        put(part, guides, TSDGuideStorage, Pb());
        const quint64 classic = newId();
        put(part, classic, KNClassicStylesheetRecord, Pb());

        Pb instructions, title, body;
        title.ref(1, ph.title).str(2, QStringLiteral("Title"));
        body.ref(1, ph.body).str(2, QStringLiteral("Body"));
        instructions.msg(1, title).msg(1, body);
        Pb s;
        s.ref(1, m.style).msg(4, transition(-1, Slide())).ref(5, ph.title).ref(6, ph.body).ref(20, ph.number);
        for (quint64 id : drawn) s.ref(7, id);
        for (quint64 id : drawn) s.ref(42, id);
        QString name = layout ? layout->name : QStringLiteral("Blank");
        if (!m.objects) name += QStringLiteral(" (no master objects)");
        s.msg(45, instructions).str(10, name).ref(29, classic);
        for (int i = 0; i < 5; ++i) s.ref(31, bodyPara);
        for (int i = 0; i < 5; ++i) s.ref(35, bulletList);
        s.str(38, QStringLiteral("Body")).flag(41, false).ref(36, guides).flag(19, true);
        putRoot(part, m.id, KNSlide, s);
        parts[part].root = m.id;

        const QImage thumb = thumbnail(objects, m.background, 356);
        m.node = newId();
        put(documentPart, m.node, KNSlideNode, slideNode(m.id, thumbnailData(thumb), thumb.size(), false, false, false));
        masterNodes.append(m.node);
    }

    quint64 writeSlide(int index) {
        const Slide &s = doc.slides[index];
        slide = index + 1;
        const Slide resolved = Design::resolve(doc, index);
        int which = 0;
        for (int i = 0; i < keyMasters.size(); ++i)
            if (keyMasters[i].layoutId == s.layoutId && keyMasters[i].objects == s.showMasterObjects) which = i;
        const KeyMaster &m = keyMasters[which];
        const quint64 id = newId();
        const int part = newPart(QStringLiteral("Slide"), QStringLiteral("Slide-%1").arg(id));

        QVector<SceneObject> objects;
        for (const auto &o : resolved.objects) if (!o.id.startsWith(QLatin1String("@master/"))) objects.append(o);
        QVector<quint64> drawn;
        writeObjects(part, id, objects, drawn);
        const Placeholders ph = placeholders(part, id, Design::layout(doc, s.layoutId), false);

        const quint64 guides = newId();
        put(part, guides, TSDGuideStorage, Pb());
        quint64 note = 0;
        if (!s.notes.trimmed().isEmpty()) {
            SceneObject words;
            words.text = s.notes;
            const quint64 storageId = storage(part, words, notesPara, 4);
            note = newId();
            Pb n;
            n.ref(1, storageId);
            put(part, note, KNNote, n);
        }
        Pb a;
        a.ref(1, slideLook(m, resolved.background)).msg(4, transition(index, s)).ref(5, ph.title).ref(6, ph.body).ref(20, ph.number);
        for (quint64 d : drawn) a.ref(7, d);
        for (quint64 d : drawn) a.ref(42, d);
        a.msg(45, Pb()).ref(17, m.id).ref(36, guides).flag(19, true);
        if (note) a.ref(27, note);
        putRoot(part, id, KNSlide, a);

        if (!s.timeline.steps.isEmpty() || !s.pptxInteractive.isEmpty())
            warnings.add(QStringLiteral("Builds (animations) were left out for now"), slide);
        for (const auto &c : doc.comments)
            if (c.slideId == s.id) { warnings.add(QStringLiteral("Comments were left out for now"), slide); break; }

        const QImage thumb = thumbnail(resolved.objects, resolved.background, 356);
        if (index == 0) firstSlide = thumbnail(resolved.objects, resolved.background, 1024);
        const quint64 node = newId();
        const bool moves = Presentation::transitionKind(doc, index) != Presentation::Cut;
        put(documentPart, node, KNSlideNode, slideNode(id, thumbnailData(thumb), thumb.size(), s.skipped, moves, note != 0));
        return node;
    }

    // ---- the package

    // An object file: each object's ArchiveInfo and message, in Snappy chunks.
    QByteArray iwa(const Part &part) const {
        QByteArray raw;
        for (const auto &o : part.objects) {
            QVector<quint64> objects, datas;
            QSet<quint64> seen;
            // Only what the object holds on to: Keynote leaves weak links out.
            for (quint64 id : o.body.refs->objects)
                if (o.body.refs->strong.contains(id) && !seen.contains(id)) { seen.insert(id); objects.append(id); }
            seen.clear();
            for (quint64 id : o.body.refs->datas) if (!seen.contains(id)) { seen.insert(id); datas.append(id); }
            Pb info;
            info.u(1, o.type).packed(2, o.version).u(3, quint64(o.body.b.size())).packed(5, objects).packed(6, datas);
            Pb archive;
            archive.u(1, o.id).msg(2, info);
            Pb length;
            length.varint(quint64(archive.b.size()));
            raw += length.b + archive.b + o.body.b;
        }
        QByteArray out;
        for (qsizetype at = 0; at < raw.size(); at += 65536) {
            const QByteArray piece = raw.mid(at, 65536);
            Pb block;
            block.varint(quint64(piece.size()));
            const int n = int(piece.size()) - 1;   // one literal holds it all
            if (n < 60) block.b.append(char(n << 2));
            else if (n < 256) { block.b.append(char(60 << 2)); block.b.append(char(n)); }
            else { block.b.append(char(61 << 2)); block.b.append(char(n & 0xFF)); block.b.append(char((n >> 8) & 0xFF)); }
            block.b += piece;
            const int size = int(block.b.size());
            out.append(char(0));
            out.append(char(size & 0xFF));
            out.append(char((size >> 8) & 0xFF));
            out.append(char((size >> 16) & 0xFF));
            out += block.b;
        }
        return out;
    }

    // What the package holds, part by part: Keynote checks this on open.
    Pb packageMetadata(quint64 last) const {
        static const QSet<int> withoutUuid = {KNShow, KNClassicStylesheetRecord, TSKCustomFormatList, TSSStylesheet,
                                              TSAFunctionBrowserState, TSCECalculationEngine, TSCENamedReferenceManager,
                                              TSCETrackedReferenceStore, TSPDocumentMetadata, TSKAnnotationAuthorStorage, KNSoundtrack};
        Pb meta;
        meta.u(1, last);
        Pb revision;
        revision.str(2, uuid(1).toString(QUuid::WithoutBraces).toUpper()).e(3, 0);
        meta.msg(2, revision);
        for (int i = 0; i < parts.size(); ++i) {
            const Part &part = parts[i];
            Pb c;
            c.u(1, part.root).str(2, part.preferred);
            if (!part.locator.isEmpty()) c.str(3, part.locator);
            c.packed(4, {2, 0, 0}).packed(5, {2, 0, 0});
            // References to other parts: a part's root by the part alone.
            QVector<QPair<quint64, quint64>> external;
            QHash<QPair<quint64, quint64>, bool> strong;
            QVector<quint64> dataRefs;
            QSet<quint64> dataSeen;
            for (const auto &o : part.objects) {
                for (quint64 target : o.body.refs->objects) {
                    const int there = home.value(target, -1);
                    if (there < 0 || there == i) continue;
                    const QPair<quint64, quint64> key(parts[there].root, target == parts[there].root ? 0 : target);
                    if (!strong.contains(key)) external.append(key);
                    strong[key] = strong.value(key, false) || o.body.refs->strong.contains(target);
                }
                for (quint64 d : o.body.refs->datas) if (!dataSeen.contains(d)) { dataSeen.insert(d); dataRefs.append(d); }
            }
            for (const auto &key : external) {
                Pb r;
                r.u(1, key.first);
                if (key.second) r.u(2, key.second);
                if (!strong.value(key)) r.flag(3, true);
                c.msg(6, r);
            }
            for (quint64 d : dataRefs) { Pb r; r.u(1, d); c.msg(7, r); }
            c.flag(10, false);
            for (const auto &o : part.objects) {
                if (withoutUuid.contains(o.type) || part.preferred == QLatin1String("CalculationEngine")) continue;
                const QByteArray bytes = uuid(o.id).toRfc4122();
                Pb u;
                u.u(1, qFromBigEndian<quint64>(bytes.constData() + 8)).u(2, qFromBigEndian<quint64>(bytes.constData()));
                Pb entry;
                entry.u(1, o.id).msg(2, u);
                c.msg(11, entry);
            }
            c.u(12, 1);
            meta.msg(3, c);
        }
        for (const auto &f : datas) {
            Pb d;
            d.u(1, f.id).bytes(2, f.digest).str(3, f.preferred).str(4, f.file);
            if (f.pixels.isValid()) {
                Pb image;
                image.msg(1, size(f.pixels)).flag(3, false);
                Pb attributes;
                attributes.msg(100, image);
                d.msg(10, attributes);
            }
            meta.msg(4, d);
        }
        meta.packed(5, {2, 0, 0}).packed(6, {2, 0, 0}).packed(7, {10, 1, 8}).u(8, 1);
        return meta;
    }

    QByteArray run() {
        if (doc.slides.isEmpty()) return {};

        // The document's parts, with the ids Keynote expects of the first two.
        documentPart = newPart(QStringLiteral("Document"));
        sheet = newId();
        sheetPart = newPart(QStringLiteral("DocumentStylesheet"));
        theme(doc.size);

        // One master for each layout the slides use (and one without the
        // master's objects, for slides that hide them).
        for (const auto &s : doc.slides) {
            KeyMaster m;
            m.layoutId = s.layoutId;
            m.objects = s.showMasterObjects;
            bool known = false;
            for (const auto &k : keyMasters) if (k.layoutId == m.layoutId && k.objects == m.objects) known = true;
            if (!known) keyMasters.append(m);
        }
        for (int i = 0; i < keyMasters.size(); ++i) { if (canceled()) return {}; writeMaster(keyMasters[i], i); }
        QVector<quint64> nodes;
        for (int i = 0; i < doc.slides.size(); ++i) {
            if (canceled()) return {};
            nodes.append(writeSlide(i));
        }
        slide = 0;
        if (!doc.sections.isEmpty()) warnings.add(QStringLiteral("Sections were left out for now"), 0);
        if (!doc.shows.isEmpty()) warnings.add(QStringLiteral("Custom shows were left out for now"), 0);

        // The stylesheet, now every style is known.
        Pb sheetBody;
        // The stylesheet keeps its named styles; a variation lives while something uses it.
        QSet<quint64> kept;
        for (const auto &n : named) kept.insert(n.second);
        for (quint64 id : styles) sheetBody.ref(1, id, !kept.contains(id));
        for (const auto &n : named) { Pb e; e.str(1, n.first).ref(2, n.second); sheetBody.msg(2, e); }
        sheetBody.flag(4, false);
        for (auto it = variations.cbegin(); it != variations.cend(); ++it) {
            Pb e;
            e.ref(1, it.key());
            for (quint64 child : it.value()) e.ref(2, child, true);
            sheetBody.msg(5, e);
        }
        sheetBody.flag(6, true);
        putRoot(sheetPart, sheet, TSSStylesheet, sheetBody);

        // The small parts every Keynote document has.
        const int annotationsPart = newPart(QStringLiteral("AnnotationAuthorStorage"));
        const quint64 annotations = newId();
        putRoot(annotationsPart, annotations, TSKAnnotationAuthorStorage, Pb());
        const int calcPart = newPart(QStringLiteral("CalculationEngine"));
        const quint64 calc = newId(), names = newId(), tracked = newId();
        Pb store, trackedUuid;
        const QByteArray tu = uuid(tracked).toRfc4122();
        trackedUuid.u(2, qFromLittleEndian<quint32>(tu.constData())).u(3, qFromLittleEndian<quint32>(tu.constData() + 4))
            .u(4, qFromLittleEndian<quint32>(tu.constData() + 8)).u(5, qFromLittleEndian<quint32>(tu.constData() + 12));
        store.msg(1, trackedUuid);
        put(calcPart, tracked, TSCETrackedReferenceStore, store);
        Pb nrm;
        nrm.ref(1, tracked);
        put(calcPart, names, TSCENamedReferenceManager, nrm);
        Pb engine;
        engine.msg(2, Pb()).ref(3, names);
        putRoot(calcPart, calc, TSCECalculationEngine, engine);
        const int docMetaPart = newPart(QStringLiteral("DocumentMetadata"));
        const quint64 docMeta = newId();
        Pb dm;
        dm.flag(1, false);
        putRoot(docMetaPart, docMeta, TSPDocumentMetadata, dm);

        // The document: its show, the theme and the slide tree.
        const quint64 formats = newId(), browser = newId(), show = newId(), themeId = newId(), soundtrack = newId();
        put(documentPart, formats, TSKCustomFormatList, Pb());
        put(documentPart, browser, TSAFunctionBrowserState, Pb());
        Pb sound;
        sound.f64(1, 1).e(2, 0);
        put(documentPart, soundtrack, KNSoundtrack, sound);

        Pb drawingPresets;
        drawingPresets.ref(4, lineStyle).ref(5, shapeStyle).ref(6, textboxStyle).ref(7, imageStyle).ref(8, movieStyle).ref(9, drawingLineStyle);
        Pb textPresets;
        textPresets.ref(1, noneList).ref(1, bulletList).ref(1, numberList).ref(7, titlePara).ref(7, bodyPara);
        Pb tss;
        tss.str(3, QStringLiteral("OmaShow")).ref(4, sheet, true);
        for (const QString &key : {QStringLiteral("foreground"), QStringLiteral("background"), QStringLiteral("muted"), QStringLiteral("accent")})
            if (doc.theme.colors.contains(key)) tss.msg(10, color(doc.theme.colors.value(key)));
        tss.msg(100, drawingPresets).msg(110, textPresets);
        Pb th;
        th.msg(1, tss);
        for (quint64 node : masterNodes) th.ref(2, node);
        th.str(3, uuid(themeId).toString(QUuid::WithoutBraces).toUpper()).ref(5, masterNodes.value(0)).flag(7, true);
        put(documentPart, themeId, KNTheme, th);

        Pb tree;
        for (quint64 node : nodes) tree.ref(2, node);
        Pb sh;
        sh.ref(2, themeId).msg(3, tree).msg(4, size(doc.size)).ref(5, sheet).flag(8, false).e(9, 0).f64(10, 5).f64(11, 2)
            .flag(15, false).f64(16, 900).ref(17, soundtrack).flag(18, false);
        put(documentPart, show, KNShow, sh);

        const QString locale = doc.language.isEmpty() ? QStringLiteral("en_US") : doc.language;
        Pb tsk;
        tsk.str(4, locale).ref(7, annotations).str(9, locale).flag(10, true).flag(12, false).flag(15, true).flag(16, true);
        Pb tsa;
        tsa.msg(1, tsk).str(3, locale.section(QLatin1Char('_'), 0, 0)).ref(4, calc).ref(6, browser).flag(8, false).ref(12, formats)
            .flag(14, false).flag(15, false);
        Pb document;
        document.msg(3, tsa).ref(2, show);
        putRoot(documentPart, 1, KNDocument, document);

        // The parts list, last.
        const Pb meta = packageMetadata(next);
        QVector<Zip::Entry> entries;
        for (const auto &part : parts)
            entries.append({QStringLiteral("Index/") + (part.locator.isEmpty() ? part.preferred : part.locator) + QStringLiteral(".iwa"), iwa(part), false});
        Part metadata;
        metadata.preferred = QStringLiteral("Metadata");
        metadata.root = 2;
        metadata.objects.append({2, TSPPackageMetadata, {1, 0, 5}, meta});
        entries.append({QStringLiteral("Index/Metadata.iwa"), iwa(metadata), false});
        for (const auto &f : datas) entries.append({QStringLiteral("Data/") + f.file, f.bytes, false});

        const QString document_ = uuid(0).toString(QUuid::WithoutBraces).toUpper();
        const QString version = uuid(1).toString(QUuid::WithoutBraces).toUpper();
        entries.append({QStringLiteral("Metadata/Properties.plist"),
                        binaryPlist({{QStringLiteral("revision"), QStringLiteral("0::") + version},
                                     {QStringLiteral("documentUUID"), document_},
                                     {QStringLiteral("fileFormatVersion"), QStringLiteral("10.1.8")},
                                     {QStringLiteral("isMultiPage"), false},
                                     {QStringLiteral("shareUUID"), document_},
                                     {QStringLiteral("versionUUID"), version}}),
                        false});
        entries.append({QStringLiteral("Metadata/DocumentIdentifier"), document_.toUtf8(), false});
        entries.append({QStringLiteral("Metadata/BuildVersionHistory.plist"),
                        QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                          "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                                          "<plist version=\"1.0\">\n<array>\n\t<string>OmaShow</string>\n</array>\n</plist>\n"),
                        false});
        if (firstSlide.isNull()) firstSlide = QImage(1024, 576, QImage::Format_RGB32), firstSlide.fill(Qt::white);
        entries.append({QStringLiteral("preview.jpg"), encoded(firstSlide, "JPEG", 85), false});
        entries.append({QStringLiteral("preview-web.jpg"), encoded(firstSlide.scaledToWidth(186, Qt::SmoothTransformation), "JPEG", 85), false});
        entries.append({QStringLiteral("preview-micro.jpg"), encoded(firstSlide.scaledToWidth(44, Qt::SmoothTransformation), "JPEG", 85), false});
        return Zip::write(entries);
    }
};

} // namespace

namespace KeynoteWriter {

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

} // namespace KeynoteWriter

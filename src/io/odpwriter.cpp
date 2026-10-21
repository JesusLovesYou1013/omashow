#include "io/odpwriter.h"

#include "anim/presentation.h"
#include "core/chart.h"
#include "core/design.h"
#include "core/fontswap.h"
#include "core/imageasset.h"
#include "core/mediaasset.h"
#include "core/shape.h"
#include "core/table.h"
#include "core/textruns.h"
#include "io/zip.h"

#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QXmlStreamWriter>
#include <QtMath>

#include <cmath>

namespace {

const char *const kMimePresentation = "application/vnd.oasis.opendocument.presentation";
const char *const kMimeTemplate = "application/vnd.oasis.opendocument.presentation-template";

// Every namespace any part uses, declared on each root.
const char *const kNamespaces =
    " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
    " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
    " xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""
    " xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\""
    " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\""
    " xmlns:xlink=\"http://www.w3.org/1999/xlink\""
    " xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
    " xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\""
    " xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\""
    " xmlns:presentation=\"urn:oasis:names:tc:opendocument:xmlns:presentation:1.0\""
    " xmlns:smil=\"urn:oasis:names:tc:opendocument:xmlns:smil-compatible:1.0\""
    " xmlns:anim=\"urn:oasis:names:tc:opendocument:xmlns:animation:1.0\""
    " xmlns:officeooo=\"http://openoffice.org/2009/office\""
    " xmlns:loext=\"urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0\""
    " office:version=\"1.3\"";

// ---------------------------------------------------------------------------
// A thin hand on QXmlStreamWriter writing a fragment: the parts are put
// together around their roots, which carry the namespaces.

class Xml {
public:
    Xml() : m_writer(&m_bytes) {}
    Xml &start(const char *tag) { m_writer.writeStartElement(QString::fromLatin1(tag)); return *this; }
    Xml &attr(const char *name, const QString &value) { m_writer.writeAttribute(QString::fromLatin1(name), value); return *this; }
    Xml &attr(const char *name, const char *value) { return attr(name, QString::fromLatin1(value)); }
    Xml &attr(const char *name, int value) { return attr(name, QString::number(value)); }
    Xml &text(const QString &words) { m_writer.writeCharacters(words); return *this; }
    Xml &end() { m_writer.writeEndElement(); return *this; }
    Xml &empty(const char *tag) { m_writer.writeEmptyElement(QString::fromLatin1(tag)); return *this; }
    QByteArray bytes() { return m_bytes; }
private:
    QByteArray m_bytes;
    QXmlStreamWriter m_writer;
};

QString esc(QString s) {
    s.replace(QLatin1Char('&'), QLatin1String("&amp;")).replace(QLatin1Char('<'), QLatin1String("&lt;"))
     .replace(QLatin1Char('>'), QLatin1String("&gt;")).replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return s;
}

// A number as ODF writes it: three places at most, no trailing zeros.
QString num(qreal v, int places = 3) {
    QString s = QString::number(v, 'f', places);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0'))) s.chop(1);
        if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    }
    if (s == QLatin1String("-0")) s = QStringLiteral("0");
    return s;
}

QString percent(qreal fraction) { return num(qBound(0.0, fraction, 1.0) * 100, 1) + QLatin1Char('%'); }
QString color(const QColor &c) { return c.name(QColor::HexRgb); }

// Attributes being collected for one properties element.
struct Props {
    QString text;
    Props &set(const char *name, const QString &value) {
        text += QLatin1Char(' ') + QLatin1String(name) + QStringLiteral("=\"") + esc(value) + QLatin1Char('"');
        return *this;
    }
    Props &set(const char *name, const char *value) { return set(name, QString::fromLatin1(value)); }
    QString element(const char *tag, const QString &children = {}) const {
        if (text.isEmpty() && children.isEmpty()) return {};
        const QString open = QLatin1Char('<') + QLatin1String(tag) + text;
        return children.isEmpty() ? open + QStringLiteral("/>") : open + QLatin1Char('>') + children + QStringLiteral("</") + QLatin1String(tag) + QLatin1Char('>');
    }
};

// Automatic styles, one per distinct look: the same look is written once and
// named everywhere it is used.
struct Pool {
    explicit Pool(const QString &namePrefix = {}) : prefix(namePrefix) {}
    QString prefix;    // keeps the master pages' names apart from the pages'
    QHash<QString, QString> byBody;
    QHash<QString, int> counters;
    QString xml;
    // `body` holds \x01 where the name goes.
    QString add(const char *kind, const QString &body) {
        const QString key = QLatin1String(kind) + body;
        auto it = byBody.constFind(key);
        if (it != byBody.constEnd()) return *it;
        const QString name = prefix + QLatin1String(kind) + QString::number(++counters[QLatin1String(kind)]);
        QString out = body;
        out.replace(QChar(1), esc(name));
        xml += out;
        byBody.insert(key, name);
        return name;
    }
    QString style(const char *kind, const char *family, const QString &inner, const QString &extra = {}) {
        return add(kind, QStringLiteral("<style:style style:name=\"\x01\" style:family=\"%1\"%2>%3</style:style>")
                             .arg(QLatin1String(family), extra, inner));
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

QString mimeOfPicture(const QString &extension) {
    if (extension == QLatin1String("png")) return QStringLiteral("image/png");
    if (extension == QLatin1String("jpg")) return QStringLiteral("image/jpeg");
    if (extension == QLatin1String("gif")) return QStringLiteral("image/gif");
    if (extension == QLatin1String("svg")) return QStringLiteral("image/svg+xml");
    if (extension == QLatin1String("bmp")) return QStringLiteral("image/bmp");
    if (extension == QLatin1String("tif")) return QStringLiteral("image/tiff");
    return {};
}

QString mimeOfMedia(const QString &extension, bool video) {
    static const QHash<QString, QString> known = {
        {QStringLiteral("mp4"), QStringLiteral("video/mp4")}, {QStringLiteral("m4v"), QStringLiteral("video/mp4")},
        {QStringLiteral("mov"), QStringLiteral("video/quicktime")}, {QStringLiteral("webm"), QStringLiteral("video/webm")},
        {QStringLiteral("mkv"), QStringLiteral("video/x-matroska")}, {QStringLiteral("avi"), QStringLiteral("video/x-msvideo")},
        {QStringLiteral("mp3"), QStringLiteral("audio/mpeg")}, {QStringLiteral("wav"), QStringLiteral("audio/wav")},
        {QStringLiteral("m4a"), QStringLiteral("audio/mp4")}, {QStringLiteral("ogg"), QStringLiteral("audio/ogg")},
        {QStringLiteral("oga"), QStringLiteral("audio/ogg")}, {QStringLiteral("flac"), QStringLiteral("audio/flac")}};
    return known.value(extension, video ? QStringLiteral("video/mp4") : QStringLiteral("audio/mpeg"));
}

struct Writer {
    const Document &doc;
    OdpWriter::Flavor flavor;
    Warnings warnings;
    std::shared_ptr<Workers::Job> job;
    qreal e = 1;   // EMU per slide unit

    QVector<Zip::Entry> files;      // pictures and films beside the XML
    QStringList manifest;           // their manifest lines
    struct Stored { QString href; QByteArray data; };
    QHash<QString, Stored> stored;  // by identity: one copy of each picture
    int pictureCount = 0, mediaCount = 0;

    Pool content, masters{QStringLiteral("M")};
    Pool *pool = &content;
    QHash<QString, QString> named; // office:styles entries by body
    QHash<QString, int> namedCounters;
    QString namedXml;
    QStringList fontFaces;

    int slide = 0;                  // 1-based while a page is written, for the report
    bool onMaster = false;

    Writer(const Document &document, OdpWriter::Flavor kind) : doc(document), flavor(kind) {
        e = 12192000.0 / qMax(1.0, doc.size.width());
    }

    bool canceled() const { return job && job->canceled; }
    QString cm(qreal units) const { return num(units * e / 360000.0) + QStringLiteral("cm"); }
    QString pt(qreal units) const { return num(units * e / 12700.0, 2) + QStringLiteral("pt"); }

    QString font(const QString &family) {
        if (!fontFaces.contains(family)) fontFaces.append(family);
        return family;
    }

    // A shared definition in office:styles (gradient, dash, marker, tile):
    // `body` holds \x01 where the name goes.
    QString define(const char *kind, const QString &body) {
        auto it = named.constFind(body);
        if (it != named.constEnd()) return *it;
        const QString name = QLatin1String(kind) + QLatin1Char('_') + QString::number(++namedCounters[QLatin1String(kind)]);
        QString out = body;
        out.replace(QChar(1), esc(name));
        namedXml += out;
        named.insert(body, name);
        return name;
    }

    // ---- files

    // A picture stored once per identity; its path in the package.
    QString store(const QString &identity, const QByteArray &data, const QString &extension) {
        const QString key = identity.isEmpty() ? QString::number(pictureCount + 1) + extension : identity + QLatin1Char('/') + extension;
        auto it = stored.constFind(key);
        if (it != stored.constEnd()) return it->href;
        const QString href = QStringLiteral("Pictures/picture%1.%2").arg(++pictureCount).arg(extension);
        stored.insert(key, {href, data});
        if (flavor != OdpWriter::Flat) {
            files.append({href, data, !(extension == QLatin1String("png") || extension == QLatin1String("jpg") || extension == QLatin1String("gif"))});
            manifest.append(QStringLiteral(" <manifest:file-entry manifest:full-path=\"%1\" manifest:media-type=\"%2\"/>\n").arg(href, mimeOfPicture(extension)));
        }
        return href;
    }
    QByteArray dataOf(const QString &href) const {
        for (const auto &s : stored) if (s.href == href) return s.data;
        return {};
    }

    // The link to a stored picture: an href in a package, the bytes themselves
    // in a flat file. The element is left open.
    void linkPicture(Xml &x, const char *tag, const QString &href) {
        x.start(tag);
        if (flavor != OdpWriter::Flat)
            x.attr("xlink:href", href).attr("xlink:type", "simple").attr("xlink:show", "embed").attr("xlink:actuate", "onLoad");
        if (QLatin1String(tag) == QLatin1String("draw:image")) {
            const QString mime = mimeOfPicture(QFileInfo(href).suffix());
            if (!mime.isEmpty()) x.attr("draw:mime-type", mime);
        }
        if (flavor == OdpWriter::Flat)
            x.start("office:binary-data").text(QString::fromLatin1(dataOf(href).toBase64())).end();
    }
    QString linkPictureXml(const char *tag, const QString &name, const QString &href) const {
        Q_UNUSED(name);
        // Built by hand so the name placeholder survives.
        const QString open = QStringLiteral("<%1 draw:name=\"\x01\"").arg(QLatin1String(tag));
        if (flavor != OdpWriter::Flat)
            return open + QStringLiteral(" xlink:href=\"%1\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"/>").arg(esc(href));
        return open + QStringLiteral("><office:binary-data>%1</office:binary-data></%2>").arg(QString::fromLatin1(dataOf(href).toBase64()), QLatin1String(tag));
    }

    // ---- placing

    // Size and place; a turned object turns about its middle, as OmaShow
    // turns it. ODF turns about the top-left corner, anticlockwise, then moves.
    void place(Xml &x, const QRectF &r, qreal rotation) const {
        x.attr("svg:width", cm(qMax(0.0, r.width()))).attr("svg:height", cm(qMax(0.0, r.height())));
        const qreal turn = std::fmod(rotation, 360.0);
        if (qFuzzyIsNull(turn)) { x.attr("svg:x", cm(r.x())).attr("svg:y", cm(r.y())); return; }
        const qreal t = qDegreesToRadians(turn);
        const QPointF c = r.center();
        const qreal hw = r.width() / 2, hh = r.height() / 2;
        const QPointF corner(c.x() - hw * std::cos(t) + hh * std::sin(t), c.y() - hw * std::sin(t) - hh * std::cos(t));
        x.attr("draw:transform", QStringLiteral("rotate (%1) translate (%2 %3)").arg(num(-t, 9), cm(corner.x()), cm(corner.y())));
    }

    void describe(Xml &x, const SceneObject &o) const {
        if (!o.altTitle.isEmpty()) x.start("svg:title").text(o.altTitle).end();
        if (!o.altText.isEmpty()) x.start("svg:desc").text(o.altText).end();
    }

    void common(Xml &x, const SceneObject &o, const QString &style) const {
        x.attr("draw:style-name", style).attr("draw:layer", onMaster ? "backgroundobjects" : "layout");
        if (o.hidden) x.attr("draw:display", "none");
        if (!o.altTitle.isEmpty()) x.attr("draw:name", o.altTitle);
    }

    // ---- graphic properties

    void fill(Props &p, const SceneObject &o) {
        switch (o.fillStyle) {
        case 0:
            p.set("draw:fill", "solid").set("draw:fill-color", color(o.fill));
            if (o.fill.alphaF() * o.opacity < 0.999) p.set("draw:opacity", percent(o.fill.alphaF() * o.opacity));
            break;
        case 1: case 2: {
            // OmaShow's angle runs clockwise from left-to-right; ODF's runs
            // anticlockwise from top-to-bottom. A radial one starts outside.
            const bool radial = o.fillStyle == 2;
            const qreal angle = radial ? 0 : std::fmod(std::fmod(90.0 - o.fillAngle, 360.0) + 360.0, 360.0);
            const QColor from = radial ? o.fillSecondary : o.fill, to = radial ? o.fill : o.fillSecondary;
            const QString name = define("Gradient", QStringLiteral(
                "<draw:gradient draw:name=\"\x01\" draw:style=\"%1\"%2 draw:start-color=\"%3\" draw:end-color=\"%4\""
                " draw:start-intensity=\"100%\" draw:end-intensity=\"100%\" draw:angle=\"%5deg\" draw:border=\"0%\"/>")
                .arg(radial ? QStringLiteral("radial") : QStringLiteral("linear"),
                     radial ? QStringLiteral(" draw:cx=\"50%\" draw:cy=\"50%\"") : QString(), color(from), color(to), num(angle, 1)));
            p.set("draw:fill", "gradient").set("draw:fill-gradient-name", name);
            const qreal alpha = qMax(o.fill.alphaF(), o.fillSecondary.alphaF()) * o.opacity;
            if (alpha < 0.999) p.set("draw:opacity", percent(alpha));
            break;
        }
        case 3: {
            // A pattern goes as a tile of itself; OmaShow counts its patterns from Dense1.
            QImage tile(8, 8, QImage::Format_ARGB32);
            tile.fill(o.fillSecondary);
            {
                QPainter painter(&tile);
                painter.fillRect(tile.rect(), QBrush(o.fill, Qt::BrushStyle(Qt::Dense1Pattern + qBound(0, o.patternStyle, 12))));
            }
            const QString href = store(QStringLiteral("pattern/%1/%2/%3").arg(o.patternStyle).arg(o.fill.rgba()).arg(o.fillSecondary.rgba()),
                                       encoded(tile.scaled(32, 32, Qt::IgnoreAspectRatio, Qt::FastTransformation), "PNG"), QStringLiteral("png"));
            const QString name = define("Pattern", linkPictureXml("draw:fill-image", QString(), href));
            p.set("draw:fill", "bitmap").set("draw:fill-image-name", name).set("style:repeat", "repeat")
             .set("draw:fill-image-width", cm(16)).set("draw:fill-image-height", cm(16));
            if (o.opacity < 0.999) p.set("draw:opacity", percent(o.opacity));
            break;
        }
        case 4:
            warnings.add(QStringLiteral("A shape filled with a picture was given a plain fill"), slide);
            p.set("draw:fill", "solid").set("draw:fill-color", color(o.fill));
            break;
        default: p.set("draw:fill", "none"); break;
        }
    }

    void stroke(Props &p, const SceneObject &o, bool arrows) {
        if (o.strokeWidth <= 0 || o.strokeColor.alpha() == 0) { p.set("draw:stroke", "none"); return; }
        static const char *dashes[] = {
            nullptr,
            "draw:dots1=\"1\" draw:dots1-length=\"400%\" draw:distance=\"200%\"",
            "draw:dots1=\"1\" draw:dots1-length=\"100%\" draw:distance=\"200%\"",
            "draw:dots1=\"1\" draw:dots1-length=\"400%\" draw:dots2=\"1\" draw:dots2-length=\"100%\" draw:distance=\"200%\"",
            "draw:dots1=\"1\" draw:dots1-length=\"400%\" draw:dots2=\"2\" draw:dots2-length=\"100%\" draw:distance=\"200%\""};
        if (o.strokeStyle >= 1 && o.strokeStyle <= 4) {
            const QString name = define("Dash", QStringLiteral("<draw:stroke-dash draw:name=\"\x01\" draw:style=\"rect\" %1/>").arg(QLatin1String(dashes[o.strokeStyle])));
            p.set("draw:stroke", "dash").set("draw:stroke-dash", name);
        } else p.set("draw:stroke", "solid");
        p.set("svg:stroke-width", cm(o.strokeWidth)).set("svg:stroke-color", color(o.strokeColor));
        if (o.strokeColor.alphaF() * o.opacity < 0.999) p.set("svg:stroke-opacity", percent(o.strokeColor.alphaF() * o.opacity));
        p.set("draw:stroke-linejoin", o.strokeJoin == 0 ? "miter" : o.strokeJoin == 2 ? "bevel" : "round");
        p.set("svg:stroke-linecap", o.strokeCap == 0 ? "butt" : o.strokeCap == 2 ? "square" : "round");
        if (arrows && (o.connectorArrowStart || o.connectorArrowEnd)) {
            const QString arrow = define("Arrow", QStringLiteral("<draw:marker draw:name=\"\x01\" svg:viewBox=\"0 0 20 30\" svg:d=\"M10 0l-10 30h20z\"/>"));
            const QString width = cm(qMax(o.strokeWidth * 3, 6.0));
            if (o.connectorArrowStart) p.set("draw:marker-start", arrow).set("draw:marker-start-width", width);
            if (o.connectorArrowEnd) p.set("draw:marker-end", arrow).set("draw:marker-end-width", width);
        }
    }

    void shadow(Props &p, const SceneObject &o) const {
        if (!o.shadowEnabled) { p.set("draw:shadow", "hidden"); return; }
        p.set("draw:shadow", "visible").set("draw:shadow-offset-x", cm(o.shadowX)).set("draw:shadow-offset-y", cm(o.shadowY))
         .set("draw:shadow-color", color(o.shadowColor)).set("draw:shadow-opacity", percent(o.shadowColor.alphaF() * o.opacity));
        if (o.shadowBlur > 0) p.set("loext:shadow-blur", cm(o.shadowBlur));
    }

    QString graphic(const Props &p, const QString &children = {}) {
        return pool->style("gr", "graphic", p.element("style:graphic-properties") + children);
    }

    // ---- shapes

    // The outline as an enhanced path in a viewBox of hundredths of a unit,
    // relative to the object's box.
    QString enhancedPath(const QPainterPath &path, const QRectF &r, bool close, bool svg) const {
        QString out;
        const auto p = [&](qreal px, qreal py) {
            return QString::number(qRound((px - r.x()) * 100)) + QLatin1Char(' ') + QString::number(qRound((py - r.y()) * 100));
        };
        for (int i = 0; i < path.elementCount(); ++i) {
            const auto el = path.elementAt(i);
            if (el.isMoveTo()) {
                if (i > 0 && close) out += QStringLiteral("Z ");
                out += QStringLiteral("M ") + p(el.x, el.y) + QLatin1Char(' ');
            } else if (el.isLineTo()) {
                out += QStringLiteral("L ") + p(el.x, el.y) + QLatin1Char(' ');
            } else if (el.isCurveTo() && i + 2 < path.elementCount()) {
                const auto b = path.elementAt(i + 1), c = path.elementAt(i + 2);
                out += QStringLiteral("C ") + p(el.x, el.y) + QLatin1Char(' ') + p(b.x, b.y) + QLatin1Char(' ') + p(c.x, c.y) + QLatin1Char(' ');
                i += 2;
            }
        }
        if (close && path.elementCount() > 1) out += QStringLiteral("Z ");
        if (!svg) out += QLatin1Char('N');
        return out.trimmed();
    }
    QString viewBox(const QRectF &r) const {
        return QStringLiteral("0 0 %1 %2").arg(qMax(1, qRound(r.width() * 100))).arg(qMax(1, qRound(r.height() * 100)));
    }

    void writeShape(Xml &x, const SceneObject &o) {
        const QPainterPath path = Shape::path(o);
        const bool open = o.connector || (o.fillStyle == 5 && o.shapeKind == 13);
        Props p;
        if (open) p.set("draw:fill", "none"); else fill(p, o);
        stroke(p, o, o.connector);
        shadow(p, o);
        if (open) {
            // A line or connector is a path, which carries arrowheads.
            x.start("draw:path");
            common(x, o, graphic(p));
            place(x, o.rect, o.rotation);
            x.attr("svg:viewBox", viewBox(o.rect)).attr("svg:d", enhancedPath(path, o.rect, false, true));
            describe(x, o);
            x.end();
            return;
        }
        x.start("draw:custom-shape");
        common(x, o, graphic(p));
        place(x, o.rect, o.rotation);
        describe(x, o);
        x.start("draw:enhanced-geometry").attr("svg:viewBox", viewBox(o.rect))
            .attr("draw:enhanced-path", enhancedPath(path, o.rect, o.fillStyle != 5, false)).end();
        x.end();
    }

    // ---- text

    QString textProps(const SceneObject &o, const TextRun *run) {
        Props p;
        const QString family = font(run && !run->fontFamily.isEmpty() ? run->fontFamily : o.fontFamily);
        const QString size = pt(run && run->fontSize > 0 ? run->fontSize : o.fontSize);
        const int weight = qBound(100, qRound((run && run->weight ? run->weight : o.fontWeight) / 100.0) * 100, 900);
        const QString w = weight == 400 ? QStringLiteral("normal") : weight == 700 ? QStringLiteral("bold") : QString::number(weight);
        const bool italic = run && run->italic ? run->italic == 1 : o.italic;
        const bool underline = run && run->underline ? run->underline == 1 : o.underline;
        const bool strike = run && run->strike ? run->strike == 1 : o.strike;
        const QColor c = run && run->color.isValid() ? run->color : o.textColor;
        QString lang = run && !run->language.isEmpty() ? run->language : !o.language.isEmpty() ? o.language : doc.language;
        if (lang.isEmpty()) lang = QStringLiteral("en_GB");
        p.set("style:font-name", family).set("style:font-name-asian", family).set("style:font-name-complex", family);
        p.set("fo:font-size", size).set("style:font-size-asian", size).set("style:font-size-complex", size);
        p.set("fo:font-weight", w).set("style:font-weight-asian", w).set("style:font-weight-complex", w);
        const char *style = italic ? "italic" : "normal";
        p.set("fo:font-style", style).set("style:font-style-asian", style).set("style:font-style-complex", style);
        p.set("style:text-underline-style", underline ? "solid" : "none");
        if (underline) p.set("style:text-underline-width", "auto").set("style:text-underline-color", "font-color");
        p.set("style:text-line-through-style", strike ? "solid" : "none");
        if (run && run->baseline) p.set("style:text-position", run->baseline == 1 ? "super 58%" : "sub 58%");
        p.set("fo:color", color(c));
        if (c.alphaF() * o.opacity < 0.999) p.set("loext:opacity", percent(c.alphaF() * o.opacity));
        p.set("fo:letter-spacing", qFuzzyIsNull(o.letterSpacing) ? QStringLiteral("normal") : cm(o.letterSpacing));
        p.set("fo:text-transform", o.uppercase ? "uppercase" : "none");
        const QStringList parts = lang.split(QLatin1Char('_'));
        p.set("fo:language", parts.value(0));
        if (parts.size() > 1) p.set("fo:country", parts.value(1));
        return pool->style("T", "text", p.element("style:text-properties"));
    }

    // OmaShow's paragraph spacing is the gap after a paragraph, none after the last.
    QString paragraphStyle(const SceneObject &o, bool listed, bool last) {
        Props p;
        p.set("fo:text-align", o.textAlign == 1 ? "center" : o.textAlign == 2 ? "end" : o.textAlign == 3 ? "justify" : "start");
        p.set("fo:line-height", num(o.lineHeight > 0 ? o.lineHeight : 100, 1) + QLatin1Char('%'));
        p.set("fo:margin-top", "0cm").set("fo:margin-bottom", last ? QStringLiteral("0cm") : cm(qMax(0.0, o.paragraphSpacing)));
        if (!listed) p.set("fo:margin-left", cm(qMax(0.0, o.textIndent))).set("fo:margin-right", "0cm").set("fo:text-indent", "0cm");
        p.set("style:tab-stop-distance", cm(o.tabStop > 0 ? o.tabStop : o.fontSize * 4));
        p.set("style:writing-mode", o.direction == 2 ? "rl-tb" : o.direction == 1 ? "lr-tb" : "page");
        return pool->style("P", "paragraph", p.element("style:paragraph-properties"));
    }

    QString listStyle(const SceneObject &o) {
        // As OmaShow lays a list out: the words of each level 1.2 type sizes
        // further in, the marker a space before them.
        QString levels;
        const qreal step = o.fontSize * 1.2;
        const int kind = o.listStyle;
        const qreal label = o.fontSize * (kind == 1 || kind == 3 || kind == 4 ? 0.6 : 1.1);
        for (int level = 1; level <= 9; ++level) {
            const QString spacing = QStringLiteral("<style:list-level-properties text:space-before=\"%1\" text:min-label-width=\"%2\"/>")
                                        .arg(cm(qMax(0.0, step * level - label)), cm(label));
            if (kind == 1 || kind == 3 || kind == 4) {
                // Disc, then circle, then square, as OmaShow nests them.
                const QChar bullet = kind == 3 ? QChar(0x25E6) : kind == 4 ? QChar(0x25A0)
                                   : level == 1 ? QChar(0x2022) : level == 2 ? QChar(0x25E6) : QChar(0x25A0);
                levels += QStringLiteral("<text:list-level-style-bullet text:level=\"%1\" text:bullet-char=\"%2\">%3<style:text-properties fo:font-family=\"OpenSymbol\" fo:font-size=\"%4\"/></text:list-level-style-bullet>")
                              .arg(level).arg(bullet).arg(spacing, bullet == QChar(0x25A0) ? QStringLiteral("70%") : QStringLiteral("120%"));
            } else {
                const char *format = kind == 5 ? "a" : kind == 6 ? "A" : kind == 7 ? "i" : kind == 8 ? "I" : "1";
                levels += QStringLiteral("<text:list-level-style-number text:level=\"%1\" style:num-format=\"%2\" style:num-suffix=\".\" text:start-value=\"%3\">%4<style:text-properties fo:font-size=\"100%\"/></text:list-level-style-number>")
                              .arg(level).arg(QLatin1String(format)).arg(level == 1 ? qMax(1, o.listStart) : 1).arg(spacing);
            }
        }
        return pool->add("L", QStringLiteral("<text:list-style style:name=\"\x01\">%1</text:list-style>").arg(levels));
    }

    // Words with their spacing kept: ODF folds runs of spaces into one.
    void words(Xml &x, const QString &s, bool &afterSpace) const {
        QString pending;
        int spaces = 0;
        const auto flushSpaces = [&]() {
            if (!spaces) return;
            if (!pending.isEmpty()) { x.text(pending); pending.clear(); }
            x.start("text:s");
            if (spaces > 1) x.attr("text:c", spaces);
            x.end();
            spaces = 0;
        };
        for (const QChar c : s) {
            if (c == QLatin1Char(' ')) {
                if (afterSpace) ++spaces;
                else { pending += c; afterSpace = true; }
            } else if (c == QLatin1Char('\t') || c == QChar(0x2028) || c == QLatin1Char('\v')) {
                flushSpaces();
                if (!pending.isEmpty()) { x.text(pending); pending.clear(); }
                x.empty(c == QLatin1Char('\t') ? "text:tab" : "text:line-break");
                afterSpace = true;
            } else {
                flushSpaces();
                pending += c;
                afterSpace = false;
            }
        }
        flushSpaces();
        if (!pending.isEmpty()) x.text(pending);
    }

    QString linkOf(const SceneObject &o) const {
        if (o.linkKind < 1 || o.linkKind > 3 || o.linkTarget.isEmpty()) return {};
        if (o.linkKind == 2) return QStringLiteral("mailto:") + o.linkTarget;
        if (o.linkKind == 3) {
            for (int i = 0; i < doc.slides.size(); ++i)
                if (doc.slides[i].id == o.linkTarget) return QStringLiteral("#page%1").arg(i + 1);
            return {};
        }
        return o.linkTarget;
    }

    // The paragraphs of a text object or a table cell.
    void writeParagraphs(Xml &x, const SceneObject &o) {
        if (o.textKind == 1) warnings.add(QStringLiteral("An equation was written as its source text"), slide);
        if (o.textKind == 2) warnings.add(QStringLiteral("Words on a path were written as an ordinary text box"), slide);
        const auto runs = TextRuns::tidy(o.runs, o.text.size());
        const QString link = linkOf(o);
        const QString list = o.listStyle > 0 ? listStyle(o) : QString();
        int depth = 0;   // open text:list elements, each with an open item
        int position = 0;
        const QStringList paragraphs = o.text.split(QLatin1Char('\n'));
        for (int index = 0; index < paragraphs.size(); ++index) {
            const QString &paragraph = paragraphs[index];
            const QString style = paragraphStyle(o, o.listStyle > 0, index + 1 == paragraphs.size());
            int level = 0;
            QString line = paragraph;
            if (o.listStyle > 0) while (line.startsWith(QLatin1Char('\t'))) { line.remove(0, 1); ++level; }
            level = qMin(level, 8);
            const int skipped = paragraph.size() - line.size();
            if (o.listStyle > 0) {
                const int want = level + 1;
                if (depth == 0) { x.start("text:list").attr("text:style-name", list); x.start("text:list-item"); depth = 1; }
                else if (want <= depth) {
                    while (depth > want) { x.end(); x.end(); --depth; }
                    x.end(); x.start("text:list-item");
                }
                while (depth < want) { x.start("text:list"); x.start("text:list-item"); ++depth; }
            }
            x.start("text:p").attr("text:style-name", style);
            if (!link.isEmpty()) x.start("text:a").attr("xlink:href", link).attr("xlink:type", "simple");
            const int start = position + skipped, end = position + paragraph.size();
            bool afterSpace = true;
            int at = start;
            if (at == end) {
                // An empty paragraph still has the box's size, so its height is right.
                x.start("text:span").attr("text:style-name", textProps(o, nullptr)).end();
            }
            while (at < end) {
                const TextRun *run = TextRuns::at(runs, at);
                int stop = end;
                if (run) stop = qMin(end, run->start + run->length);
                for (const auto &r : runs) if (r.start > at && r.start < stop) stop = r.start;
                x.start("text:span").attr("text:style-name", textProps(o, run));
                words(x, o.text.mid(at, stop - at), afterSpace);
                x.end();
                at = stop;
            }
            if (!link.isEmpty()) x.end();
            x.end(); // p
            position += paragraph.size() + 1;
        }
        while (depth > 0) { x.end(); x.end(); --depth; }
    }

    void writeText(Xml &x, const SceneObject &o) {
        Props p;
        p.set("draw:fill", "none").set("draw:stroke", "none");
        shadow(p, o);
        p.set("draw:textarea-horizontal-align", "justify")
         .set("draw:textarea-vertical-align", o.verticalAlign == 1 ? "middle" : o.verticalAlign == 2 ? "bottom" : "top")
         .set("draw:auto-grow-height", o.textFit == 3 ? "true" : "false").set("draw:auto-grow-width", "false")
         .set("draw:fit-to-size", "false").set("style:shrink-to-fit", o.textFit == 1 ? "true" : "false")
         .set("fo:min-height", "0cm").set("fo:min-width", "0cm")
         .set("fo:padding-top", "0cm").set("fo:padding-bottom", "0cm").set("fo:padding-left", "0cm").set("fo:padding-right", "0cm")
         .set("fo:wrap-option", "wrap");
        QString columns;
        if (o.columns > 1)
            columns = QStringLiteral("<style:columns fo:column-count=\"%1\" fo:column-gap=\"%2\"/>")
                          .arg(o.columns).arg(cm(o.columnGap > 0 ? o.columnGap : o.fontSize * qMax(1.0, o.lineHeight / 100.0)));
        Props glow;
        if (o.textGlowRadius > 0 && o.textGlowColor.alpha() > 0)
            glow.set("loext:glow-text-radius", cm(o.textGlowRadius)).set("loext:glow-text-color", color(o.textGlowColor))
                .set("loext:glow-text-transparency", percent(1.0 - o.textGlowColor.alphaF() * o.opacity));
        const QString style = pool->style("gr", "graphic", p.element("style:graphic-properties", columns) + glow.element("style:text-properties"));
        x.start("draw:custom-shape");
        common(x, o, style);
        place(x, o.rect, o.rotation);
        describe(x, o);
        writeParagraphs(x, o);
        const QString box = viewBox(o.rect);
        const QString w = box.section(QLatin1Char(' '), 2, 2), h = box.section(QLatin1Char(' '), 3, 3);
        x.start("draw:enhanced-geometry").attr("svg:viewBox", box)
            .attr("draw:enhanced-path", QStringLiteral("M 0 0 L %1 0 %1 %2 0 %2 Z N").arg(w, h)).end();
        x.end();
    }

    // ---- pictures

    // The picture as OmaShow draws it into its box, for when the stored
    // bytes alone would not look the same (crop, fit, adjustments, mask, SVG
    // drawn by its own rules).
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
        ImageAsset::paint(painter, flat);
        return out;
    }

    void writePicture(Xml &x, const SceneObject &o) {
        if ((o.image.isNull() && o.vectorPicture.isNull()) || o.rect.isEmpty()) {
            warnings.add(QStringLiteral("A picture had no data to write"), slide);
            return;
        }
        const bool svg = o.imageFormat.toLower() == QLatin1String("svg");
        const QSizeF natural = ImageAsset::size(o);
        const QRectF source = ImageAsset::sourceRect(o);
        QSizeF target = o.rect.size();
        if (o.imageMode == 0) { target = source.size(); target.scale(o.rect.size(), Qt::KeepAspectRatio); }
        const bool whole = qAbs(source.x()) < 0.5 && qAbs(source.y()) < 0.5 && qAbs(source.width() - natural.width()) < 0.5 &&
                           qAbs(source.height() - natural.height()) < 0.5;
        const bool fills = qAbs(target.width() - o.rect.width()) < 0.5 && qAbs(target.height() - o.rect.height()) < 0.5;
        const bool adjusted = !qFuzzyIsNull(o.imageBrightness) || !qFuzzyCompare(o.imageContrast, 1.0) ||
                              !qFuzzyCompare(o.imageSaturation, 1.0) || !qFuzzyIsNull(o.imageTintAmount);
        QString extension = o.imageFormat.toLower();
        if (extension == QLatin1String("jpeg")) extension = QStringLiteral("jpg");
        if (extension == QLatin1String("tiff")) extension = QStringLiteral("tif");
        QString href;
        if (whole && fills && !adjusted && o.imageMask == 0 && !o.imageData.isEmpty() && !mimeOfPicture(extension).isEmpty()) {
            href = store(o.imageId, o.imageData, extension);
        } else if (whole && fills && !adjusted && o.imageMask == 0 && !svg && !o.image.isNull()) {
            href = store(o.imageId, encoded(o.image, "PNG"), QStringLiteral("png"));
        } else {
            const qreal perUnit = svg ? 2.0 : source.width() / qMax(1.0, target.width());
            const qreal longest = qMax(o.rect.width(), o.rect.height());
            const qreal density = qBound(1.0, perUnit, 4096.0 / qMax(1.0, longest));
            const QImage image = drawn(o, density);
            const bool photo = extension == QLatin1String("jpg") && o.imageMask == 0 && fills;
            const QString key = o.imageId + QStringLiteral("/drawn/%1/%2/%3/%4/%5/%6/%7")
                                    .arg(o.rect.width()).arg(o.rect.height()).arg(source.x()).arg(source.y()).arg(source.width())
                                    .arg(o.imageMode).arg(o.imageMask) + QString::number(o.imageBrightness) + QString::number(o.imageContrast) +
                                QString::number(o.imageSaturation) + QString::number(o.imageTintAmount) + o.imageTint.name();
            href = photo ? store(key, encoded(image.convertToFormat(QImage::Format_RGB32), "JPEG", 92), QStringLiteral("jpg"))
                         : store(key, encoded(image, "PNG"), QStringLiteral("png"));
        }
        Props p;
        p.set("draw:stroke", "none").set("draw:fill", "none");
        shadow(p, o);
        if (o.opacity < 0.999) p.set("draw:image-opacity", percent(o.opacity));
        p.set("fo:clip", "rect(0cm, 0cm, 0cm, 0cm)").set("draw:color-mode", "standard");
        x.start("draw:frame");
        common(x, o, graphic(p));
        place(x, o.rect, o.rotation);
        linkPicture(x, "draw:image", href);
        x.start("text:p").end();
        x.end(); // image
        describe(x, o);
        x.end(); // frame
        if (o.strokeWidth > 0) {
            // The outline goes over the picture, in the picture's own shape.
            SceneObject outline;
            outline.id = o.id;
            outline.rect = o.rect;
            outline.rotation = o.rotation;
            outline.opacity = o.opacity;
            outline.shapeKind = o.imageMask == 1 ? 1 : o.imageMask == 3 ? 5 : o.imageMask == 4 ? 7 : 0;
            outline.cornerRadius = o.imageMask == 2 ? (o.cornerRadius > 0 ? o.cornerRadius : qMin(o.rect.width(), o.rect.height()) * .1) : 0;
            outline.fillStyle = 5;
            outline.strokeColor = o.strokeColor;
            outline.strokeWidth = o.strokeWidth;
            outline.strokeStyle = o.strokeStyle;
            outline.strokeJoin = o.strokeJoin;
            outline.strokeCap = o.strokeCap;
            writeShape(x, outline);
        }
    }

    // A picture of something OmaShow draws itself (a chart).
    void writeSnapshot(Xml &x, const SceneObject &o, void (*paint)(QPainter &, const SceneObject &)) {
        if (o.rect.isEmpty()) return;
        const qreal density = qBound(1.0, 2.0, 4096.0 / qMax(o.rect.width(), o.rect.height()));
        QImage image(qMax(1, qRound(o.rect.width() * density)), qMax(1, qRound(o.rect.height() * density)), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setRenderHint(QPainter::TextAntialiasing);
            painter.scale(density, density);
            painter.translate(-o.rect.topLeft());
            SceneObject flat = o;
            flat.rotation = 0;
            paint(painter, flat);
        }
        const QString href = store(QString(), encoded(image, "PNG"), QStringLiteral("png"));
        Props p;
        p.set("draw:stroke", "none").set("draw:fill", "none").set("draw:shadow", "hidden");
        if (o.opacity < 0.999) p.set("draw:image-opacity", percent(o.opacity));
        x.start("draw:frame");
        common(x, o, graphic(p));
        place(x, o.rect, o.rotation);
        linkPicture(x, "draw:image", href);
        x.start("text:p").end();
        x.end();
        describe(x, o);
        x.end();
    }

    // ---- films and sounds

    void writeMedia(Xml &x, const SceneObject &o) {
        QImage poster = MediaAsset::frameAt(o, qMax(0.0, o.mediaTrimStart));
        if (poster.isNull()) { poster = QImage(64, 36, QImage::Format_RGB32); poster.fill(QColor(30, 30, 30)); }
        const QString posterHref = store(o.mediaId + QStringLiteral("/poster"), encoded(poster, "PNG"), QStringLiteral("png"));
        QString href;
        if (!o.mediaData.isEmpty() && flavor != OdpWriter::Flat) {
            QString extension = QFileInfo(o.mediaName).suffix().toLower();
            if (extension.isEmpty()) extension = o.mediaContainer.section(QLatin1Char(','), 0, 0).toLower();
            if (extension.isEmpty() || extension.size() > 5) extension = o.mediaVideo ? QStringLiteral("mp4") : QStringLiteral("m4a");
            const QString key = o.mediaId + QStringLiteral("/media");
            auto it = stored.constFind(key);
            if (it == stored.constEnd()) {
                href = QStringLiteral("Media/media%1.%2").arg(++mediaCount).arg(extension);
                stored.insert(key, {href, QByteArray()});
                files.append({href, o.mediaData, false});
                manifest.append(QStringLiteral(" <manifest:file-entry manifest:full-path=\"%1\" manifest:media-type=\"%2\"/>\n").arg(href, mimeOfMedia(extension, o.mediaVideo)));
            } else href = it->href;
        } else if (!o.mediaPath.isEmpty()) {
            href = QUrl::fromLocalFile(o.mediaPath).toString();
            warnings.add(flavor == OdpWriter::Flat && !o.mediaData.isEmpty()
                             ? QStringLiteral("A flat OpenDocument file can't hold a film; it points at the film's file on this computer")
                             : QStringLiteral("A linked film points at its file on this computer; embed it to travel"), slide);
        } else {
            warnings.add(QStringLiteral("A film a flat OpenDocument file can't hold was left as its first frame"), slide);
        }
        if (o.mediaTrimStart > 0 || o.mediaTrimEnd > 0) warnings.add(QStringLiteral("A film's trim was left off; the whole file plays"), slide);
        Props p;
        p.set("draw:stroke", "none").set("draw:fill", "none");
        shadow(p, o);
        x.start("draw:frame");
        common(x, o, graphic(p));
        place(x, o.rect, o.rotation);
        if (!href.isEmpty()) {
            x.start("draw:plugin").attr("xlink:href", href).attr("xlink:type", "simple").attr("xlink:show", "embed").attr("xlink:actuate", "onLoad")
                .attr("draw:mime-type", "application/vnd.sun.star.media");
            const auto param = [&](const char *name, const QString &value) { x.start("draw:param").attr("draw:name", name).attr("draw:value", value).end(); };
            param("Loop", o.mediaLoops == 1 ? QStringLiteral("false") : QStringLiteral("true"));
            param("Mute", o.mediaVolume <= 0 ? QStringLiteral("true") : QStringLiteral("false"));
            param("VolumeDB", QString::number(o.mediaVolume > 0 ? qRound(20 * std::log10(qMin(1.0, o.mediaVolume))) : 0));
            param("Zoom", QStringLiteral("fit"));
            x.end();
        }
        linkPicture(x, "draw:image", posterHref);
        x.start("text:p").end();
        x.end();
        describe(x, o);
        x.end();
    }

    // ---- tables

    void writeTable(Xml &x, const SceneObject &o) {
        const auto &t = o.table;
        const int rows = t.rows.size(), columns = t.columns.size();
        if (rows < 1 || columns < 1 || t.cells.size() < rows * columns) return;
        if (!qFuzzyIsNull(o.rotation)) warnings.add(QStringLiteral("A turned table was written upright"), slide);
        qreal columnTotal = 0, rowTotal = 0;
        for (qreal c : t.columns) columnTotal += c;
        for (qreal r : t.rows) rowTotal += r;
        Props frame;
        frame.set("draw:stroke", "none").set("draw:fill", "none").set("draw:shadow", "hidden");
        x.start("draw:frame");
        common(x, o, graphic(frame));
        place(x, o.rect, 0);
        x.start("table:table");
        if (t.headerRows > 0) x.attr("table:use-first-row-styles", "true");
        if (t.headerColumns > 0) x.attr("table:use-first-column-styles", "true");
        if (t.banded) x.attr("table:use-banding-rows-styles", "true");
        for (qreal c : t.columns) {
            const QString style = pool->style("co", "table-column",
                QStringLiteral("<style:table-column-properties style:column-width=\"%1\"/>").arg(cm(o.rect.width() * c / qMax(0.0001, columnTotal))));
            x.start("table:table-column").attr("table:style-name", style).end();
        }
        for (int r = 0; r < rows; ++r) {
            const QString rowStyle = pool->style("ro", "table-row",
                QStringLiteral("<style:table-row-properties style:row-height=\"%1\"/>").arg(cm(o.rect.height() * t.rows[r] / qMax(0.0001, rowTotal))));
            x.start("table:table-row").attr("table:style-name", rowStyle);
            for (int c = 0; c < columns; ++c) {
                const auto &cell = t.cells[r * columns + c];
                if (cell.rowSpan == 0 && cell.columnSpan == 0) { x.empty("table:covered-table-cell"); continue; }
                SceneObject words = Table::textObject(o, r, c);
                words.id = cell.id;
                words.linkKind = 0;
                words.opacity = o.opacity;
                const QColor border = QColor(cell.style.value(QStringLiteral("borderColor"), t.borderColor.name(QColor::HexArgb)).toString());
                const qreal width = cell.style.value(QStringLiteral("borderWidth"), t.borderWidth).toDouble();
                Props g;
                if (words.fill.isValid() && words.fill.alpha() > 0) {
                    g.set("draw:fill", "solid").set("draw:fill-color", color(words.fill));
                    if (words.fill.alphaF() < 0.999) g.set("draw:opacity", percent(words.fill.alphaF()));
                } else g.set("draw:fill", "none");
                g.set("draw:textarea-vertical-align", words.verticalAlign == 1 ? "middle" : words.verticalAlign == 2 ? "bottom" : "top")
                 .set("fo:padding-top", cm(t.padding)).set("fo:padding-bottom", cm(t.padding))
                 .set("fo:padding-left", cm(t.padding)).set("fo:padding-right", cm(t.padding));
                Props b;
                b.set("fo:border", width > 0 && border.isValid() && border.alpha() > 0
                                       ? QStringLiteral("%1 solid %2").arg(cm(width), color(border)) : QStringLiteral("none"));
                const QString cellStyle = pool->style("ce", "table-cell", g.element("style:graphic-properties") + b.element("style:paragraph-properties"));
                x.start("table:table-cell").attr("table:style-name", cellStyle);
                if (cell.columnSpan > 1) x.attr("table:number-columns-spanned", cell.columnSpan);
                if (cell.rowSpan > 1) x.attr("table:number-rows-spanned", cell.rowSpan);
                writeParagraphs(x, words);
                x.end();
            }
            x.end(); // row
        }
        x.end(); // table
        describe(x, o);
        x.end(); // frame
    }

    void writeObjects(Xml &x, const QVector<SceneObject> &objects) {
        for (const auto &o : objects) {
            if (canceled()) return;
            switch (o.type) {
            case ObjectType::Rect: writeShape(x, o); break;
            case ObjectType::Text: writeText(x, o); break;
            case ObjectType::Image: writePicture(x, o); break;
            case ObjectType::Media: writeMedia(x, o); break;
            case ObjectType::Table: writeTable(x, o); break;
            case ObjectType::Chart:
                warnings.add(QStringLiteral("A chart was written as a picture of itself"), slide);
                writeSnapshot(x, o, &Chart::paint);
                break;
            }
        }
    }

    // ---- pages

    void transition(Props &p, int index, const Slide &s) {
        const int kind = Presentation::transitionKind(doc, index);
        const qreal seconds = Presentation::transitionSeconds(doc, index);
        if (s.advanceAfter >= 0)
            p.set("presentation:transition-type", "automatic").set("presentation:duration", QStringLiteral("PT%1S").arg(num(s.advanceAfter)));
        if (kind == Presentation::Cut) return;
        p.set("presentation:transition-speed", seconds < 0.6 ? "fast" : seconds < 0.9 ? "medium" : "slow");
        // Where the new slide comes from: OmaShow names the way things move.
        const char *from = s.transitionDirection == 1 ? "fromLeft" : s.transitionDirection == 2 ? "fromBottom"
                         : s.transitionDirection == 3 ? "fromTop" : "fromRight";
        switch (kind) {
        case Presentation::Push: p.set("smil:type", "pushWipe").set("smil:subtype", from); break;
        case Presentation::Cover: p.set("smil:type", "slideWipe").set("smil:subtype", from); break;
        case Presentation::Uncover: p.set("smil:type", "slideWipe").set("smil:subtype", from).set("smil:direction", "reverse"); break;
        case Presentation::FadeThroughBlack: p.set("smil:type", "fade").set("smil:subtype", "fadeOverColor").set("smil:fadeColor", "#000000"); break;
        case Presentation::Zoom: p.set("smil:type", "zoom").set("smil:subtype", "zoomIn"); break;
        case Presentation::Whirl: p.set("smil:type", "zoom").set("smil:subtype", "rotateIn"); break;
        case Presentation::Morph:
            warnings.add(QStringLiteral("The morph transition was written as a fade"), index + 1);
            p.set("smil:type", "fade").set("smil:subtype", "crossfade");
            break;
        default: p.set("smil:type", "fade").set("smil:subtype", "crossfade"); break;
        }
    }

    void writeTransitionTiming(Xml &x, int index) {
        const int kind = Presentation::transitionKind(doc, index);
        if (kind == Presentation::Cut) return;
        Props p;
        transition(p, index, doc.slides[index]);
        const QString attrs = p.text;
        const auto value = [&](const char *name) {
            const QString key = QLatin1Char(' ') + QLatin1String(name) + QStringLiteral("=\"");
            const int at = attrs.indexOf(key);
            if (at < 0) return QString();
            const int from = at + key.size();
            return attrs.mid(from, attrs.indexOf(QLatin1Char('"'), from) - from);
        };
        x.start("anim:par").attr("presentation:node-type", "timing-root");
        x.start("anim:par").attr("smil:begin", QStringLiteral("page%1.begin").arg(index + 1));
        x.start("anim:transitionFilter").attr("smil:dur", num(Presentation::transitionSeconds(doc, index)) + QLatin1Char('s'))
            .attr("smil:type", value("smil:type")).attr("smil:subtype", value("smil:subtype"));
        if (!value("smil:direction").isEmpty()) x.attr("smil:direction", value("smil:direction"));
        if (!value("smil:fadeColor").isEmpty()) x.attr("smil:fadeColor", value("smil:fadeColor"));
        x.end();
        x.end();
        x.end();
    }

    void writeNotes(Xml &x, const QString &notes, int number) {
        const QString page = pool->style("dp", "drawing-page", QStringLiteral("<style:drawing-page-properties presentation:display-header=\"false\" presentation:display-footer=\"false\"/>"));
        Props p;
        p.set("draw:fill", "none").set("draw:stroke", "none").set("draw:shadow", "hidden");
        const QString frameStyle = graphic(p);
        SceneObject words;
        words.text = notes;
        words.fontSize = 12 * 12700.0 / e;
        words.fontFamily = doc.theme.fonts.value(QStringLiteral("body"), QStringLiteral("Inter"));
        words.textColor = Qt::black;
        words.verticalAlign = 0;
        x.start("presentation:notes").attr("draw:style-name", page);
        x.start("draw:page-thumbnail").attr("draw:style-name", frameStyle).attr("draw:layer", "layout")
            .attr("svg:width", "14cm").attr("svg:height", num(14.0 * doc.size.height() / qMax(1.0, doc.size.width())) + QStringLiteral("cm"))
            .attr("svg:x", "3.5cm").attr("svg:y", "2cm").attr("draw:page-number", number).attr("presentation:class", "page").end();
        x.start("draw:frame").attr("draw:style-name", frameStyle).attr("draw:layer", "layout").attr("svg:width", "17cm").attr("svg:height", "12.5cm")
            .attr("svg:x", "2cm").attr("svg:y", "13cm").attr("presentation:class", "notes");
        x.start("draw:text-box");
        writeParagraphs(x, words);
        x.end();
        x.end();
        x.end();
    }

    void writeComments(Xml &x, const Slide &s) {
        for (const auto &c : doc.comments) {
            if (c.slideId != s.id) continue;
            QPointF where(10, 10);
            if (const auto *o = s.find(c.objectId)) where = o->rect.topLeft();
            x.start("officeooo:annotation").attr("svg:x", cm(where.x())).attr("svg:y", cm(where.y())).attr("svg:width", "0.6cm").attr("svg:height", "0.6cm");
            x.start("dc:creator").text(c.author.isEmpty() ? QStringLiteral("Unknown") : c.author).end();
            if (!c.created.isEmpty()) x.start("dc:date").text(c.created).end();
            QString text = c.text;
            if (!c.parentId.isEmpty()) text = QStringLiteral("Re: ") + text;
            if (c.resolved) text += QStringLiteral(" [resolved]");
            for (const auto &line : text.split(QLatin1Char('\n'))) x.start("text:p").text(line).end();
            x.end();
        }
    }

    QString masterOf(const Slide &s) const {
        const auto *layout = Design::layout(doc, s.layoutId);
        for (int i = 0; i < doc.masters.size(); ++i)
            if (layout && doc.masters[i].id == layout->masterId) return QStringLiteral("M%1").arg(i + 1);
        return QStringLiteral("M1");
    }

    void writePage(Xml &x, int index) {
        const auto &s = doc.slides[index];
        slide = index + 1;
        const Slide resolved = Design::resolve(doc, index);
        Props p;
        p.set("draw:fill", "solid").set("draw:fill-color", color(resolved.background)).set("draw:background-size", "full");
        p.set("presentation:background-visible", "true").set("presentation:background-objects-visible", s.showMasterObjects ? "true" : "false");
        p.set("presentation:display-footer", "false").set("presentation:display-page-number", "false").set("presentation:display-date-time", "false");
        if (s.skipped) p.set("presentation:visibility", "hidden");
        transition(p, index, s);
        const QString style = pool->style("dp", "drawing-page", p.element("style:drawing-page-properties"));
        x.start("draw:page").attr("draw:name", QStringLiteral("page%1").arg(slide)).attr("draw:style-name", style)
            .attr("draw:master-page-name", masterOf(s)).attr("draw:id", QStringLiteral("page%1").arg(slide)).attr("xml:id", QStringLiteral("page%1").arg(slide));
        QVector<SceneObject> objects;
        // The master's own shapes are on the master page.
        for (const auto &o : resolved.objects) if (!o.id.startsWith(QLatin1String("@master/"))) objects.append(o);
        writeObjects(x, objects);
        writeComments(x, s);
        writeTransitionTiming(x, index);
        if (!s.notes.trimmed().isEmpty()) writeNotes(x, s.notes, slide);
        x.end();
        if (!s.timeline.steps.isEmpty() || !s.pptxInteractive.isEmpty())
            warnings.add(QStringLiteral("Builds (animations) were left out for now"), slide);
    }

    // ---- masters

    QString masterPages() {
        pool = &masters;
        onMaster = true;
        slide = 0;
        QString out;
        QVector<Master> list = doc.masters;
        if (list.isEmpty()) {
            Master plain;
            plain.id = QStringLiteral("default");
            plain.name = QStringLiteral("Default");
            plain.background = doc.theme.colors.value(QStringLiteral("background"), QColor(12, 16, 24));
            list.append(plain);
        }
        for (int i = 0; i < list.size(); ++i) {
            const auto &m = list[i];
            Props p;
            p.set("draw:fill", "solid").set("draw:fill-color", color(doc.theme.colors.value(m.backgroundToken, m.background))).set("draw:background-size", "full");
            const QString style = pool->style("dp", "drawing-page", p.element("style:drawing-page-properties"));
            Xml x;
            x.start("style:master-page").attr("style:name", QStringLiteral("M%1").arg(i + 1))
                .attr("style:display-name", m.name.isEmpty() ? QStringLiteral("Master %1").arg(i + 1) : m.name)
                .attr("style:page-layout-name", "PM1").attr("draw:style-name", style);
            QVector<SceneObject> objects;
            for (const auto &o : m.objects) objects.append(Design::themed(doc.theme, o));
            writeObjects(x, objects);
            x.end();
            out += QString::fromUtf8(x.bytes());
        }
        onMaster = false;
        pool = &content;
        return out;
    }

    // ---- the deck

    QString fontDecls() const {
        QString out = QStringLiteral("<office:font-face-decls>");
        for (const auto &f : fontFaces)
            out += QStringLiteral("<style:font-face style:name=\"%1\" svg:font-family=\"%2\"/>").arg(esc(f), esc(QLatin1Char('\'') + f + QLatin1Char('\'')));
        return out + QStringLiteral("</office:font-face-decls>");
    }

    QString pageLayout() const {
        const qreal w = doc.size.width() * e / 360000.0, h = doc.size.height() * e / 360000.0;
        return QStringLiteral("<style:page-layout style:name=\"PM1\"><style:page-layout-properties fo:margin-top=\"0cm\" fo:margin-bottom=\"0cm\""
                              " fo:margin-left=\"0cm\" fo:margin-right=\"0cm\" fo:page-width=\"%1cm\" fo:page-height=\"%2cm\" style:print-orientation=\"%3\"/>"
                              "</style:page-layout>").arg(num(w), num(h), w >= h ? QStringLiteral("landscape") : QStringLiteral("portrait"));
    }

    static QString defaultStyles() {
        // Without these every application would fill and outline shapes in its own colours.
        return QStringLiteral("<style:default-style style:family=\"graphic\"><style:graphic-properties draw:fill=\"none\" draw:stroke=\"none\" draw:shadow=\"hidden\"/>"
                              "<style:paragraph-properties fo:margin-top=\"0cm\" fo:margin-bottom=\"0cm\"/></style:default-style>");
    }

    static QString layers() {
        return QStringLiteral("<draw:layer-set><draw:layer draw:name=\"layout\"/><draw:layer draw:name=\"background\"/>"
                              "<draw:layer draw:name=\"backgroundobjects\"/><draw:layer draw:name=\"controls\"/><draw:layer draw:name=\"measurelines\"/></draw:layer-set>");
    }

    QString settings() const {
        if (doc.shows.isEmpty()) return {};
        QString out = QStringLiteral("<presentation:settings>");
        for (const auto &show : doc.shows) {
            QStringList pages;
            for (const auto &id : show.slideIds)
                for (int i = 0; i < doc.slides.size(); ++i)
                    if (doc.slides[i].id == id) pages.append(QStringLiteral("page%1").arg(i + 1));
            out += QStringLiteral("<presentation:show presentation:name=\"%1\" presentation:pages=\"%2\"/>").arg(esc(show.name), pages.join(QLatin1Char(',')));
        }
        return out + QStringLiteral("</presentation:settings>");
    }

    QString meta() const {
        return QStringLiteral("<office:meta><meta:generator>OmaShow</meta:generator></office:meta>");
    }

    QByteArray run() {
        const QString masterXml = masterPages();
        QString body;
        for (int i = 0; i < doc.slides.size(); ++i) {
            if (canceled()) return {};
            Xml x;
            writePage(x, i);
            body += QString::fromUtf8(x.bytes());
        }
        if (!doc.sections.isEmpty()) warnings.add(QStringLiteral("Sections have no OpenDocument equivalent and were left out"), 0);
        const QString presentation = QStringLiteral("<office:body><office:presentation>") + body + settings() + QStringLiteral("</office:presentation></office:body>");
        const QString head = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        const QString ns = QString::fromLatin1(kNamespaces);
        const QString styles = QStringLiteral("<office:styles>") + defaultStyles() + namedXml + QStringLiteral("</office:styles>");
        const QString masterStyles = QStringLiteral("<office:master-styles>") + layers() + masterXml + QStringLiteral("</office:master-styles>");

        if (flavor == OdpWriter::Flat) {
            const QString all = head + QStringLiteral("<office:document") + ns + QStringLiteral(" office:mimetype=\"%1\">").arg(QLatin1String(kMimePresentation)) +
                                meta() + fontDecls() + styles +
                                QStringLiteral("<office:automatic-styles>") + pageLayout() + masters.xml + content.xml + QStringLiteral("</office:automatic-styles>") +
                                masterStyles + presentation + QStringLiteral("</office:document>");
            return all.toUtf8();
        }
        const QByteArray mimetype = flavor == OdpWriter::Template ? kMimeTemplate : kMimePresentation;
        const QString contentXml = head + QStringLiteral("<office:document-content") + ns + QLatin1Char('>') + fontDecls() +
                                   QStringLiteral("<office:automatic-styles>") + content.xml + QStringLiteral("</office:automatic-styles>") +
                                   presentation + QStringLiteral("</office:document-content>");
        const QString stylesXml = head + QStringLiteral("<office:document-styles") + ns + QLatin1Char('>') + fontDecls() + styles +
                                  QStringLiteral("<office:automatic-styles>") + pageLayout() + masters.xml + QStringLiteral("</office:automatic-styles>") +
                                  masterStyles + QStringLiteral("</office:document-styles>");
        const QString metaXml = head + QStringLiteral("<office:document-meta") + ns + QLatin1Char('>') + meta() + QStringLiteral("</office:document-meta>");
        QString manifestXml = head + QStringLiteral("<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">\n"
                                                    " <manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"%1\"/>\n"
                                                    " <manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>\n"
                                                    " <manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>\n"
                                                    " <manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>\n")
                                         .arg(QString::fromLatin1(mimetype));
        for (const auto &line : manifest) manifestXml += line;
        manifestXml += QStringLiteral("</manifest:manifest>\n");

        // The mimetype goes first and uncompressed, so the kind of file can be told from its first bytes.
        QVector<Zip::Entry> entries;
        entries.append({QStringLiteral("mimetype"), mimetype, false});
        entries.append({QStringLiteral("content.xml"), contentXml.toUtf8(), true});
        entries.append({QStringLiteral("styles.xml"), stylesXml.toUtf8(), true});
        entries.append({QStringLiteral("meta.xml"), metaXml.toUtf8(), true});
        entries += files;
        entries.append({QStringLiteral("META-INF/manifest.xml"), manifestXml.toUtf8(), true});
        return Zip::write(entries);
    }
};

} // namespace

namespace OdpWriter {

Flavor flavorOf(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("otp")) return Template;
    if (suffix == QLatin1String("fodp")) return Flat;
    return Presentation;
}

// The deck with every Office font that was swapped for a free match on import
// named again, at its own size, as on PowerPoint save (FontSwap::revert).
static Document original(const Document &document) {
    Document copy = document;
    FontSwap::revert(copy);
    return copy;
}

QByteArray bytes(const Document &document, Flavor flavor, QStringList *warnings, const std::shared_ptr<Workers::Job> &job) {
    const Document deck = original(document);
    Writer writer(deck, flavor);
    writer.job = job;
    const auto out = writer.run();
    if (warnings) *warnings = writer.warnings.lines();
    return out;
}

Report write(const Document &document, const QString &path, const std::shared_ptr<Workers::Job> &job) {
    Report report;
    const Document deck = original(document);
    if (deck.slides.isEmpty()) { report.error = QStringLiteral("The deck has no slides to write."); return report; }
    Writer writer(deck, flavorOf(path));
    writer.job = job;
    const auto data = writer.run();
    if (job && job->canceled) { report.error = QStringLiteral("Cancelled."); return report; }
    if (data.isEmpty()) { report.error = QStringLiteral("The deck could not be written as OpenDocument."); return report; }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        report.error = QStringLiteral("%1 could not be written.").arg(QFileInfo(path).fileName());
        return report;
    }
    report.ok = true;
    report.bytes = data.size();
    report.lines = writer.warnings.lines();
    report.lines.prepend(QStringLiteral("Wrote %1 slides as OpenDocument.").arg(deck.slides.size()));
    return report;
}

} // namespace OdpWriter

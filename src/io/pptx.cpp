#include <functional>
#include "io/pptx.h"
#include "core/fillstyle.h"

#include "anim/build.h"
#include "anim/presentation.h"
#include "core/edit.h"
#include "core/fonts.h"
#include "core/imageasset.h"
#include "core/link.h"
#include "core/mediaasset.h"
#include "core/shape.h"
#include "core/svgasset.h"
#include "core/table.h"
#include "core/textruns.h"
#include "io/metafile.h"
#include "io/zip.h"
#include "render/textlayout.h"

#include <QFontDatabase>
#include <algorithm>
#include <cmath>
#include <QImage>
#include <QPainter>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QPainterPath>
#include <QSet>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTransform>
#include <QXmlStreamReader>
#include <QtMath>

#include <memory>

namespace {

// ---------------------------------------------------------------------------
// A small tree of the XML. The format is read by reference — a slide's text
// looks up its layout, which looks up its master, which looks up the theme —
// so a streaming read would have to be repeated; the parts are small and a
// tree is simpler to ask questions of.

struct Node {
    QString name;   // local name; the prefix is conventional and the URI fixed
    QHash<QString, QString> attrs;
    QVector<Node> kids;
    QString text;

    const Node *child(const QString &n) const {
        for (const auto &k : kids) if (k.name == n) return &k;
        return nullptr;
    }
    QVector<const Node *> all(const QString &n) const {
        QVector<const Node *> out;
        for (const auto &k : kids) if (k.name == n) out.append(&k);
        return out;
    }
    QString attr(const QString &n, const QString &fallback = QString()) const {
        return attrs.value(n, fallback);
    }
    bool has(const QString &n) const { return attrs.contains(n); }
    // Descends by local names; null when any step is missing.
    const Node *path(std::initializer_list<const char *> names) const {
        const Node *node = this;
        for (const auto *n : names) {
            node = node->child(QString::fromLatin1(n));
            if (!node) return nullptr;
        }
        return node;
    }
    // Every descendant with this name, in document order.
    void collect(const QString &n, QVector<const Node *> &out) const {
        for (const auto &k : kids) {
            if (k.name == n) out.append(&k);
            k.collect(n, out);
        }
    }
    // The words in every a:t below this node, paragraphs apart.
    QString words() const {
        QString out;
        QVector<const Node *> paragraphs;
        collect(QStringLiteral("p"), paragraphs);
        if (paragraphs.isEmpty()) {
            QVector<const Node *> ts;
            collect(QStringLiteral("t"), ts);
            for (const auto *t : ts) out += t->text;
            return out;
        }
        for (const auto *p : paragraphs) {
            QVector<const Node *> ts;
            p->collect(QStringLiteral("t"), ts);
            QString line;
            for (const auto *t : ts) line += t->text;
            if (!out.isEmpty()) out += QLatin1Char('\n');
            out += line;
        }
        return out;
    }
};

bool parseXml(const QByteArray &bytes, Node *root, QString *error) {
    QXmlStreamReader xml(bytes);
    QVector<Node *> stack;
    *root = Node();
    bool started = false;
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement: {
            // A tree of any depth is a stack overflow waiting to happen; no
            // real deck nests anywhere near this.
            if (stack.size() > 256) { if (error) *error = QStringLiteral("nested too deeply"); return false; }
            Node *node;
            if (!started) { node = root; started = true; }
            else { stack.last()->kids.append(Node()); node = &stack.last()->kids.last(); }
            node->name = xml.name().toString();
            for (const auto &a : xml.attributes()) {
                // Keyed by local name, and by prefix:name where there is a
                // prefix; an unprefixed attribute keeps the local key when a
                // prefixed one shares it (p:sldId has both id and r:id).
                const auto local = a.name().toString();
                if (a.prefix().isEmpty() || !node->attrs.contains(local)) node->attrs.insert(local, a.value().toString());
                if (!a.prefix().isEmpty()) node->attrs.insert(a.qualifiedName().toString(), a.value().toString());
            }
            stack.append(node);
            break;
        }
        case QXmlStreamReader::EndElement:
            if (!stack.isEmpty()) stack.removeLast();
            break;
        case QXmlStreamReader::Characters:
            if (!stack.isEmpty() && (!xml.isWhitespace() || stack.last()->name == QLatin1String("t")))
                stack.last()->text += xml.text().toString();
            break;
        default: break;
        }
    }
    if (xml.hasError()) {
        if (error) *error = xml.errorString();
        return false;
    }
    return started;
}

// Markup written for a newer PowerPoint carries an older reading beside it.
// Shapes take the fallback, which every reader understands; the transition
// reader looks at both, because the newer one may be the interesting one.
void flattened(const Node &parent, QVector<const Node *> &out) {
    for (const auto &k : parent.kids) {
        if (k.name == QLatin1String("AlternateContent")) {
            const Node *branch = k.child(QStringLiteral("Fallback"));
            if (!branch) branch = k.child(QStringLiteral("Choice"));
            if (branch) flattened(*branch, out);
        } else out.append(&k);
    }
}

// ---------------------------------------------------------------------------
// The package: parts by path, relationships by part.

struct Rel { QString type, target; bool external = false; };

QString dirOf(const QString &path) {
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

QString resolvePath(const QString &from, const QString &target) {
    if (target.startsWith(QLatin1Char('/'))) return QDir::cleanPath(target.mid(1));
    const auto dir = dirOf(from);
    return QDir::cleanPath(dir.isEmpty() ? target : dir + QLatin1Char('/') + target);
}

struct Package {
    Zip::Reader zip;
    mutable QHash<QString, std::shared_ptr<Node>> parts;
    mutable QHash<QString, QHash<QString, Rel>> relations;

    explicit Package(const QByteArray &raw) : zip(raw) {}

    QByteArray bytes(const QString &path) const { return zip.read(path); }
    bool has(const QString &path) const { return zip.contains(path); }

    const Node *part(const QString &path) const {
        if (path.isEmpty()) return nullptr;
        auto it = parts.find(path);
        if (it != parts.end()) return it->get();
        std::shared_ptr<Node> node;
        if (zip.contains(path)) {
            node = std::make_shared<Node>();
            QString error;
            if (!parseXml(zip.read(path), node.get(), &error)) node.reset();
        }
        parts.insert(path, node);
        return node.get();
    }

    const QHash<QString, Rel> &rels(const QString &path) const {
        auto it = relations.find(path);
        if (it != relations.end()) return *it;
        QHash<QString, Rel> out;
        const auto dir = dirOf(path);
        const auto file = path.mid(dir.isEmpty() ? 0 : dir.size() + 1);
        const auto relsPath = (dir.isEmpty() ? QString() : dir + QLatin1Char('/'))
                              + QStringLiteral("_rels/") + file + QStringLiteral(".rels");
        Node root;
        QString error;
        if (zip.contains(relsPath) && parseXml(zip.read(relsPath), &root, &error)) {
            for (const auto &r : root.all(QStringLiteral("Relationship"))) {
                Rel rel;
                rel.type = r->attr(QStringLiteral("Type"));
                rel.external = r->attr(QStringLiteral("TargetMode")) == QLatin1String("External");
                rel.target = rel.external ? r->attr(QStringLiteral("Target"))
                                          : resolvePath(path, r->attr(QStringLiteral("Target")));
                out.insert(r->attr(QStringLiteral("Id")), rel);
            }
        }
        return *relations.insert(path, out);
    }

    // The part a relationship id points at, or empty when it is external or absent.
    QString target(const QString &from, const QString &rid) const {
        const auto rel = rels(from).value(rid);
        return rel.external ? QString() : rel.target;
    }
    QString external(const QString &from, const QString &rid) const {
        const auto rel = rels(from).value(rid);
        return rel.external ? rel.target : QString();
    }
    // The first related part of a type ("slideLayout", "theme", …).
    QString related(const QString &from, const QString &typeSuffix) const {
        const auto all = rels(from);
        for (auto it = all.cbegin(); it != all.cend(); ++it)
            if (!it->external && it->type.endsWith(QLatin1Char('/') + typeSuffix)) return it->target;
        return QString();
    }
    QStringList relatedAll(const QString &from, const QString &typeSuffix) const {
        QStringList out;
        const auto all = rels(from);
        for (auto it = all.cbegin(); it != all.cend(); ++it)
            if (!it->external && it->type.endsWith(QLatin1Char('/') + typeSuffix)) out.append(it->target);
        return out;
    }
};

// ---------------------------------------------------------------------------
// Theme colours and fonts.

struct Theme {
    QString name;
    QHash<QString, QColor> colors;   // dk1, lt1, dk2, lt2, accent1…6, hlink, folHlink
    QString major, minor;            // latin typefaces
    // Its background styles (bgFillStyleLst: a bgRef idx 1001 is the first),
    // and its part, for the pictures they name.
    QString path;
    const Node *backgroundStyles = nullptr;
};

QColor plainColor(const Node *clr) {
    if (!clr) return QColor();
    if (clr->name == QLatin1String("srgbClr")) return QColor(QLatin1Char('#') + clr->attr(QStringLiteral("val")));
    if (clr->name == QLatin1String("sysClr")) return QColor(QLatin1Char('#') + clr->attr(QStringLiteral("lastClr"), QStringLiteral("000000")));
    if (clr->name == QLatin1String("prstClr")) { QColor c(clr->attr(QStringLiteral("val"))); return c.isValid() ? c : QColor(Qt::black); }
    if (clr->name == QLatin1String("scrgbClr")) {
        return QColor::fromRgbF(clr->attr(QStringLiteral("r")).toDouble() / 100000.0,
                                clr->attr(QStringLiteral("g")).toDouble() / 100000.0,
                                clr->attr(QStringLiteral("b")).toDouble() / 100000.0);
    }
    if (clr->name == QLatin1String("hslClr")) {
        return QColor::fromHslF(clr->attr(QStringLiteral("hue")).toDouble() / 21600000.0,
                                clr->attr(QStringLiteral("sat")).toDouble() / 100000.0,
                                clr->attr(QStringLiteral("lum")).toDouble() / 100000.0);
    }
    return QColor();
}

Theme readTheme(const Package &pkg, const QString &path) {
    Theme theme;
    const Node *root = pkg.part(path);
    if (!root) return theme;
    theme.name = root->attr(QStringLiteral("name"));
    theme.path = path;
    theme.backgroundStyles = root->path({"themeElements", "fmtScheme", "bgFillStyleLst"});
    if (const Node *scheme = root->path({"themeElements", "clrScheme"})) {
        for (const auto &entry : scheme->kids) {
            for (const auto &clr : entry.kids) {
                const auto color = plainColor(&clr);
                if (color.isValid()) { theme.colors.insert(entry.name, color); break; }
            }
        }
    }
    if (const Node *fonts = root->path({"themeElements", "fontScheme"})) {
        if (const Node *major = fonts->path({"majorFont", "latin"})) theme.major = major->attr(QStringLiteral("typeface"));
        if (const Node *minor = fonts->path({"minorFont", "latin"})) theme.minor = minor->attr(QStringLiteral("typeface"));
    }
    return theme;
}

struct ColorContext {
    const Theme *theme = nullptr;
    QHash<QString, QString> clrMap;   // bg1 → lt1 …
    QColor phColor;                   // a placeholder colour, for "phClr"
};

QColor readColor(const Node *clr, const ColorContext &ctx) {
    if (!clr) return QColor();
    QColor color;
    if (clr->name == QLatin1String("schemeClr")) {
        auto key = clr->attr(QStringLiteral("val"));
        if (key == QLatin1String("phClr")) color = ctx.phColor;
        else {
            key = ctx.clrMap.value(key, key);
            if (key == QLatin1String("bg1")) key = QStringLiteral("lt1");
            if (key == QLatin1String("tx1")) key = QStringLiteral("dk1");
            if (key == QLatin1String("bg2")) key = QStringLiteral("lt2");
            if (key == QLatin1String("tx2")) key = QStringLiteral("dk2");
            color = ctx.theme ? ctx.theme->colors.value(key) : QColor();
        }
    } else color = plainColor(clr);
    if (!color.isValid()) return color;
    // Modifiers, in the order PowerPoint applies the common ones.
    for (const auto &mod : clr->kids) {
        const qreal v = mod.attr(QStringLiteral("val")).toDouble() / 100000.0;
        if (mod.name == QLatin1String("lumMod") || mod.name == QLatin1String("lumOff")) {
            float h, s, l, a;
            color.getHslF(&h, &s, &l, &a);
            l = mod.name == QLatin1String("lumMod") ? l * float(v) : l + float(v);
            color = QColor::fromHslF(h < 0 ? 0 : h, s, qBound(0.0f, l, 1.0f), a);
        } else if (mod.name == QLatin1String("alpha")) {
            color.setAlphaF(qBound(0.0, v, 1.0));
        } else if (mod.name == QLatin1String("tint") || mod.name == QLatin1String("shade")) {
            // PowerPoint tints towards white and shades towards black in linear
            // light, not on the sRGB values (measured: a duotone of #0F0B5C
            // with tint 60%, satMod 400%, lumMod 110% is (175,175,210) in
            // PowerPoint; this gives (176,176,211), the sRGB way (68,60,233)).
            const auto toLinear = [](qreal c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
            const auto toSrgb = [](qreal l) { l = qBound(0.0, l, 1.0); return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1 / 2.4) - 0.055; };
            const bool tint = mod.name == QLatin1String("tint");
            const auto apply = [&](qreal c) { const qreal l = toLinear(c); return toSrgb(tint ? 1 - (1 - l) * v : l * v); };
            color = QColor::fromRgbF(apply(color.redF()), apply(color.greenF()), apply(color.blueF()), color.alphaF());
        } else if (mod.name == QLatin1String("satMod") || mod.name == QLatin1String("satOff")) {
            float h, s, l, a;
            color.getHslF(&h, &s, &l, &a);
            s = mod.name == QLatin1String("satMod") ? s * float(v) : s + float(v);
            color = QColor::fromHslF(h < 0 ? 0 : h, qBound(0.0f, s, 1.0f), l, a);
        }
    }
    return color;
}

// The colour inside a solidFill (or any parent with one colour child).
QColor colorIn(const Node *parent, const ColorContext &ctx) {
    if (!parent) return QColor();
    for (const auto &k : parent->kids) {
        const auto c = readColor(&k, ctx);
        if (c.isValid()) return c;
    }
    return QColor();
}

// ---------------------------------------------------------------------------
// Fill, line and shadow of a shape.

struct Look {
    bool fillSet = false;
    int fillStyle = 5;   // none
    QColor fill, fill2;
    qreal angle = 0;
    QString blipRid;
    bool lineSet = false, line = false;
    QColor lineColor;
    qreal lineWidthEmu = 0;
    int dash = 0;
    bool shadow = false;
    QColor shadowColor;
    qreal shadowDistEmu = 0, shadowDir = 0;
    // EFFECTS-UI-1: the rest of the shadow, and the shape's glow.
    qreal shadowBlurEmu = 0;
    bool shadowInner = false;
    qreal shadowSx = 100, shadowSy = 100, shadowKx = 0, shadowKy = 0;
    int shadowAlign = 0;
    qreal glowRadEmu = 0;
    QColor glowColor;
    // FILL-UI-1: the rest of a:gradFill, a:pattFill and a:blipFill, and of a:ln.
    QVector<GradientStop> stops;
    int gradPath = 0;                       // circle, rect, shape
    qreal focus[4] = {0, 0, 0, 0};          // a:fillToRect l t r b (the schema's 0)
    bool scaled = false, rotate = true;
    QString pattern;
    const Node *blip = nullptr;             // the a:blipFill
    QString dashPreset;
    QVariantList customDash;
    int compound = 0, cap = -1, join = -1;
    bool lineGradient = false;
    QVector<GradientStop> lineStops;
    qreal lineAngle = 0;
    int lineGradPath = 0;
    // STYLE3D: soft edges, reflection (lengths in EMU until applied), 3-D.
    qreal softEdgeEmu = 0;
    Effects3D::Reflection reflection;
    Effects3D::Format format3d;
    Effects3D::Scene scene3d;
};

// ---- STYLE3D: a:reflection, a:sp3d and a:scene3d as written (lengths in EMU).

int shadowAlignOf(const QString &algn);

qreal attrNumber(const Node *n, const char *name, qreal fallback) {
    const QString v = n->attr(QLatin1String(name));
    bool ok = false;
    const qreal d = v.toDouble(&ok);
    return ok && std::isfinite(d) ? d : fallback;
}
bool attrTrue(const Node *n, const char *name, bool fallback) {
    if (!n->has(QLatin1String(name))) return fallback;
    const QString v = n->attr(QLatin1String(name));
    return v == QLatin1String("1") || v == QLatin1String("true");
}

Effects3D::Reflection readReflection(const Node *r) {
    Effects3D::Reflection out;
    if (!r) return out;
    out.on = true;
    out.blur = qMax(0.0, attrNumber(r, "blurRad", 0));
    out.dist = qMax(0.0, attrNumber(r, "dist", 0));
    out.startAlpha = qBound(0.0, attrNumber(r, "stA", 100000) / 1000, 100.0);
    out.startPos = qBound(0.0, attrNumber(r, "stPos", 0) / 1000, 100.0);
    out.endAlpha = qBound(0.0, attrNumber(r, "endA", 0) / 1000, 100.0);
    out.endPos = qBound(0.0, attrNumber(r, "endPos", 100000) / 1000, 100.0);
    out.dir = qBound(0.0, attrNumber(r, "dir", 0) / 60000, 360.0);
    out.fadeDir = qBound(0.0, attrNumber(r, "fadeDir", 5400000) / 60000, 360.0);
    out.sx = qBound(-10000.0, attrNumber(r, "sx", 100000) / 1000, 10000.0);
    out.sy = qBound(-10000.0, attrNumber(r, "sy", 100000) / 1000, 10000.0);
    out.kx = qBound(-90.0, attrNumber(r, "kx", 0) / 60000, 90.0);
    out.ky = qBound(-90.0, attrNumber(r, "ky", 0) / 60000, 90.0);
    out.align = shadowAlignOf(r->attr(QStringLiteral("algn"), QStringLiteral("b")));
    out.rotate = attrTrue(r, "rotWithShape", true);
    return out;
}

Effects3D::Bevel readBevel(const Node *b) {
    Effects3D::Bevel out;
    if (!b) return out;
    out.preset = b->attr(QStringLiteral("prst"), QStringLiteral("circle"));
    if (!Effects3D::bevelPresets().contains(out.preset)) out.preset = QStringLiteral("circle");
    out.w = qMax(0.0, attrNumber(b, "w", 76200));
    out.h = qMax(0.0, attrNumber(b, "h", 76200));
    return out;
}

Effects3D::Format readFormat3d(const Node *sp3d, const ColorContext &ctx) {
    Effects3D::Format out;
    if (!sp3d) return out;
    out.on = true;
    out.top = readBevel(sp3d->child(QStringLiteral("bevelT")));
    out.bottom = readBevel(sp3d->child(QStringLiteral("bevelB")));
    out.depth = qMax(0.0, attrNumber(sp3d, "extrusionH", 0));
    out.contour = qMax(0.0, attrNumber(sp3d, "contourW", 0));
    out.z = attrNumber(sp3d, "z", 0);
    out.material = sp3d->attr(QStringLiteral("prstMaterial"));
    if (!out.material.isEmpty() && !Effects3D::materialPresets().contains(out.material)) out.material.clear();
    if (const Node *c = sp3d->child(QStringLiteral("extrusionClr"))) out.extrusionColor = colorIn(c, ctx);
    if (const Node *c = sp3d->child(QStringLiteral("contourClr"))) out.contourColor = colorIn(c, ctx);
    return out;
}

Effects3D::Scene readScene3d(const Node *scene) {
    Effects3D::Scene out;
    if (!scene) return out;
    out.on = true;
    if (const Node *camera = scene->child(QStringLiteral("camera"))) {
        out.camera = camera->attr(QStringLiteral("prst"), QStringLiteral("orthographicFront"));
        if (!Effects3D::cameraPresets().contains(out.camera)) out.camera = QStringLiteral("orthographicFront");
        if (camera->has(QStringLiteral("fov"))) out.fov = qBound(0.0, attrNumber(camera, "fov", 0) / 60000, 180.0);
        out.zoom = qBound(0.0, attrNumber(camera, "zoom", 100000) / 1000, 100000.0);
        if (const Node *rot = camera->child(QStringLiteral("rot"))) {
            out.rotated = true;
            out.lat = qBound(0.0, attrNumber(rot, "lat", 0) / 60000, 360.0);
            out.lon = qBound(0.0, attrNumber(rot, "lon", 0) / 60000, 360.0);
            out.rev = qBound(0.0, attrNumber(rot, "rev", 0) / 60000, 360.0);
        }
    }
    if (const Node *rig = scene->child(QStringLiteral("lightRig"))) {
        out.rig = rig->attr(QStringLiteral("rig"), QStringLiteral("threePt"));
        if (!Effects3D::lightRigs().contains(out.rig)) out.rig = QStringLiteral("threePt");
        out.rigDir = rig->attr(QStringLiteral("dir"), QStringLiteral("t"));
        if (!Effects3D::rigDirections().contains(out.rigDir)) out.rigDir = QStringLiteral("t");
        if (const Node *rot = rig->child(QStringLiteral("rot"))) {
            out.rigRotated = true;
            out.rigLat = qBound(0.0, attrNumber(rot, "lat", 0) / 60000, 360.0);
            out.rigLon = qBound(0.0, attrNumber(rot, "lon", 0) / 60000, 360.0);
            out.rigRev = qBound(0.0, attrNumber(rot, "rev", 0) / 60000, 360.0);
        }
    }
    return out;
}

// spPr's a:scene3d and a:sp3d.
void read3d(const Node *spPr, Look &look, const ColorContext &ctx) {
    if (!spPr) return;
    look.scene3d = readScene3d(spPr->child(QStringLiteral("scene3d")));
    look.format3d = readFormat3d(spPr->child(QStringLiteral("sp3d")), ctx);
}

// a:outerShdw's algn, as SceneObject::shadowAlign counts them.
int shadowAlignOf(const QString &algn) {
    static const QStringList names{QStringLiteral("tl"), QStringLiteral("t"), QStringLiteral("tr"), QStringLiteral("l"), QStringLiteral("ctr"),
                                   QStringLiteral("r"), QStringLiteral("bl"), QStringLiteral("b"), QStringLiteral("br")};
    const int i = names.indexOf(algn);
    return i < 0 ? 7 : i;   // the schema's default is b
}

// Direct fill children of spPr, bgPr, tcPr, ln, rPr …
bool readFill(const Node *owner, Look &look, const ColorContext &ctx) {
    if (!owner) return false;
    for (const auto &k : owner->kids) {
        if (k.name == QLatin1String("solidFill")) {
            look.fillSet = true; look.fillStyle = 0; look.fill = colorIn(&k, ctx);
            if (!look.fill.isValid()) look.fillStyle = 5;
            return true;
        }
        if (k.name == QLatin1String("noFill")) { look.fillSet = true; look.fillStyle = 5; return true; }
        if (k.name == QLatin1String("gradFill")) {
            look.fillSet = true;
            // FILL-UI-1: every stop, the path and its focus, scaled, rotate with shape.
            QVector<GradientStop> stops;
            if (const Node *list = k.child(QStringLiteral("gsLst")))
                for (const auto *gs : list->all(QStringLiteral("gs"))) {
                    // A stop's Brightness (what PowerPoint writes: lumMod 1-b with
                    // lumOff b, or lumMod 1+b alone) is kept apart from its colour.
                    qreal brightness = 0;
                    QColor c;
                    for (const auto &clr : gs->kids) {
                        Node plain = clr;
                        QVector<Node> lum;
                        plain.kids.erase(std::remove_if(plain.kids.begin(), plain.kids.end(), [&](const Node &m) {
                            const bool l = m.name == QLatin1String("lumMod") || m.name == QLatin1String("lumOff");
                            if (l) lum.append(m);
                            return l;
                        }), plain.kids.end());
                        const auto val = [](const Node &m) { return m.attr(QStringLiteral("val")).toDouble() / 100000.0; };
                        if (lum.size() == 2 && lum[0].name == QLatin1String("lumMod") && lum[1].name == QLatin1String("lumOff") &&
                            std::abs(val(lum[0]) + val(lum[1]) - 1) < 1e-4 && val(lum[1]) > 0) brightness = val(lum[1]);
                        else if (lum.size() == 1 && lum[0].name == QLatin1String("lumMod") && val(lum[0]) < 1 && val(lum[0]) >= 0) brightness = val(lum[0]) - 1;
                        c = readColor(brightness != 0 ? &plain : &clr, ctx);
                        if (c.isValid()) break;
                        brightness = 0;
                    }
                    if (c.isValid()) stops.append({qBound(0.0, gs->attr(QStringLiteral("pos")).toDouble() / 100000.0, 1.0), c, brightness});
                }
            std::stable_sort(stops.begin(), stops.end(), [](const GradientStop &a, const GradientStop &b) { return a.position < b.position; });
            if (stops.isEmpty()) { look.fillStyle = 5; return true; }
            if (stops.size() == 1) stops.append({1, stops.first().color, 0});
            while (stops.size() > 10) stops.removeAt(stops.size() / 2);
            const Node *path = k.child(QStringLiteral("path"));
            look.fillStyle = path ? 2 : 1;
            look.fill = stops.first().color;
            look.fill2 = stops.last().color;
            look.stops = stops;
            look.rotate = k.attr(QStringLiteral("rotWithShape"), QStringLiteral("1")) != QLatin1String("0") &&
                          k.attr(QStringLiteral("rotWithShape")) != QLatin1String("false");
            if (const Node *lin = k.child(QStringLiteral("lin"))) {
                look.angle = lin->attr(QStringLiteral("ang")).toDouble() / 60000.0;
                const auto sc = lin->attr(QStringLiteral("scaled"));
                look.scaled = sc == QLatin1String("1") || sc == QLatin1String("true");
            }
            if (path) {
                const auto kind = path->attr(QStringLiteral("path"));
                // Office's defaults (MS-OI29500 20.1.8): path shape, focus 50% each side.
                look.gradPath = kind == QLatin1String("rect") ? 1 : kind == QLatin1String("circle") ? 0 : 2;
                for (auto &f : look.focus) f = .5;
                if (const Node *to = path->child(QStringLiteral("fillToRect"))) {
                    const char *names[] = {"l", "t", "r", "b"};
                    for (int i = 0; i < 4; ++i) look.focus[i] = to->attr(QLatin1String(names[i]), QStringLiteral("0")).toDouble() / 100000.0;
                }
            }
            return true;
        }
        if (k.name == QLatin1String("pattFill")) {
            look.fillSet = true; look.fillStyle = 3;
            // FILL-UI-1: the pattern itself (the schema's default is pct5),
            // black on white unless the colours are named.
            look.pattern = k.attr(QStringLiteral("prst"), QStringLiteral("pct5"));
            if (FillStyle::patternCells(look.pattern).isEmpty()) look.pattern = QStringLiteral("pct5");
            look.fill = colorIn(k.child(QStringLiteral("fgClr")), ctx);
            look.fill2 = colorIn(k.child(QStringLiteral("bgClr")), ctx);
            if (!look.fill.isValid()) look.fill = QColor(Qt::black);
            if (!look.fill2.isValid()) look.fill2 = QColor(Qt::white);
            return true;
        }
        if (k.name == QLatin1String("blipFill")) {
            look.fillSet = true; look.fillStyle = 5;
            if (const Node *blip = k.child(QStringLiteral("blip"))) look.blipRid = blip->attr(QStringLiteral("embed"));
            look.blip = &k;   // FILL-UI-1: drawn once the picture is read (convertShape)
            return true;
        }
        if (k.name == QLatin1String("grpFill")) { look.fillSet = true; look.fillStyle = 5; return true; }
    }
    return false;
}

void readLine(const Node *ln, Look &look, const ColorContext &ctx) {
    if (!ln) return;
    look.lineSet = true;
    look.lineWidthEmu = ln->attr(QStringLiteral("w"), QStringLiteral("9525")).toDouble();
    Look inner;
    readFill(ln, inner, ctx);
    if (inner.fillSet && inner.fillStyle == 5) { look.line = false; return; }
    look.lineColor = inner.fill.isValid() ? inner.fill : QColor(Qt::black);
    // A fully transparent line is no line.
    if (inner.fill.isValid() && inner.fill.alpha() == 0) { look.line = false; return; }
    look.line = true;
    if (const Node *dash = ln->child(QStringLiteral("prstDash"))) {
        const auto v = dash->attr(QStringLiteral("val"));
        // FILL-UI-1: the dash by its own name (dash keeps the nearest Qt one).
        if (FillStyle::dashPresets().contains(v) && v != QLatin1String("solid")) look.dashPreset = v;
        look.dash = FillStyle::nearestQtDash(v);
    } else if (const Node *custom = ln->child(QStringLiteral("custDash"))) {
        // Dash and space, each in thousandths of a percent of the line's width.
        QVariantList list;
        for (const auto *ds : custom->all(QStringLiteral("ds"))) {
            list.append(qBound(0.01, ds->attr(QStringLiteral("d"), QStringLiteral("100000")).toDouble() / 100000.0, 1000.0));
            list.append(qBound(0.01, ds->attr(QStringLiteral("sp"), QStringLiteral("100000")).toDouble() / 100000.0, 1000.0));
        }
        if (!list.isEmpty() && list.size() <= 20) { look.dashPreset = QStringLiteral("custom"); look.customDash = list; look.dash = 1; }
    }
    look.compound = qMax(0, FillStyle::compoundNames().indexOf(ln->attr(QStringLiteral("cmpd"), QStringLiteral("sng"))));
    const auto cap = ln->attr(QStringLiteral("cap"));
    if (cap == QLatin1String("flat")) look.cap = 0; else if (cap == QLatin1String("sq")) look.cap = 1; else if (cap == QLatin1String("rnd")) look.cap = 2;
    if (ln->child(QStringLiteral("miter"))) look.join = 0;
    else if (ln->child(QStringLiteral("bevel"))) look.join = 1;
    else if (ln->child(QStringLiteral("round"))) look.join = 2;
    if (inner.fillStyle == 1 || inner.fillStyle == 2) {
        look.lineGradient = true;
        look.lineStops = inner.stops;
        look.lineAngle = inner.angle;
        look.lineGradPath = inner.fillStyle == 1 ? 0 : 1 + inner.gradPath;
    }
}

void readEffects(const Node *effectLst, Look &look, const ColorContext &ctx) {
    if (!effectLst) return;
    // EFFECTS-UI-1: every attribute PowerPoint writes, with the schema's own
    // defaults (0 for distance, direction and blur, 100% size, no slant, algn
    // b). The outer shadow wins when a shape has both (OmaShow keeps one).
    const Node *outer = effectLst->child(QStringLiteral("outerShdw"));
    const Node *inner = effectLst->child(QStringLiteral("innerShdw"));
    if (const Node *shadow = outer ? outer : inner) {
        look.shadow = true;
        look.shadowInner = !outer;
        look.shadowColor = colorIn(shadow, ctx);
        if (!look.shadowColor.isValid()) look.shadowColor = QColor(0, 0, 0, 100);
        look.shadowDistEmu = shadow->attr(QStringLiteral("dist"), QStringLiteral("0")).toDouble();
        look.shadowDir = shadow->attr(QStringLiteral("dir"), QStringLiteral("0")).toDouble() / 60000.0;
        look.shadowBlurEmu = shadow->attr(QStringLiteral("blurRad"), QStringLiteral("0")).toDouble();
        if (outer) {
            look.shadowSx = shadow->attr(QStringLiteral("sx"), QStringLiteral("100000")).toDouble() / 1000.0;
            look.shadowSy = shadow->attr(QStringLiteral("sy"), QStringLiteral("100000")).toDouble() / 1000.0;
            look.shadowKx = shadow->attr(QStringLiteral("kx"), QStringLiteral("0")).toDouble() / 60000.0;
            look.shadowKy = shadow->attr(QStringLiteral("ky"), QStringLiteral("0")).toDouble() / 60000.0;
            look.shadowAlign = shadowAlignOf(shadow->attr(QStringLiteral("algn"), QStringLiteral("b")));
        }
    }
    if (const Node *glow = effectLst->child(QStringLiteral("glow"))) {
        const auto c = colorIn(glow, ctx);
        if (c.isValid()) { look.glowRadEmu = glow->attr(QStringLiteral("rad"), QStringLiteral("0")).toDouble(); look.glowColor = c; }
    }
    // STYLE3D: reflection and soft edges.
    look.reflection = readReflection(effectLst->child(QStringLiteral("reflection")));
    if (const Node *soft = effectLst->child(QStringLiteral("softEdge"))) look.softEdgeEmu = qMax(0.0, attrNumber(soft, "rad", 0));
}

// p:style — theme-indexed fill and line for shapes that name none of their own.
void applyStyleRefs(const Node *style, Look &look, const ColorContext &ctx) {
    if (!style) return;
    if (!look.fillSet) {
        if (const Node *ref = style->child(QStringLiteral("fillRef"))) {
            const int idx = ref->attr(QStringLiteral("idx")).toInt();
            const auto color = colorIn(ref, ctx);
            if (idx > 0 && color.isValid()) { look.fillSet = true; look.fillStyle = 0; look.fill = color; }
        }
    }
    if (!look.lineSet) {
        if (const Node *ref = style->child(QStringLiteral("lnRef"))) {
            const int idx = ref->attr(QStringLiteral("idx")).toInt();
            const auto color = colorIn(ref, ctx);
            if (idx > 0 && color.isValid()) {
                look.lineSet = true; look.line = true; look.lineColor = color;
                look.lineWidthEmu = 9525.0 * idx;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Text: what a run and a paragraph look like once every layer has spoken.

struct RunLook {
    qreal sz = 0;          // hundredths of a point; 0 unknown
    int b = -1, i = -1, u = -1, strike = -1;
    int weight = 0;        // from a weight-named family ("Noto Serif Medium" -> 500); 0 none
    QColor color;
    QString family;
    int baseline = 0;
    bool caps = false;
    qreal spc = 0;         // hundredths of a point
    QString lang, link;
    // Text glow (effectLst/glow): radius in EMU, -1 unknown, 0 none.
    qreal glowRad = -1;
    QColor glowColor;
    // Outer shadow (effectLst/outerShdw): -1 unknown, 0 none, 1 one; EMU and degrees.
    int shadow = -1;
    QColor shadowColor;
    qreal shadowDist = 0, shadowDir = 0, shadowBlur = 0;
    // TEXT-SHADOW-2: an inner shadow (innerShdw), or the outer one's size,
    // slant and anchor (sx/sy %, kx/ky degrees, algn as SceneObject counts it).
    bool shadowInner = false;
    qreal shadowSx = 100, shadowSy = 100, shadowKx = 0, shadowKy = 0;
    int shadowAlign = 7;
    // STYLE3D: Text Fill (empty: solid, as color), Text Outline (strokeWidth
    // in EMU until applied; {"none": true} for a:noFill) and the reflection
    // (lengths in EMU).
    QVariantMap fill, outline;
    Effects3D::Reflection reflection;
};

// STYLE3D: a text fill read as a Look, as SceneObject fill keys.
QVariantMap textFillMap(const Look &look) {
    if (!look.fillSet || look.fillStyle == 0) return {};
    if (look.fillStyle == 5) return {{"fillStyle", 5}};
    if (look.fillStyle == 4) return {};   // picture text fill: not kept (drawn in its first colour)
    QVariantMap m{{"fillStyle", look.fillStyle}, {"fill", look.fill.name(QColor::HexArgb)},
                  {"fillSecondary", (look.fill2.isValid() ? look.fill2 : look.fill).name(QColor::HexArgb)}};
    if (look.fillStyle == 1 || look.fillStyle == 2) {
        m.insert("fillAngle", look.angle);
        m.insert("gradientStops", FillStyle::encodeStops(look.stops));
        m.insert("gradientPath", look.gradPath);
        m.insert("gradientFocusL", look.focus[0]); m.insert("gradientFocusT", look.focus[1]);
        m.insert("gradientFocusR", look.focus[2]); m.insert("gradientFocusB", look.focus[3]);
        m.insert("gradientScaled", look.scaled);
        m.insert("gradientRotate", look.rotate);
    }
    if (look.fillStyle == 3) { m.insert("patternPreset", look.pattern); m.insert("patternStyle", FillStyle::nearestQtPattern(look.pattern)); }
    return m;
}

// STYLE3D: a text outline (a:rPr/a:ln) as SceneObject stroke keys, width in EMU.
QVariantMap textOutlineMap(const Look &look) {
    if (!look.lineSet) return {};
    if (!look.line) return {{"none", true}};
    QVariantMap m{{"strokeWidth", look.lineWidthEmu}, {"strokeColor", look.lineColor.name(QColor::HexArgb)},
                  {"strokeStyle", look.dash}, {"strokeDash", look.dashPreset}, {"strokeDashCustom", look.customDash},
                  {"strokeCompound", look.compound}, {"strokeCap", look.cap >= 0 ? look.cap : 0},
                  {"strokeJoin", look.join >= 0 ? look.join : 2}};
    if (look.lineGradient && look.lineStops.size() >= 2) {
        m.insert("strokeGradient", true);
        m.insert("strokeStops", FillStyle::encodeStops(look.lineStops));
        m.insert("strokeAngle", look.lineAngle);
        m.insert("strokeGradientPath", look.lineGradPath);
    }
    return m;
}

// Bold on top of any face is bold; otherwise a weight-named family's weight.
int lookWeight(const RunLook &l) { return l.b > 0 ? 700 : l.weight ? l.weight : 400; }

struct ParaLook {
    int algn = -1;         // 0 left, 1 centre, 2 right, 3 justify
    int bullet = -1;       // 0 none, 1 marks, 2 numbers
    int startAt = 1;
    qreal lnSpc = 0;       // percent
    qreal spcBef = -1;     // points
    qreal defTabSz = -1;   // EMU between default tab stops; -1 not set
    int lvl = 0;
};

// PowerPoint has only bold on/off, so other weights travel as their own
// family ("Noto Serif Medium", "Inter SemiBold"). When the family without the
// weight word is here (installed or bundled), read it back as that family at
// that weight, which is how OmaShow keeps it.
QString splitWeight(const QString &face, int *weight) {
    static const QList<QPair<QString, int>> words = {
        {QStringLiteral(" Extra Light"), 200}, {QStringLiteral(" ExtraLight"), 200}, {QStringLiteral(" UltraLight"), 200},
        {QStringLiteral(" Semi Bold"), 600}, {QStringLiteral(" SemiBold"), 600}, {QStringLiteral(" Semibold"), 600}, {QStringLiteral(" DemiBold"), 600},
        {QStringLiteral(" Extra Bold"), 800}, {QStringLiteral(" ExtraBold"), 800}, {QStringLiteral(" UltraBold"), 800},
        {QStringLiteral(" Thin"), 100}, {QStringLiteral(" Light"), 300}, {QStringLiteral(" Medium"), 500},
        {QStringLiteral(" Black"), 900}, {QStringLiteral(" Heavy"), 900}};
    // Families in their own right (Office ships these) keep their names.
    static const QStringList own = {QStringLiteral("Arial Black"), QStringLiteral("Segoe UI Light"), QStringLiteral("Segoe UI Semilight"),
        QStringLiteral("Segoe UI Semibold"), QStringLiteral("Segoe UI Black"), QStringLiteral("Franklin Gothic Medium"),
        QStringLiteral("Franklin Gothic Demi"), QStringLiteral("Franklin Gothic Heavy"), QStringLiteral("Arial Rounded MT Bold")};
    if (own.contains(face, Qt::CaseInsensitive)) return face;
    for (const auto &w : words) {
        if (!face.endsWith(w.first, Qt::CaseInsensitive)) continue;
        const QString stem = face.left(face.size() - w.first.size());
        // Only when the stem really has a face of that weight here.
        bool has = false;
        for (const auto &style : QFontDatabase::styles(stem))
            if (QFontDatabase::weight(stem, style) == w.second) has = true;
        if (has) { *weight = w.second; return stem; }
    }
    return face;
}

QString mapFont(const QString &face, const Theme *theme) {
    if (face.startsWith(QLatin1String("+mj"))) return theme ? theme->major : QString();
    if (face.startsWith(QLatin1String("+mn"))) return theme ? theme->minor : QString();
    return face;
}

struct TextContext {
    const Package *pkg = nullptr;
    QString partPath;                 // for hyperlink relationships
    const ColorContext *color = nullptr;
    const QHash<QString, QString> *slideIdByPath = nullptr;
};

void applyRPr(RunLook &run, const Node *rPr, const TextContext &ctx) {
    if (!rPr) return;
    if (rPr->has(QStringLiteral("sz"))) run.sz = rPr->attr(QStringLiteral("sz")).toDouble();
    if (rPr->has(QStringLiteral("b"))) run.b = rPr->attr(QStringLiteral("b")) == QLatin1String("1") || rPr->attr(QStringLiteral("b")) == QLatin1String("true");
    if (rPr->has(QStringLiteral("i"))) run.i = rPr->attr(QStringLiteral("i")) == QLatin1String("1") || rPr->attr(QStringLiteral("i")) == QLatin1String("true");
    if (rPr->has(QStringLiteral("u"))) run.u = rPr->attr(QStringLiteral("u")) != QLatin1String("none");
    if (rPr->has(QStringLiteral("strike"))) run.strike = rPr->attr(QStringLiteral("strike")) != QLatin1String("noStrike");
    if (rPr->has(QStringLiteral("baseline"))) {
        const int v = rPr->attr(QStringLiteral("baseline")).toInt();
        run.baseline = v > 0 ? 1 : v < 0 ? 2 : 0;
    }
    if (rPr->has(QStringLiteral("cap"))) run.caps = rPr->attr(QStringLiteral("cap")) == QLatin1String("all");
    if (rPr->has(QStringLiteral("spc"))) run.spc = rPr->attr(QStringLiteral("spc")).toDouble();
    if (rPr->has(QStringLiteral("lang"))) run.lang = rPr->attr(QStringLiteral("lang"));
    if (const Node *fill = rPr->child(QStringLiteral("solidFill"))) {
        const auto c = colorIn(fill, *ctx.color);
        if (c.isValid()) run.color = c;
        run.fill.clear();
    }
    // STYLE3D: Text Fill (gradient, pattern, none) and Text Outline.
    for (const char *kind : {"gradFill", "pattFill", "noFill", "blipFill"}) {
        if (!rPr->child(QLatin1String(kind))) continue;
        Look look;
        readFill(rPr, look, *ctx.color);
        run.fill = textFillMap(look);
        // What older builds (and the letters' glow) see: its first colour.
        if (look.fillStyle == 5) run.color = QColor(Qt::transparent);
        else if (look.fill.isValid()) run.color = look.fill;
        break;
    }
    if (const Node *ln = rPr->child(QStringLiteral("ln"))) {
        Look look;
        readLine(ln, look, *ctx.color);
        run.outline = textOutlineMap(look);
    }
    // An effect list replaces the inherited one: no glow or shadow in it is none.
    if (const Node *effects = rPr->child(QStringLiteral("effectLst"))) {
        run.reflection = readReflection(effects->child(QStringLiteral("reflection")));   // STYLE3D
        run.glowRad = 0;
        if (const Node *glow = effects->child(QStringLiteral("glow"))) {
            const auto c = colorIn(glow, *ctx.color);
            if (c.isValid()) { run.glowRad = glow->attr(QStringLiteral("rad")).toDouble(); run.glowColor = c; }
        }
        run.shadow = 0;
        // TEXT-SHADOW-2: the outer shadow, else the inner one (OmaShow keeps one).
        const Node *outer = effects->child(QStringLiteral("outerShdw"));
        if (const Node *shadow = outer ? outer : effects->child(QStringLiteral("innerShdw"))) {
            const auto c = colorIn(shadow, *ctx.color);
            if (c.isValid()) {
                run.shadow = 1;
                run.shadowColor = c;
                run.shadowDist = shadow->attr(QStringLiteral("dist"), QStringLiteral("0")).toDouble();
                run.shadowDir = shadow->attr(QStringLiteral("dir"), QStringLiteral("0")).toDouble() / 60000.0;
                run.shadowBlur = shadow->attr(QStringLiteral("blurRad"), QStringLiteral("0")).toDouble();
                run.shadowInner = !outer;
                run.shadowSx = outer ? shadow->attr(QStringLiteral("sx"), QStringLiteral("100000")).toDouble() / 1000.0 : 100;
                run.shadowSy = outer ? shadow->attr(QStringLiteral("sy"), QStringLiteral("100000")).toDouble() / 1000.0 : 100;
                run.shadowKx = outer ? shadow->attr(QStringLiteral("kx"), QStringLiteral("0")).toDouble() / 60000.0 : 0;
                run.shadowKy = outer ? shadow->attr(QStringLiteral("ky"), QStringLiteral("0")).toDouble() / 60000.0 : 0;
                run.shadowAlign = outer ? shadowAlignOf(shadow->attr(QStringLiteral("algn"), QStringLiteral("b"))) : 7;
            }
        }
    }
    if (const Node *latin = rPr->child(QStringLiteral("latin"))) {
        const auto face = mapFont(latin->attr(QStringLiteral("typeface")), ctx.color->theme);
        if (!face.isEmpty()) { int w = 0; run.family = splitWeight(face, &w); run.weight = w; }
    }
    if (const Node *link = rPr->child(QStringLiteral("hlinkClick"))) {
        const auto rid = link->attr(QStringLiteral("r:id"));
        const auto action = link->attr(QStringLiteral("action"));
        if (action.contains(QLatin1String("hlinksldjump")) && ctx.pkg && ctx.slideIdByPath) {
            const auto slidePath = ctx.pkg->target(ctx.partPath, rid);
            const auto id = ctx.slideIdByPath->value(slidePath);
            if (!id.isEmpty()) run.link = QStringLiteral("slide:") + id;
        } else if (ctx.pkg && !rid.isEmpty()) {
            const auto url = ctx.pkg->external(ctx.partPath, rid);
            if (!url.isEmpty()) run.link = url;
        }
    }
}

void applyPPr(ParaLook &para, RunLook &run, const Node *pPr, const TextContext &ctx) {
    if (!pPr) return;
    const auto algn = pPr->attr(QStringLiteral("algn"));
    if (algn == QLatin1String("l")) para.algn = 0;
    else if (algn == QLatin1String("ctr")) para.algn = 1;
    else if (algn == QLatin1String("r")) para.algn = 2;
    else if (algn == QLatin1String("just") || algn == QLatin1String("dist")) para.algn = 3;
    if (const Node *ln = pPr->path({"lnSpc", "spcPct"})) para.lnSpc = ln->attr(QStringLiteral("val")).toDouble() / 1000.0;
    if (const Node *bef = pPr->path({"spcBef", "spcPts"})) para.spcBef = bef->attr(QStringLiteral("val")).toDouble() / 100.0;
    if (pPr->has(QStringLiteral("defTabSz"))) para.defTabSz = pPr->attr(QStringLiteral("defTabSz")).toDouble();
    if (pPr->child(QStringLiteral("buNone"))) para.bullet = 0;
    else if (pPr->child(QStringLiteral("buChar")) || pPr->child(QStringLiteral("buBlip"))) para.bullet = 1;
    else if (const Node *num = pPr->child(QStringLiteral("buAutoNum"))) {
        para.bullet = 2;
        para.startAt = qMax(1, num->attr(QStringLiteral("startAt"), QStringLiteral("1")).toInt());
    }
    applyRPr(run, pPr->child(QStringLiteral("defRPr")), ctx);
}

// Layers holding lvl1pPr…lvl9pPr, lowest priority first; and the bodyPr
// layers the same way, for the anchor and the autofit.
struct TextChain {
    QVector<const Node *> levels;
    QVector<const Node *> bodies;
};

void resolveLevel(const TextChain &chain, int lvl, ParaLook &para, RunLook &run, const TextContext &ctx) {
    const auto name = QStringLiteral("lvl%1pPr").arg(qBound(0, lvl, 8) + 1);
    for (const auto *layer : chain.levels) {
        if (!layer) continue;
        applyPPr(para, run, layer->child(name), ctx);
    }
}

struct TextOut {
    QString text;
    QVector<TextRun> runs;
    RunLook box;
    ParaLook para;
    int listStyle = 0, listStart = 1;
    int anchor = 0;           // 0 top, 1 middle, 2 bottom
    bool shrink = false;
    bool grow = false;        // spAutoFit: the shape resizes to its text
    qreal fontScale = 1;
    // Text insets in EMU (lIns, tIns, rIns, bIns); the defaults are DrawingML's.
    qreal inset[4] = {91440, 45720, 91440, 45720};
    QString link;
    bool empty = true;
    // STYLE3D: the text's own 3-D (a:bodyPr: scene3d, sp3d in EMU, flatTx z).
    Effects3D::Scene scene3d;
    Effects3D::Format format3d;
    qreal flatZ = 0;
};

static void readInsets(const Node *bodyPr, qreal inset[4]) {
    static const char *names[4] = {"lIns", "tIns", "rIns", "bIns"};
    for (int i = 0; i < 4; ++i)
        if (bodyPr->has(QLatin1String(names[i])))
            inset[i] = qMax(0.0, bodyPr->attr(QLatin1String(names[i])).toDouble());
}

// STYLE3D: a run's whole set of text effects as SceneObject keys, lengths in
// EMU (TextRun::effects once scaled).
QVariantMap runEffectsEmu(const RunLook &l) {
    const bool glow = l.glowRad > 0 && l.glowColor.isValid();
    const bool shadow = l.shadow > 0;
    QVariantMap m{{"textGlowRadius", glow ? l.glowRad : 0.0}, {"shadowEnabled", shadow}};
    if (glow) m.insert("textGlowColor", l.glowColor.name(QColor::HexArgb));
    if (shadow) {
        m.insert("shadowColor", l.shadowColor.name(QColor::HexArgb));
        m.insert("shadowX", l.shadowDist * qCos(qDegreesToRadians(l.shadowDir)));
        m.insert("shadowY", l.shadowDist * qSin(qDegreesToRadians(l.shadowDir)));
        m.insert("shadowBlur", l.shadowBlur);
        m.insert("shadowInner", l.shadowInner);
        m.insert("shadowScaleX", qBound(-200.0, l.shadowSx, 200.0));
        m.insert("shadowScaleY", qBound(-200.0, l.shadowSy, 200.0));
        m.insert("shadowSkewX", qBound(-89.0, l.shadowKx, 89.0));
        m.insert("shadowSkewY", qBound(-89.0, l.shadowKy, 89.0));
        m.insert("shadowAlign", l.shadowInner ? 0 : l.shadowAlign);
    }
    m.insert("textReflection", Effects3D::toMap(l.reflection));
    return m;
}

// Reads a txBody (or a table cell's) into words, a box look and the runs
// that differ from it.
TextOut convertText(const Node *txBody, const TextChain &chain, const TextContext &ctx) {
    TextOut out;
    if (!txBody) return out;
    // bodyPr: the nearest layer that says wins.
    for (const auto *body : chain.bodies) {
        if (!body) continue;
        const auto anchor = body->attr(QStringLiteral("anchor"));
        if (anchor == QLatin1String("t")) out.anchor = 0;
        else if (anchor == QLatin1String("ctr")) out.anchor = 1;
        else if (anchor == QLatin1String("b")) out.anchor = 2;
        readInsets(body, out.inset);
    }
    if (const Node *own = txBody->child(QStringLiteral("bodyPr"))) {
        const auto anchor = own->attr(QStringLiteral("anchor"));
        if (anchor == QLatin1String("t")) out.anchor = 0;
        else if (anchor == QLatin1String("ctr")) out.anchor = 1;
        else if (anchor == QLatin1String("b")) out.anchor = 2;
        readInsets(own, out.inset);
        out.scene3d = readScene3d(own->child(QStringLiteral("scene3d")));
        out.format3d = readFormat3d(own->child(QStringLiteral("sp3d")), *ctx.color);
        if (const Node *flat = own->child(QStringLiteral("flatTx"))) out.flatZ = attrNumber(flat, "z", 0);
        if (own->child(QStringLiteral("spAutoFit"))) out.grow = true;
        if (const Node *fit = own->child(QStringLiteral("normAutofit"))) {
            out.shrink = true;
            if (fit->has(QStringLiteral("fontScale")))
                out.fontScale = qBound(0.1, fit->attr(QStringLiteral("fontScale")).toDouble() / 100000.0, 1.0);
        }
    }
    TextChain full = chain;
    full.levels.append(txBody->child(QStringLiteral("lstStyle")));

    struct Piece { int start, length; RunLook look; };
    QVector<Piece> pieces;
    bool firstParagraph = true, haveBox = false;
    int bulleted = 0, numbered = 0, plain = 0;
    for (const auto *p : txBody->all(QStringLiteral("p"))) {
        const Node *pPr = p->child(QStringLiteral("pPr"));
        const int lvl = qBound(0, pPr ? pPr->attr(QStringLiteral("lvl"), QStringLiteral("0")).toInt() : 0, 8);
        ParaLook para;
        RunLook base;
        resolveLevel(full, lvl, para, base, ctx);
        applyPPr(para, base, pPr, ctx);
        if (!firstParagraph) out.text += QLatin1Char('\n');
        QString line;
        bool any = false;
        for (const auto &k : p->kids) {
            if (k.name == QLatin1String("r") || k.name == QLatin1String("fld")) {
                const Node *t = k.child(QStringLiteral("t"));
                const QString words = t ? t->text : QString();
                RunLook look = base;
                applyRPr(look, k.child(QStringLiteral("rPr")), ctx);
                if (!look.link.isEmpty() && out.link.isEmpty()) out.link = look.link;
                if (words.isEmpty()) continue;
                pieces.append({out.text.size() + line.size() + (para.bullet > 0 ? lvl : 0), words.size(), look});
                line += words;
                any = true;
                if (!haveBox) { out.box = look; out.para = para; haveBox = true; }
            } else if (k.name == QLatin1String("br")) {
                line += QLatin1Char('\n');
            }
        }
        if (!any && !haveBox) {
            // An empty paragraph still says what the box would look like.
            RunLook look = base;
            applyRPr(look, p->child(QStringLiteral("endParaRPr")), ctx);
            out.box = look; out.para = para; haveBox = true;
        }
        if (any) {
            if (para.bullet == 1) ++bulleted; else if (para.bullet == 2) ++numbered; else ++plain;
            out.empty = false;
        }
        if (para.bullet > 0 && lvl > 0) out.text += QString(lvl, QLatin1Char('\t'));
        out.text += line;
        if (para.bullet == 2 && out.listStart == 1) out.listStart = para.startAt;
        firstParagraph = false;
    }
    if (bulleted + numbered > plain && bulleted + numbered > 0) out.listStyle = numbered > bulleted ? 2 : 1;
    // Runs that differ from the box.
    for (const auto &piece : pieces) {
        TextRun run;
        run.start = piece.start; run.length = piece.length;
        const auto &l = piece.look; const auto &b = out.box;
        bool differs = false;
        if (lookWeight(l) != lookWeight(b)) { run.weight = lookWeight(l); differs = true; }
        if ((l.i > 0) != (b.i > 0)) { run.italic = l.i > 0 ? 1 : 2; differs = true; }
        if ((l.u > 0) != (b.u > 0)) { run.underline = l.u > 0 ? 1 : 2; differs = true; }
        if ((l.strike > 0) != (b.strike > 0)) { run.strike = l.strike > 0 ? 1 : 2; differs = true; }
        if (l.baseline != b.baseline) { run.baseline = l.baseline; differs = true; }
        if (!qFuzzyCompare(l.sz + 1, b.sz + 1) && l.sz > 0) { run.fontSize = l.sz; differs = true; } // scaled later
        if (l.family != b.family && !l.family.isEmpty()) { run.fontFamily = l.family; differs = true; }
        if (l.color.isValid() && l.color != b.color) { run.color = l.color; differs = true; }
        if (l.lang != b.lang && !l.lang.isEmpty()) { run.language = l.lang; differs = true; }
        // STYLE3D: a fill, outline or effects of the stretch's own (lengths in
        // EMU here, slide units once applied).
        if (l.fill != b.fill) { run.fill = l.fill; if (l.fill.isEmpty() && l.color.isValid()) run.color = l.color; differs = true; }
        if (l.outline != b.outline) { run.outline = l.outline.isEmpty() ? QVariantMap{{"none", true}} : l.outline; differs = true; }
        if (runEffectsEmu(l) != runEffectsEmu(b)) { run.effects = runEffectsEmu(l); differs = true; }
        if (differs) out.runs.append(run);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Geometry.

struct Units {
    qreal k = 1;    // slide units per EMU
    qreal pt = 1;   // slide units per point
};

struct GroupSpace {
    QTransform points;   // child space → slide space
    qreal sx = 1, sy = 1;
    qreal rotation = 0;
};

QRectF xfrmRect(const Node *xfrm, const Units &u) {
    if (!xfrm) return QRectF();
    const Node *off = xfrm->child(QStringLiteral("off"));
    const Node *ext = xfrm->child(QStringLiteral("ext"));
    if (!off || !ext) return QRectF();
    return QRectF(off->attr(QStringLiteral("x")).toDouble() * u.k, off->attr(QStringLiteral("y")).toDouble() * u.k,
                  ext->attr(QStringLiteral("cx")).toDouble() * u.k, ext->attr(QStringLiteral("cy")).toDouble() * u.k);
}

struct Placement { QRectF rect; qreal rotation = 0; bool flipH = false, flipV = false; bool valid = false; };

Placement placementOf(const Node *xfrm, const Units &u, const GroupSpace &space) {
    Placement p;
    if (!xfrm) return p;
    const auto rect = xfrmRect(xfrm, u);
    if (rect.isEmpty() && rect.width() <= 0 && rect.height() <= 0) return p;
    const QPointF centre = space.points.map(rect.center());
    const qreal w = rect.width() * space.sx, h = rect.height() * space.sy;
    p.rect = QRectF(centre.x() - w / 2, centre.y() - h / 2, w, h);
    p.rotation = xfrm->attr(QStringLiteral("rot"), QStringLiteral("0")).toDouble() / 60000.0 + space.rotation;
    p.flipH = xfrm->attr(QStringLiteral("flipH")) == QLatin1String("1");
    p.flipV = xfrm->attr(QStringLiteral("flipV")) == QLatin1String("1");
    p.valid = true;
    return p;
}

int presetKind(const QString &prst, qreal *corner) {
    static const QHash<QString, int> kinds{
        {QStringLiteral("rect"), 0}, {QStringLiteral("roundRect"), 0}, {QStringLiteral("snipRoundRect"), 0},
        {QStringLiteral("round1Rect"), 0}, {QStringLiteral("round2SameRect"), 0}, {QStringLiteral("round2DiagRect"), 0},
        {QStringLiteral("ellipse"), 1}, {QStringLiteral("triangle"), 2}, {QStringLiteral("rtTriangle"), 2},
        {QStringLiteral("diamond"), 3}, {QStringLiteral("pentagon"), 4}, {QStringLiteral("hexagon"), 5},
        {QStringLiteral("star5"), 6}, {QStringLiteral("star4"), 6}, {QStringLiteral("star6"), 6}, {QStringLiteral("star8"), 6},
        {QStringLiteral("heart"), 7}, {QStringLiteral("rightArrow"), 8}, {QStringLiteral("homePlate"), 8},
        {QStringLiteral("chevron"), 9}, {QStringLiteral("wedgeRectCallout"), 10}, {QStringLiteral("wedgeRoundRectCallout"), 10},
        {QStringLiteral("parallelogram"), 11}, {QStringLiteral("can"), 12}, {QStringLiteral("line"), 13},
        {QStringLiteral("straightConnector1"), 13}, {QStringLiteral("mathPlus"), 14}, {QStringLiteral("plus"), 14},
        {QStringLiteral("mathMultiply"), 15}, {QStringLiteral("leftArrow"), 16}, {QStringLiteral("leftRightArrow"), 17},
        {QStringLiteral("flowChartDocument"), 18}, {QStringLiteral("trapezoid"), 19}, {QStringLiteral("flowChartTerminator"), 20},
        {QStringLiteral("cloud"), 21}, {QStringLiteral("cloudCallout"), 21}, {QStringLiteral("arc"), 22}, {QStringLiteral("donut"), 23},
        {QStringLiteral("flowChartProcess"), 0}, {QStringLiteral("flowChartConnector"), 1}, {QStringLiteral("flowChartDecision"), 3},
    };
    if (corner) *corner = prst.startsWith(QLatin1String("round")) ? 0.16667 : 0;
    return kinds.value(prst, -1);
}

// custGeom → a path in slide units inside `rect`.
QPainterPath customPath(const Node *custGeom, const QRectF &rect) {
    QPainterPath out;
    const Node *list = custGeom ? custGeom->child(QStringLiteral("pathLst")) : nullptr;
    if (!list) return out;
    for (const auto *path : list->all(QStringLiteral("path"))) {
        const qreal w = path->attr(QStringLiteral("w"), QStringLiteral("0")).toDouble();
        const qreal h = path->attr(QStringLiteral("h"), QStringLiteral("0")).toDouble();
        const qreal sx = w > 0 ? rect.width() / w : 1, sy = h > 0 ? rect.height() / h : 1;
        auto pt = [&](const Node *n) {
            return QPointF(rect.x() + n->attr(QStringLiteral("x")).toDouble() * sx,
                           rect.y() + n->attr(QStringLiteral("y")).toDouble() * sy);
        };
        for (const auto &cmd : path->kids) {
            const auto pts = cmd.all(QStringLiteral("pt"));
            if (cmd.name == QLatin1String("moveTo") && pts.size() >= 1) out.moveTo(pt(pts[0]));
            else if (cmd.name == QLatin1String("lnTo") && pts.size() >= 1) out.lineTo(pt(pts[0]));
            else if (cmd.name == QLatin1String("cubicBezTo") && pts.size() >= 3) out.cubicTo(pt(pts[0]), pt(pts[1]), pt(pts[2]));
            else if (cmd.name == QLatin1String("quadBezTo") && pts.size() >= 2) out.quadTo(pt(pts[0]), pt(pts[1]));
            else if (cmd.name == QLatin1String("close")) out.closeSubpath();
            else if (cmd.name == QLatin1String("arcTo")) {
                // An elliptical arc from the current point; approximated by its chord.
                const qreal wr = cmd.attr(QStringLiteral("wR")).toDouble() * sx, hr = cmd.attr(QStringLiteral("hR")).toDouble() * sy;
                const qreal st = cmd.attr(QStringLiteral("stAng")).toDouble() / 60000.0, sw = cmd.attr(QStringLiteral("swAng")).toDouble() / 60000.0;
                const QPointF cur = out.currentPosition();
                const QPointF centre(cur.x() - wr * qCos(qDegreesToRadians(st)), cur.y() - hr * qSin(qDegreesToRadians(st)));
                out.arcTo(QRectF(centre.x() - wr, centre.y() - hr, 2 * wr, 2 * hr), -st, -sw);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Warnings are counted per message with the slides they came from.

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

// ---------------------------------------------------------------------------
// Placeholders.

struct Ph { bool is = false; QString type, idx; };

Ph phOf(const Node *sp) {
    Ph ph;
    const Node *nv = sp->child(QStringLiteral("nvSpPr"));
    if (!nv) nv = sp->child(QStringLiteral("nvPicPr"));
    if (!nv) nv = sp->child(QStringLiteral("nvGraphicFramePr"));
    const Node *node = nv ? nv->path({"nvPr", "ph"}) : nullptr;
    if (!node) return ph;
    ph.is = true;
    ph.type = node->attr(QStringLiteral("type"), QStringLiteral("body"));
    ph.idx = node->attr(QStringLiteral("idx"));
    return ph;
}

QString phClass(const QString &type) {
    if (type == QLatin1String("title") || type == QLatin1String("ctrTitle")) return QStringLiteral("title");
    if (type == QLatin1String("body") || type == QLatin1String("subTitle") || type == QLatin1String("obj") || type.isEmpty())
        return QStringLiteral("body");
    return type;
}

const Node *spTreeOf(const Node *root) {
    return root ? root->path({"cSld", "spTree"}) : nullptr;
}

// The placeholder in another part that a placeholder inherits from.
const Node *findPh(const Node *root, const Ph &want, bool useIdx) {
    const Node *tree = spTreeOf(root);
    if (!tree) return nullptr;
    QVector<const Node *> shapes;
    flattened(*tree, shapes);
    const Node *best = nullptr;
    int bestScore = 0;
    for (const auto *sp : shapes) {
        if (sp->name != QLatin1String("sp")) continue;
        const Ph ph = phOf(sp);
        if (!ph.is) continue;
        int score = 0;
        if (useIdx && !want.idx.isEmpty() && ph.idx == want.idx) score = 3;
        else if (phClass(ph.type) == phClass(want.type)) score = ph.type == want.type ? 2 : 1;
        if (score > bestScore) { best = sp; bestScore = score; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// The conversion of one part's shapes.

struct MasterInfo {
    QString path;
    QString id;             // Master.id in the document
    ColorContext color;
    Theme theme;
    const Node *root = nullptr;
};

struct Reader {
    const Package &pkg;
    Units u;
    Document doc;
    Warnings warnings;
    const Node *presentation = nullptr;
    QString presentationPath;
    std::map<QString, MasterInfo> masters;  // by part path; a map so references stay put
    QHash<QString, QString> layoutIds;      // part path → SlideLayout.id
    QHash<QString, QString> slideIdByPath;
    QString scratchDir;

    explicit Reader(const Package &package) : pkg(package) {}

    const Node *defaultTextStyle() const {
        return presentation ? presentation->child(QStringLiteral("defaultTextStyle")) : nullptr;
    }

    // What one shape needs to know about where it is.
    struct Scope {
        QString partPath;              // the part the shapes are in (slide, layout or master)
        const Node *slide = nullptr;   // null when converting a layout or master
        const Node *layout = nullptr;
        const MasterInfo *master = nullptr;
        int slideNumber = 0;
        QString slideId;
        QVector<SceneObject> *objects = nullptr;
        QHash<QString, QString> *spidToObject = nullptr;
        MasterFields *fields = nullptr;
        bool fieldsSeen = false;
    };

    TextContext textContext(const Scope &scope) const {
        TextContext ctx;
        ctx.pkg = &pkg;
        ctx.partPath = scope.partPath;
        ctx.color = &scope.master->color;
        ctx.slideIdByPath = &slideIdByPath;
        return ctx;
    }

    // Layers for a shape's text, lowest first.
    TextChain chainFor(const Scope &scope, const Ph &ph, const Node *layoutSp, const Node *masterSp) const {
        TextChain chain;
        chain.levels.append(defaultTextStyle());
        const Node *styles = scope.master->root ? scope.master->root->child(QStringLiteral("txStyles")) : nullptr;
        if (styles) {
            const auto cls = ph.is ? phClass(ph.type) : QString();
            if (cls == QLatin1String("title")) chain.levels.append(styles->child(QStringLiteral("titleStyle")));
            else if (cls == QLatin1String("body")) chain.levels.append(styles->child(QStringLiteral("bodyStyle")));
            else chain.levels.append(styles->child(QStringLiteral("otherStyle")));
        }
        for (const auto *sp : {masterSp, layoutSp}) {
            if (!sp) continue;
            const Node *body = sp->child(QStringLiteral("txBody"));
            if (!body) continue;
            chain.levels.append(body->child(QStringLiteral("lstStyle")));
            chain.bodies.append(body->child(QStringLiteral("bodyPr")));
        }
        return chain;
    }

    void applyLook(SceneObject &o, const Look &look) const {
        o.fillStyle = look.fillSet ? look.fillStyle : 5;
        if (look.fill.isValid()) o.fill = look.fill;
        if (look.fill2.isValid()) o.fillSecondary = look.fill2;
        o.fillAngle = look.angle;
        // FILL-UI-1: the gradient's own stops and path, the pattern's name.
        if (o.fillStyle == 1 || o.fillStyle == 2) {
            o.gradientStops = look.stops;
            o.gradientPath = look.gradPath;
            o.gradientFocusL = look.focus[0]; o.gradientFocusT = look.focus[1];
            o.gradientFocusR = look.focus[2]; o.gradientFocusB = look.focus[3];
            o.gradientScaled = look.scaled;
            o.gradientRotate = look.rotate;
        }
        if (o.fillStyle == 3) o.patternPreset = look.pattern;
        if (look.line) {
            o.strokeColor = look.lineColor;
            o.strokeWidth = qMax(0.5, look.lineWidthEmu * u.k);
            o.strokeStyle = look.dash;
            o.strokeDash = look.dashPreset;
            o.strokeDashCustom = look.customDash;
            o.strokeCompound = look.compound;
            // Office draws a line with no cap or join given flat and round
            // (MS-OI29500 20.1.2.2.24 and 20.1.8), not the schema's square.
            o.strokeCap = look.cap >= 0 ? look.cap : 0;
            o.strokeJoin = look.join >= 0 ? look.join : 2;
            if (look.lineGradient && look.lineStops.size() >= 2) {
                o.strokeGradient = true;
                o.strokeStops = look.lineStops;
                o.strokeAngle = look.lineAngle;
                o.strokeGradientPath = look.lineGradPath;
            }
        } else o.strokeWidth = 0;
        FillStyle::syncLegacy(o);
        if (look.shadow) {
            o.shadowEnabled = true;
            o.shadowColor = look.shadowColor;
            const qreal d = look.shadowDistEmu * u.k;
            o.shadowX = d * qCos(qDegreesToRadians(look.shadowDir));
            o.shadowY = d * qSin(qDegreesToRadians(look.shadowDir));
            o.shadowBlur = look.shadowBlurEmu * u.k;
            o.shadowInner = look.shadowInner;
            o.shadowScaleX = qBound(-200.0, look.shadowSx, 200.0);
            o.shadowScaleY = qBound(-200.0, look.shadowSy, 200.0);
            o.shadowSkewX = qBound(-89.0, look.shadowKx, 89.0);
            o.shadowSkewY = qBound(-89.0, look.shadowKy, 89.0);
            o.shadowAlign = look.shadowAlign;
        }
        if (look.glowRadEmu > 0 && look.glowColor.isValid()) {
            o.glowRadius = look.glowRadEmu * u.k;
            o.glowColor = look.glowColor;
        }
        apply3d(o, look);
    }

    // STYLE3D: soft edges, reflection and 3-D, EMU to slide units.
    void apply3d(SceneObject &o, const Look &look) const {
        o.softEdge = look.softEdgeEmu * u.k;
        o.reflection = look.reflection;
        Effects3D::scale(o.reflection, u.k);
        o.format3d = look.format3d;
        Effects3D::scale(o.format3d, u.k);
        o.scene3d = look.scene3d;
    }

    // EFFECTS-UI-1: a box with no fill and no outline has its glow and shadow
    // drawn on its words by PowerPoint, so they become the words' own, unless
    // the words already name theirs. The whole shadow carries over, inner and
    // perspective too (TEXT-SHADOW-2: the words draw them). A box with an outline but no fill casts its
    // shadow from the outline and the words alike, so then only the shadow
    // carries over (shadowOnly).
    void boxEffectsOnWords(SceneObject &words, const Look &look, bool shadowOnly = false) const {
        if (!shadowOnly && look.glowRadEmu > 0 && look.glowColor.isValid() && words.textGlowRadius <= 0) {
            words.textGlowRadius = look.glowRadEmu / 12700.0 * u.pt;
            words.textGlowColor = look.glowColor;
        }
        if (look.shadow && !words.shadowEnabled) {
            words.shadowEnabled = true;
            words.shadowColor = look.shadowColor;
            const qreal d = look.shadowDistEmu * u.k;
            words.shadowX = d * qCos(qDegreesToRadians(look.shadowDir));
            words.shadowY = d * qSin(qDegreesToRadians(look.shadowDir));
            words.shadowBlur = look.shadowBlurEmu * u.k;
            words.shadowInner = look.shadowInner;
            words.shadowScaleX = qBound(-200.0, look.shadowSx, 200.0);
            words.shadowScaleY = qBound(-200.0, look.shadowSy, 200.0);
            words.shadowSkewX = qBound(-89.0, look.shadowKx, 89.0);
            words.shadowSkewY = qBound(-89.0, look.shadowKy, 89.0);
            words.shadowAlign = look.shadowInner ? 0 : look.shadowAlign;
        }
    }

    // BOTH-EFFECTS-1: a box with no fill and no outline keeps its own effects
    // as the text box's (glow*, boxShadow*), apart from its words' own; they
    // are drawn on the words where the words have none (TextLayout::paint),
    // and written back to the box's p:spPr.
    void boxEffectsOfWords(SceneObject &words, const Look &look) const {
        apply3d(words, look);   // STYLE3D: the box's own, kept on the text box
        if (look.glowRadEmu > 0 && look.glowColor.isValid()) {
            words.glowRadius = look.glowRadEmu * u.k;
            words.glowColor = look.glowColor;
        }
        if (look.shadow) {
            SceneObject box;
            applyLook(box, look);
            words.boxShadowEnabled = true;
            words.boxShadowColor = box.shadowColor; words.boxShadowX = box.shadowX; words.boxShadowY = box.shadowY;
            words.boxShadowBlur = box.shadowBlur; words.boxShadowInner = box.shadowInner;
            words.boxShadowScaleX = box.shadowScaleX; words.boxShadowScaleY = box.shadowScaleY;
            words.boxShadowSkewX = box.shadowSkewX; words.boxShadowSkewY = box.shadowSkewY; words.boxShadowAlign = box.shadowAlign;
        }
    }

    QVariantMap scaledOutline(QVariantMap m) const {
        if (m.contains("strokeWidth")) m.insert("strokeWidth", qMax(0.25, m.value("strokeWidth").toDouble() * u.k));
        QVariantMap checked;
        return FillStyle::checkTextOutline(m, &checked) ? checked : QVariantMap();
    }

    void applyText(SceneObject &o, const TextOut &text, const RunLook &fallback) {
        o.type = ObjectType::Text;
        // OmaShow text has no inset of its own: the text object is separate
        // from any filled box, so take PowerPoint's inset off its rectangle
        // and the words land, and wrap, where PowerPoint puts them.
        {
            const qreal l = text.inset[0] * u.k, t = text.inset[1] * u.k;
            const qreal r = text.inset[2] * u.k, b = text.inset[3] * u.k;
            if (o.rect.width() > l + r + 1) { o.rect.setLeft(o.rect.left() + l); o.rect.setRight(o.rect.right() - r); }
            if (o.rect.height() > t + b + 1) { o.rect.setTop(o.rect.top() + t); o.rect.setBottom(o.rect.bottom() - b); }
        }
        o.text = text.text;
        const auto &b = text.box;
        const qreal sz = (b.sz > 0 ? b.sz : fallback.sz > 0 ? fallback.sz : 1800) / 100.0 * text.fontScale;
        o.fontSize = sz * u.pt;
        o.fontWeight = lookWeight(b);
        o.italic = b.i > 0;
        o.underline = b.u > 0;
        o.strike = b.strike > 0;
        o.textColor = b.color.isValid() ? b.color : fallback.color.isValid() ? fallback.color : QColor(Qt::black);
        o.fontFamily = !b.family.isEmpty() ? b.family : !fallback.family.isEmpty() ? fallback.family : QStringLiteral("Inter");
        o.uppercase = b.caps;
        o.letterSpacing = b.spc / 100.0 * u.pt;
        if (b.glowRad > 0 && b.glowColor.isValid()) {
            o.textGlowRadius = b.glowRad / 12700.0 * u.pt;   // EMU → points → slide units
            o.textGlowColor = b.glowColor;
        }
        if (b.shadow > 0) {
            o.shadowEnabled = true;
            o.shadowColor = b.shadowColor;
            const qreal d = b.shadowDist * u.k;   // EMU → slide units, as set (drawn by PowerPoint's sizing)
            o.shadowX = d * qCos(qDegreesToRadians(b.shadowDir));
            o.shadowY = d * qSin(qDegreesToRadians(b.shadowDir));
            o.shadowBlur = b.shadowBlur * u.k;
            o.shadowInner = b.shadowInner;
            o.shadowScaleX = qBound(-200.0, b.shadowSx, 200.0);
            o.shadowScaleY = qBound(-200.0, b.shadowSy, 200.0);
            o.shadowSkewX = qBound(-89.0, b.shadowKx, 89.0);
            o.shadowSkewY = qBound(-89.0, b.shadowKy, 89.0);
            o.shadowAlign = b.shadowInner ? 0 : b.shadowAlign;
        }
        if (!b.lang.isEmpty()) { o.language = b.lang; o.language.replace(QLatin1Char('-'), QLatin1Char('_')); }
        o.textAlign = text.para.algn < 0 ? 0 : text.para.algn;
        o.verticalAlign = text.anchor;
        o.lineHeight = text.para.lnSpc > 0 ? text.para.lnSpc : 100;
        o.paragraphSpacing = text.para.spcBef > 0 ? text.para.spcBef * u.pt : 0;
        // Tabs step to PowerPoint's default stops (1 inch unless the deck
        // says otherwise), not our own default of four times the text size.
        if (o.text.contains(QLatin1Char('\t'))) o.tabStop = (text.para.defTabSz > 0 ? text.para.defTabSz : 914400) * u.k;
        o.listStyle = text.listStyle;
        o.listStart = text.listStart;
        // No autofit in the file: PowerPoint draws the text past the box.
        o.textFit = text.shrink ? 1 : text.grow ? 3 : 2;
        // PowerPoint's own shrink is kept until the box is changed (fingerprinted
        // once the deck is read: SceneObject::fitKept).
        if (text.shrink) o.fitKept = QStringLiteral("*");
        o.fillStyle = 5;
        o.strokeWidth = 0;
        o.fillToken.clear(); o.textColorToken.clear(); o.fontToken.clear();
        // STYLE3D: the words' Text Fill and Outline, reflection and 3-D.
        FillStyle::checkTextFill(b.fill, &o.textFill);
        o.textOutline = scaledOutline(b.outline);
        o.textReflection = b.reflection;
        Effects3D::scale(o.textReflection, u.k);
        o.textScene3d = text.scene3d;
        o.textFormat3d = text.format3d;
        Effects3D::scale(o.textFormat3d, u.k);
        o.textFlatZ = text.flatZ * u.k;
        QVector<TextRun> runs = text.runs;
        for (auto &run : runs) {
            run.outline = scaledOutline(run.outline);
            if (!run.effects.isEmpty()) {
                QVariantMap fx = run.effects;
                for (const char *key : {"textGlowRadius", "shadowX", "shadowY", "shadowBlur"})
                    if (fx.contains(QLatin1String(key))) fx.insert(QLatin1String(key), fx.value(QLatin1String(key)).toDouble() * u.k);
                Effects3D::Reflection r;
                if (Effects3D::fromMap(fx.value("textReflection"), &r)) { Effects3D::scale(r, u.k); fx.insert("textReflection", Effects3D::toMap(r)); }
                QVariantMap checked;
                run.effects = FillStyle::checkTextEffects(fx, &checked) ? checked : QVariantMap();
            }
            QVariantMap checked;
            run.fill = FillStyle::checkTextFill(run.fill, &checked) ? checked : QVariantMap();
            if (run.fontSize > 0) run.fontSize = run.fontSize / 100.0 * text.fontScale * u.pt;
            if (!run.language.isEmpty()) run.language.replace(QLatin1Char('-'), QLatin1Char('_'));
        }
        o.runs = TextRuns::tidy(runs, o.text.size());
        if (!text.link.isEmpty()) {
            if (text.link.startsWith(QLatin1String("slide:"))) { o.linkKind = 3; o.linkTarget = text.link.mid(6); }
            else if (text.link.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive)) { o.linkKind = 2; o.linkTarget = text.link.mid(7); }
            else if (text.link.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) { o.linkKind = 1; o.linkTarget = text.link; }
            // A link the deck would refuse to reopen with is no link.
            if (o.linkKind && !Links::validate(o.linkKind, o.linkTarget, doc, true).isEmpty()) {
                o.linkKind = 0; o.linkTarget.clear();
                warnings.add(QStringLiteral("A link OmaShow cannot follow was left off: %1").arg(text.link.left(80)), 0);
            }
        }
    }

    void finish(SceneObject &o, const Placement &p, const QStringList &groups) const {
        o.rect = p.rect;
        o.rotation = p.rotation;
        o.groups = groups;
    }

    // ---- shapes

    void convertShape(const Node *sp, Scope &scope, const GroupSpace &space, const QStringList &groups) {
        const Ph ph = phOf(sp);
        const Node *layoutSp = nullptr, *masterSp = nullptr;
        if (ph.is) {
            const auto cls = phClass(ph.type);
            if (cls == QLatin1String("sldNum") || cls == QLatin1String("dt") || cls == QLatin1String("ftr") || cls == QLatin1String("hdr")) {
                if (scope.slide && scope.fields) {
                    const Node *body = sp->child(QStringLiteral("txBody"));
                    const auto words = body ? body->words().trimmed() : QString();
                    if (cls == QLatin1String("sldNum")) scope.fields->showNumber = true;
                    else if (cls == QLatin1String("ftr") && !words.isEmpty()) { scope.fields->showFooter = true; scope.fields->footer = words; }
                    else if (cls == QLatin1String("dt")) { scope.fields->showDate = true; if (!body || !body->path({"p", "fld"})) scope.fields->date = words; }
                    scope.fieldsSeen = true;
                }
                return;
            }
            if (scope.slide) {
                layoutSp = findPh(scope.layout, ph, true);
                const Ph layoutPh = layoutSp ? phOf(layoutSp) : ph;
                masterSp = findPh(scope.master->root, layoutPh, false);
            } else if (scope.layout && scope.partPath != scope.master->path) {
                masterSp = findPh(scope.master->root, ph, false);
            }
        }

        // Geometry: its own, else inherited.
        const Node *spPr = sp->child(QStringLiteral("spPr"));
        Placement place = placementOf(spPr ? spPr->child(QStringLiteral("xfrm")) : nullptr, u, space);
        for (const auto *from : {layoutSp, masterSp}) {
            if (place.valid || !from) continue;
            const Node *pr = from->child(QStringLiteral("spPr"));
            place = placementOf(pr ? pr->child(QStringLiteral("xfrm")) : nullptr, u, GroupSpace());
        }
        if (!place.valid) return;

        // Look: its own, else the placeholder it fills, else the theme reference.
        Look look;
        const auto &color = scope.master->color;
        readFill(spPr, look, color);
        if (spPr) readLine(spPr->child(QStringLiteral("ln")), look, color);
        if (spPr) readEffects(spPr->child(QStringLiteral("effectLst")), look, color);
        read3d(spPr, look, color);   // STYLE3D
        for (const auto *from : {layoutSp, masterSp}) {
            if (!from) continue;
            const Node *pr = from->child(QStringLiteral("spPr"));
            if (!look.fillSet) readFill(pr, look, color);
            if (!look.lineSet && pr) readLine(pr->child(QStringLiteral("ln")), look, color);
        }
        applyStyleRefs(sp->child(QStringLiteral("style")), look, color);
        // FILL-UI-1: a shape filled with a picture keeps it.
        SceneObject picture;
        if (look.blip) {
            if (readBlipFill(picture, look.blip, scope)) look.fillStyle = 4;
            else warnings.add(QStringLiteral("A shape filled with a picture was given no fill"), scope.slideNumber);
        }

        // Words.
        const Node *txBody = sp->child(QStringLiteral("txBody"));
        TextChain chain = chainFor(scope, ph, layoutSp, masterSp);
        TextOut text = convertText(txBody, chain, textContext(scope));
        RunLook fallback;
        { ParaLook para; resolveLevel(chain, 0, para, fallback, textContext(scope)); }

        const bool visibleBox = (look.fillSet && look.fillStyle != 5) || look.line;
        const QString spid = [&] {
            const Node *nv = sp->child(QStringLiteral("nvSpPr"));
            const Node *c = nv ? nv->child(QStringLiteral("cNvPr")) : nullptr;
            return c ? c->attr(QStringLiteral("id")) : QString();
        }();
        const QString name = [&] {
            const Node *nv = sp->child(QStringLiteral("nvSpPr"));
            const Node *c = nv ? nv->child(QStringLiteral("cNvPr")) : nullptr;
            return c ? c->attr(QStringLiteral("name")) : QString();
        }();
        const bool hidden = [&] {
            const Node *nv = sp->child(QStringLiteral("nvSpPr"));
            const Node *c = nv ? nv->child(QStringLiteral("cNvPr")) : nullptr;
            return c && c->attr(QStringLiteral("hidden")) == QLatin1String("1");
        }();

        QStringList own = groups;
        QString firstId;
        const bool hasWords = !text.empty && !text.text.trimmed().isEmpty();
        // FILL-UI-1: a rectangle with words and a fill or an outline is one
        // text box with a box of its own, as PowerPoint has it; other shapes
        // with words stay a shape and a text box, grouped.
        const Node *prstOf = spPr ? spPr->child(QStringLiteral("prstGeom")) : nullptr;
        const bool ownBox = visibleBox && hasWords && !(spPr && spPr->child(QStringLiteral("custGeom"))) &&
                            (!prstOf || prstOf->attr(QStringLiteral("prst")) == QLatin1String("rect"));
        if (visibleBox && !ownBox) {
            SceneObject box;
            box.id = Edit::newId(QStringLiteral("shape"));
            box.type = ObjectType::Rect;
            finish(box, place, groups);
            applyLook(box, look);
            const Node *prst = spPr ? spPr->child(QStringLiteral("prstGeom")) : nullptr;
            const Node *cust = spPr ? spPr->child(QStringLiteral("custGeom")) : nullptr;
            qreal corner = 0;
            const int kind = prst ? presetKind(prst->attr(QStringLiteral("prst")), &corner) : cust ? -2 : 0;
            if (kind == -2) {
                const auto path = customPath(cust, place.rect);
                if (!path.isEmpty()) { const qreal rot = box.rotation; Shape::assignPath(box, path); box.rotation = rot; }
                else box.shapeKind = 0;
            } else if (kind < 0) {
                box.shapeKind = 0;
                warnings.add(QStringLiteral("The %1 shape was drawn as a rectangle").arg(prst->attr(QStringLiteral("prst"))), scope.slideNumber);
            } else {
                box.shapeKind = kind;
                if (corner > 0) {
                    // The rounding is a fraction of the shorter side; a pill is 50000.
                    if (const Node *adjustments = prst->child(QStringLiteral("avLst")))
                        for (const auto *gd : adjustments->all(QStringLiteral("gd")))
                            if (gd->attr(QStringLiteral("name")) == QLatin1String("adj")) {
                                const auto formula = gd->attr(QStringLiteral("fmla"));
                                if (formula.startsWith(QLatin1String("val "))) corner = qBound(0.0, formula.mid(4).toDouble() / 100000.0, 0.5);
                            }
                    box.cornerRadius = qMin(place.rect.width(), place.rect.height()) * corner;
                }
                if (kind == 13 && (place.flipV != place.flipH)) {
                    // A diagonal line: the rectangle's other diagonal.
                    QPainterPath path;
                    path.moveTo(place.rect.bottomLeft());
                    path.lineTo(place.rect.topRight());
                    const qreal rot = box.rotation; const auto keep = box.rect;
                    Shape::assignPath(box, path); box.rotation = rot; box.rect = keep;
                }
            }
            if (!text.empty && !text.text.trimmed().isEmpty()) {
                const auto groupId = Edit::newId(QStringLiteral("group"));
                own.append(groupId);
                box.groups = own;
            }
            if (!name.isEmpty()) box.altTitle = name;
            box.hidden = hidden;
            if (box.fillStyle == 4) FillStyle::copyPicture(box, picture);
            scope.objects->append(box);
            firstId = box.id;
        }
        if (!text.empty && !text.text.trimmed().isEmpty()) {
            SceneObject words;
            words.id = Edit::newId(QStringLiteral("text"));
            finish(words, place, own);
            applyText(words, text, fallback);
            if (ownBox) {
                // The box is the shape's own rectangle: the words' less the insets taken off.
                SceneObject box;
                applyLook(box, look);
                if (box.fillStyle == 4) FillStyle::copyPicture(box, picture);
                FillStyle::copyFillAndLine(words, box);
                words.boxDrawn = true;
                words.boxInsetL = qMax(0.0, words.rect.left() - place.rect.left());
                words.boxInsetT = qMax(0.0, words.rect.top() - place.rect.top());
                words.boxInsetR = qMax(0.0, place.rect.right() - words.rect.right());
                words.boxInsetB = qMax(0.0, place.rect.bottom() - words.rect.bottom());
                boxEffectsOfWords(words, look);
            } else if (!visibleBox) boxEffectsOfWords(words, look);
            // BOX-SHADOW-2: an outlined box with no fill casts one shadow from
            // its outline and its words together, turned about the box (sx/sy,
            // kx/ky, algn): the words carry it as their box's shadow (boxShadow*,
            // drawn as the box's), not as their own.
            else if (!(look.fillSet && look.fillStyle != 5) && look.shadow) {
                Look shadowLook = look;
                shadowLook.glowRadEmu = 0;
                boxEffectsOfWords(words, shadowLook);
            }
            if (!name.isEmpty() && firstId.isEmpty()) words.altTitle = name;
            words.hidden = hidden;
            scope.objects->append(words);
            if (firstId.isEmpty()) firstId = words.id;
        } else if (!visibleBox) {
            return;
        }
        if (scope.spidToObject && !spid.isEmpty() && !firstId.isEmpty()) scope.spidToObject->insert(spid, firstId);
    }

    // ---- pictures and films

    bool decodePicture(SceneObject &o, const QString &mediaPath, int slideNumber) {
        const auto bytes = generated.contains(mediaPath) ? generated.value(mediaPath) : pkg.bytes(mediaPath);
        const auto suffix = QFileInfo(mediaPath).suffix().toLower();
        if (bytes.isEmpty()) {
            warnings.add(QStringLiteral("A picture's file was missing from the deck"), slideNumber);
            return false;
        }
        QString error;
        if (suffix == QLatin1String("svg")) {
            if (SvgAsset::decode(o, bytes, &error)) return true;
        } else if (suffix == QLatin1String("emf") || suffix == QLatin1String("wmf") || Metafile::recognise(bytes)) {
            const QByteArray svg = Metafile::toSvg(bytes, &error);
            if (!svg.isEmpty() && SvgAsset::decode(o, svg, &error)) return true;
            warnings.add(QStringLiteral("A picture in the %1 format could not be drawn").arg(suffix.toUpper()), slideNumber);
            return false;
        } else if (ImageAsset::decode(o, bytes, &error)) return true;
        warnings.add(QStringLiteral("A picture could not be read: %1").arg(error.isEmpty() ? suffix : error), slideNumber);
        return false;
    }

    // FILL-UI-1: a shape's a:blipFill: the picture, cropped (srcRect), stretched
    // into the box less its insets (fillRect) or tiled (tile), see-through
    // (alphaModFix), with the picture's own adjustments.
    bool readBlipFill(SceneObject &o, const Node *blipFill, const Scope &scope) {
        const Node *blip = blipFill->child(QStringLiteral("blip"));
        if (!blip) return false;
        QString mediaPath;
        QVector<const Node *> svgs;
        blip->collect(QStringLiteral("svgBlip"), svgs);
        if (!svgs.isEmpty()) {
            const auto svgPath = pkg.target(scope.partPath, svgs.first()->attr(QStringLiteral("embed")));
            if (pkg.has(svgPath)) mediaPath = svgPath;
        }
        if (mediaPath.isEmpty()) mediaPath = pkg.target(scope.partPath, blip->attr(QStringLiteral("embed")));
        if (mediaPath.isEmpty() || !decodePicture(o, mediaPath, scope.slideNumber)) return false;
        o.fillStyle = 4;
        o.imageMode = 2;
        const auto frac = [](const Node *n, const char *name) { return n->attr(QLatin1String(name), QStringLiteral("0")).toDouble() / 100000.0; };
        if (const Node *src = blipFill->child(QStringLiteral("srcRect"))) {
            const qreal l = frac(src, "l"), t = frac(src, "t"), r = frac(src, "r"), b = frac(src, "b");
            if (l + r < 1 && t + b < 1 && (l > 0 || t > 0 || r > 0 || b > 0)) o.imageCrop = QRectF(l, t, 1 - l - r, 1 - t - b);
        }
        if (const Node *tile = blipFill->child(QStringLiteral("tile"))) {
            o.pictureTile = true;
            o.pictureTileX = tile->attr(QStringLiteral("tx"), QStringLiteral("0")).toDouble() * u.k;
            o.pictureTileY = tile->attr(QStringLiteral("ty"), QStringLiteral("0")).toDouble() * u.k;
            o.pictureTileScaleX = qBound(1.0, tile->attr(QStringLiteral("sx"), QStringLiteral("100000")).toDouble() / 1000.0, 1000.0);
            o.pictureTileScaleY = qBound(1.0, tile->attr(QStringLiteral("sy"), QStringLiteral("100000")).toDouble() / 1000.0, 1000.0);
            const auto flip = tile->attr(QStringLiteral("flip"), QStringLiteral("none"));
            o.pictureTileFlip = flip == QLatin1String("x") ? 1 : flip == QLatin1String("y") ? 2 : flip == QLatin1String("xy") ? 3 : 0;
            static const QStringList aligns{"tl", "t", "tr", "l", "ctr", "r", "bl", "b", "br"};
            o.pictureTileAlign = qMax(0, aligns.indexOf(tile->attr(QStringLiteral("algn"), QStringLiteral("tl"))));
        } else if (const Node *stretch = blipFill->child(QStringLiteral("stretch"))) {
            if (const Node *fill = stretch->child(QStringLiteral("fillRect"))) {
                o.pictureInsetL = qBound(-10.0, frac(fill, "l"), 10.0); o.pictureInsetT = qBound(-10.0, frac(fill, "t"), 10.0);
                o.pictureInsetR = qBound(-10.0, frac(fill, "r"), 10.0); o.pictureInsetB = qBound(-10.0, frac(fill, "b"), 10.0);
            }
        }
        const auto rot = blipFill->attr(QStringLiteral("rotWithShape"), QStringLiteral("1"));
        o.pictureRotate = rot != QLatin1String("0") && rot != QLatin1String("false");
        for (const auto &k : blip->kids) {
            if (k.name == QLatin1String("alphaModFix")) o.pictureTransparency = qBound(0.0, 1 - k.attr(QStringLiteral("amt"), QStringLiteral("100000")).toDouble() / 100000.0, 1.0);
            else if (k.name == QLatin1String("grayscl")) o.imageSaturation = 0;
            else if (k.name == QLatin1String("lum")) {
                o.imageBrightness = k.attr(QStringLiteral("bright"), QStringLiteral("0")).toDouble() / 100000.0;
                o.imageContrast = 1 + k.attr(QStringLiteral("contrast"), QStringLiteral("0")).toDouble() / 100000.0;
            }
        }
        return true;
    }

    void convertPicture(const Node *pic, Scope &scope, const GroupSpace &space, const QStringList &groups) {
        const Ph ph = phOf(pic);
        const Node *spPr = pic->child(QStringLiteral("spPr"));
        Placement place = placementOf(spPr ? spPr->child(QStringLiteral("xfrm")) : nullptr, u, space);
        if (!place.valid && ph.is && scope.slide) {
            const Node *layoutSp = findPh(scope.layout, ph, true);
            const Node *pr = layoutSp ? layoutSp->child(QStringLiteral("spPr")) : nullptr;
            place = placementOf(pr ? pr->child(QStringLiteral("xfrm")) : nullptr, u, GroupSpace());
        }
        if (!place.valid) return;

        const Node *nv = pic->child(QStringLiteral("nvPicPr"));
        const Node *cNvPr = nv ? nv->child(QStringLiteral("cNvPr")) : nullptr;
        const QString spid = cNvPr ? cNvPr->attr(QStringLiteral("id")) : QString();
        const QString name = cNvPr ? cNvPr->attr(QStringLiteral("name")) : QString();
        const QString descr = cNvPr ? cNvPr->attr(QStringLiteral("descr")) : QString();

        // A film or a sound is a picture with a media link beside it.
        const Node *nvPr = nv ? nv->child(QStringLiteral("nvPr")) : nullptr;
        const Node *video = nvPr ? nvPr->child(QStringLiteral("videoFile")) : nullptr;
        const Node *audio = nvPr ? nvPr->child(QStringLiteral("audioFile")) : nullptr;
        if (video || audio) {
            const auto rid = (video ? video : audio)->attr(QStringLiteral("link"));
            const auto mediaPath = pkg.target(scope.partPath, rid);
            const auto external = pkg.external(scope.partPath, rid);
            if (!mediaPath.isEmpty() && pkg.has(mediaPath)) {
                SceneObject media = convertMedia(mediaPath, scope.slideNumber);
                if (!media.id.isEmpty()) {
                    finish(media, place, groups);
                    if (!name.isEmpty()) media.altTitle = name;
                    if (!descr.isEmpty()) media.altText = descr;
                    scope.objects->append(media);
                    if (scope.spidToObject && !spid.isEmpty()) scope.spidToObject->insert(spid, media.id);
                    return;
                }
            } else if (!external.isEmpty()) {
                warnings.add(QStringLiteral("A linked film outside the deck was shown as its poster picture"), scope.slideNumber);
            }
        }

        const Node *blipFill = pic->child(QStringLiteral("blipFill"));
        const Node *blip = blipFill ? blipFill->child(QStringLiteral("blip")) : nullptr;
        if (!blip) return;
        QString rid = blip->attr(QStringLiteral("embed"));
        // A vector original beside the bitmap is the better picture.
        QVector<const Node *> svgs;
        blip->collect(QStringLiteral("svgBlip"), svgs);
        QString mediaPath;
        if (!svgs.isEmpty()) {
            const auto svgPath = pkg.target(scope.partPath, svgs.first()->attr(QStringLiteral("embed")));
            if (pkg.has(svgPath)) mediaPath = svgPath;
        }
        if (mediaPath.isEmpty()) mediaPath = pkg.target(scope.partPath, rid);
        if (mediaPath.isEmpty()) {
            if (!pkg.external(scope.partPath, blip->attr(QStringLiteral("link"))).isEmpty())
                warnings.add(QStringLiteral("A picture linked from outside the deck was left out"), scope.slideNumber);
            return;
        }
        SceneObject o;
        o.id = Edit::newId(QStringLiteral("image"));
        o.type = ObjectType::Image;
        if (!decodePicture(o, mediaPath, scope.slideNumber)) return;
        finish(o, place, groups);
        o.fillStyle = 5;
        o.imageMode = 2; // the frame is the picture, as PowerPoint places it
        if (const Node *src = blipFill->child(QStringLiteral("srcRect"))) {
            const qreal l = src->attr(QStringLiteral("l"), QStringLiteral("0")).toDouble() / 100000.0;
            const qreal t = src->attr(QStringLiteral("t"), QStringLiteral("0")).toDouble() / 100000.0;
            const qreal r = src->attr(QStringLiteral("r"), QStringLiteral("0")).toDouble() / 100000.0;
            const qreal b = src->attr(QStringLiteral("b"), QStringLiteral("0")).toDouble() / 100000.0;
            if (l + r < 1 && t + b < 1 && (l > 0 || t > 0 || r > 0 || b > 0)) o.imageCrop = QRectF(l, t, 1 - l - r, 1 - t - b);
        }
        if (spPr) {
            if (const Node *prst = spPr->child(QStringLiteral("prstGeom"))) {
                const auto p = prst->attr(QStringLiteral("prst"));
                if (p == QLatin1String("ellipse")) o.imageMask = 1;
                else if (p.startsWith(QLatin1String("round"))) o.imageMask = 2;
                else if (p == QLatin1String("hexagon")) o.imageMask = 3;
                else if (p == QLatin1String("heart")) o.imageMask = 4;
            }
            Look look;
            readLine(spPr->child(QStringLiteral("ln")), look, scope.master->color);
            readEffects(spPr->child(QStringLiteral("effectLst")), look, scope.master->color);
            read3d(spPr, look, scope.master->color);   // STYLE3D
            look.fillSet = true; look.fillStyle = 5;
            applyLook(o, look);
            o.fillStyle = 5;
        }
        for (const auto &k : blip->kids) {
            if (k.name == QLatin1String("alphaModFix")) o.opacity = qBound(0.0, k.attr(QStringLiteral("amt"), QStringLiteral("100000")).toDouble() / 100000.0, 1.0);
            else if (k.name == QLatin1String("grayscl")) o.imageSaturation = 0;
            else if (k.name == QLatin1String("lum")) {
                o.imageBrightness = k.attr(QStringLiteral("bright"), QStringLiteral("0")).toDouble() / 100000.0;
                o.imageContrast = 1 + k.attr(QStringLiteral("contrast"), QStringLiteral("0")).toDouble() / 100000.0;
            }
        }
        if (!name.isEmpty()) o.altTitle = name;
        if (!descr.isEmpty()) o.altText = descr;
        o.hidden = cNvPr && cNvPr->attr(QStringLiteral("hidden")) == QLatin1String("1");
        scope.objects->append(o);
        if (scope.spidToObject && !spid.isEmpty()) scope.spidToObject->insert(spid, o.id);
    }

    SceneObject convertMedia(const QString &mediaPath, int slideNumber) {
        SceneObject none;
        if (scratchDir.isEmpty()) return none;
        QTemporaryFile file(scratchDir + QStringLiteral("/omashow-import-XXXXXX.") + QFileInfo(mediaPath).suffix());
        if (!file.open()) return none;
        file.write(pkg.bytes(mediaPath));
        file.flush();
        const auto result = MediaAsset::fromFile(file.fileName(), true, {}, QFileInfo(mediaPath).fileName());
        if (!result.ok()) {
            warnings.add(QStringLiteral("A film could not be embedded: %1").arg(result.error), slideNumber);
            return none;
        }
        SceneObject o = result.object;
        o.id = Edit::newId(QStringLiteral("media"));
        o.type = ObjectType::Media;
        o.mediaName = QFileInfo(mediaPath).fileName();
        return o;
    }

    // ---- tables

    void convertTable(const Node *tbl, const Placement &place, Scope &scope, const QStringList &groups, const QString &spid, const QString &name) {
        const Node *grid = tbl->child(QStringLiteral("tblGrid"));
        const auto rowsNodes = tbl->all(QStringLiteral("tr"));
        const auto colsNodes = grid ? grid->all(QStringLiteral("gridCol")) : QVector<const Node *>();
        const int rows = rowsNodes.size(), columns = colsNodes.size();
        if (rows < 1 || columns < 1 || rows > Table::maxRows || columns > Table::maxColumns) {
            warnings.add(QStringLiteral("A table was too large to bring across"), scope.slideNumber);
            return;
        }
        SceneObject o;
        o.id = Edit::newId(QStringLiteral("table"));
        o.type = ObjectType::Table;
        o.table = Table::create(rows, columns);
        finish(o, place, groups);
        // Sizes are relative: each against the mean, so a wide column stays wide.
        auto relative = [](QVector<qreal> &sizes) {
            qreal total = 0;
            for (qreal s : sizes) total += s;
            const qreal mean = sizes.isEmpty() || total <= 0 ? 1 : total / sizes.size();
            for (auto &s : sizes) s = qBound(0.05, s / mean, 20.0);
        };
        for (int c = 0; c < columns; ++c) o.table.columns[c] = qMax(1.0, colsNodes[c]->attr(QStringLiteral("w")).toDouble());
        for (int r = 0; r < rows; ++r) o.table.rows[r] = qMax(1.0, rowsNodes[r]->attr(QStringLiteral("h")).toDouble());
        relative(o.table.columns);
        relative(o.table.rows);
        const Node *tblPr = tbl->child(QStringLiteral("tblPr"));
        const bool styled = tblPr && tblPr->child(QStringLiteral("tableStyleId"));
        o.fontSize = 18 * u.pt;
        o.verticalAlign = 0;
        o.table.padding = 3.6 * u.pt;
        // Cells are see-through unless they say otherwise; a named table style
        // gives the header its colour and the rows their bands.
        o.fill = QColor(0, 0, 0, 0);
        o.fillStyle = 0;
        o.fillToken.clear(); o.textColorToken = QStringLiteral("foreground"); o.fontToken = QStringLiteral("body");
        o.table.borderWidth = styled ? 0.75 * u.pt : 0;
        bool explicitFills = false;

        TextChain chain;
        chain.levels.append(defaultTextStyle());
        if (scope.master->root)
            if (const Node *styles = scope.master->root->child(QStringLiteral("txStyles")))
                chain.levels.append(styles->child(QStringLiteral("otherStyle")));
        const auto ctx = textContext(scope);
        QSet<int> covered;
        for (int r = 0; r < rows; ++r) {
            const auto cells = rowsNodes[r]->all(QStringLiteral("tc"));
            for (int c = 0; c < qMin(columns, cells.size()); ++c) {
                const Node *tc = cells[c];
                auto &cell = o.table.cells[r * columns + c];
                const int gridSpan = qMax(1, tc->attr(QStringLiteral("gridSpan"), QStringLiteral("1")).toInt());
                const int rowSpan = qMax(1, tc->attr(QStringLiteral("rowSpan"), QStringLiteral("1")).toInt());
                const bool hidden = tc->attr(QStringLiteral("hMerge")) == QLatin1String("1") || tc->attr(QStringLiteral("vMerge")) == QLatin1String("1");
                if (hidden) { cell.rowSpan = 0; cell.columnSpan = 0; continue; }
                cell.rowSpan = qMin(rowSpan, rows - r);
                cell.columnSpan = qMin(gridSpan, columns - c);
                for (int rr = r; rr < r + cell.rowSpan; ++rr)
                    for (int cc = c; cc < c + cell.columnSpan; ++cc)
                        if (rr != r || cc != c) { auto &under = o.table.cells[rr * columns + cc]; under.rowSpan = 0; under.columnSpan = 0; }
                const TextOut text = convertText(tc->child(QStringLiteral("txBody")), chain, ctx);
                cell.text = text.text;
                const auto &b = text.box;
                if (b.sz > 0) cell.style.insert(QStringLiteral("fontSize"), b.sz / 100.0 * u.pt);
                if (b.b > 0) cell.style.insert(QStringLiteral("fontWeight"), 700);
                if (b.i > 0) cell.style.insert(QStringLiteral("italic"), true);
                if (b.u > 0) cell.style.insert(QStringLiteral("underline"), true);
                if (b.color.isValid()) cell.style.insert(QStringLiteral("textColor"), b.color.name(QColor::HexArgb));
                if (!b.family.isEmpty()) cell.style.insert(QStringLiteral("fontFamily"), b.family);
                if (text.para.algn >= 0) cell.style.insert(QStringLiteral("textAlign"), text.para.algn);
                if (const Node *tcPr = tc->child(QStringLiteral("tcPr"))) {
                    Look look;
                    readFill(tcPr, look, scope.master->color);
                    if (look.fillSet) {
                        explicitFills = true;
                        cell.style.insert(QStringLiteral("fill"), look.fillStyle != 5 && look.fill.isValid() ? look.fill.name(QColor::HexArgb) : QStringLiteral("#00000000"));
                    }
                    const auto anchor = tcPr->attr(QStringLiteral("anchor"));
                    if (anchor == QLatin1String("ctr")) cell.style.insert(QStringLiteral("verticalAlign"), 1);
                    else if (anchor == QLatin1String("b")) cell.style.insert(QStringLiteral("verticalAlign"), 2);
                    // One width and colour per cell: the widest of its four edges.
                    bool anyLine = false; qreal widest = 0; QColor lineColor;
                    for (const auto *side : {"lnL", "lnR", "lnT", "lnB"}) {
                        const Node *ln = tcPr->child(QString::fromLatin1(side));
                        if (!ln) continue;
                        anyLine = true;
                        Look edge; readLine(ln, edge, scope.master->color);
                        if (edge.line && edge.lineWidthEmu * u.k > widest) { widest = edge.lineWidthEmu * u.k; lineColor = edge.lineColor; }
                    }
                    if (anyLine) {
                        cell.style.insert(QStringLiteral("borderWidth"), widest > 0 ? qMax(0.5, widest) : 0.0);
                        if (lineColor.isValid()) cell.style.insert(QStringLiteral("borderColor"), lineColor.name(QColor::HexArgb));
                    }
                }
            }
        }
        const bool header = styled && !explicitFills && tblPr->attr(QStringLiteral("firstRow")) == QLatin1String("1");
        o.table.headerRows = header ? 1 : 0;
        o.table.headerColumns = styled && !explicitFills && tblPr->attr(QStringLiteral("firstCol")) == QLatin1String("1") ? 1 : 0;
        o.table.banded = styled && !explicitFills && tblPr->attr(QStringLiteral("bandRow")) == QLatin1String("1");
        if (!name.isEmpty()) o.altTitle = name;
        scope.objects->append(o);
        if (scope.spidToObject && !spid.isEmpty()) scope.spidToObject->insert(spid, o.id);
    }

    // ---- charts

    static QStringList cachePoints(const Node *ref, int *count) {
        // strRef/numRef → strCache/numCache → pt idx / v
        QStringList out;
        if (!ref) return out;
        const Node *cache = nullptr;
        for (const auto &k : ref->kids) {
            if (k.name == QLatin1String("strRef") || k.name == QLatin1String("numRef")) {
                cache = k.child(QStringLiteral("strCache"));
                if (!cache) cache = k.child(QStringLiteral("numCache"));
            } else if (k.name == QLatin1String("strLit") || k.name == QLatin1String("numLit")) cache = &k;
        }
        if (!cache) return out;
        QMap<int, QString> points;
        int maxIdx = -1;
        for (const auto *pt : cache->all(QStringLiteral("pt"))) {
            const int idx = pt->attr(QStringLiteral("idx")).toInt();
            const Node *v = pt->child(QStringLiteral("v"));
            points.insert(idx, v ? v->text : QString());
            maxIdx = qMax(maxIdx, idx);
        }
        if (const Node *n = cache->child(QStringLiteral("ptCount"))) maxIdx = qMax(maxIdx, n->attr(QStringLiteral("val")).toInt() - 1);
        maxIdx = qMin(maxIdx, Table::maxRows * 4);   // a count is a claim, not a budget
        for (int i = 0; i <= maxIdx; ++i) out.append(points.value(i));
        if (count) *count = maxIdx + 1;
        return out;
    }

    void convertChart(const QString &chartPath, const Placement &place, Scope &scope, const QStringList &groups, const QString &spid, const QString &name) {
        const Node *root = pkg.part(chartPath);
        const Node *chart = root ? root->child(QStringLiteral("chart")) : nullptr;
        const Node *plot = chart ? chart->child(QStringLiteral("plotArea")) : nullptr;
        if (!plot) { warnings.add(QStringLiteral("A chart could not be read"), scope.slideNumber); return; }
        const Node *kindNode = nullptr;
        for (const auto &k : plot->kids) if (k.name.endsWith(QLatin1String("Chart"))) { kindNode = &k; break; }
        if (!kindNode) { warnings.add(QStringLiteral("A chart of an unknown kind was left out"), scope.slideNumber); return; }
        const auto kindName = kindNode->name;
        const auto grouping = kindNode->child(QStringLiteral("grouping")) ? kindNode->child(QStringLiteral("grouping"))->attr(QStringLiteral("val")) : QString();
        const bool stacked = grouping.contains(QLatin1String("tacked"));
        const bool horizontal = kindNode->child(QStringLiteral("barDir")) && kindNode->child(QStringLiteral("barDir"))->attr(QStringLiteral("val")) == QLatin1String("bar");
        int kind = -1;
        if (kindName.startsWith(QLatin1String("bar"))) kind = horizontal ? (stacked ? 3 : 1) : (stacked ? 2 : 0);
        else if (kindName.startsWith(QLatin1String("line")) || kindName == QLatin1String("stockChart")) kind = 4;
        else if (kindName.startsWith(QLatin1String("area"))) kind = stacked ? 6 : 5;
        else if (kindName.startsWith(QLatin1String("pie")) || kindName == QLatin1String("ofPieChart")) kind = 7;
        else if (kindName.startsWith(QLatin1String("doughnut"))) kind = 8;
        else if (kindName.startsWith(QLatin1String("scatter")) || kindName.startsWith(QLatin1String("bubble"))) kind = 9;
        else if (kindName.startsWith(QLatin1String("radar")) || kindName.startsWith(QLatin1String("surface"))) {
            kind = 4;
            warnings.add(QStringLiteral("A %1 was drawn as a line chart").arg(kindName), scope.slideNumber);
        } else { warnings.add(QStringLiteral("A %1 was left out").arg(kindName), scope.slideNumber); return; }

        struct Series { QString name; QStringList cats, vals, xs; QColor color; };
        QVector<Series> series;
        QVector<const Node *> serNodes;
        int kinds = 0;
        for (const auto &k : plot->kids) {
            if (!k.name.endsWith(QLatin1String("Chart"))) continue;
            ++kinds;
            for (const auto *ser : k.all(QStringLiteral("ser"))) serNodes.append(ser);
        }
        if (kinds > 1) warnings.add(QStringLiteral("A chart combining kinds was drawn as one kind"), scope.slideNumber);
        bool anyLabels = false;
        for (const auto *ser : serNodes) {
            if (const Node *labels = ser->child(QStringLiteral("dLbls"))) {
                const Node *show = labels->child(QStringLiteral("showVal"));
                if (show && show->attr(QStringLiteral("val")) == QLatin1String("1")) anyLabels = true;
            }
            Series s;
            if (const Node *tx = ser->child(QStringLiteral("tx"))) { const auto names = cachePoints(tx, nullptr); s.name = names.value(0); }
            if (s.name.isEmpty()) s.name = QStringLiteral("Series %1").arg(series.size() + 1);
            s.cats = cachePoints(ser->child(QStringLiteral("cat")), nullptr);
            s.vals = cachePoints(ser->child(QStringLiteral("val")), nullptr);
            if (kind == 9) { s.xs = cachePoints(ser->child(QStringLiteral("xVal")), nullptr); s.vals = cachePoints(ser->child(QStringLiteral("yVal")), nullptr); }
            if (const Node *spPr = ser->child(QStringLiteral("spPr"))) {
                Look look; readFill(spPr, look, scope.master->color);
                if (look.fill.isValid()) s.color = look.fill;
                else if (const Node *ln = spPr->child(QStringLiteral("ln"))) { Look line; readFill(ln, line, scope.master->color); if (line.fill.isValid()) s.color = line.fill; }
            }
            series.append(s);
        }
        if (series.isEmpty()) { warnings.add(QStringLiteral("A chart with no data was left out"), scope.slideNumber); return; }
        int longest = 0;
        for (const auto &s : series) longest = qMax(longest, kind == 9 ? s.xs.size() : s.vals.size());
        if (longest < 1) { warnings.add(QStringLiteral("A chart whose data was not cached in the deck was left out"), scope.slideNumber); return; }
        if (longest > Table::maxRows - 1 || series.size() > Table::maxColumns - 1)
            warnings.add(QStringLiteral("A chart was cut to %1 points and %2 series").arg(Table::maxRows - 1).arg(Table::maxColumns - 1), scope.slideNumber);

        SceneObject o;
        o.id = Edit::newId(QStringLiteral("chart"));
        o.type = ObjectType::Chart;
        o.chart.kind = kind;
        finish(o, place, groups);
        o.fontSize = 14 * u.pt;
        o.fillToken = QStringLiteral("background"); o.textColorToken = QStringLiteral("foreground"); o.fontToken = QStringLiteral("body");

        if (kind == 9) {
            // Rows are the x values every series shares; blanks where one has none.
            QStringList xs;
            bool repeated = false;
            for (const auto &s : series) { QSet<QString> seen; for (const auto &x : s.xs) { if (seen.contains(x)) repeated = true; seen.insert(x); if (!xs.contains(x)) xs.append(x); } }
            if (repeated) warnings.add(QStringLiteral("A scatter chart repeats an x value; only the first point at each x was kept"), scope.slideNumber);
            std::sort(xs.begin(), xs.end(), [](const QString &a, const QString &b) { return a.toDouble() < b.toDouble(); });
            const int rows = qMin(Table::maxRows, xs.size() + 1), columns = qMin(Table::maxColumns, series.size() + 1);
            o.table = Table::create(rows, columns);
            o.table.headerColumns = 1;
            o.table.cells[0].text = QStringLiteral("X");
            for (int c = 1; c < columns; ++c) o.table.cells[c].text = series[c - 1].name;
            for (int r = 1; r < rows; ++r) {
                o.table.cells[r * columns].text = xs[r - 1];
                for (int c = 1; c < columns; ++c) {
                    const auto &s = series[c - 1];
                    const int at = s.xs.indexOf(xs[r - 1]);
                    if (at >= 0) o.table.cells[r * columns + c].text = s.vals.value(at);
                }
            }
        } else {
            QStringList cats = series.first().cats;
            int points = 0;
            for (const auto &s : series) points = qMax(points, s.vals.size());
            while (cats.size() < points) cats.append(QStringLiteral("%1").arg(cats.size() + 1));
            const int rows = qMin(Table::maxRows, points + 1), columns = qMin(Table::maxColumns, series.size() + 1);
            o.table = Table::create(rows, columns);
            o.table.headerColumns = 1;
            o.table.cells[0].text = QStringLiteral("Category");
            for (int c = 1; c < columns; ++c) o.table.cells[c].text = series[c - 1].name;
            for (int r = 1; r < rows; ++r) {
                o.table.cells[r * columns].text = cats.value(r - 1);
                for (int c = 1; c < columns; ++c) o.table.cells[r * columns + c].text = series[c - 1].vals.value(r - 1);
            }
        }
        const int columns = o.table.columns.size();
        for (int c = 1; c < columns; ++c)
            if (series[c - 1].color.isValid()) o.chart.seriesColors.insert(o.table.cells[c].id, series[c - 1].color);

        const Node *title = chart->child(QStringLiteral("title"));
        const Node *deleted = chart->child(QStringLiteral("autoTitleDeleted"));
        o.chart.title = title ? title->words().trimmed() : QString();
        if (o.chart.title.isEmpty() && title && !(deleted && deleted->attr(QStringLiteral("val")) == QLatin1String("1")) && series.size() == 1) o.chart.title = series.first().name;
        o.chart.legend = chart->child(QStringLiteral("legend")) != nullptr;
        o.chart.labels = anyLabels;
        if (const Node *labels = kindNode->child(QStringLiteral("dLbls"))) {
            const Node *show = labels->child(QStringLiteral("showVal"));
            if (show && show->attr(QStringLiteral("val")) == QLatin1String("1")) o.chart.labels = true;
        }
        for (const auto *axis : plot->all(QStringLiteral("catAx")))
            if (const Node *t = axis->child(QStringLiteral("title"))) o.chart.xTitle = t->words().trimmed();
        for (const auto *axis : plot->all(QStringLiteral("valAx"))) {
            if (const Node *t = axis->child(QStringLiteral("title"))) {
                const bool vertical = !axis->child(QStringLiteral("axPos")) || axis->child(QStringLiteral("axPos"))->attr(QStringLiteral("val")) == QLatin1String("l") || axis->child(QStringLiteral("axPos"))->attr(QStringLiteral("val")) == QLatin1String("r");
                (vertical ? o.chart.yTitle : o.chart.xTitle) = t->words().trimmed();
            }
            if (const Node *grid = axis->child(QStringLiteral("majorGridlines"))) { Q_UNUSED(grid); o.chart.grid = true; }
            else o.chart.grid = false;
        }
        if (!name.isEmpty()) o.altTitle = name;
        scope.objects->append(o);
        if (scope.spidToObject && !spid.isEmpty()) scope.spidToObject->insert(spid, o.id);
    }

    void convertFrame(const Node *frame, Scope &scope, const GroupSpace &space, const QStringList &groups) {
        const Placement place = placementOf(frame->child(QStringLiteral("xfrm")), u, space);
        if (!place.valid) return;
        const Node *nv = frame->child(QStringLiteral("nvGraphicFramePr"));
        const Node *cNvPr = nv ? nv->child(QStringLiteral("cNvPr")) : nullptr;
        const QString spid = cNvPr ? cNvPr->attr(QStringLiteral("id")) : QString();
        const QString name = cNvPr ? cNvPr->attr(QStringLiteral("name")) : QString();
        const Node *data = frame->path({"graphic", "graphicData"});
        if (!data) return;
        const auto uri = data->attr(QStringLiteral("uri"));
        if (uri.endsWith(QLatin1String("/table"))) {
            if (const Node *tbl = data->child(QStringLiteral("tbl"))) convertTable(tbl, place, scope, groups, spid, name);
        } else if (uri.endsWith(QLatin1String("/chart"))) {
            const Node *chart = data->child(QStringLiteral("chart"));
            const auto path = chart ? pkg.target(scope.partPath, chart->attr(QStringLiteral("r:id"))) : QString();
            if (path.isEmpty()) warnings.add(QStringLiteral("A chart's data was missing from the deck"), scope.slideNumber);
            else convertChart(path, place, scope, groups, spid, name);
        } else if (uri.contains(QLatin1String("/diagram"))) {
            warnings.add(QStringLiteral("A SmartArt graphic was left out"), scope.slideNumber);
        } else if (uri.contains(QLatin1String("/ole"))) {
            warnings.add(QStringLiteral("An embedded object was left out"), scope.slideNumber);
        } else {
            warnings.add(QStringLiteral("A %1 frame was left out").arg(uri.section(QLatin1Char('/'), -1)), scope.slideNumber);
        }
    }

    void convertConnector(const Node *cxn, Scope &scope, const GroupSpace &space, const QStringList &groups) {
        const Node *spPr = cxn->child(QStringLiteral("spPr"));
        Placement place = placementOf(spPr ? spPr->child(QStringLiteral("xfrm")) : nullptr, u, space);
        if (!place.valid) return;
        Look look;
        readFill(spPr, look, scope.master->color);
        if (spPr) readLine(spPr->child(QStringLiteral("ln")), look, scope.master->color);
        applyStyleRefs(cxn->child(QStringLiteral("style")), look, scope.master->color);
        if (!look.line) return;
        SceneObject o;
        o.id = Edit::newId(QStringLiteral("shape"));
        o.type = ObjectType::Rect;
        if (place.rect.height() < 1) place.rect.setHeight(1);
        if (place.rect.width() < 1) place.rect.setWidth(1);
        finish(o, place, groups);
        look.fillSet = true; look.fillStyle = 5;
        applyLook(o, look);
        o.shapeKind = 13;
        const Node *prst = spPr ? spPr->child(QStringLiteral("prstGeom")) : nullptr;
        const auto kind = prst ? prst->attr(QStringLiteral("prst")) : QString();
        if (!kind.isEmpty() && !kind.startsWith(QLatin1String("straight")) && kind != QLatin1String("line"))
            warnings.add(QStringLiteral("A %1 connector was drawn straight").arg(kind), scope.slideNumber);
        if (place.rect.height() > 2 && place.rect.width() > 2) {
            QPainterPath path;
            if (place.flipV != place.flipH) { path.moveTo(place.rect.bottomLeft()); path.lineTo(place.rect.topRight()); }
            else { path.moveTo(place.rect.topLeft()); path.lineTo(place.rect.bottomRight()); }
            const qreal rot = o.rotation; const auto keep = o.rect;
            Shape::assignPath(o, path); o.rotation = rot; o.rect = keep;
        } else if (place.rect.width() <= 2) {
            // Vertical: turn the horizontal line.
            const QPointF c = place.rect.center();
            const qreal len = place.rect.height();
            o.rect = QRectF(c.x() - len / 2, c.y() - 0.5, len, 1);
            o.rotation += 90;
        }
        scope.objects->append(o);
        const Node *nv = cxn->child(QStringLiteral("nvCxnSpPr"));
        const Node *cNvPr = nv ? nv->child(QStringLiteral("cNvPr")) : nullptr;
        if (scope.spidToObject && cNvPr) scope.spidToObject->insert(cNvPr->attr(QStringLiteral("id")), o.id);
    }

    void convertGroup(const Node *grp, Scope &scope, const GroupSpace &space, const QStringList &groups) {
        const Node *pr = grp->child(QStringLiteral("grpSpPr"));
        const Node *xfrm = pr ? pr->child(QStringLiteral("xfrm")) : nullptr;
        GroupSpace inner = space;
        if (xfrm) {
            const Node *off = xfrm->child(QStringLiteral("off")), *ext = xfrm->child(QStringLiteral("ext"));
            const Node *chOff = xfrm->child(QStringLiteral("chOff")), *chExt = xfrm->child(QStringLiteral("chExt"));
            if (off && ext && chOff && chExt) {
                const qreal cw = chExt->attr(QStringLiteral("cx")).toDouble(), ch = chExt->attr(QStringLiteral("cy")).toDouble();
                const qreal sx = cw > 0 ? ext->attr(QStringLiteral("cx")).toDouble() / cw : 1;
                const qreal sy = ch > 0 ? ext->attr(QStringLiteral("cy")).toDouble() / ch : 1;
                QTransform local;
                local.translate(off->attr(QStringLiteral("x")).toDouble() * u.k, off->attr(QStringLiteral("y")).toDouble() * u.k);
                local.scale(sx, sy);
                local.translate(-chOff->attr(QStringLiteral("x")).toDouble() * u.k, -chOff->attr(QStringLiteral("y")).toDouble() * u.k);
                const qreal rot = xfrm->attr(QStringLiteral("rot"), QStringLiteral("0")).toDouble() / 60000.0;
                if (!qFuzzyIsNull(rot)) {
                    const QRectF frame = xfrmRect(xfrm, u);
                    QTransform spin;
                    spin.translate(frame.center().x(), frame.center().y());
                    spin.rotate(rot);
                    spin.translate(-frame.center().x(), -frame.center().y());
                    local = local * spin;
                    inner.rotation += rot;
                }
                inner.points = local * space.points;
                inner.sx *= sx; inner.sy *= sy;
            }
        }
        QStringList own = groups;
        own.append(Edit::newId(QStringLiteral("group")));
        convertChildren(grp, scope, inner, own);
    }

    void convertChildren(const Node *tree, Scope &scope, const GroupSpace &space, const QStringList &groups) {
        QVector<const Node *> kids;
        flattened(*tree, kids);
        for (const auto *k : kids) {
            if (k->name == QLatin1String("sp")) convertShape(k, scope, space, groups);
            else if (k->name == QLatin1String("pic")) convertPicture(k, scope, space, groups);
            else if (k->name == QLatin1String("grpSp")) convertGroup(k, scope, space, groups);
            else if (k->name == QLatin1String("graphicFrame")) convertFrame(k, scope, space, groups);
            else if (k->name == QLatin1String("cxnSp")) convertConnector(k, scope, space, groups);
            else if (k->name == QLatin1String("contentPart")) warnings.add(QStringLiteral("Ink or another drawing part was left out"), scope.slideNumber);
        }
    }

    // Shapes of a layout or master that are decoration, not placeholders.
    void convertDecoration(const Node *root, Scope &scope) {
        const Node *tree = spTreeOf(root);
        if (!tree) return;
        QVector<const Node *> kids;
        flattened(*tree, kids);
        for (const auto *k : kids) {
            if (k->name == QLatin1String("sp") || k->name == QLatin1String("pic") || k->name == QLatin1String("graphicFrame"))
                if (phOf(k).is) continue;
            if (k->name == QLatin1String("sp")) convertShape(k, scope, GroupSpace(), {});
            else if (k->name == QLatin1String("pic")) convertPicture(k, scope, GroupSpace(), {});
            else if (k->name == QLatin1String("grpSp")) convertGroup(k, scope, GroupSpace(), {});
            else if (k->name == QLatin1String("graphicFrame")) convertFrame(k, scope, GroupSpace(), {});
            else if (k->name == QLatin1String("cxnSp")) convertConnector(k, scope, GroupSpace(), {});
        }
    }

    // ---- backgrounds

    // The background a cSld names, or invalid when it names none. A picture
    // background comes back as its part path.
    QColor backgroundOf(const Node *root, const ColorContext &color, QString *picturePath, const QString &partPath) const {
        const Node *bg = root ? root->path({"cSld", "bg"}) : nullptr;
        if (!bg) return QColor();
        if (const Node *pr = bg->child(QStringLiteral("bgPr"))) {
            Look look;
            readFill(pr, look, color);
            if (!look.blipRid.isEmpty() && picturePath) *picturePath = pkg.target(partPath, look.blipRid);
            // A gradient of its own is drawn, as theme ones are, rather than kept as its first colour.
            if (const Node *grad = pr->child(QStringLiteral("gradFill")); grad && picturePath) {
                const QString key = QStringLiteral("generated/background-%1.png").arg(QString::number(qHash(partPath)));
                if (!generated.contains(key)) {
                    const QImage out = drawGradient(grad, color, QSize(qRound(doc.size.width()), qRound(doc.size.height())));
                    if (!out.isNull()) {
                        QByteArray png;
                        QBuffer buffer(&png);
                        buffer.open(QIODevice::WriteOnly);
                        out.save(&buffer, "PNG");
                        generated.insert(key, png);
                    }
                }
                if (generated.contains(key)) { *picturePath = key; return QColor(); }
            }
            if (look.fillSet && look.fillStyle != 5 && look.fill.isValid()) return look.fill;
            if (!look.blipRid.isEmpty()) return QColor();
            return look.fillSet ? QColor(Qt::white) : QColor();
        }
        if (const Node *ref = bg->child(QStringLiteral("bgRef"))) {
            const auto c = colorIn(ref, color);
            // A theme background style: its picture or gradient becomes a
            // picture, drawn in this background's colour.
            const QString made = themeBackground(ref, color, c);
            if (!made.isEmpty() && picturePath) { *picturePath = made; return QColor(); }
            if (c.isValid()) return c;
        }
        return QColor();
    }

    // A gradFill drawn over a whole slide.
    QImage drawGradient(const Node *style, const ColorContext &ph, const QSize &size) const {
        QGradientStops stops;
        if (const Node *list = style->child(QStringLiteral("gsLst")))
            for (const auto &gs : list->kids) stops.append({gs.attr(QStringLiteral("pos")).toDouble() / 100000.0, colorIn(&gs, ph)});
        if (stops.size() < 2) return QImage();
        std::sort(stops.begin(), stops.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        QImage out(size, QImage::Format_ARGB32);
        out.fill(Qt::transparent);
        QPainter painter(&out);
        if (const Node *path = style->child(QStringLiteral("path"))) {
            // A circle or rectangle from its fillToRect centre outwards.
            const Node *to = path->child(QStringLiteral("fillToRect"));
            const qreal l = to ? to->attr(QStringLiteral("l")).toDouble() / 100000.0 : 0.5, t = to ? to->attr(QStringLiteral("t")).toDouble() / 100000.0 : 0.5;
            const qreal r = to ? to->attr(QStringLiteral("r")).toDouble() / 100000.0 : 0.5, b = to ? to->attr(QStringLiteral("b")).toDouble() / 100000.0 : 0.5;
            const QPointF centre(size.width() * (l + (1 - l - r) / 2), size.height() * (t + (1 - t - b) / 2));
            QRadialGradient g(centre, std::hypot(size.width(), size.height()) / 2);
            g.setStops(stops);
            painter.fillRect(out.rect(), g);
        } else {
            const qreal angle = (style->child(QStringLiteral("lin")) ? style->child(QStringLiteral("lin"))->attr(QStringLiteral("ang")).toDouble() : 0) / 60000.0;
            const qreal rad = qDegreesToRadians(angle);
            const QPointF c(size.width() / 2.0, size.height() / 2.0);
            const qreal half = (std::abs(std::cos(rad)) * size.width() + std::abs(std::sin(rad)) * size.height()) / 2;
            QLinearGradient g(c - QPointF(std::cos(rad), std::sin(rad)) * half, c + QPointF(std::cos(rad), std::sin(rad)) * half);
            g.setStops(stops);
            painter.fillRect(out.rect(), g);
        }
        painter.end();
        return out;
    }

    // Pictures made while reading (theme backgrounds), by the path they are
    // known under; decodePicture takes them like any part.
    mutable QHash<QString, QByteArray> generated;

    // bgRef idx 1001… is the theme's background style 1…; "phClr" in it is
    // the bgRef's own colour. A solid style is a colour (empty here); a
    // picture style is the picture with its colour effects (duotone,
    // greyscale, transparency) applied; a gradient style is drawn. Made once
    // per theme, style and colour.
    QString themeBackground(const Node *ref, const ColorContext &color, const QColor &phColor) const {
        const Theme *theme = color.theme;
        const int idx = ref->attr(QStringLiteral("idx")).toInt();
        if (!theme || !theme->backgroundStyles || idx < 1001) return {};
        QVector<const Node *> styles;
        for (const auto &k : theme->backgroundStyles->kids) styles.append(&k);
        if (idx - 1001 >= styles.size()) return {};
        const Node *style = styles.at(idx - 1001);
        if (style->name != QLatin1String("blipFill") && style->name != QLatin1String("gradFill")) return {};
        const bool photo = style->name == QLatin1String("blipFill");
        const QString key = QStringLiteral("generated/theme-background-%1-%2-%3.%4")
                                .arg(QString::number(qHash(theme->path)), QString::number(idx), phColor.name(QColor::HexArgb).mid(1),
                                     photo ? QStringLiteral("jpg") : QStringLiteral("png"));
        if (generated.contains(key)) return key;
        ColorContext ph = color;
        ph.phColor = phColor;
        const QSize size(qRound(doc.size.width()), qRound(doc.size.height()));
        QImage out;
        if (style->name == QLatin1String("blipFill")) {
            const Node *blip = style->child(QStringLiteral("blip"));
            const QString rid = blip ? blip->attr(QStringLiteral("r:embed")) : QString();
            const QByteArray bytes = rid.isEmpty() ? QByteArray() : pkg.bytes(pkg.target(theme->path, rid));
            QImage picture = QImage::fromData(bytes);
            if (picture.isNull()) return {};
            picture = picture.convertToFormat(QImage::Format_ARGB32);
            for (const auto &effect : blip->kids) {
                if (effect.name == QLatin1String("duotone")) {
                    // Each pixel's brightness picks between the two colours.
                    QVector<QColor> two;
                    for (const auto &c : effect.kids) { const auto col = readColor(&c, ph); if (col.isValid()) two.append(col); }
                    if (two.size() != 2) continue;
                    for (int y = 0; y < picture.height(); ++y) {
                        auto *line = reinterpret_cast<QRgb *>(picture.scanLine(y));
                        for (int x = 0; x < picture.width(); ++x) {
                            const QRgb p = line[x];
                            const qreal t = (0.299 * qRed(p) + 0.587 * qGreen(p) + 0.114 * qBlue(p)) / 255.0;
                            line[x] = qRgba(qRound(two[0].red() + (two[1].red() - two[0].red()) * t),
                                            qRound(two[0].green() + (two[1].green() - two[0].green()) * t),
                                            qRound(two[0].blue() + (two[1].blue() - two[0].blue()) * t), qAlpha(p));
                        }
                    }
                } else if (effect.name == QLatin1String("grayscl")) {
                    for (int y = 0; y < picture.height(); ++y) {
                        auto *line = reinterpret_cast<QRgb *>(picture.scanLine(y));
                        for (int x = 0; x < picture.width(); ++x) { const int g = qGray(line[x]); line[x] = qRgba(g, g, g, qAlpha(line[x])); }
                    }
                } else if (effect.name == QLatin1String("alphaModFix")) {
                    const qreal amt = effect.attr(QStringLiteral("amt"), QStringLiteral("100000")).toDouble() / 100000.0;
                    for (int y = 0; y < picture.height(); ++y) {
                        auto *line = reinterpret_cast<QRgb *>(picture.scanLine(y));
                        for (int x = 0; x < picture.width(); ++x) line[x] = qRgba(qRed(line[x]), qGreen(line[x]), qBlue(line[x]), qRound(qAlpha(line[x]) * amt));
                    }
                }
            }
            // Stretched to the slide (the only mode theme backgrounds use in practice).
            out = picture.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            // Under the picture: the background colour, for any transparent part.
            QImage base(size, QImage::Format_ARGB32);
            base.fill(phColor.isValid() ? phColor : QColor(Qt::white));
            QPainter painter(&base);
            painter.drawImage(0, 0, out);
            painter.end();
            out = base;
        } else {
            out = drawGradient(style, ph, size);
            if (out.isNull()) return {};
        }
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        // A photo stays a photo-sized file; a gradient keeps its smoothness.
        out.save(&buffer, photo ? "JPG" : "PNG", photo ? 92 : -1);
        generated.insert(key, png);
        return key;
    }

    // ---- masters and layouts

    MasterInfo &masterFor(const QString &masterPath) {
        auto found = masters.find(masterPath);
        if (found != masters.end()) return found->second;
        MasterInfo info;
        info.path = masterPath;
        info.root = pkg.part(masterPath);
        const auto themePath = pkg.related(masterPath, QStringLiteral("theme"));
        info.theme = readTheme(pkg, themePath.isEmpty() ? pkg.related(presentationPath, QStringLiteral("theme")) : themePath);
        if (info.root)
            if (const Node *map = info.root->child(QStringLiteral("clrMap")))
                for (auto a = map->attrs.cbegin(); a != map->attrs.cend(); ++a) info.color.clrMap.insert(a.key(), a.value());
        MasterInfo &it = masters.emplace(masterPath, info).first->second;
        it.color.theme = &it.theme;

        Master master;
        master.id = Edit::newId(QStringLiteral("master"));
        const Node *cSld = info.root ? info.root->child(QStringLiteral("cSld")) : nullptr;
        master.name = cSld ? cSld->attr(QStringLiteral("name")) : QString();
        if (master.name.isEmpty()) master.name = QStringLiteral("Master %1").arg(doc.masters.size() + 1);
        QString picture;
        const auto bg = backgroundOf(info.root, it.color, &picture, masterPath);
        master.background = bg.isValid() ? bg : it.theme.colors.value(it.color.clrMap.value(QStringLiteral("bg1"), QStringLiteral("lt1")), QColor(Qt::white));
        master.backgroundToken.clear();
        it.id = master.id;
        Scope scope;
        scope.partPath = masterPath;
        scope.master = &it;
        scope.objects = &master.objects;
        if (!picture.isEmpty()) {
            SceneObject o;
            o.id = Edit::newId(QStringLiteral("image"));
            o.type = ObjectType::Image;
            if (decodePicture(o, picture, 0)) { o.rect = QRectF(QPointF(0, 0), doc.size); o.imageMode = 1; o.fillStyle = 5; master.objects.append(o); }
        }
        convertDecoration(info.root, scope);
        if (presentation && presentation->attr(QStringLiteral("showSpecialPlsOnTitleSld")) == QLatin1String("0")) master.fields.hideOnFirst = true;
        doc.masters.append(master);
        return it;
    }

    QString layoutFor(const QString &layoutPath, MasterInfo &master) {
        auto it = layoutIds.find(layoutPath);
        if (it != layoutIds.end()) return *it;
        const Node *root = pkg.part(layoutPath);
        SlideLayout layout;
        layout.id = Edit::newId(QStringLiteral("layout"));
        layout.masterId = master.id;
        const Node *cSld = root ? root->child(QStringLiteral("cSld")) : nullptr;
        layout.name = cSld ? cSld->attr(QStringLiteral("name")) : QString();
        if (layout.name.isEmpty()) layout.name = QStringLiteral("Layout %1").arg(doc.layouts.size() + 1);
        const Node *tree = spTreeOf(root);
        QVector<const Node *> kids;
        if (tree) flattened(*tree, kids);
        int bodies = 0;
        Scope scope;
        scope.partPath = layoutPath;
        scope.layout = root;
        scope.master = &master;
        for (const auto *sp : kids) {
            if (sp->name != QLatin1String("sp")) continue;
            const Ph ph = phOf(sp);
            if (!ph.is) continue;
            const auto cls = phClass(ph.type);
            if (cls != QLatin1String("title") && cls != QLatin1String("body")) continue;
            const Node *masterSp = findPh(master.root, ph, false);
            const Node *spPr = sp->child(QStringLiteral("spPr"));
            Placement place = placementOf(spPr ? spPr->child(QStringLiteral("xfrm")) : nullptr, u, GroupSpace());
            if (!place.valid && masterSp) {
                const Node *pr = masterSp->child(QStringLiteral("spPr"));
                place = placementOf(pr ? pr->child(QStringLiteral("xfrm")) : nullptr, u, GroupSpace());
            }
            if (!place.valid) continue;
            SceneObject o;
            o.id = cls == QLatin1String("title") ? QStringLiteral("title")
                 : bodies == 0 ? QStringLiteral("body") : QStringLiteral("body-%1").arg(bodies + 1);
            if (cls == QLatin1String("body")) ++bodies;
            bool taken = false;
            for (const auto &p : layout.placeholders) if (p.id == o.id) taken = true;
            if (taken) continue;
            TextChain chain = chainFor(scope, ph, nullptr, masterSp);
            const TextOut text = convertText(sp->child(QStringLiteral("txBody")), chain, textContext(scope));
            RunLook fallback; { ParaLook para; resolveLevel(chain, 0, para, fallback, textContext(scope)); }
            finish(o, place, {});
            applyText(o, text, fallback);
            auto prompt = text.text.trimmed();
            if (prompt.isEmpty() || prompt.startsWith(QLatin1String("Click to edit")) || prompt.startsWith(QLatin1String("Click to add")))
                prompt = cls == QLatin1String("title") ? QStringLiteral("Title") : ph.type == QLatin1String("subTitle") ? QStringLiteral("Subtitle") : QStringLiteral("Body");
            o.text = prompt;
            o.runs.clear();
            o.placeholderId.clear();
            layout.placeholders.append(o);
        }
        doc.layouts.append(layout);
        return *layoutIds.insert(layoutPath, layout.id);
    }

    // ---- transitions and builds

    void convertTransition(const Node *slideRoot, Slide &slide, int number) {
        const Node *transition = nullptr, *newerNode = nullptr;
        QString newer;
        const auto isEffect = [](const Node &k) { return k.name != QLatin1String("sndAc") && k.name != QLatin1String("extLst"); };
        for (const auto &k : slideRoot->kids) {
            if (k.name == QLatin1String("transition")) transition = &k;
            else if (k.name == QLatin1String("AlternateContent")) {
                if (const Node *choice = k.child(QStringLiteral("Choice")))
                    if (const Node *t = choice->child(QStringLiteral("transition"))) {
                        for (const auto &effect : t->kids) if (isEffect(effect)) newer = effect.name;
                        newerNode = t;
                        if (!transition) transition = t;
                    }
                if (const Node *fallback = k.child(QStringLiteral("Fallback")))
                    if (const Node *t = fallback->child(QStringLiteral("transition"))) transition = t;
            }
        }
        if (!transition) { slide.transition = 0; return; }
        QString effect = newer;
        if (effect.isEmpty()) for (const auto &k : transition->kids) if (isEffect(k)) effect = k.name;
        const Node *effectNode = nullptr;
        for (const auto &k : transition->kids) if (k.name == effect) effectNode = &k;
        if (effect.isEmpty() || effect == QLatin1String("cut")) slide.transition = 0;
        else if (effect == QLatin1String("fade"))
            slide.transition = effectNode && (effectNode->attr(QStringLiteral("thruBlk")) == QLatin1String("1") ||
                                              effectNode->attr(QStringLiteral("thruBlk")) == QLatin1String("true")) ? 6 : 1;
        else if (effect == QLatin1String("push") || effect == QLatin1String("cover") || effect == QLatin1String("pull") || effect == QLatin1String("wipe")) {
            slide.transition = effect == QLatin1String("cover") ? 4 : effect == QLatin1String("pull") ? 5 : 2;
            // Diagonal covers ("lu", "rd") travel mostly the way their first letter says.
            const auto dir = effectNode ? effectNode->attr(QStringLiteral("dir"), QStringLiteral("l")).left(1) : QStringLiteral("l");
            slide.transitionDirection = dir == QLatin1String("r") ? 1 : dir == QLatin1String("u") ? 2 : dir == QLatin1String("d") ? 3 : 0;
            if (effect == QLatin1String("wipe")) warnings.add(QStringLiteral("The wipe transition was shown as a push"), number);
        } else if (effect == QLatin1String("zoom") || effect == QLatin1String("warp")) slide.transition = 7;
        else if (effect == QLatin1String("newsflash") || effect == QLatin1String("vortex") || effect == QLatin1String("wheel")) {
            slide.transition = 8;
            if (effect != QLatin1String("newsflash")) warnings.add(QStringLiteral("The %1 transition was shown as a whirl").arg(effect), number);
        } else if (effect == QLatin1String("morph")) slide.transition = 3;
        else { slide.transition = 1; warnings.add(QStringLiteral("The %1 transition was shown as a fade").arg(effect), number); }
        const auto speed = transition->attr(QStringLiteral("spd"), QStringLiteral("fast"));
        slide.transitionSeconds = speed == QLatin1String("slow") ? 1.0 : speed == QLatin1String("med") ? 0.75 : 0.5;
        // The newer markup carries an exact duration the older one cannot.
        for (const auto *t : {transition, newerNode})
            if (t && t->has(QStringLiteral("dur"))) slide.transitionSeconds = qBound(0.05, t->attr(QStringLiteral("dur")).toDouble() / 1000.0, 30.0);
        if (transition->has(QStringLiteral("advTm"))) slide.advanceAfter = transition->attr(QStringLiteral("advTm")).toDouble() / 1000.0;
    }

    // ANIM-INTEROP: a timing subtree as XML again, shape ids replaced by
    // "@{object id}" so the writer can put the new ids back. Empty when a shape
    // it names did not come across (it could not be written back faithfully).
    static QString escapeXml(const QString &s) {
        QString o = s; o.replace(QLatin1Char('&'), QStringLiteral("&amp;")).replace(QLatin1Char('<'), QStringLiteral("&lt;"))
                         .replace(QLatin1Char('>'), QStringLiteral("&gt;")).replace(QLatin1Char('"'), QStringLiteral("&quot;"));
        return o;
    }
    // Sounds on kept effects that were left out (their media is not carried over).
    static int &droppedSounds() { static int n = 0; return n; }
    static bool xmlOf(const Node &n, const QHash<QString, QString> &spidToObject, QString &out) {
        // An effect's sound lives in subTnLst and points at a media part through a
        // relationship that is not carried over: leave the sound out. Any other
        // reference to another part cannot be kept either (written natively).
        if (n.name == QLatin1String("subTnLst")) { ++droppedSounds(); return true; }
        for (auto it = n.attrs.cbegin(); it != n.attrs.cend(); ++it)
            if (it.key().contains(QLatin1Char(':'))) return false;
        static const QSet<QString> drawing{QStringLiteral("srgbClr"), QStringLiteral("schemeClr"), QStringLiteral("prstClr"), QStringLiteral("sysClr"),
            QStringLiteral("hslClr"), QStringLiteral("scrgbClr"), QStringLiteral("lumMod"), QStringLiteral("lumOff"), QStringLiteral("tint"), QStringLiteral("shade"),
            QStringLiteral("alpha"), QStringLiteral("satMod"), QStringLiteral("satOff"), QStringLiteral("hueMod"), QStringLiteral("hueOff"), QStringLiteral("comp"),
            QStringLiteral("inv"), QStringLiteral("gray"), QStringLiteral("alphaMod"), QStringLiteral("alphaOff"), QStringLiteral("gamma"), QStringLiteral("invGamma")};
        const QString tag = (drawing.contains(n.name) ? QStringLiteral("a:") : QStringLiteral("p:")) + n.name;
        out += QLatin1Char('<') + tag;
        QStringList keys = n.attrs.keys(); keys.sort();
        for (const auto &k : keys) {
            QString v = n.attrs.value(k);
            if (k == QLatin1String("spid")) { const auto id = spidToObject.value(v); if (id.isEmpty()) return false; v = QStringLiteral("@{") + id + QLatin1Char('}'); }
            out += QLatin1Char(' ') + k + QStringLiteral("=\"") + escapeXml(v) + QLatin1Char('"');
        }
        if (n.kids.isEmpty() && n.text.isEmpty()) { out += QStringLiteral("/>"); return true; }
        out += QLatin1Char('>') + escapeXml(n.text);
        for (const auto &k : n.kids) if (!xmlOf(k, spidToObject, out)) return false;
        out += QStringLiteral("</") + tag + QLatin1Char('>');
        return true;
    }
    // What PowerPoint calls its effects (presetClass/presetID), for the ones
    // OmaShow plays as an approximation.
    static QString pptName(const QString &cls, int preset) {
        static const QHash<int, const char *> entr{{3, "Blinds"}, {4, "Box"}, {5, "Checkerboard"}, {6, "Shape"}, {7, "Crawl"}, {8, "Diamond"}, {9, "Dissolve"},
            {11, "Flash Once"}, {12, "Peek"}, {13, "Plus"}, {14, "Random Bars"}, {15, "Spiral"}, {16, "Split"}, {17, "Stretch"}, {18, "Strips"}, {19, "Swivel"},
            {20, "Wedge"}, {21, "Wheel"}, {23, "Basic Zoom"}, {24, "Random Effects"}, {25, "Boomerang"}, {26, "Bounce"}, {27, "Color Typewriter"}, {28, "Credits"},
            {29, "Ease In"}, {30, "Float"}, {31, "Grow & Turn"}, {34, "Light Speed"}, {35, "Pinwheel"}, {37, "Rise Up"}, {38, "Swish"}, {39, "Thin Line"},
            {40, "Unfold"}, {41, "Whip"}, {43, "Center Revolve"}, {45, "Fade Swivel"}, {50, "Compress"}, {51, "Basic Swivel"}, {52, "Sling"}, {54, "Stretchy"},
            {55, "Expand"}, {56, "Zip"}, {1, "Appear"}, {2, "Fly In"}, {10, "Fade"}, {22, "Wipe"}, {42, "Float In"}, {47, "Float Down"}, {49, "Spin"}, {53, "Zoom"}};
        static const QHash<int, const char *> emph{{1, "Fill Color"}, {2, "Font Color"}, {3, "Font Size"}, {4, "Font Style"}, {5, "Font"}, {7, "Line Color"},
            {9, "Transparency"}, {10, "Bold Flash"}, {14, "Blast"}, {15, "Bold Reveal"}, {16, "Brush Color"}, {18, "Underline"}, {19, "Color Blend"},
            {20, "Color Pulse"}, {21, "Complementary Color"}, {22, "Complementary Color 2"}, {23, "Contrasting Color"}, {24, "Darken"}, {25, "Desaturate"},
            {27, "Flicker"}, {28, "Grow With Color"}, {30, "Lighten"}, {31, "Style Emphasis"}, {32, "Teeter"}, {33, "Vertical Grow"}, {34, "Wave"}, {35, "Blink"},
            {36, "Shimmer"}, {6, "Grow/Shrink"}, {8, "Spin"}, {26, "Pulse"}};
        const auto &table = cls == QLatin1String("emph") ? emph : entr;
        if (const char *n = table.value(preset)) return QString::fromLatin1(n);
        return QStringLiteral("PowerPoint effect %1").arg(preset);
    }

    void convertBuilds(const Node *slideRoot, Slide &slide, const QHash<QString, QString> &spidToObject, int number) {
        const Node *timing = slideRoot->child(QStringLiteral("timing"));
        if (!timing) return;
        // ANIM-001 / ANIM-INTEROP: walk the main sequence carrying each effect's
        // offset from its click (the sum of the delays above it). Trigger
        // sequences (click a shape) are kept whole for PowerPoint.
        struct Found { const Node *cTn; const Node *par; qreal offset; int group; };
        QVector<Found> found;
        QVector<const Node *> interactive;
        int group = 0;
        std::function<void(const Node &, qreal)> walk = [&](const Node &node, qreal offset) {
            for (const auto &kid : node.kids) {
                if (kid.name != QLatin1String("cTn")) { walk(kid, offset); continue; }
                if (kid.attr(QStringLiteral("nodeType")) == QLatin1String("interactiveSeq")) { interactive.append(&node); continue; }
                qreal own = 0;
                bool click = false;
                if (const Node *st = kid.child(QStringLiteral("stCondLst")))
                    for (const auto *cond : st->all(QStringLiteral("cond"))) {
                        const auto d = cond->attr(QStringLiteral("delay"));
                        bool ok = false;
                        const qreal ms = d.toDouble(&ok);
                        if (d == QLatin1String("indefinite")) click = true;
                        else if (ok && ms > own) own = ms;
                    }
                if (click) ++group;
                const qreal at = (click ? 0.0 : offset) + own;
                if (!kid.attr(QStringLiteral("presetClass")).isEmpty()) { found.append({&kid, &node, at, group}); continue; }
                walk(kid, at);
            }
        };
        walk(*timing, 0.0);
        // The build list: what PowerPoint says about each (shape, group).
        QHash<QString, QString> buildAttrs;
        if (const Node *list = timing->child(QStringLiteral("bldLst")))
            for (const auto &b : list->kids) {
                if (b.name != QLatin1String("bldP")) continue;
                QStringList keys = b.attrs.keys(); keys.sort(); QStringList parts;
                for (const auto &k : keys) if (k != QLatin1String("spid") && k != QLatin1String("grpId")) parts << QStringLiteral("%1=\"%2\"").arg(k, escapeXml(b.attrs.value(k)));
                buildAttrs.insert(b.attr(QStringLiteral("spid")) + QLatin1Char('/') + b.attr(QStringLiteral("grpId")), parts.join(QLatin1Char(' ')));
            }
        const auto buildOf = [&](const QString &spid, const Node *effect) {
            if (!effect->has(QStringLiteral("grpId"))) return QString();
            const auto key = spid + QLatin1Char('/') + effect->attr(QStringLiteral("grpId"));
            return buildAttrs.contains(key) ? buildAttrs.value(key) : QStringLiteral("-"); };

        int diagonals = 0;
        const auto direction = [&](int subtype) {
            if (subtype == 3 || subtype == 6 || subtype == 9 || subtype == 12) ++diagonals;   // corners: OmaShow has four sides
            return subtype & 4 ? 0 : subtype & 1 ? 3 : subtype & 8 ? 1 : subtype & 2 ? 2 : 0; };
        QStringList approximations;
        int lastGroup = -1, dropped = 0;
        qreal runStart = 0, sinceClick = 0;
        const auto textOf = [&](const QString &id) { for (const auto &o : slide.objects) if (o.id == id) return o.text; return QString(); };
        for (const auto &f : found) {
            const Node *cTn = f.cTn;
            const auto cls = cTn->attr(QStringLiteral("presetClass"));
            QVector<const Node *> targets;
            cTn->collect(QStringLiteral("spTgt"), targets);
            if (targets.isEmpty()) continue;
            const auto spid = targets.first()->attr(QStringLiteral("spid"));
            const auto objectId = spidToObject.value(spid);
            if (objectId.isEmpty()) { ++dropped; continue; }
            const Node *range = targets.first()->path({"txEl", "pRg"});
            const Node *iterate = cTn->path({"iterate"});
            if (!iterate) { QVector<const Node *> it; cTn->collect(QStringLiteral("iterate"), it); if (!it.isEmpty()) iterate = it.first(); }
            BuildStep step;
            step.targetId = objectId;
            const int preset = cTn->attr(QStringLiteral("presetID")).toInt();
            const int subtype = cTn->attr(QStringLiteral("presetSubtype")).toInt();
            QVector<const Node *> scales, rots, motions;
            cTn->collect(QStringLiteral("animScale"), scales);
            cTn->collect(QStringLiteral("animRot"), rots);
            cTn->collect(QStringLiteral("animMotion"), motions);
            QVector<const Node *> inner;
            cTn->collect(QStringLiteral("cTn"), inner);
            bool innerReverses = false;
            for (const auto *n : inner) if (n->attr(QStringLiteral("autoRev")) == QLatin1String("1")) innerReverses = true;
            // PowerPoint's own timing options on the effect.
            const auto repeat = cTn->attr(QStringLiteral("repeatCount"));
            if (repeat == QLatin1String("indefinite")) step.repeat = -1;
            else if (!repeat.isEmpty()) step.repeat = qBound(0.0, repeat.toDouble() / 1000.0, 1000.0);
            if (step.repeat == 1.0) step.repeat = 0;
            step.autoReverse = cTn->attr(QStringLiteral("autoRev")) == QLatin1String("1");
            step.rewind = cTn->attr(QStringLiteral("fill")) == QLatin1String("remove");
            QString approx;   // what PowerPoint calls it, when OmaShow plays something close
            if (cls == QLatin1String("path")) {
                step.effect = Effect::Path;
                step.phase = BuildPhase::In;
                if (!motions.isEmpty()) step.motionPath = motions.first()->attr(QStringLiteral("path"));
                if (step.motionPath.isEmpty()) approx = QStringLiteral("Motion path");
            } else if (cls == QLatin1String("entr") || cls == QLatin1String("exit")) {
                step.phase = cls == QLatin1String("entr") ? BuildPhase::In : BuildPhase::Out;
                if (preset == 1) step.effect = Effect::Appear;
                else if (preset == 10) step.effect = Effect::Fade;
                else if (preset == 2) { step.effect = Effect::Fly; step.direction = direction(subtype); }
                else if (preset == 42 || preset == 47) { step.effect = Effect::Float; step.direction = preset == 47 ? 3 : 0; }
                else if (preset == 22) { step.effect = Effect::Wipe; step.direction = direction(subtype); }
                else if (preset == 53) step.effect = Effect::Zoom;
                else if (preset == 49) step.effect = Effect::Turn;
                else {
                    // Nearest OmaShow effect; the original goes back to PowerPoint.
                    approx = pptName(cls, preset);
                    switch (preset) {
                    case 3: case 16: case 18: case 20: case 21: step.effect = Effect::Wipe; step.direction = direction(subtype); break;
                    case 5: step.effect = Effect::Wipe; step.direction = 1; break;
                    case 12: step.effect = Effect::Wipe; step.direction = direction(subtype); break;
                    case 4: case 6: case 8: case 13: case 17: case 23: case 55: step.effect = Effect::Zoom; break;
                    case 7: case 26: step.effect = Effect::Fly; step.direction = preset == 26 ? 3 : 0; break;
                    case 19: case 31: case 43: case 45: case 51: step.effect = Effect::Turn; break;
                    case 30: case 37: step.effect = Effect::Float; break;
                    default: step.effect = Effect::Fade; break;
                    }
                }
            } else if (cls == QLatin1String("emph")) {
                step.phase = BuildPhase::In;
                qreal by = 0;
                if (!scales.isEmpty())
                    if (const Node *b = scales.first()->child(QStringLiteral("by"))) by = b->attr(QStringLiteral("x")).toDouble() / 100000.0;
                if (preset == 6 && by > 0 && innerReverses && !step.autoReverse && by > 1) { step.effect = Effect::Pulse; step.amount = by - 1; }   // OmaShow's older pulse
                else if (preset == 6 && by > 0) { step.effect = by >= 1 ? Effect::Grow : Effect::Shrink; step.amount = by; }
                else if (preset == 8 && !rots.isEmpty()) { step.effect = Effect::Rotate; step.amount = rots.first()->attr(QStringLiteral("by")).toDouble() / 60000.0; }
                else if (preset == 26) { step.effect = Effect::Pulse; if (by > 1) step.amount = by - 1; }
                else { step.effect = Effect::Pulse; step.amount = 0.05; approx = pptName(cls, preset); }
            } else continue;
            // Text: one effect per paragraph is OmaShow's Reveal of that paragraph;
            // by word or letter is a Reveal by word or letter.
            const bool wasEmphasis = Effects::isEmphasis(step.effect);
            if (range && !wasEmphasis && step.effect != Effect::Path) {
                if (step.effect != Effect::Fade && step.effect != Effect::Appear && approx.isEmpty()) approx = pptName(cls, preset);
                if (!approx.isEmpty()) approx += QStringLiteral(" by paragraph");
                const bool instant = step.effect == Effect::Appear;
                step.effect = Effect::Reveal; step.unit = 0;
                step.rangeFrom = range->attr(QStringLiteral("st")).toInt();
                step.rangeTo = range->attr(QStringLiteral("end")).toInt() + 1;
                if (instant) step.duration = 0;
            } else if (range && wasEmphasis && approx.isEmpty()) approx = QStringLiteral("%1 by paragraph").arg(pptName(cls, preset));
            const auto trigger = cTn->attr(QStringLiteral("nodeType"));
            step.trigger = trigger == QLatin1String("withEffect") ? BuildTrigger::WithPrevious
                         : trigger == QLatin1String("afterEffect") ? BuildTrigger::AfterPrevious
                         : BuildTrigger::OnClick;
            // How long PowerPoint plays it: the end of its last part, each part
            // at its own delay, reversed and repeated as it says (ANIM-INTEROP).
            std::function<qreal(const Node &, qreal)> endOf = [&](const Node &n, qreal at) -> qreal {
                qreal best = at;
                for (const auto &k : n.kids) {
                    if (k.name != QLatin1String("cTn")) { best = qMax(best, endOf(k, at)); continue; }
                    qreal d = 0;
                    if (const Node *st = k.child(QStringLiteral("stCondLst")))
                        for (const auto *c : st->all(QStringLiteral("cond"))) { bool ok = false; const qreal v = c->attr(QStringLiteral("delay")).toDouble(&ok); if (ok) d = qMax(d, v); }
                    bool ok = false;
                    const qreal dur = k.attr(QStringLiteral("dur")).toDouble(&ok);
                    bool rok = false;
                    const qreal reps = k.attr(QStringLiteral("repeatCount")).toDouble(&rok) / 1000.0;
                    const qreal len = ok ? dur * (k.attr(QStringLiteral("autoRev")) == QLatin1String("1") ? 2.0 : 1.0) * (rok && reps > 0 ? reps : 1.0) : 0.0;
                    best = qMax(best, qMax(at + d + len, endOf(k, at + d)));
                }
                return best;
            };
            const Node *body = cTn->child(QStringLiteral("childTnLst"));
            const qreal longest = body ? endOf(*body, 0.0) : 0.0;
            if (step.effect != Effect::Reveal) step.easing = Effects::defaultEasing(step.effect);
            else step.easing = QEasingCurve::Linear;
            if (approx.isEmpty() && step.effect != Effect::Reveal && step.effect != Effect::Appear) {
                // Play it on PowerPoint's own curve: Smooth start / end on the effect,
                // else on its first eased part, else linear (PowerPoint's default).
                const Node *eased = cTn;
                if (cTn->attr(QStringLiteral("accel")).isEmpty() && cTn->attr(QStringLiteral("decel")).isEmpty()) {
                    eased = nullptr;
                    std::function<void(const Node &)> look = [&](const Node &n) {
                        for (const auto &k : n.kids) {
                            if (eased) return;
                            if (k.name == QLatin1String("cTn") && (!k.attr(QStringLiteral("accel")).isEmpty() || !k.attr(QStringLiteral("decel")).isEmpty())) { eased = &k; return; }
                            look(k);
                        }
                    };
                    if (const Node *b = cTn->child(QStringLiteral("childTnLst"))) look(*b);
                }
                const qreal a = eased ? eased->attr(QStringLiteral("accel")).toDouble() / 100000.0 : 0.0;
                const qreal d = eased ? eased->attr(QStringLiteral("decel")).toDouble() / 100000.0 : 0.0;
                step.easing = Effects::easingFromPpt(a, d);
                // Amounts other than the four presets are kept exactly.
                const auto is = [](qreal x, qreal y) { return std::abs(x - y) < 1e-6; };
                const bool preset = (is(a, 0) && is(d, 0)) || (is(a, 1) && is(d, 0)) || (is(a, 0) && is(d, 1)) || (is(a, 0.5) && is(d, 0.5));
                if (!preset) { step.accel = a; step.decel = d; }
            }
            if (step.effect == Effect::Appear) step.duration = 0.0;
            else if (step.effect == Effect::Reveal && step.rangeFrom >= 0 && step.duration == 0.0) {}
            else if (longest > 0) step.duration = qBound(0.05, longest / 1000.0, 60.0);
            if (iterate && step.effect != Effect::Path && !wasEmphasis) {
                // By word / by letter: n units, each `d` long, the next after `s`.
                const QString type = iterate->attr(QStringLiteral("type"), QStringLiteral("el"));
                const QString text = textOf(objectId);
                const int unit = type == QLatin1String("lt") ? 2 : 1;
                const int n = unit == 2 ? int(text.size()) : int(text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size());
                const qreal d = step.effect == Effect::Appear ? 0.0 : step.duration;
                qreal s = d;
                if (const Node *abs = iterate->child(QStringLiteral("tmAbs"))) s = abs->attr(QStringLiteral("val")).toDouble() / 1000.0;
                else if (const Node *pct = iterate->child(QStringLiteral("tmPct"))) s = pct->attr(QStringLiteral("val")).toDouble() / 100000.0 * d;
                const bool native = (step.effect == Effect::Fade || step.effect == Effect::Appear) && approx.isEmpty() && (step.effect == Effect::Appear || std::abs(s - d) < 0.002);
                if (!native) approx = (approx.isEmpty() ? pptName(cls, preset) : approx) + (unit == 2 ? QStringLiteral(" by letter") : QStringLiteral(" by word"));
                step.effect = Effect::Reveal; step.unit = unit; step.easing = QEasingCurve::Linear;
                step.duration = qBound(0.05, (n > 0 ? (n - 1) * s : 0.0) + qMax(d, s), 600.0);
            }
            // Timing in PowerPoint's model (Timeline::resolvedSteps): With previous
            // counts from the run's start, After previous from the end of
            // everything since the click.
            if (f.group != lastGroup) { lastGroup = f.group; runStart = sinceClick = 0; }
            const qreal offset = f.offset / 1000.0;
            if (step.trigger == BuildTrigger::OnClick) runStart = sinceClick = 0;
            else if (step.trigger == BuildTrigger::AfterPrevious) runStart = sinceClick;
            step.delay = qMax(0.0, offset - runStart);
            if (step.delay < 0.0005) step.delay = 0;
            step.start = 0;
            sinceClick = qMax(sinceClick, runStart + step.delay + step.span());
            step.pptxName = approx;
            if (!approx.isEmpty()) approximations << approx;
            // Kept exactly as PowerPoint wrote it, for writing back.
            QString raw;
            if (xmlOf(*f.par, spidToObject, raw)) { step.pptxXml = raw; step.pptxSig = step.interopSig(); step.pptxDuration = step.duration; step.pptxBuild = buildOf(spid, cTn); }
            slide.timeline.steps.append(step);
        }
        // A text built paragraph after paragraph, each straight after the last,
        // with the same plain fade or appear, is one Reveal by paragraph.
        auto &steps = slide.timeline.steps;
        for (int i = 0; i < steps.size(); ++i) {
            auto &head = steps[i];
            if (head.effect != Effect::Reveal || head.rangeFrom != 0 || !head.pptxName.isEmpty()) continue;
            int j = i + 1, next = head.rangeTo;
            qreal total = head.duration;
            while (j < steps.size() && steps[j].effect == Effect::Reveal && steps[j].targetId == head.targetId && steps[j].phase == head.phase &&
                   steps[j].rangeFrom == next && steps[j].trigger == BuildTrigger::AfterPrevious && steps[j].delay == 0 && steps[j].pptxName.isEmpty() &&
                   std::abs(steps[j].duration - head.duration) < 0.002 && steps[j].repeat == 0 && !steps[j].rewind && !steps[j].autoReverse) {
                next = steps[j].rangeTo; total += steps[j].duration; ++j;
            }
            const QString text = textOf(head.targetId);
            const int paragraphs = int(text.split(QLatin1Char('\n')).size());
            if (j == i + 1 || next < paragraphs || head.repeat != 0 || head.rewind || head.autoReverse) continue;
            head.rangeFrom = head.rangeTo = -1; head.duration = total;
            head.pptxXml.clear(); head.pptxSig.clear();
            steps.remove(i + 1, j - i - 1);
        }
        // Trigger sequences: kept for PowerPoint, listed so the pane can say so.
        int triggers = 0;
        for (const Node *seq : interactive) {
            QString raw;
            if (!xmlOf(*seq, spidToObject, raw)) { ++dropped; continue; }
            slide.pptxInteractive << raw;
            QVector<const Node *> effects;
            std::function<void(const Node &)> collectEffects = [&](const Node &n) {
                for (const auto &k : n.kids) {
                    if (k.name == QLatin1String("cTn") && !k.attr(QStringLiteral("presetClass")).isEmpty()) {
                        ++triggers;
                        QVector<const Node *> tg; k.collect(QStringLiteral("spTgt"), tg);
                        if (!tg.isEmpty()) {
                            const auto sp = tg.first()->attr(QStringLiteral("spid"));
                            const auto id = spidToObject.value(sp);
                            if (!id.isEmpty() && !slide.pptxInteractiveTargets.contains(id)) slide.pptxInteractiveTargets << id;
                            const auto b = buildOf(sp, &k);
                            if (!id.isEmpty() && !b.isNull()) slide.pptxInteractiveBuilds << QStringLiteral("%1|%2|%3").arg(id, k.attr(QStringLiteral("grpId")), b);
                        }
                    }
                    collectEffects(k);
                }
            };
            collectEffects(*seq);
        }
        if (!approximations.isEmpty()) {
            approximations.removeDuplicates();
            warnings.add(QStringLiteral("PowerPoint effects OmaShow plays as the nearest it has (kept for PowerPoint): %1").arg(approximations.join(QStringLiteral(", "))), number);
        }
        if (droppedSounds()) { warnings.add(QStringLiteral("Sounds on PowerPoint animations were left out"), number); droppedSounds() = 0; }
        if (triggers) warnings.add(QStringLiteral("%1 trigger animation(s) (click a shape to play) kept for PowerPoint; OmaShow does not play triggers").arg(triggers), number);
        if (dropped) warnings.add(QStringLiteral("An animation on a shape OmaShow could not bring across was left out"), number);
        if (diagonals) warnings.add(QStringLiteral("An animation from a corner now comes from the nearest side"), number);
    }

    // ---- slides

    void convertSlide(const QString &slidePath, int number, const QString &sectionId) {
        const Node *root = pkg.part(slidePath);
        if (!root) { warnings.add(QStringLiteral("A slide could not be read"), number); return; }
        const auto layoutPath = pkg.related(slidePath, QStringLiteral("slideLayout"));
        auto masterPath = layoutPath.isEmpty() ? QString() : pkg.related(layoutPath, QStringLiteral("slideMaster"));
        if (masterPath.isEmpty()) masterPath = pkg.relatedAll(presentationPath, QStringLiteral("slideMaster")).value(0);
        MasterInfo &master = masterFor(masterPath);
        const Node *layoutRoot = pkg.part(layoutPath);

        Slide slide;
        slide.id = slideIdByPath.value(slidePath);
        slide.sectionId = sectionId;
        slide.layoutId = layoutPath.isEmpty() ? QString() : layoutFor(layoutPath, master);
        slide.skipped = root->attr(QStringLiteral("show")) == QLatin1String("0");
        slide.showMasterObjects = root->attr(QStringLiteral("showMasterSp")) != QLatin1String("0")
                                  && (!layoutRoot || layoutRoot->attr(QStringLiteral("showMasterSp")) != QLatin1String("0"));

        // Background: the slide's, else the layout's, else the master's.
        QString picture;
        QColor bg = backgroundOf(root, master.color, &picture, slidePath);
        if (!bg.isValid() && picture.isEmpty() && layoutRoot) bg = backgroundOf(layoutRoot, master.color, &picture, layoutPath);
        if (bg.isValid()) { slide.background = bg; slide.backgroundOverride = true; }
        else if (!picture.isEmpty()) {
            slide.background = QColor(Qt::white);
            for (const auto &m : doc.masters) if (m.id == master.id) slide.background = m.background;
            slide.backgroundOverride = true;
        }

        QHash<QString, QString> spidToObject;
        Scope scope;
        scope.partPath = slidePath;
        scope.slide = root;
        scope.layout = layoutRoot;
        scope.master = &master;
        scope.slideNumber = number;
        scope.slideId = slide.id;
        scope.objects = &slide.objects;
        scope.spidToObject = &spidToObject;
        Master *masterRecord = nullptr;
        for (auto &m : doc.masters) if (m.id == master.id) masterRecord = &m;
        MasterFields fields = masterRecord ? masterRecord->fields : MasterFields();
        scope.fields = &fields;

        if (!picture.isEmpty()) {
            SceneObject o;
            o.id = Edit::newId(QStringLiteral("image"));
            o.type = ObjectType::Image;
            if (decodePicture(o, picture, number)) { o.rect = QRectF(QPointF(0, 0), doc.size); o.imageMode = 1; o.fillStyle = 5; o.locked = true; slide.objects.append(o); }
        }
        // The layout's own decoration belongs to every slide that uses it.
        if (layoutRoot && slide.showMasterObjects) {
            Scope decoration = scope;
            decoration.partPath = layoutPath;
            decoration.slide = nullptr;
            decoration.spidToObject = nullptr;
            decoration.fields = nullptr;
            convertDecoration(layoutRoot, decoration);
            for (auto &o : slide.objects) if (o.groups.isEmpty() && !o.locked && o.type != ObjectType::Image) o.locked = true;
        }
        const Node *tree = spTreeOf(root);
        if (tree) convertChildren(tree, scope, GroupSpace(), {});
        if (masterRecord && scope.fieldsSeen) masterRecord->fields = fields;

        // Words for the speaker.
        const auto notesPath = pkg.related(slidePath, QStringLiteral("notesSlide"));
        if (const Node *notes = pkg.part(notesPath)) {
            const Node *notesTree = spTreeOf(notes);
            QVector<const Node *> kids;
            if (notesTree) flattened(*notesTree, kids);
            for (const auto *sp : kids) {
                if (sp->name != QLatin1String("sp")) continue;
                const Ph ph = phOf(sp);
                if (!ph.is || phClass(ph.type) != QLatin1String("body")) continue;
                const Node *body = sp->child(QStringLiteral("txBody"));
                if (body) slide.notes = body->words().trimmed();
            }
        }

        convertTransition(root, slide, number);
        convertBuilds(root, slide, spidToObject, number);
        convertComments(slidePath, slide, number);
        doc.slides.append(slide);
    }

    QHash<QString, QString> authors;
    int newerComments = 0;

    void convertComments(const QString &slidePath, Slide &slide, int number) {
        const auto all = pkg.rels(slidePath);
        for (auto it = all.cbegin(); it != all.cend(); ++it) {
            if (it->external) continue;
            if (it->type.endsWith(QLatin1String("/relationships/comments"))) {
                const Node *root = pkg.part(it->target);
                if (!root) continue;
                for (const auto *cm : root->all(QStringLiteral("cm"))) {
                    Comment comment;
                    comment.id = Edit::newId(QStringLiteral("comment"));
                    comment.slideId = slide.id;
                    comment.author = authors.value(cm->attr(QStringLiteral("authorId")), QStringLiteral("Unknown"));
                    comment.created = cm->attr(QStringLiteral("dt"));
                    const Node *text = cm->child(QStringLiteral("text"));
                    comment.text = text ? text->text : QString();
                    doc.comments.append(comment);
                }
            } else if (it->type.contains(QLatin1String("comments"), Qt::CaseInsensitive) && !it->type.endsWith(QLatin1String("Authors"))) {
                const Node *root = pkg.part(it->target);
                int count = 0;
                if (root) { QVector<const Node *> cms; root->collect(QStringLiteral("cm"), cms); count = cms.size(); }
                if (count) { newerComments += count; warnings.add(QStringLiteral("Comments in PowerPoint's newer format were left out"), number); }
            }
        }
    }

    // ---- the deck

    bool run(QString *error) {
        // The main part, by relationship rather than by the customary path.
        const auto rootRels = pkg.rels(QString());
        for (auto it = rootRels.cbegin(); it != rootRels.cend(); ++it)
            if (it->type.endsWith(QLatin1String("/officeDocument"))) presentationPath = it->target;
        if (presentationPath.isEmpty() && pkg.has(QStringLiteral("ppt/presentation.xml"))) presentationPath = QStringLiteral("ppt/presentation.xml");
        presentation = pkg.part(presentationPath);
        if (!presentation || presentation->name != QLatin1String("presentation")) {
            *error = QStringLiteral("this is not a PowerPoint deck: it has no presentation inside.");
            return false;
        }

        // Size.
        qreal cx = 12192000, cy = 6858000;
        if (const Node *size = presentation->child(QStringLiteral("sldSz"))) {
            cx = size->attr(QStringLiteral("cx")).toDouble();
            cy = size->attr(QStringLiteral("cy")).toDouble();
        }
        if (cx <= 0 || cy <= 0) { cx = 12192000; cy = 6858000; }
        // Any shape is allowed, within what a deck can hold.
        const qreal ratio = qBound(0.125, cy / cx, 8.0);
        doc.size = QSizeF(1920, qRound(1920 * ratio));
        cy = cx * ratio;
        u.k = doc.size.width() / cx;
        u.pt = doc.size.height() / (cy / 12700.0);

        // Slides in order, with stable ids before anything can link to them.
        QStringList slidePaths;
        QHash<QString, QString> slidePathByNumericId;
        if (const Node *list = presentation->child(QStringLiteral("sldIdLst"))) {
            for (const auto *id : list->all(QStringLiteral("sldId"))) {
                const auto path = pkg.target(presentationPath, id->attr(QStringLiteral("r:id")));
                if (path.isEmpty() || !pkg.has(path) || slidePaths.contains(path)) continue;
                slidePaths.append(path);
                slidePathByNumericId.insert(id->attr(QStringLiteral("id")), path);
                slideIdByPath.insert(path, Edit::newId(QStringLiteral("slide")));
            }
        }
        if (slidePaths.isEmpty()) {
            *error = QStringLiteral("the deck has no slides.");
            return false;
        }

        // Theme colours and fonts for the deck's own palette.
        // In the order presentation.xml lists them (sldMasterIdLst): the first is
        // the deck's own theme. The relationships alone come in no fixed order.
        QStringList masterPaths;
        if (const Node *list = presentation ? presentation->child(QStringLiteral("sldMasterIdLst")) : nullptr)
            for (const auto &id : list->kids) {
                const QString target = pkg.target(presentationPath, id.attr(QStringLiteral("r:id")));
                if (!target.isEmpty() && !masterPaths.contains(target)) masterPaths.append(target);
            }
        for (const auto &m : pkg.relatedAll(presentationPath, QStringLiteral("slideMaster")))
            if (!masterPaths.contains(m)) masterPaths.append(m);
        for (const auto &m : masterPaths) masterFor(m);
        if (!masters.empty()) {
            const auto firstPath = masterPaths.value(0, masters.begin()->first);
            const MasterInfo &first = masters.count(firstPath) ? masters.at(firstPath) : masters.begin()->second;
            const auto &t = first.theme;
            auto mapped = [&](const QString &key, const QString &fallback) {
                return t.colors.value(first.color.clrMap.value(key, fallback), t.colors.value(fallback));
            };
            doc.theme.name = t.name.isEmpty() ? QStringLiteral("Imported") : t.name;
            const QColor bg = mapped(QStringLiteral("bg1"), QStringLiteral("lt1")), fg = mapped(QStringLiteral("tx1"), QStringLiteral("dk1"));
            const QColor muted = mapped(QStringLiteral("tx2"), QStringLiteral("dk2")), accent = t.colors.value(QStringLiteral("accent1"));
            if (bg.isValid()) doc.theme.colors[QStringLiteral("background")] = bg;
            if (fg.isValid()) doc.theme.colors[QStringLiteral("foreground")] = fg;
            if (muted.isValid()) doc.theme.colors[QStringLiteral("muted")] = muted;
            if (accent.isValid()) doc.theme.colors[QStringLiteral("accent")] = accent;
            if (!t.major.isEmpty()) doc.theme.fonts[QStringLiteral("heading")] = t.major;
            if (!t.minor.isEmpty()) doc.theme.fonts[QStringLiteral("body")] = t.minor;
        }

        // Authors of comments.
        const auto authorsPath = pkg.related(presentationPath, QStringLiteral("commentAuthors"));
        if (const Node *root = pkg.part(authorsPath))
            for (const auto *a : root->all(QStringLiteral("cmAuthor"))) authors.insert(a->attr(QStringLiteral("id")), a->attr(QStringLiteral("name")));

        // Sections.
        QHash<QString, QString> sectionBySlidePath;
        QVector<const Node *> sectionLists;
        presentation->collect(QStringLiteral("sectionLst"), sectionLists);
        if (!sectionLists.isEmpty()) {
            for (const auto *section : sectionLists.first()->all(QStringLiteral("section"))) {
                Section s;
                s.id = Edit::newId(QStringLiteral("section"));
                s.name = section->attr(QStringLiteral("name"));
                bool used = false;
                if (const Node *ids = section->child(QStringLiteral("sldIdLst")))
                    for (const auto *id : ids->all(QStringLiteral("sldId"))) {
                        const auto path = slidePathByNumericId.value(id->attr(QStringLiteral("id")));
                        if (!path.isEmpty()) { sectionBySlidePath.insert(path, s.id); used = true; }
                    }
                if (used) doc.sections.append(s);
            }
            if (doc.sections.size() == 1) { doc.sections.clear(); sectionBySlidePath.clear(); }
        }

        doc.transition = 0;
        doc.transitionDuration = 0.5;
        int number = 0;
        for (const auto &path : slidePaths) convertSlide(path, ++number, sectionBySlidePath.value(path));
        if (doc.slides.isEmpty()) { *error = QStringLiteral("none of the deck's slides could be read."); return false; }

        // Embedded typefaces are not carried: say which the deck asks for.
        if (const Node *fonts = presentation->child(QStringLiteral("embeddedFontLst"))) {
            QStringList names;
            for (const auto *f : fonts->all(QStringLiteral("embeddedFont")))
                if (const Node *font = f->child(QStringLiteral("font"))) names.append(font->attr(QStringLiteral("typeface")));
            if (!names.isEmpty()) warnings.add(QStringLiteral("Typefaces embedded in the deck are not carried across; it asks for %1").arg(names.join(QStringLiteral(", "))), 0);
        }
        return true;
    }
};

} // namespace

namespace Pptx {

Result read(const QByteArray &raw) {
    Result result;
    Package pkg(raw);
    if (!pkg.zip.isValid()) {
        result.error = QStringLiteral("this is not a PowerPoint deck: %1.").arg(pkg.zip.error());
        return result;
    }
    Reader reader(pkg);
    reader.scratchDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QString error;
    if (!reader.run(&error)) { result.error = error; return result; }
    result.document = reader.doc;
    const auto keep = [](QVector<SceneObject> &objects) {
        for (auto &o : objects) if (o.fitKept == QLatin1String("*")) o.fitKept = TextLayout::keptFitKey(o);
    };
    for (auto &slide : result.document.slides) keep(slide.objects);
    for (auto &master : result.document.masters) keep(master.objects);
    for (auto &layout : result.document.layouts) keep(layout.placeholders);
    result.warnings = reader.warnings.lines();
    result.ok = true;
    return result;
}

Result load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        Result result;
        result.error = QStringLiteral("%1 could not be read.").arg(QFileInfo(path).fileName());
        return result;
    }
    return read(file.readAll());
}

} // namespace Pptx

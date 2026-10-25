#include "io/deckfeatures.h"

#include "core/design.h"
#include "core/scene.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>

#include <algorithm>

namespace {

const QString kFile = QStringLiteral("features.json");
constexpr int kFormat = 1;

// ---- What OmaShow 0.2.0 (da8f319, deck format 25) reads, frozen -----------
// 0.1.0 (74ce940) plus the Office font swap fields (#18); 0.1.0 refuses every
// format 25 deck anyway, so keeping these here loses nothing there and lets
// 0.2.0 keep them.

using Keys = QSet<QString>;

const Keys &objectBase() {
    static const Keys keys{
        "altText", "altTitle", "chart", "columnGap", "columns", "connector", "connectorArrowEnd",
        "connectorArrowStart", "connectorEndX", "connectorEndY", "connectorFrom", "connectorFromSide",
        "connectorRoute", "connectorStartX", "connectorStartY", "connectorTo", "connectorToSide",
        "cornerRadius", "cropH", "cropW", "cropX", "cropY", "dataSource", "direction", "fill", "fillAngle",
        "fillSecondary", "fillStyle", "fillToken", "fontFamily", "fontScale", "fontSize", "fontToken", "fontWeight",
        "groups", "h", "hidden", "id", "imageBrightness", "imageContrast", "imageFocalX", "imageFocalY",
        "imageFormat", "imageId", "imageMask", "imageMode", "imageOriginal", "imageSaturation", "imageTint",
        "imageTintAmount", "italic", "language", "letterSpacing", "lineHeight", "linkKind", "linkTarget",
        "listStart", "listStyle", "locked", "mediaAudio", "mediaBytes", "mediaCodec", "mediaContainer",
        "mediaDuration", "mediaId", "mediaLoops", "mediaModified", "mediaName", "mediaOriginal", "mediaPath",
        "mediaTrimEnd", "mediaTrimStart", "mediaVideo", "mediaVolume", "opacity", "origFont", "origFontSize",
        "origWeight", "overrides",
        "paragraphSpacing", "pathData", "pathWinding", "patternStyle", "placeholderId", "rotation", "runs",
        "shadowColor", "shadowEnabled", "shadowX", "shadowY", "shapeKind", "strokeCap", "strokeColor",
        "strokeJoin", "strokeStyle", "strokeWidth", "table", "tabStop", "text", "textAlign", "textColor",
        "textColorToken", "textFit", "textIndent", "textKind", "textStyleId", "type", "underline",
        "uppercase", "verticalAlign", "w", "x", "y"};
    return keys;
}
const Keys &runBase() {
    static const Keys keys{"start", "length", "weight", "italic", "underline", "strike", "baseline",
                           "fontSize", "fontFamily", "color", "language", "origFont", "origFontSize", "fontScale",
                           "origWeight"};
    return keys;
}
const Keys &cellStyleBase() {
    static const Keys keys{"fill", "textColor", "fontFamily", "fontSize", "fontWeight", "italic", "underline",
                           "textAlign", "verticalAlign", "textFit", "padding", "borderWidth", "borderColor",
                           "origFont", "origFontSize", "fontScale", "origWeight"};
    return keys;
}
const Keys &buildBase() {
    static const Keys keys{"amount", "amountX", "amountY", "delay", "duration", "easing", "effect", "orient",
                           "pathId", "pathReverse", "phase", "start", "target", "trigger", "unit"};
    return keys;
}
const Keys &effectBase() {
    static const Keys keys{"media", "fade", "rise", "move", "scale", "spin", "pulse", "reveal", "path"};
    return keys;
}
const Keys &slideBase() {
    static const Keys keys{"advanceAfter", "background", "backgroundOverride", "builds", "id", "layoutId",
                           "notes", "objects", "readingOrder", "sectionId", "showMasterFields",
                           "showMasterObjects", "skipped", "transition", "transitionDirection",
                           "transitionSeconds"};
    return keys;
}
const Keys &themeBase() { static const Keys keys{"colors", "fontFallbacks", "fontOrigins", "fonts", "name"}; return keys; }
const Keys &manifestBase() {
    static const Keys keys{"height", "id", "knownWords", "language", "name", "sections", "shows", "slides",
                           "smartPunctuation", "transition", "transitionDuration", "version", "width"};
    return keys;
}
const Keys &masterBase() { static const Keys keys{"background", "backgroundToken", "fields", "id", "name", "objects"}; return keys; }
const Keys &layoutBase() { static const Keys keys{"id", "masterId", "name", "placeholders"}; return keys; }
const Keys &styleBase() { static const Keys keys{"appearance", "id", "look", "name"}; return keys; }

// ---- Named features: which left-out fields belong together -----------------

const QString kEffects = QStringLiteral("animation.effects");
const QString kTextGlow = QStringLiteral("text.glow");
const QString kSoftShadow = QStringLiteral("shadow.soft");
const QString kShapeGlow = QStringLiteral("shape.glow");
const QString kShadowDetail = QStringLiteral("shadow.detail");
const QString kBoxShadow = QStringLiteral("textbox.shadow");
const QString kGradient = QStringLiteral("fill.gradient");
const QString kPattern = QStringLiteral("fill.pattern");
const QString kPicture = QStringLiteral("fill.picture");
const QString kLine = QStringLiteral("line.style");
const QString kBoxFill = QStringLiteral("textbox.fill");
const QString kSoftEdge = QStringLiteral("effects.soft-edge");
const QString kReflection = QStringLiteral("effects.reflection");
const QString kFormat3d = QStringLiteral("effects.3d-format");
const QString kRotation3d = QStringLiteral("effects.3d-rotation");
const QString kTextFill = QStringLiteral("text.fill-outline");
const QString kRunEffects = QStringLiteral("text.run-effects");

// What each feature is called, written into every features.json so an app that
// doesn't know a feature can still name it (e.g. to say what a deck uses that
// it can't show). A feature from a newer build keeps the name it came with.
QString readableName(const QString &feature) {
    static const QHash<QString, QString> names{
        {kEffects, QStringLiteral("Animation steps with effects OmaShow 0.2.0 doesn't play")},
        {kTextGlow, QStringLiteral("Glow around text")},
        {kSoftShadow, QStringLiteral("Soft-edged shadows")},
        {kShapeGlow, QStringLiteral("Glow around shapes")},
        {kShadowDetail, QStringLiteral("Inner, resized and slanted (perspective) shadows")},
        {kBoxShadow, QStringLiteral("A text box's own shadow, apart from its words'")},
        {kGradient, QStringLiteral("Gradients with more colours, rectangle and shape gradients, focus and rotation")},
        {kPattern, QStringLiteral("PowerPoint's preset fill patterns")},
        {kPicture, QStringLiteral("Picture fills tiled, inset or see-through")},
        {kLine, QStringLiteral("Line dashes, double and triple lines, gradient lines")},
        {kBoxFill, QStringLiteral("A text box's own fill and outline")},
        {kSoftEdge, QStringLiteral("Soft edges")},
        {kReflection, QStringLiteral("Reflections")},
        {kFormat3d, QStringLiteral("3-D format: bevels, depth, contour and material")},
        {kRotation3d, QStringLiteral("3-D rotation and lighting")},
        {kTextFill, QStringLiteral("Text fill and text outline (gradients, patterns, outlined letters)")},
        {kRunEffects, QStringLiteral("Glow, shadow and reflection on part of a text")},
    };
    if (names.contains(feature)) return names.value(feature);
    if (feature.startsWith(QLatin1String("key:"))) return QStringLiteral("The setting \"%1\"").arg(feature.section(QLatin1Char('.'), -1));
    return feature;
}

QString featureFor(const QString &part, const QString &key) {
    if (part == QLatin1String("object") && key.startsWith(QLatin1String("textGlow"))) return kTextGlow;
    if (part == QLatin1String("object") && key == QLatin1String("shadowBlur")) return kSoftShadow;
    if (part == QLatin1String("object") && key.startsWith(QLatin1String("boxShadow"))) return kBoxShadow;
    if (part == QLatin1String("object") && key.startsWith(QLatin1String("gradient"))) return kGradient;
    if (part == QLatin1String("object") && key == QLatin1String("patternPreset")) return kPattern;
    if (part == QLatin1String("object") && key.startsWith(QLatin1String("picture"))) return kPicture;
    if (part == QLatin1String("object") && (key.startsWith(QLatin1String("strokeDash")) || key == QLatin1String("strokeCompound") ||
                                            key.startsWith(QLatin1String("strokeGradient")) || key == QLatin1String("strokeStops") ||
                                            key == QLatin1String("strokeAngle"))) return kLine;
    if (part == QLatin1String("object") && (key == QLatin1String("boxDrawn") || key.startsWith(QLatin1String("boxInset")))) return kBoxFill;
    if (part == QLatin1String("object") && (key == QLatin1String("glowRadius") || key == QLatin1String("glowColor"))) return kShapeGlow;
    if (part == QLatin1String("object") && (key == QLatin1String("shadowInner") || key.startsWith(QLatin1String("shadowScale")) ||
                                            key.startsWith(QLatin1String("shadowSkew")) || key == QLatin1String("shadowAlign"))) return kShadowDetail;
    if (part == QLatin1String("object") && key == QLatin1String("softEdge")) return kSoftEdge;
    if (part == QLatin1String("object") && (key == QLatin1String("reflection") || key == QLatin1String("textReflection"))) return kReflection;
    if (part == QLatin1String("object") && (key == QLatin1String("format3d") || key == QLatin1String("textFormat3d") ||
                                            key == QLatin1String("textFlatZ") || key == QLatin1String("textFlat"))) return kFormat3d;
    if (part == QLatin1String("object") && (key == QLatin1String("scene3d") || key == QLatin1String("textScene3d") ||
                                            key == QLatin1String("effects3dFollow"))) return kRotation3d;
    if ((part == QLatin1String("object") && (key == QLatin1String("textFill") || key == QLatin1String("textOutline"))) ||
        (part == QLatin1String("run") && (key == QLatin1String("fill") || key == QLatin1String("outline")))) return kTextFill;
    if (part == QLatin1String("run") && key == QLatin1String("effects")) return kRunEffects;
    return QStringLiteral("key:%1.%2").arg(part, key);
}

QString fingerprint(const QJsonValue &value) {
    const QByteArray bytes = value.isObject() ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
                           : value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                             : QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(16));
}

// ---- Paths: object keys, array indices, or {"id": …} in arrays of things with ids

QJsonValue step(const QJsonValue &item, int index) {
    const QString id = item.isObject() ? item.toObject().value(QStringLiteral("id")).toString() : QString();
    return id.isEmpty() ? QJsonValue(index) : QJsonValue(QJsonObject{{QStringLiteral("id"), id}});
}

int indexIn(const QJsonArray &array, const QJsonValue &s) {
    if (s.isDouble()) {
        const double d = s.toDouble();
        const int i = int(d);
        return (d == i && i >= 0 && i < array.size()) ? i : -1;
    }
    if (!s.isObject()) return -1;
    const QString id = s.toObject().value(QStringLiteral("id")).toString();
    if (id.isEmpty()) return -1;
    for (int i = 0; i < array.size(); ++i)
        if (array.at(i).toObject().value(QStringLiteral("id")).toString() == id) return i;
    return -1;
}

bool get(const QJsonValue &root, const QJsonArray &path, int from, QJsonValue *out) {
    if (from == path.size()) { *out = root; return true; }
    const QJsonValue s = path.at(from);
    if (root.isObject() && s.isString()) {
        const auto o = root.toObject();
        if (!o.contains(s.toString())) return false;
        return get(o.value(s.toString()), path, from + 1, out);
    }
    if (root.isArray()) {
        const auto a = root.toArray();
        const int i = indexIn(a, s);
        return i >= 0 && get(a.at(i), path, from + 1, out);
    }
    return false;
}

// Replaces what is at path (which must exist) with value.
QJsonValue put(const QJsonValue &root, const QJsonArray &path, int from, const QJsonValue &value) {
    if (from == path.size()) return value;
    const QJsonValue s = path.at(from);
    if (root.isObject()) {
        auto o = root.toObject();
        o.insert(s.toString(), put(o.value(s.toString()), path, from + 1, value));
        return o;
    }
    auto a = root.toArray();
    const int i = indexIn(a, s);
    a.replace(i, put(a.at(i), path, from + 1, value));
    return a;
}

QJsonArray plus(QJsonArray path, std::initializer_list<QJsonValue> more) {
    for (const auto &v : more) path.append(v);
    return path;
}

// ---- Splitting ---------------------------------------------------------------

struct Split {
    QString file;
    QJsonArray entries;
    void set(const QString &feature, const QJsonArray &path, const QJsonObject &values, const QJsonValue &base) {
        entries.append(QJsonObject{{"feature", feature}, {"file", file}, {"path", path}, {"set", values},
                                   {"base", fingerprint(base)}});
    }
};

// Moves the fields of `o` that aren't in `base` (and values `allowed` refuses)
// out, by feature; `o` keeps only what 0.2.0 reads. Recorded once `o` is final,
// so the fingerprint is of the base element.
using Rule = std::function<bool(const QString &key, const QJsonValue &value, QJsonValue *replacement)>;
// defaults: what this build reads when a field is absent; a field left out at
// that value needs no entry (absent already means it).
QMap<QString, QJsonObject> leaveOut(QJsonObject &o, const Keys &base, const QString &part, const Rule &rule = {},
                                     const QJsonObject *defaults = nullptr) {
    QMap<QString, QJsonObject> moved;
    for (const auto &key : o.keys()) {
        QJsonValue replacement;
        if (!base.contains(key)) {
            if (!(defaults && defaults->contains(key) && defaults->value(key) == o.value(key)))
                moved[featureFor(part, key)].insert(key, o.value(key));
            o.remove(key);
        } else if (rule && !rule(key, o.value(key), &replacement)) {
            moved[featureFor(part, key)].insert(key, o.value(key));
            o.insert(key, replacement);
        }
    }
    return moved;
}
void record(Split &split, const QMap<QString, QJsonObject> &moved, const QJsonArray &path, const QJsonValue &base) {
    for (auto it = moved.cbegin(); it != moved.cend(); ++it) split.set(it.key(), path, it.value(), base);
}

QJsonObject splitObject(QJsonObject o, const QJsonArray &path, Split &split) {
    if (o.value("runs").isArray()) {
        auto runs = o.value("runs").toArray();
        for (int i = 0; i < runs.size(); ++i) {
            auto run = runs.at(i).toObject();
            const auto moved = leaveOut(run, runBase(), QStringLiteral("run"));
            runs.replace(i, run);
            record(split, moved, plus(path, {"runs", i}), run);
        }
        o.insert("runs", runs);
    }
    if (o.value("table").isObject()) {
        auto table = o.value("table").toObject();
        auto cells = table.value("cells").toArray();
        for (int i = 0; i < cells.size(); ++i) {
            auto cell = cells.at(i).toObject();
            if (!cell.value("style").isObject()) continue;
            auto style = cell.value("style").toObject();
            const auto moved = leaveOut(style, cellStyleBase(), QStringLiteral("cell"));
            cell.insert("style", style);
            cells.replace(i, cell);
            record(split, moved, plus(path, {"table", "cells", i, "style"}), style);
        }
        table.insert("cells", cells);
        o.insert("table", table);
    }
    for (const char *nested : {"imageOriginal", "mediaOriginal"}) {
        const auto inner = o.value(QLatin1String(nested)).toObject();
        if (!inner.isEmpty()) o.insert(QLatin1String(nested), splitObject(inner, plus(path, {QLatin1String(nested)}), split));
    }
    static const auto defaults = QJsonObject::fromVariantMap(Design::properties(SceneObject()));
    const auto moved = leaveOut(o, objectBase(), QStringLiteral("object"), {}, &defaults);
    record(split, moved, path, o);
    return o;
}

QJsonArray splitObjects(const QJsonArray &objects, const QJsonArray &path, Split &split) {
    QJsonArray out;
    for (int i = 0; i < objects.size(); ++i)
        out.append(splitObject(objects.at(i).toObject(), plus(path, {step(objects.at(i), i)}), split));
    return out;
}

bool buildIsBase(const QJsonObject &b) {
    if (b.contains("look")) return false;
    if (!effectBase().contains(b.value("effect").toString())) return false;
    const auto in = [&](const char *key, int lo, int hi) {
        const auto v = b.value(QLatin1String(key));
        if (v.isUndefined()) return true;
        const double d = v.toDouble(-1);
        return d == int(d) && d >= lo && d <= hi;
    };
    return in("phase", 0, 1) && in("trigger", 0, 3) && in("unit", 0, 2);
}

QJsonObject splitSlide(QJsonObject slide, Split &split) {
    slide.insert("objects", splitObjects(slide.value("objects").toArray(), {"objects"}, split));
    // Builds 0.2.0 can't play are left out whole and put back at their place;
    // the ones it can keep their extra timing apart.
    const auto builds = slide.value("builds").toArray();
    QJsonArray base, inserts;
    QVector<QPair<int, QMap<QString, QJsonObject>>> extras;
    for (int i = 0; i < builds.size(); ++i) {
        auto b = builds.at(i).toObject();
        if (!buildIsBase(b)) { inserts.append(QJsonObject{{"index", i}, {"step", b}}); continue; }
        const auto moved = leaveOut(b, buildBase(), QStringLiteral("build"));
        if (!moved.isEmpty()) extras.append({int(base.size()), moved});
        base.append(b);
    }
    if (slide.contains("builds")) slide.insert("builds", base);
    for (const auto &[index, moved] : extras) record(split, moved, {"builds", index}, base.at(index));
    if (!inserts.isEmpty())
        split.entries.append(QJsonObject{{"feature", kEffects}, {"file", split.file}, {"path", QJsonArray{"builds"}},
                                         {"insert", inserts}, {"base", fingerprint(base)}});
    const auto moved = leaveOut(slide, slideBase(), QStringLiteral("slide"));
    record(split, moved, {}, slide);
    return slide;
}

QJsonObject splitDesign(QJsonObject design, Split &split) {
    auto masters = design.value("masters").toArray();
    for (int i = 0; i < masters.size(); ++i) {
        auto m = masters.at(i).toObject();
        const QJsonArray path{"masters", step(masters.at(i), i)};
        m.insert("objects", splitObjects(m.value("objects").toArray(), plus(path, {"objects"}), split));
        const auto moved = leaveOut(m, masterBase(), QStringLiteral("master"));
        record(split, moved, path, m);
        masters.replace(i, m);
    }
    auto layouts = design.value("layouts").toArray();
    for (int i = 0; i < layouts.size(); ++i) {
        auto l = layouts.at(i).toObject();
        const QJsonArray path{"layouts", step(layouts.at(i), i)};
        l.insert("placeholders", splitObjects(l.value("placeholders").toArray(), plus(path, {"placeholders"}), split));
        const auto moved = leaveOut(l, layoutBase(), QStringLiteral("layout"));
        record(split, moved, path, l);
        layouts.replace(i, l);
    }
    design.insert("masters", masters);
    design.insert("layouts", layouts);
    return design;
}

QJsonObject splitStyles(QJsonObject styles, Split &split) {
    for (const char *list : {"objects", "text"}) {
        const char *inner = QLatin1String(list) == QLatin1String("objects") ? "appearance" : "look";
        auto items = styles.value(QLatin1String(list)).toArray();
        for (int i = 0; i < items.size(); ++i) {
            auto s = items.at(i).toObject();
            const QJsonArray path{QLatin1String(list), step(items.at(i), i)};
            if (s.value(QLatin1String(inner)).isObject())
                s.insert(QLatin1String(inner), splitObject(s.value(QLatin1String(inner)).toObject(), plus(path, {QLatin1String(inner)}), split));
            const auto moved = leaveOut(s, styleBase(), QStringLiteral("style"));
            record(split, moved, path, s);
            items.replace(i, s);
        }
        if (styles.contains(QLatin1String(list))) styles.insert(QLatin1String(list), items);
    }
    return styles;
}

QJsonObject splitFile(const QString &name, QJsonObject root, Split &split) {
    if (name.startsWith(QLatin1String("slides/"))) return splitSlide(root, split);
    if (name == QLatin1String("masters/design.json")) return splitDesign(root, split);
    if (name == QLatin1String("styles.json")) return splitStyles(root, split);
    const Keys *base = name == QLatin1String("theme.json") ? &themeBase()
                     : name == QLatin1String("document.json") ? &manifestBase() : nullptr;
    if (!base) return root;
    const QString part = name == QLatin1String("theme.json") ? QStringLiteral("theme") : QStringLiteral("document");
    const auto moved = leaveOut(root, *base, part);
    record(split, moved, {}, root);
    return root;
}

bool splits(const QString &name) {
    return name == QLatin1String("document.json") || name == QLatin1String("theme.json") ||
           name == QLatin1String("masters/design.json") || name == QLatin1String("styles.json") ||
           (name.startsWith(QLatin1String("slides/")) && name.endsWith(QLatin1String(".json")));
}

// ---- What this build knows ---------------------------------------------------

const QStringList &namedFeatures() {
    static const QStringList list{kEffects, kTextGlow, kSoftShadow, kShapeGlow, kShadowDetail, kBoxShadow, kGradient, kPattern, kPicture, kLine, kBoxFill, kSoftEdge, kReflection, kFormat3d, kRotation3d, kTextFill, kRunEffects};
    return list;
}

bool knows(const QString &feature) {
    static const QSet<QString> named(namedFeatures().cbegin(), namedFeatures().cend());
    if (named.contains(feature)) return true;
    // A field put aside by name: known when this build's objects have it.
    if (feature.startsWith(QLatin1String("key:object."))) {
        static const auto fields = Design::properties(SceneObject()).keys();
        return fields.contains(feature.mid(11));
    }
    return false;
}

} // namespace

QStringList DeckFeatures::unknownNames(const QByteArray &passthrough) {
    const auto kept = QJsonDocument::fromJson(passthrough).object();
    QStringList out;
    for (const auto &e : kept.value("entries").toArray()) {
        const QString feature = e.toObject().value("feature").toString();
        const QString name = kept.value("names").toObject().value(feature).toString();
        const QString shown = name.isEmpty() ? readableName(feature) : name;
        if (!out.contains(shown)) out.append(shown);
    }
    return out;
}

QStringList DeckFeatures::known() { return namedFeatures(); }

void DeckFeatures::split(QVector<Zip::Entry> &entries, const QByteArray &passthrough) {
    QJsonArray all;
    for (auto &entry : entries) {
        if (!splits(entry.name)) continue;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(entry.data, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) continue;
        Split split{entry.name, {}};
        const auto base = splitFile(entry.name, doc.object(), split);
        // Fields left out at their defaults record no entry but still go.
        if (base == doc.object()) continue;
        entry.data = QJsonDocument(base).toJson(entry.name == QLatin1String("document.json") ? QJsonDocument::Indented
                                                                                               : QJsonDocument::Indented);
        for (const auto &e : std::as_const(split.entries)) all.append(e);
    }
    // Kept from opening: what this build didn't know, as it was, with its names.
    const auto kept = QJsonDocument::fromJson(passthrough).object();
    for (const auto &e : kept.value("entries").toArray()) all.append(e);
    if (all.isEmpty()) return;
    QJsonObject names;
    for (const auto &e : std::as_const(all)) {
        const QString feature = e.toObject().value("feature").toString();
        const QString given = kept.value("names").toObject().value(feature).toString();
        names.insert(feature, knows(feature) || given.isEmpty() ? readableName(feature) : given);
    }
    const QJsonObject file{{"format", kFormat},
                           {"names", names},
                           {"about", "Features this deck uses that OmaShow 0.2.0 doesn't read. Each entry says which "
                                     "feature, which file and element it belongs to, the exact values, and a "
                                     "fingerprint of that element as written; an app applies the ones it knows."},
                           {"entries", all}};
    // After the files it describes, so a reader can stop early.
    entries.append({kFile, QJsonDocument(file).toJson(QJsonDocument::Indented), true});
}

QHash<QString, QByteArray> DeckFeatures::merge(const Zip::Reader &reader, QByteArray *passthrough) {
    QHash<QString, QByteArray> out;
    if (passthrough) passthrough->clear();
    if (!reader.contains(kFile)) return out;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(reader.read(kFile), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return out;
    const auto root = doc.object();
    if (root.value("format").toInt() < 1) return out;

    QJsonArray unknown;
    // file -> its entries this build applies
    QMap<QString, QVector<QJsonObject>> byFile;
    for (const auto &value : root.value("entries").toArray()) {
        const auto e = value.toObject();
        const QString feature = e.value("feature").toString(), file = e.value("file").toString();
        if (feature.isEmpty() || !splits(file) || !reader.contains(file) || !e.value("path").isArray() ||
            !e.value("base").isString() || (!e.value("set").isObject() && !e.value("insert").isArray()))
            continue;   // damaged: ignored
        if (!knows(feature)) { unknown.append(e); continue; }
        byFile[file].append(e);
    }
    if (passthrough && !unknown.isEmpty()) {
        QJsonObject names;
        for (const auto &e : std::as_const(unknown)) {
            const QString feature = e.toObject().value("feature").toString();
            const QString name = root.value("names").toObject().value(feature).toString();
            if (!name.isEmpty()) names.insert(feature, name.left(200));
        }
        *passthrough = QJsonDocument(QJsonObject{{"entries", unknown}, {"names", names}}).toJson(QJsonDocument::Compact);
    }

    for (auto it = byFile.cbegin(); it != byFile.cend(); ++it) {
        const auto parsed = QJsonDocument::fromJson(reader.read(it.key()));
        if (!parsed.isObject()) continue;
        const QJsonValue pristine = parsed.object();
        QJsonValue working = pristine;
        // Every entry is checked against the file as written, before any is applied.
        QVector<QJsonObject> sets, inserts;
        for (const auto &e : it.value()) {
            QJsonValue target;
            if (!get(pristine, e.value("path").toArray(), 0, &target)) continue;
            if (fingerprint(target) != e.value("base").toString()) continue;   // changed since: dropped
            (e.value("set").isObject() ? sets : inserts).append(e);
        }
        if (sets.isEmpty() && inserts.isEmpty()) continue;
        for (const auto &e : std::as_const(sets)) {
            const auto path = e.value("path").toArray();
            QJsonValue target;
            get(working, path, 0, &target);
            if (!target.isObject()) continue;
            auto o = target.toObject();
            const auto values = e.value("set").toObject();
            for (auto v = values.begin(); v != values.end(); ++v) o.insert(v.key(), v.value());
            working = put(working, path, 0, o);
        }
        // Left-out builds back at their places, in order.
        for (const auto &e : std::as_const(inserts)) {
            const auto path = e.value("path").toArray();
            QJsonValue target;
            get(working, path, 0, &target);
            if (!target.isArray()) continue;
            auto list = target.toArray();
            QVector<QPair<int, QJsonValue>> items;
            for (const auto &v : e.value("insert").toArray())
                if (v.toObject().value("step").isObject())
                    items.append({v.toObject().value("index").toInt(), v.toObject().value("step")});
            std::stable_sort(items.begin(), items.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            for (const auto &[index, item] : items) list.insert(qBound(0, index, int(list.size())), item);
            working = put(working, path, 0, list);
        }
        out.insert(it.key(), QJsonDocument(working.toObject()).toJson(QJsonDocument::Compact));
    }
    return out;
}

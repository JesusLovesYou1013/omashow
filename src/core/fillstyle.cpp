#include "core/fillstyle.h"
#include "core/design.h"

#include <QHash>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace {

struct Pattern { const char *name; const char *cells; };
// The cells of every ST_PresetPatternVal name, measured from PowerPoint's own
// PDF export by the office-open-xml-viewer project (MIT; see
// THIRD-PARTY-NOTICES.md): 8 rows of 8, one cell a point at the slide's scale.
const Pattern kPatterns[] = {
    {"pct5", "#...................................#..........................."},
    {"pct10", "#...................#...........#...................#..........."},
    {"pct20", "#...#.............#...#.........#...#.............#...#........."},
    {"pct25", "#...#.....#...#.#...#.....#...#.#...#.....#...#.#...#.....#...#."},
    {"pct30", "#.#.#.#..#...#..#.#.#.#....#...##.#.#.#..#...#..#.#.#.#....#...#"},
    {"pct40", "#.#.#.#..#.#.#.##.#.#.#..#.#...##.#.#.#..#.#.#.##.#.#.#....#.#.#"},
    {"pct50", "#.#.#.#..#.#.#.##.#.#.#..#.#.#.##.#.#.#..#.#.#.##.#.#.#..#.#.#.#"},
    {"pct60", "###.###..#.#.#.##.###.##.#.#.#.####.###..#.#.#.##.###.##.#.#.#.#"},
    {"pct70", ".###.#####.###.#.###.#####.###.#.###.#####.###.#.###.#####.###.#"},
    {"pct75", ".###.#############.###.#########.###.#############.###.#########"},
    {"pct80", "###.###################.###########.###################.########"},
    {"pct90", "############################.###########################.#######"},
    {"horz", "########........................................................"},
    {"vert", "#.......#.......#.......#.......#.......#.......#.......#......."},
    {"ltHorz", "########........................########........................"},
    {"ltVert", "#...#...#...#...#...#...#...#...#...#...#...#...#...#...#...#..."},
    {"dkHorz", "################................################................"},
    {"dkVert", "##..##..##..##..##..##..##..##..##..##..##..##..##..##..##..##.."},
    {"narHorz", "########........########........########........########........"},
    {"narVert", ".#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#.#"},
    {"dashHorz", "####................................####........................"},
    {"dashVert", "#.......#.......#.......#...........#.......#.......#.......#..."},
    {"cross", "#########.......#.......#.......#.......#.......#.......#......."},
    {"dnDiag", "#+.....++#+......+#+......+#+......+#+......+#+......+#++.....+#"},
    {"upDiag", "+.....+#.....+#+....+#+....+#+....+#+....+#+....+#+.....#+.....+"},
    {"ltDnDiag", "#...#....#...#....#...#....#...##...#....#...#....#...#....#...#"},
    {"ltUpDiag", "...#...#..#...#..#...#..#...#......#...#..#...#..#...#..#...#..."},
    {"dkDnDiag", "##..##...##..##...##..###..##..###..##...##..##...##..###..##..#"},
    {"dkUpDiag", "..##..##.##..##.##..##..#..##..#..##..##.##..##.##..##..#..##..#"},
    {"wdDnDiag", "##.....####......###......###......###......###......####.....##"},
    {"wdUpDiag", "#.....##.....###....###....###....###....###....###.....##.....#"},
    {"dashDnDiag", "................#...#....#...#....#...#....#...#................"},
    {"dashUpDiag", "...................#...#..#...#..#...#..#...#..................."},
    {"diagCross", "#+....+#+#+..+#+.+#++#+...+##+....+##+...+#++#+.+#+..+#+#+....+#"},
    {"smCheck", "#..##..#.##..##..##..##.#..##..##..##..#.##..##..##..##.#..##..#"},
    {"lgCheck", "####....####....####....####........####....####....####....####"},
    {"smGrid", "#########...#...#...#...#...#...#########...#...#...#...#...#..."},
    {"lgGrid", "#########.......#.......#.......#.......#.......#.......#......."},
    {"dotGrid", "#.#.#.#.........#...............#...............#..............."},
    {"smConfetti", "#...........#....#............#....#...........#..#..........#.."},
    {"lgConfetti", "#.##...#..##..........##...##.####.##...##..........##..#...##.#"},
    {"horzBrick", "#########.......#.......#.......########....#.......#.......#..."},
    {"diagBrick", ".......#......#......#......#......##.....#..#...#....#.#......#"},
    {"solidDmnd", "...#......###....#####..#######..#####....###......#............"},
    {"openDmnd", "#.....#..#...#....#.#......#......#.#....#...#..#.....#........#"},
    {"dotDmnd", "#.................#...#.............#.............#...#........."},
    {"plaid", "#.#.#.#..#.#.#.##.#.#.#..#.#.#.#####....####....####....####...."},
    {"sphere", ".###.####...#..##...#####...####.###.####..##...#####...#####..."},
    {"weave", "#...#....#.#.#....#...#..#...#.##...#......#.#....#...#..#.#...#"},
    {"divot", "...........#........#......#............#..............##......."},
    {"shingle", "......###....#...#..#.....##........##........#........#.......#"},
    {"wave", "...........##.....#..#.###.................##.....#..#.###......"},
    {"trellis", "########.##..##.#########..##..#########.##..##.#########..##..#"},
    {"zigZag", "#......#.#....#...#..#.....##...#......#.#....#...#..#.....##..."}
};

} // namespace

QVariantList FillStyle::encodeStops(const QVector<GradientStop> &stops) {
    QVariantList out;
    for (const auto &s : stops) {
        QVariantMap m{{"p", s.position}, {"c", s.color.name(QColor::HexArgb)}};
        if (!qFuzzyIsNull(s.brightness)) m.insert("b", s.brightness);
        out.append(m);
    }
    return out;
}

bool FillStyle::decodeStops(const QVariant &value, QVector<GradientStop> *out) {
    if (!value.isValid() || value.isNull()) { out->clear(); return true; }
    if (value.typeId() != QMetaType::QVariantList && !value.canConvert<QVariantList>()) return false;
    QVector<GradientStop> stops;
    for (const auto &item : value.toList()) {
        const auto m = item.toMap();
        bool ok = false;
        const qreal p = m.value("p").toDouble(&ok);
        const QColor c(m.value("c").toString());
        if (!ok || !std::isfinite(p) || !c.isValid()) return false;
        const qreal b = m.value("b", 0).toDouble();
        if (!std::isfinite(b)) return false;
        stops.append({qBound(0.0, p, 1.0), c, qBound(-1.0, b, 1.0)});
        if (stops.size() > 10) return false;   // PowerPoint keeps at most 10
    }
    if (stops.size() == 1) return false;
    *out = stops;
    return true;
}

QColor FillStyle::withBrightness(const QColor &color, qreal b) {
    if (qFuzzyIsNull(b) || !color.isValid()) return color;
    // PowerPoint's Brightness: lighter is lumMod 1-b, lumOff b; darker lumMod 1+b.
    float h, s, l, a;
    color.toHsl().getHslF(&h, &s, &l, &a);
    l = b > 0 ? float(l * (1 - b) + b) : float(l * (1 + b));
    QColor out = QColor::fromHslF(h, s, qBound(0.0f, l, 1.0f), a);
    return out.toRgb();
}

QColor FillStyle::stopColor(const GradientStop &stop) { return withBrightness(stop.color, stop.brightness); }

namespace {
QVector<GradientStop> sorted(QVector<GradientStop> stops) {
    std::stable_sort(stops.begin(), stops.end(), [](const GradientStop &a, const GradientStop &b) { return a.position < b.position; });
    return stops;
}
} // namespace

QVector<GradientStop> FillStyle::fillStops(const SceneObject &o) {
    if (o.gradientStops.size() >= 2) return sorted(o.gradientStops);
    return {{0, o.fill, 0}, {1, o.fillSecondary, 0}};
}

QVector<GradientStop> FillStyle::lineStops(const SceneObject &o) {
    if (o.strokeStops.size() >= 2) return sorted(o.strokeStops);
    return {{0, o.strokeColor, 0}, {1, o.strokeColor, 0}};
}

QColor FillStyle::colorAt(const QVector<GradientStop> &stops, qreal t) {
    if (stops.isEmpty()) return QColor();
    if (t <= stops.first().position) return stopColor(stops.first());
    for (int i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].position) {
            const auto &a = stops[i - 1], &b = stops[i];
            const qreal span = b.position - a.position;
            const qreal u = span > 1e-9 ? (t - a.position) / span : 1;
            const QColor ca = stopColor(a), cb = stopColor(b);
            return QColor::fromRgbF(float(ca.redF() + (cb.redF() - ca.redF()) * u), float(ca.greenF() + (cb.greenF() - ca.greenF()) * u),
                                    float(ca.blueF() + (cb.blueF() - ca.blueF()) * u), float(ca.alphaF() + (cb.alphaF() - ca.alphaF()) * u));
        }
    }
    return stopColor(stops.last());
}

void FillStyle::syncLegacy(SceneObject &o) {
    if (o.gradientStops.size() >= 2 && (o.fillStyle == 1 || o.fillStyle == 2)) {
        const auto s = sorted(o.gradientStops);
        o.fill = stopColor(s.first());
        o.fillSecondary = stopColor(s.last());
    }
    if (!o.patternPreset.isEmpty()) o.patternStyle = nearestQtPattern(o.patternPreset);
    if (!o.strokeDash.isEmpty()) o.strokeStyle = nearestQtDash(o.strokeDash);
    if (o.strokeGradient && o.strokeStops.size() >= 2) o.strokeColor = stopColor(sorted(o.strokeStops).first());
}

const QStringList &FillStyle::patternPresets() {
    static const QStringList names = [] {
        QStringList out;
        for (const auto &p : kPatterns) out.append(QLatin1String(p.name));
        return out;
    }();
    return names;
}

QByteArray FillStyle::patternCells(const QString &preset) {
    for (const auto &p : kPatterns)
        if (preset == QLatin1String(p.name)) return QByteArray(p.cells);
    return {};
}

int FillStyle::nearestQtPattern(const QString &p) {
    // patternStyle: 0-6 Qt's Dense1 (94%) to Dense7 (6%), 7 horizontal,
    // 8 vertical, 9 cross, 10 upward (/) diagonal, 11 downward (\\) diagonal,
    // 12 diagonal cross.
    static const QHash<QString, int> dense{{"pct5", 6}, {"pct10", 5}, {"pct20", 5}, {"pct25", 4}, {"pct30", 4}, {"pct40", 4},
                                           {"pct50", 3}, {"pct60", 2}, {"pct70", 2}, {"pct75", 1}, {"pct80", 1}, {"pct90", 0}};
    if (dense.contains(p)) return dense.value(p);
    if (p.endsWith(QLatin1String("Horz")) || p == QLatin1String("horz")) return 7;
    if (p.endsWith(QLatin1String("Vert")) || p == QLatin1String("vert")) return 8;
    if (p.contains(QLatin1String("UpDiag")) || p == QLatin1String("upDiag") || p == QLatin1String("diagBrick")) return 10;
    if (p.contains(QLatin1String("DnDiag")) || p == QLatin1String("dnDiag")) return 11;
    if (p == QLatin1String("diagCross") || p.endsWith(QLatin1String("Dmnd")) || p == QLatin1String("zigZag") || p == QLatin1String("wave")) return 12;
    if (p == QLatin1String("smConfetti") || p == QLatin1String("lgConfetti") || p == QLatin1String("sphere") ||
        p == QLatin1String("divot") || p == QLatin1String("shingle")) return 3;
    return 9;
}

QString FillStyle::presetForQtPattern(int style) {
    static const char *names[] = {"pct90", "pct75", "pct60", "pct50", "pct30", "pct20", "pct10",
                                  "horz", "vert", "smGrid", "wdUpDiag", "wdDnDiag", "diagCross"};
    return QLatin1String(names[qBound(0, style, 12)]);
}

const QStringList &FillStyle::dashPresets() {
    static const QStringList names{"solid", "sysDot", "sysDash", "dash", "dashDot", "lgDash", "lgDashDot", "lgDashDotDot",
                                   "dot", "sysDashDot", "sysDashDotDot"};
    return names;
}

QVector<qreal> FillStyle::dashPattern(const QString &p) {
    // ECMA-376 20.1.10.48 ST_PresetLineDashVal, in line widths.
    static const QHash<QString, QVector<qreal>> dashes{
        {"dot", {1, 3}}, {"dash", {4, 3}}, {"lgDash", {8, 3}}, {"dashDot", {4, 3, 1, 3}}, {"lgDashDot", {8, 3, 1, 3}},
        {"lgDashDotDot", {8, 3, 1, 3, 1, 3}}, {"sysDash", {3, 1}}, {"sysDot", {1, 1}}, {"sysDashDot", {3, 1, 1, 1}},
        {"sysDashDotDot", {3, 1, 1, 1, 1, 1}}};
    return dashes.value(p);
}

QVector<qreal> FillStyle::dashPattern(const SceneObject &o) {
    if (o.strokeDash == QLatin1String("custom")) {
        QVector<qreal> out;
        for (const auto &v : o.strokeDashCustom) out.append(qMax(0.01, v.toDouble()));
        if (out.size() % 2) out.append(out.last());
        return out;
    }
    if (!o.strokeDash.isEmpty()) return dashPattern(o.strokeDash);
    return {};
}

int FillStyle::nearestQtDash(const QString &p) {
    if (p == QLatin1String("solid") || p.isEmpty()) return 0;
    if (p == QLatin1String("dot") || p == QLatin1String("sysDot")) return 2;
    if (p.endsWith(QLatin1String("DashDotDot"))) return 4;
    if (p.endsWith(QLatin1String("DashDot")) || p == QLatin1String("dashDot")) return 3;
    return 1;
}

QString FillStyle::presetForQtDash(int style) {
    switch (style) {
    case 1: return QStringLiteral("dash");
    case 2: return QStringLiteral("sysDot");
    case 3: return QStringLiteral("dashDot");
    case 4: return QStringLiteral("lgDashDotDot");
    default: return QStringLiteral("solid");
    }
}

const QStringList &FillStyle::compoundNames() {
    static const QStringList names{"sng", "dbl", "thickThin", "thinThick", "tri"};
    return names;
}

bool FillStyle::modernFill(const SceneObject &o) {
    if ((o.fillStyle == 1 || o.fillStyle == 2) && (o.gradientStops.size() >= 2 || o.gradientPath != 0 || !o.gradientRotate || o.gradientScaled ||
        !qFuzzyCompare(o.gradientFocusL, .5) || !qFuzzyCompare(o.gradientFocusT, .5) || !qFuzzyCompare(o.gradientFocusR, .5) || !qFuzzyCompare(o.gradientFocusB, .5)))
        return true;
    if (o.fillStyle == 3 && !o.patternPreset.isEmpty()) return true;
    if (o.fillStyle == 4 && (o.pictureTile || o.pictureTransparency > 0 || !o.pictureRotate ||
        !qFuzzyIsNull(o.pictureInsetL) || !qFuzzyIsNull(o.pictureInsetT) || !qFuzzyIsNull(o.pictureInsetR) || !qFuzzyIsNull(o.pictureInsetB)))
        return true;
    return false;
}

bool FillStyle::modernLine(const SceneObject &o) {
    return o.strokeWidth > 0 && (!o.strokeDash.isEmpty() || o.strokeCompound != 0 || o.strokeGradient);
}

QRectF FillStyle::boxRect(const SceneObject &o) {
    return o.rect.adjusted(-o.boxInsetL, -o.boxInsetT, o.boxInsetR, o.boxInsetB);
}

bool FillStyle::boxVisible(const SceneObject &o) {
    return o.type == ObjectType::Text && o.boxDrawn && (o.fillStyle != 5 || o.strokeWidth > 0);
}

SceneObject FillStyle::boxShape(const SceneObject &o) {
    SceneObject box = o;
    box.type = ObjectType::Rect;
    box.id = o.id + QStringLiteral("-box");
    box.shapeKind = 0;
    box.cornerRadius = 0;
    box.connector = false;
    box.pathData.clear();
    box.rect = boxRect(o);
    box.text.clear();
    box.runs.clear();
    box.placeholderId.clear();
    box.textGlowRadius = 0;
    box.linkKind = 0;
    box.linkTarget.clear();
    // The box's shadow is the text box's own (boxShadow*); its glow is glow*.
    box.swapBoxShadow();
    box.boxShadowEnabled = false;
    box.boxDrawn = false;
    // The words' own effects stay with the words.
    box.textReflection = Effects3D::Reflection();
    box.textFormat3d = Effects3D::Format();
    box.textScene3d = Effects3D::Scene();
    box.textFill.clear();
    box.textOutline.clear();
    return box;
}

QVector<SceneObject> FillStyle::split(const SceneObject &o) {
    if (!boxVisible(o)) return {o};
    SceneObject words = o;
    words.boxDrawn = false;
    words.fillStyle = 5;
    words.strokeWidth = 0;
    words.glowRadius = 0;
    // An outlined box with no fill casts its shadow from its words too.
    if (o.fillStyle != 5) words.boxShadowEnabled = false;
    // The box's soft edges, reflection and 3-D format are the box's.
    words.softEdge = 0;
    words.reflection = Effects3D::Reflection();
    words.format3d = Effects3D::Format();
    return {boxShape(o), words};
}

void FillStyle::copyFillAndLine(SceneObject &to, const SceneObject &from) {
    to.fill = from.fill; to.fillToken = from.fillToken; to.fillStyle = from.fillStyle; to.fillSecondary = from.fillSecondary;
    to.fillAngle = from.fillAngle; to.patternStyle = from.patternStyle; to.patternPreset = from.patternPreset;
    to.gradientStops = from.gradientStops; to.gradientPath = from.gradientPath;
    to.gradientFocusL = from.gradientFocusL; to.gradientFocusT = from.gradientFocusT; to.gradientFocusR = from.gradientFocusR; to.gradientFocusB = from.gradientFocusB;
    to.gradientScaled = from.gradientScaled; to.gradientRotate = from.gradientRotate;
    copyPicture(to, from);
    to.fillStyle = from.fillStyle;
    to.strokeColor = from.strokeColor; to.strokeWidth = from.strokeWidth; to.strokeStyle = from.strokeStyle;
    to.strokeJoin = from.strokeJoin; to.strokeCap = from.strokeCap; to.strokeDash = from.strokeDash; to.strokeDashCustom = from.strokeDashCustom;
    to.strokeCompound = from.strokeCompound; to.strokeGradient = from.strokeGradient; to.strokeStops = from.strokeStops;
    to.strokeAngle = from.strokeAngle; to.strokeGradientPath = from.strokeGradientPath;
}

void FillStyle::copyPicture(SceneObject &to, const SceneObject &from) {
    if (from.fillStyle != 4) return;
    to.fillStyle = 4;
    to.pictureTile = from.pictureTile; to.pictureTileX = from.pictureTileX; to.pictureTileY = from.pictureTileY;
    to.pictureTileScaleX = from.pictureTileScaleX; to.pictureTileScaleY = from.pictureTileScaleY;
    to.pictureTileAlign = from.pictureTileAlign; to.pictureTileFlip = from.pictureTileFlip;
    to.pictureInsetL = from.pictureInsetL; to.pictureInsetT = from.pictureInsetT; to.pictureInsetR = from.pictureInsetR; to.pictureInsetB = from.pictureInsetB;
    to.pictureTransparency = from.pictureTransparency; to.pictureRotate = from.pictureRotate;
        to.imageOriginal = from.imageOriginal; to.image = from.image; to.imageData = from.imageData; to.imageId = from.imageId;
    to.imageFormat = from.imageFormat; to.vectorPicture = from.vectorPicture; to.vectorSize = from.vectorSize;
    to.imageCrop = from.imageCrop; to.imageMode = from.imageMode; to.imageFocalX = from.imageFocalX; to.imageFocalY = from.imageFocalY;
    to.imageBrightness = from.imageBrightness; to.imageContrast = from.imageContrast; to.imageSaturation = from.imageSaturation;
    to.imageTint = from.imageTint; to.imageTintAmount = from.imageTintAmount;
    
}

// ---- Text Fill, Text Outline and a stretch's own text effects

namespace FillStyle {

const QStringList &textFillKeys() {
    static const QStringList keys{"fillStyle", "fill", "fillSecondary", "fillAngle", "patternStyle", "gradientStops",
                                  "gradientPath", "gradientFocusL", "gradientFocusT", "gradientFocusR", "gradientFocusB",
                                  "gradientScaled", "gradientRotate", "patternPreset"};
    return keys;
}
const QStringList &textOutlineKeys() {
    static const QStringList keys{"strokeWidth", "strokeColor", "strokeStyle", "strokeJoin", "strokeCap", "strokeDash",
                                  "strokeDashCustom", "strokeCompound", "strokeGradient", "strokeStops", "strokeAngle",
                                  "strokeGradientPath"};
    return keys;
}
const QStringList &textEffectKeys() {
    static const QStringList keys{"textGlowRadius", "textGlowColor", "shadowEnabled", "shadowColor", "shadowX", "shadowY",
                                  "shadowBlur", "shadowInner", "shadowScaleX", "shadowScaleY", "shadowSkewX", "shadowSkewY",
                                  "shadowAlign", "textReflection"};
    return keys;
}

namespace {
bool checkKeys(const QVariant &value, const QStringList &keys, const QString &extra, QVariantMap *out) {
    if (!value.isValid() || value.isNull()) { out->clear(); return true; }
    if (value.metaType().id() != QMetaType::QVariantMap) return false;
    const QVariantMap in = value.toMap();
    SceneObject carrier;
    QVariantMap tidy;
    for (auto it = in.cbegin(); it != in.cend(); ++it) {
        if (!extra.isEmpty() && it.key() == extra) { tidy.insert(extra, it.value().toBool()); continue; }
        if (!keys.contains(it.key())) return false;
        if (!Design::setProperty(carrier, it.key(), it.value(), false)) return false;
    }
    const QVariantMap all = Design::properties(carrier);
    for (auto it = in.cbegin(); it != in.cend(); ++it)
        if (keys.contains(it.key())) tidy.insert(it.key(), all.value(it.key()));
    *out = tidy;
    return true;
}
} // namespace

// Only what the fill's type uses, so a fill read back from PowerPoint equals
// the one set in the editor (a linear gradient has no focus, and so on).
void pruneTextFill(QVariantMap &m) {
    const int style = m.value(QStringLiteral("fillStyle"), 0).toInt();
    static const QStringList focus{"gradientPath", "gradientFocusL", "gradientFocusT", "gradientFocusR", "gradientFocusB"};
    static const QStringList gradient{"gradientStops", "gradientPath", "gradientFocusL", "gradientFocusT", "gradientFocusR",
                                      "gradientFocusB", "gradientScaled", "gradientRotate", "fillAngle"};
    static const QStringList pattern{"patternStyle", "patternPreset", "fillSecondary"};
    if (style == 5) { m = QVariantMap{{QStringLiteral("fillStyle"), 5}}; return; }
    if (style == 1) for (const auto &k : focus) m.remove(k);
    if (style == 2) { m.remove(QStringLiteral("fillAngle")); m.remove(QStringLiteral("gradientScaled")); }
    if (style == 1 || style == 2) for (const auto &k : pattern) m.remove(k);
    if (style == 3) for (const auto &k : gradient) m.remove(k);
    if (style == 1 || style == 2) m.remove(QStringLiteral("fill"));   // the stops say it
}

bool checkTextFill(const QVariant &value, QVariantMap *out) {
    QVariantMap tidy;
    if (!checkKeys(value, textFillKeys(), QString(), &tidy)) return false;
    // Solid text is textColor (and a run's color): never a fill of its own.
    if (!tidy.isEmpty() && tidy.value("fillStyle", 0).toInt() == 0) tidy.clear();
    if (tidy.value("fillStyle").toInt() == 4) return false;   // picture text fill: not yet
    if (!tidy.isEmpty()) pruneTextFill(tidy);
    *out = tidy;
    return true;
}
// As pruneTextFill, for a line: no gradient keys on a solid line, no custom dashes unless custom.
void pruneTextOutline(QVariantMap &m) {
    if (!m.value(QStringLiteral("strokeGradient")).toBool())
        for (const auto &k : {"strokeGradient", "strokeStops", "strokeAngle", "strokeGradientPath"}) m.remove(QLatin1String(k));
    if (m.value(QStringLiteral("strokeDash")).toString() != QLatin1String("custom")) m.remove(QStringLiteral("strokeDashCustom"));
}
bool checkTextOutline(const QVariant &value, QVariantMap *out) {
    if (!checkKeys(value, textOutlineKeys(), QStringLiteral("none"), out)) return false;
    pruneTextOutline(*out);
    return true;
}
bool checkTextEffects(const QVariant &value, QVariantMap *out) { return checkKeys(value, textEffectKeys(), QString(), out); }

SceneObject textFillCarrier(const QVariantMap &fill, const QColor &color) {
    SceneObject c;
    c.fill = color;
    c.fillStyle = 0;
    c.strokeWidth = 0;
    for (auto it = fill.cbegin(); it != fill.cend(); ++it) Design::setProperty(c, it.key(), it.value(), false);
    return c;
}
SceneObject textOutlineCarrier(const QVariantMap &outline) {
    SceneObject c;
    c.fillStyle = 5;
    c.strokeWidth = 0;
    if (outline.value("none").toBool()) return c;
    for (auto it = outline.cbegin(); it != outline.cend(); ++it)
        if (it.key() != QLatin1String("none")) Design::setProperty(c, it.key(), it.value(), false);
    return c;
}
QVariantMap textFillOf(const SceneObject &carrier) {
    if (carrier.fillStyle == 0) return {};
    const QVariantMap all = Design::properties(carrier);
    QVariantMap m;
    for (const auto &key : textFillKeys()) m.insert(key, all.value(key));
    pruneTextFill(m);
    return m;
}
QVariantMap textOutlineOf(const SceneObject &carrier) {
    if (carrier.strokeWidth <= 0) return {};
    const QVariantMap all = Design::properties(carrier);
    QVariantMap m;
    for (const auto &key : textOutlineKeys()) m.insert(key, all.value(key));
    pruneTextOutline(m);
    return m;
}
SceneObject withTextEffects(const SceneObject &box, const QVariantMap &effects) {
    SceneObject o = box;
    if (effects.isEmpty()) return o;
    // A full set: what it leaves out is off.
    o.textGlowRadius = 0;
    o.shadowEnabled = false;
    o.textReflection = Effects3D::Reflection();
    for (auto it = effects.cbegin(); it != effects.cend(); ++it) Design::setProperty(o, it.key(), it.value(), false);
    return o;
}
QVariantMap textEffectsOf(const SceneObject &o) {
    const QVariantMap all = Design::properties(o);
    QVariantMap m;
    for (const auto &key : textEffectKeys()) m.insert(key, all.value(key));
    return m;
}

} // namespace FillStyle

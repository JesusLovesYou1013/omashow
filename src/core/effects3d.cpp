#include "core/effects3d.h"

#include <QHash>
#include <QtMath>
#include <cmath>

namespace Effects3D {

namespace {
bool same(qreal a, qreal b) { return qAbs(a - b) <= 1e-9 * qMax<qreal>(1, qMax(qAbs(a), qAbs(b))); }

// Reads one number from a map, checked; leaves `out` alone when the key is missing.
bool number(const QVariantMap &m, const char *key, qreal lo, qreal hi, qreal *out) {
    const auto it = m.constFind(QLatin1String(key));
    if (it == m.constEnd()) return true;
    bool ok = false;
    const qreal n = it->toDouble(&ok);
    if (!ok || !std::isfinite(n) || n < lo || n > hi) return false;
    *out = n;
    return true;
}
bool known(const QVariantMap &m, const QStringList &keys) {
    for (auto it = m.cbegin(); it != m.cend(); ++it) if (!keys.contains(it.key())) return false;
    return true;
}
bool colour(const QVariantMap &m, const char *key, QColor *out) {
    const auto it = m.constFind(QLatin1String(key));
    if (it == m.constEnd()) return true;
    const QString s = it->toString();
    if (s.isEmpty()) { *out = QColor(); return true; }
    const QColor c(s);
    if (!c.isValid()) return false;
    *out = c;
    return true;
}
qreal mix(qreal a, qreal b, qreal u) { return a + (b - a) * u; }
qreal mixAngle(qreal a, qreal b, qreal u) {
    qreal d = std::fmod(b - a, 360.0);
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    qreal r = std::fmod(a + d * u, 360.0);
    return r < 0 ? r + 360 : r;
}
QColor mixColour(const QColor &a, const QColor &b, qreal u) {
    if (!a.isValid() || !b.isValid()) return u < .5 ? a : b;
    return QColor::fromRgbF(mix(a.redF(), b.redF(), u), mix(a.greenF(), b.greenF(), u), mix(a.blueF(), b.blueF(), u),
                            mix(a.alphaF(), b.alphaF(), u));
}
} // namespace

bool Reflection::operator==(const Reflection &o) const {
    return on == o.on && same(blur, o.blur) && same(dist, o.dist) && same(startAlpha, o.startAlpha) && same(startPos, o.startPos)
        && same(endAlpha, o.endAlpha) && same(endPos, o.endPos) && same(dir, o.dir) && same(fadeDir, o.fadeDir)
        && same(sx, o.sx) && same(sy, o.sy) && same(kx, o.kx) && same(ky, o.ky) && align == o.align && rotate == o.rotate;
}
bool Bevel::operator==(const Bevel &o) const { return preset == o.preset && same(w, o.w) && same(h, o.h); }
bool Format::operator==(const Format &o) const {
    return on == o.on && top == o.top && bottom == o.bottom && same(depth, o.depth) && same(contour, o.contour) && same(z, o.z)
        && extrusionColor == o.extrusionColor && contourColor == o.contourColor && material == o.material;
}
bool Scene::operator==(const Scene &o) const {
    return on == o.on && camera == o.camera && same(fov, o.fov) && same(zoom, o.zoom) && rotated == o.rotated
        && same(lat, o.lat) && same(lon, o.lon) && same(rev, o.rev) && rig == o.rig && rigDir == o.rigDir
        && rigRotated == o.rigRotated && same(rigLat, o.rigLat) && same(rigLon, o.rigLon) && same(rigRev, o.rigRev);
}

// ---- maps

QVariantMap toMap(const Reflection &r) {
    if (!r.on) return {};
    return {{"blur", r.blur}, {"dist", r.dist}, {"startAlpha", r.startAlpha}, {"startPos", r.startPos},
            {"endAlpha", r.endAlpha}, {"endPos", r.endPos}, {"dir", r.dir}, {"fadeDir", r.fadeDir},
            {"sx", r.sx}, {"sy", r.sy}, {"kx", r.kx}, {"ky", r.ky}, {"align", r.align}, {"rotate", r.rotate}};
}

bool fromMap(const QVariant &v, Reflection *out) {
    if (!v.isValid() || v.isNull()) { *out = Reflection(); return true; }
    if (v.metaType().id() != QMetaType::QVariantMap) return false;
    const QVariantMap m = v.toMap();
    Reflection r;
    if (m.isEmpty()) { *out = r; return true; }
    static const QStringList keys{"blur", "dist", "startAlpha", "startPos", "endAlpha", "endPos", "dir", "fadeDir",
                                  "sx", "sy", "kx", "ky", "align", "rotate"};
    if (!known(m, keys)) return false;
    r.on = true;
    qreal align = r.align;
    if (!number(m, "blur", 0, 1e6, &r.blur) || !number(m, "dist", 0, 1e6, &r.dist)
        || !number(m, "startAlpha", 0, 100, &r.startAlpha) || !number(m, "startPos", 0, 100, &r.startPos)
        || !number(m, "endAlpha", 0, 100, &r.endAlpha) || !number(m, "endPos", 0, 100, &r.endPos)
        || !number(m, "dir", 0, 360, &r.dir) || !number(m, "fadeDir", 0, 360, &r.fadeDir)
        || !number(m, "sx", -10000, 10000, &r.sx) || !number(m, "sy", -10000, 10000, &r.sy)
        || !number(m, "kx", -90, 90, &r.kx) || !number(m, "ky", -90, 90, &r.ky)
        || !number(m, "align", 0, 8, &align) || align != int(align))
        return false;
    r.align = int(align);
    if (m.contains("rotate")) r.rotate = m.value("rotate").toBool();
    *out = r;
    return true;
}

namespace {
QVariantMap bevelMap(const Bevel &b) {
    if (!b.on()) return {};
    return {{"preset", b.preset}, {"w", b.w}, {"h", b.h}};
}
bool bevelFrom(const QVariant &v, Bevel *out) {
    Bevel b;
    if (!v.isValid() || v.isNull()) { *out = b; return true; }
    if (v.metaType().id() != QMetaType::QVariantMap) return false;
    const QVariantMap m = v.toMap();
    if (m.isEmpty()) { *out = b; return true; }
    if (!known(m, {"preset", "w", "h"})) return false;
    b.preset = m.value("preset", QStringLiteral("circle")).toString();
    if (!bevelPresets().contains(b.preset)) return false;
    if (!number(m, "w", 0, 1e6, &b.w) || !number(m, "h", 0, 1e6, &b.h)) return false;
    *out = b;
    return true;
}
} // namespace

QVariantMap toMap(const Format &f) {
    if (!f.on) return {};
    QVariantMap m{{"depth", f.depth}, {"contour", f.contour}, {"z", f.z}, {"material", f.material},
                  {"extrusionColor", f.extrusionColor.isValid() ? f.extrusionColor.name(QColor::HexArgb) : QString()},
                  {"contourColor", f.contourColor.isValid() ? f.contourColor.name(QColor::HexArgb) : QString()}};
    if (f.top.on()) m.insert("top", bevelMap(f.top));
    if (f.bottom.on()) m.insert("bottom", bevelMap(f.bottom));
    return m;
}

bool fromMap(const QVariant &v, Format *out) {
    if (!v.isValid() || v.isNull()) { *out = Format(); return true; }
    if (v.metaType().id() != QMetaType::QVariantMap) return false;
    const QVariantMap m = v.toMap();
    Format f;
    if (m.isEmpty()) { *out = f; return true; }
    if (!known(m, {"top", "bottom", "depth", "contour", "z", "material", "extrusionColor", "contourColor"})) return false;
    f.on = true;
    if (!bevelFrom(m.value("top"), &f.top) || !bevelFrom(m.value("bottom"), &f.bottom)) return false;
    if (!number(m, "depth", 0, 1e6, &f.depth) || !number(m, "contour", 0, 1e6, &f.contour) || !number(m, "z", -1e6, 1e6, &f.z))
        return false;
    if (!colour(m, "extrusionColor", &f.extrusionColor) || !colour(m, "contourColor", &f.contourColor)) return false;
    f.material = m.value("material").toString();
    if (!f.material.isEmpty() && !materialPresets().contains(f.material)) return false;
    *out = f;
    return true;
}

QVariantMap toMap(const Scene &s) {
    if (!s.on) return {};
    QVariantMap m{{"camera", s.camera}, {"fov", s.fov}, {"zoom", s.zoom}, {"rig", s.rig}, {"rigDir", s.rigDir}};
    if (s.rotated) { m.insert("lat", s.lat); m.insert("lon", s.lon); m.insert("rev", s.rev); }
    if (s.rigRotated) { m.insert("rigLat", s.rigLat); m.insert("rigLon", s.rigLon); m.insert("rigRev", s.rigRev); }
    return m;
}

bool fromMap(const QVariant &v, Scene *out) {
    if (!v.isValid() || v.isNull()) { *out = Scene(); return true; }
    if (v.metaType().id() != QMetaType::QVariantMap) return false;
    const QVariantMap m = v.toMap();
    Scene s;
    if (m.isEmpty()) { *out = s; return true; }
    if (!known(m, {"camera", "fov", "zoom", "lat", "lon", "rev", "rig", "rigDir", "rigLat", "rigLon", "rigRev"})) return false;
    s.on = true;
    s.camera = m.value("camera", s.camera).toString();
    if (!cameraPresets().contains(s.camera)) return false;
    s.rig = m.value("rig", s.rig).toString();
    if (!lightRigs().contains(s.rig)) return false;
    s.rigDir = m.value("rigDir", s.rigDir).toString();
    if (!rigDirections().contains(s.rigDir)) return false;
    if (!number(m, "fov", -1, 180, &s.fov) || !number(m, "zoom", 0, 100000, &s.zoom)) return false;
    s.rotated = m.contains("lat") || m.contains("lon") || m.contains("rev");
    s.rigRotated = m.contains("rigLat") || m.contains("rigLon") || m.contains("rigRev");
    if (!number(m, "lat", 0, 360, &s.lat) || !number(m, "lon", 0, 360, &s.lon) || !number(m, "rev", 0, 360, &s.rev)
        || !number(m, "rigLat", 0, 360, &s.rigLat) || !number(m, "rigLon", 0, 360, &s.rigLon) || !number(m, "rigRev", 0, 360, &s.rigRev))
        return false;
    *out = s;
    return true;
}

// ---- names

const QStringList &bevelPresets() {
    static const QStringList names{"circle", "relaxedInset", "cross", "coolSlant", "angle", "softRound", "convex",
                                   "slope", "divot", "riblet", "hardEdge", "artDeco"};
    return names;
}
const QStringList &materialPresets() {
    static const QStringList names{"warmMatte", "matte", "plastic", "metal", "dkEdge", "softEdge", "flat", "softmetal",
                                   "clear", "powder", "translucentPowder", "legacyMatte", "legacyPlastic", "legacyMetal",
                                   "legacyWireframe"};
    return names;
}
const QStringList &lightRigs() {
    static const QStringList names{"threePt", "balanced", "soft", "harsh", "flood", "contrasting", "morning", "sunrise",
                                   "sunset", "chilly", "freezing", "flat", "twoPt", "glow", "brightRoom",
                                   "legacyFlat1", "legacyFlat2", "legacyFlat3", "legacyFlat4", "legacyNormal1",
                                   "legacyNormal2", "legacyNormal3", "legacyNormal4", "legacyHarsh1", "legacyHarsh2",
                                   "legacyHarsh3", "legacyHarsh4"};
    return names;
}
const QStringList &rigDirections() {
    static const QStringList names{"t", "tr", "r", "br", "b", "bl", "l", "tl"};
    return names;
}

namespace {
struct CameraRow { const char *name; bool parallel; qreal lat, lon, rev; qreal ox, oy, skew, angle; };
// 1/60000 degree for the angles, as in the tables.
const CameraRow kCameras[] = {
    {"orthographicFront", true, 0, 0, 0, 0, 0, 0, 0},
    {"isometricTopUp", true, 19476000, 18882000, 3612000, 0, 0, 0, 0},
    {"isometricTopDown", true, 19476000, 2718000, 17988000, 0, 0, 0, 0},
    {"isometricBottomUp", true, 2124000, 2718000, 3612000, 0, 0, 0, 0},
    {"isometricBottomDown", true, 2124000, 18882000, 17988000, 0, 0, 0, 0},
    {"isometricLeftUp", true, 19500000, 2700000, 0, 0, 0, 0, 0},
    {"isometricLeftDown", true, 2100000, 2700000, 0, 0, 0, 0, 0},
    {"isometricRightUp", true, 2100000, 18900000, 0, 0, 0, 0, 0},
    {"isometricRightDown", true, 19500000, 18900000, 0, 0, 0, 0, 0},
    {"isometricOffAxis1Left", true, 1080000, 3840000, 0, 0, 0, 0, 0},
    {"isometricOffAxis1Right", true, 1080000, 20040000, 0, 0, 0, 0, 0},
    {"isometricOffAxis1Top", true, 18078000, 18390000, 3456000, 0, 0, 0, 0},
    {"isometricOffAxis2Left", true, 1080000, 1560000, 0, 0, 0, 0, 0},
    {"isometricOffAxis2Right", true, 1080000, 17760000, 0, 0, 0, 0, 0},
    {"isometricOffAxis2Top", true, 18078000, 3210000, 18144000, 0, 0, 0, 0},
    {"isometricOffAxis3Left", true, 20520000, 3840000, 0, 0, 0, 0, 0},
    {"isometricOffAxis3Right", true, 20520000, 20040000, 0, 0, 0, 0, 0},
    {"isometricOffAxis3Bottom", true, 3522000, 18390000, 18144000, 0, 0, 0, 0},
    {"isometricOffAxis4Left", true, 20520000, 1560000, 0, 0, 0, 0, 0},
    {"isometricOffAxis4Right", true, 20520000, 17760000, 0, 0, 0, 0, 0},
    {"isometricOffAxis4Bottom", true, 3522000, 3210000, 3456000, 0, 0, 0, 0},
    {"obliqueTopLeft", true, 0, 0, 0, -0.5, -0.5, 30, -45},
    {"obliqueTop", true, 0, 0, 0, 0, -0.5, 30, -90},
    {"obliqueTopRight", true, 0, 0, 0, 0.5, -0.5, 30, -135},
    {"obliqueLeft", true, 0, 0, 0, -0.5, 0, 30, -360},
    {"obliqueRight", true, 0, 0, 0, 0.5, 0, 30, 180},
    {"obliqueBottomLeft", true, 0, 0, 0, -0.5, 0.5, 30, 45},
    {"obliqueBottom", true, 0, 0, 0, 0, 0.5, 30, 90},
    {"obliqueBottomRight", true, 0, 0, 0, 0.5, 0.5, 30, 135},
    {"perspectiveFront", false, 0, 0, 0, 0, 0, 0, 0},
    {"perspectiveLeft", false, 0, 1200000, 0, 0, 0, 0, 0},
    {"perspectiveRight", false, 0, 20400000, 0, 0, 0, 0, 0},
    {"perspectiveAbove", false, 20400000, 0, 0, 0, 0, 0, 0},
    {"perspectiveBelow", false, 1200000, 0, 0, 0, 0, 0, 0},
    {"perspectiveAboveLeftFacing", false, 2358000, 858000, 20466000, 0, 0, 0, 0},
    {"perspectiveAboveRightFacing", false, 2358000, 20742000, 1134000, 0, 0, 0, 0},
    {"perspectiveContrastingLeftFacing", false, 624000, 2634000, 21384000, 0, 0, 0, 0},
    {"perspectiveContrastingRightFacing", false, 624000, 18966000, 216000, 0, 0, 0, 0},
    {"perspectiveHeroicLeftFacing", false, 20940000, 858000, 156000, 0, 0, 0, 0},
    {"perspectiveHeroicRightFacing", false, 20940000, 20742000, 21444000, 0, 0, 0, 0},
    {"perspectiveHeroicExtremeLeftFacing", false, 486000, 2070000, 21426000, 0, 0, 0, 0},
    {"perspectiveHeroicExtremeRightFacing", false, 486000, 19530000, 174000, 0, 0, 0, 0},
    {"perspectiveRelaxed", false, 18576000, 0, 0, 0, 0, 0, 0},
    {"perspectiveRelaxedModerately", false, 19488000, 0, 0, 0, 0, 0, 0},
    {"legacyObliqueTopLeft", true, 0, 0, 0, -0.5, -0.5, 50, -45},
    {"legacyObliqueTop", true, 0, 0, 0, 0, -0.5, 50, -90},
    {"legacyObliqueTopRight", true, 0, 0, 0, 0.5, -0.5, 50, -135},
    {"legacyObliqueLeft", true, 0, 0, 0, -0.5, 0, 50, -360},
    {"legacyObliqueFront", true, 0, 0, 0, 0, 0, 0, 0},
    {"legacyObliqueRight", true, 0, 0, 0, 0.5, 0, 50, 180},
    {"legacyObliqueBottomLeft", true, 0, 0, 0, -0.5, 0.5, 50, 45},
    {"legacyObliqueBottom", true, 0, 0, 0, 0, 0.5, 50, 90},
    {"legacyObliqueBottomRight", true, 0, 0, 0, 0.5, 0.5, 50, 135},
    {"legacyPerspectiveTopLeft", false, 0, 0, 0, -0.5, -0.5, 50, -45},
    {"legacyPerspectiveTop", false, 0, 0, 0, 0, -0.5, 50, -90},
    {"legacyPerspectiveTopRight", false, 0, 0, 0, 0.5, -0.5, 50, -135},
    {"legacyPerspectiveLeft", false, 0, 0, 0, -0.5, 0, 50, -360},
    {"legacyPerspectiveFront", false, 0, 0, 0, 0, 0, 0, 0},
    {"legacyPerspectiveRight", false, 0, 0, 0, 0.5, 0, 50, 180},
    {"legacyPerspectiveBottomLeft", false, 0, 0, 0, -0.5, 0.5, 50, 45},
    {"legacyPerspectiveBottom", false, 0, 0, 0, 0, 0.5, 50, 90},
    {"legacyPerspectiveBottomRight", false, 0, 0, 0, 0.5, 0.5, 50, 135},
};
} // namespace

const QStringList &cameraPresets() {
    static const QStringList names = [] {
        QStringList l;
        for (const auto &row : kCameras) l << QLatin1String(row.name);
        return l;
    }();
    return names;
}

Camera camera(const QString &preset) {
    Camera c;
    for (const auto &row : kCameras) {
        if (preset != QLatin1String(row.name)) continue;
        c.parallel = row.parallel;
        c.lat = row.lat / 60000.0; c.lon = row.lon / 60000.0; c.rev = row.rev / 60000.0;
        c.originX = row.ox; c.originY = row.oy; c.skew = row.skew; c.skewAngle = row.angle;
        c.legacy = preset.startsWith(QLatin1String("legacy"));
        break;
    }
    return c;
}

void rotation(const Scene &s, qreal *lat, qreal *lon, qreal *rev) {
    const Camera c = camera(s.camera);
    if (s.rotated) { *lat = s.lat; *lon = s.lon; *rev = s.rev; }
    else { *lat = c.lat; *lon = c.lon; *rev = c.rev; }
    if (s.camera.startsWith(QLatin1String("legacyPerspective"))) *rev = 0;   // PowerPoint ignores it there
}

bool perspective(const Scene &s) { return !camera(s.camera).parallel; }

qreal fieldOfView(const Scene &s) {
    if (!perspective(s)) return 0;
    return s.fov >= 0 ? s.fov : 45;
}

namespace {
struct RigRow { const char *name; qreal ambient; int count; Light lights[4]; };
const RigRow kRigs[] = {
    {"balanced", .13, 3, {{1.05, 1.05, 1.05, 0.5263, -0.4092, -0.7453, true, true}, {1, 1, 1, -0.9386, 0.3426, -0.041, true, true},
                          {.5, .5, .5, 0.0934, 0.763, 0.6396, true, true}, {}}},
    {"brightRoom", 1.5, 4, {{1, 1, 1, 0, -1, 0, false, true}, {1, 1, 1, 0.8227, -0.1882, -0.5364, true, false},
                            {-.5, -.5, -.5, 0, 0, -1, false, true}, {.5, .5, .5, 0, 1, 0, false, true}}},
    {"chilly", .11, 4, {{.31, .32, .32, 0.6574, -0.7316, -0.1806, true, true}, {.45, .45, .45, -0.3539, -0.1505, -0.9231, false, true},
                        {1.03, 1.02, 1.15, 0.672, -0.6185, -0.4073, false, true}, {.41, .45, .48, -0.5781, 0.7976, 0.1722, true, true}}},
    {"contrasting", 1, 2, {{1, 1, 1, 0, -1, 0, true, false}, {1, 1, 1, 0, 1, 0, true, false}, {}, {}}},
    {"flat", 1, 3, {{.821, .821, .821, -0.9546, -0.1619, -0.2502, true, false}, {2.072, 2.54, 2.91, 0.0009, 0.8605, 0.5095, true, false},
                    {3.843, 3.843, 3.843, 0.6574, -0.7316, -0.1806, true, false}, {}}},
    {"flood", .13, 3, {{1.1, 1.1, 1.1, 0.5685, -0.7651, -0.3022, true, true}, {1.1, 1.1, 1.1, -0.2366, -0.9595, -0.1531, true, true},
                       {.55, .55, .55, -0.8982, 0.1386, -0.4171, true, true}, {}}},
    {"freezing", 0, 4, {{.53, .567, .661, 0.6574, -0.7316, -0.1806, true, true}, {.37, .461, .461, -0.2781, -0.4509, -0.8482, false, true},
                        {.649, .638, .904, 0.672, -0.6185, -0.4073, false, true}, {.971, 1.19, 1.363, -0.1825, 0.968, 0.1722, true, true}}},
    {"glow", 1, 2, {{1, 1, 1, 0, -1, 0, true, true}, {.7, .7, .7, 0, 1, 0, true, true}, {}, {}}},
    {"harsh", .28, 2, {{.88, .88, .88, 0.6689, -0.6755, -0.3104, true, true}, {.88, .88, .88, -0.592, -0.7371, -0.326, true, true}, {}, {}}},
    {"legacyFlat1", .305, 2, {{.58, .58, .58, 0, 0, -0.2, true, true}, {.58, .58, .58, 0, 0, -0.2, false, true}, {}, {}}},
    {"legacyFlat2", .305, 2, {{.58, .58, .58, -1, -1, -0.2, true, true}, {.58, .58, .58, 0, 1, -0.2, false, true}, {}, {}}},
    {"legacyFlat3", .305, 2, {{.58, .58, .58, 0, -1, -0.2, true, true}, {.58, .58, .58, 0, 1, -0.2, false, true}, {}, {}}},
    {"legacyFlat4", .305, 2, {{.58, .58, .58, 1, -1, -0.2, true, true}, {.58, .58, .58, 0, 1, -0.2, false, true}, {}, {}}},
    {"legacyHarsh1", .061, 2, {{.793, .793, .793, 0, 0, -0.2, true, true}, {.214, .214, .214, 0, 0, -0.2, false, true}, {}, {}}},
    {"legacyHarsh2", .061, 2, {{.793, .793, .793, -1, -1, -0.2, true, true}, {.214, .214, .214, 0, 1, -0.2, false, true}, {}, {}}},
    {"legacyHarsh3", .061, 2, {{.793, .793, .793, 0, -1, -0.2, true, true}, {.214, .214, .214, 0, 1, -0.2, false, true}, {}, {}}},
    {"legacyHarsh4", .061, 2, {{.793, .793, .793, 1, -1, -0.2, true, true}, {.214, .214, .214, 0, 1, -0.2, false, true}, {}, {}}},
    {"legacyNormal1", .153, 2, {{.671, .671, .671, 0, 0, -0.2, true, true}, {.366, .366, .366, 0, 0, -0.2, false, true}, {}, {}}},
    {"legacyNormal2", .153, 2, {{.671, .671, .671, -1, -1, -0.2, true, true}, {.366, .366, .366, 0, 1, -0.2, false, true}, {}, {}}},
    {"legacyNormal3", .153, 2, {{.671, .671, .671, 0, -1, -0.2, true, true}, {.366, .366, .366, 0, 1, -0.2, false, true}, {}, {}}},
    {"legacyNormal4", .153, 2, {{.671, .671, .671, 1, -1, -0.2, true, true}, {.366, .366, .366, 0, 1, -0.2, false, true}, {}, {}}},
    {"morning", 0, 4, {{.669, .648, .596, 0.6574, -0.7316, -0.1806, true, true}, {.459, .454, .385, -0.2781, -0.4509, -0.8482, false, true},
                       {.9, .86, .83, 0.672, -0.6185, -0.4073, false, true}, {.911, .846, .728, -0.1825, 0.968, 0.1722, true, true}}},
    {"soft", .3, 1, {{.8, .8, .8, -0.6897, 0.2484, -0.6802, true, true}, {}, {}, {}}},
    {"sunrise", 0, 4, {{.667, .63, .527, 0.6574, -0.7316, -0.1806, true, true}, {.459, .459, .371, -0.2781, -0.4509, -0.8482, false, true},
                       {.826, .712, .638, 0.672, -0.6185, -0.4073, false, true}, {1.511, 1.319, .994, -0.1825, 0.968, 0.1722, false, true}}},
    {"sunset", 0, 4, {{.672, .169, .169, 0.6574, -0.7316, -0.1806, true, true}, {.459, .448, .327, 0.0922, -0.3551, -0.9303, false, true},
                      {.775, .612, .502, 0.672, -0.6185, -0.4073, false, true}, {.761, .69, .397, -0.424, 0.8891, 0.1722, false, true}}},
    {"threePt", 0, 3, {{1.141, 1.141, 1.141, -0.6515, -0.2693, -0.7093, true, true}, {.5, .5, .5, 0.8482, 0.2469, -0.4686, true, true},
                       {1, 1, 1, 0.5634, -0.2812, 0.7769, true, true}, {}}},
    {"twoPt", .25, 2, {{.84, .84, .84, 0.5266, -0.4089, -0.7454, true, true}, {.3, .3, .3, -0.8983, 0.2365, -0.3704, true, true}, {}, {}}},
};

// Rotation matrices as [MS-OI29500] composes them (y axis down).
struct M3 { qreal m[3][3]; };
M3 mul(const M3 &a, const M3 &b) {
    M3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
M3 aboutZ(qreal deg) { const qreal c = std::cos(qDegreesToRadians(deg)), s = std::sin(qDegreesToRadians(deg)); return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}}; }
M3 aboutX(qreal deg) { const qreal c = std::cos(qDegreesToRadians(deg)), s = std::sin(qDegreesToRadians(deg)); return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}}; }
M3 aboutY(qreal deg) { const qreal c = std::cos(qDegreesToRadians(deg)), s = std::sin(qDegreesToRadians(deg)); return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}}; }
} // namespace

Rig rig(const Scene &s) {
    Rig out;
    for (const auto &row : kRigs) {
        if (s.rig != QLatin1String(row.name)) continue;
        out.ambient = row.ambient;
        for (int i = 0; i < row.count; ++i) out.lights.append(row.lights[i]);
    }
    if (out.lights.isEmpty()) { out.ambient = 0; out.lights.append({1.141, 1.141, 1.141, -0.6515, -0.2693, -0.7093, true, true}); }
    // [MS-OI29500] direction L is V(-Ly, -Lx, Lz) in shape space; then the rig's turn.
    M3 turn;
    if (s.rigRotated) turn = mul(aboutX(s.rigLat), mul(aboutY(s.rigLon), aboutZ(s.rigRev)));
    else {
        static const QHash<QString, qreal> dirs{{"t", 0}, {"tr", 45}, {"r", 90}, {"br", 135}, {"b", 180}, {"bl", -135}, {"l", -90}, {"tl", -45}};
        turn = aboutZ(dirs.value(s.rigDir, 0));
    }
    for (auto &l : out.lights) {
        const qreal v[3] = {-l.y, -l.x, l.z};
        const qreal len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        qreal w[3];
        for (int i = 0; i < 3; ++i) w[i] = (turn.m[i][0] * v[0] + turn.m[i][1] * v[1] + turn.m[i][2] * v[2]) / (len > 0 ? len : 1);
        l.x = w[0]; l.y = w[1]; l.z = w[2];
    }
    return out;
}

Material material(const QString &name) {
    struct Row { const char *name; qreal diffuse, specular, power; bool metal; };
    static const Row rows[] = {
        {"clear", 100, 60, 20, false}, {"dkEdge", 70, 100, 35, false}, {"flat", 100, 80, 50, false},
        {"legacyMatte", 100, 0, 0, false}, {"legacyMetal", 66.7, 122.07, 32, true}, {"legacyPlastic", 100, 122.07, 32, false},
        {"matte", 100, 0, 0, false}, {"metal", 100, 100, 12, true}, {"plastic", 100, 60, 12, true}, {"powder", 100, 30, 10, false},
        {"softEdge", 100, 100, 35, false}, {"softmetal", 100, 100, 8, true}, {"translucentPowder", 100, 30, 10, true},
        {"warmMatte", 100, 30, 8, false}, {"legacyWireframe", 100, 0, 0, false}};
    const QString wanted = name.isEmpty() ? QStringLiteral("warmMatte") : name;
    for (const auto &row : rows)
        if (wanted == QLatin1String(row.name)) return {row.diffuse / 100, row.specular / 100, qMax<qreal>(1, row.power), row.metal};
    return {1, .3, 8, false};
}

// ---- galleries

const QVector<ReflectionPreset> &reflectionPresets() {
    // As PowerPoint writes them: blurRad 0.5 pt, stA 50%, dir 90°, sy -100%,
    // algn bl, no rotate with shape.
    static const QVector<ReflectionPreset> presets{
        {QStringLiteral("Tight Reflection: Touching"), 35000, 0, 300},
        {QStringLiteral("Half Reflection: Touching"), 55000, 0, 300},
        {QStringLiteral("Full Reflection: Touching"), 90000, 0, 300},
        {QStringLiteral("Tight Reflection: 4 pt offset"), 35000, 4, 300},
        {QStringLiteral("Half Reflection: 4 pt offset"), 55000, 4, 300},
        {QStringLiteral("Full Reflection: 4 pt offset"), 90000, 4, 300},
        {QStringLiteral("Tight Reflection: 8 pt offset"), 35000, 8, 300},
        {QStringLiteral("Half Reflection: 8 pt offset"), 55000, 8, 300},
        {QStringLiteral("Full Reflection: 8 pt offset"), 92000, 8, 295}};
    return presets;
}

Reflection reflectionPreset(int index, qreal unitsPerPoint) {
    Reflection r;
    const auto &all = reflectionPresets();
    if (index < 0 || index >= all.size()) return r;
    const auto &p = all.at(index);
    r.on = true;
    r.blur = 0.5 * unitsPerPoint;
    r.startAlpha = 50;
    r.endAlpha = p.endAlpha / 1000.0;
    r.endPos = p.endPos / 1000.0;
    r.dist = p.dist * unitsPerPoint;
    r.dir = 90;
    r.sy = -100;
    r.align = 6;
    r.rotate = false;
    return r;
}

const QVector<qreal> &softEdgePresets() {
    static const QVector<qreal> points{1, 2.5, 5, 10, 25, 50};
    return points;
}

bool drawn(const Reflection &r) { return r.on && r.startAlpha + r.endAlpha > 0 && r.sx != 0 && r.sy != 0; }
bool drawn(const Format &f) { return f.on && (f.top.on() || f.bottom.on() || f.depth > 0 || f.contour > 0); }
bool drawn(const Scene &s) {
    if (!s.on) return false;
    qreal lat, lon, rev;
    rotation(s, &lat, &lon, &rev);
    const Camera c = camera(s.camera);
    return lat != 0 || lon != 0 || rev != 0 || c.skew != 0 || !c.parallel;
}

void scale(Reflection &r, qreal k) { r.blur *= k; r.dist *= k; }
void scale(Format &f, qreal k) {
    f.top.w *= k; f.top.h *= k; f.bottom.w *= k; f.bottom.h *= k;
    f.depth *= k; f.contour *= k; f.z *= k;
}

Reflection lerp(const Reflection &a, const Reflection &b, qreal u) {
    if (a == b) return a;
    // Off on one side: fade it in or out from the other's look.
    Reflection from = a.on ? a : b, to = b.on ? b : a;
    if (!a.on) from.startAlpha = from.endAlpha = 0;
    if (!b.on) to.startAlpha = to.endAlpha = 0;
    Reflection r = u < .5 ? from : to;
    r.on = a.on || b.on;
    if (u >= 1 && !b.on) return b;
    r.blur = mix(from.blur, to.blur, u); r.dist = mix(from.dist, to.dist, u);
    r.startAlpha = mix(from.startAlpha, to.startAlpha, u); r.startPos = mix(from.startPos, to.startPos, u);
    r.endAlpha = mix(from.endAlpha, to.endAlpha, u); r.endPos = mix(from.endPos, to.endPos, u);
    r.dir = mixAngle(from.dir, to.dir, u); r.fadeDir = mixAngle(from.fadeDir, to.fadeDir, u);
    r.sx = mix(from.sx, to.sx, u); r.sy = mix(from.sy, to.sy, u); r.kx = mix(from.kx, to.kx, u); r.ky = mix(from.ky, to.ky, u);
    return r;
}

namespace {
Bevel lerpBevel(const Bevel &a, const Bevel &b, qreal u) {
    if (a == b) return a;
    Bevel r = u < .5 ? (a.on() ? a : b) : (b.on() ? b : a);
    if (!a.on() && !b.on()) return a;
    if (u >= 1 && !b.on()) return b;
    r.w = mix(a.on() ? a.w : 0, b.on() ? b.w : 0, u);
    r.h = mix(a.on() ? a.h : 0, b.on() ? b.h : 0, u);
    return r;
}
} // namespace

Format lerp(const Format &a, const Format &b, qreal u) {
    if (a == b) return a;
    if (u >= 1) return b;
    if (u <= 0) return a;
    Format r = u < .5 ? (a.on ? a : b) : (b.on ? b : a);
    r.on = a.on || b.on;
    r.top = lerpBevel(a.top, b.top, u);
    r.bottom = lerpBevel(a.bottom, b.bottom, u);
    r.depth = mix(a.depth, b.depth, u); r.contour = mix(a.contour, b.contour, u); r.z = mix(a.z, b.z, u);
    r.extrusionColor = mixColour(a.extrusionColor, b.extrusionColor, u);
    r.contourColor = mixColour(a.contourColor, b.contourColor, u);
    return r;
}

Scene lerp(const Scene &a, const Scene &b, qreal u) {
    if (a == b) return a;
    if (u >= 1) return b;
    if (u <= 0) return a;
    // The rotation eased; the camera kind and the rig switch half way.
    Scene r = u < .5 ? (a.on ? a : b) : (b.on ? b : a);
    r.on = a.on || b.on;
    qreal la, lo, re, lb, lob, rb;
    Scene fa = a.on ? a : Scene(), fb = b.on ? b : Scene();
    rotation(fa, &la, &lo, &re);
    rotation(fb, &lb, &lob, &rb);
    r.rotated = true;
    r.lat = mixAngle(la, lb, u); r.lon = mixAngle(lo, lob, u); r.rev = mixAngle(re, rb, u);
    r.fov = mix(fieldOfView(fa), fieldOfView(fb), u);
    if (!perspective(r)) r.fov = -1;
    r.zoom = mix(fa.zoom, fb.zoom, u);
    return r;
}

} // namespace Effects3D

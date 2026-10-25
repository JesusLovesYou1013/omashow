#pragma once

// The rest of PowerPoint's Format Shape > Effects, as data.
//
//   soft edges   (a:softEdge rad)          SceneObject::softEdge
//   reflection   (a:reflection)            Effects3D::Reflection
//   3-D format   (a:sp3d: bevels, depth, contour, material)   Effects3D::Format
//   3-D rotation (a:scene3d: camera, light rig)               Effects3D::Scene
//
// Every attribute PowerPoint writes is kept as written, so a deck goes back to
// PowerPoint unchanged even where OmaShow's drawing is only an approximation
// (render/layereffects.h says which). Lengths are slide units, angles degrees,
// transparencies and positions percent, as the rest of SceneObject.
//
// The camera, light rig and material tables are PowerPoint's own presets as
// measured and published in [MS-OI29500] and LibreOffice's
// oox/source/drawingml/scene3dhelper.cxx (MPL 2.0; data values only, see
// THIRD-PARTY-NOTICES.md).

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace Effects3D {

// a:reflection. Off when `on` is false (no element). Defaults are the
// schema's own (what an attribute left out means).
struct Reflection {
    bool on = false;
    qreal blur = 0, dist = 0;                 // slide units
    qreal startAlpha = 100, startPos = 0;     // percent (stA, stPos)
    qreal endAlpha = 0, endPos = 100;         // percent (endA, endPos)
    qreal dir = 90, fadeDir = 90;             // degrees
    qreal sx = 100, sy = 100, kx = 0, ky = 0; // percent, degrees
    int align = 7;                            // 0-8 tl t tr l ctr r bl b br (b)
    bool rotate = true;                       // rotWithShape
    bool operator==(const Reflection &o) const;
    bool operator!=(const Reflection &o) const { return !(*this == o); }
};

// One bevel (a:bevelT / a:bevelB). No preset name: no bevel.
struct Bevel {
    QString preset;                           // circle (default) ... artDeco
    qreal w = 0, h = 0;                       // slide units
    bool on() const { return !preset.isEmpty(); }
    bool operator==(const Bevel &o) const;
};

// a:sp3d.
struct Format {
    bool on = false;
    Bevel top, bottom;
    qreal depth = 0, contour = 0, z = 0;      // extrusionH, contourW, z (slide units)
    QColor extrusionColor, contourColor;      // invalid: not written (the shape's own)
    QString material;                         // empty: not written (warmMatte)
    bool operator==(const Format &o) const;
    bool operator!=(const Format &o) const { return !(*this == o); }
};

// a:scene3d.
struct Scene {
    bool on = false;
    QString camera = QStringLiteral("orthographicFront");
    qreal fov = -1;                           // degrees; -1 not written
    qreal zoom = 100;                         // percent
    bool rotated = false;                     // a:camera/a:rot written
    qreal lat = 0, lon = 0, rev = 0;          // degrees
    QString rig = QStringLiteral("threePt"), rigDir = QStringLiteral("t");
    bool rigRotated = false;                  // a:lightRig/a:rot written
    qreal rigLat = 0, rigLon = 0, rigRev = 0;
    bool operator==(const Scene &o) const;
    bool operator!=(const Scene &o) const { return !(*this == o); }
};

// ---- as values (Design::properties), checked on the way in

QVariantMap toMap(const Reflection &r);
QVariantMap toMap(const Format &f);
QVariantMap toMap(const Scene &s);
// An empty map is "none". False when anything in it is unknown or out of range.
bool fromMap(const QVariant &v, Reflection *out);
bool fromMap(const QVariant &v, Format *out);
bool fromMap(const QVariant &v, Scene *out);

// ---- PowerPoint's names

const QStringList &bevelPresets();      // ST_BevelPresetType, gallery order
const QStringList &materialPresets();   // ST_PresetMaterialType
const QStringList &cameraPresets();     // ST_PresetCameraType
const QStringList &lightRigs();         // ST_LightRigType
const QStringList &rigDirections();     // ST_LightRigDirection

// Camera preset geometry. Angles in degrees.
struct Camera {
    bool parallel = true;
    qreal lat = 0, lon = 0, rev = 0;      // the camera's rotation (Total = Z(rev) X(lat) Y(lon))
    qreal skew = 0, skewAngle = 0;        // oblique: percent of depth drawn as slant, its angle
    qreal originX = 0, originY = 0;       // oblique: fraction of the box the slant leaves from
    bool legacy = false;                  // legacy cameras: shape rotation not after the camera
};
Camera camera(const QString &preset);
// The camera's rotation as drawn: a:rot when written, else the preset's.
void rotation(const Scene &s, qreal *lat, qreal *lon, qreal *rev);
bool perspective(const Scene &s);
// Field of view as drawn (PowerPoint's 45° when perspective and not written).
qreal fieldOfView(const Scene &s);

// One light of a rig: its colour (may exceed 1), direction the light travels
// in shape space (x right, y down, z toward the viewer), and what it lights.
struct Light { qreal r, g, b; qreal x, y, z; bool specular, diffuse; };
struct Rig { qreal ambient = 0; QVector<Light> lights; };
// The rig turned by its direction (or a:rot).
Rig rig(const Scene &s);

// Material: how much of the light it scatters and how shiny it is.
struct Material { qreal diffuse = 1, specular = 0.3, power = 8; bool metal = false; };
Material material(const QString &name);

// The colour PowerPoint gives a flat front face, from its fill colour, under a
// rig and material: each channel v becomes gain * v + lift (0-255), drawn at
// `opacity`; a wireframe draws only its outline. Measured from PowerPoint's
// own pictures (eight fills, each rig at its own direction with warmMatte,
// each material under threePt, on light and dark backgrounds); a rig and a
// material are measured apart and put together.
struct FaceTint { qreal gain[3] = {1, 1, 1}; qreal lift[3] = {0, 0, 0}; qreal opacity = 1; bool wireframe = false; };
FaceTint faceTint(const Scene &s, const QString &material);

// ---- Reflection Variations (Format Shape > Effects > Reflection), and the
// Soft Edges and Bevel galleries, as PowerPoint writes them.

struct ReflectionPreset { QString name; qreal endPos; qreal dist; qreal endAlpha; qreal startAlpha = 50000; };
// tight/half/full × touching/4 pt/8 pt. Lengths in points.
const QVector<ReflectionPreset> &reflectionPresets();
Reflection reflectionPreset(int index, qreal unitsPerPoint);
const QVector<qreal> &softEdgePresets();   // points

// What any of it needs drawn (render/layereffects.h).
bool drawn(const Reflection &r);
bool drawn(const Format &f);
bool drawn(const Scene &s);

// Lengths times `scale` (deck resize).
void scale(Reflection &r, qreal scale);
void scale(Format &f, qreal scale);

// Morph: numbers eased, names and on/off switched half way.
Reflection lerp(const Reflection &a, const Reflection &b, qreal u);
Format lerp(const Format &a, const Format &b, qreal u);
Scene lerp(const Scene &a, const Scene &b, qreal u);

} // namespace Effects3D

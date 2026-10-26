#pragma once

// PowerPoint's Format Shape > Fill and Line, as data: gradient
// stops, the preset patterns and dashes by their DrawingML names, and a text
// box's own box (boxDrawn). Drawing them is render/fillpaint.h.

#include "core/scene.h"

#include <QStringList>
#include <QVector>

namespace FillStyle {

// ---- gradients

QVariantList encodeStops(const QVector<GradientStop> &stops);
// False when the value isn't a list of stops (an empty list is fine).
bool decodeStops(const QVariant &value, QVector<GradientStop> *out);
// The stop's colour with its Brightness applied (lumMod/lumOff, as PowerPoint).
QColor stopColor(const GradientStop &stop);
QColor withBrightness(const QColor &color, qreal brightness);
// The stops a fill draws: its own, else fill to fillSecondary; sorted.
QVector<GradientStop> fillStops(const SceneObject &o);
QVector<GradientStop> lineStops(const SceneObject &o);
// The colour at t (0-1) along sorted stops.
QColor colorAt(const QVector<GradientStop> &sorted, qreal t);
// Keeps fill/fillSecondary (and strokeColor) on the first and last stop.
void syncLegacy(SceneObject &o);

// ---- patterns: the 54 names of ST_PresetPatternVal, in PowerPoint's order.

const QStringList &patternPresets();
// 64 cells, row by row: '#' foreground, '.' background, '+' half way. Empty
// for a name that isn't one.
QByteArray patternCells(const QString &preset);
// The nearest of OmaShow's 13 Qt patterns (patternStyle), for older builds.
int nearestQtPattern(const QString &preset);
// The preset a Qt pattern (patternStyle) is written as.
QString presetForQtPattern(int patternStyle);

// ---- dashes: PowerPoint's preset dashes (a:prstDash), solid first.

const QStringList &dashPresets();
// Dash and space lengths in line widths (empty: solid).
QVector<qreal> dashPattern(const QString &preset);
QVector<qreal> dashPattern(const SceneObject &o);
// The nearest of the five Qt dashes (strokeStyle), and the preset each is.
int nearestQtDash(const QString &preset);
QString presetForQtDash(int strokeStyle);
// a:ln cmpd values, by strokeCompound.
const QStringList &compoundNames();

// ---- whether anything new is in use (else the old drawing is used as is)

bool modernFill(const SceneObject &o);
bool modernLine(const SceneObject &o);

// ---- a text box's own box

// Copies the fill (picture included) and the line from one object to another.
void copyFillAndLine(SceneObject &to, const SceneObject &from);
// Just the picture of a picture fill (fillStyle 4), and how it is placed.
void copyPicture(SceneObject &to, const SceneObject &from);

// The box: the text's rectangle grown by its insets.
QRectF boxRect(const SceneObject &o);
// It has a fill or an outline to draw.
bool boxVisible(const SceneObject &o);
// The box as a shape (rectangle) carrying the text box's fill, line, glow and
// box shadow; the words as a text box with none of them. For writers that
// have no text box with a box of its own (ODP, PPT, Keynote).
SceneObject boxShape(const SceneObject &o);
QVector<SceneObject> split(const SceneObject &o);

// ---- Text Fill, Text Outline and a stretch's own text effects

// The keys a text fill / outline / run effect set may hold.
const QStringList &textFillKeys();
const QStringList &textOutlineKeys();
const QStringList &textEffectKeys();
// Checked and tidied (each value as Design::properties gives it back); an
// empty map is "as the box" (fill: textColor, outline: none).
bool checkTextFill(const QVariant &value, QVariantMap *out);
bool checkTextOutline(const QVariant &value, QVariantMap *out);
bool checkTextEffects(const QVariant &value, QVariantMap *out);
// A shape carrying a text fill or outline map, to draw it and edit it with
// the shape's own editors (solid: `color`).
SceneObject textFillCarrier(const QVariantMap &fill, const QColor &color);
SceneObject textOutlineCarrier(const QVariantMap &outline);
// Back from a carrier: its fill (empty when solid) or its line.
QVariantMap textFillOf(const SceneObject &carrier);
QVariantMap textOutlineOf(const SceneObject &carrier);
// The text effects a stretch draws with: the box's, or the run's own set.
SceneObject withTextEffects(const SceneObject &box, const QVariantMap &effects);
QVariantMap textEffectsOf(const SceneObject &o);

} // namespace FillStyle

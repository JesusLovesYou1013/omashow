#include "anim/build.h"

#include <algorithm>

#include <cmath>
#include <QRegularExpression>

QPainterPath MotionPath::parse(const QString &text, const QSizeF &stage) {
    QPainterPath path;
    static const QRegularExpression token(QStringLiteral("[MLCZEmlcz]|[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?"));
    QVector<QString> tokens;
    for (auto it = token.globalMatch(text); it.hasNext();) tokens.append(it.next().captured(0));
    const qreal w = stage.width() > 0 ? stage.width() : 1920, h = stage.height() > 0 ? stage.height() : 1080;
    QChar command = QLatin1Char('M');
    QPointF at(0, 0), first(0, 0);
    bool started = false;
    int i = 0;
    const auto number = [&](bool &ok) -> qreal { if (i >= tokens.size()) { ok = false; return 0; } bool good = false; const qreal v = tokens.at(i).toDouble(&good); if (!good) { ok = false; return 0; } ++i; return v; };
    while (i < tokens.size()) {
        const QString &tk = tokens.at(i);
        if (tk.size() == 1 && tk.at(0).isLetter()) { command = tk.at(0); ++i; if (command == QLatin1Char('E')) break;
            if (command.toUpper() == QLatin1Char('Z')) { if (started) path.closeSubpath(); at = first; continue; } }
        const bool rel = command.isLower();
        const auto pt = [&](bool &ok) { const qreal x = number(ok), y = number(ok); QPointF p(x * w, y * h); return rel ? at + p : p; };
        bool ok = true;
        switch (command.toUpper().toLatin1()) {
        case 'M': { const QPointF p = pt(ok); if (!ok) return path; path.moveTo(p); at = first = p; started = true; command = rel ? QLatin1Char('l') : QLatin1Char('L'); break; }
        case 'L': { const QPointF p = pt(ok); if (!ok) return path; if (!started) { path.moveTo(0, 0); started = true; } path.lineTo(p); at = p; break; }
        case 'C': { const QPointF a = pt(ok), b = pt(ok), c = pt(ok); if (!ok) return path; if (!started) { path.moveTo(0, 0); started = true; } path.cubicTo(a, b, c); at = c; break; }
        default: ++i; break;
        }
    }
    return path;
}

QString MotionPath::write(const QPainterPath &path, const QPointF &origin, const QSizeF &stage) {
    const qreal w = stage.width() > 0 ? stage.width() : 1920, h = stage.height() > 0 ? stage.height() : 1080;
    const auto f = [&](const QPointF &p) { return QStringLiteral("%1 %2").arg((p.x() - origin.x()) / w, 0, 'f', 5).arg((p.y() - origin.y()) / h, 0, 'f', 5); };
    QStringList out;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto e = path.elementAt(i);
        if (e.isMoveTo()) out << QStringLiteral("M ") + f(e);
        else if (e.isLineTo()) out << QStringLiteral("L ") + f(e);
        else if (e.isCurveTo() && i + 2 < path.elementCount()) { out << QStringLiteral("C ") + f(e) + QLatin1Char(' ') + f(path.elementAt(i + 1)) + QLatin1Char(' ') + f(path.elementAt(i + 2)); i += 2; }
    }
    out << QStringLiteral("E");
    return out.join(QLatin1Char(' '));
}

qreal BuildStep::linearAt(qreal t) const {
    if (duration <= 0.0) return t >= start ? (rewind ? 0.0 : 1.0) : 0.0;
    if (t <= start) return 0.0;
    const qreal local = t - start;
    // ANIM-INTEROP: repeats, reverses and rewinds. Once (the default) is the
    // plain 0 -> 1 it always was.
    if (repeat >= 0.0 && local >= span()) return rewind || autoReverse ? 0.0 : 1.0;
    if (!autoReverse && repeat <= 1.0 && repeat >= 0.0) return qBound(0.0, local / duration, 1.0);
    qreal c = std::fmod(local, cycle()) / duration;
    if (c > 1.0) c = 2.0 - c;
    return qBound(0.0, c, 1.0);
}

qreal BuildStep::progressAt(qreal t) const {
    const qreal l = linearAt(t);
    if (l <= 0.0 || l >= 1.0) return l;
    return QEasingCurve(easing).valueForProgress(l);
}

QString BuildStep::interopSig() const {
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9").arg(targetId).arg(int(effect)).arg(int(phase)).arg(direction)
        .arg(amount, 0, 'g', 6).arg(unit).arg(rangeFrom).arg(rangeTo).arg(motionPath + QLatin1Char('|') + pathId);
}

bool Effects::isEmphasis(Effect e) { return e == Effect::Pulse || e == Effect::Grow || e == Effect::Shrink || e == Effect::Rotate; }
bool Effects::isNew(Effect e) { return int(e) >= int(Effect::Appear); }
QString Effects::legacyName(Effect e) {
    if (e == Effect::Grow || e == Effect::Shrink) return QStringLiteral("pulse");
    if (e == Effect::Float) return QStringLiteral("rise");
    return QStringLiteral("fade");
}

// A fade is not eased (it would pop); a spin turns evenly; a motion path starts
// and stops smoothly; everything else decelerates into place. All of these are
// curves PowerPoint can store (its Smooth start / Smooth end are quadratic).
QEasingCurve::Type Effects::defaultEasing(Effect e) {
    if (e == Effect::Fade || e == Effect::Appear || e == Effect::Rotate) return QEasingCurve::Linear;
    if (e == Effect::Path) return QEasingCurve::InOutQuad;
    return QEasingCurve::OutQuad;
}

QEasingCurve::Type Effects::easingFromPpt(qreal accel, qreal decel) {
    if (accel > 0 && decel > 0) return QEasingCurve::InOutQuad;
    if (decel > 0) return QEasingCurve::OutQuad;
    if (accel > 0) return QEasingCurve::InQuad;
    return QEasingCurve::Linear;
}

void Effects::pptFromEasing(QEasingCurve::Type e, int &accel, int &decel) {
    accel = decel = 0;
    if (e == QEasingCurve::InOutQuad || e == QEasingCurve::InOutCubic) accel = decel = 50000;
    else if (e == QEasingCurve::OutQuad || e == QEasingCurve::OutCubic) decel = 100000;
    else if (e == QEasingCurve::InQuad || e == QEasingCurve::InCubic) accel = 100000;
}

EffectClass BuildStep::effectClass() const {
    if (effect == Effect::Media) return EffectClass::Media;
    if (effect == Effect::Path) return EffectClass::Motion;
    if (Effects::isEmphasis(effect)) return EffectClass::Emphasis;
    return phase == BuildPhase::Out ? EffectClass::Exit : EffectClass::Entrance;
}

bool BuildStep::changesVisibility() const {
    switch (effect) {
    case Effect::Fade: case Effect::Rise: case Effect::Appear: case Effect::Fly: case Effect::Float:
    case Effect::Zoom: case Effect::Turn: case Effect::Wipe:
        return true;
    default:
        return false;
    }
}

bool Timeline::visibleAt(const QVector<BuildStep> &resolved, const QString &targetId, qreal t) {
    QVector<const BuildStep *> mine;
    for (const auto &step : resolved)
        if (step.targetId == targetId && step.changesVisibility()) mine.append(&step);
    if (mine.isEmpty()) return true;
    std::stable_sort(mine.begin(), mine.end(), [](const BuildStep *a, const BuildStep *b) { return a->start < b->start; });
    bool visible = mine.first()->phase != BuildPhase::In;
    for (const auto *step : mine) {
        if (t < step->start) break;
        // Running entrances and exits are visible; the effect does the rest.
        // A rewound entrance leaves again when done; a rewound exit comes back.
        visible = step->phase == BuildPhase::In ? (!step->rewind || (t < step->end() && step->duration > 0.0))
                                                : (step->rewind || (t < step->end() && step->duration > 0.0));
    }
    return visible;
}

qreal Timeline::duration() const {
    qreal last = 0.0;
    for (const BuildStep &step : resolvedSteps())
        last = std::max(last, step.end());
    return last;
}

QVector<const BuildStep *> Timeline::stepsFor(const QString &targetId) const {
    QVector<const BuildStep *> found;
    for (const BuildStep &step : steps) {
        if (step.targetId == targetId)
            found.append(&step);
    }
    return found;
}

// ANIM-INTEROP: PowerPoint's model, so a deck plays the same in both. A click
// (or After previous) opens a run; With previous starts with that run (plus
// its own delay), After previous when everything since the click has finished.
QVector<BuildStep> Timeline::resolvedSteps() const {
    QVector<BuildStep> resolved;
    qreal runStart = 0, sinceClick = 0, groupEnd = 0;
    for (auto step : steps) {
        switch (step.trigger) {
        // ANIM-001: "At time" honours its delay like every other start.
        case BuildTrigger::Absolute: runStart = step.start; step.start += step.delay; break;
        case BuildTrigger::WithPrevious: step.start = runStart + step.delay; break;
        case BuildTrigger::AfterPrevious: runStart = sinceClick; step.start = runStart + step.delay; break;
        case BuildTrigger::OnClick:
            // Deterministic preview/export schedule. The presenter pauses at
            // this boundary until the speaker advances the next click group.
            runStart = sinceClick = groupEnd; step.start = runStart + step.delay; break;
        }
        sinceClick = std::max(sinceClick, step.end());
        groupEnd = std::max(groupEnd, step.end());
        resolved.append(step);
    }
    return resolved;
}

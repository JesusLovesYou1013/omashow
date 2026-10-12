#include "anim/build.h"

#include <algorithm>

qreal BuildStep::progressAt(qreal t) const {
    if (duration <= 0.0)
        return t >= start ? 1.0 : 0.0;
    if (t <= start)
        return 0.0;
    if (t >= end())
        return 1.0;
    return QEasingCurve(easing).valueForProgress((t - start) / duration);
}

qreal BuildStep::linearAt(qreal t) const {
    if (duration <= 0.0) return t >= start ? 1.0 : 0.0;
    return qBound(0.0, (t - start) / duration, 1.0);
}

bool Effects::isEmphasis(Effect e) { return e == Effect::Pulse || e == Effect::Grow || e == Effect::Shrink; }
bool Effects::isNew(Effect e) { return int(e) >= int(Effect::Appear); }
QString Effects::legacyName(Effect e) {
    if (e == Effect::Grow || e == Effect::Shrink) return QStringLiteral("pulse");
    if (e == Effect::Float) return QStringLiteral("rise");
    return QStringLiteral("fade");
}

// A fade is not eased (it would pop); a drift or a zoom settles gently; a
// travel decelerates into place.
QEasingCurve::Type Effects::defaultEasing(Effect e) {
    if (e == Effect::Fade || e == Effect::Appear) return QEasingCurve::Linear;
    if (e == Effect::Float || e == Effect::Zoom) return QEasingCurve::OutQuad;
    return QEasingCurve::OutCubic;
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
        visible = step->phase == BuildPhase::In ? true : t < step->end() && step->duration > 0.0;
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

QVector<BuildStep> Timeline::resolvedSteps() const {
    QVector<BuildStep> resolved;
    qreal previousStart = 0, previousEnd = 0, groupEnd = 0;
    for (auto step : steps) {
        switch (step.trigger) {
        // ANIM-001: "At time" honours its delay like every other start.
        case BuildTrigger::Absolute: step.start += step.delay; break;
        case BuildTrigger::WithPrevious: step.start = previousStart + step.delay; break;
        case BuildTrigger::AfterPrevious: step.start = previousEnd + step.delay; break;
        case BuildTrigger::OnClick:
            // Deterministic preview/export schedule. The presenter pauses at
            // this boundary until the speaker advances the next click group.
            step.start = groupEnd + step.delay; break;
        }
        previousStart = step.start; previousEnd = step.end();
        groupEnd = std::max(groupEnd, previousEnd);
        resolved.append(step);
    }
    return resolved;
}

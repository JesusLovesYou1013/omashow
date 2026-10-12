#include "anim/evaluator.h"

#include "core/scene.h"
#include "core/mediaasset.h"
#include "core/connector.h"
#include "core/shape.h"
#include "core/textruns.h"

#include <QTransform>

#include <algorithm>
#include <cmath>

namespace {

// How far a Rise build travels, in document units, before settling.
constexpr qreal kRiseDistance = 48.0;

// Where each paragraph, word or character of a text ends. A Reveal shows the
// text up to one of these, so nothing is ever half a letter wide.
QVector<int> unitEnds(const QString &text, int unit) {
    QVector<int> ends;
    if (unit == 2) {
        for (int i = 1; i <= text.size(); ++i) ends.append(i);
        return ends;
    }
    bool inWord = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (unit == 0) {
            if (c == QLatin1Char('\n')) ends.append(i);
        } else if (c.isSpace()) {
            if (inWord) { ends.append(i); inWord = false; }
        } else {
            inWord = true;
        }
    }
    if (ends.isEmpty() || ends.last() != text.size()) ends.append(text.size());
    return ends;
}

// Draws [from, to) of the text at `alpha` of its own colour, run by run, so a
// word already coloured keeps its colour while it fades.
void fadeText(SceneObject &object, int from, int to, qreal alpha) {
    QVector<int> cuts{from, to};
    for (const auto &run : object.runs)
        for (int edge : {run.start, run.start + run.length})
            if (edge > from && edge < to) cuts.append(edge);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (int i = 0; i + 1 < cuts.size(); ++i) {
        const auto *run = TextRuns::at(object.runs, cuts.at(i));
        QColor colour = run && run->color.isValid() ? run->color : object.textColor;
        colour.setAlphaF(colour.alphaF() * alpha);
        TextRuns::apply(object, cuts.at(i), cuts.at(i + 1), QStringLiteral("color"),
                        colour.name(QColor::HexArgb));
    }
}

void applyBuild(SceneObject &object, const BuildStep &step, qreal t, const Slide &slide) {
    const qreal progress = step.progressAt(t);
    qreal p = step.phase == BuildPhase::Out ? 1.0 - progress : progress;
    // ANIM-001: a new exit is its entrance played backwards, so it eases out
    // of place slowly and leaves quickly rather than rushing off at once.
    if (Effects::isNew(step.effect) && !Effects::isEmphasis(step.effect) && step.phase == BuildPhase::Out)
        p = step.duration <= 0.0 ? (t >= step.start ? 0.0 : 1.0)
                                 : QEasingCurve(step.easing).valueForProgress(1.0 - step.linearAt(t));
    switch (step.effect) {
    case Effect::Media:
        if(object.type==ObjectType::Media) MediaAsset::evaluate(object,t-step.start,step.duration);
        break;
    case Effect::None:
        break;
    case Effect::Fade:
        object.opacity *= p;
        break;
    case Effect::Rise:
        object.opacity *= p;
        object.rect.translate(0.0, kRiseDistance * (1.0 - p) * (step.phase == BuildPhase::Out ? -1.0 : 1.0));
        break;
    case Effect::Move:
        object.rect.translate(step.amountX * (1.0 - p), step.amountY * (1.0 - p));
        break;
    case Effect::Scale: {
        const qreal from = step.amount > 0 ? step.amount : 0.5;
        object.paintScale *= from + (1.0 - from) * p;
        break;
    }
    case Effect::Spin:
        object.rotation += (step.amount != 0 ? step.amount : 180.0) * (1.0 - p);
        break;
    case Effect::Pulse: {
        // An emphasis is not an entrance: it swells and settles where it is.
        const qreal swell = step.amount != 0 ? step.amount : 0.15;
        const qreal q = std::sin(M_PI * qBound(0.0, progress, 1.0));
        object.paintScale *= 1.0 + swell * q;
        break;
    }
    case Effect::Path: {
        const auto *guide = slide.find(step.pathId);
        if (!guide || guide->id == object.id) break;
        const auto path = Shape::worldPath(*guide);
        if (path.isEmpty() || path.length() <= 0) break;
        const qreal along = qBound(0.0, step.pathReverse ? 1.0 - p : p, 1.0);
        const qreal landing = step.pathReverse ? 0.0 : 1.0;
        // The object lands where it was authored: the path says how it gets
        // there, not where "there" is.
        const QPointF offset = object.rect.center() - path.pointAtPercent(landing);
        object.rect.moveCenter(path.pointAtPercent(along) + offset);
        if (step.orient) object.rotation -= path.angleAtPercent(along);
        break;
    }
    // ANIM-001. `p` is eased (motion); `seen` is linear (opacity), so a fade
    // never pops. Entrances run p 0 -> 1, exits 1 -> 0; visibility before an
    // entrance and after an exit is Timeline::visibleAt's, not the effect's.
    case Effect::Appear:
        break;
    case Effect::Fly: {
        // From just beyond the slide edge, wherever the object sits.
        const QSizeF stage = slide.stage;
        const QRectF r = object.rect.normalized();
        QPointF away;
        switch (step.direction) {
        case 1: away = {-(r.right() + 8.0), 0}; break;                 // from the left
        case 2: away = {stage.width() - r.left() + 8.0, 0}; break;     // from the right
        case 3: away = {0, -(r.bottom() + 8.0)}; break;                // from the top
        default: away = {0, stage.height() - r.top() + 8.0}; break;    // from the bottom
        }
        object.rect.translate(away * (1.0 - p));
        break;
    }
    case Effect::Float: {
        const qreal seen = step.phase == BuildPhase::Out ? 1.0 - step.linearAt(t) : step.linearAt(t);
        const qreal d = std::max(160.0, slide.stage.height() * 0.2);
        QPointF away = step.direction == 1 ? QPointF(-d, 0) : step.direction == 2 ? QPointF(d, 0)
                     : step.direction == 3 ? QPointF(0, -d) : QPointF(0, d);
        object.opacity *= seen;
        object.rect.translate(away * (1.0 - p));
        break;
    }
    case Effect::Zoom: {
        const qreal seen = step.phase == BuildPhase::Out ? 1.0 - step.linearAt(t) : step.linearAt(t);
        const qreal from = step.amount > 0 && step.amount < 1 ? step.amount : 0.3;
        object.opacity *= seen;
        object.paintScale *= from + (1.0 - from) * p;
        break;
    }
    case Effect::Turn: {
        const qreal seen = step.phase == BuildPhase::Out ? 1.0 - step.linearAt(t) : step.linearAt(t);
        object.opacity *= seen;
        object.rotation += (step.amount != 0 ? step.amount : 360.0) * (1.0 - p);
        object.paintScale *= 0.5 + 0.5 * p;
        break;
    }
    case Effect::Wipe: {
        const qreal f = qBound(0.0, p, 1.0);
        // The edge travels away from where it starts: "from bottom" shows the
        // bottom first.
        QRectF part(0, 0, 1, 1);
        switch (step.direction) {
        case 1: part = {0, 0, f, 1}; break;
        case 2: part = {1 - f, 0, f, 1}; break;
        case 3: part = {0, 0, 1, f}; break;
        default: part = {0, 1 - f, 1, f}; break;
        }
        if (f < 1.0) object.paintClip = object.paintClip.isNull() ? part : object.paintClip.intersected(part);
        if (f <= 0.0) object.opacity = 0.0;
        break;
    }
    case Effect::Grow:
    case Effect::Shrink: {
        const qreal target = step.amount > 0 ? step.amount : step.effect == Effect::Grow ? 1.25 : 0.8;
        object.paintScale *= 1.0 + (target - 1.0) * qBound(0.0, progress, 1.0);
        break;
    }
    case Effect::Reveal: {
        if (object.type != ObjectType::Text) break;
        const auto ends = unitEnds(object.text, qBound(0, step.unit, 2));
        if (object.textKind != 0) {
            // Equations and words on a path cannot be coloured by the letter.
            const int shown = int(std::ceil(p * ends.size() - 1e-9));
            object.text = shown <= 0 ? QString()
                                     : object.text.left(ends.at(qMin(shown, int(ends.size())) - 1));
            object.runs = TextRuns::tidy(object.runs, object.text.size());
            break;
        }
        // The whole text keeps its place from the first frame, so lines already
        // shown never move; each unit fades in over its share of the build.
        const qreal along = qBound(0.0, p, 1.0) * ends.size();
        const int whole = int(std::floor(along + 1e-9));
        if (whole >= ends.size()) break;
        const int shownEnd = whole > 0 ? ends.at(whole - 1) : 0;
        fadeText(object, shownEnd, ends.at(whole), along - whole);
        fadeText(object, ends.at(whole), object.text.size(), 0.0);
        break;
    }
    }
}

} // namespace

QVector<SceneObject> Evaluator::stateAt(const Slide &slide, qreal t) {
    QVector<SceneObject> states;
    states.reserve(slide.objects.size());

    const auto builds = slide.timeline.resolvedSteps();
    for (const SceneObject &source : slide.objects) {
        SceneObject state = source;
        for (const auto &step : builds)
            if (step.targetId == source.id) applyBuild(state, step, t, slide);
        if (!Timeline::visibleAt(builds, source.id, t)) state.opacity = 0.0;
        states.append(state);
    }
    Connector::resolve(states);
    return states;
}

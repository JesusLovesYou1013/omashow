#pragma once

// A build is one animation of one object, placed on a slide's timeline.
//
// The whole architecture rests on one rule: a build describes *what a property
// is at time t*, never *how to animate from now*. Nothing here starts, stops or
// ticks. That is what lets present mode, scrubbing and video export share a
// single evaluator, and what makes the result testable by asserting state at a
// given t.

#include <QEasingCurve>
#include <QString>
#include <QVector>

enum class Effect {
    None,
    Fade,   // opacity 0 -> 1
    Media = 3, // source playback on the same deterministic timeline
    Rise = 2,   // opacity 0 -> 1 while translating up into place
    Move = 4,   // travels in from, or out to, an offset
    Scale = 5,  // grows or shrinks into place
    Spin = 6,   // turns into place
    Pulse = 7,  // emphasis: swells and settles again, in place
    Reveal = 8, // text arriving by paragraph, word or character
    Path = 9,   // travelling along another object's outline
    // ANIM-001: entrances and exits that really bring the object in or take it
    // away (visibility is part of the effect), and emphases that hold. Kept as
    // new numbers so decks using 4, 5 and 6 above play exactly as before.
    Appear = 10, // instant
    Fly = 11,    // travels in from beyond the slide edge (direction), no fade
    Float = 12,  // fades while drifting a short way (direction)
    Zoom = 13,   // fades while growing from small
    Turn = 14,   // fades while turning a full circle ("Spin" in the gallery)
    Wipe = 15,   // revealed by a moving edge (direction)
    Grow = 16,   // emphasis: grows and stays bigger
    Shrink = 17, // emphasis: shrinks and stays smaller
};
constexpr int kLastEffect = 17;

// What an animation does to the object's presence, as the gallery groups it.
enum class EffectClass { Entrance, Emphasis, Exit, Motion, Media };

enum class BuildPhase { In, Out };
enum class BuildTrigger { Absolute, OnClick, WithPrevious, AfterPrevious };

struct BuildStep {
    QString targetId;
    Effect effect = Effect::Fade;
    qreal start = 0.0;       // seconds from the start of the slide
    qreal duration = 0.6;
    QEasingCurve::Type easing = QEasingCurve::OutCubic;

    BuildPhase phase = BuildPhase::In;
    BuildTrigger trigger = BuildTrigger::Absolute;
    qreal delay = 0.0;

    // What the effect needs beyond its timing: how far a Move travels, what a
    // Scale starts at, how far a Spin turns, how much a Pulse swells, and what
    // a Reveal counts as one step.
    qreal amountX = 0, amountY = 0;
    qreal amount = 0;
    int unit = 0;   // Reveal: 0 paragraphs, 1 words, 2 characters

    // Path: another object on the same slide whose outline is travelled. It is
    // referred to rather than copied, so editing the guide with the ordinary
    // node handles changes the motion, and hiding it keeps it out of the show.
    QString pathId;
    bool pathReverse = false, orient = false;

    // Fly, Float, Wipe: where it comes from (or goes to): 0 bottom, 1 left,
    // 2 right, 3 top.
    int direction = 0;

    qreal end() const { return start + duration; }

    // Whether the object is not there yet before this build runs. Media plays
    // in place, and an emphasis acts on something already on the slide.
    bool startsHidden() const {
        return phase == BuildPhase::In && effect != Effect::None &&
               effect != Effect::Media && effect != Effect::Pulse &&
               effect != Effect::Grow && effect != Effect::Shrink;
    }

    // 0 before the build, 1 after it, eased in between.
    qreal progressAt(qreal t) const;
    // The same, not eased: opacity follows this so a fade never "pops".
    qreal linearAt(qreal t) const;

    EffectClass effectClass() const;
    // Whether this step brings the object in (phase In) or takes it away
    // (phase Out) for the purpose of the object's visibility. The older
    // transforms (Move, Scale, Spin), Reveal and Path keep their own rules.
    bool changesVisibility() const;
};

struct Timeline {
    QVector<BuildStep> steps;

    QVector<BuildStep> resolvedSteps() const;
    qreal duration() const;
    QVector<const BuildStep *> stepsFor(const QString &targetId) const;

    // ANIM-001: the one visibility rule. An object whose first visibility
    // change is an entrance is hidden until that entrance starts; after an exit
    // ends it is hidden until a later entrance. `resolved` is resolvedSteps().
    static bool visibleAt(const QVector<BuildStep> &resolved, const QString &targetId, qreal t);
};

namespace Effects {
bool isEmphasis(Effect effect);
bool isNew(Effect effect);              // 10 and up: ANIM-001 effects
QString legacyName(Effect effect);      // what an older OmaShow should play instead
QEasingCurve::Type defaultEasing(Effect effect);
}

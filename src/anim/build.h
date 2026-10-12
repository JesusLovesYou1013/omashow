#pragma once

// A build is one animation of one object, placed on a slide's timeline.
//
// The whole architecture rests on one rule: a build describes *what a property
// is at time t*, never *how to animate from now*. Nothing here starts, stops or
// ticks. That is what lets present mode, scrubbing and video export share a
// single evaluator, and what makes the result testable by asserting state at a
// given t.

#include <QEasingCurve>
#include <QPainterPath>
#include <QSizeF>
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
    Rotate = 18, // emphasis: turns by `amount` degrees (360) and stays turned ("Spin")
};
constexpr int kLastEffect = 18;

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

    // ANIM-INTEROP: PowerPoint's timing options, played as PowerPoint plays them.
    qreal repeat = 0;          // 0 or 1: once; n > 1: n times; -1: until the slide moves on
    bool autoReverse = false;  // each time: forwards, then backwards
    bool rewind = false;       // when done, back to how it was before it started
    // Path: a PowerPoint motion path ("M 0 0 L 0.25 0 E"), in fractions of the
    // slide, moving the object from where it is (used instead of a guide object).
    QString motionPath;
    // Reveal: only these units, [rangeFrom, rangeTo) (PowerPoint's one effect per
    // paragraph). -1: the whole text.
    int rangeFrom = -1, rangeTo = -1;
    // A PowerPoint effect kept as PowerPoint wrote it, so saving back to
    // PowerPoint does not lose what OmaShow cannot play or describe. It is
    // re-emitted (with the current start, delay, duration and repeat) while
    // pptxSig still matches what it was imported as. pptxName is set when OmaShow
    // plays an approximation: what PowerPoint calls the effect.
    QString pptxXml, pptxSig, pptxName;
    // Its build-list entry as PowerPoint wrote it ("animBg=\"1\"", "build=\"p\"", ""),
    // "-" when it had none.
    QString pptxBuild;
    qreal pptxDuration = 0;
    QString interopSig() const;

    qreal cycle() const { return duration * (autoReverse ? 2.0 : 1.0); }
    // How long it runs before the next After previous may start.
    qreal span() const { return cycle() * (repeat > 1.0 ? repeat : 1.0); }
    qreal end() const { return start + span(); }

    // Whether the object is not there yet before this build runs. Media plays
    // in place, and an emphasis acts on something already on the slide.
    bool startsHidden() const {
        return phase == BuildPhase::In && effect != Effect::None &&
               effect != Effect::Media && effect != Effect::Pulse &&
               effect != Effect::Grow && effect != Effect::Shrink && effect != Effect::Rotate;
    }

    // 0 before the build, 1 after it, eased in between (repeats, reverses and
    // rewinds included).
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

// ANIM-INTEROP: PowerPoint's motion path text ("M 0 0 L 0.25 0 C ... E"):
// commands M L C Z E (lower case: relative), numbers in fractions of the slide.
namespace MotionPath {
QPainterPath parse(const QString &text, const QSizeF &stage);
// The other way: a path in slide units, written relative to `origin`.
QString write(const QPainterPath &path, const QPointF &origin, const QSizeF &stage);
}

namespace Effects {
bool isEmphasis(Effect effect);
bool isNew(Effect effect);              // 10 and up: ANIM-001 effects
QString legacyName(Effect effect);      // what an older OmaShow should play instead
QEasingCurve::Type defaultEasing(Effect effect);
// PowerPoint's Smooth start / Smooth end (accel / decel, 0..1 of the duration):
// velocity ramps linearly, so it is exactly a quadratic curve.
QEasingCurve::Type easingFromPpt(qreal accel, qreal decel);
// Which accel / decel to write for an easing (cubic curves map to the quadratic).
void pptFromEasing(QEasingCurve::Type easing, int &accel, int &decel);
}

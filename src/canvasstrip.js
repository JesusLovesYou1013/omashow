// OMASHOW-PERSONAL: continuous canvas (upstream #14 deferred)
.pragma library

// The edit canvas as one continuous strip of slides, like a PDF viewer, with
// the current slide the only editable one. Pure arithmetic, no QML, so it can
// be tested on its own (history/tests/qml/tst_canvasstrip.qml).
//
// Units: screen pixels of the canvas viewport (0 = its top edge), except
// `ext`, which is a slide's reach in slide units: {top: <= 0, bottom: >= the
// slide's height}, objects spilling past the slide included. `s` is the zoom
// (pixels per slide unit) and `top` a slide's top edge on screen.

var SNAP_FRACTION = 0.25

// Room between two slides' reaches. At least the viewport minus the slide, so
// with one slide centred no part of a neighbour shows; never less than minGap.
function gap(viewportH, slidePx, minGap) {
    return Math.max(minGap, viewportH - slidePx)
}

// Where the next / previous slide's top edge is on screen.
function nextTop(curTop, s, curExt, nextExt, gapPx) {
    return curTop + curExt.bottom * s + gapPx - nextExt.top * s
}
function prevTop(curTop, s, curExt, prevExt, gapPx) {
    return curTop + curExt.top * s - gapPx - prevExt.bottom * s
}

// A slide's reach on screen.
function region(top, s, ext) {
    return { top: top + ext.top * s, bottom: top + ext.bottom * s }
}

// How much of a reach is showing: of the reach itself, or of the viewport when
// the reach is taller than it (a huge overflow could never show 25 % of itself).
function visibleFraction(top, bottom, viewportH) {
    const h = bottom - top
    if (h <= 0 || viewportH <= 0) return 0
    const shown = Math.max(0, Math.min(bottom, viewportH) - Math.max(top, 0))
    return shown / Math.min(h, viewportH)
}

// +1 / -1 to move to the next / previous slide, 0 to stay. g = {curTop, s,
// viewportH, gap, curExt, prevExt (or null), nextExt (or null)}.
function snapDirection(g) {
    if (g.nextExt) {
        const r = region(nextTop(g.curTop, g.s, g.curExt, g.nextExt, g.gap), g.s, g.nextExt)
        if (visibleFraction(r.top, r.bottom, g.viewportH) > SNAP_FRACTION) return 1
    }
    if (g.prevExt) {
        const r = region(prevTop(g.curTop, g.s, g.curExt, g.prevExt, g.gap), g.s, g.prevExt)
        if (visibleFraction(r.top, r.bottom, g.viewportH) > SNAP_FRACTION) return -1
    }
    return 0
}

// The top edge a slide has when centred.
function centredTop(viewportH, slidePx) {
    return (viewportH - slidePx) / 2
}

// At the first or last slide there is nothing to scroll to: the reach may come
// in no further than `margin` from that edge, and a reach that fits stops
// where it is centred. Sides with a neighbour are left alone (the snap
// handles them). Returns the allowed top.
function clampTop(curTop, s, viewportH, slideH, curExt, hasPrev, hasNext, margin) {
    const centred = centredTop(viewportH, slideH * s)
    let top = curTop
    if (!hasPrev) top = Math.min(top, Math.max(centred, margin - curExt.top * s))
    if (!hasNext) top = Math.max(top, Math.min(centred, viewportH - margin - curExt.bottom * s))
    return top
}


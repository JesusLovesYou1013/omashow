import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// FILL-UI-1: Fill and Line for the selected shapes, or a text box's own box,
// in the Style tab: PowerPoint's Format Shape > Fill & Line. Laid out as the
// Effects (EffectsSet.qml): each a collapsed row until opened, presets first.
//
// Fill: No fill, Solid (colour, transparency), Gradient (any number of stops,
// each with colour, position, transparency, brightness; Linear, Radial,
// Rectangular or Path; direction presets and angle; rotate with shape),
// Picture or texture (stretch with offsets, or tile with offsets, scale,
// alignment and mirror; transparency; rotate with shape) and Pattern (all 54
// of PowerPoint's, foreground and background). Line: No line, Solid or
// Gradient; colour, transparency, width, compound type, dash type (every
// preset; custom dashes are kept), cap and join.
//
// A text box takes a fill and a line of its own (boxDrawn: one p:sp in
// PowerPoint); the first one starts its box. Every change is one undo step
// (backend.setSelectedFill).
ColumnLayout {
    id: root
    objectName: "fillPanel"
    spacing: Theme.s2

    property bool fillOpen: false
    property bool lineOpen: false
    property bool fillPresetsOpen: false
    property bool patternsOpen: true

    readonly property var st: { backend.selection; return backend.fillState() }
    readonly property bool onText: st.target === "text"
    readonly property bool connector: st.connector ?? false
    readonly property real unitsPerPt: backend.ptPerUnit > 0 ? 1 / backend.ptPerUnit : 1
    visible: backend.hasSelection && (st.count ?? 0) > 0

    function alphaOf(c) { const s = String(c ?? ""); return s.length === 9 ? parseInt(s.substr(1, 2), 16) / 255 : 1 }
    function rgbOf(c) { const s = String(c ?? ""); return s.length === 9 ? "#" + s.substr(3) : s }
    function withAlpha(rgb, alpha) {
        const a = Math.max(0, Math.min(255, Math.round(alpha * 255))).toString(16).padStart(2, "0")
        const s = String(rgb)
        return "#" + a + (s.length === 9 ? s.substr(3) : s.substr(1))
    }
    function pts(units) { return (units ?? 0) / unitsPerPt }
    function units(points) { return (points ?? 0) * unitsPerPt }
    function round1(v) { return Math.round(v * 10) / 10 }
    function set(values) { backend.setSelectedFill(values) }
    function preview(values) { return "image://effects/fill?p=" + encodeURIComponent(JSON.stringify(values)) }

    // ---- what is set ----------------------------------------------------------
    readonly property int fillStyle: st.fillStyle ?? 5
    // The Type list: No fill, Solid, Gradient, Picture or texture, Pattern.
    readonly property int fillType: fillStyle === 5 ? 0 : fillStyle === 0 ? 1 : fillStyle <= 2 ? 2 : fillStyle === 4 ? 3 : 4
    // Gradient type: Linear, Radial, Rectangular, Path.
    readonly property int gradientType: fillStyle === 1 ? 0 : 1 + (st.gradientPath ?? 0)
    readonly property bool lineOn: (st.strokeWidth ?? 0) > 0
    readonly property int lineType: !lineOn ? 0 : (st.strokeGradient ?? false) ? 2 : 1

    readonly property var typeNames: [qsTr("No fill"), qsTr("Solid fill"), qsTr("Gradient fill"), qsTr("Picture or texture fill"), qsTr("Pattern fill")]
    readonly property var lineNames: [qsTr("No line"), qsTr("Solid line"), qsTr("Gradient line")]
    readonly property var gradientNames: [qsTr("Linear"), qsTr("Radial"), qsTr("Rectangular"), qsTr("Path")]
    readonly property var dashNames: ({ solid: qsTr("Solid"), sysDot: qsTr("Round dot"), sysDash: qsTr("Square dot"), dash: qsTr("Dash"),
                                        dashDot: qsTr("Dash dot"), lgDash: qsTr("Long dash"), lgDashDot: qsTr("Long dash dot"),
                                        lgDashDotDot: qsTr("Long dash dot dot"), dot: qsTr("Dot"), sysDashDot: qsTr("Square dash dot"),
                                        sysDashDotDot: qsTr("Square dash dot dot"), custom: qsTr("Custom") })
    readonly property var dashes: backend.lineDashPresets()
    readonly property var patterns: backend.fillPatternPresets()
    readonly property var patternNames: ({
        pct5: qsTr("5%"), pct10: qsTr("10%"), pct20: qsTr("20%"), pct25: qsTr("25%"), pct30: qsTr("30%"), pct40: qsTr("40%"),
        pct50: qsTr("50%"), pct60: qsTr("60%"), pct70: qsTr("70%"), pct75: qsTr("75%"), pct80: qsTr("80%"), pct90: qsTr("90%"),
        horz: qsTr("Horizontal"), vert: qsTr("Vertical"), ltHorz: qsTr("Light horizontal"), ltVert: qsTr("Light vertical"),
        dkHorz: qsTr("Dark horizontal"), dkVert: qsTr("Dark vertical"), narHorz: qsTr("Narrow horizontal"), narVert: qsTr("Narrow vertical"),
        dashHorz: qsTr("Dashed horizontal"), dashVert: qsTr("Dashed vertical"), cross: qsTr("Cross"), dnDiag: qsTr("Downward diagonal"),
        upDiag: qsTr("Upward diagonal"), ltDnDiag: qsTr("Light downward diagonal"), ltUpDiag: qsTr("Light upward diagonal"),
        dkDnDiag: qsTr("Dark downward diagonal"), dkUpDiag: qsTr("Dark upward diagonal"), wdDnDiag: qsTr("Wide downward diagonal"),
        wdUpDiag: qsTr("Wide upward diagonal"), dashDnDiag: qsTr("Dashed downward diagonal"), dashUpDiag: qsTr("Dashed upward diagonal"),
        diagCross: qsTr("Diagonal cross"), smCheck: qsTr("Small checkerboard"), lgCheck: qsTr("Large checkerboard"),
        smGrid: qsTr("Small grid"), lgGrid: qsTr("Large grid"), dotGrid: qsTr("Dotted grid"), smConfetti: qsTr("Small confetti"),
        lgConfetti: qsTr("Large confetti"), horzBrick: qsTr("Horizontal brick"), diagBrick: qsTr("Diagonal brick"),
        solidDmnd: qsTr("Solid diamond"), openDmnd: qsTr("Outlined diamond"), dotDmnd: qsTr("Dotted diamond"), plaid: qsTr("Plaid"),
        sphere: qsTr("Sphere"), weave: qsTr("Weave"), divot: qsTr("Divot"), shingle: qsTr("Shingle"), wave: qsTr("Wave"),
        trellis: qsTr("Trellis"), zigZag: qsTr("Zig zag") })

    // The Office theme's accents 1-6 (as the Effects presets use).
    readonly property var accents: ["#156082", "#E97132", "#196B24", "#0F9ED5", "#A02B93", "#4EA72E"]
    function accentStops(accent, kind) {
        const c = withAlpha(accent, 1)
        if (kind === 0) return [{ p: 0, c: c, b: 0.95 }, { p: 0.74, c: c, b: 0.55 }, { p: 0.83, c: c, b: 0.55 }, { p: 1, c: c, b: 0.7 }]
        if (kind === 1) return [{ p: 0, c: c, b: 0.5 }, { p: 1, c: c, b: 0 }]
        if (kind === 2) return [{ p: 0, c: c, b: 0 }, { p: 1, c: c, b: -0.5 }]
        return [{ p: 0, c: c, b: 0.6 }, { p: 1, c: c, b: 0 }]
    }
    // Variations of the accents (Light, Medium, Dark, Radial; PowerPoint's
    // own use the theme's colour transforms, approximated here) and some of
    // the older named presets.
    readonly property var gradientPresets: {
        const out = []
        const kinds = [qsTr("Light gradient"), qsTr("Medium gradient"), qsTr("Dark gradient"), qsTr("Radial gradient")]
        for (let k = 0; k < 4; ++k)
            for (let a = 0; a < accents.length; ++a)
                out.push({ name: qsTr("%1 - accent %2").arg(kinds[k]).arg(a + 1),
                           v: k < 3 ? { fillStyle: 1, fillAngle: 90, gradientStops: accentStops(accents[a], k) }
                                    : { fillStyle: 2, gradientPath: 0, gradientFocusL: 0.5, gradientFocusT: 0.5, gradientFocusR: 0.5, gradientFocusB: 0.5, gradientStops: accentStops(accents[a], k) } })
        const named = [
            [qsTr("Rainbow"), 0, [[0, "#A603AB"], [0.21, "#0819FB"], [0.35, "#1A8D48"], [0.52, "#FFFF00"], [0.73, "#EE3F17"], [0.88, "#E81766"], [1, "#A603AB"]]],
            [qsTr("Fire"), 90, [[0, "#FFF200"], [0.45, "#FF7A00"], [0.7, "#FF0300"], [1, "#4D0808"]]],
            [qsTr("Ocean"), 90, [[0, "#03D4A8"], [0.25, "#21D6E0"], [0.75, "#0087E6"], [1, "#005CBF"]]],
            [qsTr("Gold"), 90, [[0, "#E6DCAC"], [0.12, "#E6D78A"], [0.3, "#C7AC4C"], [0.45, "#E6D78A"], [0.77, "#C7AC4C"], [1, "#E6DCAC"]]],
            [qsTr("Chrome"), 90, [[0, "#FFFFFF"], [0.16, "#1F1F1F"], [0.18, "#FFFFFF"], [0.42, "#636363"], [0.53, "#CFCFCF"], [0.66, "#CFCFCF"], [0.76, "#1F1F1F"], [0.79, "#FFFFFF"], [0.9, "#7F7F7F"], [1, "#FFFFFF"]]],
            [qsTr("Silver"), 90, [[0, "#FFFFFF"], [0.07, "#E6E6E6"], [0.32, "#7D8496"], [0.48, "#E6E6E6"], [0.89, "#7D8496"], [1, "#FFFFFF"]]]]
        for (const n of named)
            out.push({ name: n[0], v: { fillStyle: 1, fillAngle: n[1], gradientStops: n[2].map(s => ({ p: s[0], c: withAlpha(s[1], 1), b: 0 })) } })
        return out
    }
    readonly property var linearDirections: [
        { name: qsTr("Linear diagonal: top left to bottom right"), a: 45 }, { name: qsTr("Linear down"), a: 90 },
        { name: qsTr("Linear diagonal: top right to bottom left"), a: 135 }, { name: qsTr("Linear right"), a: 0 },
        { name: qsTr("Linear left"), a: 180 }, { name: qsTr("Linear diagonal: bottom left to top right"), a: 315 },
        { name: qsTr("Linear up"), a: 270 }, { name: qsTr("Linear diagonal: bottom right to top left"), a: 225 }]
    readonly property var focusDirections: [
        { name: qsTr("From bottom right corner"), f: [1, 1, 0, 0] }, { name: qsTr("From bottom left corner"), f: [0, 1, 1, 0] },
        { name: qsTr("From centre"), f: [0.5, 0.5, 0.5, 0.5] }, { name: qsTr("From top right corner"), f: [1, 0, 0, 1] },
        { name: qsTr("From top left corner"), f: [0, 0, 1, 1] }]
    readonly property var stopsNow: st.gradientStops ?? []

    // ---- pieces (as in EffectsSet.qml) ----------------------------------------
    component Expander: Button {
        property bool open: false
        property string summary: ""
        property string title: ""
        Layout.fillWidth: true
        flat: true
        icon.name: open ? "chevron-down" : "chevron-right"
        text: summary ? title + " · " + summary : title
        Accessible.name: title
    }
    component Tile: Rectangle {
        id: tile
        property string source: ""
        property string tip: ""
        property bool current: false
        signal chosen()
        implicitWidth: Theme.hControl + Theme.s4
        implicitHeight: implicitWidth
        radius: Theme.rControl
        color: tileHover.hovered ? Theme.controlHover : Theme.controlBg
        border.width: current ? Theme.selectionRing : Theme.hairline
        border.color: current ? Theme.accent : tileHover.hovered ? Theme.borderStrong : Theme.border
        Accessible.role: Accessible.Button
        Accessible.name: tip
        Image {
            anchors.fill: parent
            anchors.margins: Theme.hairline
            source: tile.source
            sourceSize: Qt.size(width * 2, height * 2)
            asynchronous: true
            cache: true
            visible: tile.source !== ""
        }
        HoverHandler { id: tileHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: tile.chosen() }
        ToolTip.visible: tileHover.hovered && tip !== ""
        ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: tip
    }
    component Percent: Rectangle {
        property real value: 0
        implicitWidth: Theme.s5 * 2 + Theme.s2
        implicitHeight: Theme.hControl
        radius: Theme.rControl
        color: Theme.controlBg
        border.color: Theme.border; border.width: Theme.hairline
        Label { anchors.centerIn: parent; text: Math.round(parent.value) + "%"; font.family: Theme.monoFamily }
    }
    component Transparency: FieldRow {
        id: tr
        property string colour: "#ff000000"
        property string name: ""
        signal picked(real alpha)
        label: qsTr("Transparency")
        Slider {
            Layout.fillWidth: true
            from: 0; to: 100
            value: Math.round((1 - root.alphaOf(tr.colour)) * 100)
            onPressedChanged: if (!pressed) tr.picked(1 - value / 100)
            Accessible.name: tr.name
        }
        Percent { value: (1 - root.alphaOf(tr.colour)) * 100 }
    }

    SectionLabel { text: root.onText ? qsTr("BOX FILL AND LINE") : qsTr("FILL AND LINE") }

    // ---- Fill -------------------------------------------------------------------
    Expander {
        objectName: "fillExpander"
        visible: !root.connector
        title: qsTr("Fill")
        open: root.fillOpen
        summary: root.typeNames[root.fillType] + (root.fillType === 2 ? " · " + root.gradientNames[root.gradientType] + " · " + qsTr("%1 stops").arg(root.stopsNow.length)
                                                 : root.fillType === 4 ? " · " + (root.patternNames[root.st.patternPreset] ?? root.st.patternPreset) : "")
        onClicked: root.fillOpen = !root.fillOpen
    }
    ColumnLayout {
        objectName: "fillSection"
        visible: root.fillOpen && !root.connector
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s2
        spacing: Theme.s2

        Expander {
            objectName: "fillPresets"
            title: qsTr("Presets")
            open: root.fillPresetsOpen
            onClicked: root.fillPresetsOpen = !root.fillPresetsOpen
        }
        GridLayout {
            visible: root.fillPresetsOpen
            columns: root.accents.length
            columnSpacing: Theme.s1
            rowSpacing: Theme.s1
            Repeater {
                model: root.gradientPresets
                delegate: Tile {
                    required property var modelData
                    source: root.preview(modelData.v)
                    tip: modelData.name
                    current: root.fillType === 2 && JSON.stringify(root.stopsNow.map(s => [s.p, s.c.toLowerCase(), s.b ?? 0])) === JSON.stringify(modelData.v.gradientStops.map(s => [s.p, s.c.toLowerCase(), s.b ?? 0]))
                    onChosen: root.set(modelData.v)
                }
            }
        }
        FieldRow {
            label: qsTr("Type")
            ComboBox {
                objectName: "fillType"
                Layout.fillWidth: true
                model: root.typeNames
                currentIndex: root.fillType
                onActivated: {
                    if (currentIndex === 0) root.set({ fillStyle: 5 })
                    else if (currentIndex === 1) root.set({ fillStyle: 0, fill: root.fillStyle === 5 ? "#ff156082" : root.st.fill })
                    else if (currentIndex === 2) root.set({ fillStyle: 1, gradientStops: root.stopsNow.length >= 2 ? root.stopsNow : root.accentStops(root.accents[0], 0), fillAngle: root.st.fillAngle ?? 90 })
                    else if (currentIndex === 3) { if (root.st.hasPicture) root.set({ fillStyle: 4 }); else backend.setShapeImageDialog() }
                    else root.set({ fillStyle: 3, patternPreset: root.st.patternPreset || "pct5" })
                }
            }
        }

        // Solid
        FieldRow {
            visible: root.fillType === 1
            label: qsTr("Colour")
            ColorField {
                objectName: "fillColour"
                value: root.rgbOf(root.st.fill)
                accessibleName: qsTr("Fill colour")
                onPicked: v => root.set({ fill: root.withAlpha(v, root.alphaOf(root.st.fill)) })
            }
        }
        Transparency {
            objectName: "fillTransparency"
            visible: root.fillType === 1
            colour: root.st.fill ?? "#ff000000"
            name: qsTr("Fill transparency")
            onPicked: a => root.set({ fill: root.withAlpha(root.rgbOf(root.st.fill), a) })
        }

        // Gradient
        FieldRow {
            visible: root.fillType === 2
            label: qsTr("Gradient")
            ComboBox {
                objectName: "gradientType"
                Layout.fillWidth: true
                model: root.gradientNames
                currentIndex: root.gradientType
                onActivated: root.set(currentIndex === 0 ? { fillStyle: 1 } : { fillStyle: 2, gradientPath: currentIndex - 1 })
            }
        }
        Label { visible: root.fillType === 2; text: qsTr("Direction"); color: Theme.textSecondary; font.pixelSize: Theme.fsLabel }
        Flow {
            visible: root.fillType === 2
            Layout.fillWidth: true
            spacing: Theme.s1
            Repeater {
                model: root.gradientType === 0 ? root.linearDirections : root.gradientType === 3 ? [root.focusDirections[2]] : root.focusDirections
                delegate: Tile {
                    required property var modelData
                    readonly property var values: root.gradientType === 0 ? { fillAngle: modelData.a }
                        : { gradientFocusL: modelData.f[0], gradientFocusT: modelData.f[1], gradientFocusR: modelData.f[2], gradientFocusB: modelData.f[3] }
                    implicitWidth: Theme.hControl + Theme.s2
                    source: root.preview(Object.assign({ fillStyle: root.fillStyle, gradientPath: root.st.gradientPath ?? 0, fillAngle: root.st.fillAngle ?? 0,
                                                         gradientStops: root.stopsNow }, values))
                    tip: modelData.name
                    current: root.gradientType === 0 ? Math.abs((root.st.fillAngle ?? 0) - modelData.a) < 0.5
                             : Math.abs((root.st.gradientFocusL ?? .5) - modelData.f[0]) < 0.01 && Math.abs((root.st.gradientFocusT ?? .5) - modelData.f[1]) < 0.01
                    onChosen: root.set(values)
                }
            }
        }
        FieldRow {
            visible: root.fillType === 2 && root.gradientType === 0
            label: qsTr("Angle")
            NumField {
                objectName: "gradientAngle"
                Layout.fillWidth: true
                suffix: "°"; step: 1; decimals: 1
                value: root.round1(root.st.fillAngle ?? 0)
                onCommitted: v => root.set({ fillAngle: ((v % 360) + 360) % 360 })
            }
        }
        GradientStops {
            objectName: "fillStops"
            visible: root.fillType === 2
            Layout.fillWidth: true
            stops: root.stopsNow
            onEdited: list => root.set({ gradientStops: list })
        }
        CheckBox {
            objectName: "gradientRotate"
            visible: root.fillType === 2
            text: qsTr("Rotate with shape")
            checked: root.st.gradientRotate ?? true
            onClicked: root.set({ gradientRotate: checked })
        }

        // Picture or texture
        Button {
            objectName: "fillPictureChoose"
            visible: root.fillType === 3
            Layout.fillWidth: true
            text: root.st.hasPicture ? qsTr("Replace picture…") : qsTr("Choose picture…")
            onClicked: backend.setShapeImageDialog()
        }
        FieldRow {
            visible: root.fillType === 3
            label: qsTr("Transparency")
            Slider {
                objectName: "fillPictureTransparency"
                Layout.fillWidth: true
                from: 0; to: 100
                value: Math.round((root.st.pictureTransparency ?? 0) * 100)
                onPressedChanged: if (!pressed) root.set({ pictureTransparency: value / 100 })
                Accessible.name: qsTr("Picture transparency")
            }
            Percent { value: (root.st.pictureTransparency ?? 0) * 100 }
        }
        CheckBox {
            objectName: "fillPictureTile"
            visible: root.fillType === 3
            text: qsTr("Tile picture as texture")
            checked: root.st.pictureTile ?? false
            onClicked: root.set({ pictureTile: checked })
        }
        FieldRow {
            visible: root.fillType === 3 && !(root.st.pictureTile ?? false)
            label: qsTr("Placement")
            ComboBox {
                objectName: "fillPictureMode"
                Layout.fillWidth: true
                model: [qsTr("Fit picture"), qsTr("Fill shape"), qsTr("Stretch with offsets")]
                currentIndex: root.st.imageMode ?? 0
                onActivated: root.set({ imageMode: currentIndex })
            }
        }
        GridLayout {
            visible: root.fillType === 3 && !(root.st.pictureTile ?? false) && (root.st.imageMode ?? 0) === 2
            columns: 2
            columnSpacing: Theme.s2
            Layout.fillWidth: true
            Repeater {
                model: [["pictureInsetL", qsTr("Offset left")], ["pictureInsetR", qsTr("Offset right")], ["pictureInsetT", qsTr("Offset top")], ["pictureInsetB", qsTr("Offset bottom")]]
                delegate: NumField {
                    required property var modelData
                    Layout.fillWidth: true
                    label: modelData[1]; suffix: " %"; step: 1
                    value: Math.round((root.st[modelData[0]] ?? 0) * 100)
                    onCommitted: v => { const o = {}; o[modelData[0]] = Math.max(-1000, Math.min(1000, v)) / 100; root.set(o) }
                }
            }
        }
        GridLayout {
            visible: root.fillType === 3 && (root.st.pictureTile ?? false)
            columns: 2
            columnSpacing: Theme.s2
            Layout.fillWidth: true
            NumField { objectName: "fillTileX"; Layout.fillWidth: true; label: qsTr("Offset X"); suffix: " pt"; step: 1; decimals: 1
                       value: root.round1(root.pts(root.st.pictureTileX)); onCommitted: v => root.set({ pictureTileX: root.units(v) }) }
            NumField { objectName: "fillTileY"; Layout.fillWidth: true; label: qsTr("Offset Y"); suffix: " pt"; step: 1; decimals: 1
                       value: root.round1(root.pts(root.st.pictureTileY)); onCommitted: v => root.set({ pictureTileY: root.units(v) }) }
            NumField { objectName: "fillTileScaleX"; Layout.fillWidth: true; label: qsTr("Scale X"); suffix: " %"; step: 1
                       value: Math.round(root.st.pictureTileScaleX ?? 100); onCommitted: v => root.set({ pictureTileScaleX: Math.max(1, Math.min(1000, v)) }) }
            NumField { objectName: "fillTileScaleY"; Layout.fillWidth: true; label: qsTr("Scale Y"); suffix: " %"; step: 1
                       value: Math.round(root.st.pictureTileScaleY ?? 100); onCommitted: v => root.set({ pictureTileScaleY: Math.max(1, Math.min(1000, v)) }) }
        }
        FieldRow {
            visible: root.fillType === 3 && (root.st.pictureTile ?? false)
            label: qsTr("Alignment")
            ComboBox {
                objectName: "fillTileAlign"
                Layout.fillWidth: true
                model: [qsTr("Top left"), qsTr("Top"), qsTr("Top right"), qsTr("Left"), qsTr("Centre"), qsTr("Right"), qsTr("Bottom left"), qsTr("Bottom"), qsTr("Bottom right")]
                currentIndex: root.st.pictureTileAlign ?? 0
                onActivated: root.set({ pictureTileAlign: currentIndex })
            }
        }
        FieldRow {
            visible: root.fillType === 3 && (root.st.pictureTile ?? false)
            label: qsTr("Mirror type")
            ComboBox {
                objectName: "fillTileMirror"
                Layout.fillWidth: true
                model: [qsTr("None"), qsTr("Horizontal"), qsTr("Vertical"), qsTr("Both")]
                currentIndex: root.st.pictureTileFlip ?? 0
                onActivated: root.set({ pictureTileFlip: currentIndex })
            }
        }
        CheckBox {
            visible: root.fillType === 3
            text: qsTr("Rotate with shape")
            checked: root.st.pictureRotate ?? true
            onClicked: root.set({ pictureRotate: checked })
        }

        // Pattern
        Label { visible: root.fillType === 4; text: qsTr("Pattern"); color: Theme.textSecondary; font.pixelSize: Theme.fsLabel }
        GridLayout {
            objectName: "fillPatterns"
            visible: root.fillType === 4
            columns: 8
            columnSpacing: Theme.s1
            rowSpacing: Theme.s1
            Repeater {
                model: root.patterns
                delegate: Tile {
                    required property string modelData
                    implicitWidth: Theme.hControl
                    source: root.fillType === 4 ? root.preview({ fillStyle: 3, patternPreset: modelData, fill: root.st.fill, fillSecondary: root.st.fillSecondary }) : ""
                    tip: root.patternNames[modelData] ?? modelData
                    current: root.st.patternPreset === modelData
                    onChosen: root.set({ patternPreset: modelData })
                }
            }
        }
        FieldRow {
            visible: root.fillType === 4
            label: qsTr("Foreground")
            ColorField {
                value: root.rgbOf(root.st.fill)
                accessibleName: qsTr("Pattern foreground colour")
                onPicked: v => root.set({ fill: root.withAlpha(v, root.alphaOf(root.st.fill)) })
            }
        }
        FieldRow {
            visible: root.fillType === 4
            label: qsTr("Background")
            ColorField {
                value: root.rgbOf(root.st.fillSecondary)
                accessibleName: qsTr("Second fill colour")
                onPicked: v => root.set({ fillSecondary: root.withAlpha(v, root.alphaOf(root.st.fillSecondary)) })
            }
        }
    }

    // ---- Line -------------------------------------------------------------------
    Expander {
        objectName: "lineExpander"
        title: qsTr("Line")
        open: root.lineOpen
        summary: root.lineNames[root.lineType] + (root.lineOn ? " · " + qsTr("%1 pt").arg(root.round1(root.pts(root.st.strokeWidth))) + " · " + (root.dashNames[root.st.strokeDash] ?? root.st.strokeDash) : "")
        onClicked: root.lineOpen = !root.lineOpen
    }
    ColumnLayout {
        objectName: "lineSection"
        visible: root.lineOpen
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s2
        spacing: Theme.s2
        FieldRow {
            label: qsTr("Type")
            ComboBox {
                objectName: "lineType"
                Layout.fillWidth: true
                model: root.lineNames
                currentIndex: root.lineType
                onActivated: {
                    const w = root.lineOn ? root.st.strokeWidth : root.units(0.75)
                    if (currentIndex === 0) root.set({ strokeWidth: 0 })
                    else if (currentIndex === 1) root.set({ strokeWidth: w, strokeGradient: false })
                    else root.set({ strokeWidth: w, strokeGradient: true,
                                    strokeStops: (root.st.strokeStops ?? []).length >= 2 && root.st.strokeGradient ? root.st.strokeStops : root.accentStops(root.accents[0], 2) })
                }
            }
        }
        FieldRow {
            visible: root.lineType === 1
            label: qsTr("Colour")
            ColorField {
                objectName: "lineColour"
                value: root.rgbOf(root.st.strokeColor)
                accessibleName: qsTr("Stroke colour")
                onPicked: v => root.set({ strokeColor: root.withAlpha(v, root.alphaOf(root.st.strokeColor)), strokeWidth: root.lineOn ? root.st.strokeWidth : root.units(0.75) })
            }
        }
        Transparency {
            visible: root.lineType === 1
            colour: root.st.strokeColor ?? "#ff000000"
            name: qsTr("Line transparency")
            onPicked: a => root.set({ strokeColor: root.withAlpha(root.rgbOf(root.st.strokeColor), a) })
        }
        FieldRow {
            visible: root.lineType === 2
            label: qsTr("Gradient")
            ComboBox {
                objectName: "lineGradientType"
                Layout.fillWidth: true
                model: root.gradientNames
                currentIndex: root.st.strokeGradientPath ?? 0
                onActivated: root.set({ strokeGradientPath: currentIndex })
            }
        }
        FieldRow {
            visible: root.lineType === 2 && (root.st.strokeGradientPath ?? 0) === 0
            label: qsTr("Angle")
            NumField {
                Layout.fillWidth: true
                suffix: "°"; step: 1; decimals: 1
                value: root.round1(root.st.strokeAngle ?? 0)
                onCommitted: v => root.set({ strokeAngle: ((v % 360) + 360) % 360 })
            }
        }
        GradientStops {
            objectName: "lineStops"
            visible: root.lineType === 2
            Layout.fillWidth: true
            stops: root.st.strokeStops ?? []
            onEdited: list => root.set({ strokeStops: list })
        }
        FieldRow {
            visible: root.lineOn
            label: qsTr("Width")
            NumField {
                objectName: "lineWidth"
                Layout.fillWidth: true
                suffix: " pt"; step: 0.25; decimals: 2
                value: Math.round(root.pts(root.st.strokeWidth) * 100) / 100
                onCommitted: v => root.set({ strokeWidth: root.units(Math.max(0, Math.min(1584, v))) })
            }
        }
        FieldRow {
            visible: root.lineOn
            label: qsTr("Compound type")
            ComboBox {
                objectName: "lineCompound"
                Layout.fillWidth: true
                model: [qsTr("Single"), qsTr("Double"), qsTr("Thick thin"), qsTr("Thin thick"), qsTr("Triple")]
                currentIndex: root.st.strokeCompound ?? 0
                onActivated: root.set({ strokeCompound: currentIndex })
            }
        }
        FieldRow {
            visible: root.lineOn
            label: qsTr("Dash type")
            ComboBox {
                objectName: "lineDash"
                Layout.fillWidth: true
                readonly property var names: root.st.strokeDash === "custom" ? root.dashes.concat(["custom"]) : root.dashes
                model: names.map(d => root.dashNames[d] ?? d)
                currentIndex: Math.max(0, names.indexOf(root.st.strokeDash ?? "solid"))
                onActivated: if (names[currentIndex] !== "custom") root.set({ strokeDash: names[currentIndex] })
            }
        }
        Flow {
            visible: root.lineOn
            Layout.fillWidth: true
            spacing: Theme.s1
            Repeater {
                model: root.dashes
                delegate: Tile {
                    required property string modelData
                    implicitWidth: Theme.hControl + Theme.s2
                    implicitHeight: Theme.hControl
                    source: root.lineOpen ? root.preview({ strokeWidth: root.units(2), strokeColor: "#ffd8dce2", strokeDash: modelData, strokeCap: modelData === "sysDot" ? 2 : 0 }) : ""
                    tip: root.dashNames[modelData] ?? modelData
                    current: root.st.strokeDash === modelData
                    onChosen: root.set(modelData === "sysDot" ? { strokeDash: modelData, strokeCap: 2 } : { strokeDash: modelData })
                }
            }
        }
        FieldRow {
            visible: root.lineOn
            label: qsTr("Cap type")
            ComboBox {
                objectName: "lineCap"
                Layout.fillWidth: true
                model: [qsTr("Flat"), qsTr("Square"), qsTr("Round")]
                currentIndex: root.st.strokeCap ?? 1
                onActivated: root.set({ strokeCap: currentIndex })
            }
        }
        FieldRow {
            visible: root.lineOn
            label: qsTr("Join type")
            ComboBox {
                objectName: "lineJoin"
                Layout.fillWidth: true
                model: [qsTr("Miter"), qsTr("Bevel"), qsTr("Round")]
                currentIndex: root.st.strokeJoin ?? 1
                onActivated: root.set({ strokeJoin: currentIndex })
            }
        }
    }
}

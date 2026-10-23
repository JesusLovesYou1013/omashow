import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// BOTH-EFFECTS-1: one set of Glow and Shadow settings (EFFECTS-UI-1), for
// one target: "text" (the words, a:rPr) or "shape" (the box: a shape or
// picture's own p:spPr effects, or a text box's own when it has no shape;
// backend.effectState says which). EffectsPanel shows one, or two for Both.
ColumnLayout {
    id: root
    objectName: "effectsSet"
    spacing: Theme.s2

    property string target: "text"
    // A heading over the set (Both shows "Text effects" and "Box effects").
    property string heading: ""
    property bool glowOpen: false
    property bool shadowOpen: false
    property bool glowPresetsOpen: false
    property bool shadowPresetsOpen: false

    readonly property var fx: { backend.selection; return backend.effectState(target) }
    readonly property bool onText: target === "text"
    // The box's effects land on the words: a text box with no fill and no outline.
    readonly property bool onWords: !onText && (fx.onWords ?? false)
    // Slide units per point (the inspector shows points).
    readonly property real unitsPerPt: backend.ptPerUnit > 0 ? 1 / backend.ptPerUnit : 1

    // ---- values, in what the settings show ---------------------------------

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

    readonly property bool glowOn: (fx.glowRadius ?? 0) > 0
    readonly property bool shadowOn: fx.shadowEnabled ?? false
    readonly property real shadowDistance: pts(Math.hypot(fx.shadowX ?? 0, fx.shadowY ?? 0))
    readonly property real shadowAngle: {
        const a = Math.atan2(fx.shadowY ?? 0, fx.shadowX ?? 0) * 180 / Math.PI
        return Math.round(((a % 360) + 360) % 360)
    }
    readonly property bool perspective: shadowOn && !(fx.shadowInner ?? false) &&
                                        ((fx.shadowSkewX ?? 0) !== 0 || (fx.shadowSkewY ?? 0) !== 0 || (fx.shadowScaleX ?? 100) !== (fx.shadowScaleY ?? 100))

    function set(values) { backend.setSelectedEffect(root.target, values) }
    function setShadowVector(distancePt, angleDeg) {
        const d = units(Math.max(0, distancePt)), r = angleDeg * Math.PI / 180
        set({ shadowEnabled: true, shadowX: d * Math.cos(r), shadowY: d * Math.sin(r) })
    }

    // ---- presets (PowerPoint's own values; see history/CHANGELOG EFFECTS-UI-1) ----

    // The Office theme's accents 1-6 (PowerPoint 365's default theme), the colours of
    // its Glow Variations there. A deck's own theme accents aren't kept apart (only its first).
    readonly property var accents: ["#156082", "#E97132", "#196B24", "#0F9ED5", "#A02B93", "#4EA72E"]
    readonly property var glowSizes: [5, 8, 11, 18]
    // name, blur, distance (pt), angle, size x/y (%), slant x (deg), anchor, opacity, inner
    readonly property var outerPresets: [
        { name: qsTr("Offset: bottom right"), b: 4, d: 3, a: 45, sx: 100, sy: 100, kx: 0, al: 0, o: 0.4 },
        { name: qsTr("Offset: bottom"), b: 4, d: 3, a: 90, sx: 100, sy: 100, kx: 0, al: 1, o: 0.4 },
        { name: qsTr("Offset: bottom left"), b: 4, d: 3, a: 135, sx: 100, sy: 100, kx: 0, al: 2, o: 0.4 },
        { name: qsTr("Offset: right"), b: 4, d: 3, a: 0, sx: 100, sy: 100, kx: 0, al: 3, o: 0.4 },
        { name: qsTr("Offset: centre"), b: 5, d: 0, a: 0, sx: 102, sy: 102, kx: 0, al: 4, o: 0.4 },
        { name: qsTr("Offset: left"), b: 4, d: 3, a: 180, sx: 100, sy: 100, kx: 0, al: 5, o: 0.4 },
        { name: qsTr("Offset: top right"), b: 4, d: 3, a: 315, sx: 100, sy: 100, kx: 0, al: 6, o: 0.4 },
        { name: qsTr("Offset: top"), b: 4, d: 3, a: 270, sx: 100, sy: 100, kx: 0, al: 7, o: 0.4 },
        { name: qsTr("Offset: top left"), b: 4, d: 3, a: 225, sx: 100, sy: 100, kx: 0, al: 8, o: 0.4 }]
    readonly property var innerPresets: [
        { name: qsTr("Inside: top left"), b: 5, d: 4, a: 45, inner: true, o: 0.5 },
        { name: qsTr("Inside: top"), b: 5, d: 4, a: 90, inner: true, o: 0.5 },
        { name: qsTr("Inside: top right"), b: 5, d: 4, a: 135, inner: true, o: 0.5 },
        { name: qsTr("Inside: left"), b: 5, d: 4, a: 0, inner: true, o: 0.5 },
        { name: qsTr("Inside: centre"), b: 9, d: 0, a: 0, inner: true, o: 0.5 },
        { name: qsTr("Inside: right"), b: 5, d: 4, a: 180, inner: true, o: 0.5 },
        { name: qsTr("Inside: bottom left"), b: 5, d: 4, a: 315, inner: true, o: 0.5 },
        { name: qsTr("Inside: bottom"), b: 5, d: 4, a: 270, inner: true, o: 0.5 },
        { name: qsTr("Inside: bottom right"), b: 5, d: 4, a: 225, inner: true, o: 0.5 }]
    readonly property var perspectivePresets: [
        { name: qsTr("Perspective: upper left"), b: 6, d: 0, a: 225, sx: 100, sy: 23, kx: 20, al: 8, o: 0.2 },
        { name: qsTr("Perspective: upper right"), b: 6, d: 0, a: 315, sx: 100, sy: 23, kx: -20, al: 6, o: 0.2 },
        { name: qsTr("Perspective: below"), b: 12, d: 25, a: 90, sx: 90, sy: -19, kx: 0, al: 7, o: 0.15 },
        { name: qsTr("Perspective: lower left"), b: 6, d: 1, a: 135, sx: 100, sy: -23, kx: 13.34, al: 8, o: 0.2 },
        { name: qsTr("Perspective: lower right"), b: 6, d: 1, a: 45, sx: 100, sy: -23, kx: -13.34, al: 6, o: 0.2 }]

    function shadowValues(p) {
        const r = p.a * Math.PI / 180, d = units(p.d)
        return { shadowEnabled: true, shadowInner: p.inner ?? false, shadowColor: withAlpha("#000000", p.o),
                 shadowBlur: units(p.b), shadowX: d * Math.cos(r), shadowY: d * Math.sin(r),
                 shadowScaleX: p.sx ?? 100, shadowScaleY: p.sy ?? 100, shadowSkewX: p.kx ?? 0, shadowSkewY: 0, shadowAlign: p.al ?? 0 }
    }
    // The preset picture's id (render/effectpreview.h): settings in points.
    function shadowPreview(p) {
        const r = p.a * Math.PI / 180
        return "image://effects/" + (onText || onWords ? "text" : "shape") + "?s=1&sc=" + encodeURIComponent(withAlpha("#000000", p.o)) +
               "&sb=" + p.b + "&sx=" + round1(p.d * Math.cos(r)) + "&sy=" + round1(p.d * Math.sin(r)) + "&si=" + ((p.inner ?? false) ? 1 : 0) +
               "&ssx=" + (p.sx ?? 100) + "&ssy=" + (p.sy ?? 100) + "&skx=" + (p.kx ?? 0) + "&sa=" + (p.al ?? 0) +
               ((p.sx ?? 100) !== 100 || (p.sy ?? 100) !== 100 || (p.kx ?? 0) !== 0 ? "&lit=1" : "")
    }
    function glowPreview(size, colour) {
        return "image://effects/" + (onText || onWords ? "text" : "shape") + "?g=" + size + "&gc=" + encodeURIComponent(withAlpha(colour, 0.4))
    }
    function matchesShadow(p) {
        return shadowOn && (fx.shadowInner ?? false) === (p.inner ?? false) && Math.abs(shadowDistance - p.d) < 0.15 &&
               (p.d === 0 || Math.abs(shadowAngle - p.a) < 1) && Math.abs(pts(fx.shadowBlur ?? 0) - p.b) < 0.15 &&
               Math.abs((fx.shadowScaleY ?? 100) - (p.sy ?? 100)) < 0.5 && Math.abs((fx.shadowSkewX ?? 0) - (p.kx ?? 0)) < 0.1
    }

    // ---- pieces -------------------------------------------------------------

    // A collapsed row: the effect's name and what it is set to.
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
    // One preset: its picture, its name on hover, a ring when it is what is set.
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

    Label {
        visible: root.heading !== ""
        text: root.heading
        font.bold: true
        color: Theme.textSecondary
        font.pixelSize: Theme.fsLabel
    }
    // PowerPoint draws a box's effects on its words when the box has no fill
    // and no outline; they follow the border once it has one.
    Label {
        objectName: "effectsBoxHint"
        visible: root.onWords
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textMuted
        font.pixelSize: Theme.fsLabel
        text: qsTr("On the text. Give the box a fill or an outline to glow its border instead.") +
              (root.heading !== "" ? " " + qsTr("Where the text has its own glow or shadow, that one is drawn.") : "")
    }

    // ---- Glow ---------------------------------------------------------------

    Expander {
        objectName: "effectsGlowExpander"
        title: qsTr("Glow")
        open: root.glowOpen
        summary: root.glowOn ? qsTr("%1 pt · %2% clear").arg(root.round1(root.pts(root.fx.glowRadius))).arg(Math.round((1 - root.alphaOf(root.fx.glowColor)) * 100)) : qsTr("None")
        onClicked: root.glowOpen = !root.glowOpen
    }
    ColumnLayout {
        objectName: "effectsGlowSection"
        visible: root.glowOpen
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s2
        spacing: Theme.s2
        Expander {
            objectName: "effectsGlowPresets"
            title: qsTr("Presets")
            open: root.glowPresetsOpen
            onClicked: root.glowPresetsOpen = !root.glowPresetsOpen
        }
        ColumnLayout {
            visible: root.glowPresetsOpen
            Layout.fillWidth: true
            spacing: Theme.s1
            Button {
                objectName: "effectsGlowNone"
                Layout.fillWidth: true
                text: qsTr("No glow")
                icon.name: "x"
                enabled: root.glowOn
                onClicked: root.set({ glowRadius: 0 })
            }
            Label { text: qsTr("Glow variations"); color: Theme.textSecondary; font.pixelSize: Theme.fsLabel }
            GridLayout {
                columns: root.accents.length
                columnSpacing: Theme.s1
                rowSpacing: Theme.s1
                Repeater {
                    model: root.glowSizes.length * root.accents.length
                    delegate: Tile {
                        required property int index
                        readonly property int size: root.glowSizes[Math.floor(index / root.accents.length)]
                        readonly property string colour: root.accents[index % root.accents.length]
                        source: root.glowPreview(size, colour)
                        tip: qsTr("Glow: %1 pt; accent colour %2").arg(size).arg(index % root.accents.length + 1)
                        current: root.glowOn && Math.abs(root.pts(root.fx.glowRadius) - size) < 0.15 &&
                                 root.rgbOf(root.fx.glowColor).toLowerCase() === colour.toLowerCase()
                        onChosen: root.set({ glowRadius: root.units(size), glowColor: root.withAlpha(colour, 0.4) })
                    }
                }
            }
        }
        FieldRow {
            label: qsTr("Colour")
            ColorField {
                value: root.rgbOf(root.fx.glowColor)
                accessibleName: qsTr("Glow colour")
                onPicked: v => root.set({ glowColor: root.withAlpha(v, root.alphaOf(root.fx.glowColor)),
                                          glowRadius: root.glowOn ? root.fx.glowRadius : root.units(8) })
            }
        }
        FieldRow {
            label: qsTr("Size")
            NumField {
                objectName: "effectsGlowSize"
                Layout.fillWidth: true
                suffix: " pt"; step: 1; decimals: 1
                value: root.round1(root.pts(root.fx.glowRadius))
                onCommitted: v => root.set({ glowRadius: root.units(Math.max(0, Math.min(150, v))) })
            }
        }
        FieldRow {
            label: qsTr("Transparency")
            Slider {
                objectName: "effectsGlowTransparency"
                Layout.fillWidth: true
                from: 0; to: 100
                value: Math.round((1 - root.alphaOf(root.fx.glowColor)) * 100)
                onPressedChanged: if (!pressed) root.set({ glowColor: root.withAlpha(root.rgbOf(root.fx.glowColor), 1 - value / 100) })
                Accessible.name: qsTr("Glow transparency")
            }
            Percent { value: (1 - root.alphaOf(root.fx.glowColor)) * 100 }
        }
    }

    // ---- Shadow -------------------------------------------------------------

    Expander {
        objectName: "effectsShadowExpander"
        title: qsTr("Shadow")
        open: root.shadowOpen
        summary: !root.shadowOn ? qsTr("None")
                 : (root.fx.shadowInner ?? false) ? qsTr("Inside")
                 : root.perspective ? qsTr("Perspective")
                 : qsTr("%1 pt · %2°").arg(root.round1(root.shadowDistance)).arg(root.shadowAngle)
        onClicked: root.shadowOpen = !root.shadowOpen
    }
    ColumnLayout {
        objectName: "effectsShadowSection"
        visible: root.shadowOpen
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s2
        spacing: Theme.s2
        Expander {
            objectName: "effectsShadowPresets"
            title: qsTr("Presets")
            open: root.shadowPresetsOpen
            onClicked: root.shadowPresetsOpen = !root.shadowPresetsOpen
        }
        ColumnLayout {
            visible: root.shadowPresetsOpen
            Layout.fillWidth: true
            spacing: Theme.s1
            Button {
                objectName: "effectsShadowNone"
                Layout.fillWidth: true
                text: qsTr("No shadow")
                icon.name: "x"
                enabled: root.shadowOn
                onClicked: root.set({ shadowEnabled: false })
            }
            Repeater {
                model: [{ title: qsTr("Outer"), list: root.outerPresets, shown: true },
                        { title: qsTr("Inner"), list: root.innerPresets, shown: true },
                        { title: qsTr("Perspective"), list: root.perspectivePresets, shown: true }]
                delegate: ColumnLayout {
                    required property var modelData
                    visible: modelData.shown
                    Layout.fillWidth: true
                    spacing: Theme.s1
                    Label { text: modelData.title; color: Theme.textSecondary; font.pixelSize: Theme.fsLabel }
                    GridLayout {
                        columns: 5
                        columnSpacing: Theme.s1
                        rowSpacing: Theme.s1
                        Repeater {
                            model: modelData.list
                            delegate: Tile {
                                required property var modelData
                                source: root.shadowPreview(modelData)
                                tip: modelData.name
                                current: root.matchesShadow(modelData)
                                onChosen: root.set(root.shadowValues(modelData))
                            }
                        }
                    }
                }
            }
        }
        FieldRow {
            label: qsTr("Type")
            ComboBox {
                objectName: "effectsShadowType"
                Layout.fillWidth: true
                model: [qsTr("Outer"), qsTr("Inner")]
                currentIndex: (root.fx.shadowInner ?? false) ? 1 : 0
                onActivated: root.set({ shadowEnabled: true, shadowInner: currentIndex === 1 })
            }
        }
        FieldRow {
            label: qsTr("Colour")
            ColorField {
                value: root.rgbOf(root.fx.shadowColor)
                accessibleName: qsTr("Shadow colour")
                onPicked: v => root.set({ shadowEnabled: true, shadowColor: root.withAlpha(v, root.alphaOf(root.fx.shadowColor)) })
            }
        }
        FieldRow {
            label: qsTr("Transparency")
            Slider {
                objectName: "effectsShadowTransparency"
                Layout.fillWidth: true
                from: 0; to: 100
                value: Math.round((1 - root.alphaOf(root.fx.shadowColor)) * 100)
                onPressedChanged: if (!pressed) root.set({ shadowEnabled: true, shadowColor: root.withAlpha(root.rgbOf(root.fx.shadowColor), 1 - value / 100) })
                Accessible.name: qsTr("Shadow transparency")
            }
            Percent { value: (1 - root.alphaOf(root.fx.shadowColor)) * 100 }
        }
        FieldRow {
            visible: !(root.fx.shadowInner ?? false)
            label: qsTr("Size")
            NumField {
                objectName: "effectsShadowSize"
                Layout.fillWidth: true
                suffix: " %"; step: 1
                value: root.fx.shadowScaleX ?? 100
                onCommitted: v => {
                    const sx = Math.max(-200, Math.min(200, v)), was = root.fx.shadowScaleX ?? 100
                    root.set({ shadowEnabled: true, shadowScaleX: sx, shadowScaleY: was !== 0 ? Math.max(-200, Math.min(200, (root.fx.shadowScaleY ?? 100) * sx / was)) : sx })
                }
            }
        }
        FieldRow {
            label: qsTr("Blur")
            NumField {
                objectName: "effectsShadowBlur"
                Layout.fillWidth: true
                suffix: " pt"; step: 1; decimals: 1
                value: root.round1(root.pts(root.fx.shadowBlur))
                onCommitted: v => root.set({ shadowEnabled: true, shadowBlur: root.units(Math.max(0, Math.min(100, v))) })
            }
        }
        FieldRow {
            label: qsTr("Angle")
            NumField {
                objectName: "effectsShadowAngle"
                Layout.fillWidth: true
                suffix: " °"; step: 15
                value: root.shadowAngle
                onCommitted: v => root.setShadowVector(Math.max(root.shadowDistance, 0.1), v)
            }
        }
        FieldRow {
            label: qsTr("Distance")
            NumField {
                objectName: "effectsShadowDistance"
                Layout.fillWidth: true
                suffix: " pt"; step: 1; decimals: 1
                value: root.round1(root.shadowDistance)
                onCommitted: v => root.setShadowVector(Math.min(200, v), root.shadowAngle)
            }
        }
    }
}

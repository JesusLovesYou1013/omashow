import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// BOTH-EFFECTS-1: one set of Glow and Shadow settings (EFFECTS-UI-1), for
// one target: "text" (the words, a:rPr) or "shape" (the box: a shape or
// picture's own p:spPr effects, or a text box's own when it has no shape;
// backend.effectState says which). STYLE3D: the Style tab shows the box set under
// Effects and the text set under Text Fill and Outline (Inspector.qml).
ColumnLayout {
    id: root
    objectName: "effectsSet"
    spacing: Theme.s2

    property string target: "text"
    // A heading over the set.
    property string heading: ""
    // STYLE3D: which rows are open, kept in the inspector's styleOpen per
    // session (closed at first), apart for the text's and the box's sets.
    property QtObject opener: null
    function isOpen(name) { return opener ? opener.isOpen(target + "." + name) : false }
    function toggle(name) { if (opener) opener.toggle(target + "." + name) }
    readonly property bool glowOpen: isOpen("glow")
    readonly property bool shadowOpen: isOpen("shadow")
    readonly property bool glowPresetsOpen: isOpen("glowPresets")
    readonly property bool shadowPresetsOpen: isOpen("shadowPresets")
    readonly property bool reflectionOpen: isOpen("reflection")
    readonly property bool reflectionPresetsOpen: isOpen("reflectionPresets")
    readonly property bool softOpen: isOpen("soft")
    readonly property bool softPresetsOpen: isOpen("softPresets")
    readonly property bool formatOpen: isOpen("format")
    readonly property bool formatPresetsOpen: isOpen("formatPresets")
    readonly property bool rotationOpen: isOpen("rotation")
    readonly property bool rotationPresetsOpen: isOpen("rotationPresets")

    readonly property var fx: { backend.selection; backend.textSelection; return backend.effectState(target) }
    readonly property bool available: backend.hasSelection && (fx.count ?? 0) > 0
    visible: available
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

    // ---- STYLE3D: soft edges, reflection, 3-D format and rotation ---------------

    readonly property var presets3d: { backend.ptPerUnit; return backend.style3dPresets() }
    readonly property var refl: fx.reflection ?? ({})
    readonly property bool reflectionOn: Object.keys(refl).length > 0
    readonly property real softEdge: fx.softEdge ?? 0
    readonly property bool softOn: !onText && softEdge > 0
    readonly property var fmt: fx.format3d ?? ({})
    readonly property bool formatOn: Object.keys(fmt).length > 0
    readonly property var scn: fx.scene3d ?? ({})
    readonly property bool sceneOn: Object.keys(scn).length > 0
    readonly property var camInfo: (presets3d.cameraInfo ?? ({}))[scn.camera ?? "orthographicFront"] ?? ({ lat: 0, lon: 0, rev: 0, parallel: true })
    // PowerPoint's X, Y and Z rotation are the camera's longitude, latitude and revolution.
    readonly property real rotX: scn.lon !== undefined ? scn.lon : (camInfo.lon ?? 0)
    readonly property real rotY: scn.lat !== undefined ? scn.lat : (camInfo.lat ?? 0)
    readonly property real rotZ: scn.rev !== undefined ? scn.rev : (camInfo.rev ?? 0)
    readonly property bool rotationOn: sceneOn && ((scn.camera ?? "orthographicFront") !== "orthographicFront" || rotX !== 0 || rotY !== 0 || rotZ !== 0)
    readonly property var defaultScene: ({ camera: "orthographicFront", rig: "threePt", rigDir: "t" })
    // ROT-SHADOW-1: what turns these effects. The words also turn with their
    // box's 3-D rotation unless "Keep text flat".
    function sceneTurns(s) {
        if (!s || Object.keys(s).length === 0) return false
        const info = (presets3d.cameraInfo ?? ({}))[s.camera ?? "orthographicFront"] ?? ({ lat: 0, lon: 0, rev: 0, parallel: true })
        const x = s.lon !== undefined ? s.lon : (info.lon ?? 0), y = s.lat !== undefined ? s.lat : (info.lat ?? 0), z = s.rev !== undefined ? s.rev : (info.rev ?? 0)
        return (s.camera ?? "orthographicFront") !== "orthographicFront" || x !== 0 || y !== 0 || z !== 0
    }
    readonly property bool textFlat: fx.textFlat ?? false
    readonly property bool turnsNow: onText ? (!textFlat && (rotationOn || sceneTurns(fx.boxScene3d))) : rotationOn
    readonly property bool follows: fx.effects3dFollow ?? true
    readonly property bool outerShadowOn: shadowOn && !(fx.shadowInner ?? false)
    readonly property string followNote: !turnsNow ? "" : follows ? qsTr("Follows 3-D rotation") : qsTr("Flat (not tilted)")
    // Opens a row (and its group) and brings it into view.
    function reveal(name, item) {
        if (opener) opener.set(target + "." + name, true)
        revealTimer.item = item
        revealTimer.restart()   // once the row has been laid out
    }
    Timer {
        id: revealTimer
        property Item item: null
        interval: 60
        onTriggered: {
            let flick = item ? item.parent : null
            while (flick && flick.contentY === undefined) flick = flick.parent
            if (!flick) return
            // The row's header sits just above its section.
            const p = item.mapToItem(flick.contentItem, 0, 0)
            flick.contentY = Math.max(0, Math.min(p.y - Theme.hControl - Theme.s2, flick.contentHeight - flick.height))
        }
    }
    // One line for the group header while it is closed.
    readonly property string summary: {
        const on = []
        if (shadowOn) on.push(qsTr("Shadow"))
        if (reflectionOn) on.push(qsTr("Reflection"))
        if (glowOn) on.push(qsTr("Glow"))
        if (softOn) on.push(qsTr("Soft edges"))
        if (formatOn) on.push(qsTr("3-D format"))
        if (rotationOn) on.push(qsTr("3-D rotation"))
        return on.length ? on.join(", ") : qsTr("None")
    }

    function angle(v) { return ((v % 360) + 360) % 360 }
    function preview3d(x) { return "image://effects/" + (onText || onWords ? "text" : "shape") + "?x=" + encodeURIComponent(JSON.stringify(x)) }
    function reflectionInPoints(v) { const r = Object.assign({}, v); r.blur = pts(v.blur); r.dist = pts(v.dist); return r }
    function setReflection(change) {
        const base = reflectionOn ? refl : (presets3d.reflections ?? [{ value: ({}) }])[0].value
        set({ reflection: Object.assign({}, base, change) })
    }
    function withScene(values) { if (!sceneOn) values.scene3d = defaultScene; return values }
    function setFormat(change) { set(withScene({ format3d: Object.assign({}, fmt, change) })) }
    function setScene(change) { set({ scene3d: Object.assign({}, sceneOn ? scn : defaultScene, change) }) }
    function setBevel(which, preset) {
        const f = Object.assign({}, fmt)
        if (!preset) delete f[which]
        else { const b = fmt[which]; f[which] = { preset: preset, w: b ? b.w : units(6), h: b ? b.h : units(6) } }
        set(withScene({ format3d: f }))
    }
    function setBevelSize(which, key, points) {
        const b = Object.assign({ preset: "circle", w: units(6), h: units(6) }, fmt[which] ?? {})
        b[key] = units(Math.max(0, Math.min(1584, points)))
        const change = {}; change[which] = b
        setFormat(change)
    }
    function setCamera(name) {
        const s = Object.assign({}, sceneOn ? scn : defaultScene)
        delete s.lat; delete s.lon; delete s.rev
        s.camera = name
        set({ scene3d: s })
    }
    function setRotation(x, y, z) { setScene({ lon: angle(x), lat: angle(y), rev: angle(z) }) }
    function resetRotation() { setCamera("orthographicFront") }
    function camerasOf(kind) {
        const info = presets3d.cameraInfo ?? ({})
        return (presets3d.cameras ?? []).filter(c => {
            const i = info[c]
            if (!i || i.legacy) return false
            if (kind === "oblique") return i.oblique
            if (kind === "perspective") return !i.parallel && !i.oblique
            return i.parallel && !i.oblique
        })
    }
    function words(name) {
        const s = String(name ?? "").replace(/([a-z])([A-Z0-9])/g, "$1 $2").replace(/([0-9])([A-Z])/g, "$1 $2")
        return s.charAt(0).toUpperCase() + s.slice(1).toLowerCase()
    }
    function cameraName(c) { return c === "orthographicFront" ? qsTr("No rotation (front)") : words(c) }
    readonly property var bevelNames: ({ circle: qsTr("Circle"), relaxedInset: qsTr("Relaxed inset"), cross: qsTr("Cross"), coolSlant: qsTr("Cool slant"),
                                         angle: qsTr("Angle"), softRound: qsTr("Soft round"), convex: qsTr("Convex"), slope: qsTr("Slope"),
                                         divot: qsTr("Divot"), riblet: qsTr("Riblet"), hardEdge: qsTr("Hard edge"), artDeco: qsTr("Art deco") })
    function bevelName(b) { return bevelNames[b] ?? words(b) }
    readonly property var materialNames: ({ matte: qsTr("Matte"), warmMatte: qsTr("Warm matte"), plastic: qsTr("Plastic"), metal: qsTr("Metal"),
                                            dkEdge: qsTr("Dark edge"), softEdge: qsTr("Soft edge"), flat: qsTr("Flat"), legacyWireframe: qsTr("Wire frame"),
                                            powder: qsTr("Powder"), translucentPowder: qsTr("Translucent powder"), clear: qsTr("Clear"),
                                            softmetal: qsTr("Soft metal"), legacyMatte: qsTr("Matte (legacy)"), legacyPlastic: qsTr("Plastic (legacy)"),
                                            legacyMetal: qsTr("Metal (legacy)") })
    function materialName(m) { return materialNames[m] ?? words(m) }
    readonly property var rigNames: ({ threePt: qsTr("Three point"), balanced: qsTr("Balanced"), soft: qsTr("Soft"), harsh: qsTr("Harsh"),
                                       flood: qsTr("Flood"), contrasting: qsTr("Contrasting"), morning: qsTr("Morning"), sunrise: qsTr("Sunrise"),
                                       sunset: qsTr("Sunset"), chilly: qsTr("Chilly"), freezing: qsTr("Freezing"), flat: qsTr("Flat"),
                                       twoPt: qsTr("Two point"), glow: qsTr("Glow"), brightRoom: qsTr("Bright room") })
    function rigName(r) { return rigNames[r] ?? words(r) }
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

    // ---- Shadow -------------------------------------------------------------

    Expander {
        objectName: "effectsShadowExpander"
        title: qsTr("Shadow")
        open: root.shadowOpen
        summary: !root.shadowOn ? qsTr("None")
                 : (root.fx.shadowInner ?? false) ? qsTr("Inside")
                 : root.perspective ? qsTr("Perspective")
                 : qsTr("%1 pt · %2°").arg(root.round1(root.shadowDistance)).arg(root.shadowAngle)
        // ROT-SHADOW-1: the turned shape's shadow tilts with it.
        text: (summary ? title + " · " + summary : title) + (root.outerShadowOn && root.followNote ? " · " + root.followNote : "")
        onClicked: root.toggle("shadow")
    }
    ColumnLayout {
        id: shadowSection
        objectName: "effectsShadowSection"
        visible: root.shadowOpen
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s2
        Label {
            objectName: "effectsShadowFollowNote"
            visible: root.turnsNow
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.accent
            font.pixelSize: Theme.fsLabel
            text: root.follows ? (root.onText ? qsTr("Follows 3-D rotation: cast from the turned words, as PowerPoint draws it. An inner shadow stays on the letters.")
                                              : qsTr("Follows 3-D rotation: cast from the turned shape, as PowerPoint draws it. An inner shadow stays on the face."))
                               : qsTr("Flat: not following 3-D rotation (3-D Rotation section). PowerPoint always turns it.")
        }
        spacing: Theme.s2
        Expander {
            objectName: "effectsShadowPresets"
            title: qsTr("Presets")
            open: root.shadowPresetsOpen
            onClicked: root.toggle("shadowPresets")
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

    // ---- Reflection (STYLE3D) --------------------------------------------------

    Expander {
        objectName: "effectsReflectionExpander"
        title: qsTr("Reflection")
        open: root.reflectionOpen
        // Shorter while the 3-D note follows it, so the note fits.
        summary: !root.reflectionOn ? qsTr("None")
                 : root.followNote ? qsTr("%1% · %2 pt").arg(Math.round(root.refl.endPos ?? 100)).arg(root.round1(root.pts(root.refl.dist)))
                 : qsTr("%1% size · %2 pt away").arg(Math.round(root.refl.endPos ?? 100)).arg(root.round1(root.pts(root.refl.dist)))
        text: (summary ? title + " · " + summary : title) + (root.reflectionOn && root.followNote ? " · " + root.followNote : "")
        onClicked: root.toggle("reflection")
    }
    ColumnLayout {
        id: reflectionSection
        objectName: "effectsReflectionSection"
        visible: root.reflectionOpen
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s2
        spacing: Theme.s2
        Label {
            objectName: "effectsReflectionFollowNote"
            visible: root.turnsNow
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.accent
            font.pixelSize: Theme.fsLabel
            text: root.follows ? (root.onText ? qsTr("Follows 3-D rotation: the turned words mirrored, as PowerPoint draws it.")
                                              : qsTr("Follows 3-D rotation: the turned shape mirrored, as PowerPoint draws it."))
                               : qsTr("Flat: not following 3-D rotation (3-D Rotation section). PowerPoint always turns it.")
        }
        Expander {
            objectName: "effectsReflectionPresets"
            title: qsTr("Presets")
            open: root.reflectionPresetsOpen
            onClicked: root.toggle("reflectionPresets")
        }
        ColumnLayout {
            visible: root.reflectionPresetsOpen
            Layout.fillWidth: true
            spacing: Theme.s1
            Button {
                objectName: "effectsReflectionNone"
                Layout.fillWidth: true
                text: qsTr("No reflection")
                icon.name: "x"
                enabled: root.reflectionOn
                onClicked: root.set({ reflection: ({}) })
            }
            Label { text: qsTr("Reflection variations"); color: Theme.textSecondary; font.pixelSize: Theme.fsLabel }
            GridLayout {
                columns: 3
                columnSpacing: Theme.s1
                rowSpacing: Theme.s1
                Repeater {
                    model: root.presets3d.reflections ?? []
                    delegate: Tile {
                        required property var modelData
                        source: root.reflectionPresetsOpen ? root.preview3d({ reflection: root.reflectionInPoints(modelData.value) }) : ""
                        tip: modelData.name
                        current: root.reflectionOn && Math.abs((root.refl.endPos ?? 100) - modelData.value.endPos) < 0.5 &&
                                 Math.abs(root.pts(root.refl.dist) - root.pts(modelData.value.dist)) < 0.15
                        onChosen: root.set({ reflection: modelData.value })
                    }
                }
            }
        }
        FieldRow {
            label: qsTr("Transparency")
            Slider {
                objectName: "effectsReflectionTransparency"
                Layout.fillWidth: true
                from: 0; to: 100
                value: 100 - (root.refl.startAlpha ?? 50)
                onPressedChanged: if (!pressed) root.setReflection({ startAlpha: 100 - Math.round(value) })
                Accessible.name: qsTr("Reflection transparency")
            }
            Percent { value: root.reflectionOn ? 100 - (root.refl.startAlpha ?? 100) : 0 }
        }
        FieldRow {
            label: qsTr("Size")
            NumField {
                objectName: "effectsReflectionSize"
                Layout.fillWidth: true
                suffix: " %"; step: 1
                value: Math.round(root.reflectionOn ? (root.refl.endPos ?? 100) : 0)
                onCommitted: v => root.setReflection({ endPos: Math.max(0, Math.min(100, v)) })
            }
        }
        FieldRow {
            label: qsTr("Distance")
            NumField {
                objectName: "effectsReflectionDistance"
                Layout.fillWidth: true
                suffix: " pt"; step: 1; decimals: 1
                value: root.round1(root.pts(root.refl.dist))
                onCommitted: v => root.setReflection({ dist: root.units(Math.max(0, Math.min(100, v))) })
            }
        }
        FieldRow {
            label: qsTr("Blur")
            NumField {
                objectName: "effectsReflectionBlur"
                Layout.fillWidth: true
                suffix: " pt"; step: 0.5; decimals: 1
                value: root.round1(root.pts(root.refl.blur))
                onCommitted: v => root.setReflection({ blur: root.units(Math.max(0, Math.min(100, v))) })
            }
        }
    }

    // ---- Glow ---------------------------------------------------------------

    Expander {
        objectName: "effectsGlowExpander"
        title: qsTr("Glow")
        open: root.glowOpen
        summary: root.glowOn ? qsTr("%1 pt · %2% clear").arg(root.round1(root.pts(root.fx.glowRadius))).arg(Math.round((1 - root.alphaOf(root.fx.glowColor)) * 100)) : qsTr("None")
        onClicked: root.toggle("glow")
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
            onClicked: root.toggle("glowPresets")
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

    // ---- Soft Edges (STYLE3D; shapes and pictures: PowerPoint has none for text) ----

    Expander {
        objectName: "effectsSoftExpander"
        visible: !root.onText
        title: qsTr("Soft Edges")
        open: root.softOpen
        summary: root.softOn ? qsTr("%1 pt").arg(root.round1(root.pts(root.softEdge))) : qsTr("None")
        onClicked: root.toggle("soft")
    }
    ColumnLayout {
        objectName: "effectsSoftSection"
        visible: root.softOpen && !root.onText
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s2
        spacing: Theme.s2
        Expander {
            objectName: "effectsSoftPresets"
            title: qsTr("Presets")
            open: root.softPresetsOpen
            onClicked: root.toggle("softPresets")
        }
        ColumnLayout {
            visible: root.softPresetsOpen
            Layout.fillWidth: true
            spacing: Theme.s1
            Button {
                objectName: "effectsSoftNone"
                Layout.fillWidth: true
                text: qsTr("No soft edges")
                icon.name: "x"
                enabled: root.softOn
                onClicked: root.set({ softEdge: 0 })
            }
            GridLayout {
                columns: 6
                columnSpacing: Theme.s1
                rowSpacing: Theme.s1
                Repeater {
                    model: root.presets3d.softEdges ?? []
                    delegate: Tile {
                        required property var modelData
                        // The sample is 28 pt across: the size drawn to its scale.
                        source: root.softPresetsOpen ? root.preview3d({ softEdge: Math.min(14, modelData / 3) }) : ""
                        tip: qsTr("%1 point").arg(modelData)
                        current: root.softOn && Math.abs(root.pts(root.softEdge) - modelData) < 0.05
                        onChosen: root.set({ softEdge: root.units(modelData) })
                    }
                }
            }
        }
        FieldRow {
            label: qsTr("Size")
            NumField {
                objectName: "effectsSoftSize"
                Layout.fillWidth: true
                suffix: " pt"; step: 1; decimals: 1
                value: root.round1(root.pts(root.softEdge))
                onCommitted: v => root.set({ softEdge: root.units(Math.max(0, Math.min(100, v))) })
            }
        }
    }

    // ---- 3-D Format (STYLE3D; drawn as a shaded 2D approximation) ----------------

    Expander {
        objectName: "effects3dFormatExpander"
        title: qsTr("3-D Format")
        open: root.formatOpen
        summary: !root.formatOn ? qsTr("None")
                 : [root.fmt.top ? root.bevelName(root.fmt.top.preset) : "", (root.fmt.depth ?? 0) > 0 ? qsTr("depth %1 pt").arg(root.round1(root.pts(root.fmt.depth))) : "",
                    root.materialName(root.fmt.material || "warmMatte")].filter(s => s !== "").join(" · ")
        onClicked: root.toggle("format")
    }
    ColumnLayout {
        objectName: "effects3dFormatSection"
        visible: root.formatOpen
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s2
        spacing: Theme.s2
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textMuted
            font.pixelSize: Theme.fsLabel
            text: qsTr("Prototype: drawn as a shaded flat picture, not true 3-D. Saved to PowerPoint exactly.")
        }
        Repeater {
            model: [{ which: "top", title: qsTr("Top bevel") }, { which: "bottom", title: qsTr("Bottom bevel") }]
            delegate: ColumnLayout {
                id: bevelRow
                required property var modelData
                readonly property var bevel: root.fmt[modelData.which] ?? null
                Layout.fillWidth: true
                spacing: Theme.s1
                FieldRow {
                    label: bevelRow.modelData.title
                    ComboBox {
                        objectName: "effectsBevel_" + bevelRow.modelData.which
                        Layout.fillWidth: true
                        model: [qsTr("No bevel")].concat((root.presets3d.bevels ?? []).map(b => root.bevelName(b)))
                        currentIndex: bevelRow.bevel ? 1 + (root.presets3d.bevels ?? []).indexOf(bevelRow.bevel.preset) : 0
                        onActivated: root.setBevel(bevelRow.modelData.which, currentIndex === 0 ? "" : root.presets3d.bevels[currentIndex - 1])
                    }
                }
                Flow {
                    visible: bevelRow.modelData.which === "top" && root.formatPresetsOpen
                    Layout.fillWidth: true
                    spacing: Theme.s1
                    Repeater {
                        model: root.presets3d.bevels ?? []
                        delegate: Tile {
                            required property string modelData
                            source: root.formatPresetsOpen ? root.preview3d({ format3d: { top: { preset: modelData, w: 6, h: 6 } }, scene3d: root.defaultScene }) : ""
                            tip: root.bevelName(modelData)
                            current: bevelRow.bevel !== null && bevelRow.bevel.preset === modelData
                            onChosen: root.setBevel("top", modelData)
                        }
                    }
                }
                GridLayout {
                    visible: bevelRow.bevel !== null
                    columns: 2
                    columnSpacing: Theme.s2
                    Layout.fillWidth: true
                    NumField {
                        Layout.fillWidth: true
                        label: qsTr("Width"); suffix: " pt"; step: 1; decimals: 1
                        value: root.round1(root.pts(bevelRow.bevel ? bevelRow.bevel.w : 0))
                        onCommitted: v => root.setBevelSize(bevelRow.modelData.which, "w", v)
                    }
                    NumField {
                        Layout.fillWidth: true
                        label: qsTr("Height"); suffix: " pt"; step: 1; decimals: 1
                        value: root.round1(root.pts(bevelRow.bevel ? bevelRow.bevel.h : 0))
                        onCommitted: v => root.setBevelSize(bevelRow.modelData.which, "h", v)
                    }
                }
            }
        }
        Expander {
            objectName: "effects3dBevelGallery"
            title: qsTr("Bevel presets")
            open: root.formatPresetsOpen
            onClicked: root.toggle("formatPresets")
        }
        FieldRow {
            label: qsTr("Depth colour")
            ColorField {
                value: root.rgbOf(root.fmt.extrusionColor || "#ff000000")
                accessibleName: qsTr("Depth colour")
                onPicked: v => root.setFormat({ extrusionColor: root.withAlpha(v, 1) })
            }
        }
        FieldRow {
            label: qsTr("Depth")
            NumField {
                objectName: "effects3dDepth"
                Layout.fillWidth: true
                suffix: " pt"; step: 1; decimals: 1
                value: root.round1(root.pts(root.fmt.depth))
                onCommitted: v => root.setFormat({ depth: root.units(Math.max(0, Math.min(1584, v))) })
            }
        }
        FieldRow {
            label: qsTr("Contour colour")
            ColorField {
                value: root.rgbOf(root.fmt.contourColor || "#ffffffff")
                accessibleName: qsTr("Contour colour")
                onPicked: v => root.setFormat({ contourColor: root.withAlpha(v, 1) })
            }
        }
        FieldRow {
            label: qsTr("Contour")
            NumField {
                objectName: "effects3dContour"
                Layout.fillWidth: true
                suffix: " pt"; step: 0.5; decimals: 1
                value: root.round1(root.pts(root.fmt.contour))
                onCommitted: v => root.setFormat({ contour: root.units(Math.max(0, Math.min(1584, v))) })
            }
        }
        FieldRow {
            label: qsTr("Material")
            ComboBox {
                objectName: "effects3dMaterial"
                Layout.fillWidth: true
                model: (root.presets3d.materials ?? []).map(m => root.materialName(m))
                currentIndex: (root.presets3d.materials ?? []).indexOf(root.fmt.material || "warmMatte")
                onActivated: root.setFormat({ material: root.presets3d.materials[currentIndex] })
            }
        }
        FieldRow {
            label: qsTr("Lighting")
            ComboBox {
                objectName: "effects3dLighting"
                Layout.fillWidth: true
                model: (root.presets3d.lightRigs ?? []).map(r => root.rigName(r))
                currentIndex: (root.presets3d.lightRigs ?? []).indexOf(root.scn.rig ?? "threePt")
                onActivated: root.setScene({ rig: root.presets3d.lightRigs[currentIndex] })
            }
        }
        FieldRow {
            label: qsTr("Angle")
            NumField {
                objectName: "effects3dLightAngle"
                Layout.fillWidth: true
                suffix: "°"; step: 15; decimals: 1
                value: root.round1(root.scn.rigRev ?? 0)
                onCommitted: v => root.setScene({ rigLat: 0, rigLon: 0, rigRev: root.angle(v) })
            }
        }
        Button {
            objectName: "effects3dFormatReset"
            Layout.fillWidth: true
            text: qsTr("Reset 3-D format")
            enabled: root.formatOn
            onClicked: root.set({ format3d: ({}) })
        }
    }

    // ---- 3-D Rotation (STYLE3D; prototype drawing, exact round trip) -------------

    Expander {
        objectName: "effects3dRotationExpander"
        title: qsTr("3-D Rotation")
        open: root.rotationOpen
        summary: !root.rotationOn ? qsTr("None")
                 : qsTr("X %1° · Y %2° · Z %3°").arg(root.round1(root.rotX)).arg(root.round1(root.rotY)).arg(root.round1(root.rotZ))
        onClicked: root.toggle("rotation")
    }
    ColumnLayout {
        objectName: "effects3dRotationSection"
        visible: root.rotationOpen
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s2
        spacing: Theme.s2
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textMuted
            font.pixelSize: Theme.fsLabel
            text: qsTr("Prototype: the turned picture is approximate. The rotation is saved to PowerPoint exactly.")
        }
        // ROT-SHADOW-1: the shadow and reflection are the Shadow and
        // Reflection rows' own settings; the rotation only turns how they draw.
        CheckBox {
            objectName: "effects3dFollow"
            Layout.fillWidth: true
            text: qsTr("Shadow and reflection follow 3-D rotation")
            checked: root.follows
            onToggled: root.set({ effects3dFollow: checked })
        }
        Label {
            objectName: "effects3dFollowNote"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textMuted
            font.pixelSize: Theme.fsLabel
            text: root.follows ? (root.onText ? qsTr("The shadow and reflection tilt with the words, as in PowerPoint.")
                                              : qsTr("The shadow and reflection tilt with the shape, as in PowerPoint."))
                               : qsTr("The shadow and reflection stay flat. OmaShow only: PowerPoint always tilts them.")
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2
            Button {
                objectName: "effects3dShadowButton"
                Layout.fillWidth: true
                text: root.shadowOn ? qsTr("Edit shadow") : qsTr("Add shadow")
                // A shadow below that shows the tilt: PowerPoint's Perspective: Below.
                onClicked: {
                    if (!root.shadowOn) root.set(root.shadowValues(root.perspectivePresets[2]))
                    root.reveal("shadow", shadowSection)
                }
            }
            Button {
                objectName: "effects3dReflectionButton"
                Layout.fillWidth: true
                text: root.reflectionOn ? qsTr("Edit reflection") : qsTr("Add reflection")
                // Half Reflection: Touching.
                onClicked: {
                    if (!root.reflectionOn) {
                        const all = root.presets3d.reflections ?? []
                        if (all.length > 1) root.set({ reflection: all[1].value })
                    }
                    root.reveal("reflection", reflectionSection)
                }
            }
        }
        CheckBox {
            objectName: "effects3dKeepTextFlat"
            visible: root.onText
            Layout.fillWidth: true
            text: qsTr("Keep text flat")
            checked: root.textFlat
            onToggled: root.set({ textFlat: checked })
        }
        Expander {
            objectName: "effects3dRotationPresets"
            title: qsTr("Presets")
            open: root.rotationPresetsOpen
            onClicked: root.toggle("rotationPresets")
        }
        ColumnLayout {
            visible: root.rotationPresetsOpen
            Layout.fillWidth: true
            spacing: Theme.s1
            Button {
                objectName: "effects3dRotationNone"
                Layout.fillWidth: true
                text: qsTr("No rotation")
                icon.name: "x"
                enabled: root.rotationOn
                onClicked: root.resetRotation()
            }
            Repeater {
                model: [{ title: qsTr("Parallel"), kind: "parallel" }, { title: qsTr("Perspective"), kind: "perspective" }, { title: qsTr("Oblique"), kind: "oblique" }]
                delegate: ColumnLayout {
                    id: camGroup
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.s1
                    Label { text: camGroup.modelData.title; color: Theme.textSecondary; font.pixelSize: Theme.fsLabel }
                    Flow {
                        Layout.fillWidth: true
                        spacing: Theme.s1
                        Repeater {
                            model: root.camerasOf(camGroup.modelData.kind)
                            delegate: Tile {
                                required property string modelData
                                source: root.rotationPresetsOpen ? root.preview3d({ scene3d: { camera: modelData, rig: "threePt", rigDir: "t" } }) : ""
                                tip: root.cameraName(modelData)
                                current: root.sceneOn && (root.scn.camera ?? "") === modelData && root.scn.lat === undefined
                                onChosen: root.setCamera(modelData)
                            }
                        }
                    }
                }
            }
        }
        FieldRow {
            label: qsTr("Preset")
            ComboBox {
                objectName: "effects3dCamera"
                Layout.fillWidth: true
                model: (root.presets3d.cameras ?? []).map(c => root.cameraName(c))
                currentIndex: (root.presets3d.cameras ?? []).indexOf(root.scn.camera ?? "orthographicFront")
                onActivated: root.setCamera(root.presets3d.cameras[currentIndex])
            }
        }
        GridLayout {
            columns: 3
            columnSpacing: Theme.s2
            Layout.fillWidth: true
            NumField { objectName: "effects3dRotX"; Layout.fillWidth: true; label: qsTr("X"); suffix: "°"; step: 10; decimals: 1
                       value: root.round1(root.rotX); onCommitted: v => root.setRotation(v, root.rotY, root.rotZ) }
            NumField { objectName: "effects3dRotY"; Layout.fillWidth: true; label: qsTr("Y"); suffix: "°"; step: 10; decimals: 1
                       value: root.round1(root.rotY); onCommitted: v => root.setRotation(root.rotX, v, root.rotZ) }
            NumField { objectName: "effects3dRotZ"; Layout.fillWidth: true; label: qsTr("Z"); suffix: "°"; step: 10; decimals: 1
                       value: root.round1(root.rotZ); onCommitted: v => root.setRotation(root.rotX, root.rotY, v) }
        }
        FieldRow {
            label: qsTr("Perspective")
            NumField {
                objectName: "effects3dPerspective"
                Layout.fillWidth: true
                enabled: !(root.camInfo.parallel ?? true)
                suffix: "°"; step: 5; decimals: 1
                value: root.round1((root.scn.fov ?? -1) >= 0 ? root.scn.fov : (root.camInfo.parallel ?? true) ? 0 : 45)
                onCommitted: v => root.setScene({ fov: Math.max(0, Math.min(120, v)) })
            }
        }
        FieldRow {
            label: qsTr("Distance from ground")
            NumField {
                objectName: "effects3dGround"
                Layout.fillWidth: true
                suffix: " pt"; step: 1; decimals: 1
                value: root.round1(root.pts(root.fmt.z))
                onCommitted: v => root.setFormat({ z: root.units(Math.max(-4000, Math.min(4000, v))) })
            }
        }
        Button {
            objectName: "effects3dRotationReset"
            Layout.fillWidth: true
            text: qsTr("Reset 3-D rotation")
            enabled: root.rotationOn
            onClicked: root.resetRotation()
        }
    }
}

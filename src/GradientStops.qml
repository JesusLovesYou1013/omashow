import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// A gradient's stops, as PowerPoint's Format Shape shows them: the
// bar with a marker per stop (click one to pick it, drag it to move it), add
// and remove, and the picked stop's colour, position, transparency and
// brightness. Stops are {p: 0-1, c: "#AARRGGBB", b: -1..1}; every edit hands
// the whole list to `edited`, so each is one undo step.
ColumnLayout {
    id: root
    spacing: Theme.s2

    property var stops: []
    property string name: qsTr("Gradient")
    property int current: 0
    signal edited(var stops)

    readonly property int count: stops ? stops.length : 0
    readonly property var stop: count > 0 ? stops[Math.min(current, count - 1)] : ({ p: 0, c: "#ff000000", b: 0 })

    function alphaOf(c) { const s = String(c ?? ""); return s.length === 9 ? parseInt(s.substr(1, 2), 16) / 255 : 1 }
    function rgbOf(c) { const s = String(c ?? ""); return s.length === 9 ? "#" + s.substr(3) : s }
    function withAlpha(rgb, alpha) {
        const a = Math.max(0, Math.min(255, Math.round(alpha * 255))).toString(16).padStart(2, "0")
        const s = String(rgb)
        return "#" + a + (s.length === 9 ? s.substr(3) : s.substr(1))
    }
    function copy() { return (stops ?? []).map(s => ({ p: s.p, c: s.c, b: s.b ?? 0 })) }
    function change(values) {
        const list = copy()
        if (list.length === 0) return
        const i = Math.min(current, list.length - 1)
        for (const k in values) list[i][k] = values[k]
        root.edited(list)
    }
    // Where a stop at position p goes in the sorted list, so the pick follows a moved stop.
    function moveTo(p) {
        const list = copy()
        const i = Math.min(current, list.length - 1)
        list[i].p = Math.max(0, Math.min(1, p))
        const moved = list[i]
        list.sort((a, b) => a.p - b.p)
        root.current = list.indexOf(moved)
        root.edited(list)
    }
    function colourAt(p) {
        const list = stops ?? []
        if (list.length === 0) return "#ff808080"
        if (p <= list[0].p) return list[0].c
        for (let i = 1; i < list.length; ++i)
            if (p <= list[i].p) {
                const a = Qt.color(list[i - 1].c), b = Qt.color(list[i].c)
                const t = list[i].p > list[i - 1].p ? (p - list[i - 1].p) / (list[i].p - list[i - 1].p) : 0
                return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t)
            }
        return list[list.length - 1].c
    }
    function addStop() {
        if (count >= 10) return
        const list = copy()
        const i = Math.min(current, list.length - 1)
        // Halfway to the next stop (or the previous one at the end), its colour there.
        const j = i + 1 < list.length ? i + 1 : i - 1
        const p = j >= 0 ? (list[i].p + list[j].p) / 2 : 0.5
        const c = Qt.color(colourAt(p))
        list.push({ p: p, c: withAlpha(Qt.rgba(c.r, c.g, c.b, 1).toString(), c.a), b: 0 })
        const added = list[list.length - 1]
        list.sort((a, b) => a.p - b.p)
        root.current = list.indexOf(added)
        root.edited(list)
    }
    function removeStop() {
        if (count <= 2) return
        const list = copy()
        list.splice(Math.min(current, list.length - 1), 1)
        root.current = Math.max(0, Math.min(current, list.length - 1))
        root.edited(list)
    }
    // The stop's colour with its brightness, as drawn.
    function shown(s) {
        const c = Qt.color(s.c), b = s.b ?? 0
        if (b > 0) return Qt.rgba(c.r + (1 - c.r) * b, c.g + (1 - c.g) * b, c.b + (1 - c.b) * b, c.a)
        if (b < 0) return Qt.rgba(c.r * (1 + b), c.g * (1 + b), c.b * (1 + b), c.a)
        return c
    }

    Label { text: qsTr("Gradient stops"); color: Theme.textSecondary; font.pixelSize: Theme.fsLabel }
    Item {
        id: bar
        objectName: "gradientStopsBar"
        Layout.fillWidth: true
        implicitHeight: Theme.hControl + Theme.s3
        Rectangle {
            id: strip
            x: Theme.s2; width: parent.width - 2 * Theme.s2
            height: Theme.hControl * 0.7
            radius: Theme.rControl
            border.color: Theme.border; border.width: Theme.hairline
            clip: true
            Canvas {
                id: canvas
                anchors.fill: parent; anchors.margins: Theme.hairline
                property var list: root.stops
                onListChanged: requestPaint()
                onWidthChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    // Squares under see-through stops.
                    for (let x = 0; x < width; x += 6)
                        for (let y = 0; y < height; y += 6) {
                            ctx.fillStyle = ((x + y) / 6) % 2 ? "#c8c8c8" : "#f4f4f4"
                            ctx.fillRect(x, y, 6, 6)
                        }
                    const g = ctx.createLinearGradient(0, 0, width, 0)
                    for (const s of (root.stops ?? [])) g.addColorStop(Math.max(0, Math.min(1, s.p)), root.shown(s))
                    ctx.fillStyle = g
                    ctx.fillRect(0, 0, width, height)
                }
            }
        }
        Repeater {
            model: root.count
            delegate: Rectangle {
                id: marker
                required property int index
                readonly property var s: root.stops[index]
                objectName: "gradientStop" + index
                width: Theme.s3 + 2; height: Theme.s3 + 4
                y: strip.height - 2
                x: strip.x + (dragArea.drag.active ? x0 : s.p * strip.width) - width / 2
                property real x0: 0
                radius: 2
                color: root.shown(s)
                border.width: index === root.current ? 2 : 1
                border.color: index === root.current ? Theme.accent : Theme.borderStrong
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Stop %1 at %2%").arg(index + 1).arg(Math.round(s.p * 100))
                MouseArea {
                    id: dragArea
                    anchors.fill: parent
                    anchors.margins: -3
                    cursorShape: Qt.SizeHorCursor
                    property real startX: 0
                    property bool moved: false
                    onPressed: (e) => { root.current = marker.index; startX = e.x; marker.x0 = marker.s.p * strip.width; moved = false }
                    onPositionChanged: (e) => { if (pressed) { marker.x0 = Math.max(0, Math.min(strip.width, marker.x0 + e.x - startX)); moved = true } }
                    onReleased: if (moved) root.moveTo(marker.x0 / strip.width)
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.s1
        Label { Layout.fillWidth: true; text: qsTr("Stop %1 of %2").arg(Math.min(root.current, root.count - 1) + 1).arg(root.count); color: Theme.textSecondary; font.pixelSize: Theme.fsLabel }
        Button { objectName: "gradientStopAdd"; icon.name: "plus"; enabled: root.count < 10; onClicked: root.addStop()
                 ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Add gradient stop"); Accessible.name: qsTr("Add gradient stop") }
        Button { objectName: "gradientStopRemove"; icon.name: "minus"; enabled: root.count > 2; onClicked: root.removeStop()
                 ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Remove gradient stop"); Accessible.name: qsTr("Remove gradient stop") }
    }
    FieldRow {
        label: qsTr("Colour")
        ColorField {
            value: root.rgbOf(root.stop.c)
            accessibleName: qsTr("Stop colour")
            onPicked: v => root.change({ c: root.withAlpha(v, root.alphaOf(root.stop.c)) })
        }
    }
    FieldRow {
        label: qsTr("Position")
        NumField {
            objectName: "gradientStopPosition"
            Layout.fillWidth: true
            suffix: " %"; step: 1
            value: Math.round(root.stop.p * 100)
            onCommitted: v => root.moveTo(Math.max(0, Math.min(100, v)) / 100)
        }
    }
    FieldRow {
        label: qsTr("Transparency")
        Slider {
            objectName: "gradientStopTransparency"
            Layout.fillWidth: true
            from: 0; to: 100
            value: Math.round((1 - root.alphaOf(root.stop.c)) * 100)
            onPressedChanged: if (!pressed) root.change({ c: root.withAlpha(root.rgbOf(root.stop.c), 1 - value / 100) })
            Accessible.name: qsTr("Stop transparency")
        }
        Rectangle {
            implicitWidth: Theme.s5 * 2 + Theme.s2
            implicitHeight: Theme.hControl
            radius: Theme.rControl
            color: Theme.controlBg
            border.color: Theme.border; border.width: Theme.hairline
            Label { anchors.centerIn: parent; text: Math.round((1 - root.alphaOf(root.stop.c)) * 100) + "%"; font.family: Theme.monoFamily }
        }
    }
    FieldRow {
        label: qsTr("Brightness")
        NumField {
            objectName: "gradientStopBrightness"
            Layout.fillWidth: true
            suffix: " %"; step: 1
            value: Math.round((root.stop.b ?? 0) * 100)
            onCommitted: v => root.change({ b: Math.max(-100, Math.min(100, v)) / 100 })
        }
    }
}

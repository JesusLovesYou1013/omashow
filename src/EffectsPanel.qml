import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// EFFECTS-UI-1: Glow and Shadow for what is selected, in the Style tab.
// BOTH-EFFECTS-1: "Apply to" Text, Box or Both; Both shows the two sets
// (EffectsSet.qml), each with its own settings. A text box always offers all
// three: with no fill and no outline its box effects are still its own (saved
// and written to PowerPoint apart from the words'), drawn on the words where
// the words have none of that kind, with a hint to give the box a fill or an
// outline.
//
// A text box's effects go on its words (PowerPoint's Text Effects: a:rPr
// a:effectLst). A shape or picture has its own (Shape Effects: p:spPr
// a:effectLst). When the selection holds both, as a PowerPoint text box with
// a fill or an outline does (OmaShow keeps the box and its words as two
// grouped objects), "Apply to" picks one; the box's glow then follows its
// outline, the way PowerPoint draws it. A text box with no fill and no outline
// has no box to glow: PowerPoint itself puts box effects on the words then.
//
// Each effect is one collapsed row until opened: presets first (PowerPoint's
// Glow Variations and Shadow presets, drawn by the slide renderer), then the
// settings. Sizes are in points, as PowerPoint shows them; transparency in
// percent. Every change, a preset included, is one undo step.
ColumnLayout {
    id: root
    objectName: "effectsPanel"
    spacing: Theme.s2

    property bool glowOpen: false
    property bool shadowOpen: false
    property bool glowPresetsOpen: false
    property bool shadowPresetsOpen: false
    // "text", "shape" or "both"; empty follows the default (the words).
    property string chosen: ""

    readonly property var textState: { backend.selection; return backend.effectState("text") }
    readonly property var shapeState: { backend.selection; return backend.effectState("shape") }
    readonly property bool hasText: (textState.count ?? 0) > 0
    readonly property bool hasShape: (shapeState.count ?? 0) > 0
    readonly property string mode: hasText ? (chosen || "text") : "shape"

    visible: backend.hasSelection && (hasText || hasShape)

    property string selectedFor: ""
    Connections {
        target: backend
        function onSelectionChanged() {
            if (backend.selectedId !== root.selectedFor) { root.selectedFor = backend.selectedId; root.chosen = "" }
        }
    }

    SectionLabel { text: qsTr("EFFECTS") }

    // A text box: which the settings below change.
    FieldRow {
        visible: root.hasText
        label: qsTr("Apply to")
        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            Button {
                objectName: "effectsTargetText"
                Layout.fillWidth: true
                text: qsTr("Text"); checkable: true; checked: root.mode === "text"
                onClicked: root.chosen = "text"
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                ToolTip.text: qsTr("Glow and shadow around the letters (PowerPoint's Text Effects)")
            }
            Button {
                objectName: "effectsTargetBox"
                Layout.fillWidth: true
                text: qsTr("Box"); checkable: true; checked: root.mode === "shape"
                onClicked: root.chosen = "shape"
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                ToolTip.text: qsTr("Glow and shadow on the box: along its outline, or its whole shape when filled (PowerPoint's Shape Effects)")
            }
            Button {
                objectName: "effectsTargetBoth"
                Layout.fillWidth: true
                text: qsTr("Both"); checkable: true; checked: root.mode === "both"
                onClicked: root.chosen = "both"
                ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                ToolTip.text: qsTr("The text's and the box's effects, each with its own settings")
            }
        }
    }

    EffectsSet {
        objectName: "effectsSetText"
        Layout.fillWidth: true
        visible: root.mode === "text" || root.mode === "both"
        target: "text"
        heading: root.mode === "both" ? qsTr("Text effects") : ""
        glowOpen: root.glowOpen; shadowOpen: root.shadowOpen
        glowPresetsOpen: root.glowPresetsOpen; shadowPresetsOpen: root.shadowPresetsOpen
    }
    Rectangle {
        visible: root.mode === "both"
        Layout.fillWidth: true
        implicitHeight: Theme.hairline
        color: Theme.border
    }
    EffectsSet {
        objectName: "effectsSetBox"
        Layout.fillWidth: true
        visible: root.mode === "shape" || root.mode === "both"
        target: "shape"
        heading: root.mode === "both" ? qsTr("Box effects") : ""
        glowOpen: root.glowOpen; shadowOpen: root.shadowOpen
        glowPresetsOpen: root.glowPresetsOpen; shadowPresetsOpen: root.shadowPresetsOpen
    }
}

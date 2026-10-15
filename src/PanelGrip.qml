import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// The grip between a panel and the canvas. The panel follows the pointer
// exactly: each move reports how far the pointer is from where it was
// pressed, in scene coordinates (which do not move with the grip), and the
// owner sets the width to where it started plus that distance. Nothing is
// accumulated, so there is no speed-up; the owner saves only on release.
Item {
    id: grip
    property Item panel
    property bool fromLeft: true
    property bool collapsed: false
    // One-click hide/show (OS-008 arrow), kept alongside drag-to-resize.
    property bool showToggle: true
    signal resizeStarted()
    signal resizedBy(real total)     // pointer travel since the press, toward the canvas
    signal resizeFinished()
    signal toggled()
    // What the arrow says, and its name for tests; the editor's two panels keep theirs.
    property string showTip: fromLeft ? qsTr("Show the slide list") : qsTr("Show sidebar")
    property string hideTip: fromLeft ? qsTr("Hide the slide list") : qsTr("Hide sidebar")
    property string toggleName: fromLeft ? "navigatorToggleButton" : "sidebarToggleButton"
    objectName: "panelGrip"
    // Wide enough for the chevron; the rest of the strip is still the drag hit area.
    implicitWidth: Theme.s4
    Layout.fillHeight: true
    Rectangle { anchors.fill: parent; color: hover.hovered || drag.active ? Theme.accent : "transparent"; opacity: .35 }
    HoverHandler { id: hover; cursorShape: Qt.SplitHCursor }
    DragHandler {
        id: drag
        target: null
        yAxis.enabled: false
        // One pixel: the panel moves at once (the default waited ~10 px, then
        // jumped). The travel before it starts still counts, so the edge
        // lands under the pointer. A still double-click stays a double-click.
        dragThreshold: 1
        // Kept while dragging even when the pointer is past a limit and off the grip.
        cursorShape: Qt.SplitHCursor
        function follow() {
            const dx = centroid.scenePosition.x - centroid.scenePressPosition.x
            grip.resizedBy(grip.fromLeft ? dx : -dx)
        }
        // The move that starts the drag counts too, or the edge trails a step behind.
        onActiveChanged: if (active) { grip.resizeStarted(); follow() } else grip.resizeFinished()
        onCentroidChanged: if (active) follow()
    }
    // Visible arrow: click toggles collapse; drag still works on the bar around it.
    Button {
        id: toggleBtn
        objectName: grip.toggleName
        visible: grip.showToggle
        anchors.centerIn: parent
        z: 1
        implicitWidth: grip.width
        implicitHeight: Theme.s5 * 2
        flat: true
        focusPolicy: Qt.NoFocus
        display: AbstractButton.IconOnly
        // Point toward where the panel will go (out when shown, in when hidden).
        icon.name: grip.fromLeft
                   ? (grip.collapsed ? "chevron-right" : "chevron-left")
                   : (grip.collapsed ? "chevron-left" : "chevron-right")
        icon.width: Theme.szIcon
        icon.height: Theme.szIcon
        readonly property string tip: grip.collapsed ? grip.showTip : grip.hideTip
        onClicked: grip.toggled()
        ToolTip.visible: hovered
        ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: tip
        Accessible.name: tip
    }
    TapHandler { objectName: "gripTap"; onDoubleTapped: grip.toggled() }
}

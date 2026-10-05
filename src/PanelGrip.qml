import QtQuick
import QtQuick.Layouts
import Omashow 1.0

// The grip between a panel and the canvas. The panel follows the pointer
// exactly: each move reports how far the pointer is from where it was
// pressed, in scene coordinates (which do not move with the grip), and the
// owner sets the width to where it started plus that distance. Nothing is
// accumulated, so there is no speed-up; the owner saves only on release.
Item {
    id: grip
    property bool fromLeft: true
    property bool collapsed: false
    readonly property bool dragging: drag.active
    signal resizeStarted()
    signal resizedBy(real total)     // pointer travel since the press, toward the canvas
    signal resizeFinished()
    signal toggled()
    implicitWidth: Theme.s2
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
    TapHandler { objectName: "gripTap"; onDoubleTapped: grip.toggled() }
}

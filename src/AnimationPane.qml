import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// ANIM-001: the animation pane. Every animation on the slide in play order,
// numbered by the click it waits for. Read-only: a row only selects (the
// animation and its object); everything that changes an animation lives on the
// Animate tab, so nothing is in two places.
Rectangle {
    id: pane
    objectName: "animationPane"
    color: Theme.panelBg
    // Set by the window: the selected row and how an animation is named.
    property int row: -1
    property var nameOf: (b) => ""
    signal rowChosen(int index, string targetId)
    readonly property var builds: backend.builds
    readonly property real span: Math.max(0.5, backend.slideDuration)
    function tone(b) { return b.cls === 0 ? Theme.success : b.cls === 1 ? Theme.warning : b.cls === 2 ? Theme.danger : b.cls === 3 ? Theme.accent : Theme.textMuted }
    Rectangle { anchors.left: parent.left; width: Theme.hairline; height: parent.height; color: Theme.border }
    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.hairline
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.hCardHeader
            Layout.leftMargin: Theme.s4
            Layout.rightMargin: Theme.s3
            Label { text: qsTr("Animations"); font.pixelSize: Theme.fsSection; font.weight: Theme.wHeading }
            Item { Layout.fillWidth: true }
            Label { text: pane.builds.length ? ((n) => n === 1 ? qsTr("1 click") : qsTr("%1 clicks").arg(n))(pane.builds.reduce((n, b) => Math.max(n, b.click), 0)) : ""; color: Theme.textMuted; font.pixelSize: Theme.fsLabel }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: Theme.hairline; color: Theme.border }
        Label {
            visible: pane.builds.length === 0
            Layout.fillWidth: true; Layout.margins: Theme.s4
            wrapMode: Text.Wrap; color: Theme.textMuted
            text: qsTr("Nothing on this slide moves yet. Select an object, then choose an effect on the Animate tab.")
        }
        ListView {
            id: list
            objectName: "animationPaneList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: pane.builds
            currentIndex: pane.row
            keyNavigationEnabled: true
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Animations in play order")
            delegate: ItemDelegate {
                id: rowItem
                required property var modelData
                required property int index
                objectName: "animRow" + index
                width: ListView.view.width
                height: Theme.hTrack
                highlighted: pane.row === index || (pane.row < 0 && backend.selectedIds.indexOf(modelData.targetId) >= 0)
                // Only a row that starts a click carries its number; With and
                // After previous belong to the click above them.
                readonly property bool startsClick: modelData.trigger === 1
                onClicked: pane.rowChosen(index, modelData.targetId)
                Accessible.name: (startsClick ? qsTr("Click %1: ").arg(modelData.click) : "") + pane.nameOf(modelData) + ", " + String(modelData.label).replace(/\s+/g, " ")
                contentItem: RowLayout {
                    spacing: Theme.s2
                    Label { Layout.preferredWidth: Theme.s5; horizontalAlignment: Text.AlignRight; text: rowItem.startsClick ? rowItem.modelData.click : ""
                            font.family: Theme.monoFamily; color: Theme.textSecondary }
                    Rectangle { implicitWidth: Theme.s2; implicitHeight: Theme.s2; radius: width / 2; color: pane.tone(rowItem.modelData) }
                    Label { Layout.preferredWidth: Theme.wInspector * 0.28; elide: Text.ElideRight; text: pane.nameOf(rowItem.modelData); color: Theme.textPrimary }
                    Label { Layout.fillWidth: true; elide: Text.ElideRight; maximumLineCount: 1; text: String(rowItem.modelData.label).replace(/\s+/g, " "); color: Theme.textSecondary }
                    // Where it sits in the slide's time, and how long it takes.
                    Item {
                        Layout.preferredWidth: Theme.wInspector * 0.22
                        Layout.fillHeight: true
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            x: parent.width * Math.min(1, rowItem.modelData.start / pane.span)
                            width: Math.max(3, parent.width * Math.min(1, rowItem.modelData.duration / pane.span))
                            height: Theme.s2; radius: 2
                            color: Theme.withAlpha(pane.tone(rowItem.modelData), 0.7)
                        }
                    }
                }
            }
        }
    }
}

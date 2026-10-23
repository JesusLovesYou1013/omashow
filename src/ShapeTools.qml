import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0
ColumnLayout {
    id: root
    property var sel: backend.selection
    spacing: Theme.s2
    SectionLabel { text: qsTr("SHAPE") }
    ComboBox { objectName: "shapeKind"; Layout.fillWidth: true; model: backend.shapeNames; currentIndex: root.sel.shapeKind ?? 0; displayText: root.sel.shapeKind===99?qsTr("Editable path"):currentText; onActivated: backend.setSelectedProperty("shapeKind",currentIndex) }
    // FILL-UI-1: the fill and the line moved to the Style tab's Fill and Line (FillPanel.qml).
    // The shadow moved to the Style tab's Effects (EffectsSet.qml, EFFECTS-UI-1).
    Button { objectName: "convertPath"; Layout.fillWidth: true; visible: root.sel.shapeKind!==99; text: qsTr("Convert to editable path"); onClicked: backend.convertToPath() }
    RowLayout {
        visible: root.sel.shapeKind===99
        Button { text: qsTr("Close path"); onClicked: backend.setPathClosed(true) }
        Button { text: qsTr("Open path"); onClicked: backend.setPathClosed(false) }
    }
}

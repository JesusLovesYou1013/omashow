import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// One of the inspector's three groups (Fill and Line, Effects, Text
// Fill and Outline): a folding header, as the Saved styles row, with what is
// set in one line while it is closed. Open or closed is kept per session in
// the inspector's styleOpen (closed at first).
ColumnLayout {
    id: group
    property string title: ""
    property string summary: ""
    property string key: ""
    property QtObject opener: null
    default property alias content: body.data
    readonly property bool open: opener ? opener.isOpen(key) : false
    spacing: Theme.s2

    Button {
        objectName: group.objectName + "Header"
        Layout.fillWidth: true
        flat: true
        icon.name: group.open ? "chevron-down" : "chevron-right"
        text: !group.open && group.summary ? group.title + " · " + group.summary : group.title
        font.weight: Theme.wHeading
        Accessible.name: group.title
        Accessible.description: group.summary
        onClicked: if (group.opener) group.opener.toggle(group.key)
    }
    ColumnLayout {
        id: body
        visible: group.open
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s1
        spacing: Theme.s2
    }
}

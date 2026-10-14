import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// FONT-SETTINGS-1 (Daniel): everything about the free fonts behind one button
// on the Design tab, so the ribbon carries one entry instead of several:
// whether decks are checked on opening, the Extra fonts pack, what has been
// downloaded, and deleting it.
Sheet {
    id: root
    objectName: "fontSettingsDialog"
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 560)
    modal: true
    title: qsTr("Fonts")
    background: Rectangle { color: Theme.panelBg; border.color: Theme.border; radius: Theme.rControl }
    Overlay.modal: Rectangle { color: Theme.withAlpha(Theme.showBg, .65) }

    // Opens the Extra fonts pack view (the download, its progress, cancel).
    signal packRequested()

    property var rows: []
    property bool listed: false
    property bool confirming: false
    readonly property real megabytes: rows.reduce((n, r) => n + (r.bytes || 0), 0) / 1048576

    function show() { rows = backend.downloadedFonts(); listed = false; confirming = false; open() }
    Connections {
        target: backend
        enabled: root.visible
        function onPackFinished() { root.rows = backend.downloadedFonts() }
    }

    ColumnLayout {
        width: parent.width; spacing: Theme.s3

        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textSecondary
            text: qsTr("Presentations made in PowerPoint use Microsoft Office fonts. OmaShow draws them with free, openly licensed look-alikes (not Microsoft's fonts), kept in its own folder. Your system fonts are not changed.")
        }

        ColumnLayout {
            spacing: 0
            CheckBox {
                objectName: "fontSettingsCheck"
                text: qsTr("Check fonts when a presentation opens")
                checked: backend.fontCheckOnOpen
                onToggled: backend.fontCheckOnOpen = checked
            }
            CheckBox {
                objectName: "fontSettingsAuto"
                enabled: backend.fontCheckOnOpen
                text: qsTr("Always download missing free fonts")
                checked: backend.fontAutoDownload
                onToggled: backend.fontAutoDownload = checked
            }
            CheckBox {
                objectName: "fontSettingsPack"
                enabled: backend.fontCheckOnOpen
                text: qsTr("Extra fonts pack")
                checked: backend.extraFontsPack
                onToggled: { backend.extraFontsPack = checked; if (checked) { root.close(); root.packRequested() } }
            }
            Label {
                Layout.leftMargin: Theme.s5; Layout.fillWidth: true; wrapMode: Text.Wrap
                color: Theme.textSecondary; font.pixelSize: Theme.fsBase - 1
                text: qsTr("Free look-alikes for every Office font, so nothing has to be fetched when a presentation opens.")
            }
        }

        // What is here, and the list of it (the list's room is always kept).
        Label {
            objectName: "fontSettingsListToggle"
            text: (root.listed ? "▾ " : "▸ ") + qsTr("%n downloaded font(s)", "", root.rows.length) + qsTr(" · %1 MB").arg(root.megabytes.toFixed(0))
            color: Theme.accent
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.listed = !root.listed }
        }
        ListView {
            objectName: "fontSettingsList"
            visible: root.listed
            Layout.fillWidth: true
            Layout.preferredHeight: 200
            clip: true
            model: root.rows
            spacing: Theme.s1
            ScrollBar.vertical: ScrollBar {}
            delegate: Label {
                required property var modelData
                width: ListView.view.width - Theme.s2
                elide: Text.ElideRight
                text: qsTr("%1 · %2 MB").arg(modelData.family).arg((modelData.bytes / 1048576).toFixed(1))
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2
            Label {
                objectName: "fontSettingsFolder"
                text: qsTr("Fonts folder")
                font.pixelSize: Theme.fsBase - 1; color: Theme.textSecondary; font.underline: true
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: backend.openFontSandbox() }
            }
            Item { Layout.fillWidth: true }
            // Deleting asks once more, in the same place.
            Button {
                objectName: "fontSettingsDelete"
                enabled: root.rows.length > 0
                text: root.confirming ? qsTr("Yes, delete %n font(s)", "", root.rows.length) : qsTr("Delete downloaded fonts…")
                onClicked: {
                    if (!root.confirming) { root.confirming = true; return }
                    backend.deleteDownloadedFonts(); root.confirming = false; root.rows = backend.downloadedFonts()
                }
            }
            Button {
                objectName: "fontSettingsDownload"
                visible: !root.confirming
                text: backend.packRunning ? qsTr("Show download…") : qsTr("Download all fonts…")
                onClicked: { root.close(); root.packRequested() }
            }
            Button {
                objectName: "fontSettingsKeep"
                visible: root.confirming
                text: qsTr("Keep them")
                onClicked: root.confirming = false
            }
            Button { objectName: "fontSettingsClose"; text: qsTr("Close"); highlighted: true; onClicked: root.close() }
        }
    }
}

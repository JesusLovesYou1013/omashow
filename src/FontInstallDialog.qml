import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// Free fonts that stand in for Office fonts, offered for installing. Never
// installs without the person saying so here. Fonts from Arch packages go
// through pacman (it asks for the password); the rest are downloaded into the
// person's own font folder. The deck is then redrawn with them.
Sheet {
    id: root
    objectName: "fontInstallDialog"
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 640)
    modal: true
    title: pack ? qsTr("Extra fonts pack") : qsTr("Free fonts for this deck")
    standardButtons: rows.length > 0 ? Dialog.Ok | Dialog.Cancel : Dialog.Close
    background: Rectangle { color: Theme.panelBg; border.color: Theme.border; radius: Theme.rControl }
    Overlay.modal: Rectangle { color: Theme.withAlpha(Theme.showBg, .65) }

    property var rows: []
    property bool pack: false

    function showDeck() { pack = false; rows = backend.fontsToInstall(); open() }
    function showPack() { pack = true; rows = backend.fontPack(); open() }

    onOpened: { const ok = standardButton(Dialog.Ok); if (ok) { ok.text = qsTr("Install"); ok.objectName = "installFonts" } }
    onAccepted: backend.installFonts(rows)

    function from(row) {
        if (row.package && backend.fontInstallMode() === "system") return qsTr("Arch package %1").arg(row.package)
        if (row.googleFonts) return qsTr("Google Fonts")
        if (row.github) return qsTr("GitHub (%1)").arg(row.github.split("/").slice(0, 2).join("/"))
        return qsTr("Arch package %1").arg(row.package)
    }

    ColumnLayout {
        width: parent.width; spacing: Theme.s3
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textSecondary
            text: root.rows.length === 0
                  ? qsTr("Every free match is installed already.")
                  : root.pack
                  ? qsTr("Free, openly licensed stand-ins for %1 Windows and Office fonts, measured to look and fit like them. Arch packages are installed with pacman and ask for your password; the others go into your own font folder. Nothing is installed until you choose Install.").arg(root.rows.length)
                  : qsTr("This deck uses Office fonts that are not on this computer. These free fonts are measured to look and fit like them. Arch packages are installed with pacman and ask for your password; the others go into your own font folder. Nothing is installed until you choose Install.")
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(list.implicitHeight, 360)
            clip: true
            ColumnLayout {
                id: list
                width: root.availableWidth
                spacing: Theme.s1
                Repeater {
                    model: root.rows
                    Label {
                        required property var modelData
                        required property int index
                        objectName: "installRow" + index
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("%1, for %2 · %3%4").arg(modelData.family)
                              .arg(modelData.offices.slice(0, 4).join(", ") + (modelData.offices.length > 4 ? "…" : ""))
                              .arg(root.from(modelData))
                              .arg(modelData.metric ? qsTr(" · same widths") : "")
                    }
                }
            }
        }
    }
}

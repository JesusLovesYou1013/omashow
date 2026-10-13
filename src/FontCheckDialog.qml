import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// FONT-CHECK-1: shown when a PowerPoint deck opens and some of the free fonts
// its Office fonts map to are not available, neither in OmaShow's own font
// folder nor on the system. It only informs: nothing is downloaded or
// installed from here. "Don't show again for this document" is remembered per
// source file (view.ini, fontCheckHidden/<sha1 of the path>).
Sheet {
    id: root
    objectName: "fontCheckDialog"
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 640)
    modal: true
    title: qsTr("Free fonts this document needs")
    standardButtons: Dialog.Close
    background: Rectangle { color: Theme.panelBg; border.color: Theme.border; radius: Theme.rControl }
    Overlay.modal: Rectangle { color: Theme.withAlpha(Theme.showBg, .65) }

    property var rows: []
    property string source: ""
    property string checkedSource: ""

    // Called on every import report change; opens once per opened document.
    function check(report) {
        const src = report.source ?? ""
        if (src === "" || src === checkedSource) return
        checkedSource = src
        const needed = report.fontsToInstall ?? []
        if (needed.length === 0 || backend.fontCheckHidden(src)) return
        source = src; rows = needed; hideAgain.checked = false
        open()
    }

    onClosed: if (hideAgain.checked) backend.setFontCheckHidden(source, true)

    function from(row) {
        const where = []
        if (row.googleFonts) where.push(qsTr("Google Fonts"))
        if (row.github) where.push(qsTr("GitHub %1").arg(row.github.split("/").slice(0, 2).join("/")))
        if (row.package) where.push(qsTr("Arch package %1").arg(row.package))
        return where.join(qsTr(" or "))
    }

    ColumnLayout {
        width: parent.width; spacing: Theme.s3
        Label {
            objectName: "fontCheckIntro"
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textSecondary
            text: qsTr("This document uses Microsoft Office fonts. OmaShow draws them with free, openly licensed alternatives, measured to look and fit like them. These are not Microsoft's fonts. The alternatives below are not on this computer yet, so a nearby font is used for now.")
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(list.implicitHeight, 300)
            clip: true
            ColumnLayout {
                id: list
                width: root.availableWidth
                spacing: Theme.s2
                Repeater {
                    model: root.rows
                    ColumnLayout {
                        required property var modelData
                        required property int index
                        objectName: "fontCheckRow" + index
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true; wrapMode: Text.Wrap; font.bold: true
                            text: qsTr("%1 (free) — replaces %2").arg(modelData.family)
                                  .arg(modelData.offices.slice(0, 4).join(", ") + (modelData.offices.length > 4 ? "…" : ""))
                        }
                        Label {
                            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textSecondary
                            text: (modelData.metric ? qsTr("Same letter widths as the Office font. ") : qsTr("Similar look, sized to fit. "))
                                  + (root.from(modelData) ? qsTr("Get it from %1.").arg(root.from(modelData)) : "")
                        }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textSecondary
            text: qsTr("Put the font files (.ttf or .otf) in OmaShow's own font folder. Only OmaShow reads it, so your system fonts are not changed:")
        }
        Label {
            objectName: "fontCheckFolder"
            Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.family: "monospace"
            text: backend.fontSandboxDir()
        }
        RowLayout {
            Layout.fillWidth: true
            CheckBox { id: hideAgain; objectName: "fontCheckHide"; text: qsTr("Don't show again for this document") }
            Item { Layout.fillWidth: true }
            Button { objectName: "fontCheckReload"; text: qsTr("Check again"); onClicked: { backend.reloadFontSandbox(); root.rows = backend.fontsToInstall(); if (root.rows.length === 0) root.close() } }
            Button { objectName: "fontCheckOpenFolder"; text: qsTr("Open fonts folder"); onClicked: backend.openFontSandbox() }
        }
    }
}

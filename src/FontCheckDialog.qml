import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

// FONT-CHECK-1/2: shown when a deck (PowerPoint or a saved OmaShow deck)
// names Office fonts whose free look-alikes are not here. One click downloads
// them into OmaShow's own font folder (never the system's) and the open deck
// is redrawn with them in place; or carry on with sized fallbacks.
Sheet {
    id: root
    objectName: "fontCheckDialog"
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(parent.width - Theme.s5 * 2, 600)
    modal: true
    closePolicy: phase === "progress" ? Popup.NoAutoClose : Popup.CloseOnEscape
    title: qsTr("Free fonts for this presentation")
    background: Rectangle { color: Theme.panelBg; border.color: Theme.border; radius: Theme.rControl }
    Overlay.modal: Rectangle { color: Theme.withAlpha(Theme.showBg, .65) }

    property var rows: []
    property string source: ""
    property string phase: "offer"        // offer | progress | result
    property var failed: ({})
    property bool offline: false
    property bool details: false
    readonly property var downloadable: rows.filter(r => r.downloadable)
    readonly property real megabytes: downloadable.reduce((n, r) => n + (r.bytes || 0), 0) / 1048576

    // From backend.fontCheckRequested (a deck was opened). Never during a show:
    // the window holds it until the show ends.
    function check(src, needed) {
        if (!src || needed.length === 0) return
        const fetchable = needed.filter(r => r.downloadable)
        if (fetchable.length === 0) return
        if (backend.fontAutoDownload) { backend.downloadFonts(fetchable.map(r => r.family)); return }
        if (backend.fontCheckHidden(src)) return
        source = src; rows = needed
        phase = "offer"; failed = {}; details = false; hideAgain.checked = false
        open()
    }
    function download(families) {
        phase = "progress"
        if (!backend.downloadFonts(families)) { phase = "result"; failed = ({}); offline = false }
    }

    onClosed: if (hideAgain.checked && source) backend.setFontCheckHidden(source, true)

    Connections {
        target: backend
        function onFontDownloadFinished(installed, failedMap, wasOffline, cancelled) {
            if (!root.visible) return
            if (cancelled) { root.phase = "offer"; return }
            root.failed = failedMap; root.offline = wasOffline
            root.rows = backend.fontsToInstall()
            if (Object.keys(failedMap).length === 0) root.close()
            else root.phase = "result"
        }
    }

    function rowState(row) {
        if (root.failed[row.family]) return qsTr("not installed: %1").arg(root.failed[row.family])
        if (!row.downloadable) return qsTr("no free download; a similar font is used")
        return row.metric ? qsTr("same letter widths") : qsTr("similar look, sized to fit")
    }

    ColumnLayout {
        width: parent.width; spacing: Theme.s3

        Label {
            objectName: "fontCheckIntro"
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: root.phase === "progress"
                  ? qsTr("Downloading free fonts… %1 of %2 MB").arg((backend.fontDownloadDone / 1048576).toFixed(1)).arg((backend.fontDownloadTotal / 1048576).toFixed(1))
                  : root.phase === "result"
                  ? (root.offline ? qsTr("Couldn't reach the font server. Check the internet connection and try again, or carry on with similar fonts.")
                     : Object.keys(root.failed).length === 0 ? qsTr("A download is already running; try again in a moment.")
                     : qsTr("Some fonts could not be installed. The presentation uses similar fonts for those."))
                  : qsTr("This presentation uses Microsoft Office fonts that aren't on this computer. OmaShow can download free, openly licensed look-alikes (not Microsoft's fonts) so it looks and fits as intended. Only OmaShow uses them; your system fonts are not changed.")
        }
        ProgressBar {
            objectName: "fontCheckProgress"
            visible: root.phase === "progress"
            Layout.fillWidth: true
            from: 0; to: Math.max(1, backend.fontDownloadTotal); value: backend.fontDownloadDone
        }

        Label {
            objectName: "fontCheckDetailsToggle"
            visible: root.phase !== "progress"
            text: (root.details ? "▾ " : "▸ ") + qsTr("%n font(s)", "", root.rows.length)
            color: Theme.accent
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.details = !root.details }
        }
        ScrollView {
            visible: root.details || root.phase === "result"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(list.implicitHeight, 240)
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
                        objectName: "fontCheckRow" + index
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        color: root.failed[modelData.family] ? Theme.danger : Theme.textPrimary
                        text: qsTr("<b>%1</b> (free) for %2 · %3").arg(modelData.family)
                              .arg(modelData.offices.slice(0, 3).join(", ") + (modelData.offices.length > 3 ? "…" : ""))
                              .arg(root.rowState(modelData))
                    }
                }
            }
        }

        ColumnLayout {
            visible: root.phase === "offer"
            spacing: 0
            CheckBox { id: hideAgain; objectName: "fontCheckHide"; text: qsTr("Don't show again for this presentation") }
            CheckBox {
                objectName: "fontCheckAuto"
                text: qsTr("Always download missing free fonts")
                checked: backend.fontAutoDownload
                onToggled: backend.fontAutoDownload = checked
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2
            Label {
                objectName: "fontCheckOpenFolder"
                visible: root.phase !== "progress"
                text: qsTr("Fonts folder")
                font.pixelSize: Theme.fsBase - 1; color: Theme.textSecondary; font.underline: true
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: backend.openFontSandbox() }
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "fontCheckFallback"
                visible: root.phase !== "progress"
                text: qsTr("Use fallback fonts")
                onClicked: root.close()
            }
            Button {
                objectName: "fontCheckCancel"
                visible: root.phase === "progress"
                text: qsTr("Cancel")
                onClicked: backend.cancelFontDownload()
            }
            Button {
                objectName: "fontCheckDownload"
                visible: root.phase !== "progress" && root.downloadable.length > 0
                highlighted: true
                text: root.phase === "result" ? qsTr("Try again")
                      : qsTr("Download and install needed fonts (%1 MB)").arg(root.megabytes.toFixed(1))
                onClicked: root.download(root.downloadable.map(r => r.family))
            }
        }
    }
}

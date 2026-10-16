import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

Rectangle {
    id: root
    objectName: "startCentre"
    color: Theme.framePage
    property int themeIndex: 0
    property int layoutIndex: 0
    readonly property real deckWidth: dimensions.currentIndex === 4 ? customWidth.value : sizes[dimensions.currentIndex].w
    readonly property real deckHeight: dimensions.currentIndex === 4 ? customHeight.value : sizes[dimensions.currentIndex].h
    readonly property var sizes: [{w:1920,h:1080},{w:1440,h:1080},{w:1080,h:1920},{w:1080,h:1080},{w:1920,h:1080}]
    readonly property var themes: [qsTr("Midnight"),qsTr("Paper"),qsTr("Grove")]
    signal createRequested()
    signal openRequested()
    signal recentRequested(string path)
    signal templateRequested(string id)
    // START-PANE-1: the Recent presentations pane is dragged wider or narrower
    // at its divider, or put away with the arrow, as the editor's sidebars
    // are; both are kept for next time. The cards on the left keep their size
    // and move into more or fewer columns instead.
    readonly property real recentMin: 220
    property real recentWidth: Number(backend.panelState.startRecentWidth ?? Theme.wInspector)
    property bool recentCollapsed: (backend.panelState.startRecentCollapsed ?? "false") === "true"
    property real recentDragStart: 0
    function clampRecent(w) { return Math.max(recentMin, Math.min(w, root.width * .5)) }
    onRecentCollapsedChanged: backend.setPanelState("startRecentCollapsed", recentCollapsed)
    property string chosenTemplate: ""
    readonly property var chosen: backend.templates.find(row => row.id === chosenTemplate) ?? ({})
    // Search matches the name, where it came from and the typefaces it asks for.
    readonly property var shownTemplates: {
        const needle = templateSearch.text.trim().toLowerCase()
        const shape = templateAspect.currentIndex
        return backend.templates.filter(row => {
            if (shape === 1 && row.aspect !== "16:9") return false
            if (shape === 2 && row.aspect !== "4:3") return false
            if (shape === 3 && row.aspect !== qsTr("Square")) return false
            if (shape === 4 && row.aspect !== qsTr("Portrait")) return false
            if (needle.length === 0) return true
            const hay = (row.name + " " + row.source + " " + (row.fonts ?? []).join(" ")
                         + " " + (row.layouts ?? []).join(" ")).toLowerCase()
            return needle.split(/\s+/).every(word => hay.indexOf(word) >= 0)
        })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.s5
        spacing: Theme.s5
        RowLayout {
            Layout.fillWidth: true
            Label { text: Theme.appName; font.pixelSize: Theme.fsStartTitle; font.weight: Theme.wHeading }
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Back to deck"); visible: backend.hasDocument; onClicked: backend.resumeDeck() }
            Button { text: qsTr("Open deck…"); onClicked: root.openRequested() }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.s2
            ScrollView {
                id: templateScroll
                objectName: "templateScroll"
                Layout.fillWidth: true
                Layout.fillHeight: true
                // Never narrower than its widest row: a narrow page scrolls
                // sideways (scroll bar at the bottom) rather than cutting it off.
                // A fixed floor (the template search row's width): measuring the rows
                // themselves would make their width depend on itself.
                contentWidth: Math.max(availableWidth, Theme.wStartContentMin)
                clip: true
                readonly property bool overflowsX: contentWidth > availableWidth + 1
                // Shift + wheel goes sideways (a plain wheel stays up and down; a
                // touchpad's sideways swipe already goes sideways).
                WheelHandler {
                    objectName: "startSidewaysWheel"
                    enabled: templateScroll.overflowsX
                    acceptedModifiers: Qt.ShiftModifier
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (e) => {
                        const flick = templateScroll.contentItem
                        const d = e.angleDelta.y !== 0 ? e.angleDelta.y : e.angleDelta.x
                        flick.contentX = Math.max(0, Math.min(templateScroll.contentWidth - templateScroll.availableWidth, flick.contentX - d))
                    }
                }
                // The sideways bar shows while the pointer is over the page, so it
                // can be seen and dragged, and hides when it leaves (as the ribbon's).
                HoverHandler { id: templateHover }
                ScrollBar.horizontal: ScrollBar {
                    objectName: "startSidewaysBar"
                    parent: templateScroll
                    x: templateScroll.leftPadding
                    y: templateScroll.height - height
                    width: templateScroll.availableWidth
                    policy: ScrollBar.AlwaysOn
                    visible: templateScroll.overflowsX && (templateHover.hovered || pressed)
                }
                ColumnLayout {
                    width: templateScroll.contentWidth
                    spacing: Theme.s4
                    Label { text: qsTr("Make room for your next idea."); font.pixelSize: Theme.fsStartHeading; font.weight: Theme.wHeading }
                    Label { text: qsTr("Choose a theme. Your colours, fonts and layouts stay editable."); color: Theme.textSecondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Flow {
                        Layout.fillWidth: true
                        spacing: Theme.s3
                        Repeater {
                            model: root.themes
                            Button {
                                id: card
                                required property int index
                                required property string modelData
                                objectName: "startTheme" + index
                                width: Theme.wStartCard
                                height: Theme.hTemplateCard
                                checkable: true
                                checked: root.themeIndex === index
                                onClicked: root.themeIndex = index
                                background: Rectangle {
                                    color: card.hovered ? Theme.frameCardRaised : Theme.frameCard
                                    border.color: card.checked ? Theme.accent : Theme.border
                                    border.width: card.checked ? Theme.selectionRing : Theme.hairline
                                    radius: Theme.rControl
                                }
                                contentItem: ColumnLayout {
                                    spacing: Theme.s2
                                    Image {
                                        Layout.fillWidth: true; Layout.fillHeight: true
                                        source: "image://slides/start/" + card.index + "/" + root.deckWidth + "/" + root.deckHeight + "/" + root.layoutIndex
                                        sourceSize.width: Theme.wTemplatePreview
                                        fillMode: Image.PreserveAspectFit
                                    }
                                    Label { text: card.modelData; color: card.checked ? Theme.accent : Theme.textPrimary; font.weight: Theme.wHeading }
                                }
                            }
                        }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: Theme.hairline; color: Theme.border }
                    GridLayout {
                        columns: 2
                        Layout.fillWidth: true
                        columnSpacing: Theme.s4
                        Label { text: qsTr("SLIDE SIZE"); color: Theme.textMuted; font.pixelSize: Theme.fsLabel }
                        Label { text: qsTr("FIRST SLIDE"); color: Theme.textMuted; font.pixelSize: Theme.fsLabel }
                        ComboBox {
                            id: dimensions
                            objectName: "startDimensions"
                            Layout.fillWidth: true
                            model: [qsTr("Widescreen · 16:9"),qsTr("Standard · 4:3"),qsTr("Portrait · 9:16"),qsTr("Square · 1:1"),qsTr("Custom")]
                            // The size chosen last time is the one offered next time.
                            currentIndex: backend.startState.slideSize
                            onActivated: backend.setStartState("slideSize", currentIndex)
                        }
                        ComboBox {
                            objectName: "startLayout"
                            Layout.fillWidth: true
                            model: [qsTr("Title"),qsTr("Title and body"),qsTr("Blank")]
                            currentIndex: root.layoutIndex
                            onActivated: root.layoutIndex = currentIndex
                        }
                        RowLayout {
                            visible: dimensions.currentIndex === 4
                            Layout.columnSpan: 2
                            NumField { id: customWidth; objectName: "startWidth"; Layout.fillWidth: true; label: qsTr("W"); value: backend.startState.customWidth; onCommitted: v => { value = Math.max(240,Math.min(10000,v)); backend.setStartState("customWidth", value) } }
                            NumField { id: customHeight; objectName: "startHeight"; Layout.fillWidth: true; label: qsTr("H"); value: backend.startState.customHeight; onCommitted: v => { value = Math.max(240,Math.min(10000,v)); backend.setStartState("customHeight", value) } }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout {
                            Layout.fillWidth: true
                            Label { text: root.themes[root.themeIndex]; font.weight: Theme.wHeading }
                            Label { text: Math.round(root.deckWidth) + " × " + Math.round(root.deckHeight) + qsTr(" · 3 layouts"); color: Theme.textSecondary }
                        }
                        Button { objectName: "createDeckButton"; text: qsTr("Create presentation"); highlighted: true; onClicked: root.createRequested() }
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: qsTr("Original built-in themes · stored locally as .omashow decks")
                        color: Theme.textMuted; font.pixelSize: Theme.fsLabel
                    }

                    // ── Templates: decks kept to start from ──────────────────
                    Rectangle { Layout.fillWidth: true; implicitHeight: Theme.hairline; color: Theme.border }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Templates"); font.pixelSize: Theme.fsTitle; font.weight: Theme.wHeading }
                        Item { Layout.fillWidth: true }
                        TextField {
                            id: templateSearch
                            objectName: "templateSearch"
                            Layout.preferredWidth: 220
                            placeholderText: qsTr("Search templates")
                        }
                        ComboBox {
                            id: templateAspect
                            objectName: "templateAspect"
                            Layout.preferredWidth: 150
                            model: [qsTr("Any shape"), "16:9", "4:3", qsTr("Square"), qsTr("Portrait")]
                        }
                        Button { objectName: "installTemplate"; text: qsTr("Install…")
                                 onClicked: backend.installTemplateDialog() }
                    }
                    Label {
                        objectName: "templateNothing"
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textMuted
                        visible: root.shownTemplates.length === 0
                        text: qsTr("Nothing matches that. Install a deck as a template, or keep the one you are working on from File ▸ Keep as template.")
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: Theme.s3
                        Repeater {
                            model: root.shownTemplates
                            Rectangle {
                                id: templateCard
                                required property var modelData
                                required property int index
                                objectName: "template" + index
                                width: Theme.wStartCard
                                height: templateBody.implicitHeight + Theme.s3 * 2
                                radius: Theme.rCard
                                color: chosen ? Theme.frameCardRaised : Theme.frameCard
                                border.width: chosen ? Theme.selectionRing : Theme.hairline
                                border.color: chosen ? Theme.accent : Theme.border
                                readonly property bool chosen: root.chosenTemplate === modelData.id
                                TapHandler { onTapped: root.chosenTemplate = templateCard.modelData.id }
                                ColumnLayout {
                                    id: templateBody
                                    anchors.fill: parent
                                    anchors.margins: Theme.s3
                                    spacing: Theme.s1
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: width * 9 / 16
                                        radius: Theme.rControl
                                        // The preview takes the deck's shape inside a box that keeps its
                                        // own, so the card shows around it and no card changes size.
                                        color: templateCard.modelData.broken ? Theme.showBg : "transparent"
                                        clip: true
                                        Image {
                                            anchors.fill: parent
                                            fillMode: Image.PreserveAspectFit
                                            cache: false
                                            sourceSize.width: 360
                                            source: templateCard.modelData.broken ? ""
                                                    : "image://slides/template-at/" + root.deckWidth + "/" + root.deckHeight + "/" + encodeURIComponent(templateCard.modelData.id)
                                        }
                                        Label {
                                            anchors.centerIn: parent
                                            visible: templateCard.modelData.broken ?? false
                                            text: qsTr("Will not open"); color: Theme.accent
                                        }
                                    }
                                    Label { Layout.fillWidth: true; elide: Text.ElideRight
                                            text: templateCard.modelData.name; font.weight: Theme.wHeading }
                                    Label {
                                        Layout.fillWidth: true; elide: Text.ElideRight
                                        color: Theme.textSecondary; font.pixelSize: Theme.fsLabel
                                        text: templateCard.modelData.broken
                                              ? templateCard.modelData.error
                                              : qsTr("%1 · %2 · %3 layouts")
                                                .arg(templateCard.modelData.source)
                                                .arg(templateCard.modelData.aspect)
                                                .arg((templateCard.modelData.layouts ?? []).length)
                                    }
                                    Label {
                                        Layout.fillWidth: true; wrapMode: Text.Wrap
                                        color: Theme.accent; font.pixelSize: Theme.fsLabel
                                        visible: (templateCard.modelData.missingFonts ?? []).length > 0
                                        text: qsTr("Not installed here: %1")
                                              .arg((templateCard.modelData.missingFonts ?? []).join(", "))
                                    }
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: !!root.chosenTemplate
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label { text: root.chosen.name ?? ""; font.weight: Theme.wHeading }
                            Label {
                                Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textSecondary
                                font.pixelSize: Theme.fsLabel
                                text: qsTr("%1 · %2 masters · %3 · typefaces: %4")
                                      .arg(Math.round(root.deckWidth) + " × " + Math.round(root.deckHeight)).arg(root.chosen.masters ?? 0)
                                      .arg((root.chosen.layouts ?? []).join(", "))
                                      .arg((root.chosen.fonts ?? []).join(", "))
                            }
                        }
                        Button { objectName: "removeTemplate"; text: qsTr("Remove")
                                 visible: (root.chosen.source ?? "") === "installed"
                                 onClicked: { backend.removeTemplate(root.chosenTemplate); root.chosenTemplate = "" } }
                        Button {
                            objectName: "useTemplate"
                            text: qsTr("Use this template"); highlighted: true
                            enabled: !(root.chosen.broken ?? false)
                            onClicked: root.templateRequested(root.chosenTemplate)
                        }
                    }
                }
            }
            PanelGrip {
                objectName: "recentGrip"
                fromLeft: false
                collapsed: root.recentCollapsed
                showTip: qsTr("Show recent presentations")
                hideTip: qsTr("Hide recent presentations")
                toggleName: "recentToggleButton"
                // The divider line, under the grip's own highlight.
                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: Theme.hairline; height: parent.height; color: Theme.border; z: -1 }
                onResizeStarted: { root.recentCollapsed = false; root.recentDragStart = recentPane.width }
                onResizedBy: total => root.recentWidth = Math.round(root.clampRecent(root.recentDragStart + total))
                onResizeFinished: backend.setPanelState("startRecentWidth", Math.round(root.recentWidth))
                onToggled: root.recentCollapsed = !root.recentCollapsed
            }
            ColumnLayout {
                id: recentPane
                visible: !root.recentCollapsed
                Layout.preferredWidth: root.clampRecent(root.recentWidth)
                Layout.fillHeight: true
                spacing: Theme.s3
                Label { text: qsTr("Recent presentations"); font.pixelSize: Theme.fsTitle; font.weight: Theme.wHeading }
                Label {
                    visible: recentList.count === 0
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: qsTr("Saved and opened decks appear here. Pin the ones you use often.")
                    color: Theme.textMuted
                }
                ListView {
                    id: recentList
                    Layout.fillWidth: true; Layout.fillHeight: true
                    model: backend.recentFiles
                    clip: true; spacing: Theme.s2
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Rectangle {
                        id: recent
                        required property var modelData
                        required property int index
                        width: recentList.width
                        implicitHeight: recentContent.implicitHeight + Theme.s3 * 2
                        color: Theme.frameCard; radius: Theme.rControl
                        ColumnLayout {
                            id: recentContent
                            anchors.left: parent.left; anchors.right: parent.right
                            anchors.top: parent.top; anchors.margins: Theme.s3
                            spacing: Theme.s1
                            Button {
                                objectName: "recentDeck" + recent.index
                                Layout.fillWidth: true; flat: true
                                text: recent.modelData.name
                                enabled: !recent.modelData.missing
                                onClicked: root.recentRequested(recent.modelData.path)
                                // The name is how a deck is opened: bold, and the accent colour under the pointer.
                                ToolTip.visible: hovered; ToolTip.text: qsTr("Open %1").arg(recent.modelData.path)
                                HoverHandler { cursorShape: recent.modelData.missing ? Qt.ArrowCursor : Qt.PointingHandCursor }
                                contentItem: Label {
                                    // One line; the end gives way to "…" and comes back as the pane widens.
                                    text: parent.text; elide: Text.ElideRight; maximumLineCount: 1; verticalAlignment: Text.AlignVCenter
                                    font.weight: Theme.wHeading
                                    color: recent.modelData.missing ? Theme.textMuted : parent.hovered ? Theme.accent : Theme.textPrimary
                                }
                            }
                            Label { visible: recent.modelData.missing; text: qsTr("File missing"); color: Theme.warning }
                            RowLayout {
                                Button { flat: true; text: recent.modelData.pinned ? qsTr("Unpin") : qsTr("Pin"); onClicked: backend.pinRecent(recent.modelData.path,!recent.modelData.pinned) }
                                Button {
                                    flat: true; text: qsTr("Open folder")
                                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                                    ToolTip.text: qsTr("Show the folder this deck is in")
                                    onClicked: backend.revealRecent(recent.modelData.path)
                                }
                                Button { flat: true; text: qsTr("Remove"); onClicked: backend.removeRecent(recent.modelData.path) }
                            }
                        }
                    }
                }
            }
        }
    }
}

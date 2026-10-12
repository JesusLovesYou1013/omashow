import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omashow 1.0

ApplicationWindow {
    id: win
    width: 1440
    height: 900
    minimumWidth: 800   // OMASHOW-PERSONAL: ribbon-only prototype — tiles to ~820 px
    minimumHeight: 560
    visible: true
    // The title carries state: a dirty marker, the deck's name, and where you
    // are in it.
    title: (backend.modified ? "* " : "") + backend.fileName + " — "
           + qsTr("slide %1 of %2").arg(backend.currentSlide + 1).arg(backend.slideCount)
           + " — " + Theme.appName

    // What the body shows. 0 the slide, 2 the slide with its build timeline;
    // 1 masters, 3 review, 4 presenter setup and 6 the light table are rooms
    // visited from the slide and left with Done. (5 was Export, now a sheet.)
    property int workspace: 0
    readonly property bool inMode: workspace === 1 || workspace === 3 || workspace === 4 || workspace === 6
    // The sidebar beside the slide: 0 Format, 1 Document, 2 the animation pane
    // (ANIM-001; opened from the Animate tab). The cluster lights Format or
    // Document; the timeline view (workspace 2) is reached from the Animate tab.
    property int sidebar: 0
    readonly property int sidebarChoice: workspace === 0 ? (sidebar === 1 ? 2 : sidebar === 2 ? 3 : 0) : -1
    function chooseSidebar(choice) {
        win.commitEditors()
        win.sidebar = choice === 2 ? 1 : choice === 3 ? 2 : 0
        win.workspace = 0
        win.inspectorCollapsed = false
    }

    // ── Object animation (ANIM-001) ─────────────────────────────────────────
    // The pane's (or the timeline's) chosen row; -1 means "the selection's".
    property int animRow: -1
    property bool animAutoPreview: true
    property bool animPreviewing: false
    // What the Animate tab's Effect, Timing and Order controls act on: the
    // chosen row, else every animation of the selected objects.
    function animTargets() {
        const list = backend.builds
        if (win.animRow >= 0 && win.animRow < list.length) return [win.animRow]
        const ids = backend.selectedIds
        return list.filter(b => ids.indexOf(b.targetId) >= 0).map(b => b.index)
    }
    function animOn() { return win.animTargets().length > 0 }
    // The value every target shares, or undefined when they differ.
    function animCommon(key) {
        const list = backend.builds, rows = win.animTargets()
        if (!rows.length) return undefined
        const v = list[rows[0]][key]
        return rows.every(i => list[i][key] === v) ? v : undefined
    }
    function animAny(test) { const list = backend.builds; return win.animTargets().some(i => test(list[i])) }
    // The effect on the selection in one class, for the gallery's check mark.
    function animCurrent(cls) {
        const list = backend.builds, ids = backend.selectedIds
        const own = win.animRow >= 0 && win.animRow < list.length ? [list[win.animRow]] : list.filter(b => ids.indexOf(b.targetId) >= 0)
        const mine = own.filter(b => b.cls === cls)
        return mine.length && mine.every(b => b.effect === mine[0].effect) ? mine[0].effect : -1
    }
    readonly property var animEffectNames: ({ 1: qsTr("Fade"), 2: qsTr("Rise"), 3: qsTr("Media"), 4: qsTr("Move (older)"), 5: qsTr("Scale (older)"),
        6: qsTr("Spin (older)"), 7: qsTr("Pulse"), 8: qsTr("Reveal text"), 9: qsTr("Along a path"), 10: qsTr("Appear"), 11: qsTr("Fly"),
        12: qsTr("Float"), 13: qsTr("Zoom"), 14: qsTr("Spin"), 15: qsTr("Wipe"), 16: qsTr("Grow"), 17: qsTr("Shrink") })
    function animName(b) {
        if (!b || b.effect === undefined) return ""
        if (b.effect === 3) return qsTr("Media")
        if (b.effect === 10) return b.phase === 1 ? qsTr("Disappear") : qsTr("Appear")
        const name = win.animEffectNames[b.effect] ?? qsTr("Effect")
        return b.cls === 0 ? qsTr("%1 in").arg(name) : b.cls === 2 ? qsTr("%1 out").arg(name) : name
    }
    function applyAnim(cls, effect) {
        win.commitEditors()
        const index = backend.applyEffect(cls, effect)
        if (index < 0) return
        win.animRow = -1
        if (win.animAutoPreview) win.previewAnim(index)
    }
    function setAnim(key, value) {
        const rows = win.animTargets()
        if (rows.length) backend.setBuildsProperty(rows, key, value)
    }
    // Plays one animation (or the slide) where the slide is edited, then goes away.
    function previewAnim(index) {
        if (backend.reducedMotion) return
        if (win.workspace === 0) win.animPreviewing = true
        if (index >= 0) backend.previewBuild(index); else backend.previewSlide()
    }
    Connections {
        target: backend
        function onSelectionChanged() {
            const list = backend.builds
            if (win.animRow >= list.length || (win.animRow >= 0 && backend.selectedIds.indexOf(list[win.animRow].targetId) < 0)) win.animRow = -1
        }
        function onCurrentSlideChanged() { win.animRow = -1 }
        function onPlayingChanged() { if (!backend.playing) win.animPreviewing = false }
    }
    // Adding something to the slide brings the slide and its Format sidebar back.
    function toFormat() { win.commitEditors(); win.workspace = 0; win.sidebar = 0 }
    function leaveMode() { win.commitEditors(); win.workspace = 0 }
    function showExport() { win.commitEditors(); exportSheet.open() }
    readonly property bool slideFocus: (slideNavigator.activeFocus || slideSorter.activeFocus) && !textEntryFocused
    readonly property bool editing: workspace === 0 && !backend.startVisible
    readonly property bool presenting: presenter.running
    // A modal dialog owns the keyboard while it is up.
    // Any sheet in front of the deck, wherever it was declared: the shell's own
    // shortcuts stand down so the keyboard belongs to the dialog (see Sheet.qml).
    readonly property bool dialogOpen: Sheets.count > 0
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsControl
    // OMASHOW-PERSONAL: frame transparency / theme transparency-only
    color: Theme.frameWindow
    // Focused or not decides which of the two frame levels applies (see Theme.frameAlpha).
    onActiveChanged: Theme.frameActive = active

    // Presenter notes under the Edit canvas, as in the concept; View toggles it.
    // Closed until asked for, as in Keynote: the slide gets the room.
    property bool notesOpen: (backend.panelState.notesOpen ?? "false") === "true"
    // Panels: dragged wider or narrower, collapsed out of the way, and put back
    // where they were next time.
    property real navigatorWidth: backend.panelState.navigatorWidth ?? Theme.wNavigator
    property real inspectorWidth: backend.panelState.inspectorWidth ?? Theme.wInspector
    property bool navigatorCollapsed: (backend.panelState.navigatorCollapsed ?? "false") === "true"
    property bool inspectorCollapsed: (backend.panelState.inspectorCollapsed ?? "false") === "true"
    // OMASHOW-PERSONAL: fixed sidebar option (upstream #15 deferred) — when true,
    // the right panel stays at Theme.wInspectorFixed; drag still collapses via the
    // chevron (feat/sidebar-collapse) but does not change width.
    property bool inspectorFixed: (backend.panelState.inspectorFixed ?? "false") === "true"
    // Saved when a drag ends, not on every move: each save rewrites the panel
    // settings file.
    function savePanelWidths() {
        backend.setPanelState("navigatorWidth", Math.round(navigatorWidth))
        backend.setPanelState("inspectorWidth", Math.round(inspectorWidth))
    }
    onNavigatorCollapsedChanged: backend.setPanelState("navigatorCollapsed", navigatorCollapsed)
    onInspectorCollapsedChanged: backend.setPanelState("inspectorCollapsed", inspectorCollapsed)
    onInspectorFixedChanged: {
        backend.setPanelState("inspectorFixed", inspectorFixed)
        if (inspectorFixed) {
            inspectorWidth = Theme.wInspectorFixed
            savePanelWidths()
        }
    }
    onNotesOpenChanged: backend.setPanelState("notesOpen", notesOpen)
    // A panel dragged to nothing, or a remembered width from a bigger screen,
    // comes back to something usable rather than disappearing.
    function panelRoom() { return Math.max(320, win.width - 360) / 2 }
    function clampNavigator(w) { return Math.min(Math.max(Theme.wNavigatorMin, w), panelRoom()) }
    function clampInspector(w) {
        if (win.inspectorFixed) return Theme.wInspectorFixed
        return Math.min(Math.max(Theme.wInspectorMin, w), panelRoom())
    }
    function sanePanels() {
        const nav = clampNavigator(win.navigatorWidth), side = clampInspector(win.inspectorWidth)
        if (nav === win.navigatorWidth && side === win.inspectorWidth) return
        win.navigatorWidth = nav
        win.inspectorWidth = side
        win.savePanelWidths()
    }
    // The width a panel had when its grip was pressed.
    property real panelDragStart: 0
    onWidthChanged: win.sanePanels()

    // The grip between a panel and the canvas. The panel follows the pointer
    // exactly: each move reports how far the pointer is from where it was
    // pressed, in scene coordinates (which do not move with the grip), and the
    // owner sets the width to where it started plus that distance. Nothing is
    // accumulated, so there is no speed-up; the owner saves only on release.
    component PanelGrip: Item {
        id: grip
        property Item panel
        property bool fromLeft: true
        property bool collapsed: false
        // OMASHOW-PERSONAL: sidebar collapse chevron (related #15; drag kept)
        // One-click hide/show (OS-008 arrow), kept alongside drag-to-resize.
        property bool showToggle: true
        signal resizeStarted()
        signal resizedBy(real total)     // pointer travel since the press, toward the canvas
        signal resizeFinished()
        signal toggled()
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
            objectName: grip.fromLeft ? "navigatorToggleButton" : "sidebarToggleButton"
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
            readonly property string tip: grip.collapsed
                ? (grip.fromLeft ? qsTr("Show the slide list") : qsTr("Show sidebar"))
                : (grip.fromLeft ? qsTr("Hide the slide list") : qsTr("Hide sidebar"))
            onClicked: grip.toggled()
            ToolTip.visible: hovered
            ToolTip.delay: Theme.tooltipDelay
            ToolTip.text: tip
            Accessible.name: tip
        }
        TapHandler { objectName: "gripTap"; onDoubleTapped: grip.toggled() }
    }
    // Below this width the toolbar drops its labels and keeps the icons.
    readonly property bool compactToolbar: width < 1440
    readonly property string aspectName: {
        const w = backend.slideSize.width, h = backend.slideSize.height
        const ratio = h > 0 ? w / h : 0
        const named = [[16/9, qsTr("16:9 Wide")], [4/3, qsTr("4:3 Standard")], [16/10, qsTr("16:10")], [1, qsTr("1:1 Square")], [9/16, qsTr("9:16 Tall")]]
        for (const pair of named)
            if (Math.abs(ratio - pair[0]) < 0.01) return pair[1]
        return Math.round(w) + " × " + Math.round(h)
    }

    // A flat toolbar button: an icon, a label when there is room, a tooltip
    // that names the shortcut.
    component ToolAction: Button {
        property string tip: ""
        flat: true
        implicitHeight: Theme.hToolButton
        display: win.compactToolbar && icon.name !== "" ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
        ToolTip.visible: hovered && tip !== ""
        ToolTip.delay: Theme.tooltipDelay
        ToolTip.text: tip
        Accessible.name: text
    }
    component ToolbarRule: Rectangle {
        implicitWidth: Theme.hairline
        Layout.fillHeight: true
        Layout.topMargin: Theme.s3
        Layout.bottomMargin: Theme.s3
        Layout.leftMargin: Theme.s1
        Layout.rightMargin: Theme.s1
        color: Theme.border
    }
    // OMASHOW-PERSONAL: ribbon-only prototype (UI-RIBBON-003)
    // ONE command table. The ribbon's second row, the File and ? menus, the title-line
    // cluster and Ctrl+K are all built from it, so each command has one label, one
    // enabled rule and one handler. Where the menu bar and the toolbar both had a
    // command, the handler is the superset of their side effects (toFormat() =
    // commitEditors + workspace 0 + Format sidebar). No entry owns a key: every
    // binding stays a window-level Shortcut {}; `keys` is only the text shown.
    //   tab: home insert design arrange show review view | cluster | parent: menu owner
    //   kind: button | toggle (checked bound, toggled in run) | menu | layouts | languages
    property var ribbonCommands: [
        // ── File (drop-down) ──
        { id: "fileNewDeck", parent: "file", label: qsTr("New deck"), icon: "file-plus", also: "create start blank", run: () => win.confirmThenNew() },
        { id: "fileOpen", parent: "file", label: qsTr("Open…"), icon: "folder-open", keys: "Ctrl+O", also: "open a deck load file", run: () => win.confirmThenOpen() },
        { id: "fileStartCentre", parent: "file", label: qsTr("Start centre…"), icon: "house", also: "home recent templates", run: () => { win.commitEditors(); backend.showStart() } },
        { id: "newWindowMenuItem", parent: "file", label: qsTr("New window"), icon: "square-plus", also: "open another window second copy", run: () => backend.openInNewWindow() },
        { id: "fileSave", parent: "file", sep: true, label: qsTr("Save"), icon: "save", keys: "Ctrl+S", also: "write keep", run: () => { win.commitEditors(); backend.save() } },
        { id: "fileSaveAs", parent: "file", label: qsTr("Save as…"), keys: "Ctrl+Shift+S", also: "copy elsewhere", run: () => { win.commitEditors(); backend.saveAsDialog() } },
        { id: "keepAsTemplate", parent: "file", label: qsTr("Keep as template…"), also: "keep this deck as a template reuse starting point", on: () => !backend.startVisible, run: () => { win.commitEditors(); templateName.open() } },
        { id: "fileInstallTemplate", parent: "file", label: qsTr("Install a template…"), also: "template add pack", run: () => backend.installTemplateDialog() },
        { id: "fileExportPdf", parent: "file", sep: true, label: qsTr("Export PDF…"), icon: "file-output", keys: "Ctrl+E", also: "export a pdf document handout pages", run: () => { win.commitEditors(); backend.exportDialog({ kind: 0 }) } },
        { id: "exportMenuItem", parent: "file", label: qsTr("Export…"), icon: "share", also: "export pictures png jpeg images export film video movie mp4 print paper printer package the deck zip send share", run: () => win.showExport() },
        { id: "fileQuit", parent: "file", sep: true, label: qsTr("Quit"), keys: "Ctrl+Q", also: "exit close", run: () => win.close() },
        // ── Home ──
        { id: "ribbonPaste", tab: "home", group: "clipboard", label: qsTr("Paste"), icon: "clipboard-paste", keys: "Ctrl+V", on: () => backend.canPaste, run: () => backend.pasteAsync() },
        { id: "ribbonPasteSpecial", tab: "home", group: "clipboard", kind: "menu", label: qsTr("Paste special"), tip: qsTr("Keep or match formatting, the words only, or as a picture"), on: () => backend.canPaste },
        { id: "pasteKeeping", parent: "ribbonPasteSpecial", label: qsTr("Keep its formatting"), also: "paste keeping its formatting", on: () => backend.canPaste && (backend.clipboardKinds.objects ?? false), run: () => backend.pasteSpecial(0) },
        { id: "pasteMatching", parent: "ribbonPasteSpecial", label: qsTr("Match this deck's text style"), also: "paste matching this deck", on: () => backend.canPaste && (backend.clipboardKinds.objects ?? false), run: () => backend.pasteSpecial(1) },
        { id: "pasteWords", parent: "ribbonPasteSpecial", label: qsTr("The words only"), also: "paste the words only plain text unformatted", on: () => backend.canPaste && (backend.clipboardKinds.text ?? false), run: () => backend.pasteSpecial(2) },
        { id: "pastePicture", parent: "ribbonPasteSpecial", label: qsTr("As a picture"), also: "paste as a picture flatten image", on: () => backend.canPaste && (backend.clipboardKinds.objects ?? false), run: () => backend.pasteSpecial(3) },
        { id: "ribbonCut", tab: "home", group: "clipboard", label: qsTr("Cut"), icon: "scissors", keys: "Ctrl+X", on: () => backend.hasSelection, run: () => backend.copyAsync(true) },
        { id: "ribbonCopy", tab: "home", group: "clipboard", label: qsTr("Copy"), icon: "copy", keys: "Ctrl+C", on: () => backend.hasSelection, run: () => backend.copyAsync() },
        { id: "toolbarNewSlide", tab: "home", group: "slides", label: qsTr("New Slide"), icon: "square-plus", keys: "Ctrl+N", also: "add a slide new page", on: () => !backend.startVisible, run: () => backend.addSlide() },
        { id: "ribbonLayout", tab: "home", group: "slides", kind: "layouts", label: qsTr("Layout"), icon: "layout-template", tip: qsTr("Apply a layout to this slide"), also: "apply a layout master placeholder", on: () => !backend.startVisible,
          run: () => { win.commitEditors(); win.workspace = 1; layoutApplyDialog.show(backend.slideDesign.layoutId ?? "") } },
        { id: "ribbonDuplicateSlides", tab: "home", group: "slides", label: qsTr("Duplicate slides"), icon: "copy", also: "duplicate the selected slides copy page duplicate this slide", run: () => backend.duplicateSelectedSlides() },
        { id: "ribbonDeleteSlides", tab: "home", group: "slides", label: qsTr("Delete slides"), icon: "trash-2", also: "delete the selected slides remove page delete this slide", on: () => backend.slideSelectionState.canDelete, run: () => backend.deleteSelectedSlides() },
        { id: "importDeckMenuItem", tab: "home", group: "slides", label: qsTr("Import from deck…"), icon: "folder-open", tip: qsTr("Import slides (and their design) from another deck"), also: "import slides from another deck borrow steal reuse", on: () => !backend.startVisible, run: () => { win.commitEditors(); backend.importDeckDialog() } },
        { id: "ribbonDuplicate", tab: "home", group: "editing", label: qsTr("Duplicate"), keys: "Ctrl+D", also: "duplicate the selected objects", on: () => backend.hasSelection, run: () => backend.duplicateSelected() },
        { id: "ribbonDelete", tab: "home", group: "editing", label: qsTr("Delete"), icon: "trash-2", keys: "Del", also: "delete the selected objects remove", on: () => backend.hasSelection, run: () => backend.deleteSelected() },
        { id: "ribbonSelectAll", tab: "home", group: "editing", label: qsTr("Select all"), keys: "Ctrl+A", run: () => backend.selectAll() },
        // ── Insert ──
        { id: "insertText", tab: "insert", group: "text", label: qsTr("Text"), icon: "type", keys: "T", tip: qsTr("Text box  T"), also: "words box type", on: () => !backend.startVisible, run: () => { win.toFormat(); backend.addText() } },
        { id: "insertEquation", tab: "insert", group: "text", label: qsTr("Equation"), icon: "sigma", also: "an equation maths math formula latex fraction", on: () => !backend.startVisible, run: () => { win.workspace = 0; win.commitEditors(); backend.addEquation() } },
        { id: "insertShape", tab: "insert", group: "shapes", kind: "menu", label: qsTr("Shape"), icon: "shapes", tip: qsTr("Shapes, the pen and freehand  S adds a rectangle") },
        { id: "openShapeGallery", parent: "insertShape", label: qsTr("Shape gallery…"), icon: "shapes", also: "a shape rectangle circle arrow", on: () => !backend.startVisible, run: () => { win.toFormat(); shapeGallery.open() } },
        { id: "drawWithPen", parent: "insertShape", sep: true, label: qsTr("Draw with the pen"), icon: "pen-tool", also: "path bezier line vector", on: () => !backend.startVisible, run: () => { win.toFormat(); editCanvas.startPathTool(1) } },
        { id: "drawFreehand", parent: "insertShape", label: qsTr("Draw freehand"), icon: "pencil-line", also: "sketch scribble line", on: () => !backend.startVisible, run: () => { win.toFormat(); editCanvas.startPathTool(2) } },
        { id: "connectObjectsAction", tab: "insert", group: "shapes", label: qsTr("Connect objects"), icon: "spline", also: "connector line", on: () => backend.selectionCount === 2, run: () => backend.connectSelected() },
        { id: "insertPicture", tab: "insert", group: "objects", label: qsTr("Picture"), icon: "image", tip: qsTr("Insert a picture"), also: "a picture image photo png", on: () => !backend.startVisible, run: () => { win.toFormat(); backend.insertImageDialog() } },
        { id: "insertTable", tab: "insert", group: "objects", label: qsTr("Table"), icon: "table", tip: qsTr("Insert a table"), also: "a table grid rows columns", on: () => !backend.startVisible, run: () => { win.toFormat(); if (backend.addTable()) tableEditor.show() } },
        { id: "insertChart", tab: "insert", group: "objects", label: qsTr("Chart"), icon: "chart-column", tip: qsTr("Insert a chart"), also: "a chart graph bar line pie data", on: () => !backend.startVisible, run: () => { win.toFormat(); backend.addChart() } },
        { id: "insertDiagram", tab: "insert", group: "objects", label: qsTr("Diagram"), icon: "workflow", tip: qsTr("Insert a process or hierarchy diagram"), also: "a diagram process hierarchy flow", on: () => !backend.startVisible, run: () => { win.toFormat(); diagramDialog.show() } },
        { id: "mediaMenuButton", tab: "insert", group: "media", kind: "menu", label: qsTr("Media"), icon: "clapperboard", tip: qsTr("Audio and video") },
        { id: "insertEmbeddedMedia", parent: "mediaMenuButton", label: qsTr("Audio or video · embed…"), icon: "clapperboard", also: "insert film sound video audio movie clip embed film or sound", on: () => !backend.startVisible, run: () => { win.toFormat(); backend.insertMediaDialog(true) } },
        { id: "insertLinkedMedia", parent: "mediaMenuButton", label: qsTr("Audio or video · link…"), also: "insert film sound video audio movie clip link", on: () => !backend.startVisible, run: () => { win.toFormat(); backend.insertMediaDialog(false) } },
        { id: "objectLinkAction", tab: "insert", group: "links", label: qsTr("Link or action…"), icon: "link", also: "hyperlink jump url", on: () => backend.hasSelection, run: () => linkDialog.show() },
        // ── Design ──
        { id: "slideSizeAction", tab: "design", group: "size", label: qsTr("Slide size…"), icon: "scan", also: "change the slide size aspect ratio widescreen portrait", on: () => !backend.startVisible, run: () => { win.commitEditors(); slideSizeDialog.show() } },
        { id: "ribbonDesignMasters", tab: "design", group: "size", label: qsTr("Edit masters and layouts"), icon: "layout-template", also: "design theme master layout template remove what nothing uses unused tidy hygiene", run: () => { win.commitEditors(); win.workspace = 1 } },
        // ── Arrange ──
        { id: "ribbonGroup", tab: "arrange", group: "group", label: qsTr("Group"), icon: "group", keys: "Ctrl+G", on: () => backend.selectionCount > 1, run: () => backend.groupSelected() },
        { id: "ribbonUngroup", tab: "arrange", group: "group", label: qsTr("Ungroup"), icon: "ungroup", keys: "Ctrl+Shift+G", on: () => !!backend.selection.groupId, run: () => backend.ungroupSelected() },
        { id: "ribbonForward", tab: "arrange", group: "order", label: qsTr("Bring forward"), icon: "bring-to-front", on: () => backend.hasSelection, run: () => backend.raiseSelected(1) },
        { id: "ribbonBackward", tab: "arrange", group: "order", label: qsTr("Send backward"), icon: "send-to-back", on: () => backend.hasSelection, run: () => backend.raiseSelected(-1) },
        { id: "ribbonLock", tab: "arrange", group: "lock", label: qsTr("Lock selected"), icon: "lock", also: "lock the selected objects", on: () => backend.hasSelection, run: () => backend.setSelectedProperty("locked", true) },
        { id: "ribbonUnlockAll", tab: "arrange", group: "lock", label: qsTr("Unlock all"), icon: "lock-open", run: () => backend.unlockAllObjects() },
        { id: "ribbonHide", tab: "arrange", group: "lock", label: qsTr("Hide selected"), icon: "eye-off", also: "hide the selected objects", on: () => backend.hasSelection, run: () => backend.setSelectedProperty("hidden", true) },
        { id: "ribbonShowAll", tab: "arrange", group: "lock", label: qsTr("Show all"), icon: "eye", also: "show all objects", run: () => backend.showAllObjects() },
        { id: "combineShapesAction", tab: "arrange", group: "combine", label: qsTr("Combine shapes…"), also: "merge union subtract intersect", on: () => backend.selectionCount > 1, run: () => { win.commitEditors(); combineShapesDialog.open() } },
        { id: "textOnShapeAction", tab: "arrange", group: "combine", label: qsTr("Put the text on this shape"), icon: "spline", also: "put the text on a shape along path curve circle words", on: () => backend.selectionCount === 2, run: () => { win.commitEditors(); win.workspace = 0; backend.putTextOnShape() } },
        // ── Slide Show ──
        { id: "ribbonShowFromHere", tab: "show", group: "start", label: qsTr("Play from this slide"), icon: "play", also: "present from this slide show current", run: () => { win.commitEditors(); presenter.start(true, false) } },
        { id: "ribbonShowRehearse", tab: "show", group: "start", label: qsTr("Rehearse"), icon: "timer", also: "rehearse in a window practise timing", run: () => { win.commitEditors(); presenter.start(false, true) } },
        { id: "ribbonSkipSlides", tab: "show", group: "setup", kind: "toggle", label: qsTr("Skip in show"), icon: "eye-off", tip: qsTr("Skip the selected slides in the show (or include them again)"), also: "skip the selected slides hide omit include skip this slide in the show", checked: () => !!backend.slideSelectionState.allSkipped, run: () => backend.setSlidesSkipped(!backend.slideSelectionState.allSkipped) },
        { id: "ribbonShowSetup", tab: "show", group: "setup", label: qsTr("Presenter setup…"), icon: "monitor", also: "displays console timings custom shows", run: () => { win.commitEditors(); win.workspace = 4 } },
        { id: "openMediaPreflight", tab: "show", group: "setup", label: qsTr("Media preflight…"), also: "check media audio video before the show", run: () => mediaPreflight.open() },
        // ── Review ──
        { id: "toolbarReview", tab: "review", group: "review", label: qsTr("Review"), icon: "message-square-text", tip: qsTr("Comments, findings and spelling"), also: "comments findings outline statistics accessibility spelling check the spelling find and replace comment on this slide check what would stop someone reading this", run: () => { win.commitEditors(); win.workspace = 3 } },
        { id: "deckLanguageMenu", tab: "review", group: "proofing", kind: "languages", label: qsTr("Language"), icon: "languages", tip: qsTr("The deck is written in…"), also: "the deck is written in language dictionary spelling", run: () => win.ribbonTab = win.ribbonTabs.findIndex(t => t.key === "review") },
        { id: "smartPunctuationItem", tab: "review", group: "proofing", kind: "toggle", label: qsTr("Smart punctuation"), also: "quotes dashes ellipsis curly", checked: () => backend.smartPunctuation, run: () => backend.setSmartPunctuation(!backend.smartPunctuation) },
        // ── View ──
        { id: "lightTableMenuItem", tab: "view", group: "views", label: qsTr("Light table"), icon: "layout-grid", also: "sorter order overview grid", run: () => { win.commitEditors(); win.workspace = 6; slideSorter.focusBrowser() } },
        { id: "ribbonViewNotes", tab: "view", group: "views", kind: "toggle", label: qsTr("Presenter notes"), also: "speaker script", checked: () => win.notesOpen, run: () => win.notesOpen = !win.notesOpen },
        { id: "toggleNavigator", tab: "view", group: "panels", kind: "toggle", label: qsTr("Slide list"), also: "show the slide list hide the slide list navigator panel thumbnails sidebar", checked: () => !win.navigatorCollapsed, run: () => win.navigatorCollapsed = !win.navigatorCollapsed },
        { id: "toggleInspector", tab: "view", group: "panels", kind: "toggle", label: qsTr("Inspector"), also: "show the inspector hide the inspector panel properties sidebar", checked: () => !win.inspectorCollapsed, run: () => win.inspectorCollapsed = !win.inspectorCollapsed },
        { id: "toggleInspectorFixed", tab: "view", group: "panels", kind: "toggle", label: qsTr("Lock sidebar width"), also: "unlock sidebar width lock fixed inspector panel", checked: () => win.inspectorFixed, run: () => win.inspectorFixed = !win.inspectorFixed },
        { id: "resetPanels", tab: "view", group: "panels", label: qsTr("Put the panels back"), also: "reset widths restore layout",
          run: () => { win.navigatorCollapsed = false; win.inspectorCollapsed = false; win.navigatorWidth = Theme.wNavigator; win.inspectorWidth = Theme.wInspector; win.inspectorFixed = false; win.savePanelWidths() } },
        { id: "ribbonZoomIn", tab: "view", group: "zoom", label: qsTr("Zoom in"), icon: "zoom-in", keys: "Ctrl++", on: () => win.editing, run: () => editCanvas.zoomBy(1.25) },
        { id: "ribbonZoomOut", tab: "view", group: "zoom", label: qsTr("Zoom out"), icon: "zoom-out", keys: "Ctrl+-", on: () => win.editing, run: () => editCanvas.zoomBy(1 / 1.25) },
        { id: "ribbonZoomFit", tab: "view", group: "zoom", label: qsTr("Fit slide"), icon: "maximize", keys: "Ctrl+0", on: () => win.editing, run: () => editCanvas.fit() },
        { id: "ribbonZoomActual", tab: "view", group: "zoom", label: qsTr("Actual size"), keys: "Ctrl+1", on: () => win.editing, run: () => editCanvas.actualSize() },
        { id: "ribbonSnap", tab: "view", group: "guides", kind: "toggle", label: qsTr("Snap to guides"), also: "align magnet", checked: () => backend.snapEnabled, run: () => backend.snapEnabled = !backend.snapEnabled },
        { id: "reducedMotionItem", tab: "view", group: "access", kind: "toggle", label: qsTr("Less movement"), also: "reduced motion animation accessibility vestibular allow movement again", checked: () => backend.reducedMotion, run: () => backend.reducedMotion = !backend.reducedMotion },
        { id: "highContrastItem", tab: "view", group: "access", kind: "toggle", label: qsTr("Stronger contrast"), also: "high contrast accessibility legible ordinary", checked: () => backend.highContrast, run: () => backend.highContrast = !backend.highContrast },
        // ── Animate (ANIM-001): the only place an animation is chosen, timed or ordered ──
        { id: "animPreview", tab: "animate", group: "preview", label: qsTr("Preview"), icon: "play", also: "play animation build preview slide",
          tip: qsTr("Play the chosen animation, or the whole slide when none is chosen"), on: () => backend.builds.length > 0,
          run: () => { win.commitEditors(); if (backend.playing) { backend.pause(); return } const r = win.animTargets(); win.previewAnim(r.length ? r[0] : -1) } },
        { id: "animAutoPreview", tab: "animate", group: "preview", kind: "toggle", label: qsTr("Auto preview"), also: "play when applied",
          tip: qsTr("Play an effect once when you choose it"), checked: () => win.animAutoPreview, run: () => win.animAutoPreview = !win.animAutoPreview },
        { id: "animEntrance", tab: "animate", group: "effect", kind: "menu", on: () => backend.selectionCount > 0, label: qsTr("Entrance"), icon: "eye", tip: qsTr("How the selected objects arrive. Choosing another replaces it.") },
        { id: "animEntranceAppear", parent: "animEntrance", label: qsTr("Appear"), also: "entrance build in show", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(0) === 10, run: () => win.applyAnim(0, 10) },
        { id: "animEntranceFade", parent: "animEntrance", label: qsTr("Fade in"), also: "entrance build in dissolve", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(0) === 1, run: () => win.applyAnim(0, 1) },
        { id: "animEntranceFly", parent: "animEntrance", label: qsTr("Fly in"), also: "entrance build in slide from edge", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(0) === 11, run: () => win.applyAnim(0, 11) },
        { id: "animEntranceFloat", parent: "animEntrance", label: qsTr("Float in"), also: "entrance build in rise drift", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(0) === 12, run: () => win.applyAnim(0, 12) },
        { id: "animEntranceZoom", parent: "animEntrance", label: qsTr("Zoom in"), also: "entrance build in grow scale", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(0) === 13, run: () => win.applyAnim(0, 13) },
        { id: "animEntranceSpin", parent: "animEntrance", label: qsTr("Spin in"), also: "entrance build in turn rotate", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(0) === 14, run: () => win.applyAnim(0, 14) },
        { id: "animEntranceWipe", parent: "animEntrance", label: qsTr("Wipe in"), also: "entrance build in reveal edge", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(0) === 15, run: () => win.applyAnim(0, 15) },
        { id: "animEntranceReveal", parent: "animEntrance", sep: true, label: qsTr("Reveal text"), also: "entrance by paragraph word letter typewriter", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(0) === 8, run: () => win.applyAnim(0, 8) },
        { id: "animEmphasis", tab: "animate", group: "effect", kind: "menu", on: () => backend.selectionCount > 0, label: qsTr("Emphasis"), icon: "sparkles", tip: qsTr("Draw attention to the selected objects while they stay. Adds one each time.") },
        { id: "animEmphasisPulse", parent: "animEmphasis", label: qsTr("Pulse"), also: "emphasis swell throb", on: () => backend.selectionCount > 0, run: () => win.applyAnim(1, 7) },
        { id: "animEmphasisGrow", parent: "animEmphasis", label: qsTr("Grow"), also: "emphasis bigger enlarge", on: () => backend.selectionCount > 0, run: () => win.applyAnim(1, 16) },
        { id: "animEmphasisShrink", parent: "animEmphasis", label: qsTr("Shrink"), also: "emphasis smaller", on: () => backend.selectionCount > 0, run: () => win.applyAnim(1, 17) },
        { id: "animExit", tab: "animate", group: "effect", kind: "menu", on: () => backend.selectionCount > 0, label: qsTr("Exit"), icon: "eye-off", tip: qsTr("How the selected objects leave. Choosing another replaces it.") },
        { id: "animExitDisappear", parent: "animExit", label: qsTr("Disappear"), also: "exit build out hide", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(2) === 10, run: () => win.applyAnim(2, 10) },
        { id: "animExitFade", parent: "animExit", label: qsTr("Fade out"), also: "exit build out dissolve", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(2) === 1, run: () => win.applyAnim(2, 1) },
        { id: "animExitFly", parent: "animExit", label: qsTr("Fly out"), also: "exit build out leave edge", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(2) === 11, run: () => win.applyAnim(2, 11) },
        { id: "animExitFloat", parent: "animExit", label: qsTr("Float out"), also: "exit build out drift", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(2) === 12, run: () => win.applyAnim(2, 12) },
        { id: "animExitZoom", parent: "animExit", label: qsTr("Zoom out"), also: "exit build out shrink scale", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(2) === 13, run: () => win.applyAnim(2, 13) },
        { id: "animExitSpin", parent: "animExit", label: qsTr("Spin out"), also: "exit build out turn rotate", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(2) === 14, run: () => win.applyAnim(2, 14) },
        { id: "animExitWipe", parent: "animExit", label: qsTr("Wipe out"), also: "exit build out edge", on: () => backend.selectionCount > 0, checked: () => win.animCurrent(2) === 15, run: () => win.applyAnim(2, 15) },
        { id: "animMotion", tab: "animate", group: "effect", kind: "menu", on: () => backend.selectionCount > 0, label: qsTr("Motion"), icon: "route", tip: qsTr("Move the selected objects along a route") },
        { id: "animMotionPath", parent: "animMotion", label: qsTr("Along a path"), also: "motion path route follow travel", on: () => backend.selectionCount > 0, run: () => win.applyAnim(3, 9) },
        { id: "buildPath", tab: "animate", group: "effect", kind: "paths", label: qsTr("Path guide"), icon: "spline",
          tip: qsTr("The shape or drawn path a motion follows. Draw one with the pen and hide it; the object still follows it."), on: () => win.animAny(b => b.effect === 9) },
        { id: "animOptions", tab: "animate", group: "effect", kind: "menu", label: qsTr("Effect options"), icon: "settings", tip: qsTr("Direction, text steps, path and pace of the chosen animations"), on: () => win.animOn() },
        { id: "animFromBottom", parent: "animOptions", label: qsTr("From the bottom"), also: "direction effect options", on: () => win.animAny(b => b.effect === 11 || b.effect === 12 || b.effect === 15), checked: () => win.animCommon("direction") === 0, run: () => win.setAnim("direction", 0) },
        { id: "animFromLeft", parent: "animOptions", label: qsTr("From the left"), also: "direction effect options", on: () => win.animAny(b => b.effect === 11 || b.effect === 12 || b.effect === 15), checked: () => win.animCommon("direction") === 1, run: () => win.setAnim("direction", 1) },
        { id: "animFromRight", parent: "animOptions", label: qsTr("From the right"), also: "direction effect options", on: () => win.animAny(b => b.effect === 11 || b.effect === 12 || b.effect === 15), checked: () => win.animCommon("direction") === 2, run: () => win.setAnim("direction", 2) },
        { id: "animFromTop", parent: "animOptions", label: qsTr("From the top"), also: "direction effect options", on: () => win.animAny(b => b.effect === 11 || b.effect === 12 || b.effect === 15), checked: () => win.animCommon("direction") === 3, run: () => win.setAnim("direction", 3) },
        { id: "animByParagraph", parent: "animOptions", sep: true, label: qsTr("By paragraph"), also: "reveal text one step is", on: () => win.animAny(b => b.effect === 8), checked: () => win.animCommon("unit") === 0, run: () => win.setAnim("unit", 0) },
        { id: "animByWord", parent: "animOptions", label: qsTr("By word"), also: "reveal text one step is", on: () => win.animAny(b => b.effect === 8), checked: () => win.animCommon("unit") === 1, run: () => win.setAnim("unit", 1) },
        { id: "animByLetter", parent: "animOptions", label: qsTr("By letter"), also: "reveal text character typewriter", on: () => win.animAny(b => b.effect === 8), checked: () => win.animCommon("unit") === 2, run: () => win.setAnim("unit", 2) },
        { id: "buildPathReverse", parent: "animOptions", sep: true, label: qsTr("Travel the other way"), also: "path reverse direction", on: () => win.animAny(b => b.effect === 9), checked: () => win.animCommon("pathReverse") === true, run: () => win.setAnim("pathReverse", win.animCommon("pathReverse") !== true) },
        { id: "buildPathOrient", parent: "animOptions", label: qsTr("Turn with the path"), also: "path orient rotate", on: () => win.animAny(b => b.effect === 9), checked: () => win.animCommon("orient") === true, run: () => win.setAnim("orient", win.animCommon("orient") !== true) },
        { id: "animEaseLinear", parent: "animOptions", sep: true, label: qsTr("Even speed"), also: "easing linear pace", on: () => win.animAny(b => b.effect !== 3 && b.effect !== 10), checked: () => win.animCommon("easing") === Easing.Linear, run: () => win.setAnim("easing", Easing.Linear) },
        { id: "animEaseOut", parent: "animOptions", label: qsTr("Slow down at the end"), also: "easing ease out decelerate", on: () => win.animAny(b => b.effect !== 3 && b.effect !== 10), checked: () => win.animCommon("easing") === Easing.OutCubic, run: () => win.setAnim("easing", Easing.OutCubic) },
        { id: "animEaseGentle", parent: "animOptions", label: qsTr("Gentle stop"), also: "easing soft", on: () => win.animAny(b => b.effect !== 3 && b.effect !== 10), checked: () => win.animCommon("easing") === Easing.OutQuad, run: () => win.setAnim("easing", Easing.OutQuad) },
        { id: "animEaseInOut", parent: "animOptions", label: qsTr("Smooth start and stop"), also: "easing ease in and out", on: () => win.animAny(b => b.effect !== 3 && b.effect !== 10), checked: () => win.animCommon("easing") === Easing.InOutCubic, run: () => win.setAnim("easing", Easing.InOutCubic) },
        { id: "animStart", tab: "animate", group: "timing", kind: "menu", label: qsTr("Start"), icon: "mouse-pointer-2", on: () => win.animOn(),
          text: () => { const v = win.animCommon("trigger"); return v === undefined ? qsTr("Start") : [qsTr("At a time"), qsTr("On click"), qsTr("With previous"), qsTr("After previous")][v] },
          tip: qsTr("When the chosen animations start") },
        { id: "animStartClick", parent: "animStart", label: qsTr("On click"), also: "start trigger next", on: () => win.animOn(), checked: () => win.animCommon("trigger") === 1, run: () => win.setAnim("trigger", 1) },
        { id: "animStartWith", parent: "animStart", label: qsTr("With previous"), also: "start trigger together same time", on: () => win.animOn(), checked: () => win.animCommon("trigger") === 2, run: () => win.setAnim("trigger", 2) },
        { id: "animStartAfter", parent: "animStart", label: qsTr("After previous"), also: "start trigger then", on: () => win.animOn(), checked: () => win.animCommon("trigger") === 3, run: () => win.setAnim("trigger", 3) },
        { id: "animStartAtTime", parent: "animStart", label: qsTr("At a time"), also: "start trigger seconds absolute", on: () => win.animOn(), checked: () => win.animCommon("trigger") === 0, run: () => win.setAnim("trigger", 0) },
        { id: "buildDuration", tab: "animate", group: "timing", kind: "number", label: qsTr("Duration"), suffix: " s", step: 0.1, tip: qsTr("How long the chosen animations take"),
          on: () => win.animAny(b => b.effect !== 3 && b.effect !== 10), value: () => win.animCommon("duration"), set: v => win.setAnim("duration", v) },
        { id: "buildDelay", tab: "animate", group: "timing", kind: "number", label: qsTr("Delay"), suffix: " s", step: 0.1, tip: qsTr("How long the chosen animations wait after their start"),
          on: () => win.animOn(), value: () => win.animCommon("delay"), set: v => win.setAnim("delay", v) },
        { id: "animEarlier", tab: "animate", group: "order", label: qsTr("Earlier"), icon: "arrow-up", tip: qsTr("Play the chosen animation one place earlier"), also: "move earlier reorder animation sooner up",
          on: () => { const r = win.animTargets(); return r.length === 1 && r[0] > 0 }, run: () => { const i = win.animTargets()[0]; backend.moveBuild(i, i - 1); win.animRow = i - 1 } },
        { id: "animLater", tab: "animate", group: "order", label: qsTr("Later"), icon: "arrow-down", tip: qsTr("Play the chosen animation one place later"), also: "move later reorder animation down",
          on: () => { const r = win.animTargets(); return r.length === 1 && r[0] < backend.builds.length - 1 }, run: () => { const i = win.animTargets()[0]; backend.moveBuild(i, i + 1); win.animRow = i + 1 } },
        { id: "animRemove", tab: "animate", group: "order", label: qsTr("Remove"), icon: "trash-2", also: "delete animation build none",
          tip: qsTr("Remove the chosen animations (the objects stay)"), on: () => win.animOn(), run: () => { backend.removeBuilds(win.animTargets()); win.animRow = -1 } },
        { id: "animPane", tab: "animate", group: "show", kind: "toggle", label: qsTr("Animation pane"), icon: "list", also: "list order sequence builds sidebar",
          checked: () => win.sidebarChoice === 3, run: () => win.chooseSidebar(win.sidebarChoice === 3 ? 0 : 3) },
        { id: "animTimeline", tab: "animate", group: "show", kind: "toggle", label: qsTr("Timeline"), icon: "timer", also: "animate view tracks scrub transition seconds",
          tip: qsTr("The slide with its animations laid out in time, and the slide transition"), checked: () => win.workspace === 2,
          run: () => { win.commitEditors(); win.workspace = win.workspace === 2 ? 0 : 2 } },
        // ── The always-visible cluster on the title line ──
        { id: "commandSearchMenuItem", tab: "cluster", pal: qsTr("Help"), label: qsTr("Find a command"), icon: "search", keys: "Ctrl+K", also: "search palette", run: () => { win.commitEditors(); commandPalette.show() } },
        { id: "toolbarUndo", tab: "cluster", pal: qsTr("Edit"), label: qsTr("Undo"), icon: "undo-2", keys: "Ctrl+Z", also: "back mistake", on: () => backend.canUndo, run: () => backend.undo() },
        { id: "toolbarRedo", tab: "cluster", pal: qsTr("Edit"), label: qsTr("Redo"), icon: "redo-2", keys: "Ctrl+Shift+Z", also: "forward again", on: () => backend.canRedo, run: () => backend.redo() },
        { id: "toolbarPlay", tab: "cluster", pal: qsTr("Slide Show"), label: qsTr("Play"), icon: "play", keys: "F5", tip: qsTr("Play from the start  F5"), also: "present from the start show play full screen", run: () => win.startShow() },
        { id: "sidebarFormat", tab: "cluster", pal: qsTr("View"), kind: "toggle", label: qsTr("Format"), icon: "pencil", also: "edit canvas slide style inspector", checked: () => win.sidebarChoice === 0, run: () => win.chooseSidebar(0) },
        { id: "sidebarDocument", tab: "cluster", pal: qsTr("View"), kind: "toggle", label: qsTr("Document"), icon: "file-text", also: "theme and slide size deck settings language properties", checked: () => win.sidebarChoice === 2, run: () => win.chooseSidebar(2) },
        { id: "ribbonHelpButton", tab: "cluster", kind: "menu", label: "?", tip: qsTr("Help: the guide and keyboard shortcuts") },
        { id: "helpGuideMenuItem", parent: "ribbonHelpButton", label: qsTr("OmaShow guide"), icon: "info", keys: "F1", also: "help manual explain how what does sections parts", run: () => { win.commitEditors(); helpGuide.open() } },
        { id: "helpShortcutsMenuItem", parent: "ribbonHelpButton", label: qsTr("Keyboard shortcuts"), icon: "keyboard", keys: "?", also: "keys help", run: () => helpSheet.open() }
    ]
    readonly property var ribbonTabs: [
        { key: "home", label: qsTr("Home") }, { key: "insert", label: qsTr("Insert") }, { key: "design", label: qsTr("Design") },
        { key: "arrange", label: qsTr("Arrange") }, { key: "animate", label: qsTr("Animate") }, { key: "show", label: qsTr("Slide Show") }, { key: "review", label: qsTr("Review") },
        { key: "view", label: qsTr("View") }
    ]
    readonly property var ribbonGroupLabels: ({
        clipboard: qsTr("Clipboard"), slides: qsTr("Slides"), editing: qsTr("Editing"), text: qsTr("Text"), shapes: qsTr("Shapes"),
        objects: qsTr("Objects"), media: qsTr("Media"), links: qsTr("Links"), size: qsTr("Size and masters"), group: qsTr("Group"),
        order: qsTr("Order"), lock: qsTr("Lock and hide"), combine: qsTr("Combine"), start: qsTr("Start"), setup: qsTr("Set up"),
        review: qsTr("Review"), proofing: qsTr("Proofing"), views: qsTr("Views"), panels: qsTr("Panels"), zoom: qsTr("Zoom"),
        guides: qsTr("Guides"), access: qsTr("Accessibility"), preview: qsTr("Preview"), effect: qsTr("Effect"), timing: qsTr("Timing"),
        show: qsTr("Show") })
    // Ctrl+K's list, built when it opens (not re-built under the user on every selection change).
    function paletteCommands() {
        return ribbonCommands.filter(c => c.kind !== "menu" && !!c.run).map(c => ({
            group: paletteGroup(c), name: c.label, also: (c.also ?? "") + " " + (c.tip ?? ""),
            shortcut: c.keys ?? "", enabled: cmdOn(c), run: c.run }))
    }
    // A button reached with Tab inside a scrolled row comes into view.
    function revealInRow(item) {
        let row = item.parent
        while (row && !(row.objectName && row.objectName.indexOf("ribbonTabRow_") === 0)) row = row.parent
        if (!row) return
        const x = item.mapToItem(row.contentItem, 0, 0).x
        if (x < row.contentX) row.contentX = Math.max(0, x - Theme.s2)
        else if (x + item.width > row.contentX + row.width) row.contentX = Math.min(row.contentWidth - row.width, x + item.width - row.width + Theme.s2)
    }
    readonly property var cmd: { const m = ({}); for (const c of ribbonCommands) m[c.id] = c; return m }
    function cmdOn(c) { return !c.on || !!c.on() }
    function cmdsIn(tab, group) { return ribbonCommands.filter(c => c.tab === tab && c.group === group) }
    function childrenOf(owner) { return ribbonCommands.filter(c => c.parent === owner) }
    function groupsIn(tab) { const g = []; for (const c of ribbonCommands) if (c.tab === tab && g.indexOf(c.group) < 0) g.push(c.group); return g }
    function paletteGroup(c) {
        if (c.pal) return c.pal
        if (c.parent === "file") return qsTr("File")
        const owner = c.parent ? win.cmd[c.parent] : c
        const t = ribbonTabs.find(t => t.key === owner.tab)
        return t ? t.label : qsTr("Help")
    }
    // The tab on show in row 2. Home at start; a tab never closes and has no side effects.
    property int ribbonTab: 0

    // One ribbon control, from one table entry. Selection only enables or disables it.
    component RibbonButton: ToolAction {
        id: rb
        property var c
        objectName: c.id
        text: c.text ? c.text() : c.label
        icon.name: c.icon ?? ""
        display: icon.name === "" ? AbstractButton.TextOnly : AbstractButton.TextBesideIcon
        // Icon-only (the cluster below 1440 px) is a square, so ~820 px still fits.
        implicitWidth: display === AbstractButton.IconOnly ? implicitHeight
                       : Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        enabled: win.cmdOn(c)
        // Toggles: the binding owns `checked`; a click runs the command, which flips the source.
        checkable: false
        checked: c.kind === "toggle" && !!c.checked()
        readonly property bool opensMenu: c.kind === "menu" || c.kind === "layouts" || c.kind === "languages" || c.kind === "paths"
        tip: (c.tip ?? c.label) + (c.keys && !c.tip ? "  " + c.keys : "")
        focusPolicy: Qt.StrongFocus
        onActiveFocusChanged: if (activeFocus) win.revealInRow(rb)
        Accessible.role: c.kind === "toggle" ? Accessible.CheckBox : opensMenu ? Accessible.ButtonMenu : Accessible.Button
        Accessible.checkable: c.kind === "toggle"
        Accessible.checked: checked
        Accessible.description: c.tip ?? ""
        onClicked: {
            if (c.kind === "menu") menuLoader.item.popup(rb, 0, rb.height)
            else if (c.kind === "layouts") backend.design.layouts.length ? layoutsLoader.item.popup(rb, 0, rb.height) : backend.setupDesign()
            else if (c.kind === "languages") languagesLoader.item.popup(rb, 0, rb.height)
            else if (c.kind === "paths") pathsLoader.item.popup(rb, 0, rb.height)
            else c.run()
        }
        // Only the drop-down a button's kind needs is created.
        Loader { id: menuLoader; active: rb.c.kind === "menu"; sourceComponent: Component { RibbonMenu { owner: rb.c.id } } }
        Loader {
            id: layoutsLoader
            active: rb.c.kind === "layouts"
            sourceComponent: Component {
                Menu {
                    id: rbLayouts
                    Instantiator {
                        model: backend.design.layouts
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.name
                            checkable: true
                            checked: modelData.id === backend.slideDesign.layoutId
                            onTriggered: { win.commitEditors(); layoutApplyDialog.show(modelData.id) }
                        }
                        onObjectAdded: (index, object) => rbLayouts.insertItem(index, object)
                        onObjectRemoved: (index, object) => rbLayouts.removeItem(object)
                    }
                }
            }
        }
        // ANIM-001: the guide a motion follows, any other object on the slide.
        Loader {
            id: pathsLoader
            active: rb.c.kind === "paths"
            sourceComponent: Component {
                Menu {
                    id: rbPaths
                    Instantiator {
                        model: backend.slideObjects.filter(o => !win.animAny(b => b.effect === 9 && b.targetId === o.id))
                        delegate: MenuItem {
                            required property var modelData
                            text: String(modelData.name).replace(/\s+/g, " ")
                            checkable: true
                            checked: win.animCommon("pathId") === modelData.id
                            onTriggered: { win.setAnim("pathId", modelData.id); checked = Qt.binding(() => win.animCommon("pathId") === modelData.id) }
                        }
                        onObjectAdded: (index, object) => rbPaths.insertItem(index, object)
                        onObjectRemoved: (index, object) => rbPaths.removeItem(object)
                    }
                }
            }
        }
        Loader {
            id: languagesLoader
            active: rb.c.kind === "languages"
            sourceComponent: Component {
                Menu {
                    Repeater {
                        model: backend.spellingLanguages()
                        MenuItem {
                            required property string modelData
                            text: modelData
                            checkable: true
                            checked: backend.deckLanguage === modelData
                            onTriggered: backend.setDeckLanguage(modelData)
                        }
                    }
                    MenuItem {
                        objectName: "noDictionaries"
                        enabled: false
                        visible: backend.spellingLanguages().length === 0
                        height: visible ? implicitHeight : 0
                        text: qsTr("No dictionaries are installed")
                    }
                }
            }
        }
    }
    component RibbonMenuItem: MenuItem {
        property var c
        objectName: c.id
        text: c.keys ? c.label + "  " + c.keys : c.label
        icon.name: c.icon ?? ""
        enabled: win.cmdOn(c)
        // A choice (Start, direction, pace, current effect) shows its tick;
        // the source owns it, so the binding is put back after a click.
        checkable: !!c.checked
        checked: !!c.checked && c.checked()
        onTriggered: { c.run(); if (c.checked) checked = Qt.binding(() => !!c.checked && c.checked()) }
    }
    // ANIM-001: a number on the ribbon (Duration, Delay). Blank-ish when the
    // chosen animations disagree: it shows the first and says "mixed".
    component RibbonNumber: RowLayout {
        id: rn
        property var c
        spacing: Theme.s1
        enabled: win.cmdOn(c)
        Accessible.role: Accessible.Grouping
        Accessible.name: c.label
        readonly property var shared: c.value()
        Label { text: rn.shared === undefined && rn.enabled ? qsTr("%1 (mixed)").arg(rn.c.label) : rn.c.label; color: rn.enabled ? Theme.textSecondary : Theme.textMuted }
        NumField {
            objectName: rn.c.id
            Layout.preferredWidth: Theme.s5 * 4
            step: rn.c.step ?? 0.1
            suffix: rn.c.suffix ?? ""
            value: rn.shared ?? (win.animTargets().length ? backend.builds[win.animTargets()[0]][rn.c.id === "buildDuration" ? "duration" : "delay"] : 0)
            tabStop: true
            onCommitted: v => rn.c.set(v)
            HoverHandler { id: rnHover }
            ToolTip.visible: rnHover.hovered && (rn.c.tip ?? "") !== ""; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: rn.c.tip ?? ""
            Accessible.name: rn.c.label
        }
    }
    Component { id: ribbonMenuItemComp; RibbonMenuItem {} }
    Component { id: ribbonButtonComp; RibbonButton { c: parent.modelData } }
    Component { id: ribbonNumberComp; RibbonNumber { c: parent.modelData } }
    Component { id: ribbonMenuSepComp; MenuSeparator {} }
    // A drop-down filled from the table: every entry whose parent is `owner`, in order.
    component RibbonMenu: Menu {
        id: rm
        property string owner
        onOwnerChanged: fill()
        Component.onCompleted: fill()
        property bool filled: false
        function fill() {
            if (filled || owner === "") return
            filled = true
            for (const k of win.childrenOf(owner)) {
                if (k.sep && rm.count > 0) rm.addItem(ribbonMenuSepComp.createObject(win.contentItem))
                rm.addItem(ribbonMenuItemComp.createObject(win.contentItem, { c: k }))
            }
        }
    }
    component StatusText: Label {
        color: Theme.textSecondary
        font.pixelSize: Theme.fsLabel
    }
    component StatusRule: Rectangle {
        implicitWidth: Theme.hairline
        implicitHeight: Theme.s4
        color: Theme.border
    }
    component Hairline: Rectangle {
        color: Theme.border
        height: Theme.hairline
    }

    // ── Keys ────────────────────────────────────────────────────────────────
    Shortcut { sequences: [StandardKey.Open]; context: Qt.ApplicationShortcut; enabled: !presenter.running
               onActivated: win.confirmThenOpen() }
    Shortcut { sequences: [StandardKey.Save]; context: Qt.ApplicationShortcut; enabled: !presenter.running && !backend.startVisible
               onActivated: { win.commitEditors(); backend.save() } }
    Shortcut { sequences: [StandardKey.SaveAs]; context: Qt.ApplicationShortcut; enabled: !presenter.running && !backend.startVisible
               onActivated: { win.commitEditors(); backend.saveAsDialog() } }
    Shortcut { sequence: "Ctrl+Shift+N"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running
               onActivated: { win.commitEditors(); backend.showStart() } }
    Shortcut { sequence: "Ctrl+E"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible
               onActivated: { win.commitEditors(); backend.exportDialog({ kind: 0 }) } }
    Shortcut { sequence: "Ctrl+Z"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused; onActivated: backend.undo() }
    Shortcut { sequence: "Ctrl+Shift+Z"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused; onActivated: backend.redo() }
    Shortcut { sequence: "Ctrl+N"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible; onActivated: backend.addSlide() }
    Shortcut { sequence: "Ctrl+Shift+Up"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.slideFocus && !win.textEntryFocused;
               onActivated: backend.nudgeSelectedSlides(-1) }
    Shortcut { sequence: "Ctrl+Shift+Down"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.slideFocus && !win.textEntryFocused;
               onActivated: backend.nudgeSelectedSlides(1) }
    Shortcut { sequence: "Ctrl+A"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.editing); onActivated: backend.selectAll() }
    Shortcut { sequence: "Ctrl+G"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.editing); onActivated: backend.groupSelected() }
    Shortcut { sequence: "Ctrl+Shift+G"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.editing); onActivated: backend.ungroupSelected() }
    Shortcut { sequence: "Return"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.editing && !!backend.selection.groupId); onActivated: backend.enterGroup() }
    Shortcut { sequence: "Ctrl+D"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus; onActivated: win.editing && backend.hasSelection ? backend.duplicateSelected() : backend.duplicateSlide() }
    Shortcut { sequences: [StandardKey.Copy]; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && win.editing && !presenter.running && !win.textEntryFocused && !win.slideFocus; onActivated: backend.copyAsync() }
    Shortcut { sequences: [StandardKey.Cut]; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && win.editing && !presenter.running && !win.textEntryFocused && !win.slideFocus; onActivated: backend.copyAsync(true) }
    Shortcut { sequences: [StandardKey.Paste]; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && win.editing && !presenter.running && !win.textEntryFocused && !win.slideFocus; onActivated: backend.pasteAsync() }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.editing)
               onActivated: backend.addText() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.editing)
               onActivated: backend.addRect() }
    Shortcut { sequences: [StandardKey.Delete, StandardKey.Backspace]
               context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.editing)
               onActivated: editCanvas.pathMode===3 ? editCanvas.deletePathNode() : backend.deleteSelected() }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible
               onActivated: {
                   if (win.presenting) win.endShow()
                   else if(editCanvas.cropMode) editCanvas.cancelCrop()
                   else if(editCanvas.pathMode!==0) editCanvas.cancelPathTool()
                   else if (win.inMode) win.leaveMode()
                   else { backend.cancelEdit(); backend.leaveGroup() }
               } }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && (win.workspace === 2 || win.workspace === 4)
               onActivated: backend.togglePlay() }
    Shortcut { sequence: "F5"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible; onActivated: win.startShow() }
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused; onActivated: helpSheet.open() }
    Shortcut { sequence: "F1"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running; onActivated: { win.commitEditors(); helpGuide.open() } }
    Shortcut { sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; enabled: !presenter.running; onActivated: win.close() }

    Shortcut { sequences: ["Ctrl++", "Ctrl+="]; enabled: !win.dialogOpen && win.editing && !presenter.running && !win.textEntryFocused; onActivated: editCanvas.zoomBy(1.25) }
    Shortcut { sequence: "Ctrl+-"; enabled: !win.dialogOpen && win.editing && !presenter.running && !win.textEntryFocused; onActivated: editCanvas.zoomBy(1/1.25) }
    Shortcut { sequence: "Ctrl+0"; enabled: !win.dialogOpen && win.editing && !presenter.running && !win.textEntryFocused; onActivated: editCanvas.fit() }
    Shortcut { sequence: "Ctrl+Shift+0"; enabled: !win.dialogOpen && win.editing && !presenter.running && !win.textEntryFocused; onActivated: editCanvas.fitSelection() }
    Shortcut { sequence: "Ctrl+1"; enabled: !win.dialogOpen && win.editing && !presenter.running && !win.textEntryFocused; onActivated: editCanvas.actualSize() }

    // Arrows mean different things per workspace, so they are bound per
    // workspace rather than doing something surprising in one of them.
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.workspace !== 1)
               onActivated: win.editing ? backend.nudgeSelected(-Theme.nudge, 0) : backend.step(-0.1) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.workspace !== 1)
               onActivated: win.editing ? backend.nudgeSelected(Theme.nudge, 0) : backend.step(0.1) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.editing)
               onActivated: backend.nudgeSelected(0, -Theme.nudge) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible && !win.textEntryFocused && !win.slideFocus && (win.editing)
               onActivated: backend.nudgeSelected(0, Theme.nudge) }
    Shortcut { sequence: "PgDown"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible
               onActivated: backend.currentSlide = backend.currentSlide + 1 }
    Shortcut { sequence: "PgUp"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running && !backend.startVisible
               onActivated: backend.currentSlide = backend.currentSlide - 1 }


    // Never lose work without asking — and never ask when nothing would be lost.
    property bool allowClose: false
    readonly property bool textEntryFocused: activeFocusItem && typeof activeFocusItem.cursorPosition === "number"
    function commitEditors() {
        tableEditor.commitCell()
        editCanvas.commitTextEdit()
        editCanvas.finishCrop()
        contentItem.forceActiveFocus()
    }
    property var pendingAction: null
    function confirmThenNew() { win.guard(() => { if (backend.createDeck(startCentre.themeIndex, startCentre.deckWidth, startCentre.deckHeight, startCentre.layoutIndex)) win.workspace = 0 }) }
    function confirmThenOpen() { win.guard(() => backend.openDialog()) }
    function guard(action) {
        if (backend.operation.length > 0) return
        win.commitEditors()
        if (!backend.modified) {
            action()
            return
        }
        win.pendingAction = action
        discardSheet.open()
    }

    function startShow() { win.commitEditors(); presenter.start(false,false) }
    function endShow() { presenter.stop() }
    SlideSizeDialog { id: slideSizeDialog }
    LayoutApplyDialog { id: layoutApplyDialog }
    // Every command the menus offer, by name and by the words people use for it.
    // OMASHOW-PERSONAL: ribbon-only prototype (UI-RIBBON-003) — Ctrl+K lists the
    // ribbon's own table (same labels, rules and handlers), built as it opens.
    CommandPalette {
        id: commandPalette
        onAboutToShow: commands = win.paletteCommands()
    }
    // Where the guide's "Take me there" goes. Most parts need a deck open.
    function canVisitHelp(key) {
        return key === "start" || key === "commands" || !backend.startVisible
    }
    function visitHelp(key) {
        win.commitEditors()
        switch (key) {
        case "start": backend.showStart(); break
        case "slides": win.workspace = 0; win.navigatorCollapsed = false; slideNavigator.forceActiveFocus(); break
        case "format": win.chooseSidebar(0); break
        case "animate": win.ribbonTab = win.ribbonTabs.findIndex(k => k.key === "animate"); win.chooseSidebar(3); break
        case "document": win.chooseSidebar(2); break
        case "notes": win.workspace = 0; win.notesOpen = true; break
        case "masters": win.workspace = 1; break
        case "lighttable": win.workspace = 6; slideSorter.focusBrowser(); break
        case "review": win.workspace = 3; break
        case "setup": win.workspace = 4; break
        case "export": win.showExport(); break
        case "media": mediaPreflight.open(); break
        case "commands": commandPalette.show(); break
        }
    }
    HelpGuide {
        id: helpGuide
        canVisit: win.canVisitHelp
        onVisit: (key) => win.visitHelp(key)
        onKeysRequested: helpSheet.open()
    }
    Shortcut { sequence: "Ctrl+K"; context: Qt.ApplicationShortcut; enabled: !win.dialogOpen && !presenter.running
               onActivated: { win.commitEditors(); commandPalette.show() } }
    ImportDesignDialog { id: importDesignDialog }
    Connections { target: backend; function onImportReady() { win.commitEditors(); importDesignDialog.show() } }
    ShapeGallery { id: shapeGallery }
    TableEditor { id: tableEditor }
    DiagramDialog { id: diagramDialog }
    Connections { target: backend; function onTableEditorRequested() { win.commitEditors(); tableEditor.show() } }
    LinkDialog { id: linkDialog }
    FontsDialog { id: fontsDialog }
    MediaOptimisation { id: mediaOptimisation }
    Connections { target: backend; function onMediaOptimisationRequested() { win.commitEditors(); mediaOptimisation.show() } }
    MediaPreflight { id: mediaPreflight; onEditRequested: win.workspace=0 }
    Connections { target: backend; function onMediaPreviewRequested() { win.commitEditors(); win.workspace=2 } }
    Timer { id: mediaProgressDelay; interval: 150; onTriggered: if(backend.busy) mediaProgressDialog.open() }
    Connections {
        target: backend
        function onBusyChanged() {
            if(backend.busy) mediaProgressDelay.start()
            else { mediaProgressDelay.stop(); mediaProgressDialog.close() }
        }
    }
    Sheet {
        id: mediaProgressDialog; objectName: "mediaProgressDialog"; parent: Overlay.overlay; anchors.centerIn: parent
        width: 380; modal: true; closePolicy: Popup.NoAutoClose
        title: backend.mediaJobLabel
        background: Rectangle { color: Theme.panelBg; border.color: Theme.border; radius: Theme.rMenu }
        Overlay.modal: Rectangle { color: Theme.withAlpha(Theme.showBg, 0.65) }
        contentItem: ColumnLayout {
            Label { text: qsTr("%1% complete").arg(backend.mediaProgress); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textMuted }
            ProgressBar { Layout.fillWidth: true; from: 0; to: 100; value: backend.mediaProgress }
            Button { objectName: "cancelMediaImport"; text: qsTr("Cancel"); onClicked: backend.cancelMediaJob() }
        }
    }
    Timer { id: operationDelay; interval: 150; onTriggered: if (backend.operation.length > 0) operationDialog.open() }
    Connections {
        target: backend
        function onOperationChanged() {
            if (backend.operation.length > 0) operationDelay.start()
            else { operationDelay.stop(); operationDialog.close() }
        }
    }
    Sheet {
        id: operationDialog; objectName: "documentOperationDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: 380; modal: true; closePolicy: Popup.NoAutoClose
        title: backend.operation
        contentItem: ColumnLayout {
            ProgressBar { Layout.fillWidth: true; indeterminate: true }
            Button { text: qsTr("Cancel"); onClicked: backend.cancelOperation() }
        }
    }
    CombineShapesDialog { id: combineShapesDialog }
    Sheet {
        id: templateName
        objectName: "keepTemplateDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 520)
        modal: true; title: qsTr("Keep this deck as a template")
        standardButtons: Dialog.Save | Dialog.Cancel
        onOpened: { field.text = backend.fileName.replace(/\.omashow$/, ""); field.forceActiveFocus() }
        onAccepted: backend.saveAsTemplate(field.text)
        ColumnLayout {
            width: parent.width
            TextField { id: field; objectName: "templateNameField"; Layout.fillWidth: true
                        placeholderText: qsTr("Name for the template")
                        onAccepted: if (text.trim().length) templateName.accept() }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.textMuted; font.pixelSize: Theme.fsLabel
                text: qsTr("The whole deck is kept, so whatever is on the slides becomes the starting point. Linked pictures and film stay linked and are not carried.")
            }
        }
    }
    // Export is somewhere you go for a moment, as in Keynote: a sheet over
    // the deck, not a room of its own.
    Sheet {
        id: exportSheet
        objectName: "exportSheet"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s5 * 2, 900)
        height: Math.min(parent.height - Theme.s5 * 2, 820)
        modal: true
        padding: 0
        standardButtons: Dialog.Close
        contentItem: Loader {
            active: exportSheet.visible
            sourceComponent: ExportWorkspace {}
        }
    }
    AudienceWindow { id: audienceWindow }
    PresenterConsole { id: consoleWindow }

    StartCentre {
        id: startCentre
        anchors.fill: parent
        visible: backend.startVisible
        onCreateRequested: win.confirmThenNew()
        onOpenRequested: win.confirmThenOpen()
        onRecentRequested: path => win.guard(() => backend.openRecent(path))
        onTemplateRequested: id => win.guard(() => { if (backend.createFromTemplate(id, startCentre.deckWidth, startCentre.deckHeight)) win.workspace = 0 })
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: !win.presenting && !backend.startVisible

        // ── Title line: name · File · tabs · the always-visible cluster ─────
        // OMASHOW-PERSONAL: ribbon-only prototype (UI-RIBBON-003). The menu bar and
        // the toolbar are one ribbon: this line plus row 2 below, at the height the
        // two had, so the canvas never moves.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.hTitleBar
            color: Theme.frameFlush

            RowLayout {
                anchors.fill: parent
                // Eight tabs and the cluster still fit at ~820 px (ANIM-001 added Animate).
                anchors.leftMargin: win.width < 900 ? Theme.s1 : Theme.s4
                anchors.rightMargin: win.width < 900 ? Theme.s1 : Theme.s3
                spacing: win.width < 900 ? 0 : Theme.s1

                Label {
                    // The window title names the app too; below 1000 px the tabs need the room.
                    visible: win.width >= 1000
                    text: Theme.appName
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fsTitle
                    font.weight: Font.Bold
                    Layout.rightMargin: Theme.s2
                }
                ToolAction {
                    id: fileButton
                    objectName: "ribbonFileButton"
                    text: qsTr("File")
                    display: AbstractButton.TextOnly
                    implicitHeight: Theme.hTitleBar - 6
                    readonly property bool opensMenu: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.ButtonMenu
                    onClicked: fileMenu.popup(fileButton, 0, fileButton.height)
                    RibbonMenu { id: fileMenu; objectName: "ribbonFileMenu"; owner: "file" }
                }
                Row {
                    objectName: "ribbonTabBar"
                    Layout.fillHeight: true
                    Accessible.role: Accessible.PageTabList
                    Accessible.name: qsTr("Ribbon tabs")
                    Repeater {
                        model: win.ribbonTabs
                        ToolAction {
                            required property var modelData
                            required property int index
                            objectName: "ribbonTab" + index
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            display: AbstractButton.TextOnly
                            implicitHeight: Theme.hTitleBar - 6
                            leftPadding: win.width < 900 ? 2 : win.width < 1000 ? Theme.s1 : Theme.s2; rightPadding: leftPadding
                            // Usable in rooms too: personal's menus were reachable there.
                            checkable: false
                            checked: win.ribbonTab === index
                            focusPolicy: Qt.StrongFocus
                            Accessible.role: Accessible.PageTab
                            Accessible.checked: checked
                            // Showing a tab has no side effects.
                            onClicked: win.ribbonTab = index
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Label {
                    visible: backend.canUndo && win.width > 1100
                    text: qsTr("Ctrl+Z  %1").arg(backend.undoLabel)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fsLabel
                    elide: Text.ElideRight
                    Layout.maximumWidth: Theme.wInspector
                }
                // The cluster: each of these exists only here.
                RibbonButton { c: win.cmd.commandSearchMenuItem; display: AbstractButton.IconOnly; implicitHeight: Theme.hTitleBar - 6 }
                RibbonButton { c: win.cmd.toolbarUndo; display: AbstractButton.IconOnly; implicitHeight: Theme.hTitleBar - 6
                               tip: backend.canUndo ? qsTr("Undo %1  Ctrl+Z").arg(backend.undoLabel) : qsTr("Undo  Ctrl+Z") }
                RibbonButton { c: win.cmd.toolbarRedo; display: AbstractButton.IconOnly; implicitHeight: Theme.hTitleBar - 6 }
                ToolbarRule { Layout.topMargin: Theme.s2; Layout.bottomMargin: Theme.s2 }
                RibbonButton { c: win.cmd.toolbarPlay; highlighted: true; flat: false; implicitHeight: Theme.hTitleBar - 6
                               display: win.compactToolbar ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon }
                ToolbarRule { Layout.topMargin: Theme.s2; Layout.bottomMargin: Theme.s2 }
                // The sidebar's switches. Animation lives on the Animate tab (ANIM-001).
                Repeater {
                    model: ["sidebarFormat", "sidebarDocument"]
                    RibbonButton {
                        required property string modelData
                        c: win.cmd[modelData]
                        implicitHeight: Theme.hTitleBar - 6
                        display: win.compactToolbar ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
                        Accessible.role: Accessible.PageTab
                        Accessible.description: qsTr("Sidebar")
                    }
                }
                RibbonButton { c: win.cmd.ribbonHelpButton; display: AbstractButton.TextOnly; implicitHeight: Theme.hTitleBar - 6 }
            }
        }

        // ── Row 2: the selected tab's groups (+ in a room: its title and Done) ─
        Rectangle {
            objectName: "mainToolbar"
            Layout.fillWidth: true
            implicitHeight: Theme.hToolbar
            color: Theme.framePanel
            Hairline { anchors.top: parent.top; width: parent.width }

            // Every tab is built once and kept; only the selected one shows. Too
            // wide for the window: the row scrolls sideways (never wraps), with a
            // 3 px bar shown only while the pointer is over the row.
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.s3
                anchors.rightMargin: Theme.s3
                spacing: Theme.s2
                StackLayout {
                    id: ribbonStack
                    objectName: "ribbonStack"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: win.ribbonTab
                    Repeater {
                        model: win.ribbonTabs
                        Flickable {
                            id: tabFlick
                            required property var modelData
                            required property int index
                            objectName: "ribbonTabRow_" + modelData.key
                            readonly property bool overflows: contentWidth > width + 1
                            contentWidth: tabRow.implicitWidth
                            contentHeight: height
                            clip: true
                            // Wheel and scrollbar only: a drag on a button must stay a click.
                            interactive: false
                            flickableDirection: Flickable.HorizontalFlick
                            boundsBehavior: Flickable.StopAtBounds
                            Accessible.role: Accessible.Pane
                            Accessible.name: modelData.label
                            HoverHandler { id: rowHover }
                            WheelHandler {
                                enabled: tabFlick.overflows
                                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                                onWheel: (e) => tabFlick.contentX = Math.max(0, Math.min(tabFlick.contentWidth - tabFlick.width,
                                                                                         tabFlick.contentX - (e.angleDelta.y !== 0 ? e.angleDelta.y : e.angleDelta.x)))
                            }
                            ScrollBar.horizontal: ScrollBar {
                                objectName: "ribbonScrollBar"
                                // A 3 px line with a 9 px grab area.
                                height: 9
                                topPadding: 6
                                leftPadding: 0; rightPadding: 0; bottomPadding: 0
                                policy: ScrollBar.AlwaysOn
                                visible: tabFlick.overflows && (rowHover.hovered || pressed)
                                contentItem: Rectangle { implicitHeight: 3; radius: 1.5; color: Theme.textMuted }
                                background: Item {}
                            }
                            RowLayout {
                                id: tabRow
                                height: tabFlick.height
                                spacing: Theme.s1
                                Repeater {
                                    model: win.groupsIn(tabFlick.modelData.key)
                                    RowLayout {
                                        required property string modelData
                                        required property int index
                                        spacing: Theme.s1
                                        Accessible.role: Accessible.Grouping
                                        Accessible.name: win.ribbonGroupLabels[modelData] ?? modelData
                                        ToolbarRule { visible: index > 0 }
                                        Repeater {
                                            model: win.cmdsIn(tabFlick.modelData.key, modelData)
                                            Loader {
                                                required property var modelData
                                                Layout.alignment: Qt.AlignVCenter
                                                sourceComponent: modelData.kind === "number" ? ribbonNumberComp : ribbonButtonComp
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // In a room: its name and the way back, pinned right; the tabs stay usable.
                ToolbarRule { visible: win.inMode }
                Label {
                    objectName: "modeTitle"
                    visible: win.inMode
                    text: win.workspace === 1 ? qsTr("Masters and layouts")
                        : win.workspace === 3 ? qsTr("Review")
                        : win.workspace === 4 ? qsTr("Presenter setup")
                        : qsTr("Light table")
                    font.pixelSize: Theme.fsSection
                    font.weight: Theme.wHeading
                    color: Theme.textPrimary
                }
                Button {
                    objectName: "modeDone"
                    visible: win.inMode
                    text: qsTr("Done")
                    highlighted: true
                    onClicked: win.leaveMode()
                    readonly property string about: win.workspace === 1 ? qsTr("Changes here reach every slide that uses the master or layout.")
                        : win.workspace === 3 ? qsTr("Comments, findings, search and spelling across the whole deck.")
                        : win.workspace === 4 ? qsTr("Choose the displays and rehearse. The console opens when the show starts.")
                        : qsTr("Every slide at once. Drag to reorder; double-click one to edit it.")
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: about + "\n" + qsTr("Back to the slide  Esc")
                    Accessible.description: about
                }
            }
            Hairline { anchors.bottom: parent.bottom; width: parent.width }
        }
        // ── Body ────────────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            SlideNavigator {
                id: slideNavigator
                objectName: "slideNavigatorPanel"
                onEditRequested: win.workspace = 0
                onLightTableRequested: { win.commitEditors(); win.workspace = 6; slideSorter.focusBrowser() }
                Layout.preferredWidth: win.navigatorCollapsed ? 0 : win.navigatorWidth
                Layout.minimumWidth: win.navigatorCollapsed ? 0 : Theme.wNavigatorMin
                Layout.maximumWidth: win.navigatorCollapsed ? 0 : Number.POSITIVE_INFINITY
                Layout.fillHeight: true
                clip: true
                visible: (win.workspace === 0 || win.workspace === 2) && !win.navigatorCollapsed
            }
            PanelGrip {
                objectName: "navigatorGrip"
                visible: win.workspace === 0 || win.workspace === 2
                collapsed: win.navigatorCollapsed
                onResizeStarted: { win.navigatorCollapsed = false; win.panelDragStart = win.navigatorWidth }
                onResizedBy: total => win.navigatorWidth = Math.round(win.clampNavigator(win.panelDragStart + total))
                onResizeFinished: win.savePanelWidths()
                onToggled: win.navigatorCollapsed = !win.navigatorCollapsed
            }

            SlideNavigator { id: slideSorter; sorter: true; Layout.fillWidth: true; Layout.fillHeight: true; visible: win.workspace === 6; onEditRequested: { win.workspace=0; editCanvas.forceActiveFocus() } }

            // Edit: the canvas, and the presenter notes under it.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0
                visible: win.editing

                Rectangle {
                    id: canvasArea
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.framePasteboard
                    EditCanvas { id: editCanvas; anchors.fill: parent }
                    // ANIM-001 auto preview: the slide plays its animation over the
                    // canvas, then goes; any click ends it. Nothing is edited.
                    Rectangle {
                        objectName: "animPreviewOverlay"
                        anchors.fill: parent
                        visible: win.animPreviewing && backend.playing
                        color: Theme.framePasteboard
                        SlideView { anchors.fill: parent; anchors.margins: Theme.s5; deck: backend; time: backend.time; editSlide: -1 }
                        MouseArea { anchors.fill: parent; onPressed: { backend.pause(); win.animPreviewing = false } }
                    }

                }

                // Presenter notes, as in the concept: a header strip and a well.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: win.notesOpen ? Theme.hCardHeader + Theme.hNotes : Theme.hCardHeader
                    color: Theme.framePanel
                    Hairline { anchors.top: parent.top; width: parent.width }
                    RowLayout {
                        id: notesHeader
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.leftMargin: Theme.s4; anchors.rightMargin: Theme.s3
                        height: Theme.hCardHeader
                        spacing: Theme.s2
                        Label { text: qsTr("Presenter Notes"); font.weight: Theme.wHeading; color: Theme.textPrimary }
                        Icon { name: "notebook-pen"; color: Theme.textMuted; visible: backend.slideNotes !== "" }
                        Item { Layout.fillWidth: true }
                        ToolButton {
                            icon.name: win.notesOpen ? "chevron-down" : "chevron-up"
                            onClicked: win.notesOpen = !win.notesOpen
                            ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                            ToolTip.text: win.notesOpen ? qsTr("Hide notes") : qsTr("Show notes")
                            Accessible.name: ToolTip.text
                        }
                    }
                    ScrollView {
                        visible: win.notesOpen
                        anchors.top: notesHeader.bottom; anchors.bottom: parent.bottom
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.leftMargin: Theme.s4; anchors.rightMargin: Theme.s4; anchors.bottomMargin: Theme.s3
                        TextArea {
                            objectName: "editNotes"
                            text: backend.slideNotes
                            placeholderText: qsTr("Notes for this slide — only you see them while presenting.")
                            wrapMode: TextArea.Wrap
                            textFormat: TextEdit.PlainText
                            onEditingFinished: if (text !== backend.slideNotes) backend.setSlideNotes(text)
                        }
                    }
                }
            }

            DesignWorkspace {
                onApplyLayoutRequested: layoutId => { win.commitEditors(); layoutApplyDialog.show(layoutId) }
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: win.workspace === 1
            }

            AnimateWorkspace {
                id: animateRoom
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: win.workspace === 2
                nameOf: b => win.animName(b)
                selectedBuild: win.animRow
                onRowChosen: index => { win.animRow = index; const b = backend.builds[index]; if (b) backend.select(b.targetId) }
            }

            ReviewWorkspace {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: win.workspace === 3
                onFontsRequested: fontsDialog.show()
            }

            PresenterPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: win.workspace === 4
                onStarting: win.commitEditors()
            }

            PanelGrip {
                objectName: "inspectorGrip"
                fromLeft: false
                visible: win.editing
                collapsed: win.inspectorCollapsed
                onResizeStarted: { win.inspectorCollapsed = false; win.panelDragStart = win.inspectorWidth }
                onResizedBy: total => win.inspectorWidth = Math.round(win.clampInspector(win.panelDragStart + total))
                onResizeFinished: win.savePanelWidths()
                onToggled: win.inspectorCollapsed = !win.inspectorCollapsed
            }
            Inspector {
                objectName: "inspectorPanel"
                onApplyLayoutRequested: layoutId => { win.commitEditors(); layoutApplyDialog.show(layoutId) }
                Layout.preferredWidth: win.inspectorCollapsed ? 0 : win.inspectorWidth
                Layout.minimumWidth: win.inspectorCollapsed ? 0 : Theme.wInspectorMin
                Layout.maximumWidth: win.inspectorCollapsed ? 0 : Number.POSITIVE_INFINITY
                Layout.fillHeight: true
                clip: true
                visible: win.editing && win.sidebar === 0 && !win.inspectorCollapsed
            }
            DocumentPanel {
                onEditMastersRequested: { win.commitEditors(); win.workspace = 1 }
                onSlideSizeRequested: { win.commitEditors(); slideSizeDialog.show() }
                onPresenterSetupRequested: { win.commitEditors(); win.workspace = 4 }
                onReviewRequested: { win.commitEditors(); win.workspace = 3 }
                Layout.preferredWidth: win.inspectorCollapsed ? 0 : win.inspectorWidth
                Layout.minimumWidth: win.inspectorCollapsed ? 0 : Theme.wInspectorMin
                Layout.maximumWidth: win.inspectorCollapsed ? 0 : Number.POSITIVE_INFINITY
                Layout.fillHeight: true
                clip: true
                visible: win.editing && win.sidebar === 1 && !win.inspectorCollapsed
            }
            AnimationPane {
                Layout.preferredWidth: win.inspectorCollapsed ? 0 : win.inspectorWidth
                Layout.minimumWidth: win.inspectorCollapsed ? 0 : Theme.wInspectorMin
                Layout.maximumWidth: win.inspectorCollapsed ? 0 : Number.POSITIVE_INFINITY
                Layout.fillHeight: true
                clip: true
                visible: win.editing && win.sidebar === 2 && !win.inspectorCollapsed
                row: win.animRow
                nameOf: b => win.animName(b)
                onRowChosen: (index, targetId) => { win.commitEditors(); backend.select(targetId); win.animRow = index }
            }
        }

        // ── A deck that came from another application ───────────────────────
        Rectangle {
            objectName: "importBar"
            property var report: backend.importReport
            property var notes: report.warnings ?? []
            property bool open: false
            Layout.fillWidth: true
            visible: (report.name ?? "") !== ""
            implicitHeight: importColumn.implicitHeight + Theme.s2
            color: Theme.withAlpha(Theme.accent, 0.12)
            Hairline { anchors.bottom: parent.bottom; width: parent.width }
            ColumnLayout {
                id: importColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.s4
                anchors.rightMargin: Theme.s4
                spacing: Theme.s1
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumHeight: Theme.hRow
                    spacing: Theme.s3
                    Icon { name: "info"; color: Theme.accent }
                    Label {
                        objectName: "importMessage"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: parent.parent.parent.notes.length === 0
                              ? qsTr("%1 was opened from %2 as a new OmaShow deck. Save it to keep it; the original is untouched.").arg(parent.parent.parent.report.name).arg(parent.parent.parent.report.kind)
                              : parent.parent.parent.notes.length === 1
                              ? qsTr("%1 was opened from %2 as a new OmaShow deck. One thing could not be brought across; the original is untouched.").arg(parent.parent.parent.report.name).arg(parent.parent.parent.report.kind)
                              : qsTr("%1 was opened from %2 as a new OmaShow deck. %3 things could not be brought across; the original is untouched.").arg(parent.parent.parent.report.name).arg(parent.parent.parent.report.kind).arg(parent.parent.parent.notes.length)
                    }
                    Button {
                        objectName: "importFonts"
                        visible: (parent.parent.parent.report.missingFonts ?? []).length > 0
                        text: qsTr("Choose typefaces…")
                        onClicked: fontsDialog.show()
                    }
                    Button {
                        objectName: "importDetails"
                        visible: parent.parent.parent.notes.length > 0
                        text: parent.parent.parent.open ? qsTr("Hide") : qsTr("What was left out")
                        onClicked: parent.parent.parent.open = !parent.parent.parent.open
                    }
                    Button {
                        objectName: "importDismiss"
                        text: qsTr("OK")
                        onClicked: backend.dismissImportReport()
                    }
                }
                Repeater {
                    model: parent.parent.open ? parent.parent.notes : []
                    delegate: Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.s5
                        wrapMode: Text.Wrap
                        color: Theme.textMuted
                        text: "• " + modelData
                    }
                }
            }
        }

        // ── What happened to the file while you were working ────────────────
        Rectangle {
            objectName: "fileStateBar"
            Layout.fillWidth: true
            visible: (backend.fileState.changedOnDisk ?? false) || (backend.fileState.readOnly ?? false)
                     || (backend.fileState.missing ?? false) || (backend.fileState.openedElsewhere ?? false)
            implicitHeight: Theme.hRow + Theme.s2
            color: Theme.withAlpha(Theme.accent, 0.12)
            Hairline { anchors.bottom: parent.bottom; width: parent.width }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.s4
                anchors.rightMargin: Theme.s4
                spacing: Theme.s3
                Icon { name: "triangle-alert"; color: Theme.accent }
                Label {
                    objectName: "fileStateMessage"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: backend.fileState.openedElsewhere
                          ? qsTr("%1 is open in another copy of OmaShow. The last one to save wins; the version it replaces is kept beside it as .bak.").arg(backend.fileName)
                          : backend.fileState.missing
                          ? qsTr("%1 is no longer on disk. Saving will write it again.").arg(backend.fileName)
                          : backend.fileState.changedOnDisk
                          ? qsTr("%1 has changed on disk since you opened it.").arg(backend.fileName)
                          : qsTr("%1 is read-only. Saving will ask where to put it instead.").arg(backend.fileName)
                }
                Button {
                    objectName: "reloadFromDisk"
                    visible: backend.fileState.changedOnDisk ?? false
                    text: backend.modified ? qsTr("Discard mine and reload") : qsTr("Reload")
                    onClicked: win.guard(() => backend.reloadFromDisk())
                }
                Button {
                    objectName: "keepMyVersion"
                    visible: backend.fileState.changedOnDisk ?? false
                    text: qsTr("Keep mine")
                    onClicked: backend.keepMyVersion()
                }
                Button {
                    objectName: "saveElsewhere"
                    visible: (backend.fileState.readOnly ?? false) && !(backend.fileState.changedOnDisk ?? false)
                    text: qsTr("Save as…")
                    onClicked: { win.commitEditors(); backend.saveAsDialog() }
                }
            }
        }

        // ── Footer ──────────────────────────────────────────────────────────
        Rectangle {
            objectName: "statusBar"
            Layout.fillWidth: true
            implicitHeight: Theme.hStatusBar
            color: Theme.frameFlush
            Hairline { anchors.top: parent.top; width: parent.width }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.s4
                anchors.rightMargin: Theme.s3
                spacing: Theme.s3

                StatusText { text: qsTr("Slide %1 of %2").arg(backend.currentSlide + 1).arg(backend.slideCount) }
                StatusRule {}
                StatusText { text: backend.fileName; elide: Text.ElideMiddle; Layout.maximumWidth: Theme.wInspector }
                StatusRule { visible: backend.hasSelection }
                StatusText { visible: backend.hasSelection; text: qsTr("%1 selected").arg(backend.selection.type) }
                Item { Layout.fillWidth: true }
                StatusText {
                    objectName: "statusMessage"
                    text: backend.status; visible: text !== "" && text !== backend.fileName
                    // What just happened, for anything reading the window aloud.
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
                StatusText { visible: showGuard.engaged; text: showGuard.state; color: Theme.success }

                // Zoom lives here, as in the concept, so the canvas stays clear.
                RowLayout {
                    visible: win.editing
                    spacing: 0
                    StatusRule {}
                    ToolButton { objectName: "zoomOut"; icon.name: "zoom-out"; onClicked: editCanvas.zoomBy(1/1.25)
                                 ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Zoom out  Ctrl+−") }
                    ToolButton { objectName: "zoomActual"; text: Math.round(editCanvas.zoom*100) + "%"; font.family: Theme.monoFamily; onClicked: editCanvas.actualSize()
                                 implicitWidth: Theme.s5 * 2 + Theme.s3
                                 ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Actual size · 100%  Ctrl+1") }
                    ToolButton { objectName: "zoomIn"; icon.name: "zoom-in"; onClicked: editCanvas.zoomBy(1.25)
                                 ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Zoom in  Ctrl++") }
                    ToolButton { objectName: "zoomFit"; icon.name: "maximize"; onClicked: editCanvas.fit()
                                 ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Fit slide  Ctrl+0") }
                    ToolButton { objectName: "zoomSelection"; icon.name: "scan-search"; enabled: backend.hasSelection; onClicked: editCanvas.fitSelection()
                                 ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay; ToolTip.text: qsTr("Fit selection  Ctrl+Shift+0") }
                }
                StatusRule {}
                ToolButton {
                    text: win.aspectName
                    enabled: !win.presenting && !backend.startVisible
                    onClicked: { win.commitEditors(); slideSizeDialog.show() }
                    ToolTip.visible: hovered; ToolTip.delay: Theme.tooltipDelay
                    ToolTip.text: qsTr("Slide size %1 × %2").arg(Math.round(backend.slideSize.width)).arg(Math.round(backend.slideSize.height))
                }
                StatusRule {}
                Rectangle {
                    width: Theme.szStatusDot; height: width; radius: width / 2
                    color: backend.modified ? Theme.warning : Theme.success
                }
                StatusText {
                    text: backend.modified ? qsTr("Unsaved changes") : qsTr("Saved")
                    color: backend.modified ? Theme.warning : Theme.textSecondary
                }
            }
        }
    }

    // Offered only when a previous session actually left work behind — a clean
    // quit leaves no journal, so an ordinary launch shows nothing.
    Sheet {
        id: recoverySheet
        objectName: "recoveryDialog"
        title: qsTr("Unsaved work was found")
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: parent
        property var candidates: []

        // Runs in the sheet's scope, not the row's: replacing `candidates`
        // destroys the row that was clicked, and a handler still running in
        // that row loses its ids (ReferenceError) before it can close the sheet.
        function settle(handledPath) {
            const remaining = backend.recoveryCandidates()
                .filter(c => c.journalPath !== handledPath)
            if (remaining.length === 0)
                close()
            candidates = remaining
        }
        function discard(journalPath) {
            backend.discardRecovery(journalPath)
            settle(journalPath)
        }
        function recover(journalPath) {
            // The journal is forgotten only once the open finishes, so leave it
            // out of what remains rather than offering it again.
            backend.openAsync("file://" + journalPath, true)
            settle(journalPath)
        }

        ColumnLayout {
            spacing: Theme.s3

            Label {
                text: qsTr("OmaShow closed unexpectedly. This work was not saved.")
                color: Theme.textPrimary
                font.pixelSize: Theme.fsControl
            }

            Repeater {
                model: recoverySheet.candidates
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.s3

                    ColumnLayout {
                        spacing: 0
                        Label {
                            text: modelData.name
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fsControl
                            font.weight: Theme.wHeading
                        }
                        Label {
                            text: modelData.savedAt
                            color: Theme.textMuted
                            font.pixelSize: Theme.fsLabel
                        }
                    }
                    Item { Layout.fillWidth: true }
                    ToolAction {
                        text: qsTr("Discard")
                        onClicked: recoverySheet.discard(modelData.journalPath)
                    }
                    ToolAction {
                        text: qsTr("Recover")
                        onClicked: recoverySheet.recover(modelData.journalPath)
                    }
                }
            }

            Label {
                text: qsTr("Recovering opens a copy — nothing already saved is written over.")
                color: Theme.textMuted
                font.pixelSize: Theme.fsLabel
            }
        }
    }

    onClosing: event => {
        if (backend.operation.length > 0) { event.accepted = false; return }
        if (presenter.running) { event.accepted = false; presenter.stop(); return }
        win.commitEditors()
        if (backend.modified && !win.allowClose) {
            event.accepted = false
            win.guard(() => { win.allowClose = true; win.close() })
        }
    }

    Component.onCompleted: {
        Theme.frameActive = active
        win.sanePanels()
        presenter.attach(audienceWindow,consoleWindow,win)
        const found = backend.recoveryCandidates()
        if (found.length > 0) {
            recoverySheet.candidates = found
            recoverySheet.open()
        }
    }

    Sheet {
        id: discardSheet
        width: Theme.wInspector + Theme.s5 * 2
        objectName: "discardDialog"
        title: qsTr("Discard unsaved changes?")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Discard | Dialog.Save | Dialog.Cancel

        Label {
            width: discardSheet.availableWidth
            wrapMode: Text.Wrap
            text: qsTr("%1 has changes that are not saved.").arg(backend.fileName)
            color: Theme.textPrimary
            font.pixelSize: Theme.fsControl
        }
        onDiscarded: { const go = win.pendingAction; win.pendingAction = null; discardSheet.close(); if (go) go() }
        onAccepted: backend.save()
        onRejected: win.pendingAction = null
    }

    Sheet {
        id: errorSheet
        objectName: "errorDialog"
        width: Theme.wInspector + Theme.s5 * 2
        title: qsTr("That did not work")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Close
        property string message
        Label {
            text: errorSheet.message
            color: Theme.textPrimary
            font.pixelSize: Theme.fsControl
            wrapMode: Text.Wrap
            width: Theme.wInspector
        }
    }

    Connections {
        target: backend
        function onImageCropEditorRequested() { win.workspace=0; editCanvas.startCrop() }
        function onLinkEditorRequested() { linkDialog.show() }
        function onSaved() {
            const go = win.pendingAction
            win.pendingAction = null
            if (go) go()
        }
        function onSaveCanceled() { win.pendingAction = null }
        function onFailed(message) {
            win.pendingAction = null
            errorSheet.message = message
            errorSheet.open()
        }
    }

    Sheet {
        id: helpSheet
        objectName: "keysDialog"
        title: qsTr("Keys")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Close

        GridLayout {
            columns: 2
            columnSpacing: Theme.s5
            rowSpacing: Theme.s1
            Repeater {
                model: [
                    qsTr("F1 — the OmaShow guide"),
                    qsTr("Ctrl+K — find a command"),
                    qsTr("Ctrl+O / Ctrl+S — open / save"),
                    qsTr("Ctrl+Shift+S — save as"),
                    qsTr("Ctrl+Shift+N — Start centre"),
                    qsTr("Ctrl+E — export PDF"),
                    qsTr("Ctrl+Z / Ctrl+Shift+Z — undo / redo"),
                    qsTr("Ctrl+Q — quit"),
                    qsTr("Double-click text — edit it on the slide"),
                    qsTr("T / S — add text / shape"),
                    qsTr("Delete — remove the selected object"),
                    qsTr("Arrows — nudge (step time in Animate)"),
                    qsTr("Shift+click — add to the selection"),
                    qsTr("Alt+click — select the object behind"),
                    qsTr("Ctrl+A — select everything on the slide"),
                    qsTr("Ctrl+G / Ctrl+Shift+G — group / ungroup"),
                    qsTr("Enter — step inside a group, Escape leaves"),
                    qsTr("Ctrl+D — duplicate selected objects or slide"),
                    qsTr("Ctrl+C / Ctrl+X / Ctrl+V — copy / cut / paste"),
                    qsTr("Rotation handle · Shift snaps to 15°"),
                    qsTr("Ctrl++ / Ctrl+- — zoom in / out"),
                    qsTr("Ctrl+0 / Ctrl+1 — fit slide / 100%"),
                    qsTr("Ctrl+Shift+0 — fit selection"),
                    qsTr("Ctrl+wheel — zoom; middle drag or wheel — pan"),
                    qsTr("Ctrl+N — new slide"),
                    qsTr("PgUp / PgDn — previous / next slide"),
                    qsTr("Ctrl+Shift+↑ / ↓ — move the selected slides"),
                    qsTr("Slides: Shift range, Ctrl toggle, Ctrl+A all"),
                    qsTr("Slides: Ctrl+D duplicate, Delete remove, Enter edit"),
                    qsTr("Sections: Ctrl+← / Ctrl+→ collapse / expand"),
                    qsTr("Sections: Ctrl+Alt+↑ / ↓ move section"),
                    qsTr("Space — play / pause in Animate and Presenter setup"),
                    qsTr("Escape — cancel a crop or the pen, or leave a room"),
                    qsTr("F5 — start the show"),
                    qsTr("Show: Space, → or PgDn — next; ←, PgUp — back"),
                    qsTr("Show: B black, W white, F freeze, Escape ends"),
                    qsTr("Show: P pointer, S spotlight, D draw, E clear ink")
                ]
                Label {
                    required property string modelData
                    text: modelData
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fsControl
                }
            }
        }
    }
}

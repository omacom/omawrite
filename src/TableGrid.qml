import QtQuick
import QtQuick.Controls
import QtQml.Models

Rectangle {
    id: panel
    objectName: "markdownTableGrid"
    required property var modelData
    required property var preview
    required property color foreground
    required property color backgroundColor
    required property color borderColor
    required property font editorFont
    signal editorChanged(var editor)
    signal sourceRequested()
    signal expandRequested()
    property bool readOnly: false
    property bool expanded: false
    property bool suspended: false
    enabled: !suspended
    onReadOnlyChanged: if (grid) preview.watchTableInput(grid, !readOnly && !suspended)
    onSuspendedChanged: if (grid) preview.watchTableInput(grid, !readOnly && !suspended)
    property var activeEditor: null
    property string error: ""
    property bool restored: false
    property int currentColumn: 0
    property int currentRow: 0
    property var editingWidths: ({})
    property bool layoutQueued: false
    property bool submitting: false
    property real exactContentWidth: 0
    function requestLayout() {
        if (layoutQueued) return;
        layoutQueued = true;
        Qt.callLater(function() {
            layoutQueued = false;
            var width = 0;
            for (var c = 0; c < grid.columns; ++c)
                width += Math.max(tableModel.columnWidth(c), editingWidths[c] || 0);
            exactContentWidth = width;
            grid.forceLayout();
            restorePosition();
        });
    }
    function selectedRectangle() {
        var indexes = grid.selectionModel.selectedIndexes;
        if (!indexes.length) return null;
        var top = grid.rows, left = grid.columns, bottom = -1, right = -1;
        for (var i = 0; i < indexes.length; ++i) {
            top = Math.min(top, indexes[i].row); bottom = Math.max(bottom, indexes[i].row);
            left = Math.min(left, indexes[i].column); right = Math.max(right, indexes[i].column);
        }
        if ((bottom - top + 1) * (right - left + 1) !== indexes.length) return null;
        return { top: top, left: left, bottom: bottom, right: right, revision: tableModel.sourceRevision };
    }
    function fillRectangle(range, text) {
        if (readOnly || !range) return false;
        submitting = true;
        var accepted = tableModel.fillCells(range.top, range.left, range.bottom, range.right, text, range.revision);
        submitting = false;
        return accepted;
    }
    function expandColumn(column, text) {
        var width = Math.min(4096, Math.ceil(metrics.advanceWidth(text)) + 40);
        if (width <= (editingWidths[column] || tableModel.columnWidth(column))) return;
        var widths = editingWidths; widths[column] = width; editingWidths = widths;
        requestLayout();
    }
    readonly property var tableModel: modelData.model
    x: modelData.x
    y: modelData.y
    width: modelData.width
    height: modelData.height
    // Keep the frame distinct from the cells, in both light and dark themes.
    color: Qt.tint(backgroundColor, Qt.rgba(foreground.r, foreground.g, foreground.b, 0.04))
    radius: 8
    border.color: borderColor
    clip: true
    function discardCell() {
        if (activeEditor) activeEditor.discarded = true;
        grid.closeEditor();
    }
    function commitCell() {
        if (readOnly) return true;
        if (!activeEditor) {
            if (Object.keys(tableModel.drafts).length) { error = "Correct or discard pending cell edits"; return false; }
            return true;
        }
        if (!activeEditor.commit()) return false;
        grid.closeEditor();
        if (Object.keys(tableModel.drafts).length) { error = "Correct or discard pending cell edits"; return false; }
        return true;
    }
    // TableView can adjust its native end extent while virtualizing uneven
    // columns. visibleArea includes that extent; contentWidth alone does not.
    function maximumX() {
        var ratio = grid.visibleArea.widthRatio;
        return Math.max(0, (ratio > 0 ? grid.width / ratio : grid.contentWidth) - grid.width);
    }
    function maximumY() {
        var ratio = grid.visibleArea.heightRatio;
        return Math.max(0, (ratio > 0 ? grid.height / ratio : grid.contentHeight) - grid.height);
    }
    function restorePosition() {
        if (restored || exactContentWidth <= 0) return;
        grid.contentX = grid.originX + Math.min(tableModel.scrollX, maximumX());
        grid.contentY = grid.originY + Math.min(tableModel.scrollY, maximumY());
        restored = true;
    }
    Row {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 8
        anchors.rightMargin: 12
        height: 26
        spacing: 4
        Label { text: panel.error || "Table"; color: panel.foreground; height: 26; verticalAlignment: Text.AlignVCenter; font.pixelSize: 12 }
        ToolButton {
            objectName: "tableSourceButton"
            visible: !panel.readOnly
            enabled: !panel.readOnly
            text: "Source"
            focusPolicy: Qt.NoFocus
            height: 26
            onClicked: if (panel.commitCell()) { panel.preview.setTableSource(panel.modelData.id, true); panel.sourceRequested(); }
        }
        ToolButton {
            objectName: "tableExpandButton"
            visible: !panel.expanded
            text: "⛶"
            Accessible.name: "Expand table"
            ToolTip.visible: hovered
            ToolTip.text: "Expand table"
            focusPolicy: Qt.NoFocus
            height: 26
            onClicked: if (panel.commitCell()) panel.expandRequested()
        }
        ToolButton {
            objectName: "tableActionsButton"
            visible: !panel.readOnly
            enabled: !panel.readOnly
            text: "⋯"
            focusPolicy: Qt.NoFocus
            height: 26
            onClicked: actions.popup()
            FontMetrics { id: menuMetrics; font: actions.font }
            Menu {
                id: actions
                objectName: "tableActionsMenu"
                width: Math.max(320, menuMetrics.advanceWidth("Discard pending cell edits") + 80)
                MenuItem { text: "Insert row below"; onTriggered: if (panel.commitCell()) Qt.callLater(function() { panel.tableModel.insertRow(Math.max(0, panel.currentRow)); }); }
                MenuItem { text: "Delete row"; enabled: panel.currentRow > 0; onTriggered: if (panel.commitCell()) Qt.callLater(function() { panel.tableModel.removeRow(panel.currentRow); }); }
                MenuItem {
                    text: "Copy pending cell drafts"
                    enabled: Object.keys(panel.tableModel.drafts).length > 0
                    onTriggered: panel.preview.copyTableDrafts(panel.modelData.id)
                }
                MenuItem {
                    text: "Discard pending cell edits"
                    enabled: Object.keys(panel.tableModel.drafts).length > 0
                    onTriggered: {
                        if (panel.activeEditor) panel.activeEditor.discarded = true;
                        grid.closeEditor(); panel.tableModel.drafts = {}; panel.error = "";
                    }
                }
                MenuSeparator {}
                MenuItem { objectName: "alignColumnLeft"; text: "Align column left"; onTriggered: if (panel.commitCell()) panel.tableModel.setColumnAlignment(panel.currentColumn, "left"); }
                MenuItem { objectName: "alignColumnCenter"; text: "Align column center"; onTriggered: if (panel.commitCell()) panel.tableModel.setColumnAlignment(panel.currentColumn, "center"); }
                MenuItem { objectName: "alignColumnRight"; text: "Align column right"; onTriggered: if (panel.commitCell()) panel.tableModel.setColumnAlignment(panel.currentColumn, "right"); }
                MenuSeparator {}
                MenuItem { text: "Insert column to the right"; onTriggered: if (panel.commitCell()) Qt.callLater(function() { panel.tableModel.insertColumn(Math.max(0, panel.currentColumn)); }); }
                MenuItem { text: "Delete column"; enabled: grid.columns > 1; onTriggered: if (panel.commitCell()) Qt.callLater(function() { panel.tableModel.removeColumn(panel.currentColumn); }); }
            }
        }
    }
    TableView {
        id: grid
        objectName: "tableCellView"
        // Keep these in sync with the 68px slot overhead in LivePreview.
        x: 12
        y: 44
        width: Math.max(1, parent.width - 24)
        height: Math.max(1, parent.height - y - 24)
        clip: true
        model: panel.tableModel
        reuseItems: true
        contentWidth: panel.exactContentWidth
        contentHeight: rows * (Math.ceil(metrics.height) + 16)
        boundsBehavior: Flickable.StopAtBounds
        selectionBehavior: TableView.SelectCells
        selectionMode: TableView.ContiguousSelection
        editTriggers: panel.readOnly ? TableView.NoEditTriggers : TableView.SingleTapped | TableView.EditKeyPressed | TableView.AnyKeyPressed
        selectionModel: ItemSelectionModel { model: grid.model }
        columnWidthProvider: function(column) { return Math.max(panel.tableModel.columnWidth(column), panel.editingWidths[column] || 0); }
        rowHeightProvider: function(row) { return Math.ceil(metrics.height) + 16; }
        FontMetrics { id: metrics; font: panel.editorFont }
        readonly property bool cellInputAllowed: !panel.readOnly && !panel.suspended
        readonly property rect cellInputRectangle: {
            var item = selectionModel.currentIndex.row >= 0 ? itemAtIndex(selectionModel.currentIndex) : null;
            var p = item ? item.mapToItem(grid, 0, 0) : Qt.point(0, 0);
            return Qt.rect(p.x, p.y, 1, metrics.height);
        }
        function inputMethodEditor() {
            if (!cellInputAllowed || selectionModel.currentIndex.row < 0) return null;
            if (!panel.activeEditor) edit(selectionModel.currentIndex);
            return panel.activeEditor;
        }
        onContentWidthChanged: Qt.callLater(panel.restorePosition)
        onContentXChanged: if (panel.restored && !panel.suspended) panel.tableModel.scrollX = Math.max(0, contentX - originX)
        onContentYChanged: if (panel.restored && !panel.suspended) panel.tableModel.scrollY = Math.max(0, contentY - originY)
        Component.onCompleted: {
            // acceptedButtons was added after our Qt 6.7 baseline. On newer
            // Qt, reserve mouse drags for selection, not content flicking.
            if ("acceptedButtons" in grid) grid.acceptedButtons = Qt.NoButton;
            panel.preview.watchTableInput(grid, !panel.readOnly && !panel.suspended);
            panel.requestLayout();
        }
        WheelHandler {
            target: null
            orientation: Qt.Horizontal
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.NoModifier
            onWheel: function(event) {
                var dx = event.pixelDelta.x !== 0 ? event.pixelDelta.x : event.angleDelta.x / 2;
                if (!dx || grid.contentWidth <= grid.width) { event.accepted = false; return; }
                grid.contentX = Math.max(grid.originX, Math.min(grid.originX + panel.maximumX(), grid.contentX - dx));
                event.accepted = true;
            }
        }
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: function(event) {
            if (!panel.readOnly && !panel.activeEditor && (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)) {
                var range = panel.selectedRectangle();
                if (range && panel.commitCell()) {
                    if (!panel.fillRectangle(range, "")) panel.error = "Cells not cleared: source changed or table limits exceeded";
                    event.accepted = true;
                }
            }
        }
        Connections {
            target: grid.selectionModel
            function onCurrentChanged(current, previous) {
                if (current.valid) { panel.currentRow = current.row; panel.currentColumn = current.column; }
            }
        }
        Connections {
            target: panel.tableModel
            function onColumnWidthsChanged() { panel.requestLayout(); }
            function onModelReset() { panel.editingWidths = ({}); panel.requestLayout(); }
            function onSourceRevisionChanged() { if (!panel.submitting) grid.selectionModel.clearSelection(); }
        }
        ScrollBar.horizontal: ScrollBar {
            objectName: "blockHorizontalScrollBar"
            parent: panel
            x: grid.x; y: grid.y + grid.height
            width: grid.width; height: 12
            policy: ScrollBar.AsNeeded
            focusPolicy: Qt.NoFocus
        }
        ScrollBar.vertical: ScrollBar { objectName: "tableVerticalScrollBar"; policy: ScrollBar.AsNeeded; focusPolicy: Qt.NoFocus }
        delegate: Rectangle {
            id: cell
            objectName: "tableCellDelegate"
            required property int row
            required property int column
            required property string markdown
            required property string html
            required property bool header
            required property bool current
            required property bool selected
            required property bool editing
            required property int alignment
            implicitWidth: 100
            implicitHeight: Math.ceil(metrics.height) + 16
            color: selected && !panel.readOnly ? Qt.rgba(panel.foreground.r, panel.foreground.g, panel.foreground.b, 0.18)
                                              : current || header ? Qt.tint(panel.backgroundColor, Qt.rgba(panel.foreground.r, panel.foreground.g, panel.foreground.b, 0.08)) : panel.backgroundColor
            border.width: 0.5
            border.color: panel.borderColor
            clip: true
            Text {
                objectName: "tableCellDisplay"
                visible: !cell.editing
                horizontalAlignment: cell.alignment
                anchors.fill: parent
                anchors.margins: 8
                text: !panel.readOnly && panel.tableModel.drafts[cell.row + ":" + cell.column] ? panel.tableModel.drafts[cell.row + ":" + cell.column].text : cell.html
                textFormat: !panel.readOnly && panel.tableModel.drafts[cell.row + ":" + cell.column] ? Text.PlainText : Text.RichText
                wrapMode: Text.NoWrap
                font.family: panel.editorFont.family
                font.pixelSize: panel.editorFont.pixelSize
                font.bold: cell.header
                color: panel.foreground
                onLinkActivated: function(link) { if (panel.readOnly) Qt.openUrlExternally(link); }
            }
            TableView.editDelegate: TextField {
                id: field
                objectName: "tableCellEditor"
                enabled: !panel.readOnly
                anchors.fill: parent
                font: panel.editorFont
                text: cell.markdown
                selectByMouse: true
                horizontalAlignment: cell.alignment
                background: Rectangle { color: panel.backgroundColor; border.color: field.activeFocus ? field.palette.highlight : panel.borderColor; border.width: 1 }
                function revealCursor() {
                    if (!grid || !activeFocus) return;
                    var p = mapToItem(grid, cursorRectangle.x, 0);
                    var x = grid.contentX;
                    if (p.x < 8) x += p.x - 8;
                    else if (p.x + cursorRectangle.width > grid.width - 8) x += p.x + cursorRectangle.width - grid.width + 8;
                    grid.contentX = Math.max(grid.originX, Math.min(x, grid.originX + panel.maximumX()));
                }
                onCursorRectangleChanged: Qt.callLater(revealCursor)
                onPreeditTextChanged: { panel.expandColumn(cell.column, text + preeditText); Qt.callLater(revealCursor); }
                property string original: ""
                property var revision: 0
                property var rectangle: null
                function storeDraft() {
                    var drafts = panel.tableModel.drafts;
                    var key = (revision === panel.tableModel.draftRevision ? "" : "recovered/" + revision + "/") + cell.row + ":" + cell.column;
                    drafts[key] = { text: text, original: original, rectangle: rectangle };
                    panel.tableModel.drafts = drafts;
                }
                function clearDraft() {
                    var drafts = panel.tableModel.drafts; delete drafts[cell.row + ":" + cell.column]; panel.tableModel.drafts = drafts;
                }
                property bool discarded: false
                property bool committed: false
                function commit() {
                    if (panel.readOnly) return false;
                    if (discarded || committed) return true;
                    if (inputMethodComposing) Qt.inputMethod.commit();
                    if (inputMethodComposing) return false;
                    if (revision !== panel.tableModel.draftRevision || cell.markdown !== original
                        || !(rectangle ? panel.fillRectangle(rectangle, text) : panel.tableModel.setCell(cell.row, cell.column, text))) {
                        storeDraft();
                        panel.error = "Cell not saved: correct or discard its draft";
                        return false;
                    }
                    clearDraft();
                    committed = true;
                    panel.error = "";
                    return true;
                }
                onTextEdited: { committed = false; storeDraft(); panel.expandColumn(cell.column, text); Qt.callLater(revealCursor); }
                onEditingFinished: if (!discarded) commit()
                TableView.onCommit: commit()
                // TableView filters Escape before the edit delegate's Keys
                // handler. Claim the shortcut so cancellation also removes
                // our session draft, not just Qt's transient editor.
                Shortcut {
                    objectName: "tableCancelCellShortcut"
                    sequence: "Escape"
                    enabled: field.activeFocus && !field.inputMethodComposing
                    onActivated: { field.discarded = true; field.clearDraft(); grid.closeEditor(); }
                }
                Keys.priority: Keys.BeforeItem
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Escape && !inputMethodComposing) {
                        discarded = true; clearDraft(); grid.closeEditor(); event.accepted = true;
                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !inputMethodComposing) {
                        if (commit()) grid.closeEditor(); event.accepted = true;
                    } else if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) && !inputMethodComposing && !commit()) event.accepted = true;
                }
                Component.onCompleted: {
                    var draft = panel.tableModel.drafts[cell.row + ":" + cell.column];
                    revision = panel.tableModel.draftRevision;
                    var range = panel.selectedRectangle();
                    rectangle = draft && draft.rectangle ? draft.rectangle
                              : range && (range.bottom > range.top || range.right > range.left) ? range : null;
                    original = draft ? draft.original : cell.markdown;
                    if (draft) text = draft.text;
                    panel.currentColumn = cell.column; panel.currentRow = cell.row;
                    panel.activeEditor = field;
                    panel.editorChanged(field);
                    selectAll();
                }
                Component.onDestruction: {
                    if (panel.activeEditor === field) { panel.activeEditor = null; panel.editorChanged(null); }
                }
            }
        }
    }
    SelectionRectangle {
        objectName: "tableCellSelectionRectangle"
        target: grid
        enabled: !panel.readOnly && !panel.activeEditor
        selectionMode: SelectionRectangle.Drag
        onDraggingChanged: if (dragging) grid.forceActiveFocus()
        topLeftHandle: null
        bottomRightHandle: null
    }
}

import QtQuick

// Siblings of the clipped native editor: box padding/chrome can extend past
// the text column, while horizontally scrolled native glyphs cannot.
Item {
    id: decoration
    required property var preview
    required property var presentation
    required property var sourceEditor
    required property var host
    property bool foreground: true
    x: presentation.x
    y: presentation.y
    width: presentation.width
    height: presentation.height
    z: foreground ? 2 : -1
    Repeater {
        model: decoration.foreground ? [] : decoration.preview.codeBlockPanels
        delegate: Rectangle {
            required property var modelData
            objectName: "codeBlockPanel"
            x: modelData.x
            y: modelData.y
            width: modelData.width
            height: modelData.height
            radius: 4
            color: decoration.host.darkMode ? "#252525" : "#eeeeee"
            border.width: 0.5
            border.color: Qt.rgba(decoration.host.textColor.r, decoration.host.textColor.g, decoration.host.textColor.b, 0.12)
            antialiasing: true
        }
    }
    Repeater {
        model: decoration.foreground ? decoration.preview.listMarkers : []
        delegate: Text {
            required property var modelData
            objectName: "listPreviewMarker"
            text: modelData.text
            textFormat: Text.PlainText
            font: decoration.presentation.font
            renderType: decoration.presentation.renderType
            color: decoration.host.bodyTextColor
            x: modelData.right - width
            y: modelData.baseline - baselineOffset
            visible: !decoration.presentation.inputMethodComposing && !modelData.searchReveal
                && !(decoration.sourceEditor.cursorPosition >= modelData.start && decoration.sourceEditor.cursorPosition < modelData.end)
                && !(decoration.sourceEditor.selectionStart !== decoration.sourceEditor.selectionEnd
                     && decoration.sourceEditor.selectionStart < modelData.end && decoration.sourceEditor.selectionEnd > modelData.start)
        }
    }
    Repeater {
        model: decoration.foreground ? decoration.preview.codeBlockPanels : []
        delegate: CodeBlockControls {
            preview: decoration.preview
            readMode: decoration.host.readMode
            labelColor: decoration.host.mutedColor
            composing: decoration.presentation.inputMethodComposing
        }
    }
    property var tableItems: ({})
    function syncTables() {
        if (!foreground) return;
        var next = {};
        var panels = preview.tablePanels;
        for (var n = 0; n < panels.length; ++n) {
            var data = panels[n], key = String(data.id), item = tableItems[key];
            if (!item && data.inViewport) item = tableComponent.createObject(decoration, { modelData: data });
            if (!item) continue;
            item.modelData = data;
            if (!data.inViewport && !item.suspended && !item.activeEditor && Object.keys(item.tableModel.drafts).length === 0) item.destroy();
            else next[key] = item;
        }
        for (var old in tableItems) if (!next[old] && panels.every(function(p) { return String(p.id) !== old; })) tableItems[old].destroy();
        tableItems = next;
    }
    function commitTables() {
        for (var key in tableItems) if (!tableItems[key].commitCell()) return false;
        return true;
    }
    function abandonEditors() {
        for (var key in tableItems) tableItems[key].discardCell();
    }
    function restorePositions() {
        for (var key in tableItems) { tableItems[key].restored = false; tableItems[key].restorePosition(); }
    }
    // A QVariantList Repeater recreates every delegate when native frame
    // bounds change during scrolling. Keep the Read scrollbar's mouse grab
    // attached to the same item for the entire table session instead.
    property var tableScrollItems: ({})
    function syncTableScrolls() {
        if (!foreground) return;
        var next = {}, panels = preview.tableScrollPanels;
        for (var n = 0; n < panels.length; ++n) {
            var data = panels[n], key = String(data.id), item = tableScrollItems[key];
            if (!item) item = tableScrollComponent.createObject(decoration, { modelData: data });
            if (item) { item.modelData = data; next[key] = item; }
        }
        for (var old in tableScrollItems) if (!next[old]) tableScrollItems[old].destroy();
        tableScrollItems = next;
    }
    Connections { target: decoration.preview; function onTablePanelsChanged() { decoration.syncTables(); decoration.syncTableScrolls(); } }
    Component.onCompleted: { syncTables(); syncTableScrolls(); }
    Component {
        id: tableComponent
        TableGrid {
            id: tablePanel
            preview: decoration.preview
            readOnly: decoration.host.readMode
            suspended: decoration.host.tableExpanded && decoration.host.expandedTableId === modelData.id
            foreground: decoration.host.textColor
            backgroundColor: decoration.host.darkMode ? "#252525" : "#eeeeee"
            borderColor: Qt.rgba(decoration.host.textColor.r, decoration.host.textColor.g, decoration.host.textColor.b, 0.2)
            editorFont: decoration.presentation.font
            onSourceRequested: decoration.presentation.forceActiveFocus()
            onExpandRequested: decoration.host.expandTable(tablePanel)
            onEditorChanged: function(editor) {
                if (editor) decoration.host.activeTablePanel = tablePanel;
                else if (decoration.host.activeTablePanel === tablePanel) decoration.host.activeTablePanel = null;
            }
        }
    }
    Component {
        id: tableScrollComponent
        BlockOverflow {
            objectName: "readTableOverflow"
            wheelOrientation: Qt.Horizontal
            wheelDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            required property var modelData
            x: modelData.x
            y: modelData.y
            width: modelData.width
            height: modelData.height
            viewportBottom: decoration.preview.viewportBottom < 0 ? height : decoration.preview.viewportBottom - y
            contentWidth: modelData.contentWidth
            viewportWidth: width
            offset: decoration.preview.blockOffsets[modelData.id] || 0
            onScrollRequested: function(offset) { decoration.preview.scrollBlock(modelData.id, offset); }
        }
    }
}

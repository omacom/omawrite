import QtQuick
import QtQuick.Controls

Item {
    id: root

    property bool expanded: false
    property bool darkMode: true
    property real textScale: 1
    property color pageColor: darkMode ? "#101010" : "#ffffff"
    property color textColor: darkMode ? "#eeeeee" : "#222324"
    property color mutedColor: darkMode ? "#909191" : "#aeb1b5"
    property color accentColor: "#428bca"
    property color selectionFill: "#186a9a"
    property url folderUrl
    property string folderName: ""
    property bool folderHasParent: false
    property var entries: []
    property url currentFileUrl

    // Sizes are kept at text scale 1 like every other dimension in the app,
    // so a dragged width survives a change of desktop text size.
    property int logicalWidth: 240
    property int minimumLogicalWidth: 160
    property int maximumLogicalWidth: 640

    signal parentFolderRequested()
    signal folderRequested(url folderUrl)
    signal fileRequested(url fileUrl)
    signal createDocumentRequested(string name)
    signal createFolderRequested(string name)
    signal widthChangeRequested(int width)
    signal widthCommitted()
    signal dismissed()

    readonly property int rowHeight: Math.round(26 * root.textScale)
    readonly property bool creating: newEntryField.visible
    readonly property string selectedName:
        list.currentIndex >= 0 && list.currentIndex < entries.length
            ? entries[list.currentIndex].name : ""
    readonly property alias listHasFocus: list.activeFocus

    // Held by name: the model is rebuilt from scratch every time the folder is
    // re-read, and an index into it survives nothing.
    property string selectedEntryName: ""

    width: expanded ? Math.round(logicalWidth * root.textScale) : 0
    visible: width > 0
    clip: true

    onExpandedChanged: if (!expanded) cancelNewEntry()
    onFolderUrlChanged: selectedEntryName = ""
    // Deferred: the list also resets its own index when the model is replaced.
    onEntriesChanged: Qt.callLater(restoreSelection)

    function focusList() {
        list.forceActiveFocus();
        var open = indexOfUrl(root.currentFileUrl);
        if (open >= 0)
            selectIndex(open);
        else
            restoreSelection();
    }

    function indexOfName(name) {
        if (name.length === 0)
            return -1;
        for (var i = 0; i < root.entries.length; i++) {
            if (root.entries[i].name === name)
                return i;
        }
        return -1;
    }

    function indexOfUrl(entryUrl) {
        var target = entryUrl.toString();
        if (target.length === 0)
            return -1;
        for (var i = 0; i < root.entries.length; i++) {
            if (root.entries[i].url.toString() === target)
                return i;
        }
        return -1;
    }

    // The one way the selection moves, so the remembered name never drifts.
    function selectIndex(index) {
        if (index < 0 || index >= root.entries.length)
            return;
        list.currentIndex = index;
        root.selectedEntryName = root.entries[index].name;
    }

    // The folder is re-read whenever anything in it changes — saving is enough
    // — so keep the selection on the row it was on.
    function restoreSelection() {
        if (root.entries.length === 0) {
            list.currentIndex = -1;
            return;
        }
        var index = indexOfName(root.selectedEntryName);
        if (index < 0)
            index = indexOfUrl(root.currentFileUrl);
        selectIndex(Math.max(0, index));
    }

    // After making something, the selection sits on it rather than snapping
    // back to the top of the folder.
    function selectUrl(entryUrl) {
        selectIndex(indexOfUrl(entryUrl));
    }

    function selectNext() {
        if (root.entries.length === 0)
            return;
        selectIndex(Math.min(root.entries.length - 1, list.currentIndex + 1));
    }

    function selectPrevious() {
        if (root.entries.length === 0)
            return;
        selectIndex(Math.max(0, list.currentIndex - 1));
    }

    // Enter opens a document or walks into a folder, the one key doing what
    // the row in front of you calls for.
    function activateSelection() {
        if (list.currentIndex < 0 || list.currentIndex >= root.entries.length)
            return;
        var entry = root.entries[list.currentIndex];
        if (entry.isDir)
            root.folderRequested(entry.url);
        else
            root.fileRequested(entry.url);
    }

    function goUp() {
        if (root.folderHasParent)
            root.parentFolderRequested();
    }

    function beginNewDocument() {
        newEntryField.folderMode = false;
        newEntryField.text = "";
        newEntryField.visible = true;
        newEntryField.forceActiveFocus();
    }

    function beginNewFolder() {
        newEntryField.folderMode = true;
        newEntryField.text = "";
        newEntryField.visible = true;
        newEntryField.forceActiveFocus();
    }

    function commitNewEntry() {
        var name = newEntryField.text;
        var folderMode = newEntryField.folderMode;
        cancelNewEntry();
        if (name.trim().length === 0)
            return;
        if (folderMode)
            root.createFolderRequested(name);
        else
            root.createDocumentRequested(name);
    }

    function cancelNewEntry() {
        newEntryField.visible = false;
        newEntryField.text = "";
        if (root.expanded)
            list.forceActiveFocus();
    }

    // The editor sits behind the panel rather than beside it, so the
    // background has to be opaque to keep text from showing through.
    Rectangle {
        anchors.fill: parent
        color: root.pageColor
    }

    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: root.mutedColor
        opacity: resizeHandle.containsMouse || resizeHandle.pressed ? 0.6 : 0.25
    }

    Item {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 12
        anchors.rightMargin: 13
        height: Math.round(40 * root.textScale)

        Label {
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            text: (root.folderHasParent ? "‹ " : "") + root.folderName
            color: headerArea.containsMouse ? root.textColor : root.mutedColor
            elide: Text.ElideMiddle
            font.family: "iA Writer Mono S"
            font.pixelSize: Math.round(12 * root.textScale)
        }

        MouseArea {
            id: headerArea
            anchors.fill: parent
            enabled: root.folderHasParent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.goUp()
        }
    }

    Item {
        id: newEntryRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.rightMargin: 1
        height: newEntryField.visible ? root.rowHeight : 0

        TextInput {
            id: newEntryField
            objectName: "newEntryField"

            property bool folderMode: false

            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            verticalAlignment: TextInput.AlignVCenter
            visible: false
            selectByMouse: true
            color: root.textColor
            selectionColor: root.selectionFill
            selectedTextColor: root.textColor
            font.family: "iA Writer Mono S"
            font.pixelSize: Math.round(13 * root.textScale)

            Keys.onReturnPressed: function(event) {
                root.commitNewEntry();
                event.accepted = true;
            }
            Keys.onEnterPressed: function(event) {
                root.commitNewEntry();
                event.accepted = true;
            }
            Keys.onEscapePressed: function(event) {
                root.cancelNewEntry();
                event.accepted = true;
            }

            Label {
                anchors.fill: parent
                verticalAlignment: Text.AlignVCenter
                visible: newEntryField.text.length === 0
                text: newEntryField.folderMode ? "New folder" : "New file"
                color: root.mutedColor
                opacity: 0.7
                font.family: newEntryField.font.family
                font.pixelSize: newEntryField.font.pixelSize
            }
        }
    }

    Label {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: newEntryRow.bottom
        anchors.leftMargin: 12
        anchors.rightMargin: 13
        height: root.rowHeight
        verticalAlignment: Text.AlignVCenter
        visible: root.entries.length === 0 && !root.creating
        text: "Nothing here yet"
        color: root.mutedColor
        opacity: 0.7
        font.family: "iA Writer Mono S"
        font.pixelSize: Math.round(13 * root.textScale)
    }

    ListView {
        id: list
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: newEntryRow.bottom
        anchors.bottom: parent.bottom
        anchors.rightMargin: 1
        anchors.bottomMargin: Math.round(32 * root.textScale)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.entries
        currentIndex: -1
        highlightMoveDuration: 0
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

        // Arrow keys and their vim counterparts both move, so neither habit
        // has to be unlearned to leave the editor for a moment.
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Down || event.text === "j") {
                root.selectNext();
            } else if (event.key === Qt.Key_Up || event.text === "k") {
                root.selectPrevious();
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                       || event.key === Qt.Key_Right || event.text === "l") {
                root.activateSelection();
            } else if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Left
                       || event.text === "h") {
                root.goUp();
            } else if (event.key === Qt.Key_Escape) {
                root.dismissed();
            } else if (event.text === "a") {
                root.beginNewDocument();
            } else if (event.text === "A") {
                root.beginNewFolder();
            } else {
                return;
            }
            event.accepted = true;
        }

        delegate: Item {
            id: row

            required property var modelData
            required property int index

            width: ListView.view.width
            height: root.rowHeight

            readonly property bool current:
                !row.modelData.isDir
                && row.modelData.url.toString() === root.currentFileUrl.toString()

            Rectangle {
                anchors.fill: parent
                color: root.selectionFill
                // The keyboard selection fades when focus is back in the
                // editor, so the panel never looks like it is still driving.
                opacity: row.ListView.isCurrentItem
                    ? (list.activeFocus ? 0.35 : 0.12)
                    : (rowArea.containsMouse ? 0.25 : 0)
            }

            Label {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: Text.AlignVCenter
                text: row.modelData.isDir ? row.modelData.name + "/" : row.modelData.name
                color: row.current ? root.accentColor
                                   : (row.modelData.isDir ? root.mutedColor : root.textColor)
                elide: Text.ElideRight
                font.family: "iA Writer Mono S"
                font.pixelSize: Math.round(13 * root.textScale)
            }

            MouseArea {
                id: rowArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    root.selectIndex(row.index);
                    if (row.modelData.isDir)
                        root.folderRequested(row.modelData.url);
                    else
                        root.fileRequested(row.modelData.url);
                }
            }
        }
    }

    MouseArea {
        id: resizeHandle
        objectName: "sidebarResizeHandle"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 6
        hoverEnabled: true
        cursorShape: Qt.SplitHCursor

        property real grabOffset: 0

        onPressed: function(mouse) {
            root.beginResize(mapToItem(root, mouse.x, mouse.y).x);
        }
        onPositionChanged: function(mouse) {
            if (pressed)
                root.resizeTo(mapToItem(root, mouse.x, mouse.y).x);
        }
        // Settings are written once the drag ends rather than every frame.
        onReleased: root.widthCommitted()
    }

    // The handle rides the edge it moves, so a width measured as a delta from
    // it feeds the panel's own width back in and halves the tracking speed.
    // Measure from the left edge, which stays put.
    function beginResize(pointerX) {
        resizeHandle.grabOffset = root.width - pointerX;
    }

    function resizeTo(pointerX) {
        requestLogicalWidth((pointerX + resizeHandle.grabOffset) / root.textScale);
    }

    function requestLogicalWidth(width) {
        var clamped = Math.max(root.minimumLogicalWidth,
                               Math.min(root.maximumLogicalWidth, Math.round(width)));
        if (clamped !== root.logicalWidth)
            root.widthChangeRequested(clamped);
    }
}

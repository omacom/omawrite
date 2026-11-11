import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    objectName: "shortcutsDialog"

    property bool darkMode: true
    property color textColor: darkMode ? "#d0d0d0" : "#42464c"
    property color strongTextColor: darkMode ? "#eeeeee" : "#222324"
    property color activeButtonColor: "#428bca"
    property real textScale: 1
    property int containerWidth: 1280
    property int containerHeight: 820
    readonly property string versionText: "Omawrite " + Qt.application.version
    readonly property var shortcuts: [
        { key: "Ctrl+S", desc: qsTr("Save") },
        { key: "Ctrl+Shift+S", desc: qsTr("Save As") },
        { key: "Ctrl+O", desc: qsTr("Open") },
        { key: "Ctrl+N", desc: qsTr("New Window") },
        { key: "Ctrl+P", desc: qsTr("Print") },
        { key: "Ctrl+Z", desc: qsTr("Undo") },
        { key: "Ctrl+Shift+Z / Ctrl+Y", desc: qsTr("Redo") },
        { key: "F11 / Super+F", desc: qsTr("Fullscreen") },
        { key: "Ctrl+F", desc: qsTr("Find") },
        { key: "Ctrl+H", desc: qsTr("Find & Replace") },
        { key: "Ctrl+G / Enter", desc: qsTr("Next Match in Find") },
        { key: "Shift+Enter", desc: qsTr("Previous Match in Find") },
        { key: "Escape", desc: qsTr("Close Find") },
        { key: "Ctrl+B", desc: qsTr("Bold") },
        { key: "Ctrl+I", desc: qsTr("Italic") },
        { key: "Ctrl+Shift+X", desc: qsTr("Strikethrough") },
        { key: "Ctrl+K", desc: qsTr("Link") },
        { key: "Ctrl+Shift+C", desc: qsTr("Copy as HTML") },
        { key: "Ctrl+= / Ctrl++", desc: qsTr("Increase Text Size") },
        { key: "Ctrl+-", desc: qsTr("Decrease Text Size") },
        { key: "Ctrl+0", desc: qsTr("Reset Text Size") },
        { key: "Ctrl+scroll", desc: qsTr("Change Text Size") },
        { key: "Ctrl+? / Ctrl+/ / F1", desc: qsTr("Shortcuts") }
    ]

    title: qsTr("Keyboard shortcuts")
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    implicitWidth: Math.round(760 * textScale)
    implicitHeight: header.implicitHeight + footer.implicitHeight
                    + topPadding + bottomPadding + shortcutsGrid.implicitHeight
    width: Math.max(0, Math.min(implicitWidth, containerWidth - 32))
    height: Math.max(0, Math.min(implicitHeight, containerHeight - 32))
    x: Math.round((containerWidth - width) / 2)
    y: Math.round((containerHeight - height) / 2)
    padding: 24

    onOpened: closeButton.forceActiveFocus()

    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.65) }

    background: Rectangle {
        color: root.darkMode ? "#181818" : "#fbfbfb"
        border.color: root.darkMode ? "#343434" : "#d8d8d8"
        border.width: 1
        radius: 0
    }

    header: ColumnLayout {
        spacing: 6
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.topMargin: 20
            Layout.bottomMargin: Qt.application.version.length > 0 ? 0 : 8
            text: root.title
            color: root.strongTextColor
            font.family: "IBM Plex Mono"
            font.pixelSize: Math.round(15 * root.textScale)
            font.bold: true
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "versionLabel"
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.bottomMargin: 8
            text: root.versionText
            visible: Qt.application.version.length > 0
            color: root.textColor
            opacity: 0.75
            font.family: "IBM Plex Mono"
            font.pixelSize: Math.round(11 * root.textScale)
            wrapMode: Text.Wrap
        }
    }

    contentItem: ScrollView {
        id: shortcutsScroll
        objectName: "shortcutsScroll"
        clip: true
        contentWidth: availableWidth
        contentHeight: shortcutsGrid.implicitHeight
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        GridLayout {
            id: shortcutsGrid
            objectName: "shortcutsGrid"
            width: shortcutsScroll.availableWidth
            columns: width >= 640 * root.textScale ? 2 : 1
            columnSpacing: 24
            rowSpacing: 10

            Repeater {
                model: root.shortcuts
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: (shortcutsGrid.width
                        - (shortcutsGrid.columns - 1) * shortcutsGrid.columnSpacing)
                        / shortcutsGrid.columns
                    spacing: 10
                    Label {
                        Layout.preferredWidth: Math.min(Math.round(148 * root.textScale),
                            (shortcutsGrid.width / shortcutsGrid.columns - 10) * 0.5)
                        Layout.minimumWidth: 0
                        text: modelData.key
                        color: root.strongTextColor
                        font.family: "IBM Plex Mono"
                        font.pixelSize: Math.round(12 * root.textScale)
                        font.bold: true
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: modelData.desc
                        color: root.textColor
                        font.family: "IBM Plex Mono"
                        font.pixelSize: Math.round(12 * root.textScale)
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    footer: Item {
        implicitHeight: closeButton.implicitHeight + 20
        SquareDialogButton {
            id: closeButton
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Close")
            primary: true
            darkMode: root.darkMode
            textScale: root.textScale
            activeColor: root.activeButtonColor
            onClicked: root.close()
        }
    }
}

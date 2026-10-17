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
    property string edition: ""
    readonly property string versionText: "Omawrite " + Qt.application.version
        + (edition.length > 0 ? " · " + edition : "")
    // Other features can add their bindings without coupling this component
    // to the backend or to a particular editor layout.
    property var extraShortcuts: []
    readonly property var shortcuts: [
        { key: "Ctrl+S", desc: "Save" },
        { key: "Ctrl+Shift+S", desc: "Save As" },
        { key: "Ctrl+O", desc: "Open" },
        { key: "Ctrl+N", desc: "New Window" },
        { key: "Ctrl+P", desc: "Print" },
        { key: "Ctrl+Z", desc: "Undo" },
        { key: "Ctrl+Shift+Z\nCtrl+Y", desc: "Redo" },
        { key: "F11 / Super+F", desc: "Fullscreen" },
        { key: "Ctrl+F", desc: "Find" },
        { key: "Ctrl+H", desc: "Find & Replace" },
        { key: "Ctrl+G / Enter", desc: "Next Match in Find" },
        { key: "Shift+Enter", desc: "Previous Match in Find" },
        { key: "Ctrl+B", desc: "Bold" },
        { key: "Ctrl+I", desc: "Italic" },
        { key: "Ctrl+Shift+X", desc: "Strikethrough" },
        { key: "Ctrl+K", desc: "Link" },
        { key: "Ctrl+Shift+C", desc: "Copy as HTML" },
        { key: "Ctrl+= / Ctrl++", desc: "Increase Text Size" },
        { key: "Ctrl+-", desc: "Decrease Text Size" },
        { key: "Ctrl+0", desc: "Reset Text Size" },
        { key: "Ctrl+scroll", desc: "Change Text Size" },
        { key: "Ctrl+? / Ctrl+/ / F1", desc: "Shortcuts" }
    ].concat(extraShortcuts)

    title: "Keyboard shortcuts"
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
            text: "Close"
            primary: true
            darkMode: root.darkMode
            textScale: root.textScale
            activeColor: root.activeButtonColor
            onClicked: root.close()
        }
    }
}

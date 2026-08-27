QT += core gui widgets printsupport qml quick quickcontrols2 quickdialogs2
unix:!macx: QT += dbus

CONFIG += c++17 release
TARGET = omawrite
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/markdownhighlighter.h \
    src/systemtheme.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/markdownhighlighter.cpp \
    src/systemtheme.cpp

RESOURCES += src/resources.qrc

macx {
    ICON = macos/omawrite.icns
    QMAKE_INFO_PLIST = macos/Info.plist
}

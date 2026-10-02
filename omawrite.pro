QT += core gui widgets printsupport qml quick quickcontrols2 quickdialogs2

# The XDG desktop portal integration is Linux-only. macOS and Windows use
# QStyleHints for native light/dark mode detection instead.
linux: QT += dbus

CONFIG += c++17 release
TARGET = omawrite
TEMPLATE = app
VERSION = 0.1.0

macx {
    ICON = pkgbuild/omawrite.icns
    QMAKE_INFO_PLIST = pkgbuild/Info.plist
}

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

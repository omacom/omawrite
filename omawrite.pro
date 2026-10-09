QT += core gui widgets printsupport qml quick quickcontrols2 quickdialogs2
# The desktop portal (dark mode and text size) is reached over D-Bus, which
# macOS does not have; Qt's own color scheme covers it there.
!macx: QT += dbus

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
    # A native app bundle, so Finder can offer Open With and the Dock shows the
    # icon. Every window lives in this one process; see windowmanager.h.
    QMAKE_TARGET_BUNDLE_PREFIX = io.omacom
    QMAKE_INFO_PLIST = macos/Info.plist
    ICON = macos/omawrite.icns
    # Keep in step with pkgver in pkgbuild/PKGBUILD.
    VERSION = 0.1.0

    HEADERS += src/windowmanager.h
    SOURCES += src/windowmanager.cpp
}

QT += core gui widgets printsupport qml quick quickcontrols2 quickdialogs2

unix:!macx {
    QT += dbus
    SOURCES += src/systemtheme_linux.cpp
}

macx {
    ICON = pkgbuild/omawrite.icns
    SOURCES += src/systemtheme_macos.cpp
}

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

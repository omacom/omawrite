QT += core gui widgets printsupport qml quick quickcontrols2 quickdialogs2 dbus svg

CONFIG += c++17 release
TARGET = omawrite
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/markdownhighlighter.h \
    src/mathoverlay.h \
    src/mathrenderer.h \
    src/systemtheme.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/markdownhighlighter.cpp \
    src/mathoverlay.cpp \
    src/mathrenderer.cpp \
    src/systemtheme.cpp

RESOURCES += src/resources.qrc src/mathjax.qrc

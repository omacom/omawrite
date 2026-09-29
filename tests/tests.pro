QT += core gui quick testlib
CONFIG += testcase c++17
TEMPLATE = app
TARGET = tst_omawrite

INCLUDEPATH += ../src
SOURCES += \
    tst_omawrite.cpp \
    ../src/backend.cpp \
    ../src/markdownhighlighter.cpp \
    ../src/mathoverlay.cpp \
    ../src/mathrenderer.cpp
HEADERS += \
    ../src/backend.h \
    ../src/markdownhighlighter.h \
    ../src/mathoverlay.h \
    ../src/mathrenderer.h
RESOURCES += ../src/mathjax.qrc

QT += widgets printsupport quickcontrols2 quickdialogs2 dbus svg

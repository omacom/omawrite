QT += core gui quick testlib
CONFIG += testcase c++17
macx: CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_omawrite

INCLUDEPATH += ../src
SOURCES += \
    tst_omawrite.cpp \
    ../src/backend.cpp \
    ../src/markdownhighlighter.cpp \
    ../src/systemtheme.cpp
HEADERS += \
    ../src/backend.h \
    ../src/markdownhighlighter.h \
    ../src/systemtheme.h

QT += widgets printsupport quickcontrols2 quickdialogs2
linux: QT += dbus

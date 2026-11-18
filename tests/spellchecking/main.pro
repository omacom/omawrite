linux: QT += dbus
QT += core gui concurrent quick testlib widgets printsupport quickcontrols2 quickdialogs2
CONFIG += testcase c++17 console
CONFIG -= app_bundle
include(../../syntax-highlighting.pri)
include(../../spellchecking.pri)

TEMPLATE = app
TARGET = tst_spelling_main
INCLUDEPATH += ../../src
SOURCES += \
    tst_main.cpp \
    ../../src/backend.cpp \
    ../../src/printdocument.cpp \
    ../../src/spellingcontroller.cpp \
    ../../src/spellingprose.cpp \
    ../../src/cli.cpp \
    ../../src/markdownhighlighter.cpp \
    ../../src/livepreviewtables.cpp \
    ../../src/markdowntablemodel.cpp \
    ../../src/livepreview.cpp \
    ../../src/previewblocks.cpp \
    ../../src/mermaidrenderer.cpp
HEADERS += \
    ../../src/backend.h \
    ../../src/spellingcontroller.h \
    ../../src/spellingprose.h \
    ../../src/cli.h \
    ../../src/markdownhighlighter.h \
    ../../src/markdowntablemodel.h \
    ../../src/livepreview.h \
    ../../src/previewblocks.h \
    ../../src/mermaidrenderer.h

QT += core gui quick qml testlib concurrent
CONFIG += testcase c++17 console
CONFIG -= app_bundle
include(../../spellchecking.pri)
include(../../syntax-highlighting.pri)

TEMPLATE = app
TARGET = tst_spellingintegration
SOURCES += tst_integration.cpp \
    ../../src/livepreviewtables.cpp \
    ../../src/markdowntablemodel.cpp \
    ../../src/livepreview.cpp \
    ../../src/mermaidrenderer.cpp \
    ../../src/previewblocks.cpp \
    ../../src/markdownhighlighter.cpp \
    ../../src/spellingcontroller.cpp \
    ../../src/spellingprose.cpp
HEADERS += ../../src/markdowntablemodel.h ../../src/livepreview.h \
    ../../src/mermaidrenderer.h \
    ../../src/previewblocks.h \
    ../../src/markdownhighlighter.h \
    ../../src/spellingcontroller.h \
    ../../src/spellingprose.h

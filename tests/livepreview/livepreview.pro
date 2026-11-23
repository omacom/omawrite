QT += core gui qml quick testlib concurrent
CONFIG += testcase c++17 console
CONFIG -= app_bundle
include(../../syntax-highlighting.pri)
include(../../spellchecking.pri)

TEMPLATE = app
TARGET = tst_livepreview
SOURCES += tst_livepreview.cpp ../../src/livepreviewtables.cpp ../../src/markdowntablemodel.cpp ../../src/livepreview.cpp ../../src/mermaidrenderer.cpp ../../src/previewblocks.cpp ../../src/markdownhighlighter.cpp \
    ../../src/spellingcontroller.cpp ../../src/spellingprose.cpp
HEADERS += ../../src/markdownreferences.h ../../src/markdowntablemodel.h ../../src/livepreview.h ../../src/mermaidrenderer.h ../../src/previewblocks.h ../../src/markdownhighlighter.h \
    ../../src/spellingcontroller.h ../../src/spellingprose.h

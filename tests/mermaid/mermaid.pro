QT += core gui testlib
CONFIG += c++17 testcase console
CONFIG -= app_bundle
include(../../syntax-highlighting.pri)

TEMPLATE = app
TARGET = tst_mermaid
INCLUDEPATH += ../../src
SOURCES += tst_mermaid.cpp ../../src/mermaidrenderer.cpp ../../src/previewblocks.cpp ../../src/markdownhighlighter.cpp
HEADERS += ../../src/mermaidrenderer.h ../../src/previewblocks.h ../../src/markdownhighlighter.h

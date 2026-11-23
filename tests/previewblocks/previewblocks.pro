QT += gui testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
include(../../syntax-highlighting.pri)

TEMPLATE = app
TARGET = tst_previewblocks
SOURCES += tst_previewblocks.cpp ../../src/previewblocks.cpp ../../src/markdownhighlighter.cpp
HEADERS += ../../src/previewblocks.h ../../src/markdownhighlighter.h

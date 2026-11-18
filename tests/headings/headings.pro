QT += core gui testlib
CONFIG += testcase c++17 console
CONFIG -= app_bundle
include(../../syntax-highlighting.pri)

TEMPLATE = app
TARGET = tst_headings
INCLUDEPATH += ../../src
SOURCES += tst_headings.cpp ../../src/markdownhighlighter.cpp
HEADERS += ../../src/markdownhighlighter.h

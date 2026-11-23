QT += gui testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_codehighlighting
include(../../syntax-highlighting.pri)
SOURCES += tst_codehighlighting.cpp ../../src/markdownhighlighter.cpp
HEADERS += ../../src/markdownhighlighter.h

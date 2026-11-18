QT += core network testlib
QT -= gui
CONFIG += testcase c++17 console
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tst_instancebroker
INCLUDEPATH += ../../src
SOURCES += tst_instancebroker.cpp ../../src/instancebroker.cpp
HEADERS += ../../src/instancebroker.h

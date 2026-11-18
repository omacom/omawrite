!macx: error("The optional native spelling smoke test requires macOS.")
QT += core gui concurrent testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_native_spellchecking

include(../../spellchecking.pri)
INCLUDEPATH += ../../src
SOURCES += tst_native.cpp \
    ../../src/spellingcontroller.cpp \
    ../../src/spellingprose.cpp
HEADERS += ../../src/spellingcontroller.h ../../src/spellingprose.h

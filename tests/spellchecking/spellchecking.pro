QT += core gui concurrent testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_spellchecking

include(../../spellchecking.pri)
INCLUDEPATH += ../../src
SOURCES += tst_spellchecking.cpp \
    ../../src/spellingcontroller.cpp \
    ../../src/spellingprose.cpp
HEADERS += ../../src/spellingcontroller.h ../../src/spellingprose.h

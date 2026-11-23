QT += core gui widgets printsupport concurrent testlib
include(../../syntax-highlighting.pri)
include(../../spellchecking.pri)
CONFIG += c++17 testcase console
TEMPLATE = app
TARGET = tst_printing
INCLUDEPATH += ../../src
SOURCES += tst_printing.cpp ../../src/printdocument.cpp ../../src/markdownhighlighter.cpp \
    ../../src/spellingcontroller.cpp ../../src/spellingprose.cpp
HEADERS += ../../src/printdocument.h ../../src/attachmenthelper.h ../../src/listpreview.h \
    ../../src/markdownhighlighter.h ../../src/spellingcontroller.h ../../src/spellingprose.h

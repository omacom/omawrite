linux: QT += dbus
QT += core gui widgets concurrent printsupport qml quick testlib
CONFIG += c++17 testcase console
include(../../syntax-highlighting.pri)
include(../../spellchecking.pri)

TEMPLATE = app
TARGET = tst_attachments
INCLUDEPATH += ../../src
SOURCES += tst_attachments.cpp ../../src/backend.cpp ../../src/printdocument.cpp ../../src/markdownhighlighter.cpp \
    ../../src/spellingcontroller.cpp ../../src/spellingprose.cpp
HEADERS += ../../src/backend.h ../../src/attachmenthelper.h ../../src/markdownhighlighter.h \
    ../../src/spellingcontroller.h ../../src/spellingprose.h

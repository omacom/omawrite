QT += core gui concurrent quick testlib
linux: QT += dbus
CONFIG += testcase c++17
include(../syntax-highlighting.pri)
include(../spellchecking.pri)

TEMPLATE = app
TARGET = tst_editor
macx {
    OBJECTIVE_SOURCES += ../src/macapplicationreopen.mm macapplicationreopentest.mm
    LIBS += -framework AppKit
}

INCLUDEPATH += ../src
SOURCES += \
    tst_editor.cpp \
    ../src/backend.cpp \
    ../src/printdocument.cpp \
    ../src/windowmanager.cpp \
    ../src/fileopenrequests.cpp \
    ../src/systemtheme.cpp \
    ../src/translator.cpp \
    ../src/spellingcontroller.cpp \
    ../src/spellingprose.cpp \
    ../src/cli.cpp \
    ../src/markdownhighlighter.cpp \
    ../src/markdowntablemodel.cpp \
    ../src/livepreviewtables.cpp \
    ../src/livepreview.cpp \
    ../src/previewblocks.cpp \
    ../src/mermaidrenderer.cpp
HEADERS += \
    ../src/backend.h \
    ../src/printdocument.h \
    ../src/windowmanager.h \
    ../src/fileopenrequests.h \
    ../src/systemtheme.h \
    ../src/translator.h \
    ../src/spellingcontroller.h \
    ../src/spellingprose.h \
    ../src/cli.h \
    ../src/markdownhighlighter.h \
    ../src/markdownreferences.h \
    ../src/markdowntablemodel.h \
    ../src/livepreview.h \
    ../src/previewblocks.h \
    ../src/mermaidrenderer.h

QT += widgets printsupport quickcontrols2 quickdialogs2
RESOURCES += ../src/resources.qrc

# Shared by app and tests; the static library includes offline definitions/themes.
SYNTAX_HIGHLIGHTING_PREFIX = $$(OMAWRITE_SYNTAX_PREFIX)
isEmpty(SYNTAX_HIGHLIGHTING_PREFIX): SYNTAX_HIGHLIGHTING_PREFIX = $$PWD/.deps/install
!exists($$SYNTAX_HIGHLIGHTING_PREFIX/include/KF6/KSyntaxHighlighting/KSyntaxHighlighting/Repository) {
    error("KSyntaxHighlighting is required. Run bin/build-dependencies (see docs/install.md), or set OMAWRITE_SYNTAX_PREFIX to a matched static installation.")
}
# The private static dependency is Release-only, including Windows' CRT.
CONFIG -= debug debug_and_release
CONFIG += release
QT += network
INCLUDEPATH += $$SYNTAX_HIGHLIGHTING_PREFIX/include/KF6/KSyntaxHighlighting
DEFINES += KSYNTAXHIGHLIGHTING_STATIC_DEFINE
LIBS += -L$$quote($$SYNTAX_HIGHLIGHTING_PREFIX/lib) -lKF6SyntaxHighlighting
SOURCES += $$PWD/src/codehighlighter.cpp $$PWD/src/readcodehighlighting.cpp
HEADERS += $$PWD/src/codehighlighter.h $$PWD/src/readcodehighlighting.h

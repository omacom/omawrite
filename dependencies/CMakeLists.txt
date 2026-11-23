cmake_minimum_required(VERSION 3.24)
project(OmawriteDependencies LANGUAGES CXX)
include(ExternalProject)
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Network)
# KSyntaxHighlighting generates PHP/Twig/Doxygen definitions even without tests.
# Fail early and forward an explicit native Perl path into the external build.
find_package(Perl REQUIRED)

# Standalone, pinned Kate syntax engine. No Kate editor, WebEngine, or desktop.
set(prefix "${CMAKE_BINARY_DIR}/install")
ExternalProject_Add(ecm
    URL https://download.kde.org/stable/frameworks/6.13/extra-cmake-modules-6.13.0.tar.xz
    URL_HASH SHA256=7006017c00c817ff4c056995146d271791d1487a398d39ea6cac1cd59a8bf402
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=${prefix}
        -DBUILD_TESTING:BOOL=OFF -DBUILD_HTML_DOCS:BOOL=OFF
        -DBUILD_QTHELP_DOCS:BOOL=OFF -DBUILD_MAN_DOCS:BOOL=OFF
)
ExternalProject_Add(syntax-highlighting
    DEPENDS ecm
    URL https://download.kde.org/stable/frameworks/6.13/syntax-highlighting-6.13.0.tar.xz
    URL_HASH SHA256=d6f19bb886d6e1c7251d813497f14ecc9dac413253764eb61a8f3a836806450d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=${prefix}
        -DECM_DIR:PATH=${prefix}/share/ECM/cmake -DQt6_DIR:PATH=${Qt6_DIR}
        -DCMAKE_BUILD_TYPE:STRING=Release -DBUILD_SHARED_LIBS:BOOL=OFF
        -DBUILD_TESTING:BOOL=OFF -DBUILD_QCH:BOOL=OFF -DKF_SKIP_PO_PROCESSING:BOOL=ON
        -DKDE_INSTALL_LIBDIR:PATH=lib -DKDE_INSTALL_INCLUDEDIR_KF:PATH=include/KF6
        -DKSYNTAXHIGHLIGHTING_USE_GUI:BOOL=ON -DQRC_SYNTAX:BOOL=ON
        -DPERL_EXECUTABLE:FILEPATH=${PERL_EXECUTABLE}
        -DBUILD_codeeditor:BOOL=OFF -DBUILD_codepdfprinter:BOOL=OFF -DBUILD_minimaltest:BOOL=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_Qt6Quick:BOOL=ON -DCMAKE_DISABLE_FIND_PACKAGE_XercesC:BOOL=ON
        -DCMAKE_CXX_STANDARD:STRING=17 -DCMAKE_CXX_STANDARD_REQUIRED:BOOL=ON
        -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release
)

# Sonnet Core only: native dictionaries on macOS/Windows, installed Hunspell on
# Linux. No Sonnet widgets, QML editor, browser, or bundled dictionary data.
set(sonnet_platform_args)
if(APPLE OR WIN32)
    list(APPEND sonnet_platform_args -DCMAKE_DISABLE_FIND_PACKAGE_HUNSPELL:BOOL=ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SYSTEM_HUNSPELL REQUIRED hunspell)
endif()
ExternalProject_Add(sonnet
    DEPENDS ecm
    URL https://download.kde.org/stable/frameworks/6.13/sonnet-6.13.0.tar.xz
    URL_HASH SHA256=d47465a9d623257cfb0192c3a4dc24733490aa8952310422e402d828ad461eee
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR:PATH=<SOURCE_DIR>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/sonnet-static-plugins.cmake
    CMAKE_ARGS ${sonnet_platform_args} -DCMAKE_INSTALL_PREFIX:PATH=${prefix}
        -DECM_DIR:PATH=${prefix}/share/ECM/cmake -DQt6_DIR:PATH=${Qt6_DIR}
        -DCMAKE_BUILD_TYPE:STRING=Release -DBUILD_SHARED_LIBS:BOOL=OFF
        -DBUILD_TESTING:BOOL=OFF -DBUILD_QCH:BOOL=OFF -DBUILD_EXAMPLES:BOOL=OFF
        -DKF_SKIP_PO_PROCESSING:BOOL=ON -DSONNET_USE_WIDGETS:BOOL=OFF -DSONNET_USE_QML:BOOL=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_ASPELL:BOOL=ON -DCMAKE_DISABLE_FIND_PACKAGE_HSPELL:BOOL=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_VOIKKO:BOOL=ON
        -DKDE_INSTALL_LIBDIR:PATH=lib -DKDE_INSTALL_INCLUDEDIR_KF:PATH=include/KF6
        -DCMAKE_CXX_STANDARD:STRING=17 -DCMAKE_CXX_STANDARD_REQUIRED:BOOL=ON
        -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release
)

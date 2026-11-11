cmake_minimum_required(VERSION 3.21)
project(omawrite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Widgets and PrintSupport are for the system print dialog, which is why the
# app runs a QApplication rather than a QGuiApplication.
find_package(Qt6 REQUIRED COMPONENTS
  Core Gui Concurrent Widgets PrintSupport Qml Quick QuickControls2 QuickDialogs2 DBus Test)

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB omawrite_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM omawrite_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(omawrite_core STATIC ${omawrite_sources})
target_include_directories(omawrite_core PUBLIC src)
target_link_libraries(omawrite_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Widgets Qt6::PrintSupport
  Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2 Qt6::DBus)
target_compile_options(omawrite_core PUBLIC -Wall -Wextra)

add_executable(omawrite src/main.cpp src/resources.qrc)
target_link_libraries(omawrite PRIVATE omawrite_core)

# The tests read the QML they drive straight from src/.
enable_testing()
add_executable(omawrite_tests tests/omawrite_tests.cpp)
target_link_libraries(omawrite_tests PRIVATE omawrite_core Qt6::Test)
add_test(NAME omawrite_tests COMMAND omawrite_tests)
set_tests_properties(omawrite_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

cmake_minimum_required(VERSION 3.21)

project(qrgen
    VERSION 0.1.0
    DESCRIPTION "A minimal Qt 6 front end for libqrencode"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(QRENCODE REQUIRED IMPORTED_TARGET libqrencode)

qt_standard_project_setup()

# Everything except main() lives in a static library so that the test suite can
# link against exactly the same code the application runs.
add_library(qrgen_core STATIC
    src/dropzone.cpp
    src/dropzone.h
    src/mainwindow.cpp
    src/mainwindow.h
    src/mimetext.cpp
    src/mimetext.h
    src/qrcode.cpp
    src/qrcode.h
    src/qrview.cpp
    src/qrview.h
    src/theme.cpp
    src/theme.h
)
target_include_directories(qrgen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(qrgen_core PUBLIC Qt6::Widgets PkgConfig::QRENCODE)
target_compile_options(qrgen_core PRIVATE -Wall -Wextra)

qt_add_executable(qrgen src/main.cpp)
target_link_libraries(qrgen PRIVATE qrgen_core)

include(GNUInstallDirs)
install(TARGETS qrgen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

option(QRGEN_BUILD_TESTS "Build the unit tests" ON)
if(QRGEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(QRGEN_BUILD_TEST_TOOLS "Build the helpers used by the GUI smoke test" OFF)
if(QRGEN_BUILD_TEST_TOOLS)
    add_subdirectory(tools)
endif()

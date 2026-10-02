cmake_minimum_required(VERSION 3.21)

project(enquber
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
add_library(enquber_core STATIC
    src/aboutpage.cpp
    src/aboutpage.h
    src/dropzone.cpp
    src/dropzone.h
    src/mainwindow.cpp
    src/mainwindow.h
    src/mimeimage.cpp
    src/mimeimage.h
    src/mimetext.cpp
    src/mimetext.h
    src/qrcode.cpp
    src/qrcode.h
    src/qrview.cpp
    src/qrview.h
    src/theme.cpp
    src/theme.h
)
target_include_directories(enquber_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(enquber_core PUBLIC Qt6::Widgets PkgConfig::QRENCODE)
target_compile_options(enquber_core PRIVATE -Wall -Wextra)

# The action glyphs and the app icon are embedded so that the interface keeps
# its pictures even on a session whose icon theme only ships hicolor.
qt_add_resources(enquber_core "enquber_icons"
    PREFIX "/enquber/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/data/icon"
    FILES
        data/icon/enquber.png
        data/icon/actions/arrow-left.png
        data/icon/actions/document-save.png
        data/icon/actions/edit-clear.png
        data/icon/actions/edit-copy.png
        data/icon/actions/help-circle.png
        data/icon/actions/info.png
        data/icon/actions/insert-link.png
)

qt_add_executable(enquber src/main.cpp)
target_link_libraries(enquber PRIVATE enquber_core)

include(GNUInstallDirs)
install(TARGETS enquber RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/enquber.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/icon/enquber.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/icon/enquber.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps)

option(ENQUBER_BUILD_TESTS "Build the unit tests" ON)
if(ENQUBER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(ENQUBER_BUILD_TEST_TOOLS "Build the helpers used by the GUI smoke test" OFF)
if(ENQUBER_BUILD_TEST_TOOLS)
    add_subdirectory(tools)
endif()

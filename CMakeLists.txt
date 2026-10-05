cmake_minimum_required(VERSION 3.21)

project(enquber
    VERSION 0.1.0
    DESCRIPTION "A minimal Qt 6 front end for libqrencode"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets LinguistTools)
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
    src/i18n.cpp
    src/i18n.h
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

# The catalogs are compiled and embedded in enquber_core so that the app and
# every test helper that links the library load the same ":/i18n" resources.
qt_add_translations(enquber_core
    TS_FILES
        i18n/enquber_en.ts
        i18n/enquber_es.ts)

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

    # Lint the text IDs and the catalogs. The script needs only Python and
    # works on the committed catalogs, so a dropped //% or a stale .ts does not
    # silently ship a raw ID. Registered after enable_testing() so ctest sees it.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME check_i18n
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-i18n.py
                    --source ${CMAKE_CURRENT_SOURCE_DIR}/src
                    --translations ${CMAKE_CURRENT_SOURCE_DIR}/i18n
                    --require-translated es)
    else()
        message(WARNING "Python3 not found: the check_i18n translation lint will not run")
    endif()
endif()

option(ENQUBER_BUILD_TEST_TOOLS "Build the helpers used by the GUI smoke test" OFF)
if(ENQUBER_BUILD_TEST_TOOLS)
    add_subdirectory(tools)
endif()

# CMake toolchain file: cross-compile Enquber for 64-bit Windows with MinGW-w64.
#
# The target Qt 6 and libqrencode come from an MSYS2 "mingw64" tree, while the
# native Qt installation supplies the host tools (lupdate, lrelease) through
# QT_HOST_PATH. See the README section "Cross-compiling for Windows" for how to
# fetch the tree and install its dependencies.
#
# Point ENQUBER_MINGW_PREFIX (cache variable or environment) at the root of the
# extracted tree when it does not live in the default location.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

if(NOT DEFINED ENQUBER_MINGW_PREFIX)
    if(NOT "$ENV{ENQUBER_MINGW_PREFIX}" STREQUAL "")
        set(ENQUBER_MINGW_PREFIX "$ENV{ENQUBER_MINGW_PREFIX}" CACHE PATH
            "Root of the extracted MSYS2 mingw64 tree")
    else()
        set(ENQUBER_MINGW_PREFIX "$ENV{HOME}/.local/msys2-mingw64" CACHE PATH
            "Root of the extracted MSYS2 mingw64 tree")
    endif()
endif()

if(NOT EXISTS "${ENQUBER_MINGW_PREFIX}/mingw64/lib/cmake/Qt6/Qt6Config.cmake")
    message(FATAL_ERROR
        "No Qt 6 below ${ENQUBER_MINGW_PREFIX}/mingw64.\n"
        "Fetch the MinGW SDK with packaging/windows/mingw-sdk.py, or point "
        "ENQUBER_MINGW_PREFIX at an existing MSYS2 mingw64 tree.")
endif()

set(CMAKE_FIND_ROOT_PATH "${ENQUBER_MINGW_PREFIX}/mingw64")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Qt's cross-compilation support prepends QT_HOST_PATH to the find root paths,
# so the Qt6*Tools and LinguistTools packages resolve to the native Qt
# installation even though packages are only searched below the root path.
if(NOT DEFINED QT_HOST_PATH)
    if(NOT "$ENV{QT_HOST_PATH}" STREQUAL "")
        set(QT_HOST_PATH "$ENV{QT_HOST_PATH}" CACHE PATH
            "Native Qt 6 installation providing the host tools")
    else()
        set(QT_HOST_PATH "/usr" CACHE PATH
            "Native Qt 6 installation providing the host tools")
    endif()
endif()

# Point pkg-config at the target tree. The .pc files there use /mingw64 as
# their prefix, which PKG_CONFIG_SYSROOT_DIR rewrites to the extracted tree.
set(ENV{PKG_CONFIG_LIBDIR} "${ENQUBER_MINGW_PREFIX}/mingw64/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${ENQUBER_MINGW_PREFIX}")
unset(ENV{PKG_CONFIG_PATH})

cmake_minimum_required(VERSION 3.16)
project(CustomUSBProtocol LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(CustomUSBProtocol
    main.c
    include/usbdev.c
    include/jsoncmd.c
    include/proto.c
    include/app.c
    include/ui.c
    include/tray.c
)

# Clay is a single-header library; exactly one translation unit may define its
# implementation, and ui.c is that unit.
set_source_files_properties(include/ui.c PROPERTIES
    COMPILE_DEFINITIONS "CLAY_IMPLEMENTATION")
set_target_properties(CustomUSBProtocol PROPERTIES OUTPUT_NAME "main")

# Only Windows gets the extension. CMake would append it there anyway, but
# stating it keeps the intent obvious -- setting SUFFIX unconditionally is what
# produced a "main.exe" on Linux.
if(WIN32)
    set_target_properties(CustomUSBProtocol PROPERTIES SUFFIX ".exe")
endif()

# Our own headers, plus the vendored Clay single-headers in libs/. libusb's
# header is NOT here -- it comes from vcpkg or the system, and a stale copy in
# this directory would shadow it.
target_include_directories(CustomUSBProtocol PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs")

# --- raylib, the renderer behind Clay ----------------------------------------
#
# Clay only computes layout, so it needs a backend to draw and to own the
# window. vcpkg provides a config package on Windows; elsewhere raylib comes
# from the system and pkg-config or a plain link is enough.
find_package(raylib CONFIG QUIET)
if(TARGET raylib)
    target_link_libraries(CustomUSBProtocol PRIVATE raylib)
    message(STATUS "raylib found via: CMake package")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RAYLIB QUIET IMPORTED_TARGET raylib)
    endif()
    if(TARGET PkgConfig::RAYLIB)
        target_link_libraries(CustomUSBProtocol PRIVATE PkgConfig::RAYLIB)
        message(STATUS "raylib found via: pkg-config")
    else()
        message(FATAL_ERROR
            "raylib not found.\n"
            "  vcpkg:           vcpkg install raylib:<triplet>\n"
            "  Debian/Ubuntu:   sudo apt install libraylib-dev\n"
            "  macOS:           brew install raylib")
    endif()
endif()

# raylib and the wheel generator both need the math library on Unix.
if(UNIX)
    target_link_libraries(CustomUSBProtocol PRIVATE m)
endif()

# The tray and the window icon go through the Windows shell.
if(WIN32)
    target_link_libraries(CustomUSBProtocol PRIVATE shell32 user32 gdi32)
endif()

# The interface loads its fonts from resources/ relative to the working
# directory, so keep a copy beside the executable.
add_custom_command(TARGET CustomUSBProtocol POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/resources"
            "$<TARGET_FILE_DIR:CustomUSBProtocol>/resources")

if(MSVC)
    target_compile_definitions(CustomUSBProtocol PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(CustomUSBProtocol PRIVATE /W3)
else()
    target_compile_options(CustomUSBProtocol PRIVATE -Wall -Wextra)
endif()

# --- locate libusb-1.0 -------------------------------------------------------
#
# Resolution order:
#   1. pkg-config   Linux, macOS, MSYS2, and vcpkg when pkgconf is on PATH
#   2. find_path / find_library
#                   vcpkg (its toolchain puts installed/<triplet> on
#                   CMAKE_PREFIX_PATH, so no vcpkg-specific code is needed),
#                   Homebrew, system packages, or -DLIBUSB_ROOT=<dir>
#
# vcpkg ships no CMake config package for libusb, so find_package(libusb) is
# not attempted -- it would always fail and only add noise.
#
set(LIBUSB_SOURCE "")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
endif()

if(TARGET PkgConfig::LIBUSB)
    target_link_libraries(CustomUSBProtocol PRIVATE PkgConfig::LIBUSB)
    set(LIBUSB_SOURCE "pkg-config")
else()
    # vcpkg installs the header as <prefix>/include/libusb-1.0/libusb.h, so the
    # libusb-1.0 suffix is what makes plain #include <libusb.h> resolve.
    find_path(LIBUSB_INCLUDE_DIR
        NAMES libusb.h
        PATH_SUFFIXES libusb-1.0
        HINTS ${LIBUSB_ROOT} ENV LIBUSB_ROOT)

    find_library(LIBUSB_LIBRARY
        NAMES usb-1.0 libusb-1.0
        HINTS ${LIBUSB_ROOT} ENV LIBUSB_ROOT)

    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
        set(_hint "")
        if(DEFINED VCPKG_TARGET_TRIPLET)
            # Being here with a vcpkg toolchain almost always means the port is
            # installed for a different triplet than the one being built.
            set(_hint
                "\n  This build uses vcpkg triplet '${VCPKG_TARGET_TRIPLET}'."
                "\n  Install libusb for it:"
                "\n      vcpkg install libusb:${VCPKG_TARGET_TRIPLET}"
                "\n  A port installed for another triplet is not visible here.")
        endif()
        message(FATAL_ERROR
            "libusb-1.0 not found.\n"
            "  vcpkg:           vcpkg install libusb:<triplet>, and configure with\n"
            "                   -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake\n"
            "  Debian/Ubuntu:   sudo apt install libusb-1.0-0-dev\n"
            "  Fedora:          sudo dnf install libusb1-devel\n"
            "  macOS:           brew install libusb\n"
            "  MSYS2:           pacman -S mingw-w64-x86_64-libusb\n"
            "  unpacked build:  -DLIBUSB_ROOT=C:/path/to/libusb"
            ${_hint})
    endif()

    target_include_directories(CustomUSBProtocol PRIVATE ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(CustomUSBProtocol PRIVATE ${LIBUSB_LIBRARY})
    set(LIBUSB_SOURCE "${LIBUSB_LIBRARY}")

    # libusb reaches WinUSB through the setup API and COM. Required for a static
    # link, harmless alongside an import library.
    if(WIN32)
        target_link_libraries(CustomUSBProtocol PRIVATE setupapi ole32 advapi32)
    endif()
endif()

# Put the runtime DLL beside the executable. vcpkg only does this automatically
# for MSVC targets, so a MinGW build would otherwise fail to start.
if(WIN32 AND LIBUSB_LIBRARY)
    get_filename_component(_libusb_libdir "${LIBUSB_LIBRARY}" DIRECTORY)
    get_filename_component(_libusb_prefix "${_libusb_libdir}" DIRECTORY)

    find_file(LIBUSB_RUNTIME_DLL
        NAMES libusb-1.0.dll usb-1.0.dll
        HINTS "${_libusb_prefix}/bin" "${_libusb_libdir}"
        NO_DEFAULT_PATH)

    if(LIBUSB_RUNTIME_DLL)
        add_custom_command(TARGET CustomUSBProtocol POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LIBUSB_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:CustomUSBProtocol>")
        message(STATUS "libusb runtime: ${LIBUSB_RUNTIME_DLL}")
    endif()
endif()

message(STATUS "libusb-1.0 found via: ${LIBUSB_SOURCE}")

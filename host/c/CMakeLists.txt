cmake_minimum_required(VERSION 3.16)
project(CustomUSBProtocol LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(CustomUSBProtocol
    main.c
    include/usbdev.c
    include/jsoncmd.c
    include/proto.c
)
set_target_properties(CustomUSBProtocol PROPERTIES
    OUTPUT_NAME "main"
    SUFFIX ".exe"
)

# Our own headers. libusb's header is NOT here -- it comes from vcpkg or the
# system, and a stale copy in this directory would shadow it.
target_include_directories(CustomUSBProtocol PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

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

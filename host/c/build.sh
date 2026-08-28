#!/usr/bin/env bash
#
# Ubuntu counterpart of build.bat.
#
#   ./build.sh          configure if needed, compile, then run
#   ./build.sh clear    clear the screen, compile, then run
#   ./build.sh clean    wipe the build directory and reconfigure
#   ./build.sh run      run without recompiling
#   ./build.sh help     this text
#
# Unlike the Windows build there is no vcpkg or MinGW here: libusb comes from
# the system package manager and CMake finds it through pkg-config.
#
#   sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev
#
# Talking to the device needs permission for the USB node. Either run as root,
# or install a udev rule once:
#
#   echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4001", MODE="0660", TAG+="uaccess"' \
#     | sudo tee /etc/udev/rules.d/99-esp32s3-vendor.rules
#   sudo udevadm control --reload-rules && sudo udevadm trigger
#
set -uo pipefail

cd "$(dirname "$0")" || exit 1

BUILD_DIR=build

# CMake appends ".exe" on Windows only, so this is "main" here. The fallback
# covers a build directory left over from before that was made conditional.
exe_path() {
    if   [ -f "$BUILD_DIR/main" ];     then printf '%s\n' "$BUILD_DIR/main"
    elif [ -f "$BUILD_DIR/main.exe" ]; then printf '%s\n' "$BUILD_DIR/main.exe"
    else return 1
    fi
}

require() {
    command -v "$1" >/dev/null 2>&1 && return 0
    echo "$1 not found. Install it with:" >&2
    echo "    sudo apt install $2" >&2
    exit 1
}

usage() {
    echo "Usage: ./build.sh [command]"
    echo
    echo "  (none)   configure if needed, compile, then run"
    echo "  clear    clear the console, compile, then run"
    echo "  clean    wipe the build directory and reconfigure"
    echo "  run      run without recompiling"
    echo "  help     show this text"
}

setup() {
    require cmake cmake
    require pkg-config pkg-config

    if ! pkg-config --exists libusb-1.0; then
        echo "libusb-1.0 development files not found. Install them with:" >&2
        echo "    sudo apt install libusb-1.0-0-dev" >&2
        exit 1
    fi

    rm -rf "$BUILD_DIR"
    cmake -S . -B "$BUILD_DIR" || exit 1
}

compile() {
    echo "Compiling main..."
    cmake --build "$BUILD_DIR" || exit 1
    echo "Compiling done !"
    printf '\n\n\n'
}

run() {
    local exe
    if ! exe=$(exe_path); then
        echo "Failed to compile !" >&2
        exit 1
    fi

    "./$exe"
    local status=$?

    # libusb reports a permission problem as a failure to open the device.
    if [ $status -ne 0 ] && [ "$(id -u)" -ne 0 ]; then
        echo >&2
        echo "If the device was found but could not be opened, you likely need" >&2
        echo "the udev rule described at the top of this script." >&2
    fi
    return $status
}

case "${1-}" in
    "")
        # Configure only when there is no cache, then build and run.
        [ -f "$BUILD_DIR/CMakeCache.txt" ] || setup
        compile
        run
        ;;
    clear)
        clear
        compile
        run
        ;;
    clean)
        setup
        echo "Configured. Run ./build.sh to compile."
        ;;
    run)
        run
        ;;
    help|-h|--help|/?)
        usage
        ;;
    *)
        echo "Unknown option: $1" >&2
        echo >&2
        usage
        exit 1
        ;;
esac

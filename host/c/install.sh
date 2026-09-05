#!/usr/bin/env bash
#
# Installs IOMeeter for the current user.
#
#   ./install.sh              prerequisites, build, install
#   sudo ./install.sh --udev  install the USB permission rule
#   ./install.sh --uninstall  remove the installed copy
#   ./install.sh --check      report whether the USB rule is working
#
# Everything lands in ~/.IOMeeter, with a launcher in the application menu so
# it appears in the panel with its own icon. Nothing needs root except --udev.
#
# Do NOT run the install with sudo: $HOME becomes /root and it would all go to
# the wrong account. IOMeeter must also run as your own user, because the audio
# mixer talks to the desktop session's sound server, which root cannot reach.
#
set -uo pipefail

cd "$(dirname "$0")" || exit 1

TARGET="$HOME/.IOMeeter"
BIN="$TARGET/IOMeeter"
DESKTOP_DIR="$HOME/.local/share/applications"
DESKTOP_FILE="$DESKTOP_DIR/IOMeeter.desktop"
THEME_DIR="$HOME/.local/share/icons/hicolor"
ICON_DIR="$THEME_DIR/128x128/apps"

# Must sort before systemd's 73-seat-late.rules, which is what turns the
# uaccess tag into an ACL. A rule numbered above that sets the tag too late for
# anything to read it.
UDEV_RULE=/etc/udev/rules.d/60-iomeeter.rules

# Rules earlier versions told people to write by hand, all numbered too late.
UDEV_RULES_STALE="/etc/udev/rules.d/99-iomeeter.rules
/etc/udev/rules.d/99-esp32s3-vendor.rules"

# Without these there is no build at all.
APT_REQUIRED="build-essential cmake pkg-config libusb-1.0-0-dev libpulse-dev"

# These the build can do without: no tray icon, or raylib from somewhere other
# than apt. libraylib-dev only exists from Debian 12 and Ubuntu 23.04 onward,
# so on Mint 21 and Ubuntu 22.04 it is simply absent.
APT_OPTIONAL="libayatana-appindicator3-dev libraylib-dev"

MODE=install

usage() {
    echo "Usage: ./install.sh [--udev|--uninstall|--check]"
    echo
    echo "  (none)       install prerequisites, build, then install to ~/.IOMeeter"
    echo "  --udev       install the USB permission rule (needs root)"
    echo "  --uninstall  remove the installed copy, keeping config.json"
    echo "  --check      report whether the USB rule is working"
}

for arg in "$@"; do
    case "$arg" in
        --udev)      MODE=udev ;;
        --uninstall) MODE=uninstall ;;
        --check)     MODE=check ;;
        -h|--help)   usage; exit 0 ;;
        *)
            echo "Unknown option: $arg" >&2
            echo >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [ "$MODE" = install ] && [ "$(id -u)" -eq 0 ]; then
    echo "Do not run the install with sudo: HOME is /root there, so it would" >&2
    echo "all be installed for the root account instead of you." >&2
    echo >&2
    echo "Run it as yourself:" >&2
    echo "    ./install.sh" >&2
    echo >&2
    echo "Only --udev needs root." >&2
    exit 1
fi

# Print the device node's permissions, so a rule that did not take effect is
# visible immediately instead of surfacing later as LIBUSB_ERROR_ACCESS.
show_device_access() {
    command -v lsusb >/dev/null 2>&1 || return 0

    local line bus dev node
    line=$(lsusb -d 303a:4001 2>/dev/null | head -1)
    [ -n "$line" ] || line=$(lsusb -d 303a:4002 2>/dev/null | head -1)
    if [ -z "$line" ]; then
        echo
        echo "Device not plugged in, so nothing to check yet."
        return 0
    fi

    bus=$(printf '%s' "$line" | awk '{print $2}')
    dev=$(printf '%s' "$line" | awk '{sub(/:/, "", $4); print $4}')
    node="/dev/bus/usb/$bus/$dev"
    [ -e "$node" ] || return 0

    echo
    echo "Device node $node:"
    ls -l "$node"
    if command -v getfacl >/dev/null 2>&1; then
        if getfacl -p "$node" 2>/dev/null | grep -qE '^user:[^:]+:'; then
            echo "An ACL for your user is present, so the rule worked."
        else
            echo "No user ACL yet -- replug the device, then check again."
        fi
    fi
}

refresh_caches() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$DESKTOP_DIR" 2>/dev/null
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t "$THEME_DIR" 2>/dev/null
    fi
    return 0
}

# Written line by line rather than from a file, so the rule cannot go missing
# from a checkout and drift out of step with this script.
write_udev_rule() {
    {
        echo '# IOMeeter: hand the ESP32-S3 vendor interface to whoever is logged in'
        echo '# at the desktop, so libusb can open it without root.'
        echo '#'
        echo '# Running IOMeeter under sudo is not an equivalent workaround: it opens'
        echo '# the device but cuts the process off from the session sound server, so'
        echo '# the audio mixer stops working.'
        echo '#'
        echo '# uaccess gives an ACL to the user holding the active seat; plugdev is'
        echo '# the fallback for anything without a seat, such as an ssh session.'
        echo '#'
        echo '# 4001 is the controller, 4002 the dongle; either may be attached.'
        echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4001", MODE="0660", GROUP="plugdev", TAG+="uaccess"'
        echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4002", MODE="0660", GROUP="plugdev", TAG+="uaccess"'
    } > "$UDEV_RULE"
}

case "$MODE" in
udev)
    if [ "$(id -u)" -ne 0 ]; then
        echo "Installing the udev rule needs root:" >&2
        echo "    sudo ./install.sh --udev" >&2
        exit 1
    fi

    write_udev_rule || exit 1
    chmod 644 "$UDEV_RULE"

    echo "$UDEV_RULES_STALE" | while read -r stale; do
        [ -n "$stale" ] && rm -f "$stale"
    done
    udevadm control --reload-rules && udevadm trigger

    echo "Installed $UDEV_RULE"
    echo "Unplug and replug the device for it to take effect."
    show_device_access
    exit 0
    ;;

check)
    if [ -f "$UDEV_RULE" ]; then
        echo "Rule installed: $UDEV_RULE"
    else
        echo "Rule MISSING: $UDEV_RULE"
        echo "Install it with: sudo $0 --udev"
    fi

    echo "$UDEV_RULES_STALE" | while read -r stale; do
        if [ -n "$stale" ] && [ -f "$stale" ]; then
            echo "Stale rule present: $stale (sorts too late to work)"
        fi
    done

    if [ -x "$BIN" ]; then
        echo "Installed binary: $BIN"
    else
        echo "Installed binary: $BIN (MISSING)"
    fi
    echo "Your groups: $(id -nG)"
    show_device_access
    exit 0
    ;;

uninstall)
    rm -f "$BIN" "$DESKTOP_FILE" "$ICON_DIR/iomeeter.png"
    rm -rf "$TARGET/resources"
    refresh_caches

    echo "Removed IOMeeter from $TARGET"
    if [ -f "$TARGET/config.json" ]; then
        echo "Kept your configuration at $TARGET/config.json"
    fi
    exit 0
    ;;
esac

echo "=== 1/3  Prerequisites ==="

# apt aborts the whole transaction over one unknown name, so anything the
# distribution does not carry is dropped before the call rather than taking
# the packages that do exist down with it.
apt_knows() {
    apt-cache show "$1" >/dev/null 2>&1
}

installed() {
    dpkg -s "$1" >/dev/null 2>&1
}

wanted=""
absent=""

for pkg in $APT_REQUIRED; do
    installed "$pkg" && continue
    if apt_knows "$pkg"; then
        wanted="$wanted $pkg"
    else
        echo "Required package not available on this distribution: $pkg" >&2
        exit 1
    fi
done

# raylib often comes from a source build or a PPA instead, in which case there
# is nothing to install and nothing to warn about.
raylib_present() {
    pkg-config --exists raylib 2>/dev/null && return 0
    [ -f /usr/local/lib/libraylib.a ] && return 0
    [ -f /usr/local/lib/libraylib.so ] && return 0
    ls /usr/local/lib/cmake/raylib >/dev/null 2>&1 && return 0
    ls /usr/lib/*/cmake/raylib >/dev/null 2>&1 && return 0
    return 1
}

for pkg in $APT_OPTIONAL; do
    installed "$pkg" && continue
    if [ "$pkg" = libraylib-dev ] && raylib_present; then
        continue
    fi
    if apt_knows "$pkg"; then
        wanted="$wanted $pkg"
    else
        absent="$absent $pkg"
    fi
done

if [ -n "$wanted" ]; then
    echo "Installing:$wanted"
    sudo apt install -y $wanted || exit 1
else
    echo "Already present."
fi

for pkg in $absent; do
    echo
    echo "WARNING: $pkg is not in this distribution's repositories."
    case "$pkg" in
        libraylib-dev)
            echo "raylib is needed to build. Get it with one of:"
            echo "    sudo apt install libraylib-dev      (Debian 12+, Ubuntu 23.04+)"
            echo "    or build it from https://github.com/raysan5/raylib"
            ;;
        libayatana-appindicator3-dev)
            echo "There will be no tray icon; everything else still works."
            echo "Try: sudo apt install libappindicator3-dev"
            ;;
    esac
done

echo
echo "=== 2/3  Build ==="

# A build directory left behind by a sudo run belongs to root, and cmake then
# cannot write its cache as you. It is about to be wiped anyway.
if [ -d build ] && [ ! -w build ]; then
    echo "build/ is not writable -- it was created by a run under sudo."
    echo "Removing it so the build can proceed:"
    sudo rm -rf build || exit 1
fi

./build.sh clean || exit 1
./build.sh build || exit 1

if [ ! -x build/IOMeeter ]; then
    echo "Build did not produce build/IOMeeter." >&2
    exit 1
fi

echo
echo "=== 3/3  Install into $TARGET ==="
mkdir -p "$TARGET/resources" "$DESKTOP_DIR" "$ICON_DIR" || exit 1

install -m 755 build/IOMeeter                  "$BIN"                   || exit 1
install -m 644 resources/Roboto-Regular.ttf    "$TARGET/resources/"     || exit 1
install -m 644 resources/RobotoMono-Medium.ttf "$TARGET/resources/"     || exit 1
install -m 644 resources/icon.png              "$TARGET/resources/"     || exit 1
install -m 644 resources/icon.png              "$ICON_DIR/iomeeter.png" || exit 1

# The configuration is the user's; a reinstall must not discard it.
if [ ! -f "$TARGET/config.json" ] && [ -f config.json ]; then
    install -m 644 config.json "$TARGET/config.json"
fi

# Path= makes ~/.IOMeeter the working directory, which is where config.json is
# read and written, and beside which respath finds resources/.
sed -e "s|@BIN@|$BIN|" -e "s|@DATA@|$TARGET|" \
    resources/IOMeeter.desktop > "$DESKTOP_FILE" || exit 1
chmod 644 "$DESKTOP_FILE"

# An ELF binary carries no icon of its own, so the file manager is told which
# one to use through GVFS metadata.
if command -v gio >/dev/null 2>&1; then
    gio set "$BIN" metadata::custom-icon "file://$TARGET/resources/icon.png" 2>/dev/null || true
fi

refresh_caches

echo
echo "Installed:"
echo "  $BIN"
echo "  $TARGET/resources/     fonts and icon"
echo "  $TARGET/config.json    settings, including the debug flag"
echo "  $DESKTOP_FILE"

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$DESKTOP_FILE" && echo "Launcher validates."
fi

if [ ! -f "$UDEV_RULE" ]; then
    echo
    echo "The USB permission rule is not installed, so IOMeeter cannot open the"
    echo "device. Install it with:"
    echo "    sudo ./install.sh --udev"
fi

echo
echo "IOMeeter should now be in the menu under Sound & Video."

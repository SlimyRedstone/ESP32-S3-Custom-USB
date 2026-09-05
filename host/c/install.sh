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

APT_PACKAGES="build-essential cmake pkg-config libusb-1.0-0-dev libpulse-dev
libayatana-appindicator3-dev libraylib-dev"

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
        echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4001", MODE="0660", GROUP="plugdev", TAG+="uaccess"'
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
missing=""
for pkg in $APT_PACKAGES; do
    dpkg -s "$pkg" >/dev/null 2>&1 || missing="$missing $pkg"
done

if [ -n "$missing" ]; then
    echo "Installing:$missing"
    sudo apt install -y $missing || exit 1
else
    echo "Already present."
fi

echo
echo "=== 2/3  Build ==="
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

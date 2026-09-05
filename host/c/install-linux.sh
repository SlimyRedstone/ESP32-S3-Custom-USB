#!/usr/bin/env bash
#
# Desktop integration, for Cinnamon and anything else following the
# freedesktop specifications.
#
# Installs the binary, the fonts and icon it loads, an icon in the hicolor
# theme, and a launcher. IOMeeter then shows up in the application menu, can be
# pinned to the panel, and the window carries its own icon in the window list
# instead of a placeholder.
#
#   ./install-linux.sh              install for this user, under ~/.local
#   ./install-linux.sh --system     install for everyone, under /usr/local
#   ./install-linux.sh --uninstall  remove whichever is present
#
# Separately, the USB device is owned by root until a udev rule hands it to
# the desktop user. Without that, IOMeeter cannot open the device; with sudo
# instead, it opens the device but loses the audio mixer. Install the rule:
#
#   sudo ./install-linux.sh --udev
#
# Run ./build.sh first: this installs what that produced.
#
set -uo pipefail

cd "$(dirname "$0")" || exit 1

PREFIX="$HOME/.local"
MODE=install
SYSTEM=0

usage() {
    echo "Usage: ./install-linux.sh [--system|--uninstall|--udev|--check]"
    echo
    echo "  (none)       install for this user, under ~/.local"
    echo "  --system     install for everyone, under /usr/local (needs root)"
    echo "  --uninstall  remove whichever installation is present"
    echo "  --udev       install the USB permission rule (needs root)"
    echo "  --check      report whether the USB rule is working"
}

for arg in "$@"; do
    case "$arg" in
        --system)    PREFIX=/usr/local; SYSTEM=1 ;;
        --uninstall) MODE=uninstall ;;
        --udev)      MODE=udev ;;
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

if [ "$SYSTEM" -eq 0 ] && [ "$MODE" = install ] && [ "$(id -u)" -eq 0 ]; then
    echo "Do not run a user install with sudo: \$HOME is /root there, so" >&2
    echo "everything would be installed for the root account instead of you." >&2
    echo >&2
    echo "Run it as yourself:" >&2
    echo "    ./install-linux.sh" >&2
    echo >&2
    echo "Only --udev and --system need root." >&2
    if [ -n "${SUDO_USER-}" ] && [ -e "/root/.local/bin/IOMeeter" ]; then
        echo >&2
        echo "An earlier run did install into /root. Undo it with:" >&2
        echo "    sudo $0 --uninstall" >&2
    fi
    exit 1
fi

BIN_DIR="$PREFIX/bin"
DATA_DIR="$PREFIX/share/IOMeeter"
THEME_DIR="$PREFIX/share/icons/hicolor"
ICON_DIR="$THEME_DIR/128x128/apps"
DESKTOP_DIR="$PREFIX/share/applications"
DESKTOP_FILE="$DESKTOP_DIR/IOMeeter.desktop"
# Must sort before systemd's 73-seat-late.rules, which is what turns the
# uaccess tag into an ACL. A rule numbered above that sets the tag too late
# for anything to read it.
UDEV_RULE=/etc/udev/rules.d/60-iomeeter.rules
# Rules earlier versions of this project told people to write by hand. Both
# sort after 73-seat-late.rules, so their uaccess tag never did anything.
UDEV_RULES_STALE="/etc/udev/rules.d/99-iomeeter.rules
/etc/udev/rules.d/99-esp32s3-vendor.rules"

# The menu and the icon theme are both cached; without this the entry can take
# until the next login to appear.
refresh() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$DESKTOP_DIR" 2>/dev/null
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t "$THEME_DIR" 2>/dev/null
    fi
    return 0
}

need_root() {
    [ "$PREFIX" = /usr/local ] && [ "$(id -u)" -ne 0 ]
}

# Print the device node's permissions, so a rule that did not take effect is
# visible immediately instead of surfacing later as LIBUSB_ERROR_ACCESS.
show_device_access() {
    command -v lsusb >/dev/null 2>&1 || return 0

    local line bus dev node
    line=$(lsusb -d 303a:4001 2>/dev/null | head -1)
    [ -n "$line" ] || { echo; echo "Device not plugged in, so nothing to check yet."; return 0; }

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

if [ "$MODE" = check ]; then
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

    echo "Your groups: $(id -nG)"
    show_device_access
    exit 0
fi

if [ "$MODE" = udev ]; then
    if [ "$(id -u)" -ne 0 ]; then
        echo "Installing the udev rule needs root:" >&2
        echo "    sudo ./install-linux.sh --udev" >&2
        exit 1
    fi

    cat > "$UDEV_RULE" <<'RULE' || exit 1
# IOMeeter: hand the ESP32-S3 vendor interface to whoever is logged in at
# the desktop, so libusb can open it without root.
#
# Running IOMeeter under sudo is not an equivalent workaround: it opens the
# device but cuts the process off from the session sound server, so the
# audio mixer stops working.
#
# uaccess gives an ACL to the user holding the active seat; the plugdev group
# is the fallback for anything without a seat, such as an ssh session.
SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="4001", MODE="0660", GROUP="plugdev", TAG+="uaccess"
RULE
    chmod 644 "$UDEV_RULE"
    echo "$UDEV_RULES_STALE" | while read -r stale; do
        [ -n "$stale" ] && rm -f "$stale"
    done
    udevadm control --reload-rules && udevadm trigger

    echo "Installed $UDEV_RULE"
    echo "Unplug and replug the device for it to take effect."
    echo "IOMeeter can then be run WITHOUT sudo, which is what the mixer needs."
    show_device_access
    exit 0
fi

if [ "$MODE" = uninstall ]; then
    if need_root; then
        echo "Removing a --system install needs root. Re-run with sudo." >&2
        exit 1
    fi

    rm -f "$BIN_DIR/IOMeeter" \
          "$DESKTOP_FILE" \
          "$ICON_DIR/iomeeter.png" \
          "$DATA_DIR/icon.png" \
          "$DATA_DIR/Roboto-Regular.ttf" \
          "$DATA_DIR/RobotoMono-Medium.ttf"

    # config.json lives here too and is the user's, so the directory is only
    # removed when nothing is left in it.
    rmdir "$DATA_DIR" 2>/dev/null

    refresh
    echo "Removed IOMeeter from $PREFIX."
    if [ -f "$DATA_DIR/config.json" ]; then
        echo "Kept your configuration at $DATA_DIR/config.json"
    fi
    exit 0
fi

if [ ! -f build/main ]; then
    echo "build/main not found. Run ./build.sh first." >&2
    exit 1
fi

if need_root; then
    echo "--system needs root. Re-run with sudo." >&2
    exit 1
fi

mkdir -p "$BIN_DIR" "$DATA_DIR" "$ICON_DIR" "$DESKTOP_DIR" || exit 1

install -m 755 build/main                     "$BIN_DIR/IOMeeter"      || exit 1
install -m 644 resources/Roboto-Regular.ttf   "$DATA_DIR/"             || exit 1
install -m 644 resources/RobotoMono-Medium.ttf "$DATA_DIR/"            || exit 1
install -m 644 resources/icon.png             "$DATA_DIR/"             || exit 1
install -m 644 resources/icon.png             "$ICON_DIR/iomeeter.png" || exit 1

# The launcher gets its icon from the theme, but the installed binary is just a
# file, so it is tagged the same way build.sh tags the one in build/.
if command -v gio >/dev/null 2>&1; then
    gio set "$BIN_DIR/IOMeeter" metadata::custom-icon "file://$DATA_DIR/icon.png" 2>/dev/null || true
fi

# Carry an existing configuration over on the first install only, so a later
# reinstall never overwrites what the user has since changed.
if [ ! -f "$DATA_DIR/config.json" ] && [ -f config.json ]; then
    install -m 644 config.json "$DATA_DIR/config.json"
fi

# Path= makes the data directory the working directory, which is where
# config.json is then read and written.
sed -e "s|@BIN@|$BIN_DIR/IOMeeter|" \
    -e "s|@DATA@|$DATA_DIR|" \
    resources/IOMeeter.desktop > "$DESKTOP_FILE" || exit 1
chmod 644 "$DESKTOP_FILE"

refresh

echo "Installed:"
echo "  $BIN_DIR/IOMeeter"
echo "  $DATA_DIR/            fonts, icon, config.json"
echo "  $DESKTOP_FILE"
echo

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$DESKTOP_FILE" && echo "Launcher validates."
fi

case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) echo "Note: $BIN_DIR is not on PATH, so \"IOMeeter\" will not run from a shell." ;;
esac

if [ ! -f "$UDEV_RULE" ]; then
    echo
    echo "The USB permission rule is not installed, so IOMeeter will not be"
    echo "able to open the device. Install it with:"
    echo "    sudo ./install-linux.sh --udev"
fi

echo
echo "IOMeeter should now be in the menu under Sound & Video."

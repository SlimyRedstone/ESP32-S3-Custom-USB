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

usage() {
    echo "Usage: ./install-linux.sh [--system|--uninstall]"
    echo
    echo "  (none)       install for this user, under ~/.local"
    echo "  --system     install for everyone, under /usr/local (needs root)"
    echo "  --uninstall  remove whichever installation is present"
    echo "  --udev       install the USB permission rule (needs root)"
}

for arg in "$@"; do
    case "$arg" in
        --system)    PREFIX=/usr/local ;;
        --uninstall) MODE=uninstall ;;
        --udev)      MODE=udev ;;
        -h|--help)   usage; exit 0 ;;
        *)
            echo "Unknown option: $arg" >&2
            echo >&2
            usage >&2
            exit 1
            ;;
    esac
done

BIN_DIR="$PREFIX/bin"
DATA_DIR="$PREFIX/share/IOMeeter"
THEME_DIR="$PREFIX/share/icons/hicolor"
ICON_DIR="$THEME_DIR/128x128/apps"
DESKTOP_DIR="$PREFIX/share/applications"
DESKTOP_FILE="$DESKTOP_DIR/IOMeeter.desktop"
UDEV_RULE=/etc/udev/rules.d/99-iomeeter.rules

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

if [ "$MODE" = udev ]; then
    if [ "$(id -u)" -ne 0 ]; then
        echo "Installing the udev rule needs root:" >&2
        echo "    sudo ./install-linux.sh --udev" >&2
        exit 1
    fi

    install -m 644 resources/99-iomeeter.rules "$UDEV_RULE" || exit 1
    udevadm control --reload-rules && udevadm trigger

    echo "Installed $UDEV_RULE"
    echo "Unplug and replug the device for it to take effect."
    echo "IOMeeter can then be run WITHOUT sudo, which is what the mixer needs."
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

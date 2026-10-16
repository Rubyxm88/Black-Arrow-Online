#!/bin/bash
# Sets up a Black Arrow Online Mac test build.
#
#   bash setup-test.sh <content pack zip>     install or update
#   bash setup-test.sh --start-over           new console (the old one is lost)
#
# Run it with "bash" from Terminal. The app goes to ~/Applications, the console
# and its files to ~/Library/Application Support/Black Arrow Online, laid out
# like the Windows package folder (system/ and games/).
#
# macOS 10.x ships bash 3.2, so this keeps to what it supports.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
data="$HOME/Library/Application Support/Black Arrow Online"
system="$data/system"
games="$data/games"
app="$HOME/Applications/Black Arrow Online Test.app"
NL=$'\n'

fail() {
    echo
    echo "Setup stopped: $*" >&2
    exit 1
}

default_settings() {
    cat <<'EOF'
[general]
show_welcome = false
skip_boot_anim = true
games_dir = '../games'

[general.updates]
check = false

[display.quality]
surface_scale = 2
skip_reuse_copyback = true

[net]
enable = true
dns_redirect = 'xb_online'

[net.nat]
forward_ports = [
    { host = 3074, guest = 3074, protocol = 'udp'}
    ]

[sys]
mem_limit = '128'

[sys.files]
bootrom_path = 'mcpx.bin'
flashrom_path = 'Complex_4627v1.03.bin'
eeprom_path = 'eeprom.bin'
hdd_path = 'xbox_hdd.qcow2'
EOF
}

# A newly set up console: clean hard disk, new EEPROM (made on first start),
# and the first start opens the XBL Dashboard again
new_console() {
    [ -f "$system/fresh/xbox_hdd.qcow2" ] ||
        fail "the content pack is not installed yet. Run: bash setup-test.sh <content pack zip>"
    echo "Setting up a new console..."
    cp "$system/fresh/xbox_hdd.qcow2" "$system/xbox_hdd.qcow2"
    rm -f "$system/eeprom.bin"
    if [ -f "$system/xemu.toml" ]; then
        grep -vE '^(xb_online_setup_offered|dvd_path) = ' "$system/xemu.toml" \
            > "$system/xemu.toml.new" || true
        mv "$system/xemu.toml.new" "$system/xemu.toml"
    fi
}

if [ "${1:-}" = "--start-over" ]; then
    pgrep -qf "Black Arrow Online Test.app" && fail "quit Black Arrow Online first."
    new_console
    echo "Done. Start Black Arrow Online Test from Applications in your home folder."
    exit 0
fi

pack="${1:-}"
[ -n "$pack" ] || fail "give the content pack zip, e.g.
  bash setup-test.sh ~/Downloads/BlackArrowOnline-content.zip"
[ -f "$pack" ] || fail "$pack was not found."
[ -d "$here/xemu.app" ] || fail "xemu.app is not next to this script. Unzip the whole test build first."
entries="$(unzip -Z1 "$pack" 2>/dev/null)" ||
    fail "$pack is not a zip, or it did not download completely."
case "$NL$entries$NL" in
    *"${NL}system/content.txt${NL}"*) ;;
    *) fail "$pack is not a Black Arrow Online content pack." ;;
esac
pgrep -qf "Black Arrow Online Test.app" && fail "quit Black Arrow Online first."

echo "Installing the app..."
mkdir -p "$HOME/Applications"
rm -rf "$app"
ditto "$here/xemu.app" "$app"
# Downloaded by a browser, so macOS would block it; it is a test build
xattr -dr com.apple.quarantine "$app" 2>/dev/null || true

echo "Installing the content pack (this takes a minute)..."
mkdir -p "$system" "$games"
# The console's own files never come from a pack
unzip -q -o "$pack" 'system/*' -x 'system/xemu.toml' 'system/eeprom.bin' \
    'system/xbox_hdd.qcow2' -d "$data" \
    2> >(grep -v 'excluded filename not matched' >&2)
if [ -f "$here/shader-list.bin" ]; then
    cp "$here/shader-list.bin" "$system/shader-list.bin"
fi
cp "$here/test-build.txt" "$system/test-build.txt" 2>/dev/null || true

if [ ! -f "$system/xemu.toml" ]; then
    default_settings > "$system/xemu.toml"
fi
if [ ! -f "$system/xbox_hdd.qcow2" ]; then
    new_console
fi

echo
echo "Done."
echo "Put your Rainbow Six 3: Black Arrow disc image (.iso) in the games folder,"
echo "which opens now, then start Black Arrow Online Test from Applications in"
echo "your home folder."
open "$games"

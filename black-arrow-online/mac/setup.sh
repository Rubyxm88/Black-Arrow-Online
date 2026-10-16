#!/bin/bash
# Installs or updates Black Arrow Online on a Mac.
#
#   bash setup.sh                   install or update from this folder
#   bash setup.sh <content zip>     also install a content pack zip
#   bash setup.sh --start-over      new console (the old one is lost)
#
# Run it with "bash" from Terminal: macOS blocks apps and scripts downloaded
# in a browser until they are approved in System Settings, and this clears
# that for the app it installs.
#
# The app goes to Applications. The console and its files go to
# ~/Library/Application Support/Black Arrow Online, laid out like the Windows
# package folder (system/ and games/). They are kept on every update.
#
# macOS 10.x ships bash 3.2, so this keeps to what it supports.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
data="$HOME/Library/Application Support/Black Arrow Online"
system="$data/system"
games="$data/games"
name="Black Arrow Online.app"
NL=$'\n'

fail() {
    echo
    echo "Setup stopped: $*" >&2
    exit 1
}

running() {
    pgrep -qf "$name/Contents/MacOS/" 2>/dev/null
}

# The console's own files, never replaced from a package or content pack
is_kept() {
    case "$1" in
        xemu.toml|eeprom.bin|xbox_hdd.qcow2) return 0 ;;
    esac
    return 1
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

# "Black Arrow Report" on the Desktop: double-clicked, it puts what is needed
# to look into a problem in one zip on the Desktop. While the app is running
# (or hanging) it also records what each part of the app is doing.
report_tool() {
    cat <<'REPORT'
#!/bin/bash
# Black Arrow Online report. Double-click it, best while the problem is
# happening (with the game still open): it makes a zip on the Desktop to send
# to whoever shared the package.
data="$HOME/Library/Application Support/Black Arrow Online"
name="Black Arrow Online.app"
stamp="$(date +%Y-%m-%d_%H%M%S)"
out="$HOME/Desktop/BlackArrow-report-$stamp"
mkdir -p "$out" || exit 1
echo "Making a Black Arrow Online report. This takes about 15 seconds..."

pid="$(pgrep -f "$name/Contents/MacOS/" | head -n 1)"
if [ -n "$pid" ]; then
    echo "Black Arrow Online is open: recording what it is doing (10 seconds)..."
    /usr/bin/sample "$pid" 10 -file "$out/sample.txt" >/dev/null 2>&1 ||
        echo "Could not record it." > "$out/sample.txt"
else
    echo "Black Arrow Online was not open." > "$out/not-open.txt"
fi

{
    sw_vers
    echo
    echo "Model: $(sysctl -n hw.model)"
    echo "CPU: $(sysctl -n machdep.cpu.brand_string)"
    echo "Memory: $(( $(sysctl -n hw.memsize) / 1073741824 )) GB"
    echo
    system_profiler SPDisplaysDataType
} > "$out/mac.txt" 2>&1

for file in xemu.log version.txt xemu.toml; do
    if [ -f "$data/system/$file" ]; then
        cp "$data/system/$file" "$out/"
    fi
done
if [ -d "$data/system/logs" ]; then
    mkdir -p "$out/logs"
    ls -t "$data/system/logs" | head -n 10 | while IFS= read -r file; do
        cp "$data/system/logs/$file" "$out/logs/" 2>/dev/null
    done
fi
# macOS's own crash and hang reports for the app from the last week
mkdir -p "$out/crash-reports"
find "$HOME/Library/Logs/DiagnosticReports" -maxdepth 1 -type f \
    \( -iname 'xemu*' -o -iname 'Black Arrow*' \) -mtime -7 \
    -exec cp {} "$out/crash-reports/" \; 2>/dev/null
rmdir "$out/crash-reports" 2>/dev/null

ditto -c -k --keepParent "$out" "$out.zip" && rm -rf "$out"
echo
echo "Done: BlackArrow-report-$stamp.zip is on your Desktop."
echo "Send it to whoever shared the package. You can close this window."
open -R "$out.zip"
REPORT
}

install_report_tool() {
    local tool="$HOME/Desktop/Black Arrow Report.command"
    if report_tool > "$tool" 2>/dev/null; then
        chmod 755 "$tool"
        xattr -d com.apple.quarantine "$tool" 2>/dev/null || true
        echo "Put \"Black Arrow Report\" on your Desktop."
    else
        rm -f "$tool"
        echo "Could not put \"Black Arrow Report\" on your Desktop (skipped)."
    fi
}

# A newly set up console: clean hard disk, new EEPROM (made on first start),
# and the first start opens the XBL Dashboard again
new_console() {
    [ -f "$system/fresh/xbox_hdd.qcow2" ] ||
        fail "the Xbox files are not installed yet. Run setup.sh from the full Mac package."
    echo "Setting up a new console..."
    cp "$system/fresh/xbox_hdd.qcow2" "$system/xbox_hdd.qcow2"
    rm -f "$system/eeprom.bin"
    if [ -f "$system/xemu.toml" ]; then
        grep -vE '^(xb_online_setup_offered|dvd_path) = ' "$system/xemu.toml" \
            > "$system/xemu.toml.new" || true
        mv "$system/xemu.toml.new" "$system/xemu.toml"
    fi
}

# Xbox files from the full package's system folder
install_folder_content() {
    echo "Installing the Xbox files (this takes a minute)..."
    (cd "$here/system" && find . -type f) | while IFS= read -r file; do
        file="${file#./}"
        if is_kept "$file"; then
            continue
        fi
        mkdir -p "$(dirname "$system/$file")"
        cp "$here/system/$file" "$system/$file"
    done
}

# Xbox files from a content pack zip
install_zip_content() {
    local pack="$1" entries
    [ -f "$pack" ] || fail "$pack was not found."
    entries="$(unzip -Z1 "$pack" 2>/dev/null)" ||
        fail "$pack is not a zip, or it did not download completely."
    case "$NL$entries$NL" in
        *"${NL}system/content.txt${NL}"*) ;;
        *) fail "$pack is not a Black Arrow Online content pack." ;;
    esac
    echo "Installing the content pack (this takes a minute)..."
    unzip -q -o "$pack" 'system/*' -x 'system/xemu.toml' 'system/eeprom.bin' \
        'system/xbox_hdd.qcow2' -d "$data" \
        2> >(grep -v 'excluded filename not matched' >&2)
}

if [ "${1:-}" = "--start-over" ]; then
    running && fail "quit Black Arrow Online first."
    new_console
    echo "Done. Start Black Arrow Online from Applications."
    exit 0
fi

[ -d "$here/$name" ] ||
    fail "\"$name\" is not next to setup.sh. Unzip the whole download first."
running && fail "quit Black Arrow Online first."

mkdir -p "$system" "$games"
if [ -n "${1:-}" ]; then
    install_zip_content "$1"
elif [ -f "$here/system/content.txt" ]; then
    install_folder_content
elif [ ! -f "$system/content.txt" ]; then
    fail "this download is only the app, and the Xbox files are not installed yet.
Get the full Mac package from where Black Arrow Online is shared, unzip it and
run its setup.sh instead."
fi

# Applications when this account may write there (admin accounts can),
# otherwise the Applications folder in the home folder
if [ -w /Applications ]; then
    app="/Applications/$name"
    other="$HOME/Applications/$name"
else
    app="$HOME/Applications/$name"
    other="/Applications/$name"
fi
echo "Installing the app in $(dirname "$app")..."
mkdir -p "$(dirname "$app")"
rm -rf "$app"
ditto "$here/$name" "$app"
# Downloaded in a browser, so macOS would block it until approved
xattr -dr com.apple.quarantine "$app" 2>/dev/null || true
# An older copy in the other Applications folder would only confuse
if [ -d "$other" ] && [ -w "$(dirname "$other")" ]; then
    rm -rf "$other"
fi

for file in shader-list.bin version.txt; do
    if [ -f "$here/$file" ]; then
        cp "$here/$file" "$system/$file"
    fi
done
if [ ! -f "$system/xemu.toml" ]; then
    default_settings > "$system/xemu.toml"
fi
first_time=false
if [ ! -f "$system/xbox_hdd.qcow2" ]; then
    new_console
    first_time=true
fi
install_report_tool

echo
echo "Done."
echo "If something goes wrong, double-click \"Black Arrow Report\" on your"
echo "Desktop (with the game still open if it is stuck) and send the zip it"
echo "makes there."
if $first_time; then
    echo "Put your Rainbow Six 3: Black Arrow disc image (.iso) in the games"
    echo "folder, which opens now, then start Black Arrow Online from Applications."
    open "$games"
else
    echo "Your console and games are as you left them. Start Black Arrow Online"
    echo "from Applications."
fi

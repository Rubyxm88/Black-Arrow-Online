#!/bin/bash
# Installs or updates Black Arrow Online on a Mac.
#
#   bash setup.sh                   install or update from this folder
#   bash setup.sh <content zip>     also install a content pack zip
#   bash setup.sh --start-over      new console (the old one is lost)
#   bash setup.sh --update <pid>    what the app's Update now runs: waits
#                                   for the app to close, updates it from
#                                   this folder and opens it again
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

# Applications when this account may write there (admin accounts can),
# otherwise the Applications folder in the home folder
if [ -w /Applications ]; then
    app="/Applications/$name"
    other="$HOME/Applications/$name"
else
    app="$HOME/Applications/$name"
    other="/Applications/$name"
fi

# The app's process when the app started this to update itself
update_pid=""
if [ "${1:-}" = "--update" ]; then
    update_pid="${2:-}"
    [ -n "$update_pid" ] || { echo "--update needs the app's process id" >&2; exit 1; }
    set --
fi

fail() {
    echo
    echo "Setup stopped: $*" >&2
    if [ -n "$update_pid" ]; then
        # The app shows it on its next start; open the app that is there
        trap - ERR
        printf '%s\n' "$*" > "$system/update-failed.txt" 2>/dev/null || true
        if [ -d "$app" ]; then
            open "$app" || true
        fi
    fi
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

# "Black Arrow Report" next to the app in Applications: double-clicked, it
# puts what is needed to look into a problem in one zip in Downloads. While
# the app is running (or hanging) it also records what each part of the app
# is doing. Downloads, not the Desktop: Terminal already may use Downloads
# (setup ran from there), so macOS does not ask again.
report_tool() {
    cat <<'REPORT'
#!/bin/bash
# Black Arrow Online report. Double-click it, best while the problem is
# happening (with the game still open): it makes a zip in Downloads to send
# to whoever shared the package.
data="$HOME/Library/Application Support/Black Arrow Online"
name="Black Arrow Online.app"
stamp="$(date +%Y-%m-%d_%H%M%S)"
out="$HOME/Downloads/BlackArrow-report-$stamp"
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

for file in xemu.log version.txt xemu.toml update.log; do
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
echo "Done: BlackArrow-report-$stamp.zip is in your Downloads folder."
echo "Send it to whoever shared the package. You can close this window."
open -R "$out.zip"
REPORT
}

install_report_tool() {
    local tool="$1/Black Arrow Report.command"
    if report_tool > "$tool" 2>/dev/null; then
        chmod 755 "$tool"
        xattr -d com.apple.quarantine "$tool" 2>/dev/null || true
    else
        rm -f "$tool"
        echo "Could not add \"Black Arrow Report\" to $1 (skipped)."
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

if [ -n "$update_pid" ]; then
    # No Terminal window: what happens goes to update.log
    mkdir -p "$system"
    if [ -f "$system/update.log" ]; then
        tail -n 300 "$system/update.log" > "$system/update.log.tmp" &&
            mv "$system/update.log.tmp" "$system/update.log"
    fi
    exec >> "$system/update.log" 2>&1
    echo "=== $(date): updating from $here"
    trap 'fail "step at line $LINENO of setup.sh failed (see update.log)."' ERR
    # A copy stuck in the game (builds up to 21 on Apple Silicon) never
    # closes: after a minute ask it to quit, then make it
    waited=0
    while kill -0 "$update_pid" 2>/dev/null; do
        if [ "$waited" -eq 120 ]; then
            echo "Black Arrow Online did not close in a minute, quitting it."
            kill -TERM "$update_pid" 2>/dev/null || true
        elif [ "$waited" -eq 140 ]; then
            echo "Still open, closing it."
            kill -KILL "$update_pid" 2>/dev/null || true
        elif [ "$waited" -ge 160 ]; then
            fail "Black Arrow Online did not close."
        fi
        sleep 0.5
        waited=$((waited + 1))
    done
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

echo "Installing the app in $(dirname "$app")..."
mkdir -p "$(dirname "$app")"
# Copied next to the old app first, then swapped, so a failed copy leaves the
# old app working
rm -rf "$app.new" "$app.old"
ditto "$here/$name" "$app.new" || fail "could not copy the app to $(dirname "$app")."
# Downloaded in a browser, so macOS would block it until approved
xattr -dr com.apple.quarantine "$app.new" 2>/dev/null || true
if [ -d "$app" ]; then
    mv "$app" "$app.old" || fail "could not replace the app in $(dirname "$app")."
fi
if ! mv "$app.new" "$app"; then
    mv "$app.old" "$app" 2>/dev/null || true
    fail "could not replace the app in $(dirname "$app")."
fi
rm -rf "$app.old"
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
install_report_tool "$(dirname "$app")"
# Only in the Applications folder the app is in, like the app
other_tool="$(dirname "$other")/Black Arrow Report.command"
if [ -f "$other_tool" ] && [ -w "$(dirname "$other")" ]; then
    rm -f "$other_tool"
fi

if [ -n "$update_pid" ]; then
    # The app says "Updated to build N" once, then this folder (unpacked by
    # the app in a temporary folder) goes
    build="$({ grep -m 1 '^build=' "$here/version.txt" 2>/dev/null || true; } | cut -d= -f2-)"
    printf '%s\n' "${build:-0}" > "$system/update-done.txt"
    echo "Done: build ${build:-?} installed. Opening it."
    trap - ERR
    open "$app" || echo "Could not open $app"
    case "$(dirname "$here")" in
        */BlackArrowOnline-update-*) rm -rf "$(dirname "$here")" ;;
    esac
    exit 0
fi

echo
echo "Done."
echo "If something goes wrong, open Applications and double-click"
echo "\"Black Arrow Report\" (with the game still open if it is stuck). Send"
echo "the BlackArrow-report zip it puts in Downloads."
if $first_time; then
    echo "Put your Rainbow Six 3: Black Arrow disc image (.iso) in the games"
    echo "folder, which opens now, then start Black Arrow Online from Applications."
    open "$games"
else
    echo "Your console and games are as you left them. Start Black Arrow Online"
    echo "from Applications."
fi

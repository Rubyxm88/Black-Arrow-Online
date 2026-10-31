#!/bin/bash
# Installs or updates Black Arrow Online on Linux.
#
#   bash setup.sh                   install or update from this folder
#   bash setup.sh <content zip>     also install a content pack zip
#   bash setup.sh --start-over      new console (the old one is lost)
#   bash setup.sh --update <pid>    what the app's Update now runs: waits
#                                   for the app to close, updates it from
#                                   this folder and opens it again
#   bash setup.sh --uninstall       removes the app and its menu entries;
#                                   the console and games stay
#
# Everything goes to ~/.local/share/Black Arrow Online (XDG_DATA_HOME when
# set), laid out like the Windows package folder: app/ (the emulator with
# its libraries), system/ (the console) and games/. Nothing needs root.
# Black Arrow Online and Black Arrow Report are added to the applications
# menu.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
data="${XDG_DATA_HOME:-$HOME/.local/share}/Black Arrow Online"
system="$data/system"
games="$data/games"
app="$data/app"
menu="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor"
NL=$'\n'

# The app's process when the app started this to update itself
update_pid=""
if [ "${1:-}" = "--update" ]; then
    update_pid="${2:-}"
    [ -n "$update_pid" ] || { echo "--update needs the app's process id" >&2; exit 1; }
    set --
fi

start_app() {
    setsid "$app/AppRun" </dev/null >/dev/null 2>&1 &
}

fail() {
    echo
    echo "Setup stopped: $*" >&2
    if [ -n "$update_pid" ]; then
        # The app shows it on its next start; open the app that is there
        trap - ERR
        printf '%s\n' "$*" > "$system/update-failed.txt" 2>/dev/null || true
        if [ -e "$app/AppRun" ]; then
            start_app || true
        fi
    fi
    exit 1
}

# Processes of the installed app: started from app/ and running its xemu
# (not just any command line that names the folder)
app_pids() {
    local exe p
    exe="$(readlink -f "$app/usr/bin/xemu" 2>/dev/null)" || return 0
    for p in $(pgrep -f -- "$app/" 2>/dev/null); do
        [ "$(readlink -f "/proc/$p/exe" 2>/dev/null)" = "$exe" ] && echo "$p"
    done
    return 0
}

running() {
    [ -n "$(app_pids)" ]
}

# Still open: a closed app nobody has reaped yet (state Z) counts as closed
alive() {
    kill -0 "$1" 2>/dev/null &&
        ! sed 's/.*) //' "/proc/$1/stat" 2>/dev/null | grep -q '^Z'
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

# A zip's entries, and unpacking one: unzip is not on every system (SteamOS
# has bsdtar, most have python3)
zip_list() {
    if command -v unzip >/dev/null 2>&1; then
        unzip -Z1 "$1"
    elif command -v bsdtar >/dev/null 2>&1; then
        bsdtar -tf "$1"
    else
        python3 -c 'import sys, zipfile; print("\n".join(zipfile.ZipFile(sys.argv[1]).namelist()))' "$1"
    fi
}

# The pack's system/ files into $data, except the console's own files
zip_extract_system() {
    local pack="$1" tmp
    tmp="$(mktemp -d "$data/.pack-XXXXXX")"
    if command -v unzip >/dev/null 2>&1; then
        unzip -q -o "$pack" 'system/*' -d "$tmp"
    elif command -v bsdtar >/dev/null 2>&1; then
        bsdtar -xf "$pack" -C "$tmp" 'system/*'
    else
        python3 -c '
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
z.extractall(sys.argv[2], [n for n in z.namelist() if n.startswith("system/")])
' "$pack" "$tmp"
    fi
    (cd "$tmp/system" && find . -type f) | while IFS= read -r file; do
        file="${file#./}"
        if is_kept "$file"; then
            continue
        fi
        mkdir -p "$(dirname "$system/$file")"
        mv -f "$tmp/system/$file" "$system/$file"
    done
    rm -rf "$tmp"
}

# "Black Arrow Report" in the applications menu: puts what is needed to look
# into a problem in one file in Downloads. While the app is running (or
# hanging) it also records what each part of the app is doing.
report_tool() {
    cat <<'REPORT'
#!/bin/bash
# Black Arrow Online report. Start it from the applications menu, best while
# the problem is happening (with the game still open): it makes a .tar.gz in
# Downloads to send to whoever shared the package.
data="${XDG_DATA_HOME:-$HOME/.local/share}/Black Arrow Online"
runlog="$data/system/report-run.log"
mkdir -p "$data/system" 2>/dev/null

# From the applications menu there is no terminal: open one and run again in
# it. The copy in /tmp has no spaces in its path, which some terminals' -e
# split on. With no terminal the report is still made; the Downloads folder
# opens and a notice shows when it is done.
if [ ! -t 1 ] && [ -z "${BA_REPORT_IN_TERMINAL:-}" ]; then
    export BA_REPORT_IN_TERMINAL=1
    copy="$(mktemp /tmp/black-arrow-report-XXXXXX.sh)" && cp "$0" "$copy"
    for term in x-terminal-emulator gnome-terminal kgx ptyxis konsole \
            xfce4-terminal mate-terminal lxterminal tilix xterm; do
        [ -n "$copy" ] && command -v "$term" >/dev/null 2>&1 || continue
        echo "$(date '+%F %T') opening $term" >> "$runlog"
        case "$term" in
            gnome-terminal|kgx|ptyxis|tilix) "$term" -- bash "$copy" ;;
            xfce4-terminal) "$term" -x bash "$copy" ;;
            *) "$term" -e bash "$copy" ;;
        esac && exit 0
        echo "$(date '+%F %T') $term did not start" >> "$runlog"
    done
    rm -f "$copy"
    echo "$(date '+%F %T') no terminal, making the report without one" >> "$runlog"
    exec >> "$runlog" 2>&1 </dev/null
fi
echo "$(date '+%F %T') report started (terminal: $([ -t 1 ] && echo yes || echo no))" >> "$runlog"
stamp="$(date +%Y-%m-%d_%H%M%S)"
downloads="$(xdg-user-dir DOWNLOAD 2>/dev/null || true)"
[ -n "$downloads" ] && [ -d "$downloads" ] || downloads="$HOME/Downloads"
mkdir -p "$downloads" 2>/dev/null || downloads="$HOME"
name="BlackArrow-report-$stamp"
out="$(mktemp -d)/$name"
mkdir -p "$out" || exit 1
echo "Making a Black Arrow Online report. This takes about 15 seconds..."

pid=""
exe="$(readlink -f "$data/app/usr/bin/xemu" 2>/dev/null)"
for p in $(pgrep -f -- "$data/app/" 2>/dev/null); do
    [ -n "$exe" ] && [ "$(readlink -f "/proc/$p/exe" 2>/dev/null)" = "$exe" ] && { pid="$p"; break; }
done
if [ -n "$pid" ]; then
    echo "Black Arrow Online is open: recording what it is doing..."
    for round in 1 2 3; do
        echo "=== $(date +%T)"
        for task in /proc/"$pid"/task/*; do
            printf '%s %s %s %s\n' "${task##*/}" "$(cat "$task/comm" 2>/dev/null)" \
                "$(cut -d' ' -f3 "$task/stat" 2>/dev/null)" "$(cat "$task/wchan" 2>/dev/null)"
        done
        sleep 3
    done > "$out/threads.txt" 2>&1
    if command -v gdb >/dev/null 2>&1; then
        timeout 30 gdb -p "$pid" -batch -ex 'thread apply all bt' \
            > "$out/stacks.txt" 2>&1 || true
    fi
    cat "/proc/$pid/status" > "$out/process.txt" 2>/dev/null
else
    echo "Black Arrow Online was not open." > "$out/not-open.txt"
fi

{
    cat /etc/os-release
    echo
    uname -a
    echo
    echo "Session: ${XDG_SESSION_TYPE:-?} ${XDG_CURRENT_DESKTOP:-?}"
    grep -m 1 'model name' /proc/cpuinfo
    echo "CPUs: $(nproc)"
    free -h
    echo
    lspci 2>/dev/null | grep -Ei 'vga|3d|display'
    command -v glxinfo >/dev/null 2>&1 && glxinfo -B
    command -v vulkaninfo >/dev/null 2>&1 && vulkaninfo --summary
} > "$out/linux.txt" 2>&1

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
# Crashes of the app from the last week, where systemd keeps them
if command -v coredumpctl >/dev/null 2>&1; then
    coredumpctl list --since=-7d --no-pager 2>/dev/null |
        grep -Ei 'xemu|AppRun' > "$out/crashes.txt" || rm -f "$out/crashes.txt"
fi

tar -czf "$downloads/$name.tar.gz" -C "$(dirname "$out")" "$name" && rm -rf "$(dirname "$out")"
echo
echo "Done: $name.tar.gz is in $downloads."
echo "Send it to whoever shared the package. You can close this window."
echo "$(date '+%F %T') report done: $downloads/$name.tar.gz" >> "$runlog"
xdg-open "$downloads" >/dev/null 2>&1 &
command -v notify-send >/dev/null 2>&1 &&
    notify-send "Black Arrow Report" "$name.tar.gz is in $downloads" >/dev/null 2>&1
[ -n "${BA_REPORT_IN_TERMINAL:-}" ] && [ "$0" != "${0#/tmp/black-arrow-report-}" ] && rm -f "$0"
sleep 1
read -r -p "Press Enter to close. " _ || true
REPORT
}

# The applications menu entries and the icon
install_menu() {
    mkdir -p "$menu" "$icons/256x256/apps"
    if [ -f "$app/xemu.png" ]; then
        cp "$app/xemu.png" "$icons/256x256/apps/black-arrow-online.png"
    elif [ -f "$app/usr/share/icons/hicolor/256x256/apps/xemu.png" ]; then
        cp "$app/usr/share/icons/hicolor/256x256/apps/xemu.png" \
            "$icons/256x256/apps/black-arrow-online.png"
    fi
    report_tool > "$data/report.sh"
    chmod 755 "$data/report.sh"
    cat > "$menu/black-arrow-online.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Black Arrow Online
Comment=Rainbow Six 3: Black Arrow on XB Online
Exec="$app/AppRun"
Icon=black-arrow-online
Terminal=false
Categories=Game;Emulator;
StartupWMClass=xemu
EOF
    cat > "$menu/black-arrow-report.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Black Arrow Report
Comment=Makes a file with logs to send when Black Arrow Online has a problem
Exec=bash "$data/report.sh"
Icon=black-arrow-online
Terminal=false
Categories=Game;
EOF
    chmod 644 "$menu/black-arrow-online.desktop" "$menu/black-arrow-report.desktop"
    command -v update-desktop-database >/dev/null 2>&1 &&
        update-desktop-database "$menu" >/dev/null 2>&1 || true
}

# A newly set up console: clean hard disk, new EEPROM (made on first start),
# and the first start opens the XBL Dashboard again
new_console() {
    [ -f "$system/fresh/xbox_hdd.qcow2" ] ||
        fail "the Xbox files are not installed yet. Run setup.sh from the full Linux package."
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
    entries="$(zip_list "$pack" 2>/dev/null)" ||
        fail "$pack is not a zip, or it did not download completely."
    case "$NL$entries$NL" in
        *"${NL}system/content.txt${NL}"*) ;;
        *) fail "$pack is not a Black Arrow Online content pack." ;;
    esac
    echo "Installing the content pack (this takes a minute)..."
    zip_extract_system "$pack"
}

if [ "${1:-}" = "--start-over" ]; then
    running && fail "quit Black Arrow Online first."
    new_console
    echo "Done. Start Black Arrow Online from your applications menu."
    exit 0
fi

if [ "${1:-}" = "--uninstall" ]; then
    running && fail "quit Black Arrow Online first."
    rm -rf "$app" "$data/report.sh"
    rm -f "$menu/black-arrow-online.desktop" "$menu/black-arrow-report.desktop" \
        "$icons/256x256/apps/black-arrow-online.png"
    echo "Black Arrow Online is removed. Your console and games are still in"
    echo "$data; delete that folder too to remove everything."
    exit 0
fi

if [ -n "$update_pid" ]; then
    # No terminal: what happens goes to update.log
    mkdir -p "$system"
    if [ -f "$system/update.log" ]; then
        tail -n 300 "$system/update.log" > "$system/update.log.tmp" &&
            mv "$system/update.log.tmp" "$system/update.log"
    fi
    exec >> "$system/update.log" 2>&1
    echo "=== $(date): updating from $here"
    trap 'fail "step at line $LINENO of setup.sh failed (see update.log)."' ERR
    # After a minute ask it to quit, then make it
    waited=0
    while alive "$update_pid"; do
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

[ -e "$here/app/AppRun" ] ||
    fail "the app folder is not next to setup.sh. Unpack the whole download first."
running && fail "quit Black Arrow Online first."

mkdir -p "$system" "$games"
if [ -n "${1:-}" ]; then
    install_zip_content "$1"
elif [ -f "$here/system/content.txt" ]; then
    install_folder_content
elif [ ! -f "$system/content.txt" ]; then
    fail "this download is only the app, and the Xbox files are not installed yet.
Get the full Linux package from where Black Arrow Online is shared, unpack it
and run its setup.sh instead."
fi

echo "Installing the app in $data..."
# Copied next to the old app first, then swapped, so a failed copy leaves the
# old app working
rm -rf "$app.new" "$app.old"
cp -a "$here/app" "$app.new" || fail "could not copy the app to $data."
if [ -d "$app" ]; then
    mv "$app" "$app.old" || fail "could not replace the app in $data."
fi
if ! mv "$app.new" "$app"; then
    mv "$app.old" "$app" 2>/dev/null || true
    fail "could not replace the app in $data."
fi
rm -rf "$app.old"

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
install_menu

if [ -n "$update_pid" ]; then
    # The app says "Updated to build N" once, then this folder (unpacked by
    # the app in a temporary folder) goes
    build="$({ grep -m 1 '^build=' "$here/version.txt" 2>/dev/null || true; } | cut -d= -f2-)"
    # A sub build: 26.1 (build=26, sub=1)
    sub="$({ grep -m 1 '^sub=' "$here/version.txt" 2>/dev/null || true; } | cut -d= -f2- | tr -d '\r')"
    build="$(printf '%s' "$build" | tr -d '\r')${sub:+.$sub}"
    printf '%s\n' "${build:-0}" > "$system/update-done.txt"
    echo "Done: build ${build:-?} installed. Opening it."
    trap - ERR
    start_app || echo "Could not open $app/AppRun"
    case "$(dirname "$here")" in
        */BlackArrowOnline-update-*) rm -rf "$(dirname "$here")" ;;
    esac
    exit 0
fi

echo
echo "Done. Black Arrow Online is in your applications menu."
echo "If something goes wrong, start \"Black Arrow Report\" from the same menu"
echo "(with the game still open if it is stuck) and send the file it makes."
if $first_time; then
    echo "Put your Rainbow Six 3: Black Arrow disc image (.iso) in the games"
    echo "folder, which opens now:"
    echo "  $games"
    xdg-open "$games" >/dev/null 2>&1 &
else
    echo "Your console and games are as you left them."
fi

# Black Arrow Online

A build of [xemu](https://github.com/xemu-project/xemu), the original Xbox
emulator, set up for playing Rainbow Six 3: Black Arrow on XB Online
(xb.live). This repository is the xemu source with the changes below, plus the
launcher and the scripts used to assemble the package.

xemu is licensed under the GPL, version 2 or later. See `LICENSE` and
`COPYING` in the repository root.

## What is changed from xemu

- **Menus**: Play, XB Live, Sound, Graphics, Troubleshoot and Help: start a game from the games folder, run the game
  update installer, open the XBL Dashboard for account setup or the original
  Xbox dashboard, show the console's Xbox Live gamertag, and open the xb.live
  pages to create an account or get a link code.
- **XB Online DNS redirect**: a network setting that points the console's
  Xbox Live lookups at XB Online.
- **Hosting**: with NAT networking the console takes the computer's own
  network address, so players on the same network can join a game hosted in
  the emulator. `lan_address = false` under `[net.nat]` in `xemu.toml`
  restores the stock behaviour.
- **First start**: a newly set up console opens the XBL Dashboard, from the
  hard disk when it is installed there and from its disc image otherwise.
- **Portable folder**: settings, BIOS and disk paths are relative to the
  folder of the executable.
- **Graphics**: enhanced graphics (antialiasing, sharpening, trilinear and
  anisotropic filtering), a fix for menu textures in Black Arrow, and an
  optional speed-up, "Improve render handling", under Graphics and Settings > Display.
- **Idle CPU use**: short main-loop waits sleep instead of spinning.
- **Network card**: packets held while the game's receive buffer was full
  are delivered as soon as it has room again.
- **Memory**: the console has 128 MB by default instead of 64 MB.

The source changes are the commits on top of the upstream history.

## What is not in this repository

The Xbox BIOS and MCPX boot ROM, the Xbox dashboard files, the XBL Dashboard,
hard disk images and game content. They are not ours to distribute here. The
launcher asks each player for their own files, or starts from a clean disk
that the package maintainer assembles.

## Folders

| Folder | Contents |
|---|---|
| `docs/` | Images used by the README and the release notes |
| `package/` | Launcher source (`Launcher.cs`), the `xemu.toml` shipped with the package, the player README and the reset script |
| `tools/` | Build script for the Windows executable, the clean hard disk builder, and FATX / Xbox disc image helpers |

## Building xemu.exe

With Docker, from a checkout of this repository:

```
docker run --rm -v xemu-build:/work -v "<checkout>:/src:ro" \
    -v "<checkout>/black-arrow-online/tools:/tools:ro" -v "<output folder>:/out" \
    ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-2881edd \
    bash /tools/docker-build.sh
```

The result is `<output folder>/dist/xemu.exe`.

## Building the launcher

On Windows, with the C# compiler that ships with the .NET Framework:

```
csc.exe /nologo /target:winexe /optimize+ /win32icon:ui\icons\xemu-icon.ico ^
    "/out:Black Arrow Online.exe" /r:System.Windows.Forms.dll ^
    /r:System.Drawing.dll /r:System.IO.Compression.dll ^
    /r:System.IO.Compression.FileSystem.dll black-arrow-online\package\Launcher.cs
```

## The full package and the content pack

Players get the full package: a release plus the files that are not in this
repository, in one zip, shared outside GitHub. Releases then keep it up to
date through the launcher.

For assembling or repairing a package, the launcher can also take those
files as a separate zip, laid out like the package folder, with a marker
file so the launcher recognises it:

```
BlackArrowOnline-content.zip
    system/content.txt
    system/mcpx.bin
    system/Complex_4627v1.03.bin
    system/XBL Dashboard.iso
    system/XBL Dashboard Loader.iso
    system/Black Arrow Update and Maps.iso
    system/fresh/xbox_hdd.qcow2
```

A release unzipped into an empty folder plus the content pack is a complete
package. The launcher writes a default `xemu.toml` when there is none, and
while the BIOS, MCPX or clean disk is missing its clean start button asks
for the content pack instead. It only ever unpacks into `system` and never
over `xemu.toml`, `eeprom.bin`, `xbox_hdd.qcow2` or `xemu.exe`.

## Releases and updates

`.github/workflows/black-arrow-online.yml` builds `xemu.exe` and the launcher
and publishes them as a release named `build-N`. It runs when `main` changes
and once a day, when it looks for a new xemu release. A new xemu release is
merged at build time, not pushed to `main`; if it does not merge cleanly the
workflow opens an issue and releases nothing until `main` has been merged by
hand.

Each release has three files:

| File | Contents |
|---|---|
| `BlackArrowOnline-update.zip` | Launcher, `xemu.exe`, README and `system/version.txt`, laid out like the package |
| `version.txt` | Build number, xemu version and the SHA-256 of the zip |
| `BlackArrowOnline-source-build-N.tar.zst` | The complete source of that build |

The launcher compares `system\version.txt` with the `version.txt` of the
latest release and offers the update when the build number is higher. It
never replaces `xemu.toml`, `eeprom.bin`, `xbox_hdd.qcow2` or anything in
`games`.

## Package layout

```
Black Arrow Online.exe      launcher
README.txt
games\                      the player's own game disc image
system\
    xemu.exe
    xemu.toml
    mcpx.bin, <BIOS>.bin    supplied separately
    XBL Dashboard.iso       supplied separately
    XBL Dashboard Loader.iso
    fresh\xbox_hdd.qcow2    clean disk, built with tools/build-clean-hdd.sh
```

# Black Arrow Online

A build of [xemu](https://github.com/xemu-project/xemu), the original Xbox
emulator, set up for playing Rainbow Six 3: Black Arrow on XB Online
(xb.live). This repository is the xemu source with the changes below, plus the
launcher and the scripts used to assemble the package.

xemu is licensed under the GPL, version 2 or later. See `LICENSE` and
`COPYING` in the repository root.

## What is changed from xemu

- **XB Online menu**: start a game from the games folder, open the XBL
  Dashboard for account setup, open the original Xbox dashboard, run the game
  update installer.
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
- **Graphics**: a fix for menu textures in Black Arrow and an optional
  speed-up, "Skip render target copy-back", under Settings > Display.
- **Idle CPU use**: short main-loop waits sleep instead of spinning.

The source changes are the commits on top of the upstream history.

## What is not in this repository

The Xbox BIOS and MCPX boot ROM, the Xbox dashboard files, the XBL Dashboard,
hard disk images and game content. They are not ours to distribute here. The
launcher asks each player for their own files, or starts from a clean disk
that the package maintainer assembles.

## Folders

| Folder | Contents |
|---|---|
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
    /r:System.Drawing.dll black-arrow-online\package\Launcher.cs
```

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

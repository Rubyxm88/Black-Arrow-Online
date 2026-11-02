#!/usr/bin/env python3
"""Builds the full Black Arrow Online package zip and the update zip.

The stage starts as a copy of the last package folder. The files kept in
this repository and the new xemu.exe are copied over it, then the game
content discs below are added. Those discs never go to GitHub, so they come
from the project folder on this PC; a disc that is missing stops the build.

The update zip holds the same files as the release workflow.

usage: make-package.py BASE STAGE OUT --xemu EXE --content-root DIR
                       --build N --commit SHA --version V --notes TEXT
                       [--launcher EXE] [--date YYYY-MM-DD]

  BASE   the last package folder (holding "Black Arrow Online.exe")
  STAGE  folder the new package is built in (BlackArrowOnline goes inside)
  OUT    folder for BlackArrowOnline.zip, BlackArrowOnline-update.zip and
         version.txt
"""
import argparse
import datetime
import filecmp
import hashlib
import os
import re
import shutil
import struct
import sys
import zipfile

PACKAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                       'package')

# Package path -> file name in --content-root. Taken from BASE when the
# content root does not have it.
CONTENT_DISCS = [
    # Play > Black Arrow Updates (stock title update and map packs)
    ('system/Black Arrow Update and Maps.iso',
     'Black Arrow Update and Maps.iso'),
    # Play > Old Bow Updates (stock Rainbow Six 3 title updates and maps)
    ('system/Rainbow Six 3 Update and Maps.iso', 'Rainbow Six 3 DLC.iso'),
]

# The title updates on the update discs and the hash xemu's game update
# check knows each by (ui/xemu-xbonline.h). Packaging stops when one
# differs: a changed game update has to ship with an xemu that knows it.
UPDATE_HASHES = [
    ('system/Black Arrow Update and Maps.iso', 'content/u/default.xbe',
     'XEMU_XBONLINE_TITLE_UPDATE_SHA256', 'Black Arrow'),
    ('system/Rainbow Six 3 Update and Maps.iso', 'content/uRF/default.xbe',
     'XEMU_XBONLINE_RS3_UPDATE_RF_SHA256', 'Old Bow RF'),
    ('system/Rainbow Six 3 Update and Maps.iso', 'content/uGer/default.xbe',
     'XEMU_XBONLINE_RS3_UPDATE_GER_SHA256', 'Old Bow Ger'),
]
XBONLINE_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                          '..', 'ui', 'xemu-xbonline.h')


def xiso_file(iso, path):
    """Bytes of a file on an Xbox disc image, None when it is not there"""
    with open(iso, 'rb') as f:
        for base in (0, 0x18300000, 0x1040000, 0xFD90000):
            f.seek(base + 32 * 2048)
            vd = f.read(28)
            if vd[:20] == b'MICROSOFT*XBOX*MEDIA':
                break
        else:
            return None
        sector, size = struct.unpack_from('<II', vd, 20)
        for part in path.split('/'):
            f.seek(base + sector * 2048)
            table = f.read(size)
            found = None
            pos = 0
            while pos + 14 <= len(table):
                left, right, sec, sz, attr, n = struct.unpack_from(
                    '<HHIIBB', table, pos)
                if left == 0xffff:      # padding up to the next sector
                    pos = (pos // 2048 + 1) * 2048
                    continue
                name = table[pos + 14:pos + 14 + n].decode('latin-1')
                if name.lower() == part.lower():
                    found = (sec, sz)
                    break
                pos = (pos + 14 + n + 3) & ~3
            if not found:
                return None
            sector, size = found
        f.seek(base + sector * 2048)
        return f.read(size)


def check_update_discs(pkg):
    """The title updates on the update discs are the ones xemu's check knows.
    Stops with both hashes when one changed, so whoever packages asks
    whether that game update was changed on purpose."""
    with open(XBONLINE_H) as f:
        header = f.read()
    changed = []
    for disc, path, define, game in UPDATE_HASHES:
        want = re.search(r'#define %s \\\s*"([0-9a-f]{64})"' % define,
                         header).group(1)
        iso = os.path.join(pkg, disc)
        xbe = xiso_file(iso, path)
        if xbe is None:
            sys.exit('%s has no %s' % (iso, path))
        have = hashlib.sha256(
            xbe[:struct.unpack_from('<I', xbe, 0x108)[0]]).hexdigest()
        if have != want:
            changed.append('  %s (%s on %s)\n    disc: %s\n    xemu: %s (%s)'
                           % (game, path, disc, have, want, define))
        else:
            print('%s update: %s' % (game, have))
    if changed:
        sys.exit('STOP: a game update on the update discs is not the one '
                 'this xemu knows:\n' + '\n'.join(changed) + '\n'
                 'Ask: was this game update changed on purpose? If yes, put '
                 'the disc hash in ui/xemu-xbonline.h, build xemu again and '
                 'package with that build. If no, use the right disc.')


# Same files as the release workflow, rooted at the package folder
UPDATE_FILES = ['Black Arrow Online.exe', 'README.txt', 'system/xemu.exe',
                'system/LICENSE.txt', 'system/Start over.bat',
                'system/Launcher-source.cs', 'system/shader-list.bin',
                'system/version.txt']


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False):
        return
    shutil.copy2(src, dst)
    print('copied', src)


def make_zip(path, root, files):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED,
                         compresslevel=6) as z:
        for f in files:
            z.write(os.path.join(root, f), f)
    print(os.path.getsize(path) // (1024 * 1024), 'MB', len(files), 'files',
          path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base')
    ap.add_argument('stage')
    ap.add_argument('out')
    ap.add_argument('--xemu', required=True)
    ap.add_argument('--launcher')
    ap.add_argument('--content-root', required=True)
    ap.add_argument('--build', required=True)
    ap.add_argument('--commit', required=True)
    ap.add_argument('--version', required=True)
    ap.add_argument('--notes', required=True)
    ap.add_argument('--date', default=datetime.date.today().isoformat())
    a = ap.parse_args()

    pkg = os.path.join(a.stage, 'BlackArrowOnline')
    if os.path.abspath(a.base) != os.path.abspath(pkg):
        shutil.copytree(a.base, pkg, dirs_exist_ok=True)

    copy(os.path.join(PACKAGE, 'README.txt'), os.path.join(pkg, 'README.txt'))
    copy(os.path.join(PACKAGE, 'Start over.bat'),
         os.path.join(pkg, 'system', 'Start over.bat'))
    copy(os.path.join(PACKAGE, 'Launcher.cs'),
         os.path.join(pkg, 'system', 'Launcher-source.cs'))
    copy(os.path.join(PACKAGE, 'shader-list.bin'),
         os.path.join(pkg, 'system', 'shader-list.bin'))
    copy(a.xemu, os.path.join(pkg, 'system', 'xemu.exe'))
    if a.launcher:
        copy(a.launcher, os.path.join(pkg, 'Black Arrow Online.exe'))

    missing = []
    for dest, name in CONTENT_DISCS:
        src = os.path.join(a.content_root, name)
        if os.path.exists(src):
            copy(src, os.path.join(pkg, dest))
        if not os.path.exists(os.path.join(pkg, dest)):
            missing.append('%s (from %s)' % (dest, src))
    if missing:
        sys.exit('missing content discs:\n  ' + '\n  '.join(missing))
    check_update_discs(pkg)

    # The xemu release is the start of the version, as in 0.8.137-91-g...
    xemu = a.version.lstrip('v').split('-')[0]
    with open(os.path.join(pkg, 'system', 'version.txt'), 'w',
              newline='\n') as f:
        f.write('build=%s\nxemu=%s\nversion=%s\ndate=%s\ncommit=%s\n'
                'upstream=v%s\nnotes=%s\n' %
                (a.build, xemu, a.version, a.date, a.commit, xemu, a.notes))

    os.makedirs(a.out, exist_ok=True)
    full = []
    for r, _, fs in os.walk(pkg):
        for f in fs:
            full.append(os.path.relpath(os.path.join(r, f),
                                        a.stage).replace(os.sep, '/'))
    make_zip(os.path.join(a.out, 'BlackArrowOnline.zip'), a.stage,
             sorted(full))

    upath = os.path.join(a.out, 'BlackArrowOnline-update.zip')
    make_zip(upath, pkg, UPDATE_FILES)

    with open(os.path.join(pkg, 'system', 'version.txt')) as f:
        text = f.read()
    with open(upath, 'rb') as f:
        text += 'sha256=' + hashlib.sha256(f.read()).hexdigest() + '\n'
    with open(os.path.join(a.out, 'version.txt'), 'w', newline='\n') as f:
        f.write(text)
    print(text)


if __name__ == '__main__':
    main()

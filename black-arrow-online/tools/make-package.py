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
import shutil
import sys
import zipfile

PACKAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                       'package')

# Package path -> file name in --content-root. Taken from BASE when the
# content root does not have it.
CONTENT_DISCS = [
    ('system/Black Arrow Update and Maps.iso',
     'Black Arrow Update and Maps.iso'),
    # Old Bow - Install Game Updates (Rainbow Six 3 title updates and maps)
    ('system/Rainbow Six 3 Update and Maps.iso', 'Rainbow Six 3 DLC.iso'),
]

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

    with open(os.path.join(pkg, 'system', 'version.txt'), 'w',
              newline='\n') as f:
        f.write('build=%s\nxemu=0.8.136\nversion=%s\ndate=%s\ncommit=%s\n'
                'upstream=v0.8.136\nnotes=%s\n' %
                (a.build, a.version, a.date, a.commit, a.notes))

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

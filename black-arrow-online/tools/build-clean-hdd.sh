#!/bin/bash
# Builds the clean hard disk for the shareable package from xemu's free blank
# disk image. Run inside the build container with /blank, /tools and /out
# mounted, plus any of:
#   /cdrive   files for the system partition (the original Xbox dashboard)
#   /edrive   files for the data partition
# The XBL Dashboard goes in /edrive/XBLDashboard so it can update itself.
set -e
apt-get update -qq
apt-get install -y -qq qemu-utils unzip > /dev/null
cd /tmp
unzip -o -q /blank/xbox_hdd.qcow2.zip
qemu-img convert -O raw xbox_hdd.qcow2 hdd.raw
if [ -d /cdrive ]; then
    python3 /tools/fatx_put.py put hdd.raw /cdrive 'C:\'
fi
if [ -d /edrive ]; then
    python3 /tools/fatx_put.py put hdd.raw /edrive 'E:\'
fi
echo "--- finished disk"
python3 /tools/fatx_put.py list hdd.raw | grep -v -E '^C:\\(Audio|xodash|xboxdashdata|fonts)\\.'
qemu-img convert -O qcow2 hdd.raw /out/xbox_hdd.clean.qcow2
qemu-img info /out/xbox_hdd.clean.qcow2

#!/usr/bin/env bash
# Runs inside the xemu-win64-toolchain container. Syncs the Windows checkout
# (/src, read-only) into a Linux volume (/work), builds, and copies the
# result to /out.
set -euo pipefail

export CROSSPREFIX=x86_64-w64-mingw32.static-
export CROSSAR=x86_64-w64-mingw32.static-gcc-ar
export CCACHE_DIR=/work/ccache
export LTO_CACHE_DIR=/work/lto-cache
mkdir -p "$CCACHE_DIR" "$LTO_CACHE_DIR"

command -v curl >/dev/null || { apt-get update -q && apt-get install -qy curl; }

git config --global --add safe.directory '*'

if [ ! -d /work/xemu/.git ]; then
    git clone --depth 1 "file:///src" /work/xemu
fi
cd /work/xemu
git fetch --depth 1 origin HEAD
git reset --hard FETCH_HEAD
git clean -fdq -e build -e dist -e macos-libs
# Shallow clones carry no tags; give the build a version to report.
git tag -f v0.8.136-1-xbl

# Carry over uncommitted changes from the Windows checkout.
if ! git -C /src diff --quiet HEAD; then
    git -C /src diff HEAD | git apply --whitespace=nowarn
fi

./build.sh -p win64-cross \
    --extra-cflags="-flto-incremental=${LTO_CACHE_DIR} -flto-partition=cache" \
    -Db_lto=true \
    -Dx86_version=3 \
    --enable-whpx

rm -rf /out/dist
cp -r dist /out/dist
ls -la /out/dist

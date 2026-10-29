#!/usr/bin/env bash
# Builds the Linux app as the release workflow's linux job does, inside an
# ubuntu:22.04 container. Syncs the checkout (/src, read-only) into a Linux
# volume (/work), builds the .deb, makes the app folder with linuxdeploy and
# copies xemu-linux-app.tar.gz to /out.
#
#   MSYS_NO_PATHCONV=1 docker run --rm -v xemu-build-linux:/work \
#       -v "$PWD":/src:ro -v "$PWD/../out/linux-beta":/out \
#       -e XEMU_TAG=$(git describe --tags --match 'v*') ubuntu:22.04 \
#       bash /src/black-arrow-online/tools/docker-build-linux.sh
#
# From a worktree, also mount the repository's .git folder as /repo and
# pass -e XEMU_BRANCH=<branch>.
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
export CCACHE_DIR=/work/ccache CCACHE_MAXSIZE=2G
export DEB_BUILD_MAINT_OPTIONS=optimize=-lto
export CC="ccache clang-21" CXX="ccache clang++-21" CC_LD=lld-21 CXX_LD=lld-21
export AR=llvm-ar-21 RANLIB=llvm-ranlib-21 NM=llvm-nm-21
mkdir -p "$CCACHE_DIR"

# The container is new each run; the LLVM 21 packages (as xemu's own Linux
# build) are kept in /work
apt-get -qy update >/dev/null
apt-get install -qy ccache curl git wget file libusb-1.0-0-dev dpkg-dev \
    devscripts equivs >/dev/null
if ! ls /work/llvm21/clang-21_*.deb >/dev/null 2>&1; then
    pool=https://apt.llvm.org/jammy/pool/main/l/llvm-toolchain-21/
    ver="$(curl -fsSL --compressed "$pool" |
           grep -oE 'libllvm21_[0-9][^"]+_amd64\.deb' | head -n1 |
           sed -E 's/^libllvm21_//; s/_amd64\.deb$//')"
    mkdir -p /work/llvm21
    for pkg in clang-21 clang-tools-21 lld-21 libllvm21 libclang-cpp21 \
        libclang-common-21-dev libclang1-21 libclang-rt-21-dev llvm-21 \
        llvm-21-dev llvm-21-linker-tools llvm-21-runtime llvm-21-tools; do
        [ -f "/work/llvm21/${pkg}_${ver}_amd64.deb" ] ||
            curl -fsSL -o "/work/llvm21/${pkg}_${ver}_amd64.deb" "${pool}${pkg}_${ver}_amd64.deb"
    done
fi
apt-get install -qy --no-install-recommends /work/llvm21/*.deb >/dev/null
export PATH=/usr/lib/ccache:$PATH

git config --global --add safe.directory '*'
# A worktree's .git names a Windows path the container cannot reach: then
# the repository's .git folder is mounted as /repo and XEMU_BRANCH is built
# (committed changes only)
repo=/src
ref=HEAD
if [ -d /repo ]; then
    repo=/repo
    ref="${XEMU_BRANCH:?set XEMU_BRANCH with /repo}"
fi
if [ ! -d /work/xemu/.git ]; then
    git init -q /work/xemu
fi
cd /work/xemu
git fetch --depth 1 "file://$repo" "$ref"
git reset --hard FETCH_HEAD
echo "Building $(git rev-parse --short HEAD)"
git clean -fdxq -e build -e subprojects
# Shallow clones carry no tags; give the build the checkout's version
git tag -f "${XEMU_TAG:-v0.8.139-1-xbl}"
# Carry over uncommitted changes from the checkout
if [ "$repo" = /src ] && ! git -C /src diff --quiet HEAD; then
    git -C /src diff HEAD | git apply --whitespace=nowarn
fi

mk-build-deps -i -r -t 'apt-get -qy --no-install-recommends' debian/control >/dev/null
printf 'xemu (1:0.0.0-0) unstable; urgency=medium\n\n  Built from %s\n\n -- Black Arrow Online <noreply@github.com>  %s\n' \
    "${XEMU_TAG:-local}" "$(date -R)" > debian/changelog
ln -sf /bin/true /usr/bin/dpkg-genbuildinfo
export XEMU_BUILD_OPTIONS="-Dx86_version=3"
rm -f ../xemu_*.deb
dpkg-buildpackage --no-sign -b

cd /work
rm -rf deb app
mkdir deb app
[ -x linuxdeploy-x86_64.AppImage ] || {
    wget --no-verbose https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20250213-2/linuxdeploy-x86_64.AppImage
    chmod +x linuxdeploy-x86_64.AppImage
}
(cd deb && ar x ../xemu_*.deb)
tar -C app -xf deb/data.tar*
# libusb too: linuxdeploy leaves it to the system, and not every system has
# it
./linuxdeploy-x86_64.AppImage --appimage-extract-and-run --appdir app \
    --exclude-library 'libglib-2.0.so*' \
    --library /usr/lib/x86_64-linux-gnu/libusb-1.0.so.0
test -x app/AppRun
test -f app/usr/lib/libusb-1.0.so.0
tar -czf /out/xemu-linux-app.tar.gz app
ls -la app /out

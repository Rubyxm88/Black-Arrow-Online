"""Make shader-list.bin from xemu shader cache folders.

Each file in a cache folder (system/shaders-<tag>/xxxx/..., up to build 20
system/shaders/xxxx/...) holds the emulated GPU state of one shader next to
the program compiled for one graphics card. The state does not depend on
the graphics card, so the states of all files make a list that xemu's Shader Cache window can build on any PC.

  python make-shader-list.py OUT.bin CACHE_DIR [CACHE_DIR ...]

Several cache folders (other maps, other players) are merged and duplicates
dropped. Files from an xemu with a different shader state size are skipped,
because that emulator could not use them.
"""
import os
import struct
import sys

MAGIC = b"XBLSHL01"


def read_state(path):
    """The (state size, state bytes) stored in one cache file, or None."""
    data = open(path, "rb").read()
    pos = 0
    for _ in range(3):  # cache tag, GL vendor, GL version
        if pos + 8 > len(data):
            return None
        (n,) = struct.unpack_from("<Q", data, pos)
        pos += 8 + n
    pos += 4  # program binary format
    # The state is followed by the program size and the program, so the one
    # state size whose next 8 bytes give the rest of the file is the right one
    for size in range(64, len(data) - pos - 8 + 1):
        (program_size,) = struct.unpack_from("<Q", data, pos + size)
        if program_size == len(data) - (pos + size + 8):
            return size, data[pos:pos + size]
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, dirs = sys.argv[1], sys.argv[2:]
    sizes = {}
    states = {}
    for cache in dirs:
        for top, _, files in os.walk(cache):
            for name in files:
                found = read_state(os.path.join(top, name))
                if found:
                    size, state = found
                    sizes[size] = sizes.get(size, 0) + 1
                    states.setdefault(size, set()).add(state)
    if not sizes:
        sys.exit("no shader cache files found")
    size = max(sizes, key=sizes.get)
    if len(sizes) > 1:
        print("skipped files with other state sizes:",
              {s: n for s, n in sizes.items() if s != size})
    chosen = sorted(states[size])
    with open(out, "wb") as f:
        f.write(MAGIC + struct.pack("<II", size, len(chosen)))
        f.writelines(chosen)
    print(f"{out}: {len(chosen)} shaders, state size {size}, "
          f"{os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()

"""Copy files onto, or list, the FATX partitions of a raw Xbox hard disk image.

    python fatx_put.py list image.raw
    python fatx_put.py put  image.raw <local file or folder> <X:\\dest path>

For a folder, its contents are copied into the destination folder. An existing
file with the same name is replaced. Works on raw images only; convert a qcow2
image with qemu-img first.
"""
import os
import struct
import sys
import time

PARTS = {
    'C': (0x8CA80000, 0x1F400000),
    'E': (0xABE80000, 0x1312D6000),
}
ENTRY = 64
ATTR_DIR = 0x10


class Fatx:
    def __init__(self, f, base, size):
        self.f, self.base = f, base
        f.seek(base)
        hdr = f.read(0x1000)
        if hdr[:4] != b'FATX':
            raise SystemExit('no FATX signature at 0x%x' % base)
        self.cluster = struct.unpack_from('<I', hdr, 8)[0] * 512
        total = size // self.cluster
        self.wide = total >= 0xFFF0
        self.width = 4 if self.wide else 2
        fat_bytes = (total * self.width + 0xFFF) & ~0xFFF
        self.fat_off = base + 0x1000
        self.data_off = self.fat_off + fat_bytes
        self.count = (base + size - self.data_off) // self.cluster
        self.eoc = 0xFFFFFFFF if self.wide else 0xFFFF
        self.search_from = 2

    # --- allocation table ---
    def get(self, c):
        self.f.seek(self.fat_off + c * self.width)
        return struct.unpack('<I' if self.wide else '<H',
                             self.f.read(self.width))[0]

    def set(self, c, value):
        self.f.seek(self.fat_off + c * self.width)
        self.f.write(struct.pack('<I' if self.wide else '<H', value))

    def next(self, c):
        n = self.get(c)
        return None if n == 0 or n >= (self.eoc & ~0xF) else n

    def chain(self, c):
        out = []
        while c:
            out.append(c)
            c = self.next(c)
        return out

    def alloc(self, n):
        """Allocate and link a chain of n clusters; returns the first."""
        got = []
        c = self.search_from
        while len(got) < n:
            if c > self.count:
                raise SystemExit('partition is full')
            if self.get(c) == 0:
                got.append(c)
            c += 1
        self.search_from = c
        for i, c in enumerate(got):
            self.set(c, got[i + 1] if i + 1 < len(got) else self.eoc)
        return got[0]

    def free(self, first):
        for c in self.chain(first):
            self.set(c, 0)
        self.search_from = 2

    def offset(self, c):
        return self.data_off + (c - 1) * self.cluster

    # --- directories ---
    def entries(self, dir_cluster):
        """Yield (file offset, raw entry) for every slot of a directory."""
        for c in self.chain(dir_cluster):
            self.f.seek(self.offset(c))
            data = self.f.read(self.cluster)
            for i in range(0, self.cluster, ENTRY):
                yield self.offset(c) + i, data[i:i + ENTRY]

    def listdir(self, dir_cluster):
        out = []
        for pos, e in self.entries(dir_cluster):
            n = e[0]
            if n in (0x00, 0xFF):
                break
            if n == 0xE5 or n > 42:
                continue
            first, size = struct.unpack_from('<II', e, 44)
            out.append((e[2:2 + n].decode('latin-1'), bool(e[1] & ATTR_DIR),
                        first, size, pos))
        return out

    def find(self, dir_cluster, name):
        for item in self.listdir(dir_cluster):
            if item[0].lower() == name.lower():
                return item
        return None

    def add_entry(self, dir_cluster, name, attr, first, size):
        raw = name.encode('latin-1')
        if not 0 < len(raw) <= 42:
            raise SystemExit('bad file name: %r' % name)
        t = time.localtime()
        stamp = ((t.tm_year - 2000) << 25 | t.tm_mon << 21 | t.tm_mday << 16 |
                 t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec // 2)
        entry = (bytes([len(raw), attr]) + raw.ljust(42, b'\xff') +
                 struct.pack('<IIIII', first, size, stamp, stamp, stamp))
        slot = None
        for pos, e in self.entries(dir_cluster):
            if e[0] in (0x00, 0xFF, 0xE5):
                slot = pos
                break
        if slot is None:
            last = self.chain(dir_cluster)[-1]
            new = self.alloc(1)
            self.set(last, new)
            self.f.seek(self.offset(new))
            self.f.write(b'\xff' * self.cluster)
            slot = self.offset(new)
        self.f.seek(slot)
        self.f.write(entry)

    def mkdir(self, dir_cluster, name):
        found = self.find(dir_cluster, name)
        if found:
            if not found[1]:
                raise SystemExit('%s exists and is not a folder' % name)
            return found[2]
        new = self.alloc(1)
        self.f.seek(self.offset(new))
        self.f.write(b'\xff' * self.cluster)
        self.add_entry(dir_cluster, name, ATTR_DIR, new, 0)
        return new

    def put_file(self, dir_cluster, name, path):
        found = self.find(dir_cluster, name)
        if found:
            if found[1]:
                raise SystemExit('%s exists and is a folder' % name)
            if found[2]:
                self.free(found[2])
            self.f.seek(found[4])
            self.f.write(b'\xe5')
        size = os.path.getsize(path)
        first = 0
        if size:
            first = self.alloc(-(-size // self.cluster))
            with open(path, 'rb') as src:
                for c in self.chain(first):
                    self.f.seek(self.offset(c))
                    self.f.write(src.read(self.cluster).ljust(self.cluster,
                                                              b'\0'))
        self.add_entry(dir_cluster, name, 0, first, size)

    def put_tree(self, dir_cluster, folder):
        for name in sorted(os.listdir(folder)):
            full = os.path.join(folder, name)
            if os.path.isdir(full):
                self.put_tree(self.mkdir(dir_cluster, name), full)
            else:
                self.put_file(dir_cluster, name, full)


def get_tree(fs, cluster, folder):
    os.makedirs(folder, exist_ok=True)
    for name, is_dir, first, size, _ in fs.listdir(cluster):
        target = os.path.join(folder, name)
        if is_dir:
            get_tree(fs, first, target)
            continue
        with open(target, 'wb') as out:
            left = size
            for c in fs.chain(first) if size else []:
                fs.f.seek(fs.offset(c))
                out.write(fs.f.read(min(left, fs.cluster)))
                left -= min(left, fs.cluster)


def walk(fs, cluster, path, depth):
    for name, is_dir, first, size, _ in sorted(fs.listdir(cluster)):
        print('%s%s%s' % (path, name, '\\' if is_dir else '  (%d bytes)' % size))
        if is_dir and depth < 3:
            walk(fs, first, path + name + '\\', depth + 1)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    action, image = sys.argv[1], sys.argv[2]
    if action == 'list':
        with open(image, 'rb') as f:
            for letter, (base, size) in PARTS.items():
                walk(Fatx(f, base, size), 1, letter + ':\\', 0)
        return
    if action == 'get' and len(sys.argv) == 5:
        letter, _, rest = sys.argv[3].partition(':')
        with open(image, 'rb') as f:
            fs = Fatx(f, *PARTS[letter.upper()])
            cluster = 1
            for p in [p for p in rest.replace('/', '\\').split('\\') if p]:
                found = fs.find(cluster, p)
                if not found or not found[1]:
                    raise SystemExit('folder not found: %s' % p)
                cluster = found[2]
            get_tree(fs, cluster, sys.argv[4])
        print('copied %s -> %s' % (sys.argv[3], sys.argv[4]))
        return
    if action != 'put' or len(sys.argv) != 5:
        raise SystemExit(__doc__)
    source, dest = sys.argv[3], sys.argv[4]
    letter, _, rest = dest.partition(':')
    parts = [p for p in rest.replace('/', '\\').split('\\') if p]
    with open(image, 'r+b') as f:
        fs = Fatx(f, *PARTS[letter.upper()])
        cluster = 1
        if os.path.isdir(source):
            for p in parts:
                cluster = fs.mkdir(cluster, p)
            fs.put_tree(cluster, source)
        else:
            if not parts:
                raise SystemExit('destination needs a file name')
            for p in parts[:-1]:
                cluster = fs.mkdir(cluster, p)
            fs.put_file(cluster, parts[-1], source)
    print('copied %s -> %s' % (source, dest))


if __name__ == '__main__':
    main()

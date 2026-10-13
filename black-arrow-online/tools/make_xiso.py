"""Build an Xbox disc image (XDVDFS) from a single XBE or from a folder.

    python make_xiso.py input.xbe  output.iso   (stored as default.xbe)
    python make_xiso.py folder     output.iso   (folder must hold default.xbe)
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
FIRST_SECTOR = 33
ATTR_DIR = 0x10
ATTR_FILE = 0x20


class Node:
    def __init__(self, name, path=None, data=None):
        self.name = name
        self.path = path
        self.data = data
        self.children = None
        self.sector = 0
        self.size = 0


def scan(folder):
    node = Node(os.path.basename(folder))
    node.children = []
    for name in os.listdir(folder):
        full = os.path.join(folder, name)
        if os.path.isdir(full):
            child = scan(full)
            child.name = name
        else:
            child = Node(name, path=full)
            child.size = os.path.getsize(full)
        node.children.append(child)
    # XDVDFS directories are search trees ordered by upper-cased name.
    node.children.sort(key=lambda c: c.name.upper())
    return node


def entry_size(child):
    return (14 + len(child.name.encode('ascii')) + 3) & ~3


def layout_table(node):
    """Place the entries of the balanced tree so that every entry comes
    before the two it branches to: the console treats a branch that points
    backwards as a damaged disc."""
    count = len(node.children)
    order = []

    def visit(lo, hi):
        if lo < hi:
            mid = (lo + hi) // 2
            order.append(mid)
            visit(lo, mid)
            visit(mid + 1, hi)
    visit(0, count)
    offsets = [0] * count
    pos = 0
    for idx in order:
        size = entry_size(node.children[idx])
        if pos // SECTOR != (pos + size - 1) // SECTOR:
            pos = (pos + SECTOR - 1) // SECTOR * SECTOR
        offsets[idx] = pos
        pos += size
    node.offsets = offsets
    node.size = max(pos, 1)
    return node.size


def sectors(nbytes):
    return max(1, -(-nbytes // SECTOR))


def assign(node, next_sector):
    layout_table(node)
    node.sector = next_sector
    next_sector += sectors(node.size)
    for child in node.children:
        if child.children is not None:
            next_sector = assign(child, next_sector)
        else:
            child.sector = next_sector if child.size else 0
            next_sector += sectors(child.size) if child.size else 0
    return next_sector


def build_table(node):
    table = bytearray(b'\xff' * (sectors(node.size) * SECTOR))
    count = len(node.children)

    def link(lo, hi):
        """Balanced tree over children[lo:hi]; returns the root's index."""
        if lo >= hi:
            return None
        mid = (lo + hi) // 2
        left, right = link(lo, mid), link(mid + 1, hi)
        child = node.children[mid]
        name = child.name.encode('ascii')
        is_dir = child.children is not None
        struct.pack_into(
            '<HHIIBB', table, node.offsets[mid],
            node.offsets[left] // 4 if left is not None else 0,
            node.offsets[right] // 4 if right is not None else 0,
            child.sector,
            # Subdirectory tables are recorded in whole sectors, as other
            # image tools do; some programs fail to list them otherwise.
            sectors(child.size) * SECTOR if is_dir else child.size,
            ATTR_DIR if is_dir else ATTR_FILE, len(name))
        table[node.offsets[mid] + 14:node.offsets[mid] + 14 + len(name)] = name
        return mid

    root = link(0, count)
    if root is not None and node.offsets[root] != 0:
        raise SystemExit('internal error: tree root is not the first entry')
    return bytes(table)


def write(node, out):
    out.seek(node.sector * SECTOR)
    out.write(build_table(node))
    for child in node.children:
        if child.children is not None:
            write(child, out)
        elif child.size:
            out.seek(child.sector * SECTOR)
            if child.data is not None:
                out.write(child.data)
            else:
                with open(child.path, 'rb') as f:
                    out.write(f.read())


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    source, target = sys.argv[1], sys.argv[2]

    if os.path.isdir(source):
        root = scan(source)
    else:
        with open(source, 'rb') as f:
            data = f.read()
        root = Node('')
        child = Node('default.xbe', data=data)
        child.size = len(data)
        root.children = [child]

    names = [c.name.lower() for c in root.children]
    if 'default.xbe' not in names:
        raise SystemExit('no default.xbe at the top level')

    end = assign(root, FIRST_SECTOR)

    volume = bytearray(SECTOR)
    volume[0:20] = MAGIC
    struct.pack_into('<IIQ', volume, 20, root.sector, root.size, 0)
    volume[SECTOR - 20:] = MAGIC

    with open(target, 'wb') as out:
        # Pad to a whole number of 64 KiB blocks so reads near the end of
        # the last file never run past the end of the image.
        out.truncate(-(-((end + 32) * SECTOR) // 0x10000) * 0x10000)
        out.seek(32 * SECTOR)
        out.write(volume)
        write(root, out)
    print('wrote %s (%d sectors)' % (target, end + 32))


if __name__ == '__main__':
    main()

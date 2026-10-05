"""Read Saints Row: The Third packfiles (.vpp_pc, version 6). Read-only: game files are never changed.

  python tools/vpp.py list misc_tables.vpp_pc [filter]          files in a packfile (name in the game's cache/)
  python tools/vpp.py get misc_tables.vpp_pc explosions.xtbl OUTDIR
  python tools/vpp.py find explosions.xtbl                      which packfile has a file
Format: ThomasJepp.SaintsRow Packfiles/Version06, as changed to 64 bits by the Remaster: header fields from
0x150 and the 48-byte directory entries are u64, sections are 4 KB aligned, and compressed files are LZ4
(measured on misc_tables.vpp_pc: 30/30 files decompress to their listed sizes).
"""
import os
import struct
import sys

import lz4.block

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(yaml.safe_load(open(os.path.join(ROOT, "config.yaml")))["sr3"], "cache")
COMPRESSED, CONDENSED = 1, 2


def align(n, a=4096):  # the Remaster aligns sections to 4 KB (directory at 0x1000)
    return (n + a - 1) // a * a


class Packfile:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(0x188)
        magic, version = struct.unpack_from("<II", h, 0)
        if magic != 0x51890ACE or version != 6:
            raise ValueError(f"{path}: not a version 6 packfile")
        self.flags = struct.unpack_from("<I", h, 0x14C)[0]
        _, num, size, dir_size, names_size, self.data_size, self.comp_size = struct.unpack_from("<7Q", h, 0x150)
        if size != os.path.getsize(path):
            raise ValueError(f"{path}: header size {size} isn't the file's: unknown layout")
        names_off = align(align(0x188) + dir_size)
        self.data_off = align(names_off + names_size)
        self.f.seek(align(0x188))
        dir_ = self.f.read(num * 0x30)
        self.f.seek(names_off)
        names = self.f.read(names_size)
        self.entries = []
        running = 0
        for i in range(num):
            name_off, _, start, size, csize, _ = struct.unpack_from("<6Q", dir_, i * 0x30)
            name = names[name_off:names.index(0, name_off)].decode("ascii")
            if self.flags & CONDENSED and self.flags & COMPRESSED:
                start, running = running, running + size
            elif self.flags & CONDENSED:
                start, running = running, running + align(size, 16)
            elif self.flags & COMPRESSED:
                start, running = running, running + align(csize)
            self.entries.append((name, start, size, csize))
        self._blob = None

    def read(self, name):
        for n, start, size, csize in self.entries:
            if n.lower() != name.lower():
                continue
            if self.flags & CONDENSED and self.flags & COMPRESSED:
                if self._blob is None:
                    # Remaster: one LZ4 chunk per file (16-byte header as below), back to back (e.g. .str2_pc)
                    self.f.seek(self.data_off)
                    data, out, o = self.f.read(self.comp_size), [], 0
                    while o + 16 <= len(data):
                        _, _, cs, us = struct.unpack_from("<4I", data, o)
                        out.append(lz4.block.decompress(data[o + 16:o + 16 + cs], uncompressed_size=us))
                        o += 16 + cs
                    self._blob = b"".join(out)
                return self._blob[start:start + size]
            self.f.seek(self.data_off + start)
            if self.flags & COMPRESSED:
                # Remaster: per file a 16-byte header (magic, ?, compressed size, size), then an LZ4 block
                _, _, cs, us = struct.unpack("<4I", self.f.read(16))
                return lz4.block.decompress(self.f.read(cs), uncompressed_size=us)
            return self.f.read(size)
        raise KeyError(name)


def main():
    cmd = sys.argv[1]
    if cmd == "find":
        for p in sorted(os.listdir(CACHE)):
            if p.endswith(".vpp_pc"):
                try:
                    names = [e[0] for e in Packfile(os.path.join(CACHE, p)).entries]
                except ValueError:
                    continue
                for n in names:
                    if sys.argv[2].lower() in n.lower():
                        print(p, n)
        return
    pf = Packfile(os.path.join(CACHE, sys.argv[2]))
    if cmd == "list":
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for name, _, size, _ in pf.entries:
            if flt in name.lower():
                print(f"{size:10d} {name}")
    elif cmd == "get":
        os.makedirs(sys.argv[4], exist_ok=True)
        out = os.path.join(sys.argv[4], sys.argv[3])
        open(out, "wb").write(pf.read(sys.argv[3]))
        print(out)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Find C++ class vtables in a dumped image (tools/xexdump) through MSVC RTTI.

TypeDescriptor:  u32 vftable, u32 spare, char name[] (".?AVUIList@@")
CompleteObjectLocator: u32 signature, u32 offset, u32 cd_offset,
                       u32 type_descriptor, u32 class_descriptor
The word before each vtable points to its locator. Prints, for each class,
every vtable (offset 0 = the primary one) and its first N entries.

Usage: rtti.py <image_dir> <count> <Class> [<Class>...]
"""
import struct
import sys
from pathlib import Path


def load(d):
    secs = {}
    for line in (d / "sections.txt").read_text().splitlines():
        name, addr, size, _ = line.split()
        fname = d / (name.lstrip(".") + ".bin")
        if fname.exists():
            secs[name] = (int(addr, 16), fname.read_bytes()[: int(size, 16)])
    return secs


def main():
    d, count, classes = Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3:]
    secs = load(d)
    data = [(a, b) for n, (a, b) in secs.items() if n in (".rdata", ".data")]

    def u32(addr):
        for a, b in secs.values():
            if a <= addr < a + len(b) - 3:
                return struct.unpack_from(">I", b, addr - a)[0]
        return None

    def find_all(pattern):
        out = []
        for a, b in data:
            i = b.find(pattern)
            while i != -1:
                out.append(a + i)
                i = b.find(pattern, i + 1)
        return out

    for cls in classes:
        names = find_all(f".?AV{cls}@@\0".encode())
        if not names:
            print(f"{cls}: no RTTI"); continue
        td = names[0] - 8
        cols = [c - 12 for c in find_all(struct.pack(">I", td))]
        cols = [c for c in cols if u32(c) == 0]  # signature 0
        for col in cols:
            offset = u32(col + 4)
            for ref in find_all(struct.pack(">I", col)):
                vt = ref + 4
                entries = [u32(vt + 4 * i) for i in range(count)]
                print(f"{cls} vtable 0x{vt:08X} (offset {offset}):",
                      " ".join(f"{i}:{e:08X}" for i, e in enumerate(entries) if e))


main()

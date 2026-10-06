#!/usr/bin/env python3
"""Find functions that are only reachable indirectly (vtables, callbacks).

It looks at two kinds of indirect reference. One is data words, covered
further down. The other is addresses built in code with `lis rX,hi` +
`addi rY,rX,lo`, which is how callbacks get passed (qsort comparators, thread
entry points) and how selector functions return function pointers. Those are
accepted when the instruction before the target ends control flow. A pair
that feeds a `bctr` within a few instructions is a switch jump, so it's
skipped. So is a target right after `b .+4` (0x48000004): MSVC puts that
before EH/SEH funclets, which reach the parent frame through r31 and can't be
called on their own.

Data words:

Scans the read-only and writable data sections of a dumped image (see
tools/xexdump) for big-endian words that point into .text but are not
function starts in the last codegen run. A hit only counts as a missed
function if the instruction before it ends control flow (blr, bctr,
unconditional b) or is zero padding, and an adjacent data word is itself a
function start, like in a vtable or callback table. Newly found functions
count, so a vtable with several missed entries is found in full. A hit that
follows a `b` also has to look like a thunk: an adjustor `addi r3,r3,N`, a
bare `b`, or a vtable load that reaches `bctr` within a few instructions.

Analysis mostly misses small stubs with no .pdata entry. Most code pointers
in .rdata that follow a `b` are labels inside functions, not entries. C++ EH
state tables are (start, state, end, -1) records whose neighbours are small
integers, so the neighbour rule throws them out. SEH __try scope tables are
(count, begin, end, filter, handler) records. Their neighbours are code
labels too, so it takes the thunk-shape rule to catch those.

Prints a [functions] TOML block. Each entry runs up to the next known or
newly found function start.

Usage: find_missing_funcs.py <image_dir> <codegen.partition.json> [--exclude <toml> ...]
"""

import argparse
import json
import re
import struct
import sys
from bisect import bisect_right
from pathlib import Path

DATA_SECTIONS = (".rdata", ".data")

BLR = 0x4E800020
BCTR = 0x4E800420


def load_sections(image_dir):
    sections = {}
    for line in (image_dir / "sections.txt").read_text().splitlines():
        name, vaddr, size, _flags = line.split()
        data = (image_dir / (name.lstrip(".") + ".bin")).read_bytes()
        sections[name] = (int(vaddr, 16), int(size, 16), data)
    return sections


def is_branch(word):
    """Unconditional relative branch without link: opcode 18, AA=0, LK=0."""
    return (word >> 26) == 18 and (word & 3) == 0


def ends_flow(word):
    return word in (BLR, BCTR, 0) or is_branch(word)


def looks_like_thunk(text_word, addr):
    first = text_word(addr)
    if first >> 16 == 0x3863 or is_branch(first):  # addi r3,r3,N / b
        return True
    return any(text_word(addr + 4 * i) == BCTR for i in range(1, 6))


def code_refs(text, text_base, text_end, known, text_word):
    """Yield (target, ref address) for lis/addi pairs that build a code address."""
    count = len(text) // 4
    for i in range(count):
        w = struct.unpack_from(">I", text, i * 4)[0]
        if w >> 26 != 15 or (w >> 16) & 31 != 0:  # lis rD,hi (addis rD,0,hi)
            continue
        rd, hi = (w >> 21) & 31, w & 0xFFFF
        for j in range(i + 1, min(i + 10, count)):
            x = struct.unpack_from(">I", text, j * 4)[0]
            if x >> 26 == 15 and (x >> 21) & 31 == rd:  # rd reloaded
                break
            if x >> 26 != 14 or (x >> 16) & 31 != rd:  # addi rT,rd,lo
                continue
            lo = (x & 0xFFFF) - (0x10000 if x & 0x8000 else 0)
            target = ((hi << 16) + lo) & 0xFFFFFFFF
            if (text_base < target < text_end - 24 and not target & 3
                    and target not in known and text_word(target) != 0):
                prev = text_word(target - 4)
                following = [struct.unpack_from(">I", text, k * 4)[0]
                             for k in range(j + 1, min(j + 7, count))]
                if ends_flow(prev) and prev != 0x48000004 and BCTR not in following:
                    yield target, text_base + j * 4
            break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image_dir", type=Path)
    ap.add_argument("partition", type=Path)
    ap.add_argument("--exclude", type=Path, action="append", default=[],
                    help="TOML files whose [functions] addresses are already handled")
    args = ap.parse_args()

    sections = load_sections(args.image_dir)
    text_base, text_size, text = sections[".text"]
    text_end = text_base + text_size

    known = {int(a, 16) for a in json.loads(args.partition.read_text())["assignments"]}
    for path in args.exclude:
        known |= {int(a, 16) for a in re.findall(r"^(0x[0-9A-Fa-f]+)\s*=", path.read_text(), re.M)}

    def text_word(addr):
        return struct.unpack_from(">I", text, addr - text_base)[0]

    # (ptr, ref address, neighbouring data words) for every plausible hit.
    hits = []
    for name in DATA_SECTIONS:
        base, size, data = sections[name]
        for off in range(0, size - 3, 4):
            ptr = struct.unpack_from(">I", data, off)[0]
            if not (text_base < ptr < text_end - 24) or ptr & 3 or ptr in known:
                continue
            prev = text_word(ptr - 4)
            if not ends_flow(prev) or text_word(ptr) == 0:
                continue
            if is_branch(prev) and not looks_like_thunk(text_word, ptr):
                continue
            neighbours = [struct.unpack_from(">I", data, o)[0]
                          for o in (off - 4, off + 4) if 0 <= o <= size - 4]
            hits.append((ptr, base + off, neighbours))

    # Accept hits next to a function pointer, repeating until nothing changes.
    found = {}
    changed = True
    while changed:
        changed = False
        for ptr, ref, neighbours in hits:
            if ptr not in found and any(n in known or n in found for n in neighbours):
                found[ptr] = ref
                changed = True

    for target, ref in code_refs(text, text_base, text_end, known, text_word):
        found.setdefault(target, ref)

    starts = sorted(known | found.keys())
    print(f"# {len(found)} indirectly referenced functions missed by analysis")
    print("[functions]")
    for addr in sorted(found):
        i = bisect_right(starts, addr)
        nxt = starts[i] if i < len(starts) else text_end
        print(f"0x{addr:08X} = {{ size = 0x{nxt - addr:X} }}  # ref @0x{found[addr]:08X}")
    print(f"{len(found)} candidates", file=sys.stderr)


if __name__ == "__main__":
    main()

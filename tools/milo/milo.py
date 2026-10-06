"""Minimal reader for Harmonix .milo containers (RB3 Xbox 360, version 28).

Container (little-endian header):
  u32 magic       0xCDBEDEAF: blocks are raw deflate, each prefixed with its
                  u32 inflated size; 0xCABEDEAF: blocks stored uncompressed
  u32 data_offset where the first block starts
  u32 block_count
  u32 max_block_size
  u32 block_size[block_count]  top byte 0x01 = this block is stored as-is
Inflated stream (big-endian): an ObjectDir whose objects follow in order,
each ending with the 0xADDEADDE marker.
"""

import struct
import zlib


def inflate(path):
    data = open(path, "rb").read()
    magic, offset, count, _max = struct.unpack_from("<4I", data, 0)
    sizes = struct.unpack_from(f"<{count}I", data, 16)
    out = bytearray()
    pos = offset
    for size in sizes:
        stored = (size >> 24) == 1
        size &= 0x00FFFFFF
        block = data[pos:pos + size]
        pos += size
        if magic == 0xCABEDEAF or stored:
            out += block
        elif magic == 0xCDBEDEAF:
            out += zlib.decompressobj(-15).decompress(block[4:])
        elif magic == 0xCBBEDEAF:
            out += zlib.decompressobj(-15).decompress(block)
        else:
            raise ValueError(f"unknown milo magic {magic:#x}")
    return bytes(out)


def read_string(buf, pos):
    (n,) = struct.unpack_from(">I", buf, pos)
    return buf[pos + 4:pos + 4 + n].decode("latin-1"), pos + 4 + n


def entries(buf):
    """(type, name) of each object in the directory, in serialization order."""
    (version,) = struct.unpack_from(">I", buf, 0)
    pos = 4
    dir_type, pos = read_string(buf, pos)
    dir_name, pos = read_string(buf, pos)
    pos += 8  # two u32 counts/hashes
    (count,) = struct.unpack_from(">I", buf, pos)
    pos += 4
    out = []
    for _ in range(count):
        t, pos = read_string(buf, pos)
        n, pos = read_string(buf, pos)
        out.append((t, n))
    return version, (dir_type, dir_name), out, pos


def objects(buf):
    """Split the inflated stream on the 0xADDEADDE object terminator."""
    marker = b"\xad\xde\xad\xde"
    parts = buf.split(marker)
    return parts


def find_bitmap(obj):
    """Locate an embedded RndBitmap (rev 1) inside a Tex object's data.

    Header (big-endian): u8 rev=1, u8 bpp, u32 encoding, u8 mips, u16 width,
    u16 height, u16 row_bytes, 19 bytes padding; pixel data follows.
    Encodings: 3 = RGBA8888, 8 = DXT1, 24 = DXT5, 32 = ATI2/DXN.
    """
    for i in range(len(obj) - 32):
        if obj[i] != 1:
            continue
        bpp = obj[i + 1]
        (enc,) = struct.unpack_from(">I", obj, i + 2)
        mips = obj[i + 6]
        w, h, row = struct.unpack_from(">3H", obj, i + 7)
        if bpp not in (4, 8, 32) or enc not in (3, 8, 24, 32) or mips > 12:
            continue
        if not w or not h or w & (w - 1) or h & (h - 1) or row * 8 != w * bpp:
            continue
        start = i + 13 + 19
        if len(obj) - start < row * h:
            continue
        return dict(bpp=bpp, encoding=enc, mips=mips, width=w, height=h,
                    data=obj[start:start + row * h])
    return None


def _swap16(data):
    b = bytearray(data)
    b[0::2], b[1::2] = data[1::2], data[0::2]
    return bytes(b)


def _rgb565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _color_block(blk, out, w, x0, y0, dxt1):
    c0, c1, bits = struct.unpack_from("<HHI", blk, 0)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    if c0 > c1 or not dxt1:
        p2 = tuple((2 * a + b) // 3 for a, b in zip(p0, p1))
        p3 = tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))
        pal = [p0 + (255,), p1 + (255,), p2 + (255,), p3 + (255,)]
    else:
        p2 = tuple((a + b) // 2 for a, b in zip(p0, p1))
        pal = [p0 + (255,), p1 + (255,), p2 + (255,), (0, 0, 0, 0)]
    for py in range(4):
        for px in range(4):
            idx = (bits >> (2 * (py * 4 + px))) & 3
            o = ((y0 + py) * w + x0 + px) * 4
            out[o:o + 4] = bytes(pal[idx])


def _alpha_block(blk, out, w, x0, y0):
    a0, a1 = blk[0], blk[1]
    bits = int.from_bytes(blk[2:8], "little")
    if a0 > a1:
        pal = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
    else:
        pal = [a0, a1] + [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]
    for py in range(4):
        for px in range(4):
            o = ((y0 + py) * w + x0 + px) * 4 + 3
            out[o] = pal[(bits >> (3 * (py * 4 + px))) & 7]


def decode_bitmap(bmp):
    """RGBA8 bytes for a bitmap from find_bitmap (Xbox 360 byte order)."""
    w, h, enc = bmp["width"], bmp["height"], bmp["encoding"]
    data = bmp["data"]
    out = bytearray(w * h * 4)
    if enc == 3:  # 32-bit, stored ARGB big-endian
        for i in range(w * h):
            a, r, g, b = data[i * 4:i * 4 + 4]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
        return bytes(out)
    data = _swap16(data)  # Xbox 360 stores 16-bit words byte-swapped
    block = 8 if enc == 8 else 16
    i = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = data[i:i + block]
            i += block
            if enc == 8:
                _color_block(blk, out, w, bx, by, True)
            elif enc == 24:
                _alpha_block(blk[0:8], out, w, bx, by)
                saved = [out[((by + y) * w + bx + x) * 4 + 3] for y in range(4) for x in range(4)]
                _color_block(blk[8:16], out, w, bx, by, False)
                for k, a in enumerate(saved):
                    out[((by + k // 4) * w + bx + k % 4) * 4 + 3] = a
    return bytes(out)


def font_glyphs(obj):
    """Glyph table of a RndFont (rev 17) object.

    The object ends with: Vector2 tex_cell_size, u32 count, count x
    (u16 char, f32 u, f32 v, f32 width, f32 advance), then the next-font
    reference (u32 length + name). Found by working back from the end.
    Returns (tex_cell_size, {char: (u, v, width, advance)}).
    """
    end = len(obj)
    for name_len in range(0, 128):
        tail = end - 4 - name_len
        if tail < 12:
            break
        if struct.unpack_from(">I", obj, tail)[0] != name_len:
            continue
        for count in range(1, 2048):
            start = tail - count * 18
            if start < 12:
                break
            if struct.unpack_from(">I", obj, start - 4)[0] != count:
                continue
            cell = struct.unpack_from(">2f", obj, start - 12)
            glyphs = {}
            for i in range(count):
                c, u, v, w, adv = struct.unpack_from(">H4f", obj, start + i * 18)
                glyphs[c] = (u, v, w, adv)
            return cell, glyphs
    return None

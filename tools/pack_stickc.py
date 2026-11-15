#!/usr/bin/env python3
"""Make an 8 MB StickC Plus2 sprite pack from the checked-in PMD assets.

Preserves all 151 normal/shiny forms, palette colors, and all available actions.
Bounds each animation to 48x48 and six frames, preserving its full duration.
SPM1: magic, u32[303] absolute offsets, then 302 RLE records (normal, shiny).
Record: u32 decoded length followed by (u8 count, u8 value) runs of TPK2 bytes.
No downloads or additional Python packages are needed.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct

ROOT = Path(__file__).resolve().parents[1]
MAX_SIDE = 48
MAX_FRAMES = 6
MAX_DECODED = 180000


def compact(src):
    if src[:4] != b'TPK2':
        raise ValueError('Not TPK2')
    count, colors = struct.unpack_from('<BH', src, 4)
    pos = 7 + 2 * colors
    out = bytearray(src[:pos])
    for _ in range(count):
        aid, w, h, nf = struct.unpack_from('4B', src, pos)
        pos += 4
        durations = struct.unpack_from(f'<{nf}H', src, pos)
        pos += 2 * nf
        frames = [src[pos + i*w*h:pos + (i+1)*w*h] for i in range(nf)]
        pos += w*h*nf
        if any(len(f) != w*h for f in frames) or not w or not h or not nf:
            raise ValueError('Truncated source')
        # One common crop per action prevents animation jitter.
        occupied = [(j % w, j // w) for f in frames for j, v in enumerate(f) if v != 255]
        if occupied:
            x0 = min(x for x, y in occupied)
            y0 = min(y for x, y in occupied)
            cw = max(x for x, y in occupied) - x0 + 1
            ch = max(y for x, y in occupied) - y0 + 1
        else:
            x0 = y0 = 0
            cw = ch = 1
        scale = min(1, MAX_SIDE / max(cw, ch))
        ow, oh = max(1, round(cw*scale)), max(1, round(ch*scale))
        n = min(nf, MAX_FRAMES)
        starts = [i*nf//n for i in range(n)] + [nf]
        ms = [sum(durations[starts[i]:starts[i+1]]) for i in range(n)]
        out += struct.pack('4B', aid, ow, oh, n)
        out += struct.pack(f'<{n}H', *ms)
        for i in starts[:-1]:
            out += bytes(frames[i][(y0 + min(ch-1, int((y+.5)*ch/oh)))*w
                                    + x0 + min(cw-1, int((x+.5)*cw/ow))]
                         for y in range(oh) for x in range(ow))
    if pos != len(src) or len(out) > MAX_DECODED:
        raise ValueError('Invalid source size / decompression budget exceeded')
    return bytes(out)


def rle(data):
    out = bytearray(struct.pack('<I', len(data)))
    i = 0
    while i < len(data):
        j = i + 1
        while j < len(data) and data[j] == data[i] and j-i < 255:
            j += 1
        out += bytes((j-i, data[i]))
        i = j
    return out


def unpack(record):
    size, = struct.unpack_from('<I', record)
    if len(record) % 2 or size > MAX_DECODED:
        raise ValueError('Invalid RLE record')
    out = bytearray()
    for i in range(4, len(record), 2):
        n, v = record[i:i+2]
        if n == 0 or len(out)+n > size:
            raise ValueError('RLE overflow')
        out += bytes((v,))*n
    if len(out) != size:
        raise ValueError('RLE underflow')
    return bytes(out)


def build(dest):
    records = []
    total_source = 0
    for shiny in ('', 's'):
        for dex in range(1, 152):
            src = (ROOT / f'tools/sdcard/mons/p{shiny}{dex:03}.bin').read_bytes()
            total_source += len(src)
            small = compact(src)
            record = rle(small)
            assert unpack(record) == small
            records.append(record)
    offsets = [4 + 303*4]
    for record in records:
        offsets.append(offsets[-1] + len(record))
    blob = b'SPM1' + struct.pack('<303I', *offsets) + b''.join(records)
    # Leave ample room for LittleFS metadata / allocation overhead.
    if len(blob) > 0x5F0000 - 256*1024:
        raise ValueError(f'Sprite pack exceeds flash budget: {len(blob)}')
    dest.mkdir(parents=True, exist_ok=True)
    (dest / 'monsters.pack').write_bytes(blob)
    report = dict(forms=302, source_bytes=total_source, packed_bytes=len(blob),
                  sha256=hashlib.sha256(blob).hexdigest(), max_side=MAX_SIDE,
                  max_frames=MAX_FRAMES, actions='all available upstream actions')
    (dest / 'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'ports/stickc-plus2/data')
    build(parser.parse_args().out)

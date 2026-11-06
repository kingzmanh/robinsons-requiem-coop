"""Wrap a flat X-32 group image into a minimal PE32 so REA's Windows Ghidra provider
accepts it (REA on Windows is read-only and cannot discover jump-table asm functions,
so every named function is published as a PE export; Ghidra then creates and names it).
VA = 0x00400000 + 0x1000 + flat offset  (flat 0 -> 0x401000).
Usage: python -I tools/wrap_pe.py <in.bin> <out.exe> <dataStartHex> [names.json]
"""
import json, struct, sys

src, dst, data_start = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
names_path = sys.argv[4] if len(sys.argv) > 4 else None
img = open(src, 'rb').read()
FA, SA, BASE = 0x200, 0x1000, 0x400000
TEXT_RVA = 0x1000


def align(x, a):
    return (x + a - 1) // a * a


body_vsize = len(img) + 0x20000            # data + BSS slack
exports = []
if names_path:
    seen = set()
    for r in json.load(open(names_path)):
        n = r['name']
        if r['kind'] != 'code' or n[0] in '@?' or n in seen or r['addr'] >= data_start:
            continue
        seen.add(n)
        exports.append((n, r['addr']))
    exports.sort(key=lambda e: e[0].encode())

sections = [(b'.text', TEXT_RVA, body_vsize, img, 0xE0000060)]
edata_rva = align(TEXT_RVA + body_vsize, SA)
edata = b''
if exports:
    n = len(exports)
    dll = b'rrq_alis.exe\0'
    o_eat = 40
    o_npt = o_eat + 4 * n
    o_ord = o_npt + 4 * n
    o_dll = o_ord + 2 * n
    o_str = o_dll + len(dll)
    strs, eat, npt, ords = b'', b'', b'', b''
    for i, (nm, a) in enumerate(exports):
        eat += struct.pack('<I', TEXT_RVA + a)
        npt += struct.pack('<I', edata_rva + o_str + len(strs))
        ords += struct.pack('<H', i)
        strs += nm.encode('latin1') + b'\0'
    hdr = struct.pack('<IIHHIIIIIII', 0, 0, 0, 0, edata_rva + o_dll, 1, n, n,
                      edata_rva + o_eat, edata_rva + o_npt, edata_rva + o_ord)
    edata = hdr + eat + npt + ords + dll + strs
    sections.append((b'.edata', edata_rva, len(edata), edata, 0x40000040))

hdr_size = align(0x40 + 4 + 20 + 224 + 40 * len(sections), FA)
out = bytearray()
mz = bytearray(0x40); mz[0:2] = b'MZ'; struct.pack_into('<I', mz, 0x3c, 0x40)
out += mz + b'PE\0\0'
out += struct.pack('<HHIIIHH', 0x14c, len(sections), 0, 0, 0, 224, 0x0103 | 0x0100)
last = sections[-1]
image_size = align(last[1] + last[2], SA)
opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII',
                  0x10b, 1, 0, data_start, len(img) - data_start, 0, TEXT_RVA, TEXT_RVA, TEXT_RVA + data_start,
                  BASE, SA, FA, 4, 0, 0, 0, 4, 0, 0, image_size, hdr_size, 0, 3, 0,
                  0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
dirs = bytearray(16 * 8)
if exports:
    struct.pack_into('<II', dirs, 0, edata_rva, len(edata))
out += opt + dirs
raw = hdr_size
raws = []
for name, rva, vsz, data, ch in sections:
    rs = align(len(data), FA)
    out += struct.pack('<8sIIIIIIHHI', name, vsz, rva, rs, raw, 0, 0, 0, 0, ch)
    raws.append((raw, data, rs))
    raw += rs
out += b'\0' * (hdr_size - len(out))
for off, data, rs in raws:
    out += data + b'\0' * (rs - len(data))
open(dst, 'wb').write(out)
print('wrote', dst, hex(len(out)), 'exports', len(exports), 'flat 0 ->', hex(BASE + TEXT_RVA))

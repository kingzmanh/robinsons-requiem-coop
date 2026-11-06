"""Extract the flat 32-bit X-32 group (code+data, base 0) from S.TST (debug build,
CodeView names) and START.EXE (retail build), and resolve CodeView public symbols
to flat offsets. Outputs in analysis/ghidra_in/.
Usage: python -I tools/extract32.py <mod-root>
Layout facts (from the CodeView segmap in S.TST):
  inner MZ image at file 0x5c7; 32-bit group frame 0x426 -> file 0x4827 (S.TST)
  seg3 _TEXT  group+0x00000 size 0x1465f   (32-bit code)
  seg4 _DATA  group+0x14660 size 0x7c48    (initialised data)
  BSS segs follow, group size 0x1c442
  START.EXE: the 16-bit extender part is 0x30 bytes longer -> group at file 0x4857
"""
import json, os, struct, sys

root = sys.argv[1]
orig = os.path.join(root, 'original')
out = os.path.join(root, 'analysis', 'ghidra_in')
os.makedirs(out, exist_ok=True)

TEXT_SIZE = 0x1465f
DATA_BASE = 0x14660


def group(fn, base):
    d = open(os.path.join(orig, fn), 'rb').read()
    end = 0x527 + (struct.unpack_from('<H', d, 0x527 + 4)[0] - 1) * 512 + struct.unpack_from('<H', d, 0x527 + 2)[0]
    return d, d[base:end]


dbg_file, dbg = group('S.TST', 0x4827)
ret_file, ret = group('START.EXE', 0x4857)
open(os.path.join(out, 'rrq_dbg.bin'), 'wb').write(dbg)
open(os.path.join(out, 'rrq_retail.bin'), 'wb').write(ret)

# segment frames from the segmap (seg index -> group-relative base)
j = json.load(open(os.path.join(root, 'analysis', 'symbols.json')))
sm = [e for e in j['dir'] if e[0] == 'SegMap'][0]
b = dbg_file[sm[2]:sm[2] + sm[3]]
cseg = struct.unpack_from('<H', b, 0)[0]
segbase = {}
for i in range(cseg):
    fl, ovl, grp, frame, iname, icls, off, cb = struct.unpack_from('<HHHHHHII', b, 4 + 20 * i)
    segbase[i + 1] = ((frame - 0x426) * 16 + off, cb)

# public symbols (16-bit offsets in this Zortech NB05 -> resolve >64K code wrap)
syms = []
for e in j['dir']:
    if e[0] != 'PublicSym':
        continue
    blk = dbg_file[e[2]:e[2] + e[3]]
    o = 4
    while o + 4 <= len(blk):
        rl, rt = struct.unpack_from('<HH', blk, o)
        if rl == 0:
            break
        ofs, seg, typ = struct.unpack_from('<HHH', blk, o + 4)
        name = blk[o + 11:o + 11 + blk[o + 10]].decode('latin1')
        syms.append((seg, ofs, typ, name, e[1]))
        o += rl + 2

PROLOG = (0x55, 0x53, 0x56, 0x57, 0xc8, 0x8b, 0x66, 0x0f, 0x60, 0x9c, 0x1e, 0x06, 0xb8, 0x83, 0x51, 0x52, 0x50)


def looks_func(a):
    return dbg[a] in PROLOG and (a == 0 or dbg[a - 1] in (0xc3, 0xc2, 0x00, 0x90, 0xcb, 0xcf, 0xe9, 0xeb))


mods = j['modules']
resolved = []
amb = 0
for seg, ofs, typ, name, mod in syms:
    if seg == 3:
        ms = mods.get(str(mod), {}).get('segs', [])
        rng = [(m['off'], m['off'] + m['cb']) for m in ms if m['seg'] == 3]
        cands = [ofs + k for k in (0, 0x10000) if ofs + k < TEXT_SIZE
                 and (not rng or any(lo <= ofs + k < hi for lo, hi in rng))]
        if not cands:
            cands = [ofs + k for k in (0, 0x10000) if ofs + k < TEXT_SIZE]
        good = [c for c in cands if looks_func(c)]
        if len(cands) > 1:
            amb += 1
        addr = (good or cands)[0] if len(good) != 2 else cands[0]
        kind = 'code'
    elif seg in segbase and seg >= 4:
        addr = segbase[seg][0] + ofs
        kind = 'data'
    else:
        continue  # 16-bit extender / absolute
    resolved.append({'addr': addr, 'name': name, 'kind': kind, 'module': mods.get(str(mod), {}).get('name', ''),
                     'alt': [c for c in cands if c != addr] if kind == 'code' else []})
resolved.sort(key=lambda r: r['addr'])
json.dump(resolved, open(os.path.join(out, 'names_dbg.json'), 'w'), indent=0)
with open(os.path.join(out, 'names_dbg.txt'), 'w') as f:
    for r in resolved:
        f.write(f"{r['addr']:08x} {r['kind']} {r['name']} {r['module']}\n")
print('dbg group', hex(len(dbg)), 'retail group', hex(len(ret)))
print('symbols', len(resolved), 'code', sum(r['kind'] == 'code' for r in resolved), 'ambiguous-wrap', amb)
print('segbase', {k: (hex(v[0]), hex(v[1])) for k, v in segbase.items()})

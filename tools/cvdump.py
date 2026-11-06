"""Parse the CodeView (NB05) debug info appended to S.TST (Zortech build of the
ALIS interpreter). Writes analysis/symbols.json and analysis/symbols.txt.
Usage: python -I tools/cvdump.py original/S.TST analysis
"""
import json, os, struct, sys

SST = {0x120: 'Module', 0x121: 'Types', 0x122: 'Public', 0x123: 'PublicSym',
       0x124: 'Symbols', 0x125: 'AlignSym', 0x126: 'SrcLnSeg', 0x127: 'SrcModule',
       0x128: 'Libraries', 0x129: 'GlobalSym', 0x12a: 'GlobalPub', 0x12b: 'GlobalTypes',
       0x12c: 'MPC', 0x12d: 'SegMap', 0x12e: 'SegName'}


def pstr(b, o):
    n = b[o]
    return b[o + 1:o + 1 + n].decode('latin1'), o + 1 + n


def main(path, outdir):
    d = open(path, 'rb').read()
    assert d[-8:-4] in (b'NB05', b'NB09'), d[-8:]
    base = len(d) - struct.unpack_from('<I', d, len(d) - 4)[0]
    sig, lfodir = d[base:base + 4], struct.unpack_from('<I', d, base + 4)[0]
    dh = base + lfodir
    cbhdr, cbent, cdir = struct.unpack_from('<HHI', d, dh)
    ents = []
    for i in range(cdir):
        st, imod, lfo, cb = struct.unpack_from('<HHII', d, dh + cbhdr + i * cbent)
        ents.append([SST.get(st, hex(st)), imod, base + lfo, cb])
    # Zortech NB05 stores cb as 16 bits: fix sizes that wrapped using the next entry's offset
    order = sorted(ents, key=lambda e: e[2])
    for a, b2 in zip(order, order[1:]):
        while a[3] + 0x10000 <= b2[2] - a[2]:
            a[3] += 0x10000
    modules, syms, lines = {}, [], []
    for kind, imod, off, cb in ents:
        blk = d[off:off + cb]
        if kind == 'Module':
            ovl, ilib, cseg, style = struct.unpack_from('<HHHH', blk, 0)
            segs = [struct.unpack_from('<HHII', blk, 8 + 12 * k) for k in range(cseg)]
            name, _ = pstr(blk, 8 + 12 * cseg)
            modules[imod] = {'name': name, 'segs': [{'seg': s, 'off': o, 'cb': c} for s, _, o, c in segs]}
        elif kind in ('Public', 'PublicSym', 'Symbols', 'GlobalPub', 'GlobalSym'):
            o = 4 if kind in ('Symbols',) and blk[:4] == b'\x01\x00\x00\x00' else 0
            if kind in ('GlobalPub', 'GlobalSym'):
                o = 8
            while o + 4 <= len(blk):
                rl, rt = struct.unpack_from('<HH', blk, o)
                if rl == 0:
                    break
                r = blk[o + 4:o + 2 + rl]
                if rt in (0x0103, 0x0102, 0x0101):  # PUB16/GDATA16/LDATA16
                    ofs, seg, typ = struct.unpack_from('<HHH', r, 0)
                    nm, _ = pstr(r, 6)
                    syms.append({'kind': {0x103: 'pub', 0x102: 'gdata', 0x101: 'ldata'}[rt], 'seg': seg,
                                 'off': ofs, 'name': nm, 'mod': imod})
                elif rt in (0x0203, 0x0202, 0x0201):  # 32-bit variants
                    ofs, seg, typ = struct.unpack_from('<IHH', r, 0)
                    nm, _ = pstr(r, 8)
                    syms.append({'kind': {0x203: 'pub', 0x202: 'gdata', 0x201: 'ldata'}[rt], 'seg': seg,
                                 'off': ofs, 'name': nm, 'mod': imod})
                elif rt in (0x0104, 0x0105):  # LPROC16/GPROC16
                    pp, pe, pn, ln, ds, de, ofs, seg, typ, flg = struct.unpack_from('<IIIHHHHHHB', r, 0)
                    nm, _ = pstr(r, 27)
                    syms.append({'kind': 'proc', 'seg': seg, 'off': ofs, 'len': ln, 'name': nm, 'mod': imod})
                elif rt in (0x0204, 0x0205):  # LPROC32/GPROC32
                    pp, pe, pn, ln, ds, de, ofs, seg, typ, flg = struct.unpack_from('<IIIIIIIHHB', r, 0)
                    nm, _ = pstr(r, 35)
                    syms.append({'kind': 'proc', 'seg': seg, 'off': ofs, 'len': ln, 'name': nm, 'mod': imod})
                o += rl + 2
        elif kind == 'SrcModule':
            lines.append({'mod': imod, 'cb': cb})
    seen, uniq = set(), []
    for s in sorted(syms, key=lambda s: (s['seg'], s['off'], s['kind'] != 'proc')):
        k = (s['seg'], s['off'], s['name'])
        if k not in seen:
            seen.add(k); uniq.append(s)
    os.makedirs(outdir, exist_ok=True)
    json.dump({'sig': sig.decode(), 'dir': [list(e) for e in ents], 'modules': modules, 'symbols': uniq},
              open(os.path.join(outdir, 'symbols.json'), 'w'), indent=1)
    with open(os.path.join(outdir, 'symbols.txt'), 'w') as f:
        for s in uniq:
            m = modules.get(s['mod'], {}).get('name', '')
            f.write(f"{s['seg']:04x}:{s['off']:08x} {s['kind']:5} {s['name']:32} {m}\n")
    from collections import Counter
    print('sig', sig, 'subsections', Counter(e[0] for e in ents))
    print('modules', len(modules), 'symbols', len(uniq), Counter(s['kind'] for s in uniq),
          'segs', Counter(s['seg'] for s in uniq))
    for i, m in list(modules.items())[:60]:
        print(i, m['name'], m['segs'])


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])

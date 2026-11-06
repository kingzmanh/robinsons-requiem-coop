"""Transfer CodeView names from the debug interpreter (S.TST) to the retail one
(START.EXE) by masked byte-pattern matching (abs addresses and rel32 masked).
Usage: python -I tools/transfer_names.py <mod-root>
Writes analysis/ghidra_in/names_retail.json/.txt
"""
import json, os, re, sys

root = sys.argv[1]
gi = os.path.join(root, 'analysis', 'ghidra_in')
dbg = open(os.path.join(gi, 'rrq_dbg.bin'), 'rb').read()
ret = open(os.path.join(gi, 'rrq_retail.bin'), 'rb').read()
names = json.load(open(os.path.join(gi, 'names_dbg.json')))
D_DATA, R_DATA = 0x14660, 0x14d80


def pattern(buf, a, n=28):
    b = buf[a:a + n]
    out, i = [], 0
    while i < len(b):
        # rel32 after call/jmp
        if b[i] in (0xe8, 0xe9) and i + 5 <= len(b):
            out.append(re.escape(b[i:i + 1]) + b'....'); i += 5; continue
        # absolute address dword (group-relative, < 0x40000)
        if i + 4 <= len(b) and b[i + 3] == 0 and b[i + 2] <= 3 and (b[i + 2] or b[i + 1]):
            out.append(b'....'); i += 4; continue
        out.append(re.escape(b[i:i + 1])); i += 1
    return re.compile(b''.join(out), re.S)


res, miss, amb = [], 0, 0
code = [n for n in names if n['kind'] == 'code']
last_delta = 0
for n in sorted(code, key=lambda x: x['addr']):
    a = n['addr']
    if a + 28 > D_DATA:
        continue
    hits = [m.start() for m in pattern(dbg, a).finditer(ret[:R_DATA])]
    if len(hits) > 1:
        # prefer the hit closest to the running delta
        hits.sort(key=lambda h: abs((h - a) - last_delta))
        if abs((hits[0] - a) - last_delta) < 0x400 and abs((hits[1] - a) - last_delta) > 0x400:
            hits = hits[:1]
        else:
            amb += 1; continue
    if not hits:
        miss += 1; continue
    last_delta = hits[0] - a
    res.append({**n, 'addr': hits[0], 'dbg_addr': a, 'alt': []})

# interpolate: unmatched labels between two matched neighbours with the same delta
import bisect
got = sorted((r['dbg_addr'], r['addr'] - r['dbg_addr']) for r in res)
keys = [g[0] for g in got]
have = {r['dbg_addr'] for r in res}
interp = 0
for n in code:
    a = n['addr']
    if a in have or a >= D_DATA:
        continue
    i = bisect.bisect_left(keys, a)
    if 0 < i < len(keys) and got[i - 1][1] == got[i][1]:
        res.append({**n, 'addr': a + got[i][1], 'dbg_addr': a, 'alt': [], 'interp': True}); interp += 1
print('interpolated', interp)

# data labels: map via sequential data delta, verified on bytes where possible
dd = R_DATA - D_DATA
for n in names:
    if n['kind'] == 'data':
        res.append({**n, 'addr': n['addr'] + dd if n['addr'] < 0x1c2a8 else n['addr'] + (len(ret) - len(dbg)),
                    'dbg_addr': n['addr'], 'alt': [], 'approx': True})
res.sort(key=lambda r: r['addr'])
json.dump(res, open(os.path.join(gi, 'names_retail.json'), 'w'), indent=0)
with open(os.path.join(gi, 'names_retail.txt'), 'w') as f:
    for r in res:
        f.write(f"{r['addr']:08x} {r['kind']} {r['name']} dbg={r['dbg_addr']:08x}\n")
print('code matched', sum(r['kind'] == 'code' for r in res), 'of', len(code), 'missing', miss, 'ambiguous', amb)

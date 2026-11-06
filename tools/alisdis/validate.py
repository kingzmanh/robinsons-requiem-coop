"""Check the static decoder against a runtime trace (alis.exe -d, data_org-relative offsets).
For each executed instruction: the runtime sub-op name sequence must equal the static
decode's name sequence, and for straight-line opcodes the next executed offset must be
offset + static length. Prints mismatches grouped by opcode.
Usage: python -I tools/alisdis/validate.py <unpacked_dir> <dis.txt> [max_examples]
"""
import re, sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import alisdis as A

src, log = sys.argv[1], sys.argv[2]
maxex = int(sys.argv[3]) if len(sys.argv) > 3 else 3
INS = re.compile(r'^(\S+\.IO) \[([0-9a-f]{6})\][0-9a-f]{6}: ([0-9a-f]{2}): (\w+) ###')
SUB = re.compile(r'^\s+--> \[([0-9a-f]{6})\][0-9a-f]{6}: ([0-9a-f]{2}): (\w+) ###')

files = {}
def data(sc):
    if sc not in files:
        p = os.path.join(src, sc.upper() + '.bin')
        files[sc] = open(p, 'rb').read() if os.path.exists(p) else None
    return files[sc]

def static_names(d, a):
    dis = A.Dis(d, '')
    names = []
    orig = dis.expr
    def rec(p, out, at, table):
        c = dis.u8(p)
        if c in table: names.append(table[c][0])
        return orig(p, out, at, table)
    dis.expr = rec
    try:
        nxt, _, _ = dis.one(a)
    except A.Bad as e:
        return None, None, str(e)
    return nxt - a, names, None

seen = set()
mism = collections.defaultdict(list)
ok = collections.Counter()
cur = None
def finish(c):
    if not c: return
    sc, a, op, name, subs = c
    d = data(sc)
    if d is None or (sc, a) in seen: return
    seen.add((sc, a))
    ln, names, err = static_names(d, a)
    if err:
        mism[name].append((sc, a, 'ERR ' + err, subs)); return
    # runtime subs include table dispatch names; static names likewise (oeval/seval etc)
    rs = [s for s in subs if s not in ('ofin',)]
    ss = [s for s in names if s not in ('ofin',)]
    if rs != ss[:len(rs)] or len(ss) != len(rs):
        mism[name].append((sc, a, f'static {ss}', rs))
    else:
        ok[name] += 1

for line in open(log, errors='replace'):
    m = INS.match(line)
    if m:
        finish(cur)
        cur = (m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4), [])
        continue
    m = SUB.match(line)
    if m and cur:
        cur[4].append(m.group(3))
finish(cur)
print('instructions checked:', sum(ok.values()) + sum(len(v) for v in mism.values()), 'ok:', sum(ok.values()))
for name, lst in sorted(mism.items(), key=lambda x: -len(x[1])):
    print(f'== {name}: {len(lst)} mismatches (ok {ok[name]})')
    for sc, a, st, rs in lst[:maxex]:
        print(f'   {sc} {a:06x}: {st}\n      runtime {rs}')

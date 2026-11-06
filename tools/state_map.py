"""Build analysis/save_state_map.tsv: everything the game writes to a savegame (BARATINE save
routine: cfwritev singles + cfwriteb blocks), annotated with debug-screen labels, cdim table
layout and the scripts that statically read/write each item.
Usage: python -I tools/state_map.py <mod-root>
"""
import csv, os, re, sys, collections

root = sys.argv[1]
st = os.path.join(root, 'analysis', 'scripts_static')
lst = open(os.path.join(st, 'BARATINE.IO.lst'), encoding='utf-8').read()
m = re.search(r'0047a7: cfopen.*?\n(.*?)L004a02', lst, re.S)
seg = m.group(1)
items = []
for mm in re.finditer(r'cfwritev omain(b|w)\[0x([0-9a-f]+)\]', seg):
    items.append(('single', int(mm.group(2), 16), 1 if mm.group(1) == 'b' else 2))
for mm in re.finditer(r'cfwriteb main\[0x([0-9a-f]+)\] 0x([0-9a-f]+)', seg):
    items.append(('block', int(mm.group(1), 16), int(mm.group(2), 16)))

labels = {}
for r in csv.DictReader(open(os.path.join(root, 'analysis', 'hero_vars_debugscreen.tsv')), delimiter='\t'):
    v = re.match(r'main\.(\w+)\[0x([0-9a-f]+)\](?:\[(\d+)\])?', r['variable'])
    if v:
        labels.setdefault(int(v.group(2), 16), []).append(r['label'] + (f'[{v.group(3)}]' if v.group(3) else ''))

acc = collections.defaultdict(lambda: {'read': set(), 'write': set(), 'add': set()})
for r in csv.DictReader(open(os.path.join(st, 'mainvars.tsv')), delimiter='\t'):
    o = int(r['offset'], 16)
    for k in ('read', 'write', 'add'):
        acc[o][k.replace('read', 'read')] |= set(x for x in r[k + ('ers' if k != 'add' else 'ers')].split() if x) if False else set()
for r in csv.DictReader(open(os.path.join(st, 'mainvars.tsv')), delimiter='\t'):
    o = int(r['offset'], 16)
    acc[o]['read'] |= set(r['readers'].split())
    acc[o]['write'] |= set(r['writers'].split())
    acc[o]['add'] |= set(r['adders'].split())

dims = {}
for r in csv.DictReader(open(os.path.join(st, 'dims.tsv')), delimiter='\t'):
    if r['script'] == 'MAIN.IO':
        dims[int(r['offset'], 16)] = (r['counter'], r['byte2'])

def who(lo, hi):
    w, rd = set(), set()
    for o, a in acc.items():
        if lo <= o < hi:
            w |= a['write'] | a['add']; rd |= a['read']
    return w, rd

out = os.path.join(root, 'analysis', 'save_state_map.tsv')
with open(out, 'w') as f:
    f.write('kind\toffset\tsize\tlabels\tmain_cdim\twriters\treaders\n')
    for kind, o, sz in items:
        lab = []
        for k in range(o, o + sz):
            lab += labels.get(k, [])
        d = dims.get(o + sz, '') if kind == 'block' else ''
        w, rd = who(o, o + sz)
        f.write(f'{kind}\t{o:04x}\t{sz}\t{",".join(lab)}\t{d}\t{" ".join(sorted(w))}\t{" ".join(sorted(rd))}\n')
print(out, len(items), 'items', sum(s for _, _, s in items), 'bytes')

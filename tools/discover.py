"""Turn the engine's runtime disassembly (alis.exe -d) into
  analysis/scripts_dis/<script>.lst   every executed instruction, sorted by address
  analysis/script_mainvars.tsv        which script reads / writes / adds each main variable
Usage: python -I tools/discover.py <out_dir> <dis.txt> [more dis.txt ...]
Line formats:
  'heros.IO [06fa03]06f9e5: 1f: ceval ### ...'
  '      --> [06fa04]06f9e6: 00: oimmb ###  0x00'
Operand prefix -> access: o* read, s* store, a* add (main = basemain-relative offset).
"""
import collections, os, re, sys

out = sys.argv[1]
logs = sys.argv[2:]
INS = re.compile(r'^(\S+\.IO) \[([0-9a-f]{6})\]([0-9a-f]{6}): ([0-9a-f]{2}): (\w+) ###(.*)$')
SUB = re.compile(r'^\s+--> \[([0-9a-f]{6})\]([0-9a-f]{6}): ([0-9a-f]{2}): (\w+) ###(.*)$')
MAINOP = re.compile(r'^([osa])(main|him)(b|w|p|tc|ti|tp)$')

listing = collections.defaultdict(dict)       # script -> addr -> text
access = collections.defaultdict(lambda: collections.defaultdict(set))  # (off) -> kind -> scripts
count = collections.Counter()
for path in logs:
    cur = None
    for line in open(path, errors='replace'):
        m = INS.match(line)
        if m:
            sc, a, rel, op, name, rest = m.groups()
            cur = (sc, a)
            listing[sc].setdefault(a, [f'{a} {op} {name}{rest.rstrip()}'])
            count[sc] += 1
            continue
        m = SUB.match(line)
        if m and cur:
            a, rel, op, name, rest = m.groups()
            lst = listing[cur[0]][cur[1]]
            txt = f'    {a} {op} {name}{rest.rstrip()}'
            if txt not in lst and len(lst) < 40:
                lst.append(txt)
            mm = MAINOP.match(name)
            if mm and mm.group(2) == 'main':
                v = re.search(r'0x([0-9a-f]{1,4})', rest)
                if v:
                    kind = {'o': 'read', 's': 'write', 'a': 'add'}[mm.group(1)]
                    off = int(v.group(1), 16)
                    access[(off, mm.group(3))][kind].add(cur[0])

os.makedirs(os.path.join(out, 'scripts_dis'), exist_ok=True)
for sc, ins in listing.items():
    with open(os.path.join(out, 'scripts_dis', sc + '.lst'), 'w') as f:
        f.write(f'; {sc}: {len(ins)} distinct executed instructions (runtime coverage only)\n')
        for a in sorted(ins):
            f.write('\n'.join(ins[a]) + '\n')
with open(os.path.join(out, 'script_mainvars.tsv'), 'w') as f:
    f.write('offset\ttype\treaders\twriters\tadders\n')
    for (off, ty) in sorted(access):
        k = access[(off, ty)]
        f.write(f'{off:04x}\t{ty}\t{" ".join(sorted(k["read"]))}\t{" ".join(sorted(k["write"]))}\t{" ".join(sorted(k["add"]))}\n')
print('scripts:', {s: len(v) for s, v in sorted(listing.items(), key=lambda x: -len(x[1]))})
print('main var refs:', len(access))

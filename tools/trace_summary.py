"""Summarise an RRQ_TRACE_MAIN trace: per main-variable offset -> writers, write count,
which marked phases touched it, and the debug-screen label if known.
Usage: python -I tools/trace_summary.py <trace.tsv> [<more traces>...] --out <summary.tsv>
"""
import csv, sys, os, collections

args = sys.argv[1:]
out = args[args.index('--out') + 1] if '--out' in args else None
traces = [a for a in args if a != '--out' and a != out]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
labels = {}
lab_path = os.path.join(root, 'analysis', 'hero_vars_debugscreen.tsv')
if os.path.exists(lab_path):
    for r in csv.DictReader(open(lab_path), delimiter='\t'):
        v = r['variable']
        if v.startswith('main.'):
            try:
                off = int(v.split('[0x')[1][:4], 16)
                labels.setdefault(off, r['label'])
            except Exception:
                pass

writers = collections.defaultdict(collections.Counter)
phases = collections.defaultdict(set)
lastval = {}
for t in traces:
    phase = 'start'
    for line in open(t):
        if line.startswith('#MARK'):
            phase = line.rstrip('\n').split('\t')[2]; continue
        if line.startswith('tick'):
            continue
        p = line.rstrip('\n').split('\t')
        if len(p) < 7:
            continue
        tick, script, vram, off, ln, old, new = p
        off = int(off, 16); ln = int(ln)
        for k in range(ln):
            o = off + k
            if old[2 * k:2 * k + 2] == new[2 * k:2 * k + 2]:
                continue
            writers[o][script] += 1
            phases[o].add(phase)
            lastval[o] = new[2 * k:2 * k + 2]

rows = []
for o in sorted(writers):
    rows.append((o, labels.get(o & ~1, labels.get(o, '')), sum(writers[o].values()),
                 ' '.join(f'{s}:{c}' for s, c in writers[o].most_common(6)), ' '.join(sorted(phases[o]))))
if out:
    with open(out, 'w') as f:
        f.write('offset\tlabel\twrites\twriters\tphases\n')
        for r in rows:
            f.write(f'{r[0]:04x}\t{r[1]}\t{r[2]}\t{r[3]}\t{r[4]}\n')
byw = collections.Counter()
for o in writers:
    for s in writers[o]:
        byw[s] += 1
print('bytes changed:', len(writers))
print('bytes per writer script:', byw.most_common(30))

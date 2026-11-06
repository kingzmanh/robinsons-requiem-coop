"""Static disassembler for ALIS v3.x (PC, little-endian) scripts, e.g. Robinson's Requiem.
Recursive descent from the header entry points, following jumps, calls, switch tables
and cstart targets. Operand specs: specs_auto.json (from the engine C source) + the
OVERRIDES below (checked against runtime traces with tools/alisdis/validate.py).

Usage: python -I tools/alisdis/alisdis.py <unpacked_dir> <out_dir>
Writes <out_dir>/<NAME>.lst and <out_dir>/mainvars.tsv (static main-variable access map).
"""
import json, os, struct, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
AUTO = json.load(open(os.path.join(HERE, 'specs_auto.json')))


def tab(name):
    return {int(k): (v[0], list(v[1])) for k, v in AUTO[name].items()}


OPC, OPER, STOR, ADDN = tab('opcodes'), tab('opernames'), tab('storenames'), tab('addnames')
ESC1 = tab('codesc1')
ESC1[6] = ('copenfilm', ['E', 'E', 'E', 'E', 'S'])

# --- overrides for v3.1 PC (version-specific branches removed) ---------------------------
FLOW = {  # opcode: (kind, width)   kind: jmp/jsr/bcond/loop/start
    0x05: ('jsr', 1), 0x06: ('jsr', 2), 0x07: ('jsr', 3),
    0x08: ('jmp', 1), 0x09: ('jmp', 2), 0x0a: ('jmp', 3),
    0x12: ('bcond', 1), 0x13: ('bcond', 2), 0x14: ('bcond', 3),
    0x15: ('bcond', 1), 0x16: ('bcond', 2), 0x17: ('bcond', 3),
    0x18: ('bcond', 1), 0x19: ('bcond', 2), 0x1a: ('bcond', 3),
    0x1b: ('bcond', 1), 0x1c: ('bcond', 2), 0x1d: ('bcond', 3),
    0x2b: ('loop', 1), 0x2c: ('loop', 2), 0x2d: ('loop', 3),
    0x30: ('start', 1), 0x31: ('start', 2), 0x32: ('start', 3),
}
END = {0x11, 0x44, 0x43}            # cret, cexit, cstopret end a path (cstop resumes next tick)
OPC_OVR = {
    0x29: ['?cdim'], 0x45: ['E', 'w', 'Z', 'Z', 'E'],
}
OPER_OVR = {0x04: ['Z'], 0x2a: ['w', 'w'], 0x2c: ['w', 'w'], 0x2e: ['w', 'w'],
            0x32: ['w', 'w'], 0x34: ['w', 'w'], 0x30: ['w', 'w'], 0x3a: []}
STOR_OVR = {0x2a: ['w', 'w'], 0x2c: ['w', 'w'], 0x2e: ['w', 'w'], 0x30: ['w', 'w'], 0x32: ['w', 'w'], 0x34: ['w', 'w']}
ADD_OVR = {0x2a: ['w', 'w'], 0x2c: ['w', 'w'], 0x2e: ['w', 'w'], 0x30: ['w', 'w'], 0x32: ['w', 'w'], 0x34: ['w', 'w']}
ovr_path = os.path.join(HERE, 'specs_override.json')
if os.path.exists(ovr_path):
    o = json.load(open(ovr_path))
    for k, v in o.get('opcodes', {}).items():
        OPC_OVR[int(k, 16)] = v
    for k, v in o.get('opernames', {}).items():
        OPER_OVR[int(k, 16)] = v
for k, v in OPER_OVR.items():
    if k in OPER:
        OPER[k] = (OPER[k][0], v)
for k, v in STOR_OVR.items():
    if k in STOR:
        STOR[k] = (STOR[k][0], v)
for k, v in ADD_OVR.items():
    if k in ADDN:
        ADDN[k] = (ADDN[k][0], v)
for k, v in OPC_OVR.items():
    if k in OPC:
        OPC[k] = (OPC[k][0], v)

MAINREF = {'omainb', 'omainw', 'omainp', 'omaintc', 'omainti', 'omaintp',
           'smainb', 'smainw', 'smainp', 'smaintc', 'smainti', 'smaintp',
           'amainb', 'amainw', 'amainp', 'amaintc', 'amainti', 'amaintp'}


class Bad(Exception):
    pass


class Dis:
    def __init__(self, data, name):
        self.d, self.name = data, name
        self.ins = {}            # addr -> (len, text, refs)
        self.todo = []
        self.labels = collections.Counter()
        self.bad = []
        self.mainrefs = []       # (addr, opname, off)
        self.dims = []
        self.fileblocks = []     # (addr, op, main_off, length) for cfreadb/cfwriteb

    def u8(self, p):
        if p >= len(self.d): raise Bad('eof')
        return self.d[p]

    def s16(self, p):
        if p + 2 > len(self.d): raise Bad('eof')
        return struct.unpack_from('<h', self.d, p)[0]

    def u16(self, p):
        if p + 2 > len(self.d): raise Bad('eof')
        return struct.unpack_from('<H', self.d, p)[0]

    def operand(self, p, tok, out, at):
        if tok == 'b': out.append(f'{self.u8(p):#04x}'); return p + 1
        if tok == 'FNAME':   # 0xff + expression, else literal zero-terminated name
            if self.u8(p) == 0xff:
                p = self.expr(p + 1, out, at, OPER)
                return self.expr(p, out, at, OPER)
            p = self.operand(p, 'Z', out, at)
            return self.operand(p, 'w', out, at)
        if tok == 'R32':
            out.append(self.d[p:p + 32].hex()); return p + 32
        if tok == 'V':   # read_vaddr: own vram offset, or 0 + main offset
            v = self.u16(p); p += 2
            if v == 0:
                v = self.u16(p); p += 2
                out.append(f'main[{v:#06x}]'); self.mainrefs.append((at, 'vmain', v)); self.lastv = v
            else:
                out.append(f'loc[{v:#06x}]'); self.lastv = None
            return p
        if tok == 'CDIM':
            off = self.u16(p); cnt = self.u8(p + 2); b2 = self.u8(p + 3); p += 4
            n = cnt & 0xf
            p += (n * 2 + 4) if cnt < 0x80 else (n * 4 + 4)
            out.append(f'{off:#06x} cnt={cnt:#04x} {b2:#04x}'); self.dims.append((off, cnt, b2)); return p
        if tok == 'CHART':
            v = self.u16(p); p += 2
            if v == 0: v = self.u16(p); p += 2
            sub = []
            q = p
            p = self.expr(p, sub, at, OPER)
            out.append(f'{v:#06x} ' + ' '.join(sub))
            if self.d[q] != 0x00: raise Bad('cchartmap non-immediate selector')
            val = self.d[q + 1]
            if val == 0: return p
            if val == 1: out.append(f'{self.u16(p):#06x}'); return p + 2
            if val == 5: raise Bad('cchartmap mode 5')
            return self.expr(p, out, at, OPER)
        if tok == 'W0':
            v = self.u16(p); p += 2
            if v == 0: v = self.u16(p); p += 2
            out.append(f'{v:#06x}'); return p
        if tok == 'w': out.append(f'{self.u16(p):#06x}'); return p + 2
        if tok == 't': out.append(f'{self.s16(p):+#x}'); return p + 3
        if tok == 'l': out.append(f'{struct.unpack_from("<I", self.d, p)[0]:#x}'); return p + 4
        if tok == 'Z':
            e = self.d.find(b'\0', p)
            if e < 0: raise Bad('Z eof')
            out.append(repr(self.d[p:e].decode('latin1'))); return e + 1
        if tok == 'E': return self.expr(p, out, at, OPER)
        if tok == 'S': return self.expr(p, out, at, STOR)
        if tok == 'A': return self.expr(p, out, at, ADDN)
        if tok == 'C1':
            c = self.u8(p)
            if c not in ESC1: raise Bad(f'codesc1 {c:#x}')
            nm, sp = ESC1[c]; out.append(nm); p += 1
            for t in sp: p = self.operand(p, t, out, at)
            return p
        raise Bad(f'token {tok}')

    def expr(self, p, out, at, table):
        c = self.u8(p)
        if c not in table: raise Bad(f'oper {c:#x} not in table')
        nm, sp = table[c]
        p += 1
        if c == 0x38:  # oeval / seval / aeval: loop until ofin
            sub = []
            n = 0
            while True:
                if self.u8(p) == 0x3a:
                    p += 1; break
                p = self.expr(p, sub, at, OPER)
                n += 1
                if n > 64: raise Bad('eval runaway')
            out.append('(' + ' '.join(sub) + ')')
            if table is STOR: return self.expr(p, out, at, STOR)
            if table is ADDN: return self.expr(p, out, at, ADDN)
            return p
        args = []
        for t in sp:
            p = self.operand(p, t, args, at)
        if nm in MAINREF and args:
            self.mainrefs.append((at, nm, int(args[0], 16)))
        out.append(nm + ('[' + ','.join(args) + ']' if args else ''))
        return p

    def one(self, a):
        op = self.u8(a)
        if op not in OPC: raise Bad(f'opcode {op:#x}')
        nm, sp = OPC[op]
        p = a + 1
        args, targets, stop = [], [], False
        if op in FLOW:
            kind, w = FLOW[op]
            off = self.u8(p) - 256 * (self.u8(p) > 127) if w == 1 else self.s16(p)
            p += w
            if kind == 'loop':
                tgt = p + off
                p = self.expr(p, args, a, ADDN)
            else:
                tgt = p + off
            args.insert(0, f'-> {tgt:#06x}')
            targets.append(tgt)
            if kind == 'jmp': stop = True
            self.labels[tgt] += 1
        elif nm == 'csend':
            n = self.u8(p); p += 1
            for i in range(n + 2):
                p = self.expr(p, args, a, OPER)
        elif nm == 'cswitch1':
            p = self.expr(p, args, a, OPER)
            n = self.u8(p); p += 1
            # alignment: data_org parity unknown; RRQ loads keep even data_org (validated)
            if p & 1: p += 1
            for i in range(n + 1):
                v = self.u16(p); o = self.s16(p + 2)
                tgt = p + 4 + o
                targets.append(tgt); self.labels[tgt] += 1
                args.append(f'{v:#x}->{tgt:#06x}')
                p += 4
        elif nm == 'cswitch2':
            p = self.expr(p, args, a, OPER)
            n = self.u8(p); p += 1
            if p & 1: p += 1
            base = self.s16(p); p += 2
            args.append(f'base {base}')
            for i in range(n + 1):
                o = self.s16(p)
                tgt = p + 2 + o
                targets.append(tgt); self.labels[tgt] += 1
                args.append(f'{i}->{tgt:#06x}')
                p += 2
        else:
            self.lastv = None
            for t in sp:
                if t.startswith('?'): raise Bad(f'unspecified opcode {nm}')
                p = self.operand(p, t, args, a)
            if nm in ('cfreadb', 'cfwriteb') and self.lastv is not None:
                self.fileblocks.append((a, nm, self.lastv, int(args[-1], 16)))
        if op in END: stop = True
        self.ins[a] = (p - a, f'{a:06x}: {nm} ' + ' '.join(args))
        return p, targets, stop

    def run(self, entries):
        self.todo = list(entries)
        seen = set()
        while self.todo:
            a = self.todo.pop()
            while 0 <= a < len(self.d) and a not in self.ins:
                try:
                    nxt, tg, stop = self.one(a)
                except Bad as e:
                    self.bad.append((a, str(e))); break
                for t in tg:
                    if t not in self.ins and t not in seen:
                        seen.add(t); self.todo.append(t)
                if stop: break
                a = nxt


def entries(d):
    code = struct.unpack_from('<H', d, 2)[0] + 2
    e = [code]
    for off in (6, 10):
        v = struct.unpack_from('<i', d, off)[0]
        if v: e.append(off + v)
    return e


def main(src, out):
    os.makedirs(out, exist_ok=True)
    allrefs = collections.defaultdict(lambda: collections.defaultdict(set))
    summary = []
    alldims, allblocks = [], []
    for fn in sorted(os.listdir(src)):
        if not fn.endswith('.IO.bin'): continue
        d = open(os.path.join(src, fn), 'rb').read()
        name = fn[:-4]
        dis = Dis(d, name)
        try:
            ent = entries(d)
        except struct.error:
            continue
        dis.run(ent)
        covered = sum(l for l, _ in dis.ins.values())
        with open(os.path.join(out, name + '.lst'), 'w', encoding='utf-8') as f:
            f.write(f'; {name}: {len(dis.ins)} instructions, {covered}/{len(d)} bytes, entries {[hex(x) for x in ent]}\n')
            for a, msg in dis.bad[:50]:
                f.write(f'; !! {a:06x}: {msg}\n')
            for a in sorted(dis.ins):
                if dis.labels[a]: f.write(f'L{a:06x}:\n')
                f.write('  ' + dis.ins[a][1] + '\n')
        for at, nm, off in dis.mainrefs:
            kind = {'o': 'read', 's': 'write', 'a': 'add', 'v': 'read'}[nm[0]]
            allrefs[(off, nm[5:])][kind].add(name)
        summary.append((name, len(dis.ins), covered, len(d), len(dis.bad)))
        for off, cnt, b2 in dis.dims: alldims.append((name, off, cnt, b2))
        for a, nm, off, ln in dis.fileblocks: allblocks.append((name, a, nm, off, ln))
    with open(os.path.join(out, 'mainvars.tsv'), 'w') as f:
        f.write('offset\ttype\treaders\twriters\tadders\n')
        for (off, ty) in sorted(allrefs):
            k = allrefs[(off, ty)]
            f.write(f'{off:04x}\t{ty}\t{" ".join(sorted(k["read"]))}\t{" ".join(sorted(k["write"]))}\t{" ".join(sorted(k["add"]))}\n')
    with open(os.path.join(out, 'summary.tsv'), 'w') as f:
        f.write('script\tinstructions\tcode_bytes\tfile_bytes\tdecode_errors\n')
        for r in summary: f.write('\t'.join(map(str, r)) + '\n')
    with open(os.path.join(out, 'dims.tsv'), 'w') as f:
        f.write('script\toffset\tcounter\tbyte2\n')
        for r in alldims: f.write(f'{r[0]}\t{r[1]:04x}\t{r[2]:#04x}\t{r[3]:#04x}\n')
    with open(os.path.join(out, 'fileblocks.tsv'), 'w') as f:
        f.write('script\tat\top\tmain_offset\tlength\n')
        for r in allblocks: f.write(f'{r[0]}\t{r[1]:06x}\t{r[2]}\t{r[3]:04x}\t{r[4]}\n')
    tot_e = sum(r[4] for r in summary)
    print(f'{len(summary)} scripts, {sum(r[1] for r in summary)} instructions, decode errors {tot_e}, main var refs {len(allrefs)}')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])

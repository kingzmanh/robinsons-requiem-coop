"""Extract the hero-state variable map from the author-mode debug screens that ship
inside the ALIS scripts (BARATIN*.IO: 'LABEL :' followed by a print of a variable).
Usage: python -I tools/debugvars.py <unpacked_dir> <out.tsv>
Operand encodings (PC, little-endian; from reference/alis/src/opernames.c):
  1e omainb / 20 omainw / 22 omainp  <u16 main offset>
  2a ohimb  / 2c ohimw  / 2e ohimp   <u16 local slot holding entity id> <s16 offset in that entity>
  06 olocb  / 08 olocw  / 0a olocp   <u16 local offset>
  38 ... 3a  eval: 00 <u8 imm> | 02 <u16 imm> | 26 omaintc/28 omainti/24 omaintp <u16 main table> | 0e oloctc/10 olocti <u16>
"""
import re, sys, os

SIMPLE = {0x1e: ('main', 'b'), 0x20: ('main', 'w'), 0x22: ('main', 'str'),
          0x06: ('local', 'b'), 0x08: ('local', 'w'), 0x0a: ('local', 'str'),
          0x12: ('dir', 'b'), 0x14: ('dir', 'w')}
HIM = {0x2a: 'b', 0x2c: 'w', 0x2e: 'str'}
TAB = {0x24: ('main', 'tabstr'), 0x26: ('main', 'tabb'), 0x28: ('main', 'tabw'),
       0x0c: ('local', 'tabstr'), 0x0e: ('local', 'tabb'), 0x10: ('local', 'tabw'),
       0x1a: ('dir', 'tabb'), 0x1c: ('dir', 'tabw')}


def u16(d, i):
    return d[i] | (d[i + 1] << 8)


def s16(d, i):
    v = u16(d, i)
    return v - 0x10000 if v & 0x8000 else v


def decode(d, i):
    op = d[i]
    if op in SIMPLE:
        sp, ty = SIMPLE[op]
        return f'{sp}.{ty}[0x{u16(d, i + 1):04x}]'
    if op in HIM:
        return f'entity(local.w[0x{u16(d, i + 1):04x}]).{HIM[op]}[{s16(d, i + 3):+#x}]'
    if op == 0x38:
        j, idx = i + 1, []
        while j < len(d) and d[j] != 0x3a and j < i + 24:
            c = d[j]
            if c == 0x00:
                idx.append(str(d[j + 1])); j += 2
            elif c == 0x02:
                idx.append(str(s16(d, j + 1))); j += 3
            elif c in TAB:
                sp, ty = TAB[c]
                return f'{sp}.{ty}[0x{u16(d, j + 1):04x}][{",".join(idx)}]'
            elif c in SIMPLE:
                sp, ty = SIMPLE[c]
                idx.append(f'{sp}.{ty}[0x{u16(d, j + 1):04x}]'); j += 3
            else:
                return f'expr({d[i:i + 16].hex(" ")})'
        return f'expr({d[i:i + 16].hex(" ")})'
    return f'?({d[i:i + 8].hex(" ")})'


def main(src, out):
    rows = []
    for fn in sorted(os.listdir(src)):
        if not fn.upper().startswith('BARATINU'):
            continue
        d = open(os.path.join(src, fn), 'rb').read()
        for m in re.finditer(rb'([A-Z][A-Z0-9 _\-\.]{1,14}):\x00\xb8', d):
            lab = m.group(1).decode().strip()
            rows.append((fn, hex(m.start()), lab, decode(d, m.end())))
    with open(out, 'w') as f:
        f.write('script\toffset\tlabel\tvariable\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')
    for r in rows:
        print(f'{r[2]:12} {r[3]}')
    print(len(rows), 'labels')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])

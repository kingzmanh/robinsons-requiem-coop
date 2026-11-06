"""Derive operand specs for every ALIS opcode / opername / storename / addname /
codesc handler from the engine's C source (pre-v6 path only).
Writes tools/alisdis/specs_auto.json: {table: {code: [name, [tokens...], flags]}}
Tokens: b=read8 w=read16 t=read24 E=expr(opername) S=storename A=addname
        Z=zero-terminated string  C1/C2/C3=codesc subtables  ?=unparsed control flow
"""
import json, re, os, sys

SRC = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\Msi\Tools\rrq_coop\engine\src'
files = {f: open(os.path.join(SRC, f), encoding='utf-8', errors='replace').read()
         for f in ['opcodes.c', 'opernames.c', 'storenames.c', 'addnames.c', 'escnames.c', 'alis.c', 'script.c']}
allsrc = '\n'.join(files.values())

TABLES = {'opcodes': ('opcodes.c', r'const sAlisOpcode opcodes\[\] = \{'),
          'opernames': ('opernames.c', r'const sAlisOpcode opernames\[\] = \{'),
          'storenames': ('storenames.c', r'const sAlisOpcode storenames\[\] = \{'),
          'addnames': ('addnames.c', r'const sAlisOpcode addnames\[\] = \{'),
          'codesc1': ('escnames.c', r'const sAlisOpcode codesc1names\[\] = \{'),
          'codesc2': ('escnames.c', r'const sAlisOpcode codesc2names\[\] = \{'),
          'codesc3': ('escnames.c', r'const sAlisOpcode codesc3names\[\] = \{')}

TOK = [(r'script_read8\s*\(', 'b'), (r'script_read16\s*\(', 'w'), (r'script_read24\s*\(', 't'),
       (r'script_read32\s*\(', 'l'),
       (r'readexec_opername_saveD7\s*\(', 'E'), (r'readexec_opername_saveD6\s*\(', 'E'),
       (r'readexec_opername_swap\s*\(', 'E'), (r'readexec_opername\s*\(', 'E'),
       (r'readexec_storename\s*\(', 'S'), (r'readexec_addname_swap\s*\(', 'A'), (r'readexec_addname\s*\(', 'A'),
       (r'readexec_codesc1name\s*\(', 'C1'), (r'readexec_codesc2name\s*\(', 'C2'), (r'readexec_codesc3name\s*\(', 'C3'),
       (r'script_read_until_zero\s*\(', 'Z'), (r'script_read_bytes\s*\(', '?bytes'),
       (r'cstore_continue\s*\(', 'S'), (r'cadd_continue\s*\(', 'A')]
TOKRE = re.compile('|'.join(f'(?P<t{i}>{p})' for i, (p, _) in enumerate(TOK)))
CALLRE = re.compile(r'\b([a-z_][a-z0-9_]*)\s*\(')


def body(name):
    m = re.search(r'(?m)^(?:static\s+)?(?:inline\s+)?(?:void|s16|u16|s32|u32|u8|s8|int)\s+' + re.escape(name) + r'\s*\(\s*(?:void)?\s*\)\s*\{', allsrc)
    if not m:
        return None
    i, depth = m.end(), 1
    while depth and i < len(allsrc):
        c = allsrc[i]
        depth += c == '{'
        depth -= c == '}'
        i += 1
    return allsrc[m.end():i - 1]


def strip(b):
    b = re.sub(r'//.*', '', b)
    b = re.sub(r'/\*.*?\*/', '', b, flags=re.S)
    b = re.sub(r'ALIS_DEBUG\s*\([^;]*;', '', b)
    return b


memo = {}


def tokens(name, depth=0):
    if name in memo:
        return memo[name]
    memo[name] = (['?rec'], set())
    b = body(name)
    if b is None:
        memo[name] = ([], {'nobody'})
        return memo[name]
    b = strip(b)
    flags = set()
    if re.search(r'\b(if|while|for|switch|do)\b', b):
        flags.add('cf')
    if re.search(r'version|platform\.uid|platform\.kind', b):
        flags.add('ver')
    out = []
    pos = 0
    for m in re.finditer(r'[A-Za-z_][A-Za-z0-9_]*\s*\(', b):
        nm = m.group(0).rstrip('( \t')
        hit = None
        for i, (p, t) in enumerate(TOK):
            if re.fullmatch(p.replace(r'\s*\(', ''), nm):
                hit = t
                break
        if hit:
            out.append(hit)
        elif depth < 3 and nm not in ('if', 'while', 'for', 'switch', 'return', 'sizeof') and re.match(r'^[a-z]', nm):
            sub, fl = tokens(nm, depth + 1)
            if sub and sub != ['?rec']:
                out += sub
                flags |= {f for f in fl if f != 'nobody'}
    memo[name] = (out, flags)
    return memo[name]


specs = {}
for tab, (fn, start) in TABLES.items():
    s = files[fn]
    m = re.search(start, s)
    end = s.find('};', m.end())
    ent = re.findall(r'DECL_OPCODE\((0x[0-9a-fA-F]+|0),\s*(\w+|NULL)\s*,', s[m.end():end])
    d = {}
    for idx, (code, h) in enumerate(ent):
        if h == 'NULL':
            continue
        t, fl = tokens(h)
        d[idx] = [h, t, sorted(fl)]
    specs[tab] = d
json.dump(specs, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'specs_auto.json'), 'w'), indent=0)
for tab, d in specs.items():
    cf = sum('cf' in v[2] for v in d.values())
    print(tab, len(d), 'with control flow:', cf)

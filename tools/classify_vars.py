"""First WORLD / PLAYER / UI classification of the saved game state (analysis/save_state_map.tsv).
Rules come from the Phase 1 evidence (writer scripts, debug-screen labels, how the save routine
and the scripts use each table). Output: analysis/player_vars.tsv
Confidence: high = labelled / clear writer set, med = inferred from usage, low = needs a runtime check.
Usage: python -I tools/classify_vars.py <mod-root>
"""
import csv, os, sys

root = sys.argv[1]
# (lo, hi_exclusive, class, confidence, note)
RULES = [
    (0x0e82, 0x0e8b, 'UI', 'med', 'mouse x/y, panel mode, current action icon (souris/yoda)'),
    (0x0e89, 0x0e8a, 'PLAYER', 'med', 'saved single, body/time state'),
    (0x0e5e, 0x0e92, 'PLAYER', 'low', '2D table defined by MAIN cdim, body related?'),
    (0x0e92, 0x0eac, 'PLAYER', 'high', 'wounds table (BLESSURE[]), 13 body parts'),
    (0x0eb2, 0x0eba, 'PLAYER', 'med', 'body table (time/yoda), read by BODY screen'),
    (0x0ec0, 0x0eda, 'PLAYER', 'high', 'illness table (VENIN, INSOLATION, VENTRE, GRIPPE ...)'),
    (0x0ee0, 0x0ef6, 'PLAYER', 'med', 'body table read by BODY screen'),
    (0x0efc, 0x0f12, 'PLAYER', 'med', 'yoda table, saved'),
    (0x0f18, 0x0f2e, 'PLAYER', 'med', 'body table (heros/time/yoda), read by BODY'),
    (0x0f2e, 0x0f36, 'PLAYER', 'med', 'body bytes; 0f35 = inventory panel scroll'),
    (0x0f3e, 0x173e, 'PLAYER', 'high', 'INVENTORY: slots x [type, qty, weight, state]'),
    (0x1744, 0x183a, 'UI', 'high', 'inventory panel slot -> inventory index (objet/yoda)'),
    (0x183a, 0x1854, 'PLAYER', 'med', 'selection / posture (POST 184c), 184d zone or mode id read by NPCs'),
    (0x1854, 0x1869, 'PLAYER', 'med', 'BODY screen state (written only by body.IO)'),
    (0x1872, 0x1922, 'PLAYER', 'low', 'MAIN cdim table 1872, unknown'),
    (0x1922, 0x195e, 'PLAYER', 'high', 'body stats: fever, blood, water, bag load, fatigue, energy, PSY, temp, rain, clothes, malaria ...'),
    (0x195e, 0x19da, 'PLAYER', 'low', 'param/yoda table (124 bytes) - skills/known recipes?'),
    (0x19da, 0x19e2, 'PLAYER', 'high', 'sleep, drunk'),
    (0x19e8, 0x19fe, 'PLAYER', 'low', 'time/yoda table read by MESSAGE'),
    (0x1a04, 0x1a16, 'PLAYER', 'low', 'body timers (1a08 fast counter reset on save), mouse 1a06/1a12'),
    (0x1a1e, 0x1a46, 'PLAYER', 'low', 'MAIN cdim table 1a1e'),
    (0x1a46, 0x1a4c, 'PLAYER', 'med', 'DRFUSEE (flare), heros 1a48/1a4a'),
    (0x1a52, 0x1a8e, 'META', 'high', 'BACKSAVE.INF: last 5 save names (not game state)'),
    (0x1a8e, 0x1a96, 'PLAYER', 'high', 'movement flag DRDEP, SPEED, ALTI, head angle HTETE'),
    (0x1a9e, 0x28ae, 'WORLD', 'high', 'creature/animal state table (all animal scripts)'),
    (0x28b6, 0x2d66, 'WORLD', 'high', 'NPC/human state table (amazons, centaurs, giants ...)'),
    (0x2d66, 0x2d68, 'WORLD', 'med', 'NPC interaction byte written by NPC scripts'),
    (0x2d6d, 0x2da0, 'PLAYER', 'med', 'heros/yoda bytes (DRCOU, MINERVE), 2d9a table'),
    (0x39ac, 0x39b4, 'WORLD', 'med', 'chart map descriptor (map knowledge; co-op: shared map)'),
    (0x39b4, 0x39b7, 'PLAYER', 'high', 'light: LUX, DRLUX, DRALLUME (torch/lamp lit)'),
    (0x3a2a, 0x3ad8, 'WORLD', 'low', 'MAIN cdim tables 3a2a..3acc'),
    (0x3ad8, 0x3ad9, 'PLAYER', 'high', 'DRLAMPE'),
    (0x3ad9, 0x3ada, 'WORLD', 'high', 'DRAMAZONE story flag (amazons)'),
    (0x3ada, 0x3adc, 'PLAYER', 'med', 'NBPLAQUE (plates carried), 3adb'),
    (0x3ae2, 0x3b06, 'PLAYER', 'low', 'yoda tables 3ae2/3af2, ETOUFFE 3aea'),
    (0x3b06, 0x3b08, 'PLAYER', 'med', 'CALPLUIE, DRGOURDE (gourd)'),
    (0x3b0e, 0x3b14, 'WORLD', 'low', 'yoda table read by avion.IO (plane)'),
    (0x3b14, 0x3b18, 'PLAYER', 'med', 'heros words'),
    (0x3b1e, 0x3b3c, 'WORLD', 'low', 'MAIN cdim tables'),
    (0x3b3c, 0x3b3e, 'PLAYER', 'med', 'DRRADEAU (raft), 3b3d'),
    (0x3b44, 0x3baa, 'WORLD', 'low', 'MAIN cdim tables'),
    (0x3baa, 0x3bb8, 'PLAYER', 'high', 'hero position/angle copy (save only)'),
    (0x3bb8, 0x3bb9, 'WORLD', 'med', 'DRDONNE (gave item to amazons) story flag'),
    (0x3bb9, 0x3bc4, 'PLAYER', 'med', 'DRGAZ, DRATOMIC, DRSAC equipment flags'),
    (0x3bcc, 0x3be0, 'WORLD', 'low', 'MAIN cdim table'),
    (0x3be0, 0x3be4, 'PLAYER', 'med', 'HATOMIC, CPTSEL'),
    (0x3be6, 0x3bf2, 'WORLD', 'high', 'rain / daylight palette (weather, time of day)'),
    (0x3bf6, 0x3bfa, 'WORLD', 'low', 'saved bytes (time)'),
    (0x3c00, 0x3c14, 'WORLD', 'low', 'yoda/param block'),
    (0x3c1e, 0x3c82, 'WORLD', 'med', 'fires table (feu.IO reads)'),
    (0x3c92, 0x3c96, 'WORLD', 'med', 'descriptor of a 40 KB table (world objects/map)'),
    (0x3ca0, 0x3ca1, 'PLAYER', 'low', 'yoda byte'),
]


def classify(o):
    for lo, hi, c, conf, note in RULES:
        if lo <= o < hi:
            return c, conf, note
    return 'UNKNOWN', 'low', ''


rows = list(csv.DictReader(open(os.path.join(root, 'analysis', 'save_state_map.tsv')), delimiter='\t'))
out = os.path.join(root, 'analysis', 'player_vars.tsv')
tot = {}
with open(out, 'w') as f:
    f.write('kind\toffset\tsize\tclass\tconfidence\tlabels\tnote\twriters\n')
    for r in rows:
        o = int(r['offset'], 16)
        c, conf, note = classify(o)
        tot[c] = tot.get(c, 0) + int(r['size'])
        f.write(f"{r['kind']}\t{r['offset']}\t{r['size']}\t{c}\t{conf}\t{r['labels']}\t{note}\t{r['writers']}\n")
print(out, {k: f'{v} bytes' for k, v in tot.items()})

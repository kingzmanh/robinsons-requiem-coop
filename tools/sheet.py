"""Contact sheet of harness BMP shots. Usage: python -I tools/sheet.py <dir> <out.png> [cols] [glob]"""
import sys, glob, os
from PIL import Image, ImageDraw
d, out = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
pat = sys.argv[4] if len(sys.argv) > 4 else '*.bmp'
fs = sorted(glob.glob(os.path.join(d, pat)))
W, H = 320, 220
rows = (len(fs) + cols - 1) // cols
sheet = Image.new('RGB', (cols * W, max(1, rows) * H), (40, 40, 40))
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(fs):
    im = Image.open(f).convert('RGB').resize((320, 200))
    x, y = (i % cols) * W, (i // cols) * H
    sheet.paste(im, (x, y))
    dr.text((x + 2, y + 202), os.path.basename(f)[:40], fill=(255, 255, 0))
sheet.save(out)
print(out, len(fs))

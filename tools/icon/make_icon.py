"""Build the RRQ.exe icon from the game's own art, on the player's PC.

Input: a 320x200 screenshot of the game's options menu (taken by RRQ.exe itself, see
make_icon.bat). Output: a multi-size .ico (16..256) of the hero from that menu.
No game art is shipped with the mod; this runs where the game is installed.
"""
import sys
from PIL import Image, ImageDraw

HERO = (21, 61, 65, 105)          # hero head + shoulders in the options menu (320x200)
GOLD = (196, 160, 60, 255)
DARK = (40, 28, 12, 255)


def build(src, out, preview=None):
    im = Image.open(src).convert('RGB')
    if im.size != (320, 200):
        im = im.resize((320, 200), Image.NEAREST)
    art = im.crop(HERO)                                   # 44x44
    big = art.resize((44 * 5, 44 * 5), Image.NEAREST)     # crisp pixel art, 220x220
    canvas = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    mask = Image.new('L', (256, 256), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 255, 255), radius=40, fill=255)
    bg = Image.new('RGBA', (256, 256), DARK)
    canvas.paste(bg, (0, 0), mask)
    d = ImageDraw.Draw(canvas)
    d.rounded_rectangle((4, 4, 251, 251), radius=36, outline=GOLD, width=8)
    inner = Image.new('L', (220, 220), 0)
    ImageDraw.Draw(inner).rounded_rectangle((0, 0, 219, 219), radius=26, fill=255)
    canvas.paste(big, (18, 18), inner)
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    canvas.save(out, format='ICO', sizes=sizes)
    if preview:
        pv = Image.new('RGBA', (256 + 128 + 48 + 32 + 16 + 50, 260), (60, 60, 60, 255))
        x = 0
        for s in (256, 128, 48, 32, 16):
            pv.paste(canvas.resize((s, s), Image.LANCZOS), (x, 0))
            x += s + 10
        pv.save(preview)


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)

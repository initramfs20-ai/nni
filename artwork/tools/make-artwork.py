#!/usr/bin/env python3
"""Regenerate the nni artwork from the wallpapers (needs Pillow).
   python3 tools/make-artwork.py      (run from the artwork/ folder)
Outputs (the four used by the packages are copied to artwork/generated/):
  build/chick.png              the chick cut out on a transparent background
  build/splash-ynnios.ppm      boot splash for YnniOS     (binary PPM, for nni-splash)
  build/splash-sgnnios.ppm     boot splash for SGnniOS
  build/logo_linux_clut224.ppm 80x80, <=224 colours: the kernel boot logo (chickens instead of tuxes)
  build/grub-background.png    GRUB menu background
"""
import os, sys
from collections import deque
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.dirname(HERE)
OUT = os.path.join(ART, 'build')
os.makedirs(OUT, exist_ok=True)

def font(size, bold=True):
    for p in ['/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf' if bold else
              '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
              '/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf']:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()

# ---------------------------------------------------------------- cut out the chick
src = Image.open(os.path.join(ART, 'wallpapers', '04_amber_dark.png')).convert('RGB')
box = (940, 300, 1480, 866)                 # chick, beak and feet (not the floor reflection)
crop = src.crop(box)
W, H = crop.size
px = crop.load()

def is_chick(r, g, b):
    # bright, warm and saturated: yellow fluff, orange beak and feet
    mx, mn = max(r, g, b), min(r, g, b)
    return mx > 150 and (mx - mn) > 70 and r >= g >= b - 10 and b < 140

raw = Image.new('L', (W, H))
rp = raw.load()
for y in range(H):
    for x in range(W):
        rp[x, y] = 255 if is_chick(*px[x, y]) else 0
# shrink first so thin glowing arcs lose their bridge to the body, grow back after
core = raw.filter(ImageFilter.MinFilter(9)).load()
mask = [[core[x, y] > 0 for x in range(W)] for y in range(H)]
# keep only the blob that contains the chick's belly, so the glowing arcs fall away
seed = (int(W * 0.55), int(H * 0.60))
keep = [[False] * W for _ in range(H)]
q = deque([seed]); keep[seed[1]][seed[0]] = True
while q:
    x, y = q.popleft()
    for nx, ny in ((x+1, y), (x-1, y), (x, y+1), (x, y-1)):
        if 0 <= nx < W and 0 <= ny < H and not keep[ny][nx] and mask[ny][nx]:
            keep[ny][nx] = True; q.append((nx, ny))
grown = Image.new('L', (W, H)); gp = grown.load()
for y in range(H):
    for x in range(W):
        gp[x, y] = 255 if keep[y][x] else 0
grown = grown.filter(ImageFilter.MaxFilter(11)).load()
keep = [[grown[x, y] > 0 and rp[x, y] > 0 for x in range(W)] for y in range(H)]
# fill holes (the eye, shadows inside the body): anything not reachable from the border
outside = [[False] * W for _ in range(H)]
q = deque()
for x in range(W):
    for y in (0, H - 1):
        if not keep[y][x] and not outside[y][x]:
            outside[y][x] = True; q.append((x, y))
for y in range(H):
    for x in (0, W - 1):
        if not keep[y][x] and not outside[y][x]:
            outside[y][x] = True; q.append((x, y))
while q:
    x, y = q.popleft()
    for nx, ny in ((x+1, y), (x-1, y), (x, y+1), (x, y-1)):
        if 0 <= nx < W and 0 <= ny < H and not keep[ny][nx] and not outside[ny][nx]:
            outside[ny][nx] = True; q.append((nx, ny))
alpha = Image.new('L', (W, H))
ap = alpha.load()
for y in range(H):
    for x in range(W):
        ap[x, y] = 0 if outside[y][x] else 255
alpha = alpha.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(1.2))
chick = crop.convert('RGBA'); chick.putalpha(alpha)
chick = chick.crop(chick.getbbox())
chick.save(os.path.join(OUT, 'chick.png'))

def on_black(img, size):
    """chick scaled to fit `size`, flattened onto black"""
    c = img.copy(); c.thumbnail(size, Image.LANCZOS)
    bg = Image.new('RGB', size, (0, 0, 0))
    bg.paste(c, ((size[0] - c.width) // 2, (size[1] - c.height) // 2), c)
    return bg

# ---------------------------------------------------------------- boot splashes
def splash(title, subtitle, path):
    S = Image.new('RGB', (480, 360), (0, 0, 0))
    c = chick.copy(); c.thumbnail((240, 250), Image.LANCZOS)
    S.paste(c, ((480 - c.width) // 2, 20), c)
    d = ImageDraw.Draw(S)
    f1, f2 = font(34), font(15, bold=False)
    tw = d.textlength(title, font=f1)
    d.text(((480 - tw) / 2, 280), title, font=f1, fill=(255, 214, 64))
    sw = d.textlength(subtitle, font=f2)
    d.text(((480 - sw) / 2, 324), subtitle, font=f2, fill=(200, 200, 200))
    with open(path, 'wb') as fh:
        fh.write(b'P6\n%d %d\n255\n' % S.size)
        fh.write(S.tobytes())
    S.save(path.replace('.ppm', '-preview.png'))

splash('YnniOS', 'starting up...', os.path.join(OUT, 'splash-ynnios.ppm'))
splash('SGnniOS', 'Source-Genius - starting up...', os.path.join(OUT, 'splash-sgnnios.ppm'))

# ---------------------------------------------------------------- kernel logo
# The kernel wants an 80x80 plain PPM (P3) with at most 224 colours.
logo = on_black(chick, (80, 80))
logo = logo.quantize(colors=224, method=Image.MEDIANCUT, dither=Image.NONE).convert('RGB')
colours = len(set(logo.tobytes()[i:i + 3] for i in range(0, 80 * 80 * 3, 3)))
assert colours <= 224, colours
with open(os.path.join(OUT, 'logo_linux_clut224.ppm'), 'w') as fh:
    fh.write('P3\n# nni chicken boot logo\n80 80\n255\n')
    raw_px = logo.tobytes()
    data = [tuple(raw_px[i:i + 3]) for i in range(0, len(raw_px), 3)]
    for i in range(0, len(data), 4):
        fh.write('  '.join('%3d %3d %3d' % p for p in data[i:i + 4]) + '\n')
logo.resize((320, 320), Image.NEAREST).save(os.path.join(OUT, 'logo-preview.png'))

# ---------------------------------------------------------------- GRUB background
g = Image.open(os.path.join(ART, 'wallpapers', '01_debian_crimson.png')).convert('RGB')
g = g.resize((1024, 576), Image.LANCZOS)
g.save(os.path.join(OUT, 'grub-background.png'), optimize=True)

# the files the packages use live in artwork/generated (committed to git)
import shutil
GEN = os.path.join(ART, 'generated')
os.makedirs(GEN, exist_ok=True)
for f in ('splash-ynnios.ppm', 'splash-sgnnios.ppm', 'logo_linux_clut224.ppm', 'grub-background.png'):
    shutil.copy(os.path.join(OUT, f), os.path.join(GEN, f))
print('artwork written to', GEN, '(previews in', OUT + ') - kernel logo colours:', colours)

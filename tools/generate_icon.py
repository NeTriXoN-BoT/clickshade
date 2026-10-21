#!/usr/bin/env python3
"""Recreate ClickShade's original vector-native icon and sharp size variants.

Requires Pillow. No fonts or external art. The SVG is generated from the same
geometry as the raster versions. Each ICO frame is rendered from geometry at
its own size, never enlarged from a 16/32 px resource. The desktop binary embeds
only the 256 px PNG; the 1024 px PNG and vector master remain in the repository.
"""
from io import BytesIO
from pathlib import Path
import struct
import math
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
POINTS = [(72, 48), (86, 187), (117, 158), (143, 202),
          (164, 189), (139, 144), (180, 138)]
FACET = [(72, 48), (180, 138), (139, 144), (164, 189),
         (154, 195), (126, 143), (157, 137)]
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


def render(size):
    # Extra samples preserve smooth diagonal edges at every resource size.
    samples = max(4, (1024 + size - 1) // size)
    side = size * samples
    unit = side / 256
    def box(coords):
        return tuple(round(value * unit) for value in coords)
    im = Image.new('RGBA', (side, side))
    gradient = Image.new('RGBA', (side, side))
    draw = ImageDraw.Draw(gradient)
    for y in range(side):
        t = y / max(1, side - 1)
        color = tuple(round(a + (b - a) * t) for a, b in zip((28, 43, 60), (9, 16, 25))) + (255,)
        draw.line((0, y, side, y), fill=color)
    mask = Image.new('L', (side, side))
    ImageDraw.Draw(mask).rounded_rectangle(box((6, 6, 250, 250)), radius=round(55 * unit), fill=255)
    im.paste(gradient, (0, 0), mask)
    draw = ImageDraw.Draw(im)
    draw.rounded_rectangle(box((6, 6, 250, 250)), radius=round(55 * unit),
                           outline='#35465a', width=max(1, round(unit)))
    # The quiet orbit and split-tone arrow remain legible in a16px tray slot.
    ring_width = 13 if size <= 24 else 10
    draw.ellipse(box((47, 43, 209, 205)), fill='#162e39')
    for angle in range(360):
        fraction = (math.sin(math.radians(angle)) + 1) / 2
        color = tuple(round(a + (b - a) * fraction) for a, b in zip((95, 232, 202), (69, 165, 233)))
        draw.arc(box((47, 43, 209, 205)), angle, angle + 2, fill=color,
                 width=round(ring_width * unit))
    points = [(round(x * unit), round(y * unit)) for x, y in POINTS]
    shadow = [(x + round(2 * unit), y + round(4 * unit)) for x, y in points]
    draw.polygon(shadow, fill='#08111c')
    draw.line(shadow + [shadow[0]], fill='#08111c', width=round(10 * unit), joint='curve')
    draw.polygon(points, fill='#f2f8fd')
    if size > 24:
        draw.polygon([(round(x * unit), round(y * unit)) for x, y in FACET], fill='#8adadf')
    stroke = round((7 if size <= 24 else 5.5) * unit)
    draw.line(points + [points[0]], fill='#0b1724', width=stroke, joint='curve')
    return im.resize((size, size), Image.Resampling.LANCZOS)


def write_ico(path):
    frames = []
    for size in SIZES:
        stream = BytesIO()
        render(size).save(stream, format='PNG', optimize=True)
        frames.append(stream.getvalue())
    offset = 6 + 16 * len(frames)
    directory = bytearray(struct.pack('<HHH', 0, 1, len(frames)))
    for size, data in zip(SIZES, frames):
        directory += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    path.write_bytes(directory + b''.join(frames))


(ROOT / 'assets').mkdir(exist_ok=True)
(ROOT / 'resources').mkdir(exist_ok=True)
points_svg = ' '.join(f'{x},{y}' for x, y in POINTS)
facet_svg = ' '.join(f'{x},{y}' for x, y in FACET)
(ROOT / 'assets/clickshade.svg').write_text(f'''<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 256 256" role="img" aria-label="ClickShade">
  <defs>
    <linearGradient id="tile" x2="0" y2="1"><stop stop-color="#1c2b3c"/><stop offset="1" stop-color="#091019"/></linearGradient>
    <linearGradient id="orbit" x2="0" y2="1"><stop stop-color="#5fe8ca"/><stop offset="1" stop-color="#45a5e9"/></linearGradient>
  </defs>
  <rect x="6" y="6" width="244" height="244" rx="55" fill="url(#tile)" stroke="#35465a"/>
  <circle cx="128" cy="124" r="76" fill="#162e39" stroke="url(#orbit)" stroke-width="10"/>
  <polygon points="{points_svg}" transform="translate(2 4)" fill="#08111c" stroke="#08111c" stroke-width="10" stroke-linejoin="round"/>
  <polygon points="{points_svg}" fill="#f2f8fd"/>
  <polygon points="{facet_svg}" fill="#8adadf"/>
  <polygon points="{points_svg}" fill="none" stroke="#0b1724" stroke-width="5.5" stroke-linejoin="round"/>
</svg>
''', encoding='utf-8')
render(1024).save(ROOT / 'assets/clickshade.png', optimize=True)
render(256).save(ROOT / 'assets/clickshade-256.png', optimize=True)
write_ico(ROOT / 'resources/app.ico')
data = (ROOT / 'assets/clickshade-256.png').read_bytes()
lines = [','.join(f'0x{x:02x}' for x in data[i:i+16])+',' for i in range(0, len(data), 16)]
(ROOT / 'linux/icon_data.h').write_text(
    '// Generated from the 256 px variant of the original ClickShade vector icon.\n'
    '#pragma once\nnamespace clickshade::linuxapp {\n'
    'inline constexpr unsigned char kIconPng[] = {\n' +
    ''.join('    '+line+'\n' for line in lines) + '};\n}\n', encoding='utf-8')
print('Generated SVG, 1024/256 px PNG, ten Windows ICO sizes and Linux embedded icon')

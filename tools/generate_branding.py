#!/usr/bin/env python3
"""Generate the original, editable GitHub banner and social preview.

Pillow is needed only to regenerate the PNG; the software has no Pillow/font
runtime dependency. The SVG uses system sans-serif fonts. No external artwork.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import os

ROOT = Path(__file__).resolve().parents[1]
W, H, S = 1280, 640, 2
FONT_DIRS = [Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Fonts',
             Path('/usr/share/fonts/truetype/dejavu'),
             Path('/usr/share/fonts/opentype/urw-base35')]

def font(size, bold=False):
    names = ('segoeuib.ttf', 'DejaVuSans-Bold.ttf', 'NimbusSans-Bold.otf') if bold else (
        'segoeui.ttf', 'DejaVuSans.ttf', 'NimbusSans-Regular.otf')
    for folder in FONT_DIRS:
        for name in names:
            if (folder / name).is_file():
                return ImageFont.truetype(str(folder / name), round(size * S))
    raise SystemExit('Install DejaVu Sans, or add your local sans-serif font path to FONT_DIRS.')

im = Image.new('RGB', (W*S, H*S))
d = ImageDraw.Draw(im)
for y in range(H*S):
    t = y / (H*S - 1)
    d.line((0, y, W*S, y), fill=tuple(round(a+(b-a)*t) for a,b in zip((20,30,44),(11,17,27))))
def rect(box, fill, outline=None, width=1, radius=0):
    xy = tuple(round(v*S) for v in box)
    if radius: d.rounded_rectangle(xy, radius=round(radius*S), fill=fill, outline=outline, width=round(width*S))
    else: d.rectangle(xy, fill=fill, outline=outline, width=round(width*S))
def ellipse(box, fill=None, outline=None, width=1):
    d.ellipse(tuple(round(v*S) for v in box), fill=fill, outline=outline, width=round(width*S))
def text(x,y,value,size,color,bold=False):
    d.text((x*S,y*S),value,font=font(size,bold),fill=color,stroke_width=0)

# Quiet geometric echoes of a highlight and a short trail, not a fake app UI.
ellipse((804,-10,1374,560),outline='#203b42',width=2)
ellipse((854,40,1324,510),outline='#234b50',width=2)
ellipse((907,93,1271,457),outline='#326d6d',width=2)
for x,y,r,color in [(806,439,5,'#25474c'),(839,425,7,'#315b60'),(874,410,9,'#437e7d'),(912,391,11,'#67c3b5')]:
    ellipse((x-r,y-r,x+r,y+r),fill=color)
icon = Image.open(ROOT/'assets/clickshade.png').convert('RGBA').resize((282*S,282*S),Image.Resampling.LANCZOS)
im.paste(icon,(946*S,157*S),icon)
d=ImageDraw.Draw(im)
text(80,68,'DESKTOP CURSOR EFFECTS',18,'#82e5d0',True)
text(74,123,'ClickShade',92,'#f4f8fc',True)
text(80,250,'Cursor effects for your desktop.',32,'#e4edf4')
text(80,316,'Highlights, press feedback, trails',25,'#a4b5c6')
text(80,354,'and click sounds. All your settings stay local.',23,'#a4b5c6')
for x,width,label in [(80,172,'Native C++'),(268,160,'Windows'),(444,134,'Linux')]:
    rect((x,444,x+width,490),'#172b35','#31515b',radius=23)
    text(x+23,455,label,19,'#c4e7e2')
d.line((80*S,548*S,1200*S,548*S),fill='#2a3b4a',width=S)
text(80,574,'No account. No telemetry. Open source.',18,'#7f95a9')
text(1035,574,'OPEN SOURCE',15,'#82e5d0',True)
im.resize((W,H),Image.Resampling.LANCZOS).save(ROOT/'assets/github-banner.png',optimize=True)

icon_svg=(ROOT/'assets/clickshade.svg').read_text(encoding='utf-8')
icon_body=icon_svg[icon_svg.index('>')+1:icon_svg.rfind('</svg>')]
svg=f'''<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="640" viewBox="0 0 1280 640" role="img" aria-label="ClickShade — desktop cursor effects">
<defs><linearGradient id="background" x2="0" y2="1"><stop stop-color="#141e2c"/><stop offset="1" stop-color="#0b111b"/></linearGradient></defs>
<rect width="1280" height="640" fill="url(#background)"/>
<g fill="none" stroke-width="2"><circle cx="1089" cy="275" r="285" stroke="#203b42"/><circle cx="1089" cy="275" r="235" stroke="#234b50"/><circle cx="1089" cy="275" r="182" stroke="#326d6d"/></g>
<g><circle cx="806" cy="439" r="5" fill="#25474c"/><circle cx="839" cy="425" r="7" fill="#315b60"/><circle cx="874" cy="410" r="9" fill="#437e7d"/><circle cx="912" cy="391" r="11" fill="#67c3b5"/></g>
<g transform="translate(946 157) scale(1.1015625)">{icon_body}</g>
<g font-family="Segoe UI,DejaVu Sans,sans-serif">
<text x="80" y="87" font-size="18" font-weight="700" fill="#82e5d0">DESKTOP CURSOR EFFECTS</text>
<text x="74" y="211" font-size="92" font-weight="700" fill="#f4f8fc">ClickShade</text>
<text x="80" y="282" font-size="32" fill="#e4edf4">Cursor effects for your desktop.</text>
<g font-size="25" fill="#a4b5c6"><text x="80" y="341">Highlights, press feedback, trails</text><text x="80" y="379" font-size="23">and click sounds. All your settings stay local.</text></g>
<g fill="#172b35" stroke="#31515b"><rect x="80" y="444" width="172" height="46" rx="23"/><rect x="268" y="444" width="160" height="46" rx="23"/><rect x="444" y="444" width="134" height="46" rx="23"/></g>
<g font-size="19" fill="#c4e7e2"><text x="103" y="477">Native C++</text><text x="291" y="477">Windows</text><text x="467" y="477">Linux</text></g>
<path d="M80 548H1200" stroke="#2a3b4a"/>
<text x="80" y="592" font-size="18" fill="#7f95a9">No account. No telemetry. Open source.</text>
<text x="1035" y="590" font-size="15" font-weight="700" fill="#82e5d0">OPEN SOURCE</text>
</g></svg>
'''
(ROOT/'assets/github-banner.svg').write_text(svg,encoding='utf-8')
print('Generated 1280 × 640 GitHub banner/social preview PNG and editable SVG')

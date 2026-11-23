# Builds the bubble icon and LiveArea images (sce_sys/). With two grabs of the game's own intro in art_src/
# (legal_screen.png: the GAUNTLET LEGENDS logo, skorne.png: the demon in the opening cinematic; take them
# yourself, e.g. in Vita3K) they are made from those; without them, plain title cards are drawn instead.
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'art_src')
OUT = os.path.join(HERE, 'sce_sys')


def logo():
    # the logo sits on black: key it out by brightness, then trim
    im = Image.open(os.path.join(SRC, 'legal_screen.png')).convert('RGB').crop((150, 0, 810, 120))
    a = im.convert('L').point(lambda v: 0 if v < 18 else min(255, (v - 18) * 4))
    im = im.convert('RGBA')
    im.putalpha(a)
    return im.crop(im.getbbox())


def scene():
    # Skorne without the subtitle lines (game image spans x 118..842 of the 960x544 grab)
    return Image.open(os.path.join(SRC, 'skorne.png')).convert('RGB').crop((118, 0, 842, 420))


def fit(im, w, h, cx=0.5, cy=0.5):
    s = max(w / im.width, h / im.height)
    im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
    x = round((im.width - w) * cx)
    y = round((im.height - h) * cy)
    return im.crop((x, y, x + w, y + h))


def shade_bottom(im, frac, strength=230):
    g = Image.new('L', im.size, 0)
    d = ImageDraw.Draw(g)
    y0 = int(im.height * (1 - frac))
    for y in range(y0, im.height):
        d.line([(0, y), (im.width, y)], fill=int(strength * (y - y0) / (im.height - y0)))
    return Image.composite(Image.new('RGB', im.size, 0), im, g)


def put_logo(im, width, y):
    lg = logo()
    lg = lg.resize((width, round(lg.height * width / lg.width)), Image.LANCZOS)
    glow = Image.new('RGBA', lg.size, (0, 0, 0, 0))
    glow.putalpha(lg.getchannel('A').filter(ImageFilter.GaussianBlur(3)))
    x = (im.width - lg.width) // 2
    im = im.convert('RGBA')
    im.alpha_composite(glow, (x, y))
    im.alpha_composite(lg, (x, y))
    return im.convert('RGB')


def plain_card(w, h, title_size):
    # dark red to black, with the title in gold
    im = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=(int(90 * (1 - t)) + 10, 8, int(20 * (1 - t)) + 6))
    font = ImageFont.load_default(size=title_size)
    lines = ['GAUNTLET', 'LEGENDS']
    boxes = [d.textbbox((0, 0), s, font=font) for s in lines]
    total = sum(b[3] - b[1] for b in boxes) + title_size // 4
    y = (h - total) // 2
    for s, b in zip(lines, boxes):
        x = (w - (b[2] - b[0])) // 2 - b[0]
        d.text((x + 2, y - b[1] + 2), s, font=font, fill=(0, 0, 0))
        d.text((x, y - b[1]), s, font=font, fill=(236, 186, 64))
        y += b[3] - b[1] + title_size // 4
    return im


def main():
    os.makedirs(os.path.join(OUT, 'livearea', 'contents'), exist_ok=True)
    if not os.path.isfile(os.path.join(SRC, 'skorne.png')):
        icon, bg, st = plain_card(128, 128, 22), plain_card(840, 500, 96), plain_card(280, 158, 30)
        icon.convert('P', palette=Image.ADAPTIVE).save(os.path.join(OUT, 'icon0.png'), optimize=True)
        bg.convert('P', palette=Image.ADAPTIVE).save(os.path.join(OUT, 'livearea', 'contents', 'bg.png'), optimize=True)
        st.convert('P', palette=Image.ADAPTIVE).save(os.path.join(OUT, 'livearea', 'contents', 'startup.png'),
                                                     optimize=True)
        write_template()
        return
    sc = scene()
    icon = fit(sc.crop((150, 0, 574, 420)), 128, 128, 0.5, 0.2)
    icon = put_logo(shade_bottom(icon, 0.4), 120, 98)
    icon.convert('P', palette=Image.ADAPTIVE).save(os.path.join(OUT, 'icon0.png'), optimize=True)
    bg = put_logo(shade_bottom(fit(sc, 840, 500, 0.5, 0.15), 0.35), 600, 392)
    bg.convert('P', palette=Image.ADAPTIVE).save(os.path.join(OUT, 'livearea', 'contents', 'bg.png'), optimize=True)
    st = put_logo(Image.new('RGB', (280, 158), (8, 4, 16)), 250, 50)
    st.convert('P', palette=Image.ADAPTIVE).save(os.path.join(OUT, 'livearea', 'contents', 'startup.png'), optimize=True)
    write_template()


def write_template():
    with open(os.path.join(OUT, 'livearea', 'contents', 'template.xml'), 'w') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<livearea style="a1" format-ver="01.00" content-rev="1">\n'
                '  <livearea-background>\n    <image>bg.png</image>\n  </livearea-background>\n'
                '  <gate>\n    <startup-image>startup.png</startup-image>\n  </gate>\n'
                '</livearea>\n')


if __name__ == '__main__':
    main()

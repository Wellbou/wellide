#!/usr/bin/env python3
"""Generate Wellide artwork from the same vortex field the app draws.

  data/icons/wellide.svg, wellide-on.svg, wellide-<size>.png
  packaging/windows/wellide.ico
  docs/*.svg  (README art)

Keep field() in sync with src/vortex.c.
"""
import math, os, sys

INK, GLOW, BG = "#222222", "#ca31cc", "#0b0710"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def field(n, x, y, phase=0.0, twist=1.55):
    c = (n - 1) / 2; dx, dy = x - c, y - c; r = math.hypot(dx, dy)
    if r > n / 2 - 0.3: return 0
    if r < n * 0.07: return 3
    th = math.atan2(dy, dx) + phase
    seg = (4 * th / (2 * math.pi) - twist * math.log(r + 1)) % 4.0
    i = int(seg)
    return (1 if i % 2 == 0 else 2) if seg - i < 0.55 else 0

def grid(n, phase=0.0, twist=1.55, clean=True):
    g = [[field(n, x, y, phase, twist) for x in range(n)] for y in range(n)]
    if clean:  # pixel art hates specks: drop pixels with no same-colour neighbour
        for y in range(n):
            for x in range(n):
                v = g[y][x]
                if v in (1, 2) and not any(0 <= x+dx < n and 0 <= y+dy < n and g[y+dy][x+dx] == v
                                           for dx, dy in ((1,0),(-1,0),(0,1),(0,-1))):
                    g[y][x] = 0
    return g

def show(g):
    print("\n".join(" ".join(".#@o"[v] for v in row) for row in g))

def rects(g, px, colors, ox=0, oy=0):
    out = []
    for y, row in enumerate(g):
        for x, v in enumerate(row):
            col = colors.get(v)
            if col:
                out.append(f'<rect x="{ox+x*px}" y="{oy+y*px}" width="{px}" height="{px}" fill="{col}"/>')
    return "".join(out)

def icon_svg(n=16, on=False, px=16, plate=True):
    g = grid(n)
    size = n * px
    pad = px  # plate margin
    total = size + 2 * pad
    ink = "#3a2a44" if plate else INK
    cols = {1: ink, 2: GLOW, 3: "#f4d6f5" if on else None}
    body = rects(g, px, cols, pad, pad)
    bg = f'<rect width="{total}" height="{total}" rx="{px*2}" fill="{BG}"/>' if plate else ""
    glow = ""
    if on:
        glow = (f'<defs><radialGradient id="h"><stop offset="0" stop-color="{GLOW}" stop-opacity=".55"/>'
                f'<stop offset="1" stop-color="{GLOW}" stop-opacity="0"/></radialGradient></defs>'
                f'<circle cx="{total/2}" cy="{total/2}" r="{total/2}" fill="url(#h)"/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {total} {total}" '
            f'shape-rendering="crispEdges">{bg}{glow}{body}</svg>')

def write(path, data, mode="w"):
    path = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as f: f.write(data)
    return path

def pngs():
    from PIL import Image, ImageDraw
    imgs = {}
    for s in (16, 24, 32, 48, 64, 128, 256):
        # small sizes: fewer cells so each pixel stays crisp
        n = 12 if s <= 24 else 16
        g = grid(n)
        big = n + 2
        im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        if s >= 32:
            d.rectangle([0, 0, big - 1, big - 1], fill=BG)
        for y, row in enumerate(g):
            for x, v in enumerate(row):
                if v == 1: im.putpixel((x + 1, y + 1), (0x3a, 0x2a, 0x44, 255) if s >= 32 else (0x55, 0x3d, 0x63, 255))
                elif v == 2: im.putpixel((x + 1, y + 1), (0xca, 0x31, 0xcc, 255))
        im = im.resize((s, s), Image.NEAREST)
        imgs[s] = im
        if s != 24:
            im.save(os.path.join(ROOT, f"data/icons/wellide-{s}.png"))
    ico = os.path.join(ROOT, "packaging/windows/wellide.ico")
    os.makedirs(os.path.dirname(ico), exist_ok=True)
    imgs[256].save(ico, sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)],
                   append_images=[imgs[s] for s in (16, 24, 32, 48, 64, 128)])

# ---------- README art ----------

FONT = "ui-monospace,'JetBrains Mono','DejaVu Sans Mono',monospace"

def spin_frames(n, frames=12):
    """Animated vortex: stacked frames toggled with SMIL, pixel-exact."""
    groups = []
    px = 10
    for k in range(frames):
        g = grid(n, phase=-2 * math.pi * k / frames / 4)  # 4 arms -> quarter turn loops
        body = rects(g, px, {1: "#3a2a44", 2: GLOW, 3: "#f4d6f5"})
        vals = ";".join("visible" if i == k else "hidden" for i in range(frames))
        groups.append(f'<g visibility="hidden"><set attributeName="visibility" to="visible"/>'
                      f'<animate attributeName="visibility" values="{vals}" dur="1.2s" '
                      f'repeatCount="indefinite" calcMode="discrete"/>{body}</g>')
    return "".join(groups), n * px

def banner():
    frames, sz = spin_frames(21)
    W, H = 960, 300
    ox, oy = 70, (H - sz) // 2
    stars = "".join(f'<rect x="{(i*137)%W}" y="{(i*71)%H}" width="3" height="3" fill="{GLOW}" opacity="{0.15+0.1*(i%4)}"/>'
                    for i in range(40))
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" shape-rendering="crispEdges">
<rect width="{W}" height="{H}" rx="18" fill="{BG}"/>{stars}
<g transform="translate({ox},{oy})">{frames}</g>
<text x="340" y="150" font-family="{FONT}" font-size="72" font-weight="700" fill="#f1e6f5">wellide</text>
<text x="344" y="196" font-family="{FONT}" font-size="22" fill="#b58fc0">tiny sing-box VPN client · GTK · Linux &amp; Windows</text>
<text x="344" y="232" font-family="{FONT}" font-size="18" fill="#6f5a7a">~60 MB RAM · TUN · subscriptions · auto ping</text>
</svg>'''

def pixel_text(s, x, y, px, col):
    F = {"O": ["111", "101", "101", "101", "111"], "N": ["101", "111", "111", "111", "101"],
         "F": ["111", "100", "110", "100", "100"]}
    out, cx = [], x
    for ch in s:
        for r, row in enumerate(F[ch]):
            for c, b in enumerate(row):
                if b == "1": out.append(f'<rect x="{cx+c*px}" y="{y+r*px}" width="{px}" height="{px}" fill="{col}"/>')
        cx += 4 * px
    return "".join(out)

def button_demo():
    """OFF: loose pixels -> collapse into a spinning vortex -> ON."""
    n, px = 21, 10
    sz = n * px
    W, H = 520, 300
    ox, oy = 40, 45
    off = grid(n, twist=0.35)   # nearly straight arms = relaxed
    parts = []
    T = 4.0
    # relaxed state (0..40%), spinning (50..100%)
    body_off = rects(off, px, {1: "#3a2a44", 2: "#6b3a70"})
    parts.append(f'<g>{body_off}<animate attributeName="opacity" values="1;1;0;0;1" keyTimes="0;.38;.46;.94;1" dur="{T}s" repeatCount="indefinite"/></g>')
    frames, _ = spin_frames(n, 12)
    parts.append(f'<g opacity="0">{frames}<animate attributeName="opacity" values="0;0;1;1;0" keyTimes="0;.38;.46;.94;1" dur="{T}s" repeatCount="indefinite"/></g>')
    lab_on = pixel_text("ON", 330, 120, 10, "#f1e6f5")
    lab_off = pixel_text("OFF", 310, 120, 10, "#6f5a7a")
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" shape-rendering="crispEdges">
<rect width="{W}" height="{H}" rx="18" fill="{BG}"/>
<g transform="translate({ox},{oy})">{"".join(parts)}</g>
<g>{lab_off}<animate attributeName="opacity" values="1;1;0;0;1" keyTimes="0;.38;.46;.94;1" dur="{T}s" repeatCount="indefinite"/></g>
<g opacity="0">{lab_on}<animate attributeName="opacity" values="0;0;1;1;0" keyTimes="0;.38;.46;.94;1" dur="{T}s" repeatCount="indefinite"/></g>
<text x="300" y="220" font-family="{FONT}" font-size="16" fill="#6f5a7a">click the vortex</text>
</svg>'''

def themes_card():
    th = [("Void", BG, "#140c1a", GLOW, "#f1e6f5", 0, False),
          ("Notebook", "#fbf8ef", "#f1ecdd", "#2b59c3", "#1d1d1d", 4, True),
          ("Purple", "#17121f", "#211a2c", "#9b6bff", "#eee8f7", 14, False),
          ("Graphite", "#1b1d20", "#23262a", "#5aa9ff", "#e6e8ea", 10, False)]
    W, H = 960, 250
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}">']
    for i, (nm, bg, side, acc, fg, rad, ruled) in enumerate(th):
        x = 10 + i * 237
        out.append(f'<g transform="translate({x},10)"><rect width="225" height="230" rx="{max(rad,2)}" fill="{bg}" stroke="{acc}" stroke-opacity=".35"/>')
        out.append(f'<rect width="58" height="230" rx="{max(rad,2)}" fill="{side}"/>')
        if ruled:
            out.extend(f'<line x1="58" x2="225" y1="{y}" y2="{y}" stroke="#a9c4ea" stroke-width="1"/>' for y in range(30, 230, 22))
            out.append('<line x1="78" x2="78" y1="0" y2="230" stroke="#e58a8a"/>')
        for k in range(4):
            out.append(f'<rect x="10" y="{20+k*22}" width="38" height="8" rx="2" fill="{fg}" opacity="{0.9 if k==0 else 0.35}"/>')
        g = grid(11)
        out.append(f'<g transform="translate(98,40)" shape-rendering="crispEdges">{rects(g, 7, {1: fg if bg.startswith("#f") else "#3a2a44", 2: acc})}</g>')
        out.append(f'<text x="141" y="200" text-anchor="middle" font-family="{FONT}" font-size="18" font-weight="700" fill="{fg}">{nm}</text></g>')
    out.append("</svg>")
    return "".join(out)

def ram_chart():
    """Measured RSS on Arch/KDE, connected in TUN mode (see README)."""
    rows = [("wellide (GTK UI)", 62, GLOW), ("sing-box core", 64, "#8a4fa0"), ("total", 126, "#f1e6f5")]
    W, H = 760, 40 + len(rows) * 46
    mx = 160
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" shape-rendering="crispEdges">',
           f'<rect width="{W}" height="{H}" rx="14" fill="{BG}"/>']
    for i, (nm, mb, col) in enumerate(rows):
        y = 24 + i * 46
        w = int(mb / mx * 440)
        out.append(f'<text x="20" y="{y+20}" font-family="{FONT}" font-size="16" fill="#d9cde0">{nm}</text>')
        for b in range(0, w, 12):
            out.append(f'<rect x="{230+b}" y="{y+4}" width="10" height="20" fill="{col}"/>')
        out.append(f'<text x="{240+w}" y="{y+20}" font-family="{FONT}" font-size="15" fill="#b58fc0">~{mb} MB</text>')
    out.append("</svg>")
    return "".join(out)

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "show":
        show(grid(int(sys.argv[2]) if len(sys.argv) > 2 else 16)); return
    write("data/icons/wellide.svg", icon_svg())
    write("data/icons/wellide-on.svg", icon_svg(on=True))
    write("docs/banner.svg", banner())
    write("docs/button.svg", button_demo())
    write("docs/themes.svg", themes_card())
    write("docs/ram.svg", ram_chart())
    pngs()
    print("ok")

if __name__ == "__main__":
    main()

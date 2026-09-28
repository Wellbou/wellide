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

def icon_svg(n=16, on=False, px=16, plate=False):
    g = grid(n)
    size = n * px
    pad = px if plate else 0
    total = size + 2 * pad
    ink = "#3a2a44" if plate else "#553d63"   # must read on dark AND light panels
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
    from PIL import Image
    imgs = {}
    for s in (16, 24, 32, 48, 64, 128, 256):
        n = 12 if s <= 24 else 16          # fewer cells at tiny sizes stay crisp
        g = grid(n)
        im = Image.new("RGBA", (n, n), (0, 0, 0, 0))   # transparent background
        for y, row in enumerate(g):
            for x, v in enumerate(row):
                if v == 1: im.putpixel((x, y), (0x55, 0x3d, 0x63, 255))
                elif v == 2: im.putpixel((x, y), (0xca, 0x31, 0xcc, 255))
        # keep a 1/16 margin so the whirl doesn't touch the edges
        pad = max(1, n // 16)
        canvas = Image.new("RGBA", (n + 2 * pad, n + 2 * pad), (0, 0, 0, 0))
        canvas.paste(im, (pad, pad))
        im = canvas.resize((s, s), Image.NEAREST)
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

def banner(lang="en"):
    frames, sz = spin_frames(21)
    W, H = 960, 300
    ox, oy = 60, (H - sz) // 2
    stars = "".join(f'<rect x="{(i*137)%W}" y="{(i*71)%H}" width="3" height="3" fill="{GLOW}" opacity="{0.15+0.1*(i%4)}"/>'
                    for i in range(40))
    if lang == "ru":
        l1, l2, l3 = "лёгкий VPN-клиент на sing-box", "GTK · Linux и Windows", "~125 МБ ОЗУ · TUN · подписки · автопинг"
    else:
        l1, l2, l3 = "tiny sing-box VPN client", "GTK · Linux &amp; Windows", "~125 MB RAM · TUN · subscriptions · auto ping"
    # text column starts at 320 and must end before W-30: 610 px available.
    # textLength pins the rendered width, so no font can push it past the edge.
    def t(y, size, col, txt, weight="400"):
        est = len(txt) * size * 0.62               # monospace advance ≈ 0.6 em
        fit = f' textLength="{min(est, 600):.0f}" lengthAdjust="spacingAndGlyphs"' if est > 600 else ""
        return f'<text x="320" y="{y}" font-family="{FONT}" font-size="{size}" font-weight="{weight}" fill="{col}"{fit}>{txt}</text>'
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" shape-rendering="crispEdges">
<rect width="{W}" height="{H}" rx="18" fill="{BG}"/>{stars}
<g transform="translate({ox},{oy})">{frames}</g>
{t(128, 68, "#f1e6f5", "wellide", "700")}
{t(172, 24, "#d9bfe3", l1)}
{t(206, 20, "#b58fc0", l2)}
{t(240, 17, "#6f5a7a", l3)}
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

def frames_svg(frame_dir, step=3, scale=0.75, dur=None, bg=BG, frames=None):
    """Turn rendered PNG frames into a looping SVG (discrete frame switching).
    Pixel-art frames compress well as run-length rectangles per colour."""
    from PIL import Image
    files = sorted(f for f in os.listdir(frame_dir) if f.endswith(".png"))
    if frames: files = files[frames[0]:frames[1]]
    files = files[::step]
    if not files: return ""
    first = Image.open(os.path.join(frame_dir, files[0]))
    W, H = first.size
    cell = 5   # tools/render uses S/56 → 5px cells at 320
    n = len(files)
    dur = dur or n * step / 25.0
    bgc = tuple(int(bg[i:i+2], 16) for i in (1, 3, 5))
    groups = []
    for k, f in enumerate(files):
        im = Image.open(os.path.join(frame_dir, f)).convert("RGB")
        px = im.load()
        runs = {}
        # sample the centre of each cell
        for gy in range(0, H // cell):
            y = gy * cell + cell // 2
            x0 = None; col0 = None
            for gx in range(0, W // cell + 1):
                x = gx * cell + cell // 2
                col = px[x, y] if gx < W // cell else None
                if col is not None and sum(abs(col[i] - bgc[i]) for i in range(3)) < 24: col = None
                if col is not None: col = tuple(min(255, (c + 8) // 16 * 16) for c in col)
                if col != col0:
                    if col0 is not None:
                        runs.setdefault(col0, []).append((x0, gy, gx - x0))
                    x0, col0 = gx, col
        body = []
        for col, rs in runs.items():
            hexc = "#%02x%02x%02x" % col
            d = "".join(f"M{x*cell} {y*cell}h{w*cell-1}v{cell-1}h-{w*cell-1}z" for x, y, w in rs)
            body.append(f'<path fill="{hexc}" d="{d}"/>')
        vals = ";".join("inline" if i == k else "none" for i in range(n))
        groups.append(f'<g display="none"><animate attributeName="display" values="{vals}" '
                      f'dur="{dur:.2f}s" repeatCount="indefinite" calcMode="discrete"/>{"".join(body)}</g>')
    return W, H, "".join(groups)

def button_demo(frame_dir, lang="en"):
    r = frames_svg(frame_dir, frames=(20, 150))
    if not r: return None
    W, H, g = r
    cap = "нажми на кнопку — рукава сливаются, вспышка, кнопка загорается" if lang == "ru" \
          else "click: the arms wrap in, a star bursts, the button lights up"
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H + 36}" shape-rendering="crispEdges">'
            f'<rect width="{W}" height="{H + 36}" rx="16" fill="{BG}"/>{g}'
            f'<text x="{W/2}" y="{H + 22}" text-anchor="middle" font-family="{FONT}" font-size="11" '
            f'fill="#8e7f99">{cap}</text></svg>')

def themes_card(lang="en"):
    """Each theme with its own button, rendered by the app (tools/render --still)."""
    import base64
    names = {"void": "Void", "notebook": "Тетрадь" if lang == "ru" else "Notebook",
             "purple": "Фиолетовая" if lang == "ru" else "Purple", "graphite": "Графит" if lang == "ru" else "Graphite"}
    fd = os.environ.get("WL_STILLS", "")
    W, H = 960, 270
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 {W} {H}">']
    for i, tid in enumerate(names):
        x = 10 + i * 237
        png = os.path.join(fd, f"{tid}.png")
        out.append(f'<g transform="translate({x},10)">')
        if os.path.exists(png):
            b64 = base64.b64encode(open(png, "rb").read()).decode()
            out.append(f'<rect width="225" height="250" rx="10" fill="#000" opacity=".25"/>'
                       f'<image x="0" y="0" width="225" height="210" xlink:href="data:image/png;base64,{b64}"/>')
        out.append(f'<text x="112" y="238" text-anchor="middle" font-family="{FONT}" font-size="18" '
                   f'font-weight="700" fill="#d9cde0">{names[tid]}</text></g>')
    out.append("</svg>")
    return "".join(out)

def ram_chart():
    """Measured RSS on Arch/KDE, connected in TUN mode (see README)."""
    # RSS, Arch + KDE, TUN connected. GUI private memory is ~38 MB; the rest is shared GTK libs.
    rows = [("wellide (GTK UI)", 92, GLOW), ("sing-box core", 61, "#8a4fa0"), ("total", 153, "#f1e6f5")]
    W, H = 760, 40 + len(rows) * 46
    mx = 180
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
    write("docs/banner.svg", banner("en"))
    write("docs/banner.ru.svg", banner("ru"))
    write("docs/themes.svg", themes_card("en"))
    write("docs/themes.ru.svg", themes_card("ru"))
    write("docs/ram.svg", ram_chart())
    fd = os.environ.get("WL_FRAMES")   # frames from: tools/render void $WL_FRAMES 320 25
    if fd and os.path.isdir(fd):
        for lang, name in (("en", "docs/button.svg"), ("ru", "docs/button.ru.svg")):
            b = button_demo(fd, lang)
            if b: write(name, b)
    pngs()
    print("ok")

if __name__ == "__main__":
    main()

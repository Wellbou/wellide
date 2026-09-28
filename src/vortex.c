/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The connect button.
 *
 * A round power button with the vortex arms orbiting *around* it. The
 * button shows the state, not the action: lit with the theme colour and
 * "ON/ВКЛ" when connected, dark with "OFF/ВЫКЛ" when not.
 *
 * Each theme picks a look (ButtonStyle) and its own switch effect:
 *   PIXEL  (Void)     — pixel grid; a white star bursts out, then burns out
 *                       from the centre and breaks into sparks that spin
 *                       away with the arms; the shock ring crumbles.
 *   SKETCH (Notebook) — pen strokes; the pen circles the button and
 *                       rewrites the label, ink dots splash out.
 *   CRYSTAL (Amethyst) — a cut gem with shards for arms; the light spins
 *                       around the facets and the girdle breaks apart.
 *   DIAL   (Graphite) — calm arcs; an accent arc sweeps around the rim.
 *
 * Animation: GATHER (arms wrap onto the button) → SWITCH (effect, label
 * flips) → RELEASE (arms spring back out). While the core starts the arms
 * stay wrapped and spin. A failed start skips the effect.
 *
 * Cost: frames are drawn into an offscreen image and blitted once; the
 * idle orbit runs on a 20 fps timer, cached layers are rotated instead of
 * re-rendered, and nothing runs while the window is hidden. */
#include "wellide.h"
#include <math.h>
#include <string.h>

typedef enum { PH_IDLE, PH_GATHER, PH_SWITCH, PH_RELEASE } Phase;

typedef struct {
    ButtonStyle style;
    GdkRGBA bg, card, fg, dim, glow, ink, line, armb, onfg;
    char *label, *next;
    CoreState st;
    Phase ph;
    gint64 ph_start;
    double g, g0;             /* how far the arms are wrapped onto the button */
    gboolean pending_switch;
    double phase, speed, hover;
    gboolean shown_on;        /* state the button currently shows (flips at the switch) */
    double lit, arms_lit;     /* 0 = off colours, 1 = on colours, eased */
    gboolean hovering, pressed, animated;
    guint tick, timer;
    gint64 last;
    void (*click)(void);
    PangoLayout *layout;
    guint8 *grid;
    int grid_n;
    cairo_surface_t *cells, *gridl, *arms, *btn, *halo;
    int cache_size;
    char *btn_key;
} Vortex;

#define T_GATHER  0.55
#define T_RELEASE 0.80

static const char *KEY = "wl-vortex";
static gint64 fake_now;   /* offline renderer */
static gint64 now_us(void) { return fake_now ? fake_now : g_get_monotonic_time(); }
static Vortex *V(GtkWidget *w) { return g_object_get_data(G_OBJECT(w), KEY); }

static double clamp01(double t) { return t < 0 ? 0 : t > 1 ? 1 : t; }
static double ease_in_out(double t) { return t < .5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2; }
static double ease_out(double t) { return 1 - pow(1 - t, 3); }
static double ease_out_back(double t)
{
    const double c1 = 1.9, c3 = c1 + 1;
    return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2);
}
static double lerp(double a, double b, double t) { return a + (b - a) * t; }
static double hash01(int i, int salt)
{
    double h = sin(i * 12.9898 + salt * 78.233) * 43758.5453;
    return h - floor(h);
}

static void mix(GdkRGBA *o, const GdkRGBA *a, const GdkRGBA *b, double t)
{
    GdkRGBA r = { lerp(a->red, b->red, t), lerp(a->green, b->green, t),
                  lerp(a->blue, b->blue, t), lerp(a->alpha, b->alpha, t) };
    *o = r;
}
static double lum(const GdkRGBA *c) { return 0.2126 * c->red + 0.7152 * c->green + 0.0722 * c->blue; }
static void src(cairo_t *cr, const GdkRGBA *c, double a)
{
    cairo_set_source_rgba(cr, c->red, c->green, c->blue, c->alpha * a);
}
static const GdkRGBA WHITE = { 1, 1, 1, 1 };

/* caches live next to the window surface (server side on X11), so a
 * frame only uploads what actually changed */
static cairo_surface_t *similar(cairo_t *cr, cairo_content_t c, int w, int h)
{
    return cairo_surface_create_similar(cairo_get_target(cr), c, w, h);
}

static double switch_len(ButtonStyle s) { return s == BTN_PIXEL ? 0.85 : 0.7; }

/* ---------- shared geometry ---------- */

typedef struct {
    double W, H, S, cx, cy, rb, rin, rout;
    double g;          /* wrap amount; overshoots below 0 on release */
    double sw;         /* 0..1 progress of the switch effect, <0 = none */
    double press;
    double lit;        /* 0..1 off→on colour blend */
    gboolean on, busy;
} Frame;

static void arm_point(const Vortex *v, const Frame *f, int k, int arms, double t,
                      double *x, double *y, double *w)
{
    double g = f->g;
    double r0 = f->rin + (f->rout - f->rin) * t;
    double r1 = f->rb * 1.06 + f->rb * 0.22 * t;
    double r = lerp(r0, r1, clamp01(g));
    if (g < 0) r = r0 * (1 - g * 0.12);
    double curve = 1.7 * (1 + 1.8 * clamp01(g));
    double a = v->phase + k * 2 * G_PI / arms - curve * t;
    *x = f->cx + cos(a) * r;
    *y = f->cy + sin(a) * r;
    *w = f->S * 0.060 * (1 - 0.72 * t) * (1 - 0.3 * clamp01(g));
}

static const char *shown_label(const Vortex *v)
{
    return v->label ? v->label : v->next ? v->next : "";
}

/* colours of the button face for the current state */
static void face_colors(const Vortex *v, const Frame *f, GdkRGBA *fill, GdkRGBA *ring, GdkRGBA *mark, double *mark_a)
{
    double pulse = f->busy ? 0.5 + 0.5 * sin(now_us() / 1e6 * 6) : 0;
    GdkRGBA ofill, oring, omark, nring;
    mix(&ofill, &v->card, &v->glow, f->busy ? 0.15 + 0.2 * pulse : v->hover * 0.10);
    mix(&oring, &v->line, &v->glow, f->busy ? 0.6 : v->hover * 0.8);
    mix(&omark, &v->dim, &v->fg, v->hover);
    mix(&nring, &v->glow, &WHITE, 0.45);
    mix(fill, &ofill, &v->glow, f->lit);
    mix(ring, &oring, &nring, f->lit);
    mix(mark, &omark, &v->onfg, f->lit);
    *mark_a = f->busy ? 0.55 + 0.45 * pulse : 1;
}

/* arm colours: dim and cool while off, full glow while on */
static void arm_colors(const Vortex *v, double lit, GdkRGBA *a, GdkRGBA *b)
{
    GdkRGBA da, db;
    mix(&da, &v->glow, &v->bg, 0.62);
    mix(&db, &v->armb, &v->bg, 0.45);
    mix(a, &da, &v->glow, lit);
    mix(b, &db, &v->armb, lit);
}

static void paint_halo(cairo_t *cr, Vortex *v, const Frame *f)
{
    double breathe = 0.5 + 0.5 * sin(now_us() / 1e6 * 2.0);
    double a = f->lit * (0.22 + 0.12 * breathe) + v->hover * 0.08;
    if (a < 0.01) return;
    int size = (int)ceil(f->rout * 2.05);
    if (!v->halo || v->cache_size != (int)f->S) {
        if (v->halo) cairo_surface_destroy(v->halo);
        v->halo = similar(cr, CAIRO_CONTENT_COLOR_ALPHA, size, size);
        cairo_t *c = cairo_create(v->halo);
        double m = size / 2.0;
        cairo_pattern_t *p = cairo_pattern_create_radial(m, m, f->rb * 0.8, m, m, m);
        cairo_pattern_add_color_stop_rgba(p, 0, v->glow.red, v->glow.green, v->glow.blue, 1);
        cairo_pattern_add_color_stop_rgba(p, 1, v->glow.red, v->glow.green, v->glow.blue, 0);
        cairo_set_source(c, p);
        cairo_paint(c);
        cairo_pattern_destroy(p);
        cairo_destroy(c);
    }
    cairo_set_source_surface(cr, v->halo, f->cx - size / 2.0, f->cy - size / 2.0);
    cairo_paint_with_alpha(cr, a);
}

/* power symbol ⏻ + state word, vector looks */
static void paint_face(cairo_t *cr, Vortex *v, const Frame *f, const char *family, double reveal)
{
    GdkRGBA fill, ring, mark; double ma;
    face_colors(v, f, &fill, &ring, &mark, &ma);
    double k = 1 - 0.05 * f->press;
    double R = f->rb * 0.27 * k, lw = f->rb * 0.085 * k;
    double gx = f->cx, gy = f->cy - f->rb * 0.20 * k;
    cairo_save(cr);
    if (reveal < 1) {   /* the pen writes the face from left to right */
        cairo_rectangle(cr, f->cx - f->rb, f->cy - f->rb, 2 * f->rb * reveal, 2 * f->rb);
        cairo_clip(cr);
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, lw);
    src(cr, &mark, ma);
    cairo_arc(cr, gx, gy, R, -G_PI / 2 + 0.75, -G_PI / 2 - 0.75 + 2 * G_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, gx, gy - R * 1.25);
    cairo_line_to(cr, gx, gy - R * 0.1);
    cairo_stroke(cr);

    const char *txt = shown_label(v);
    if (*txt) {
        if (!v->layout) v->layout = pango_cairo_create_layout(cr);
        else pango_cairo_update_layout(cr, v->layout);
        PangoFontDescription *d = pango_font_description_from_string(family);
        pango_font_description_set_absolute_size(d, f->rb * 0.27 * k * PANGO_SCALE);
        pango_font_description_set_weight(d, PANGO_WEIGHT_HEAVY);
        pango_layout_set_font_description(v->layout, d);
        pango_font_description_free(d);
        pango_layout_set_text(v->layout, txt, -1);
        int tw, th;
        pango_layout_get_pixel_size(v->layout, &tw, &th);
        double sc = tw > f->rb * 1.3 ? f->rb * 1.3 / tw : 1;
        cairo_translate(cr, f->cx, f->cy + f->rb * 0.45 * k);
        cairo_scale(cr, sc, sc);
        cairo_move_to(cr, -tw / 2.0, -th / 2.0);
        src(cr, &mark, ma);
        pango_cairo_show_layout(cr, v->layout);
    }
    cairo_restore(cr);
    cairo_new_path(cr);   /* the path survives restore: don't let the next arc connect to it */
}

/* ---------- PIXEL (Void) ---------- */

enum { C_NONE, C_ARM_A, C_ARM_B, C_FILL, C_RING, C_MARK, C_WHITE, C_SPARK, C_N };

static void put(guint8 *G, int n, int x, int y, guint8 c)
{
    if (x >= 0 && y >= 0 && x < n && y < n) G[y * n + x] = c;
}

static void stamp_disc(guint8 *G, int n, double gx, double gy, double r, guint8 c, gboolean over)
{
    int x0 = MAX(0, (int)floor(gx - r)), x1 = MIN(n - 1, (int)ceil(gx + r));
    int y0 = MAX(0, (int)floor(gy - r)), y1 = MIN(n - 1, (int)ceil(gy + r));
    double r2 = r * r;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            double dx = x + 0.5 - gx, dy = y + 0.5 - gy;
            if (dx * dx + dy * dy <= r2 && (over || !G[y * n + x])) G[y * n + x] = c;
        }
}

static void paint_pixel(cairo_t *cr, Vortex *v, const Frame *f)
{
    double px = MAX(2, floor(f->S / 56));
    int n = (int)ceil(f->S / px) + 2;
    n += n & 1;   /* even: the centre falls on a cell boundary, so shapes mirror exactly */
    if (n > v->grid_n) { g_free(v->grid); v->grid = g_malloc(n * n); v->grid_n = n; }
    guint8 *G = v->grid;
    memset(G, 0, n * n);
    double ox = f->cx - n / 2 * px, oy = f->cy - n / 2 * px;
    double cgx = n / 2, cgy = n / 2;
#define GX(x) (((x) - ox) / px)
#define GY(y) (((y) - oy) / px)

    for (int k = 0; k < 4; k++)
        for (int i = 0; i <= 56; i++) {
            double x, y, w, t = i / 56.0;
            arm_point(v, f, k, 4, t, &x, &y, &w);
            stamp_disc(G, n, GX(x), GY(y), MAX(w / px / 2, 0.55), k % 2 ? C_ARM_B : C_ARM_A, FALSE);
        }

    double rb = round(f->rb / px * (1 - 0.05 * f->press));
    stamp_disc(G, n, cgx, cgy, rb, C_RING, TRUE);
    stamp_disc(G, n, cgx, cgy, rb - 1.3, C_FILL, TRUE);

    /* power glyph ⏻, mirror-symmetric around the centre line. No words
     * here: in Void the state reads from the lit button itself. */
    {
        int th = MAX(2, (int)round(rb * 0.15));
        th += th & 1;                                   /* even → centred bar */
        double gR = rb * 0.40, gcy = cgy + rb * 0.04;
        int x0 = (int)floor(cgx - gR - th), x1 = (int)ceil(cgx + gR + th);
        int y0 = (int)floor(gcy - gR - th * 2), y1 = (int)ceil(gcy + gR + th);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                double dx = x + 0.5 - cgx, dy = y + 0.5 - gcy, r = hypot(dx, dy);
                double ang = atan2(fabs(dx), -dy);       /* 0 = straight up, symmetric */
                if (fabs(r - gR) <= th / 2.0 && ang > 0.62) put(G, n, x, y, C_MARK);
            }
        int top = (int)floor(gcy - gR - th * 0.9), bot = (int)ceil(gcy - gR * 0.05);
        for (int y = top; y <= bot; y++)
            for (int x = (int)cgx - th / 2; x < (int)cgx + th / 2; x++)
                put(G, n, x, y, C_MARK);
    }

    /* the star. Everything stays at full brightness: pixels switch off
     * one by one instead of fading, which is how pixel art "fades". */
    if (f->sw >= 0) {
        double t = f->sw;
        double Lmax = f->S * 0.47 / px;
        /* 1. white-out of the button, revealed again from the centre */
        if (t < 0.5) {
            double rev = t < 0.12 ? -1 : rb * ease_out(clamp01((t - 0.12) / 0.38));
            for (int y = (int)(cgy - rb - 1); y <= (int)(cgy + rb + 1); y++)
                for (int x = (int)(cgx - rb - 1); x <= (int)(cgx + rb + 1); x++) {
                    double r = hypot(x + 0.5 - cgx, y + 0.5 - cgy);
                    if (r <= rb && r >= rev) put(G, n, x, y, C_WHITE);
                }
        }
        /* 2. rays: shoot out, then burn out from the centre */
        double L = Lmax * ease_out(clamp01(t / 0.16));
        double s0 = t < 0.18 ? 0 : L * ease_in_out(clamp01((t - 0.18) / 0.34));
        for (int d = 0; d < 8 && s0 < L; d++) {
            double a = d * G_PI / 4 - G_PI / 2 + v->phase * 0.15;
            double len = d % 2 ? L * 0.55 : L, st = d % 2 ? s0 * 0.55 : s0;
            for (double s = MAX(st, rb * 0.2); s < len; s += 0.5) {
                double w = (d % 2 ? 0.7 : 1.5) * (1 - s / Lmax) + 0.5;
                stamp_disc(G, n, cgx + cos(a) * s, cgy + sin(a) * s, w, C_WHITE, TRUE);
            }
        }
        /* 3. sparks break off the rays and spiral away with the arms */
        if (t > 0.26) {
            double tt = t - 0.26;
            for (int i = 0; i < 40; i++) {
                double die = 0.5 + 0.5 * hash01(i, 1);
                if (t >= die) continue;
                int d = i % 8;
                double a = d * G_PI / 4 - G_PI / 2 + (hash01(i, 2) - 0.5) * 0.35;
                double sp = 0.7 + 0.9 * hash01(i, 3);
                double r = Lmax * (0.35 + 0.5 * hash01(i, 4) + tt * sp * 1.3) * (d % 2 ? 0.7 : 1);
                a += tt * 2.2 * sp;                     /* swirl like the arms */
                double x = cgx + cos(a) * r, y = cgy + sin(a) * r;
                guint8 c = t < die - 0.14 ? C_WHITE : C_SPARK;   /* cool down: white → glow */
                if (hash01(i, 5) > 0.7) stamp_disc(G, n, x, y, 1.0, c, TRUE);
                else put(G, n, (int)x, (int)y, c);
            }
        }
        /* 4. shock ring crumbles pixel by pixel */
        if (t > 0.1) {
            double tt = (t - 0.1) / 0.9;
            double rr = rb + (f->rout * 1.08 / px - rb) * ease_out(tt);
            int cntp = (int)(rr * 6);
            for (int i = 0; i < cntp; i++) {
                if (hash01(i, 7) < tt * 1.1) continue;
                double a = i * 2 * G_PI / cntp;
                int x = (int)(cgx + cos(a) * rr), y = (int)(cgy + sin(a) * rr);
                if (x >= 0 && y >= 0 && x < n && y < n && G[y * n + x] != C_FILL) G[y * n + x] = C_WHITE;
            }
        }
    }

    GdkRGBA col[C_N]; double ma;
    face_colors(v, f, &col[C_FILL], &col[C_RING], &col[C_MARK], &ma);
    arm_colors(v, f->lit, &col[C_ARM_A], &col[C_ARM_B]);
    col[C_WHITE] = WHITE;
    mix(&col[C_SPARK], &v->glow, &WHITE, 0.3);
    guint32 lut[C_N] = { 0 };
    for (int c = 1; c < C_N; c++) {
        double a = c == C_MARK ? ma : 1;
        GdkRGBA k = col[c];
        if (c == C_MARK) mix(&k, &col[C_FILL], &k, a);   /* pulse without transparency */
        lut[c] = 0xff000000u | ((guint32)(k.red * 255) << 16) | ((guint32)(k.green * 255) << 8) | (guint32)(k.blue * 255);
    }
    if (!v->cells || cairo_image_surface_get_width(v->cells) != n) {
        if (v->cells) cairo_surface_destroy(v->cells);
        v->cells = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, n, n);
    }
    cairo_surface_flush(v->cells);
    guint8 *data = cairo_image_surface_get_data(v->cells);
    int stride = cairo_image_surface_get_stride(v->cells);
    for (int y = 0; y < n; y++) {
        guint32 *row = (guint32 *)(data + y * stride);
        for (int x = 0; x < n; x++) row[x] = lut[G[y * n + x]];
    }
    cairo_surface_mark_dirty(v->cells);
    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, px, px);
    cairo_set_source_surface(cr, v->cells, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    if (px >= 4) {   /* 1px gaps between big pixels */
        int sz = (int)(n * px);
        if (!v->gridl || v->cache_size != (int)f->S) {
            if (v->gridl) cairo_surface_destroy(v->gridl);
            v->gridl = similar(cr, CAIRO_CONTENT_ALPHA, sz, sz);
            cairo_t *g = cairo_create(v->gridl);
            /* gaps sit on the side of each cell facing away from the centre,
             * so the whole picture mirrors exactly around the middle */
            for (int i = 0; i < n; i++) {
                double gx = i < n / 2 ? i * px : (i + 1) * px - 1;
                cairo_rectangle(g, gx, 0, 1, sz);
                cairo_rectangle(g, 0, gx, sz, 1);
            }
            cairo_fill(g);
            cairo_destroy(g);
        }
        src(cr, &v->bg, 1);
        cairo_mask_surface(cr, v->gridl, ox, oy);
    }
#undef GX
#undef GY
}

/* ---------- SKETCH (Notebook) ---------- */

static void sketch_arms(cairo_t *cr, Vortex *v, const Frame *f)
{
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    GdkRGBA ca, cb;
    arm_colors(v, f->lit, &ca, &cb);
    /* on paper, "off" means pencil grey rather than dark */
    GdkRGBA grey; mix(&grey, &v->ink, &v->bg, 0.55);
    mix(&cb, &grey, &v->ink, f->lit);
    mix(&ca, &grey, &v->glow, f->lit);
    for (int k = 0; k < 4; k++) {
        const GdkRGBA *c = k % 2 ? &cb : &ca;
        double px = 0, py = 0;
        for (int i = 0; i <= 28; i++) {
            double x, y, w, t = i / 28.0;
            arm_point(v, f, k, 4, t, &x, &y, &w);
            x += sin(t * 11 + k * 1.7) * f->S * 0.003;
            y += cos(t * 9 + k) * f->S * 0.003;
            if (i) {
                cairo_set_line_width(cr, MAX(w * 0.62, 1.2));
                src(cr, c, 1);
                cairo_move_to(cr, px, py);
                cairo_line_to(cr, x, y);
                cairo_stroke(cr);
            }
            px = x; py = y;
        }
        cairo_set_line_width(cr, MAX(f->S * 0.004, 1));
        src(cr, c, 0.55);
        for (int i = 0; i <= 20; i++) {
            double x, y, w, t = 0.08 + i / 24.0;
            arm_point(v, f, k, 4, t, &x, &y, &w);
            double dx = x - f->cx, dy = y - f->cy, d = hypot(dx, dy);
            double o = w * 0.55 + f->S * 0.006;
            x += dx / d * o; y += dy / d * o;
            if (i) cairo_line_to(cr, x, y); else cairo_move_to(cr, x, y);
        }
        cairo_stroke(cr);
    }
}

static void sketch_button(cairo_t *cr, Vortex *v, const Frame *f)
{
    GdkRGBA fill, ring, mark; double ma;
    face_colors(v, f, &fill, &ring, &mark, &ma);
    double rb = f->rb * (1 - 0.05 * f->press);
    double sh = f->S * 0.02 * (1 - f->press);
    src(cr, &v->ink, 1);
    cairo_arc(cr, f->cx + sh, f->cy + sh, rb, 0, 2 * G_PI);
    cairo_fill(cr);
    src(cr, &fill, 1);
    cairo_arc(cr, f->cx, f->cy, rb, 0, 2 * G_PI);
    cairo_fill(cr);
    /* the outline is redrawn by the pen during the switch */
    double pen = f->sw >= 0 ? ease_in_out(clamp01(f->sw / 0.55)) : 1;
    cairo_set_line_width(cr, f->S * 0.012);
    src(cr, &v->ink, 1);
    cairo_arc(cr, f->cx, f->cy, rb, -G_PI / 2, -G_PI / 2 + 2 * G_PI * pen);
    cairo_stroke(cr);
    if (f->lit < 0.5 && v->hover > 0.01) {
        double dash[] = { f->S * 0.02, f->S * 0.016 };
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_line_width(cr, f->S * 0.007);
        src(cr, &v->ink, v->hover * 0.6);
        cairo_arc(cr, f->cx, f->cy, rb + f->S * 0.035, 0, 2 * G_PI);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
    double reveal = f->sw >= 0 ? ease_out(clamp01((f->sw - 0.15) / 0.6)) : 1;
    paint_face(cr, v, f, "Comic Neue, Patrick Hand, Comic Sans MS, sans", reveal);
}

static void sketch_switch(cairo_t *cr, Vortex *v, const Frame *f)
{
    if (f->sw < 0) return;
    cairo_new_path(cr);
    double t = f->sw;
    /* pen tip travelling around the rim */
    double pen = ease_in_out(clamp01(t / 0.55));
    if (pen < 1) {
        double a = -G_PI / 2 + 2 * G_PI * pen;
        src(cr, &v->ink, 1);
        cairo_arc(cr, f->cx + cos(a) * f->rb, f->cy + sin(a) * f->rb, f->S * 0.012, 0, 2 * G_PI);
        cairo_fill(cr);
    }
    /* ink splash: dots fly out and dry (shrink) */
    for (int i = 0; i < 9; i++) {
        double tt = clamp01((t - 0.1 - 0.03 * i) / 0.6);
        if (tt <= 0 || tt >= 1) continue;
        double a = i * 2 * G_PI / 9 + hash01(i, 3);
        double r = f->rb * (1.15 + 0.55 * ease_out(tt) * (0.6 + hash01(i, 4)));
        double s = f->S * (0.008 + 0.01 * hash01(i, 5)) * (1 - tt);
        src(cr, i % 3 ? &v->ink : &v->glow, 1);
        cairo_arc(cr, f->cx + cos(a) * r, f->cy + sin(a) * r, s, 0, 2 * G_PI);
        cairo_fill(cr);
    }
}

/* ---------- CRYSTAL (Amethyst) ----------
 * The button is a cut gem seen from above: an octagonal table and eight
 * facets lit from the top-left. The arms are strings of amethyst shards,
 * each split along its axis into a lit and a shaded half. Switching
 * spins the light once around the stone, so the facets catch it one
 * after another, and the octagonal girdle breaks off and flies apart. */

static double crystal_light(const Vortex *v, const Frame *f)
{
    double a = -2.25 + v->hover * 0.5;
    if (f->sw >= 0) a += 2 * G_PI * ease_in_out(clamp01(f->sw / 0.8));
    return a;
}

static void shade(GdkRGBA *o, const GdkRGBA *base, const Vortex *v, double facing, double gain)
{
    if (facing >= 0) mix(o, base, &WHITE, facing * gain);
    else mix(o, base, &v->bg, -facing * 0.45);
}

static void shard(cairo_t *cr, const Vortex *v, double x, double y, double ang, double len, double wid,
                  const GdkRGBA *base, double light, double gain)
{
    double ux = cos(ang), uy = sin(ang), nx = -uy, ny = ux;
    double ax = x + ux * len / 2, ay = y + uy * len / 2, bx = x - ux * len / 2, by = y - uy * len / 2;
    for (int side = -1; side <= 1; side += 2) {
        double fa = cos(atan2(ny * side, nx * side) - light);
        GdkRGBA c; shade(&c, base, v, fa, gain);
        src(cr, &c, 1);
        cairo_move_to(cr, ax, ay);
        cairo_line_to(cr, x + nx * side * wid / 2, y + ny * side * wid / 2);
        cairo_line_to(cr, bx, by);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

static void crystal_arms(cairo_t *cr, Vortex *v, const Frame *f)
{
    GdkRGBA ca, cb;
    arm_colors(v, f->lit, &ca, &cb);
    double light = -2.25;
    for (int k = 0; k < 4; k++) {
        const GdkRGBA *c = k % 2 ? &cb : &ca;
        for (int i = 0; i < 8; i++) {
            double t = (i + 0.5) / 8, x, y, w, x2, y2, w2;
            arm_point(v, f, k, 4, t, &x, &y, &w);
            arm_point(v, f, k, 4, t + 0.02, &x2, &y2, &w2);
            double ang = atan2(y2 - y, x2 - x);
            shard(cr, v, x, y, ang, w * 2.3, w * 0.95, c, light, 0.55);
        }
    }
}

static void octagon(cairo_t *cr, double cx, double cy, double r)
{
    for (int i = 0; i < 8; i++) {
        double a = G_PI / 8 + i * G_PI / 4;
        if (i) cairo_line_to(cr, cx + cos(a) * r, cy + sin(a) * r);
        else cairo_move_to(cr, cx + cos(a) * r, cy + sin(a) * r);
    }
    cairo_close_path(cr);
}

static void crystal_button(cairo_t *cr, Vortex *v, const Frame *f)
{
    GdkRGBA fill, ring, mark; double ma;
    face_colors(v, f, &fill, &ring, &mark, &ma);
    double rb = f->rb * 1.04 * (1 - 0.05 * f->press), rt = rb * 0.64;
    double light = crystal_light(v, f), gain = 0.22 + 0.25 * f->lit;
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    for (int i = 0; i < 8; i++) {
        double a0 = G_PI / 8 + i * G_PI / 4, a1 = a0 + G_PI / 4;
        double facing = cos(a0 + G_PI / 8 - light);
        GdkRGBA c; shade(&c, &fill, v, facing, gain);
        src(cr, &c, 1);
        cairo_move_to(cr, f->cx + cos(a0) * rb, f->cy + sin(a0) * rb);
        cairo_line_to(cr, f->cx + cos(a1) * rb, f->cy + sin(a1) * rb);
        cairo_line_to(cr, f->cx + cos(a1) * rt, f->cy + sin(a1) * rt);
        cairo_line_to(cr, f->cx + cos(a0) * rt, f->cy + sin(a0) * rt);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    GdkRGBA table; mix(&table, &fill, &WHITE, 0.05 + 0.05 * f->lit);
    src(cr, &table, 1);
    octagon(cr, f->cx, f->cy, rt);
    cairo_fill(cr);
    /* facet edges */
    cairo_set_line_width(cr, MAX(1, f->S * 0.003));
    GdkRGBA edge; mix(&edge, &ring, &fill, 0.45);
    src(cr, &edge, 1);
    for (int i = 0; i < 8; i++) {
        double a = G_PI / 8 + i * G_PI / 4;
        cairo_move_to(cr, f->cx + cos(a) * rt, f->cy + sin(a) * rt);
        cairo_line_to(cr, f->cx + cos(a) * rb, f->cy + sin(a) * rb);
    }
    cairo_stroke(cr);
    octagon(cr, f->cx, f->cy, rt);
    cairo_stroke(cr);
    cairo_set_line_width(cr, f->S * 0.007);
    src(cr, &ring, 1);
    octagon(cr, f->cx, f->cy, rb);
    cairo_stroke(cr);
    paint_face(cr, v, f, "sans", 1);
}

/* four-point glints travelling on the shards; drawn live, a few px each */
static void crystal_glints(cairo_t *cr, Vortex *v, const Frame *f)
{
    double now = now_us() / 1e6;
    for (int k = 0; k < 4; k++) {
        double ph = fmod(now * 0.45 + k * 0.27, 1.0);
        if (ph > 0.16) continue;
        double s = sin(G_PI * ph / 0.16) * f->S * 0.022 * (0.4 + 0.6 * f->lit);
        double x, y, w;
        arm_point(v, f, k, 4, 0.18 + 0.1 * (k % 3), &x, &y, &w);
        src(cr, &WHITE, 1);
        for (int d = 0; d < 2; d++) {
            double ux = d ? 0 : 1, uy = d ? 1 : 0;
            cairo_move_to(cr, x - ux * s, y - uy * s);
            cairo_line_to(cr, x + uy * s * 0.18, y + ux * s * 0.18);
            cairo_line_to(cr, x + ux * s, y + uy * s);
            cairo_line_to(cr, x - uy * s * 0.18, y - ux * s * 0.18);
            cairo_close_path(cr);
        }
        cairo_fill(cr);
    }
}

static void crystal_switch(cairo_t *cr, Vortex *v, const Frame *f)
{
    if (f->sw < 0) return;
    cairo_new_path(cr);
    double t = f->sw;
    /* the girdle breaks into its eight edges; each flies out and shortens */
    double rr = f->rb * 1.04 + (f->rout * 1.02 - f->rb) * ease_out(t);
    double keep = 1 - ease_in_out(clamp01((t - 0.1) / 0.9));
    if (keep <= 0.02) return;
    GdkRGBA c; mix(&c, &WHITE, &v->glow, clamp01(t * 1.6));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_width(cr, f->S * 0.009);
    src(cr, &c, 1);
    for (int i = 0; i < 8; i++) {
        double a0 = G_PI / 8 + i * G_PI / 4, a1 = a0 + G_PI / 4, spin = t * 0.5 * (i % 2 ? 1 : -1);
        double mx = (cos(a0) + cos(a1)) / 2 * rr, my = (sin(a0) + sin(a1)) / 2 * rr;
        double hx = (cos(a1) - cos(a0)) / 2 * rr * keep, hy = (sin(a1) - sin(a0)) / 2 * rr * keep;
        double cs = cos(spin), sn = sin(spin);
        double ex = hx * cs - hy * sn, ey = hx * sn + hy * cs;
        cairo_move_to(cr, f->cx + mx - ex, f->cy + my - ey);
        cairo_line_to(cr, f->cx + mx + ex, f->cy + my + ey);
    }
    cairo_stroke(cr);
}

/* ---------- DIAL (Graphite) ---------- */

static void dial_arms(cairo_t *cr, Vortex *v, const Frame *f)
{
    static const double MUL[] = { 1.0, -0.7, 0.45 }, POS[] = { 0.2, 0.52, 0.84 };
    GdkRGBA ca, cb;
    arm_colors(v, f->lit, &ca, &cb);
    const GdkRGBA *cols[] = { &ca, &cb, &v->dim };
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int k = 0; k < 3; k++) {
        double g = f->g;
        double r = lerp(f->rin + (f->rout - f->rin) * POS[k], f->rb * (1.12 + 0.1 * k), clamp01(g));
        if (g < 0) r *= 1 - g * 0.1;
        double len = lerp(1.1 + 0.4 * k, 5.6, clamp01(g));
        double a0 = v->phase * MUL[k] * (1 + clamp01(g)) + k * 2.1;
        cairo_set_line_width(cr, f->S * (0.022 - 0.005 * k));
        for (int s = 0; s < 2; s++) {
            src(cr, cols[k], 1);
            cairo_arc(cr, f->cx, f->cy, r, a0 + s * G_PI, a0 + s * G_PI + len / 2);
            cairo_stroke(cr);
        }
    }
}

static void dial_button(cairo_t *cr, Vortex *v, const Frame *f)
{
    GdkRGBA fill, ring, mark; double ma;
    face_colors(v, f, &fill, &ring, &mark, &ma);
    double rb = f->rb * (1 - 0.05 * f->press);
    src(cr, &fill, 1);
    cairo_arc(cr, f->cx, f->cy, rb, 0, 2 * G_PI);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, f->S * 0.008);
    src(cr, &ring, 1);
    cairo_stroke(cr);
    paint_face(cr, v, f, "sans", 1);
}

static void dial_switch(cairo_t *cr, Vortex *v, const Frame *f)
{
    if (f->sw < 0) return;
    cairo_new_path(cr);
    double t = f->sw;
    double head = -G_PI / 2 + 2 * G_PI * ease_in_out(clamp01(t / 0.6));
    double tail = -G_PI / 2 + 2 * G_PI * ease_in_out(clamp01((t - 0.3) / 0.7));
    double r = f->rb * 1.1;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (head > tail) {
        cairo_set_line_width(cr, f->S * 0.018);
        src(cr, &v->glow, 1);
        cairo_arc(cr, f->cx, f->cy, r, tail, head);
        cairo_stroke(cr);
    }
    /* ticks light up as the head passes and go dark as the tail passes */
    cairo_set_line_width(cr, f->S * 0.008);
    for (int i = 0; i < 24; i++) {
        double a = -G_PI / 2 + i * 2 * G_PI / 24;
        if (a > head || a < tail) continue;
        src(cr, &v->fg, 1);
        cairo_move_to(cr, f->cx + cos(a) * f->rb * 1.24, f->cy + sin(a) * f->rb * 1.24);
        cairo_line_to(cr, f->cx + cos(a) * f->rb * 1.34, f->cy + sin(a) * f->rb * 1.34);
        cairo_stroke(cr);
    }
}

/* ---------- frame ---------- */

static void paint_to_window(GtkWidget *w, cairo_t *cr, Vortex *v);

static void drop_caches(Vortex *v)
{
    g_clear_pointer(&v->arms, cairo_surface_destroy);
    g_clear_pointer(&v->btn, cairo_surface_destroy);
    g_clear_pointer(&v->halo, cairo_surface_destroy);
    g_clear_pointer(&v->gridl, cairo_surface_destroy);
    g_clear_pointer(&v->btn_key, g_free);
}

typedef void (*PaintFn)(cairo_t *, Vortex *, const Frame *);

static void cached(cairo_t *cr, cairo_surface_t **slot, PaintFn fn, Vortex *v, const Frame *f,
                   gboolean rebuild, double rot)
{
    int size = (int)f->S;
    double c = floor(size / 2.0);
    if (!*slot || rebuild) {
        if (!*slot) *slot = similar(cr, CAIRO_CONTENT_COLOR_ALPHA, size, size);
        cairo_t *k = cairo_create(*slot);
        cairo_set_operator(k, CAIRO_OPERATOR_CLEAR);
        cairo_paint(k);
        cairo_set_operator(k, CAIRO_OPERATOR_OVER);
        Frame lf = *f;
        lf.cx = c; lf.cy = c;
        fn(k, v, &lf);
        cairo_destroy(k);
    }
    cairo_save(cr);
    cairo_translate(cr, f->cx, f->cy);
    if (rot != 0) cairo_rotate(cr, rot);
    cairo_translate(cr, -c, -c);
    cairo_set_source_surface(cr, *slot, 0, 0);
    if (rot != 0) cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void paint(cairo_t *cr, Vortex *v, double W, double H, gboolean use_cache)
{
    Frame f = { .W = W, .H = H, .S = MIN(W, H), .cx = floor(W / 2), .cy = floor(H / 2) };
    f.rb = f.S * 0.21;
    f.rin = f.rb * 1.30;
    f.rout = f.S * 0.48;
    f.g = v->g;
    f.on = v->shown_on;
    f.lit = v->lit;
    f.busy = v->st == ST_STARTING || v->st == ST_STOPPING;
    f.press = v->pressed ? 1 : 0;
    f.sw = v->ph == PH_SWITCH ? clamp01((now_us() - v->ph_start) / 1e6 / switch_len(v->style)) : -1;
    cairo_new_path(cr);
    if ((int)f.S != v->cache_size) { drop_caches(v); v->cache_size = (int)f.S; }
    paint_halo(cr, v, &f);
    if (v->style == BTN_PIXEL) { paint_pixel(cr, v, &f); return; }

    PaintFn arms = v->style == BTN_SKETCH ? sketch_arms : v->style == BTN_CRYSTAL ? crystal_arms : dial_arms;
    PaintFn button = v->style == BTN_SKETCH ? sketch_button : v->style == BTN_CRYSTAL ? crystal_button : dial_button;
    /* resting arms are a rigid rotation of one cached image */
    if (use_cache && f.g == 0 && v->style != BTN_DIAL) {
        double p = v->phase;
        v->phase = 0;
        gboolean fresh = v->arms == NULL || fabs(v->arms_lit - f.lit) > 0.004;
        if (fresh) v->arms_lit = f.lit;
        cached(cr, &v->arms, arms, v, &f, fresh, p);
        v->phase = p;
    } else arms(cr, v, &f);

    if (use_cache && !f.busy && f.sw < 0) {
        g_autofree char *key = g_strdup_printf("%s|%d|%d|%d", shown_label(v), (int)round(f.lit * 100),
                                               (int)round(v->hover * 8), (int)f.press);
        gboolean fresh = g_strcmp0(key, v->btn_key) != 0;
        if (fresh) { g_free(v->btn_key); v->btn_key = g_steal_pointer(&key); }
        cached(cr, &v->btn, button, v, &f, fresh, 0);
    } else button(cr, v, &f);

    if (v->style == BTN_SKETCH) sketch_switch(cr, v, &f);
    else if (v->style == BTN_CRYSTAL) { crystal_glints(cr, v, &f); crystal_switch(cr, v, &f); }
    else dial_switch(cr, v, &f);
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer ud)
{
    Vortex *v = V(w);
    paint_to_window(w, cr, v);
    return TRUE;
}

static void paint_to_window(GtkWidget *w, cairo_t *cr, Vortex *v)
{
    paint(cr, v, gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w), TRUE);
}

/* ---------- animation ---------- */

static void apply_label(Vortex *v)
{
    v->shown_on = v->st == ST_ON;
    if (!v->next) return;
    g_free(v->label);
    v->label = g_strdup(v->next);
}

static void enter(Vortex *v, Phase ph, gint64 now)
{
    v->ph = ph;
    v->ph_start = now;
    v->g0 = v->g;
    if (ph == PH_SWITCH) { apply_label(v); v->pending_switch = FALSE; }
}

static void step(Vortex *v, gint64 now)
{
    double dt = v->last ? MIN((now - v->last) / 1e6, 0.1) : 0;
    v->last = now;
    double pt = (now - v->ph_start) / 1e6;
    gboolean busy = v->st == ST_STARTING || v->st == ST_STOPPING;
    double rest = v->st == ST_ON ? 0.9 : 0.4;

    double target = v->ph != PH_IDLE || busy ? 7.5 : rest;
    if (v->ph == PH_RELEASE) target = lerp(7.5, rest, clamp01(pt / T_RELEASE));
    target += v->hover * 0.5;
    v->speed += (target - v->speed) * MIN(dt * 4, 1);
    v->phase += v->speed * dt;
    v->hover += ((v->hovering ? 1 : 0) - v->hover) * MIN(dt * 10, 1);
    /* colours flow over ~0.4 s from the moment the switch fires */
    double lt = v->shown_on ? 1 : 0;
    v->lit += (lt - v->lit) * MIN(dt * 7, 1);
    if (fabs(v->lit - lt) < 0.003) v->lit = lt;

    switch (v->ph) {
    case PH_GATHER:
        v->g = lerp(v->g0, 1, ease_in_out(clamp01(pt / T_GATHER)));
        if (pt >= T_GATHER && !busy) {
            if (v->pending_switch) enter(v, PH_SWITCH, now);
            else { apply_label(v); enter(v, PH_RELEASE, now); }
        }
        break;
    case PH_SWITCH:
        v->g = 1;
        if (pt >= switch_len(v->style)) enter(v, PH_RELEASE, now);
        break;
    case PH_RELEASE:
        v->g = v->g0 * (1 - ease_out_back(clamp01(pt / T_RELEASE)));
        if (pt >= T_RELEASE) { v->g = 0; enter(v, PH_IDLE, now); }
        break;
    default:
        v->g = 0;
        break;
    }
}

/* idle = nothing but the slow orbit; only then may the eco timer take over */
static gboolean is_idle(Vortex *v)
{
    gboolean busy = v->st == ST_STARTING || v->st == ST_STOPPING;
    return v->ph == PH_IDLE && !busy && !v->pressed && fabs(v->hover - (v->hovering ? 1 : 0)) < 0.01 &&
           v->lit == (v->shown_on ? 1 : 0);
}

static void start_ticking(GtkWidget *w);

/* idle orbit: a 20 fps timer instead of the 60 fps frame clock */
static gboolean on_timer(gpointer ud)
{
    GtkWidget *w = ud;
    Vortex *v = V(w);
    if (!gtk_widget_get_mapped(w) || !v->animated) { v->timer = 0; return G_SOURCE_REMOVE; }
    step(v, now_us());
    gtk_widget_queue_draw(w);
    if (!is_idle(v) || !S.eco_fps) { v->timer = 0; start_ticking(w); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static gboolean on_tick(GtkWidget *w, GdkFrameClock *fc, gpointer ud)
{
    Vortex *v = V(w);
    if (!v->animated && v->ph == PH_IDLE) {
        v->tick = 0;
        gtk_widget_queue_draw(w);
        return G_SOURCE_REMOVE;
    }
    step(v, now_us());
    gtk_widget_queue_draw(w);
    if (S.eco_fps && is_idle(v) && v->animated) {
        v->tick = 0;
        if (!v->timer) v->timer = g_timeout_add(50, on_timer, w);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void start_ticking(GtkWidget *w)
{
    Vortex *v = V(w);
    if (!gtk_widget_get_mapped(w)) return;
    if (v->timer) { g_source_remove(v->timer); v->timer = 0; }
    if (!v->tick) v->tick = gtk_widget_add_tick_callback(w, on_tick, NULL, NULL);
}

static void on_map(GtkWidget *w, gpointer ud)
{
    Vortex *v = V(w);
    v->last = 0;
    if (v->animated) start_ticking(w);
}

static void on_unmap(GtkWidget *w, gpointer ud)
{
    Vortex *v = V(w);
    if (v->timer) { g_source_remove(v->timer); v->timer = 0; }
    /* free the buffers while hidden (tray) */
    drop_caches(v);
}

static gboolean inside(GtkWidget *w, double x, double y)
{
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    return hypot(x - W / 2, y - H / 2) < MIN(W, H) * 0.21 * 1.2;
}

static void set_hover(GtkWidget *w, gboolean in)
{
    Vortex *v = V(w);
    if (in == v->hovering) return;
    v->hovering = in;
    GdkWindow *win = gtk_widget_get_window(w);
    g_autoptr(GdkCursor) c = in ? gdk_cursor_new_from_name(gtk_widget_get_display(w), "pointer") : NULL;
    if (win) gdk_window_set_cursor(win, c);
    if (v->animated) start_ticking(w);
    else { v->hover = in; drop_caches(v); gtk_widget_queue_draw(w); }
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer ud)
{
    set_hover(w, inside(w, e->x, e->y));
    return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *e, gpointer ud)
{
    V(w)->pressed = FALSE;
    set_hover(w, FALSE);
    gtk_widget_queue_draw(w);
    return FALSE;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *e, gpointer ud)
{
    if (e->button != 1 || !inside(w, e->x, e->y)) return FALSE;
    V(w)->pressed = TRUE;
    gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *e, gpointer ud)
{
    Vortex *v = V(w);
    if (e->button != 1 || !v->pressed) return FALSE;
    v->pressed = FALSE;
    gtk_widget_queue_draw(w);
    if (inside(w, e->x, e->y) && v->click) v->click();
    return TRUE;
}

static void vortex_free(Vortex *v)
{
    if (v->timer) g_source_remove(v->timer);
    drop_caches(v);
    g_clear_pointer(&v->cells, cairo_surface_destroy);
    g_free(v->label); g_free(v->next); g_free(v->grid);
    g_clear_object(&v->layout);
    g_free(v);
}

static void parse(GdkRGBA *c, const char *s, const char *def)
{
    if (!s || !gdk_rgba_parse(c, s)) gdk_rgba_parse(c, def);
}

static void load_theme(Vortex *v, const Theme *t)
{
    v->style = t->button;
    parse(&v->bg, t->c[TC_BG], "#0c0a10");
    parse(&v->card, t->c[TC_CARD], "#17131f");
    parse(&v->fg, t->c[TC_FG], "#efe6f2");
    parse(&v->dim, t->c[TC_FG_DIM], "#8e7f99");
    parse(&v->glow, t->c[TC_GLOW], "#ca31cc");
    parse(&v->ink, t->c[TC_INK], "#222222");
    parse(&v->line, t->c[TC_LINE], "#2a2233");
    if (fabs(lum(&v->ink) - lum(&v->bg)) > 0.25) v->armb = v->ink;
    else mix(&v->armb, &v->glow, &v->bg, 0.5);
    /* text on the lit button: whichever reads better on the glow colour */
    v->onfg = lum(&v->glow) > 0.55 ? v->bg : WHITE;
}

GtkWidget *vortex_new(void)
{
    GtkWidget *w = gtk_drawing_area_new();
    Vortex *v = g_new0(Vortex, 1);
    v->animated = TRUE;
    v->st = ST_OFF;
    v->speed = 0.4;
    v->phase = 0.6;
    load_theme(v, theme_current());
    g_object_set_data_full(G_OBJECT(w), KEY, v, (GDestroyNotify)vortex_free);
    gtk_widget_add_events(w, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK |
                             GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(w, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(w, "motion-notify-event", G_CALLBACK(on_motion), NULL);
    g_signal_connect(w, "leave-notify-event", G_CALLBACK(on_leave), NULL);
    g_signal_connect(w, "button-press-event", G_CALLBACK(on_press), NULL);
    g_signal_connect(w, "button-release-event", G_CALLBACK(on_release), NULL);
    g_signal_connect_after(w, "map", G_CALLBACK(on_map), NULL);
    g_signal_connect(w, "unmap", G_CALLBACK(on_unmap), NULL);
    return w;
}

void vortex_set_theme(GtkWidget *w, const Theme *t)
{
    drop_caches(V(w));
    load_theme(V(w), t);
    gtk_widget_queue_draw(w);
}

void vortex_on_click(GtkWidget *w, void (*cb)(void)) { V(w)->click = cb; }

/* the label flips at the switch effect, or right away when nothing animates */
void vortex_set_label(GtkWidget *w, const char *text)
{
    Vortex *v = V(w);
    if (!text || !strcmp(text, "...")) return;
    g_free(v->next);
    v->next = g_strdup(text);
    if (!v->label || (!v->animated && v->ph == PH_IDLE)) apply_label(v);
    gtk_widget_queue_draw(w);
}

static void change_state(Vortex *v, CoreState st, gboolean animate)
{
    CoreState prev = v->st;
    v->st = st;
    if (!animate) {
        v->ph = PH_IDLE; v->g = 0; v->pending_switch = FALSE;
        apply_label(v);
        v->lit = v->shown_on;
        return;
    }
    gboolean settled = st == ST_ON || st == ST_OFF;
    gboolean failed = prev == ST_STARTING && st == ST_OFF;
    if (settled) v->pending_switch = !failed;
    if (v->ph == PH_IDLE || v->ph == PH_RELEASE) enter(v, PH_GATHER, now_us());
}

void vortex_set_state(GtkWidget *w, CoreState st)
{
    Vortex *v = V(w);
    if (v->st == st) return;
    change_state(v, st, v->animated && gtk_widget_get_mapped(w));
    drop_caches(v);
    if (v->animated) start_ticking(w);
    gtk_widget_queue_draw(w);
}

void vortex_set_animated(GtkWidget *w, gboolean on)
{
    Vortex *v = V(w);
    v->animated = on;
    if (on) start_ticking(w);
    else {
        if (v->timer) { g_source_remove(v->timer); v->timer = 0; }
        v->ph = PH_IDLE; v->g = 0; v->pending_switch = FALSE;
        apply_label(v);
        v->lit = v->shown_on;
        gtk_widget_queue_draw(w);
    }
}

/* still frame for the theme picker */
void vortex_paint_preview(cairo_t *cr, const Theme *t, double size)
{
    Vortex v = { .st = ST_OFF, .phase = 0.6, .animated = FALSE };
    load_theme(&v, t);
    v.label = g_strdup(N_("ВЫКЛ", "OFF"));
    paint(cr, &v, size, size, FALSE);
    g_free(v.label);
    g_free(v.grid);
    g_clear_pointer(&v.cells, cairo_surface_destroy);
    drop_caches(&v);
    g_clear_object(&v.layout);
}

/* ---------- offline renderer (tools/render.c) ---------- */

gpointer vortex_sim_new(const Theme *t, const char *label)
{
    Vortex *v = g_new0(Vortex, 1);
    v->animated = TRUE; v->st = ST_OFF; v->speed = 0.4; v->phase = 0.6;
    load_theme(v, t);
    v->label = g_strdup(label);
    return v;
}
void vortex_sim_step(gpointer sim, gint64 t_us) { fake_now = t_us; step(sim, t_us); }
void vortex_sim_state(gpointer sim, CoreState st, const char *label)
{
    Vortex *v = sim;
    if (label) { g_free(v->next); v->next = g_strdup(label); }
    change_state(v, st, TRUE);
    drop_caches(v);
}
void vortex_sim_paint(gpointer sim, cairo_t *cr, double size) { paint(cr, sim, size, size, TRUE); }
void vortex_sim_free(gpointer sim) { vortex_free(sim); fake_now = 0; }

/* ---------- app / tray icon: log-spiral whirl, transparent ---------- */

static int field(double n, double x, double y)
{
    double c = (n - 1) / 2, dx = x - c, dy = y - c, r = hypot(dx, dy);
    if (r > n / 2 - 0.3) return 0;
    if (r < n * 0.07) return 3;
    double seg = fmod(4 * atan2(dy, dx) / (2 * G_PI) - 1.55 * log(r + 1), 4.0);
    if (seg < 0) seg += 4;
    int i = (int)seg;
    return (seg - i) < 0.55 ? (i % 2 == 0 ? 1 : 2) : 0;
}

cairo_surface_t *vortex_icon_surface(int cells, int px, const char *ink, const char *glow)
{
    GdkRGBA k, g;
    parse(&k, ink, "#553d63");
    parse(&g, glow, "#ca31cc");
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cells * px, cells * px);
    cairo_t *cr = cairo_create(s);
    for (int y = 0; y < cells; y++)
        for (int x = 0; x < cells; x++) {
            int f = field(cells, x, y);
            if (f == 1 || f == 2) {
                src(cr, f == 2 ? &g : &k, 1);
                cairo_rectangle(cr, x * px, y * px, px, px);
                cairo_fill(cr);
            }
        }
    cairo_destroy(cr);
    return s;
}

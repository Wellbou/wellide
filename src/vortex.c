/* The pixel vortex.
 *
 * A top-down whirl drawn on an NxN grid of square "pixels". Each cell is
 * coloured by a log-spiral field; rotating the field phase makes the arms
 * sweep around, so the pixels look like they are being pulled inwards.
 *
 * Connect/disconnect animation (driven by the frame clock, ~60 fps):
 *   1. spin up   — the phase accelerates, the vortex twists tighter
 *   2. collapse  — every pixel spirals along its own orbit into the eye
 *   3. bloom     — pixels fly back out, landing on the glyph of the new
 *                  label ("ON"/"ВКЛ" ⇄ "OFF"/"ВЫКЛ"), then relax into
 *                  the vortex again with a springy overshoot
 * While connecting it keeps spinning; when connected it breathes slowly.
 *
 * Everything is procedural: no images, a few KB of state. */
#include "wellide.h"
#include <math.h>
#include <string.h>

/* 3x5 pixel font, enough for ON/OFF/ВКЛ/ВЫКЛ/… */
typedef struct { gunichar ch; const char *rows[5]; } Glyph;
static const Glyph FONT[] = {
    { 'O', { "111", "101", "101", "101", "111" } },
    { 'N', { "101", "111", "111", "111", "101" } },
    { 'F', { "111", "100", "110", "100", "100" } },
    { 0x412 /*В*/, { "110", "101", "110", "101", "110" } },
    { 0x41A /*К*/, { "101", "110", "100", "110", "101" } },
    { 0x41B /*Л*/, { "011", "101", "101", "101", "101" } },
    { 0x42B /*Ы*/, { "1001", "1001", "1101", "1011", "1101" } },
    { '.', { "0", "0", "0", "0", "1" } },
    { ' ', { "0", "0", "0", "0", "0" } },
};

typedef enum { PH_IDLE, PH_COLLAPSE, PH_BLOOM, PH_HOLD, PH_RELAX } Phase;

typedef struct {
    int n;
    double *tx, *ty;      /* per-cell current offset (for the fly animation) */
    guint8 *glyph;        /* 1 where the label glyph lights a cell */
    char *label;
    CoreState st;
    Phase ph;
    gint64 ph_start;      /* µs */
    gint64 last;
    double phase;         /* spiral rotation */
    double speed;         /* current angular speed, rad/s */
    double twist;
    double hover;         /* 0..1 eased */
    gboolean hovering;
    gboolean animated;
    guint tick;
    GdkRGBA ink, glow, deep, bg;
} Vortex;

static const char *KEY = "wl-vortex";

static Vortex *V(GtkWidget *w) { return g_object_get_data(G_OBJECT(w), KEY); }

static double ease_in_out(double t) { return t < .5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2; }
static double ease_out_back(double t)
{
    const double c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * pow(t - 1, 3) + c1 * pow(t - 1, 2);
}

static const Glyph *glyph_for(gunichar c)
{
    c = g_unichar_toupper(c);
    for (guint i = 0; i < G_N_ELEMENTS(FONT); i++)
        if (FONT[i].ch == c) return &FONT[i];
    return &FONT[G_N_ELEMENTS(FONT) - 1];
}

/* rasterise the label, centred, scaled to fit */
static void build_glyph(Vortex *v)
{
    memset(v->glyph, 0, v->n * v->n);
    if (!v->label || !*v->label) return;
    int total = 0, count = 0;
    for (const char *p = v->label; *p; p = g_utf8_next_char(p), count++)
        total += strlen(glyph_for(g_utf8_get_char(p))->rows[0]);
    total += count - 1;                       /* 1-cell gaps */
    int scale = MAX(1, MIN((v->n - 2) / MAX(total, 1), (v->n - 4) / 5));
    scale = MIN(scale, 2);
    int w = total * scale, h = 5 * scale;
    int x0 = (v->n - w) / 2, y0 = (v->n - h) / 2;
    int cx = x0;
    for (const char *p = v->label; *p; p = g_utf8_next_char(p)) {
        const Glyph *g = glyph_for(g_utf8_get_char(p));
        int gw = strlen(g->rows[0]);
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < gw; c++)
                if (g->rows[r][c] == '1')
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int x = cx + c * scale + sx, y = y0 + r * scale + sy;
                            if (x >= 0 && y >= 0 && x < v->n && y < v->n) v->glyph[y * v->n + x] = 1;
                        }
        cx += (gw + 1) * scale;
    }
}

/* spiral field value for a cell: 0 = empty, 1 = ink arm, 2 = glow arm, 3 = eye */
static int field(double n, double x, double y, double phase, double twist)
{
    double c = (n - 1) / 2, dx = x - c, dy = y - c, r = hypot(dx, dy);
    if (r > n / 2 - 0.3) return 0;
    if (r < n * 0.07) return 3;
    double th = atan2(dy, dx) + phase;
    double seg = fmod(4 * th / (2 * G_PI) - twist * log(r + 1), 4.0);
    if (seg < 0) seg += 4;
    int i = (int)seg;
    return (seg - i) < 0.55 ? (i % 2 == 0 ? 1 : 2) : 0;
}

static void set_rgba(cairo_t *cr, const GdkRGBA *c, double a)
{
    cairo_set_source_rgba(cr, c->red, c->green, c->blue, c->alpha * a);
}

static void mix(GdkRGBA *o, const GdkRGBA *a, const GdkRGBA *b, double t)
{
    o->red = a->red + (b->red - a->red) * t;
    o->green = a->green + (b->green - a->green) * t;
    o->blue = a->blue + (b->blue - a->blue) * t;
    o->alpha = a->alpha + (b->alpha - a->alpha) * t;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer ud)
{
    Vortex *v = V(w);
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    int n = v->n;
    double cell = floor(MIN(W, H) / (double)n);
    if (cell < 1) return FALSE;
    double ox = floor((W - cell * n) / 2), oy = floor((H - cell * n) / 2);
    double c = (n - 1) / 2.0;
    gint64 now = g_get_monotonic_time();
    double pt = (now - v->ph_start) / 1e6;

    /* phase-dependent blend: 0 = vortex, 1 = glyph */
    double to_glyph = 0, collapse = 0;
    switch (v->ph) {
    case PH_COLLAPSE: collapse = ease_in_out(MIN(pt / 0.42, 1)); break;
    case PH_BLOOM:    collapse = 1 - ease_out_back(MIN(pt / 0.55, 1)); to_glyph = 1; break;
    case PH_HOLD:     to_glyph = 1; break;
    case PH_RELAX:    to_glyph = 1 - ease_in_out(MIN(pt / 0.7, 1)); break;
    default: break;
    }
    if (collapse < 0) collapse = 0;   /* overshoot pushes pixels slightly past home */

    gboolean on = v->st == ST_ON;
    double breathe = on ? 0.5 + 0.5 * sin(now / 1e6 * 2.2) : 0;
    double gap = cell >= 6 ? 1 : 0;   /* crisp pixel grid lines on large sizes */

    /* soft glow under the vortex when connected */
    if (on || v->hover > 0.01) {
        double a = (on ? 0.25 + 0.15 * breathe : 0) + v->hover * 0.12;
        cairo_pattern_t *g = cairo_pattern_create_radial(W / 2.0, H / 2.0, cell * n * 0.2,
                                                         W / 2.0, H / 2.0, cell * n * 0.62);
        cairo_pattern_add_color_stop_rgba(g, 0, v->glow.red, v->glow.green, v->glow.blue, a);
        cairo_pattern_add_color_stop_rgba(g, 1, v->glow.red, v->glow.green, v->glow.blue, 0);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
    }

    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            int f = field(n, x, y, v->phase, v->twist);
            gboolean gl = v->glyph[y * n + x];
            double dx = x - c, dy = y - c, r = hypot(dx, dy);
            if (r > n / 2.0 - 0.3) continue;

            /* colour: vortex colour cross-fading to glyph colour */
            GdkRGBA col = v->bg;
            double alpha = 1;
            if (f == 1) col = v->ink;
            else if (f == 2) col = v->glow;
            else if (f == 3) col = v->ink;
            else alpha = 0;
            if (to_glyph > 0) {
                GdkRGBA gc = gl ? v->glow : v->ink;
                double ga = gl ? 1 : 0.9;
                if (alpha == 0) { col = gc; alpha = ga * to_glyph; }
                else { mix(&col, &col, &gc, to_glyph); alpha = alpha + (ga - alpha) * to_glyph; }
            }
            if (alpha <= 0.01) continue;
            if (on && f == 2 && to_glyph < 0.5) mix(&col, &col, &(GdkRGBA){1, 0.8, 1, 1}, 0.18 * breathe);

            /* position: each pixel orbits into the eye while collapsing */
            double px = x, py = y, sz = 1;
            if (collapse > 0) {
                double th = atan2(dy, dx) + collapse * (2.6 + r * 0.25);
                double rr = r * (1 - collapse);
                px = c + cos(th) * rr;
                py = c + sin(th) * rr;
                sz = 1 - 0.55 * collapse;
            }
            if (v->hover > 0 && to_glyph < 0.5) sz *= 1 - 0.08 * v->hover * (r / (n / 2.0));

            double s = cell * sz;
            /* snap to the pixel grid so it stays crisp when at rest */
            double X = ox + px * cell + (cell - s) / 2, Y = oy + py * cell + (cell - s) / 2;
            if (collapse == 0) { X = round(X); Y = round(Y); }
            set_rgba(cr, &col, alpha);
            cairo_rectangle(cr, X, Y, MAX(s - gap, 1), MAX(s - gap, 1));
            cairo_fill(cr);
        }
    }
    return TRUE;
}

static gboolean on_tick(GtkWidget *w, GdkFrameClock *fc, gpointer ud)
{
    Vortex *v = V(w);
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    double dt = v->last ? (now - v->last) / 1e6 : 0;
    v->last = now;
    dt = MIN(dt, 0.05);
    double pt = (now - v->ph_start) / 1e6;

    /* target angular speed per state */
    double target = v->st == ST_ON ? 0.9 : v->st == ST_OFF ? 0.25 : 5.5;
    if (v->ph == PH_COLLAPSE) target = 9;
    if (!v->animated && v->ph == PH_IDLE && v->st != ST_STARTING && v->st != ST_STOPPING) target = 0;
    v->speed += (target - v->speed) * MIN(dt * 3.5, 1);
    v->phase -= v->speed * dt;
    double tw_target = v->st == ST_STARTING || v->st == ST_STOPPING || v->ph == PH_COLLAPSE ? 2.3 : 1.55;
    v->twist += (tw_target - v->twist) * MIN(dt * 2.5, 1);
    v->hover += ((v->hovering ? 1 : 0) - v->hover) * MIN(dt * 10, 1);

    switch (v->ph) {
    case PH_COLLAPSE: if (pt >= 0.42) { v->ph = PH_BLOOM; v->ph_start = now; build_glyph(v); } break;
    case PH_BLOOM:    if (pt >= 0.55) { v->ph = PH_HOLD; v->ph_start = now; } break;
    case PH_HOLD:     if (pt >= 0.9 && v->st != ST_STARTING && v->st != ST_STOPPING) { v->ph = PH_RELAX; v->ph_start = now; } break;
    case PH_RELAX:    if (pt >= 0.7) { v->ph = PH_IDLE; v->ph_start = now; } break;
    default: break;
    }
    gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static void start_ticking(GtkWidget *w)
{
    Vortex *v = V(w);
    if (!v->tick) {
        v->last = 0;
        v->tick = gtk_widget_add_tick_callback(w, on_tick, NULL, NULL);
    }
}

static gboolean on_cross(GtkWidget *w, GdkEventCrossing *e, gpointer ud)
{
    V(w)->hovering = e->type == GDK_ENTER_NOTIFY;
    return FALSE;
}

static void vortex_free(Vortex *v)
{
    g_free(v->tx); g_free(v->ty); g_free(v->glyph); g_free(v->label);
    g_free(v);
}

static void parse(GdkRGBA *c, const char *s, const char *def)
{
    if (!s || !gdk_rgba_parse(c, s)) gdk_rgba_parse(c, def);
}

GtkWidget *vortex_new(int cells)
{
    GtkWidget *w = gtk_drawing_area_new();
    Vortex *v = g_new0(Vortex, 1);
    v->n = cells;
    v->glyph = g_malloc0(cells * cells);
    v->twist = 1.55;
    v->speed = 0.25;
    v->animated = TRUE;
    v->st = ST_OFF;
    v->ph_start = g_get_monotonic_time();
    parse(&v->ink, NULL, "#222222");
    parse(&v->glow, NULL, "#ca31cc");
    parse(&v->bg, NULL, "#000000");
    g_object_set_data_full(G_OBJECT(w), KEY, v, (GDestroyNotify)vortex_free);
    gtk_widget_add_events(w, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(w, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(w, "enter-notify-event", G_CALLBACK(on_cross), NULL);
    g_signal_connect(w, "leave-notify-event", G_CALLBACK(on_cross), NULL);
    g_signal_connect(w, "realize", G_CALLBACK(start_ticking), NULL);
    return w;
}

void vortex_set_colors(GtkWidget *w, const char *ink, const char *glow, const char *bg)
{
    Vortex *v = V(w);
    parse(&v->ink, ink, "#222222");
    parse(&v->glow, glow, "#ca31cc");
    parse(&v->bg, bg, "#000000");
    gtk_widget_queue_draw(w);
}

void vortex_set_label(GtkWidget *w, const char *text)
{
    Vortex *v = V(w);
    if (!g_strcmp0(v->label, text)) return;
    g_free(v->label);
    v->label = g_strdup(text);
    if (v->ph == PH_HOLD || v->ph == PH_BLOOM) build_glyph(v);
}

void vortex_set_state(GtkWidget *w, CoreState st)
{
    Vortex *v = V(w);
    if (v->st == st) return;
    CoreState prev = v->st;
    v->st = st;
    /* the swirl-to-label transition plays on every settled change */
    gboolean settle = (st == ST_ON || st == ST_OFF) && prev != st;
    if (settle || st == ST_STARTING || st == ST_STOPPING) {
        if (!v->animated) { build_glyph(v); v->ph = PH_HOLD; }
        else if (v->ph == PH_IDLE || v->ph == PH_RELAX || settle) v->ph = PH_COLLAPSE;
        v->ph_start = g_get_monotonic_time();
    }
    gtk_widget_queue_draw(w);
}

void vortex_set_animated(GtkWidget *w, gboolean on)
{
    V(w)->animated = on;
}

/* static render for icons / tray: same field, no animation */
GdkPixbuf *vortex_icon_pixbuf(int cells, int px, const char *ink, const char *glow)
{
    GdkRGBA k, g;
    parse(&k, ink, "#222222");
    parse(&g, glow, "#ca31cc");
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cells * px, cells * px);
    cairo_t *cr = cairo_create(s);
    for (int y = 0; y < cells; y++)
        for (int x = 0; x < cells; x++) {
            int f = field(cells, x, y, 0, 1.55);
            if (!f) continue;
            set_rgba(cr, f == 2 ? &g : &k, 1);
            cairo_rectangle(cr, x * px, y * px, px, px);
            cairo_fill(cr);
        }
    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(s, 0, 0, cells * px, cells * px);
    cairo_surface_destroy(s);
    return pb;
}

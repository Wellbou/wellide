/* Traffic sparkline: last 60 s of up/down speed, pixel-stepped bars that
 * slide smoothly between samples (interpolated at frame rate). */
#include "wellide.h"
#include <math.h>
#include <string.h>

#define SAMPLES 60

typedef struct {
    double up[SAMPLES], down[SAMPLES];
    int head, count;
    gint64 last_push;
    double scale;          /* eased max */
    GdkRGBA cu, cd, cg;
    guint tick;
} Graph;

static const char *KEY = "wl-graph";
static Graph *G(GtkWidget *w) { return g_object_get_data(G_OBJECT(w), KEY); }

static double at(double *a, Graph *g, int i)   /* i = 0 oldest .. count-1 newest */
{
    return a[(g->head - g->count + i + SAMPLES * 2) % SAMPLES];
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer ud)
{
    Graph *g = G(w);
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    const int px = 4;                                     /* pixel size */
    int rows = H / px;

    /* dotted baseline grid */
    cairo_set_source_rgba(cr, g->cg.red, g->cg.green, g->cg.blue, 0.5);
    for (int x = 0; x < W; x += px * 2) {
        cairo_rectangle(cr, x, H - 1, px, 1);
        cairo_rectangle(cr, x, H / 2, px / 2, 1);
    }
    cairo_fill(cr);
    if (g->count < 2) return TRUE;

    double mx = 1;
    for (int i = 0; i < g->count; i++) mx = MAX(mx, MAX(at(g->up, g, i), at(g->down, g, i)));
    if (g->scale <= 0) g->scale = mx;
    g->scale += (mx - g->scale) * 0.08;

    /* sub-sample scroll so the bars glide left between 1 s samples */
    double frac = MIN((g_get_monotonic_time() - g->last_push) / 1e6, 1.0);
    double step = (double)W / (SAMPLES - 1);
    for (int pass = 0; pass < 2; pass++) {
        double *a = pass ? g->up : g->down;
        GdkRGBA *c = pass ? &g->cu : &g->cd;
        for (int i = 0; i < g->count; i++) {
            double x = W - (g->count - 1 - i + frac) * step;
            if (x < -step) continue;
            int hcells = (int)round(at(a, g, i) / g->scale * (rows - 1));
            if (hcells <= 0) continue;
            int bx = (int)floor(x / px) * px;
            double alpha = pass ? 0.95 : 0.55;
            cairo_set_source_rgba(cr, c->red, c->green, c->blue, alpha);
            for (int k = 0; k < hcells; k++)
                cairo_rectangle(cr, bx, H - (k + 1) * px, px - 1, px - 1);
            cairo_fill(cr);
        }
    }
    return TRUE;
}

static gboolean on_tick(GtkWidget *w, GdkFrameClock *fc, gpointer ud)
{
    gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

GtkWidget *graph_new(void)
{
    GtkWidget *w = gtk_drawing_area_new();
    Graph *g = g_new0(Graph, 1);
    gdk_rgba_parse(&g->cu, "#ca31cc");
    gdk_rgba_parse(&g->cd, "#7c4dff");
    gdk_rgba_parse(&g->cg, "#444444");
    g_object_set_data_full(G_OBJECT(w), KEY, g, g_free);
    gtk_widget_set_size_request(w, -1, 56);
    g_signal_connect(w, "draw", G_CALLBACK(on_draw), NULL);
    g->tick = gtk_widget_add_tick_callback(w, on_tick, NULL, NULL);
    return w;
}

void graph_push(GtkWidget *w, gint64 up, gint64 down)
{
    Graph *g = G(w);
    g->up[g->head] = up;
    g->down[g->head] = down;
    g->head = (g->head + 1) % SAMPLES;
    if (g->count < SAMPLES) g->count++;
    g->last_push = g_get_monotonic_time();
}

void graph_clear(GtkWidget *w)
{
    Graph *g = G(w);
    g->count = 0;
    g->scale = 0;
    gtk_widget_queue_draw(w);
}

void graph_set_colors(GtkWidget *w, const char *up, const char *down, const char *grid)
{
    Graph *g = G(w);
    gdk_rgba_parse(&g->cu, up);
    gdk_rgba_parse(&g->cd, down);
    gdk_rgba_parse(&g->cg, grid);
}

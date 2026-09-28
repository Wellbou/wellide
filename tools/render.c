/* Offline renderer: plays the real button animation with a fake clock
 * and writes PNG frames.  make tools/render && tools/render <theme> <outdir> */
#include "../src/wellide.h"

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--stills")) {
        /* one still per built-in theme: half ON (left), card-sized */
        WL_RU = FALSE;
        g_mkdir_with_parents(argv[2], 0755);
        const char *ids[] = { "void", "notebook", "purple", "graphite" };
        for (int i = 0; i < 4; i++) {
            g_free(S.theme); S.theme = g_strdup(ids[i]);
            themes_reload();
            const Theme *t = theme_current();
            gpointer v = vortex_sim_new(t, "OFF");
            vortex_sim_step(v, 1000000);
            vortex_sim_state(v, ST_ON, "ON");
            for (int k = 1; k < 600; k++) vortex_sim_step(v, 1000000 + k * 10000);   /* settle lit */
            int Wd = 450, Hd = 420;
            cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, Wd, Hd);
            cairo_t *cr = cairo_create(cs);
            GdkRGBA bg; gdk_rgba_parse(&bg, t->c[TC_BG]);
            gdk_cairo_set_source_rgba(cr, &bg);
            cairo_paint(cr);
            if (t->ruled) {
                GdkRGBA rl; gdk_rgba_parse(&rl, t->c[TC_RULE]);
                gdk_cairo_set_source_rgba(cr, &rl);
                for (int y = 27; y < Hd; y += 28) cairo_rectangle(cr, 0, y, Wd, 1);
                cairo_fill(cr);
            }
            cairo_translate(cr, (Wd - Hd) / 2.0, 0);
            vortex_sim_paint(v, cr, Hd);
            cairo_destroy(cr);
            g_autofree char *p = g_strdup_printf("%s/%s.png", argv[2], ids[i]);
            cairo_surface_write_to_png(cs, p);
            cairo_surface_destroy(cs);
            vortex_sim_free(v);
        }
        return 0;
    }
    const char *tid = argc > 1 ? argv[1] : "void";
    const char *out = argc > 2 ? argv[2] : "/tmp/frames";
    int size = argc > 3 ? atoi(argv[3]) : 320;
    double fps = argc > 4 ? atof(argv[4]) : 25;
    WL_RU = FALSE;
    g_mkdir_with_parents(out, 0755);
    g_free(S.theme); S.theme = g_strdup(tid);
    themes_reload();
    const Theme *t = theme_current();
    gpointer v = vortex_sim_new(t, "OFF");
    GdkRGBA bg; gdk_rgba_parse(&bg, t->c[TC_BG]);
    /* script: idle 1.2s → click (starting) → on at +0.9s → idle 1.6s → click off → idle */
    double T = 7.0;
    gint64 t0 = 1000000;
    int fi = 0;
    int ev = 0;
    for (double s = 0; s <= T; s += 1.0 / 240) {
        if (ev == 0 && s >= 1.2) { vortex_sim_state(v, ST_STARTING, NULL); ev++; }
        if (ev == 1 && s >= 2.0) { vortex_sim_state(v, ST_ON, "ON"); ev++; }
        if (ev == 2 && s >= 4.4) { vortex_sim_state(v, ST_STOPPING, NULL); ev++; }
        if (ev == 3 && s >= 4.6) { vortex_sim_state(v, ST_OFF, "OFF"); ev++; }
        vortex_sim_step(v, t0 + (gint64)(s * 1e6));
        if (s + 1e-9 >= fi / fps) {
            cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
            cairo_t *cr = cairo_create(cs);
            gdk_cairo_set_source_rgba(cr, &bg);
            cairo_paint(cr);
            vortex_sim_paint(v, cr, size);
            cairo_destroy(cr);
            g_autofree char *p = g_strdup_printf("%s/f%03d.png", out, fi);
            cairo_surface_write_to_png(cs, p);
            cairo_surface_destroy(cs);
            fi++;
        }
    }
    vortex_sim_free(v);
    printf("%d frames\n", fi);
    return 0;
}

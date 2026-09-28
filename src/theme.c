/* Themes.
 *
 * Built-in themes are tables below. Custom themes are plain key files in
 *   ~/.config/wellide/themes/<id>.ini   (Windows: %LOCALAPPDATA%\wellide\themes)
 * and are hot-reloaded when saved. Format (all keys optional):
 *
 *   [theme]
 *   name=Midnight
 *   base=purple          ; inherit every key you don't set
 *   bg=#101018           ; window background
 *   bg2=#16161f          ; sidebar
 *   card=#1d1d29         ; cards, entries
 *   fg=#e8e8f0           ; text
 *   fg-dim=#8a8aa0       ; secondary text
 *   accent=#7c4dff       ; buttons, active nav, progress
 *   accent-fg=#ffffff    ; text on accent
 *   line=#2a2a3a         ; borders, switch track
 *   danger=#ff5277       ; errors, bad ping
 *   ink=#222222          ; dark pixels of the vortex button
 *   glow=#ca31cc         ; bright pixels of the vortex button
 *   ruled=false          ; notebook ruling on the main area
 *   rule-color=#c7d8f4
 *   margin-color=#e05a5a
 *   outline=false        ; hard ink outlines + offset shadows ("paper" look)
 *   radius=14            ; corner radius of cards, px
 *   pixel=false          ; square corners + stepped pixel borders everywhere
 *
 *   [css]
 *   extra=.h1 { letter-spacing: 2px; }   ; appended GTK CSS
 *   file=midnight.css                    ; or a CSS file next to the .ini
 */
#include "wellide.h"
#include <string.h>

static GPtrArray *THEMES;
static GFileMonitor *mon;
static void (*on_change)(void);

static const char *KEYS[] = { "bg", "bg2", "card", "fg", "fg-dim", "accent", "accent-fg",
                              "line", "danger", "ink", "glow", "rule-color", "margin-color", NULL };

static void theme_free(Theme *t)
{
    g_free(t->id); g_free(t->name); g_free(t->css_extra);
    for (int i = 0; i < TC_N; i++) g_free(t->c[i]);
    g_free(t);
}

static Theme *mk(const char *id, const char *name, const char *const cols[TC_N],
                 gboolean ruled, gboolean outline, gboolean pixel, int radius)
{
    Theme *t = g_new0(Theme, 1);
    t->id = g_strdup(id);
    t->name = g_strdup(name);
    for (int i = 0; i < TC_N; i++) t->c[i] = g_strdup(cols[i]);
    t->ruled = ruled;
    t->outline = outline;
    t->pixel = pixel;
    t->radius = radius;
    t->builtin = TRUE;
    return t;
}

static void add_builtins(void)
{
    /* order matches ThemeColor */
    /* Void: the icon's palette — near-black ink, magenta glow, pixel edges */
    static const char *voidp[TC_N] = {
        "#0c0a10", "#110e17", "#17131f", "#efe6f2", "#8e7f99", "#ca31cc", "#ffffff",
        "#2a2233", "#ff4f7b", "#222222", "#ca31cc", "#000000", "#000000" };
    static const char *notebook[TC_N] = {
        "#fdfdf8", "#f6f6ee", "#ffffff", "#1d2433", "#6b7385", "#1d2433", "#ffffff",
        "#c9cfdb", "#d0342c", "#222222", "#ca31cc", "#c7d8f4", "#e05a5a" };
    static const char *purple[TC_N] = {
        "#130a22", "#1c1030", "#241540", "#ede7f6", "#a594c4", "#ab47bc", "#ffffff",
        "#3b2960", "#ff5277", "#2a1f38", "#ca31cc", "#000000", "#000000" };
    static const char *graphite[TC_N] = {
        "#1b1b1d", "#222225", "#2a2a2e", "#ececec", "#9a9aa2", "#ca31cc", "#ffffff",
        "#3a3a40", "#ff5c5c", "#101012", "#ca31cc", "#000000", "#000000" };
    g_ptr_array_add(THEMES, mk("void", "Void", voidp, FALSE, FALSE, TRUE, 0));
    g_ptr_array_add(THEMES, mk("notebook", N_("Тетрадь", "Notebook"), notebook, TRUE, TRUE, FALSE, 4));
    g_ptr_array_add(THEMES, mk("purple", N_("Фиолетовая", "Purple"), purple, FALSE, FALSE, FALSE, 14));
    g_ptr_array_add(THEMES, mk("graphite", N_("Графит", "Graphite"), graphite, FALSE, FALSE, FALSE, 10));
}

char *themes_dir(void)
{
    g_autofree char *d = wl_config_dir();
    char *td = g_build_filename(d, "themes", NULL);
    g_mkdir_with_parents(td, 0700);
    return td;
}

static Theme *find(const char *id)
{
    for (guint i = 0; THEMES && id && i < THEMES->len; i++) {
        Theme *t = THEMES->pdata[i];
        if (!strcmp(t->id, id)) return t;
    }
    return NULL;
}

static gboolean valid_color(const char *s)
{
    GdkRGBA c;
    return s && gdk_rgba_parse(&c, s);
}

static void load_user_theme(const char *dir, const char *file)
{
    g_autofree char *path = g_build_filename(dir, file, NULL);
    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) return;

    g_autofree char *id = g_strndup(file, strlen(file) - 4);
    g_autofree char *base_id = g_key_file_get_string(kf, "theme", "base", NULL);
    Theme *base = find(base_id ? base_id : "void");
    if (!base) base = THEMES->pdata[0];

    Theme *t = g_new0(Theme, 1);
    t->id = g_strdup(id);
    char *name = g_key_file_get_string(kf, "theme", "name", NULL);
    t->name = name ? name : g_strdup(id);
    for (int i = 0; i < TC_N; i++) {
        g_autofree char *v = g_key_file_get_string(kf, "theme", KEYS[i], NULL);
        t->c[i] = g_strdup(valid_color(v) ? v : base->c[i]);
    }
    GError *e = NULL;
    t->ruled = g_key_file_get_boolean(kf, "theme", "ruled", &e);
    if (e) { t->ruled = base->ruled; g_clear_error(&e); }
    t->outline = g_key_file_get_boolean(kf, "theme", "outline", &e);
    if (e) { t->outline = base->outline; g_clear_error(&e); }
    t->pixel = g_key_file_get_boolean(kf, "theme", "pixel", &e);
    if (e) { t->pixel = base->pixel; g_clear_error(&e); }
    t->radius = g_key_file_get_integer(kf, "theme", "radius", &e);
    if (e) { t->radius = base->radius; g_clear_error(&e); }
    t->radius = CLAMP(t->radius, 0, 40);

    GString *css = g_string_new(NULL);
    g_autofree char *extra = g_key_file_get_string(kf, "css", "extra", NULL);
    if (extra) g_string_append_printf(css, "%s\n", extra);
    g_autofree char *cf = g_key_file_get_string(kf, "css", "file", NULL);
    if (cf && !strchr(cf, '/') && !strchr(cf, '\\')) {
        g_autofree char *cp = g_build_filename(dir, cf, NULL);
        g_autofree char *body = NULL;
        if (g_file_get_contents(cp, &body, NULL, NULL)) g_string_append(css, body);
    }
    t->css_extra = g_string_free(css, FALSE);

    Theme *old = find(id);
    if (old) g_ptr_array_remove(THEMES, old);  /* user file overrides a built-in */
    g_ptr_array_add(THEMES, t);
}

static void write_example(const char *dir)
{
    g_autofree char *p = g_build_filename(dir, "example.ini.sample", NULL);
    if (g_file_test(p, G_FILE_TEST_EXISTS)) return;
    const char *body =
        "; Copy to <name>.ini to create a theme. Wellide reloads it on save.\n"
        "[theme]\n"
        "name=Midnight\n"
        "base=purple\n"
        "bg=#0d0f1a\n"
        "bg2=#12152a\n"
        "card=#181c36\n"
        "accent=#5c6cff\n"
        "glow=#8fa2ff\n"
        "ink=#1a1d33\n"
        "radius=18\n"
        "\n"
        "[css]\n"
        "extra=.brand { letter-spacing: 3px; }\n";
    g_file_set_contents(p, body, -1, NULL);
}

void themes_reload(void)
{
    if (THEMES) g_ptr_array_unref(THEMES);
    THEMES = g_ptr_array_new_with_free_func((GDestroyNotify)theme_free);
    add_builtins();
    g_autofree char *dir = themes_dir();
    write_example(dir);
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *f;
    while (d && (f = g_dir_read_name(d)))
        if (g_str_has_suffix(f, ".ini")) load_user_theme(dir, f);
    if (d) g_dir_close(d);
}

static guint reload_id;

static gboolean do_reload(gpointer ud)
{
    reload_id = 0;
    themes_reload();
    if (on_change) on_change();
    return G_SOURCE_REMOVE;
}

static void on_dir_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer ud)
{
    if (reload_id) g_source_remove(reload_id);
    reload_id = g_timeout_add(300, do_reload, NULL);
}

void themes_watch(void (*cb)(void))
{
    on_change = cb;
    if (mon) return;
    g_autofree char *dir = themes_dir();
    g_autoptr(GFile) gf = g_file_new_for_path(dir);
    mon = g_file_monitor_directory(gf, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (mon) g_signal_connect(mon, "changed", G_CALLBACK(on_dir_changed), NULL);
}

GPtrArray *themes_list(void)
{
    if (!THEMES) themes_reload();
    return THEMES;
}

const Theme *theme_current(void)
{
    if (!THEMES) themes_reload();
    Theme *t = find(S.theme);
    return t ? t : THEMES->pdata[0];
}

char *theme_css(const Theme *t)
{
#define C(i) t->c[i]
    GString *s = g_string_new(NULL);
    g_string_append_printf(s,
        "* { outline-width: 0; }\n"
        /* smooth state changes everywhere; GTK animates colour/shadow/size */
        "button, .nav, row, entry, switch, switch slider, progressbar progress, .card {"
        "   transition: all 180ms cubic-bezier(0.2, 0.8, 0.2, 1); }\n"
        "row.server:hover { margin-left: 4px; }\n"
        "window, .main-bg { background-color: %s; color: %s; }\n"
        "label { color: %s; }\n"
        ".dim, .dim label { color: %s; }\n"
        ".sidebar { background-color: %s; padding: 14px 8px; }\n",
        C(TC_BG), C(TC_FG), C(TC_FG), C(TC_FG_DIM), C(TC_BG2));
    g_string_append_printf(s,
        ".brand { font-size: 19px; font-weight: 800; margin: 2px 8px 16px 8px; }\n"
        ".nav { background: none; border: none; box-shadow: none; border-radius: %dpx;"
        "       padding: 9px 14px; color: %s; font-weight: 600; }\n"
        ".nav label { color: %s; }\n"
        ".nav:hover { background-color: alpha(%s, 0.12); }\n"
        ".nav.active { background-color: %s; }\n"
        ".nav.active label { color: %s; }\n",
        MIN(t->radius, 12), C(TC_FG_DIM), C(TC_FG_DIM), C(TC_ACCENT), C(TC_ACCENT), C(TC_ACCENT_FG));
    g_string_append_printf(s,
        ".card { background-color: %s; border-radius: %dpx; padding: 14px 16px; }\n"
        ".h1 { font-size: 22px; font-weight: 800; }\n"
        ".h2 { font-size: 14px; font-weight: 700; }\n"
        ".big { font-size: 17px; font-weight: 800; }\n"
        ".mono { font-family: monospace; font-size: 11px; }\n"
        ".status-on { color: %s; }\n"
        ".status-off { color: %s; }\n"
        ".error { color: %s; }\n",
        C(TC_CARD), t->radius, C(TC_GLOW), C(TC_FG_DIM), C(TC_DANGER));
    g_string_append_printf(s,
        "button.flat-btn, button.accent-btn, combobox button, spinbutton button {"
        "   background-image: none; box-shadow: none; text-shadow: none; border-radius: %dpx; }\n"
        "button.flat-btn { background-color: %s; color: %s; border: 1px solid %s; padding: 5px 12px; }\n"
        "button.flat-btn label { color: %s; }\n"
        "button.flat-btn:hover { background-color: alpha(%s, 0.14); }\n"
        "button.accent-btn { background-color: %s; border: none; padding: 5px 14px; font-weight: 700; }\n"
        "button.accent-btn label { color: %s; }\n"
        "button.accent-btn:hover { background-color: shade(%s, 1.15); }\n"
        "button.icon-btn { padding: 2px 8px; min-width: 0; }\n",
        MIN(t->radius, 10), C(TC_CARD), C(TC_FG), C(TC_LINE), C(TC_FG), C(TC_ACCENT),
        C(TC_ACCENT), C(TC_ACCENT_FG), C(TC_ACCENT));
    g_string_append_printf(s,
        "entry, textview text, spinbutton, combobox button { background-color: %s; color: %s;"
        "   border-radius: %dpx; border: 1px solid %s; box-shadow: none; }\n"
        "textview, textview text { background-color: %s; }\n"
        "list, row { background-color: transparent; }\n"
        "row.server { border-radius: %dpx; padding: 6px 10px; margin: 1px 0; }\n"
        "row.server:hover { background-color: alpha(%s, 0.10); }\n"
        "row.server.selected-srv { background-color: alpha(%s, 0.20); }\n"
        "row.sep { padding: 10px 10px 2px 10px; }\n"
        "row.sep label { color: %s; font-weight: 700; font-size: 11px; }\n"
        ".ping-good { color: #2fae6e; font-weight: 700; }\n"
        ".ping-mid { color: #d99a20; font-weight: 700; }\n"
        ".ping-bad { color: %s; font-weight: 700; }\n",
        C(TC_CARD), C(TC_FG), MIN(t->radius, 8), C(TC_LINE), C(TC_CARD), MIN(t->radius, 10),
        C(TC_ACCENT), C(TC_ACCENT), C(TC_FG_DIM), C(TC_DANGER));
    g_string_append_printf(s,
        "progressbar trough { background-color: %s; border-radius: 6px; min-height: 8px; border: none; }\n"
        "progressbar progress { background-color: %s; border-radius: 6px; min-height: 8px; border: none; }\n"
        "switch { background-color: %s; border: none; }\n"
        "switch:checked { background-color: %s; }\n"
        "switch slider { background-color: #ffffff; border: none; }\n"
        "scrollbar, scrolledwindow, viewport { background-color: transparent; border: none; }\n"
        ".toast { background-color: %s; border-radius: 10px; padding: 8px 14px; }\n"
        ".toast label { color: %s; }\n"
        "menu, .menu, popover { background-color: %s; color: %s; }\n",
        C(TC_LINE), C(TC_ACCENT), C(TC_LINE), C(TC_ACCENT), C(TC_FG), C(TC_BG), C(TC_CARD), C(TC_FG));

    if (t->ruled) {
        /* notebook paper: ruling every 28px, red margin line */
        g_string_append_printf(s,
            ".main-bg { background-image:"
            "  linear-gradient(to right, transparent 46px, alpha(%s,0.6) 46px, alpha(%s,0.6) 48px, transparent 48px),"
            "  repeating-linear-gradient(to bottom, transparent 0px, transparent 27px, %s 27px, %s 28px); }\n",
            C(TC_MARGIN), C(TC_MARGIN), C(TC_RULE), C(TC_RULE));
    }
    if (t->outline) {
        g_string_append_printf(s,
            ".sidebar { border-right: 2px solid %s; }\n"
            ".card { border: 2px solid %s; box-shadow: 3px 3px 0 %s; }\n"
            "button.flat-btn { border: 2px solid %s; }\n"
            "button.accent-btn { border: 2px solid %s; }\n"
            ".toast { border: 2px solid %s; }\n",
            C(TC_FG), C(TC_FG), C(TC_FG), C(TC_FG), C(TC_FG), C(TC_FG));
    }
    if (t->pixel) {
        /* hard edges: square corners, 2px borders, offset "pixel" shadows */
        g_string_append_printf(s,
            "* { border-radius: 0; }\n"
            ".card { border: 2px solid %s; box-shadow: 4px 4px 0 %s; }\n"
            ".card:hover { box-shadow: 4px 4px 0 alpha(%s, 0.35); }\n"
            "button.flat-btn { border: 2px solid %s; box-shadow: 2px 2px 0 %s; }\n"
            "button.flat-btn:hover { border-color: %s; box-shadow: 2px 2px 0 alpha(%s,0.5); }\n"
            "button.flat-btn:active { box-shadow: none; margin: 2px -2px -2px 2px; }\n"
            "button.accent-btn { box-shadow: 3px 3px 0 %s; }\n"
            "button.accent-btn:active { box-shadow: none; margin: 3px -3px -3px 3px; }\n"
            ".nav.active { box-shadow: 3px 3px 0 %s; }\n"
            "entry, spinbutton, combobox button { border: 2px solid %s; }\n"
            "entry:focus { border-color: %s; }\n"
            ".sidebar { border-right: 2px solid %s; }\n"
            "progressbar trough, progressbar progress { min-height: 10px; }\n"
            ".brand, .h1 { font-family: monospace; letter-spacing: 1px; }\n"
            ".toast { border: 2px solid %s; box-shadow: 4px 4px 0 %s; }\n",
            C(TC_LINE), C(TC_INK), C(TC_ACCENT), C(TC_LINE), C(TC_INK), C(TC_ACCENT), C(TC_ACCENT),
            C(TC_INK), C(TC_INK), C(TC_LINE), C(TC_ACCENT), C(TC_LINE), C(TC_ACCENT), C(TC_INK));
    }
    if (t->css_extra) g_string_append(s, t->css_extra);
#undef C
    return g_string_free(s, FALSE);
}

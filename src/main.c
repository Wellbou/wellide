/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Wellide — lightweight sing-box GUI (GTK3). */
#include "wellide.h"
#include <string.h>
#include <math.h>
#ifndef G_OS_WIN32
#include <unistd.h>
#endif
#ifdef HAVE_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif

static GtkApplication *app;
static GtkWidget *win, *stack, *toast_rev, *toast_lbl;
static GtkCssProvider *css;
static GHashTable *nav_btns;       /* page -> button */
static gboolean have_tray;
#ifdef HAVE_APPINDICATOR
static AppIndicator *tray;
static GtkWidget *tray_toggle;
#else
static GtkStatusIcon *tray;
#endif

/* home */
static GtkWidget *h_vortex, *h_status, *h_err, *h_server, *h_ip, *h_speed, *h_total, *h_graph,
                 *h_prof_name, *h_prof_usage, *h_prof_bar, *h_prof_exp, *h_mode;
static GtkWidget *brand_icon;
/* proxies */
static GtkWidget *p_list, *p_title;
/* profiles */
static GtkWidget *pr_list, *pr_entry, *pr_add;
/* logs */
static GtkTextBuffer *log_buf;
static int log_lines;
/* settings */
static GtkWidget *st_tun_btn, *st_tun_lbl, *st_themes;

static gboolean quitting;
static guint ping_timer;

static void tray_update(void);
#ifdef HAVE_APPINDICATOR
static void tray_theme_changed(void);
#endif
static void rebuild_proxies(void);
static void rebuild_profiles(void);
static void refresh_home(void);
static void schedule_ping(guint delay_s);
static G_GNUC_UNUSED gboolean on_signal(gpointer ud);

/* ---------- small helpers ---------- */

static GtkWidget *label(const char *text, const char *cls)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (cls) gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    return l;
}

static void add_class(GtkWidget *w, const char *c)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(w), c);
}

static void set_class(GtkWidget *w, const char *c, gboolean on)
{
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    if (on) gtk_style_context_add_class(sc, c);
    else gtk_style_context_remove_class(sc, c);
}

static GtkWidget *btn(const char *text, const char *cls, GCallback cb, gpointer ud)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    add_class(b, cls ? cls : "flat-btn");
    if (cb) g_signal_connect(b, "clicked", cb, ud);
    return b;
}

static GtkWidget *card(void)
{
    GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    add_class(c, "card");
    return c;
}

static guint toast_timer;

static gboolean hide_toast(gpointer ud)
{
    toast_timer = 0;
    gtk_revealer_set_reveal_child(GTK_REVEALER(toast_rev), FALSE);
    return G_SOURCE_REMOVE;
}

void ui_toast(const char *msg)
{
    if (!toast_rev) return;
    gtk_label_set_text(GTK_LABEL(toast_lbl), msg);
    gtk_revealer_set_reveal_child(GTK_REVEALER(toast_rev), TRUE);
    if (toast_timer) g_source_remove(toast_timer);
    toast_timer = g_timeout_add_seconds(3, hide_toast, NULL);
}

static void apply_theme(void)
{
    const Theme *t = theme_current();
    g_autofree char *c = theme_css(t);
    gtk_css_provider_load_from_data(css, c, -1, NULL);
    if (h_vortex) vortex_set_theme(h_vortex, t);
#ifdef HAVE_APPINDICATOR
    tray_theme_changed();
#endif
    tray_update();
    if (brand_icon) {
        cairo_surface_t *cs = vortex_icon_surface(16, 2, "#6a4a7a", t->c[TC_GLOW]);
        gtk_image_set_from_surface(GTK_IMAGE(brand_icon), cs);
        cairo_surface_destroy(cs);
    }
    if (h_graph) graph_set_colors(h_graph, t->c[TC_GLOW], t->c[TC_ACCENT], t->c[TC_LINE]);
}

static const char *mode_name(int m)
{
    return m == MODE_TUN   ? N_("TUN — весь трафик", "TUN — all traffic")
         : m == MODE_PROXY ? N_("Системный прокси", "System proxy")
                           : N_("Только локальный порт", "Local port only");
}

/* the button shows the state, not the action */
static const char *on_label(CoreState st)
{
    return st == ST_ON ? N_("ВКЛ", "ON") : st == ST_OFF ? N_("ВЫКЛ", "OFF") : "...";
}

/* ---------- navigation ---------- */

static void go(const char *page)
{
    gtk_stack_set_visible_child_name(GTK_STACK(stack), page);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, nav_btns);
    while (g_hash_table_iter_next(&it, &k, &v))
        set_class(v, "active", !strcmp(k, page));
}

static void on_nav(GtkButton *b, gpointer page) { go(page); }

/* ---------- core callbacks ---------- */

#ifdef HAVE_APPINDICATOR
/* Tray icons are rendered by the app into ~/.cache/wellide/tray/ with the
 * current theme colours. Plasma caches icons by name, so we pass an
 * absolute file path whose name alternates on every theme change: a new
 * path is always reloaded. PNGs are written with cairo, not gdk-pixbuf:
 * newer gdk-pixbuf routes image work through sandboxed glycin helper
 * processes, ~40 MB for a 64-pixel icon. */
static char *tray_dir;
static guint tray_gen;

static const char *tray_icon(gboolean on)
{
    static char path[512];
    const Theme *t = theme_current();
    if (!tray_dir) {
        g_autofree char *c = wl_cache_dir();
        tray_dir = g_build_filename(c, "tray", NULL);
        g_mkdir_with_parents(tray_dir, 0700);
    }
    g_snprintf(path, sizeof path, "%s/wellide-tray-%s-%u.png", tray_dir, on ? "on" : "off", tray_gen & 1);
    GdkRGBA g;
    if (!gdk_rgba_parse(&g, t->c[TC_GLOW])) gdk_rgba_parse(&g, "#ca31cc");
    if (!on) {   /* grey when off */
        double l = 0.3 * g.red + 0.59 * g.green + 0.11 * g.blue;
        g.red = g.green = g.blue = 0.35 + l * 0.4;
    }
    g_autofree char *glow = gdk_rgba_to_string(&g);
    cairo_surface_t *cs = vortex_icon_surface(16, 4, on ? "#6a4a7a" : "#5a5a5a", glow);
    cairo_surface_write_to_png(cs, path);
    cairo_surface_destroy(cs);
    return path;
}

static void tray_theme_changed(void) { tray_gen++; }
#endif

static void tray_update(void)
{
    if (!tray) return;
    CoreState st = core_state();
#ifdef HAVE_APPINDICATOR
    gtk_menu_item_set_label(GTK_MENU_ITEM(tray_toggle),
        st == ST_ON ? N_("Отключиться", "Disconnect") : st == ST_OFF ? N_("Подключиться", "Connect") : "…");
    app_indicator_set_icon_full(tray, tray_icon(st == ST_ON),
                                st == ST_ON ? N_("Подключено", "Connected") : N_("Отключено", "Disconnected"));
#else
    gtk_status_icon_set_tooltip_text(tray, st == ST_ON ? "Wellide — ON" : "Wellide — OFF");
#endif
}

void ui_on_state(CoreState st, const char *error)
{
    if (quitting && st == ST_OFF) { g_application_quit(G_APPLICATION(app)); return; }
    const char *txt = st == ST_ON       ? N_("Подключено", "Connected")
                    : st == ST_STARTING ? N_("Подключение…", "Connecting…")
                    : st == ST_STOPPING ? N_("Отключение…", "Disconnecting…")
                                        : N_("Отключено", "Disconnected");
    gtk_label_set_text(GTK_LABEL(h_status), txt);
    set_class(h_status, "status-on", st == ST_ON);
    set_class(h_status, "status-off", st != ST_ON);
    vortex_set_label(h_vortex, on_label(st));
    vortex_set_state(h_vortex, st);
    if (error) {
        gtk_label_set_text(GTK_LABEL(h_err), error);
        gtk_widget_show(h_err);
    } else if (st != ST_OFF) {
        gtk_widget_hide(h_err);
    }
    if (st != ST_ON) gtk_label_set_text(GTK_LABEL(h_ip), "");
    if (st == ST_OFF) graph_clear(h_graph);
    /* delays mean different things on/off — re-measure after settling */
    if (st == ST_ON) schedule_ping(4);
    else if (st == ST_OFF) schedule_ping(1);
    tray_update();
}

void ui_on_traffic(gint64 up, gint64 down)
{
    if (up < 0) {
        gtk_label_set_text(GTK_LABEL(h_speed), "");
        gtk_label_set_text(GTK_LABEL(h_total), "");
        return;
    }
    graph_push(h_graph, up, down);
    g_autofree char *u = fmt_bytes(up), *d = fmt_bytes(down);
    g_autofree char *s = g_strdup_printf("↑ %s/%s    ↓ %s/%s", u, N_("с", "s"), d, N_("с", "s"));
    gtk_label_set_text(GTK_LABEL(h_speed), s);
    g_autofree char *tu = fmt_bytes(core_total_up()), *td = fmt_bytes(core_total_down());
    g_autofree char *t = g_strdup_printf("%s ↑ %s   ↓ %s", N_("за сессию:", "this session:"), tu, td);
    gtk_label_set_text(GTK_LABEL(h_total), t);
}

void ui_on_log(const char *line)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(log_buf, &end);
    gtk_text_buffer_insert(log_buf, &end, line, -1);
    gtk_text_buffer_get_end_iter(log_buf, &end);
    gtk_text_buffer_insert(log_buf, &end, "\n", 1);
    /* bounded: the memory budget matters more than history */
    if (++log_lines > 400) {
        GtkTextIter a, b;
        gtk_text_buffer_get_start_iter(log_buf, &a);
        gtk_text_buffer_get_iter_at_line(log_buf, &b, 100);
        gtk_text_buffer_delete(log_buf, &a, &b);
        log_lines -= 100;
    }
}

static void set_delay_label(GtkWidget *l, int ms)
{
    GtkStyleContext *sc = gtk_widget_get_style_context(l);
    gtk_style_context_remove_class(sc, "ping-good");
    gtk_style_context_remove_class(sc, "ping-mid");
    gtk_style_context_remove_class(sc, "ping-bad");
    if (ms == -1) { gtk_label_set_text(GTK_LABEL(l), ""); return; }
    if (ms == -2) { gtk_label_set_text(GTK_LABEL(l), "···"); return; }
    if (ms == 0) {
        gtk_label_set_text(GTK_LABEL(l), "✕");
        gtk_style_context_add_class(sc, "ping-bad");
        return;
    }
    g_autofree char *t = g_strdup_printf("%d %s", ms, N_("мс", "ms"));
    gtk_label_set_text(GTK_LABEL(l), t);
    gtk_style_context_add_class(sc, ms < 300 ? "ping-good" : ms < 800 ? "ping-mid" : "ping-bad");
}

void ui_on_delay(const char *tag, int ms, gboolean tcp)
{
    Server *s = profile_find_server(profile_active(), tag);
    if (!s) return;
    s->delay = ms;
    s->delay_tcp = tcp;
    if (s->delay_label) set_delay_label(s->delay_label, ms);
}

void ui_on_ip(const char *ip, const char *cc)
{
    if (!ip) { gtk_label_set_text(GTK_LABEL(h_ip), N_("IP: не удалось проверить", "IP: check failed")); return; }
    g_autofree char *flag = cc && *cc ? flag_emoji(cc) : g_strdup("");
    g_autofree char *t = g_strdup_printf("IP %s %s", ip, flag);
    gtk_label_set_text(GTK_LABEL(h_ip), t);
}

void ui_on_tun_setup(gboolean ok, const char *msg)
{
    ui_toast(msg);
    if (!st_tun_btn) return;
    gtk_widget_set_sensitive(st_tun_btn, TRUE);
    gtk_label_set_text(GTK_LABEL(st_tun_lbl), tun_ready() ? N_("Права выданы ✓", "Granted ✓")
                                                          : N_("Права не выданы", "Not granted"));
    if (ok && S.mode == MODE_TUN && core_state() == ST_OFF) gtk_widget_hide(h_err);
}

/* ---------- automatic ping ---------- */

static gboolean ping_now(gpointer ud)
{
    ping_timer = 0;
    CoreState st = core_state();
    if (st == ST_ON || st == ST_OFF) core_test_all();
    schedule_ping(st == ST_ON ? 120 : 300);   /* keep numbers fresh */
    return G_SOURCE_REMOVE;
}

static void schedule_ping(guint delay_s)
{
    if (ping_timer) g_source_remove(ping_timer);
    ping_timer = g_timeout_add_seconds(delay_s, ping_now, NULL);
}

/* ---------- home page ---------- */

static void on_connect(GtkButton *b, gpointer ud)
{
    switch (core_state()) {
    case ST_OFF: core_start(); break;
    case ST_ON: case ST_STARTING: core_stop(); break;
    default: break;
    }
}

static void on_vortex_click(void) { on_connect(NULL, NULL); }

static void refresh_home(void)
{
    Profile *p = profile_active();
    const char *sel = S.selected && *S.selected ? S.selected : "auto";
    Server *s = profile_find_server(p, sel);
    g_autofree char *srv = s ? g_strdup(s->name) : g_strdup(N_("⚡ Авто — лучший пинг", "⚡ Auto — lowest ping"));
    gtk_label_set_text(GTK_LABEL(h_server), srv);
    gtk_label_set_text(GTK_LABEL(h_mode), mode_name(S.mode));

    if (!p) {
        gtk_label_set_text(GTK_LABEL(h_prof_name), N_("Нет профиля", "No profile"));
        gtk_label_set_text(GTK_LABEL(h_prof_usage), N_("Добавьте подписку на вкладке «Профили»",
                                                       "Add a subscription on the Profiles tab"));
        gtk_widget_hide(h_prof_bar);
        gtk_label_set_text(GTK_LABEL(h_prof_exp), "");
        return;
    }
    gtk_label_set_text(GTK_LABEL(h_prof_name), p->name);
    gint64 used = p->upload + p->download;
    if (p->total > 0) {
        g_autofree char *u = fmt_bytes(used), *t = fmt_bytes(p->total);
        g_autofree char *s2 = g_strdup_printf("%s %s %s", u, N_("из", "of"), t);
        gtk_label_set_text(GTK_LABEL(h_prof_usage), s2);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(h_prof_bar), CLAMP((double)used / p->total, 0, 1));
        gtk_widget_show(h_prof_bar);
    } else {
        g_autofree char *u = fmt_bytes(used);
        g_autofree char *s2 = used ? g_strdup_printf("%s %s · %s", N_("использовано", "used"), u, N_("безлимит", "unlimited"))
                                   : g_strdup_printf("%d %s", profile_server_count(p), N_("серверов", "servers"));
        gtk_label_set_text(GTK_LABEL(h_prof_usage), s2);
        gtk_widget_hide(h_prof_bar);
    }
    g_autofree char *exp = NULL;
    if (p->expire > 0) {
        g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(p->expire);
        gint64 days = (p->expire - g_get_real_time() / G_USEC_PER_SEC) / 86400;
        g_autofree char *d = g_date_time_format(dt, "%d.%m.%Y");
        exp = g_strdup_printf("%s %s (%" G_GINT64_FORMAT " %s)", N_("до", "until"), d, MAX(days, 0), N_("дн.", "d"));
    } else exp = g_strdup(N_("бессрочно", "no expiry"));
    gtk_label_set_text(GTK_LABEL(h_prof_exp), exp);
}

static GtkWidget *page_home(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 22);

    GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_halign(center, GTK_ALIGN_CENTER);

    /* the vortex *is* the connect button */
    /* the vortex *is* the connect button; its arms orbit around it */
    h_vortex = vortex_new();
    gtk_widget_set_size_request(h_vortex, 300, 300);
    vortex_set_label(h_vortex, on_label(ST_OFF));
    vortex_set_animated(h_vortex, S.animations);
    vortex_on_click(h_vortex, on_vortex_click);
    gtk_widget_set_tooltip_text(h_vortex, N_("Подключить / отключить", "Connect / disconnect"));
    GtkWidget *ev = h_vortex;
    gtk_box_pack_start(GTK_BOX(center), ev, FALSE, FALSE, 0);

    h_status = gtk_label_new(N_("Отключено", "Disconnected"));
    add_class(h_status, "big");
    add_class(h_status, "status-off");
    gtk_box_pack_start(GTK_BOX(center), h_status, FALSE, FALSE, 2);

    h_err = gtk_label_new("");
    add_class(h_err, "error");
    gtk_label_set_line_wrap(GTK_LABEL(h_err), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(h_err), 64);
    gtk_label_set_justify(GTK_LABEL(h_err), GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable(GTK_LABEL(h_err), TRUE);
    gtk_widget_set_no_show_all(h_err, TRUE);
    gtk_box_pack_start(GTK_BOX(center), h_err, FALSE, FALSE, 0);

    h_ip = gtk_label_new("");
    add_class(h_ip, "dim");
    gtk_box_pack_start(GTK_BOX(center), h_ip, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), center, FALSE, FALSE, 0);

    /* live traffic */
    GtkWidget *tc = card();
    GtkWidget *trow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    h_speed = label("", "h2");
    gtk_box_pack_start(GTK_BOX(trow), h_speed, TRUE, TRUE, 0);
    h_total = label("", "dim");
    gtk_box_pack_end(GTK_BOX(trow), h_total, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tc), trow, FALSE, FALSE, 0);
    h_graph = graph_new();
    gtk_box_pack_start(GTK_BOX(tc), h_graph, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), tc, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);

    GtkWidget *c1 = card();
    gtk_box_pack_start(GTK_BOX(c1), label(N_("Сервер", "Server"), "dim"), FALSE, FALSE, 0);
    h_server = label("", "h2");
    gtk_label_set_ellipsize(GTK_LABEL(h_server), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(c1), h_server, FALSE, FALSE, 0);
    h_mode = label("", "dim");
    gtk_box_pack_start(GTK_BOX(c1), h_mode, FALSE, FALSE, 0);
    GtkWidget *chg = btn(N_("Выбрать сервер", "Choose server"), "flat-btn", G_CALLBACK(on_nav), "proxies");
    gtk_widget_set_halign(chg, GTK_ALIGN_START);
    gtk_widget_set_margin_top(chg, 4);
    gtk_box_pack_start(GTK_BOX(c1), chg, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), c1, TRUE, TRUE, 0);

    GtkWidget *c2 = card();
    gtk_box_pack_start(GTK_BOX(c2), label(N_("Профиль", "Profile"), "dim"), FALSE, FALSE, 0);
    h_prof_name = label("", "h2");
    gtk_label_set_ellipsize(GTK_LABEL(h_prof_name), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(c2), h_prof_name, FALSE, FALSE, 0);
    h_prof_usage = label("", NULL);
    gtk_box_pack_start(GTK_BOX(c2), h_prof_usage, FALSE, FALSE, 0);
    h_prof_bar = gtk_progress_bar_new();
    gtk_widget_set_no_show_all(h_prof_bar, TRUE);
    gtk_box_pack_start(GTK_BOX(c2), h_prof_bar, FALSE, FALSE, 2);
    h_prof_exp = label("", "dim");
    gtk_box_pack_start(GTK_BOX(c2), h_prof_exp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), c2, TRUE, TRUE, 0);

    gtk_box_pack_end(GTK_BOX(box), row, FALSE, FALSE, 0);
    return box;
}

/* ---------- proxies page ---------- */

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer ud)
{
    const char *tag = g_object_get_data(G_OBJECT(row), "tag");
    if (!tag) return;
    core_select(tag);
    GList *rows = gtk_container_get_children(GTK_CONTAINER(p_list));
    for (GList *l = rows; l; l = l->next) {
        const char *t = g_object_get_data(l->data, "tag");
        set_class(l->data, "selected-srv", t && !strcmp(t, tag));
    }
    g_list_free(rows);
    refresh_home();
    ui_toast(core_state() == ST_ON ? N_("Сервер переключён", "Server switched") : N_("Сервер выбран", "Server selected"));
}

static void on_ping_all(GtkButton *b, gpointer ud) { schedule_ping(0); }

static void on_ping_one(GtkButton *b, gpointer ud)
{
    Server *s = profile_find_server(profile_active(), ud);
    if (!s) return;
    ui_on_delay(s->tag, -2, FALSE);
    core_test_delay(s->tag);
}

static void on_sort_ping(GtkButton *b, gpointer ud)
{
    Profile *p = profile_active();
    if (!p) return;
    GPtrArray *a = p->servers;
    for (guint i = 1; i < a->len; i++)
        for (guint j = i; j > 0; j--) {
            Server *x = a->pdata[j - 1], *y = a->pdata[j];
            int dx = x->separator ? -3 : (x->delay > 0 ? x->delay : 1 << 30);
            int dy = y->separator ? -3 : (y->delay > 0 ? y->delay : 1 << 30);
            if (dx <= dy) break;
            a->pdata[j - 1] = y; a->pdata[j] = x;
        }
    rebuild_proxies();
}

static GtkWidget *server_row(const char *tag, const char *name, const char *sub, Server *s)
{
    GtkWidget *row = gtk_list_box_row_new();
    add_class(row, "server");
    g_object_set_data_full(G_OBJECT(row), "tag", g_strdup(tag), g_free);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *n = label(name, "h2");
    gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(v), n, FALSE, FALSE, 0);
    if (sub) {
        GtkWidget *sl = label(sub, "dim");
        gtk_label_set_ellipsize(GTK_LABEL(sl), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(v), sl, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    if (s) {
        GtkWidget *d = gtk_label_new("");
        gtk_widget_set_size_request(d, 70, -1);
        gtk_label_set_xalign(GTK_LABEL(d), 1);
        s->delay_label = d;
        set_delay_label(d, s->delay);
        g_signal_connect_swapped(d, "destroy", G_CALLBACK(g_nullify_pointer), &s->delay_label);
        gtk_box_pack_start(GTK_BOX(h), d, FALSE, FALSE, 0);
        GtkWidget *pb = btn("⟳", "flat-btn", G_CALLBACK(on_ping_one), s->tag);
        add_class(pb, "icon-btn");
        gtk_widget_set_tooltip_text(pb, N_("Проверить пинг", "Test ping"));
        gtk_box_pack_start(GTK_BOX(h), pb, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(row), h);
    const char *cur = S.selected && *S.selected ? S.selected : "auto";
    if (!strcmp(cur, tag)) add_class(row, "selected-srv");
    return row;
}

static void rebuild_proxies(void)
{
    if (!p_list) return;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(p_list));
    for (GList *l = rows; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(rows);

    Profile *p = profile_active();
    g_autofree char *title = p ? g_strdup_printf("%s · %d", N_("Серверы", "Servers"), profile_server_count(p))
                               : g_strdup(N_("Серверы", "Servers"));
    gtk_label_set_text(GTK_LABEL(p_title), title);
    if (!p) return;

    g_autofree char *auto_sub = S.region && *S.region && S.auto_skip_region
        ? g_strdup_printf(N_("лучший пинг, проверка каждые 2 мин · без серверов %s",
                             "lowest ping, rechecked every 2 min · skips %s servers"),
                          flag_emoji(S.region))
        : g_strdup(N_("лучший пинг, проверка каждые 2 мин", "lowest ping, rechecked every 2 min"));
    gtk_container_add(GTK_CONTAINER(p_list), server_row("auto", N_("⚡ Авто", "⚡ Auto"), auto_sub, NULL));
    for (guint i = 0; i < p->servers->len; i++) {
        Server *s = p->servers->pdata[i];
        if (s->separator) {
            GtkWidget *row = gtk_list_box_row_new();
            add_class(row, "sep");
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_container_add(GTK_CONTAINER(row), label(s->name, NULL));
            gtk_container_add(GTK_CONTAINER(p_list), row);
            continue;
        }
        g_autofree char *sub = g_strdup_printf("%s · %s:%d", s->proto, s->server, s->port);
        gtk_container_add(GTK_CONTAINER(p_list), server_row(s->tag, s->name, sub, s));
    }
    gtk_widget_show_all(p_list);
}

static GtkWidget *page_proxies(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    p_title = label(N_("Серверы", "Servers"), "h1");
    gtk_box_pack_start(GTK_BOX(hdr), p_title, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hdr), btn(N_("По пингу", "Sort by ping"), "flat-btn", G_CALLBACK(on_sort_ping), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hdr), btn(N_("Обновить пинг", "Refresh ping"), "accent-btn", G_CALLBACK(on_ping_all), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hdr, FALSE, FALSE, 0);
    GtkWidget *hint = label(N_("Пинг обновляется сам. Без подключения — время TCP-соединения, с подключением — реальная задержка через сервер.",
                               "Ping refreshes automatically. Offline it is TCP connect time; connected it is the real delay through the server."), "dim");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    p_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(p_list), GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(p_list), TRUE);
    g_signal_connect(p_list, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(sw), p_list);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    return box;
}

/* ---------- profiles page ---------- */

static void on_profile_done(const char *id, const char *error, gpointer ud)
{
    if (pr_add) gtk_widget_set_sensitive(pr_add, TRUE);
    if (error) {
        g_autofree char *m = g_strdup_printf("%s: %s", N_("Ошибка", "Error"), error);
        ui_toast(m);
        return;
    }
    if (ud && pr_entry) gtk_entry_set_text(GTK_ENTRY(pr_entry), "");
    Profile *p = profile_by_id(id);
    g_autofree char *m = p ? g_strdup_printf("«%s»: %d %s", p->name, profile_server_count(p), N_("серверов", "servers"))
                           : g_strdup(N_("Готово", "Done"));
    ui_toast(m);
    rebuild_profiles();
    rebuild_proxies();
    refresh_home();
    if (p && p == profile_active()) schedule_ping(1);
}

static void on_add_profile(GtkWidget *w, gpointer ud)
{
    const char *t = gtk_entry_get_text(GTK_ENTRY(pr_entry));
    if (!t || !*t) {
        /* empty field: take the clipboard, like Hiddify's "add from clipboard" */
        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        g_autofree char *clip = gtk_clipboard_wait_for_text(cb);
        if (!clip || !*g_strstrip(clip)) { ui_toast(N_("Вставьте ссылку на подписку или ключ", "Paste a subscription link or key")); return; }
        gtk_entry_set_text(GTK_ENTRY(pr_entry), clip);
        t = gtk_entry_get_text(GTK_ENTRY(pr_entry));
    }
    gtk_widget_set_sensitive(pr_add, FALSE);
    ui_toast(N_("Загружаю подписку…", "Fetching subscription…"));
    profile_add_async(t, on_profile_done, GINT_TO_POINTER(1));
}

static void on_use_profile(GtkButton *b, gpointer id)
{
    g_free(S.active);
    S.active = g_strdup(id);
    g_free(S.selected);
    S.selected = g_strdup("auto");
    settings_save();
    rebuild_profiles();
    rebuild_proxies();
    refresh_home();
    core_restart();
    schedule_ping(1);
}

static void on_update_profile(GtkButton *b, gpointer id)
{
    Profile *p = profile_by_id(id);
    if (!p) return;
    ui_toast(N_("Обновляю…", "Updating…"));
    profile_update_async(p, on_profile_done, NULL);
}

static void on_delete_profile(GtkButton *b, gpointer id)
{
    Profile *p = profile_by_id(id);
    if (!p) return;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_OK_CANCEL, N_("Удалить профиль «%s»?", "Delete profile “%s”?"), p->name);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r != GTK_RESPONSE_OK) return;
    gboolean was_active = p == profile_active();
    profile_delete(p);
    if (was_active && core_state() != ST_OFF) core_stop();
    rebuild_profiles();
    rebuild_proxies();
    refresh_home();
}

static void rebuild_profiles(void)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(pr_list));
    for (GList *l = rows; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(rows);

    Profile *act = profile_active();
    if (!PROFILES->len)
        gtk_box_pack_start(GTK_BOX(pr_list), label(N_("Профилей пока нет.", "No profiles yet."), "dim"), FALSE, FALSE, 0);
    for (guint i = 0; i < PROFILES->len; i++) {
        Profile *p = PROFILES->pdata[i];
        GtkWidget *c = card();
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        g_autofree char *nm = g_strdup_printf("%s%s", p == act ? "▶ " : "", p->name);
        GtkWidget *n = label(nm, "h2");
        gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(top), n, TRUE, TRUE, 0);
        if (p != act)
            gtk_box_pack_start(GTK_BOX(top), btn(N_("Использовать", "Use"), "accent-btn", G_CALLBACK(on_use_profile), p->id), FALSE, FALSE, 0);
        if (p->url)
            gtk_box_pack_start(GTK_BOX(top), btn(N_("Обновить", "Update"), "flat-btn", G_CALLBACK(on_update_profile), p->id), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(top), btn(N_("Удалить", "Delete"), "flat-btn", G_CALLBACK(on_delete_profile), p->id), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(c), top, FALSE, FALSE, 0);

        GString *info = g_string_new(NULL);
        g_string_append_printf(info, "%d %s", profile_server_count(p), N_("серверов", "servers"));
        if (p->total > 0) {
            g_autofree char *u = fmt_bytes(p->upload + p->download), *t = fmt_bytes(p->total);
            g_string_append_printf(info, " · %s / %s", u, t);
        }
        if (p->updated) {
            g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(p->updated);
            g_autofree char *d = g_date_time_format(dt, "%d.%m %H:%M");
            g_string_append_printf(info, " · %s %s", N_("обновлено", "updated"), d);
        }
        gtk_box_pack_start(GTK_BOX(c), label(info->str, "dim"), FALSE, FALSE, 0);
        g_string_free(info, TRUE);
        if (p->total > 0) {
            GtkWidget *bar = gtk_progress_bar_new();
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar),
                CLAMP((double)(p->upload + p->download) / p->total, 0, 1));
            gtk_box_pack_start(GTK_BOX(c), bar, FALSE, FALSE, 2);
        }
        gtk_box_pack_start(GTK_BOX(pr_list), c, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(pr_list);
}

static GtkWidget *page_profiles(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    gtk_box_pack_start(GTK_BOX(box), label(N_("Профили", "Profiles"), "h1"), FALSE, FALSE, 0);
    GtkWidget *hint = label(N_("Ссылка на подписку (https://…) или ключи vless:// vmess:// trojan:// ss:// hy2:// tuic://. "
                               "Пустое поле — вставить из буфера обмена.",
                               "A subscription link (https://…) or vless:// vmess:// trojan:// ss:// hy2:// tuic:// keys. "
                               "Leave empty to paste from the clipboard."), "dim");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    pr_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(pr_entry), N_("https://… или vless://…", "https://… or vless://…"));
    g_signal_connect(pr_entry, "activate", G_CALLBACK(on_add_profile), NULL);
    gtk_box_pack_start(GTK_BOX(row), pr_entry, TRUE, TRUE, 0);
    pr_add = btn(N_("Добавить", "Add"), "accent-btn", G_CALLBACK(on_add_profile), NULL);
    gtk_box_pack_start(GTK_BOX(row), pr_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    pr_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(pr_list, 6);
    gtk_widget_set_margin_end(pr_list, 6);
    gtk_container_add(GTK_CONTAINER(sw), pr_list);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    return box;
}

/* ---------- logs page ---------- */

static void on_clear_log(GtkButton *b, gpointer ud)
{
    gtk_text_buffer_set_text(log_buf, "", 0);
    log_lines = 0;
}

static void on_log_changed(GtkTextBuffer *buf, GtkTextView *tv)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    GtkTextMark *m = gtk_text_buffer_get_mark(buf, "end");
    if (!m) m = gtk_text_buffer_create_mark(buf, "end", &end, FALSE);
    else gtk_text_buffer_move_mark(buf, m, &end);
    if (gtk_widget_get_mapped(GTK_WIDGET(tv))) gtk_text_view_scroll_mark_onscreen(tv, m);
}

static GtkWidget *page_logs(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(hdr), label(N_("Логи ядра", "Core logs"), "h1"), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hdr), btn(N_("Очистить", "Clear"), "flat-btn", G_CALLBACK(on_clear_log), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hdr, FALSE, FALSE, 0);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 8);
    add_class(tv, "mono");
    log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    g_signal_connect(log_buf, "changed", G_CALLBACK(on_log_changed), tv);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    return box;
}

/* ---------- settings page ---------- */

/* theme picker: a card per theme with a still of its own connect button */
static gboolean draw_preview(GtkWidget *w, cairo_t *cr, gpointer id)
{
    GPtrArray *ts = themes_list();
    for (guint i = 0; i < ts->len; i++) {
        Theme *t = ts->pdata[i];
        if (strcmp(t->id, id)) continue;
        int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
        GdkRGBA bg;
        gdk_rgba_parse(&bg, t->c[TC_BG]);
        gdk_cairo_set_source_rgba(cr, &bg);
        cairo_paint(cr);
        cairo_translate(cr, (W - H) / 2.0, 0);
        vortex_paint_preview(cr, t, H);
        break;
    }
    return TRUE;
}

static void on_theme_card(GtkButton *b, gpointer ud)
{
    const char *id = g_object_get_data(G_OBJECT(b), "id");
    if (!id || !g_strcmp0(S.theme, id)) return;
    g_free(S.theme);
    S.theme = g_strdup(id);
    settings_save();
    apply_theme();
    GList *kids = gtk_container_get_children(GTK_CONTAINER(st_themes));
    for (GList *l = kids; l; l = l->next) {
        GtkWidget *c = gtk_bin_get_child(GTK_BIN(l->data));   /* flowboxchild -> button */
        set_class(c, "active", !g_strcmp0(g_object_get_data(G_OBJECT(c), "id"), id));
    }
    g_list_free(kids);
}

static void fill_themes(void)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(st_themes));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(kids);
    GPtrArray *ts = themes_list();
    const char *cur = theme_current()->id;
    for (guint i = 0; i < ts->len; i++) {
        Theme *t = ts->pdata[i];
        GtkWidget *b = gtk_button_new();
        add_class(b, "theme-card");
        if (!strcmp(t->id, cur)) add_class(b, "active");
        g_object_set_data_full(G_OBJECT(b), "id", g_strdup(t->id), g_free);
        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, 128, 96);
        g_object_set_data_full(G_OBJECT(da), "id", g_strdup(t->id), g_free);
        g_signal_connect(da, "draw", G_CALLBACK(draw_preview), g_object_get_data(G_OBJECT(da), "id"));
        gtk_box_pack_start(GTK_BOX(v), da, FALSE, FALSE, 0);
        g_autofree char *nm = t->builtin ? g_strdup(t->name) : g_strdup_printf("%s ✎", t->name);
        GtkWidget *l = gtk_label_new(nm);
        gtk_box_pack_start(GTK_BOX(v), l, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(b), v);
        g_signal_connect(b, "clicked", G_CALLBACK(on_theme_card), NULL);
        gtk_flow_box_insert(GTK_FLOW_BOX(st_themes), b, -1);
    }
    gtk_widget_show_all(st_themes);
}

static void on_themes_changed(void)
{
    apply_theme();
    if (st_themes) fill_themes();
}

static void on_open_themes(GtkButton *b, gpointer ud)
{
    g_autofree char *d = themes_dir();
    g_autofree char *uri = g_filename_to_uri(d, NULL, NULL);
    if (uri) g_app_info_launch_default_for_uri(uri, NULL, NULL);
}

static void on_mode(GtkComboBox *c, gpointer ud)
{
    int m = gtk_combo_box_get_active(c);
    if (m < 0 || m == S.mode) return;
    if (S.mode == MODE_PROXY && core_state() == ST_ON) sysproxy_disable();
    S.mode = m;
    settings_save();
    refresh_home();
    if (m == MODE_TUN && !tun_ready())
        ui_toast(N_("Для TUN нажмите «Выдать права для TUN» ниже", "For TUN, press “Grant TUN permissions” below"));
    core_restart();
}

static void on_region(GtkComboBox *c, gpointer ud)
{
    const char *id = gtk_combo_box_get_active_id(c);
    if (!id) return;
    g_free(S.region);
    S.region = g_strdup(id);
    settings_save();
    rules_update_async(FALSE);
    rebuild_proxies();
    core_restart();
}

static void on_lang(GtkComboBox *c, gpointer ud)
{
    const char *id = gtk_combo_box_get_active_id(c);
    if (!id || !strcmp(id, S.lang)) return;
    g_free(S.lang);
    S.lang = g_strdup(id);
    settings_save();
    ui_toast(N_("Язык сменится после перезапуска", "Language changes after restart"));
}

static void on_bool(gboolean *field)
{
    settings_save();
    if (field == &S.auto_skip_region) { rebuild_proxies(); core_restart(); }
    if (field == &S.animations) {
        vortex_set_animated(h_vortex, S.animations);
        gtk_stack_set_transition_type(GTK_STACK(stack), S.animations
            ? GTK_STACK_TRANSITION_TYPE_CROSSFADE : GTK_STACK_TRANSITION_TYPE_NONE);
    }
    if (field == &S.eco_fps && S.animations) vortex_set_animated(h_vortex, TRUE);   /* re-pick the clock */
}

/* port fields are plain entries: GtkSpinButton's +/- icons are SVGs, and
 * loading any image through gdk-pixbuf spawns glycin sandboxes (~40 MB) */
static void on_port(GtkEntry *e, gpointer field)
{
    const char *t = gtk_entry_get_text(e);
    char *end = NULL;
    long v = strtol(t, &end, 10);
    gboolean ok = t[0] && end && !*end && v >= 1024 && v <= 65535;
    set_class(GTK_WIDGET(e), "error", !ok);
    if (!ok || *(int *)field == v) return;
    *(int *)field = (int)v;
    settings_save();
}

static GtkWidget *port_entry(int *field)
{
    GtkWidget *e = gtk_entry_new();
    g_autofree char *t = g_strdup_printf("%d", *field);
    gtk_entry_set_text(GTK_ENTRY(e), t);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 6);
    gtk_entry_set_max_length(GTK_ENTRY(e), 5);
    gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_DIGITS);
    g_signal_connect(e, "changed", G_CALLBACK(on_port), field);
    return e;
}

#ifndef G_OS_WIN32
static void on_tun_setup(GtkButton *b, gpointer ud)
{
    gtk_widget_set_sensitive(st_tun_btn, FALSE);
    tun_setup_async();
}
#endif

static void on_update_rules(GtkButton *b, gpointer ud)
{
    rules_update_async(TRUE);
    ui_toast(N_("Правила обновляются в фоне", "Rules are updating in the background"));
}

static GtkWidget *setting_row(const char *title, const char *hint, GtkWidget *ctl)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(v), label(title, "h2"), FALSE, FALSE, 0);
    if (hint) {
        GtkWidget *l = label(hint, "dim");
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_box_pack_start(GTK_BOX(v), l, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    gtk_widget_set_valign(ctl, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(h), ctl, FALSE, FALSE, 0);
    return h;
}

/* Toggle drawn with cairo. GtkSwitch loads its on/off glyphs as PNG
 * resources, and on systems where gdk-pixbuf uses glycin every image load
 * spawns sandboxed helper processes (~40 MB). This one costs nothing and
 * follows the theme (square in pixel themes). */
typedef struct { gboolean *field; double pos; guint tick; } Toggle;

static gboolean toggle_tick(GtkWidget *w, GdkFrameClock *fc, gpointer ud)
{
    Toggle *t = g_object_get_data(G_OBJECT(w), "tg");
    double target = *t->field ? 1 : 0;
    t->pos += (target - t->pos) * (S.animations ? 0.28 : 1);
    if (fabs(t->pos - target) < 0.01) { t->pos = target; t->tick = 0; gtk_widget_queue_draw(w); return G_SOURCE_REMOVE; }
    gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static gboolean toggle_draw(GtkWidget *w, cairo_t *cr, gpointer ud)
{
    Toggle *t = g_object_get_data(G_OBJECT(w), "tg");
    const Theme *th = theme_current();
    GdkRGBA off, on, knob;
    gdk_rgba_parse(&off, th->c[TC_LINE]);
    gdk_rgba_parse(&on, th->c[TC_ACCENT]);
    gdk_rgba_parse(&knob, "#ffffff");
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double tw = 44, thh = 24, x = (W - tw) / 2, y = (H - thh) / 2, p = t->pos;
    double r = th->pixel ? 0 : thh / 2;
    cairo_set_source_rgba(cr, off.red + (on.red - off.red) * p, off.green + (on.green - off.green) * p,
                          off.blue + (on.blue - off.blue) * p, 1);
    if (r > 0) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + r, y + r, r, G_PI / 2, 3 * G_PI / 2);
        cairo_arc(cr, x + tw - r, y + r, r, -G_PI / 2, G_PI / 2);
        cairo_close_path(cr);
    } else cairo_rectangle(cr, x, y, tw, thh);
    cairo_fill(cr);
    double k = thh - 6, kx = x + 3 + (tw - 6 - k) * p;
    gdk_cairo_set_source_rgba(cr, &knob);
    if (r > 0) cairo_arc(cr, kx + k / 2, y + 3 + k / 2, k / 2, 0, 2 * G_PI);
    else cairo_rectangle(cr, kx, y + 3, k, k);
    cairo_fill(cr);
    if (gtk_widget_has_visible_focus(w)) {
        cairo_set_line_width(cr, 1);
        gdk_cairo_set_source_rgba(cr, &on);
        cairo_rectangle(cr, x - 2.5, y - 2.5, tw + 5, thh + 5);
        cairo_stroke(cr);
    }
    return TRUE;
}

static void toggle_flip(GtkWidget *w)
{
    Toggle *t = g_object_get_data(G_OBJECT(w), "tg");
    *t->field = !*t->field;
    if (!t->tick) t->tick = gtk_widget_add_tick_callback(w, toggle_tick, NULL, NULL);
    on_bool(t->field);
}

static gboolean toggle_click(GtkWidget *w, GdkEventButton *e, gpointer ud)
{
    if (e->button == 1 && e->type == GDK_BUTTON_RELEASE) toggle_flip(w);
    return TRUE;
}

static gboolean toggle_key(GtkWidget *w, GdkEventKey *e, gpointer ud)
{
    if (e->keyval == GDK_KEY_space || e->keyval == GDK_KEY_Return || e->keyval == GDK_KEY_KP_Enter) {
        toggle_flip(w);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *sw_for(gboolean *field)
{
    GtkWidget *w = gtk_drawing_area_new();
    Toggle *t = g_new0(Toggle, 1);
    t->field = field;
    t->pos = *field ? 1 : 0;
    g_object_set_data_full(G_OBJECT(w), "tg", t, g_free);
    gtk_widget_set_size_request(w, 52, 30);
    gtk_widget_set_can_focus(w, TRUE);
    gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(w, "draw", G_CALLBACK(toggle_draw), NULL);
    g_signal_connect(w, "button-release-event", G_CALLBACK(toggle_click), NULL);
    g_signal_connect(w, "key-press-event", G_CALLBACK(toggle_key), NULL);
    AtkObject *acc = gtk_widget_get_accessible(w);
    if (acc) atk_object_set_role(acc, ATK_ROLE_TOGGLE_BUTTON);
    return w;
}

static GtkWidget *page_settings(void)
{
    GtkWidget *outer = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    gtk_box_pack_start(GTK_BOX(box), label(N_("Настройки", "Settings"), "h1"), FALSE, FALSE, 0);

    /* look */
    GtkWidget *c = card();
    GtkWidget *thdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *tv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(tv), label(N_("Тема", "Theme"), "h2"), FALSE, FALSE, 0);
    GtkWidget *thint = label(N_("У каждой темы своя кнопка. Свои темы — .ini файлы в папке тем, подхватываются на лету.",
                                "Every theme has its own button. Custom themes are .ini files in the themes folder, applied live."), "dim");
    gtk_label_set_line_wrap(GTK_LABEL(thint), TRUE);
    gtk_box_pack_start(GTK_BOX(tv), thint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(thdr), tv, TRUE, TRUE, 0);
    GtkWidget *ob = btn(N_("Папка тем…", "Themes folder…"), "flat-btn", G_CALLBACK(on_open_themes), NULL);
    gtk_widget_set_valign(ob, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(thdr), ob, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), thdr, FALSE, FALSE, 0);
    st_themes = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(st_themes), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(st_themes), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(st_themes), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(st_themes), 10);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(st_themes), 10);
    fill_themes();
    gtk_box_pack_start(GTK_BOX(c), st_themes, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(c), setting_row(N_("Анимации", "Animations"), NULL, sw_for(&S.animations)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), setting_row(N_("Экономный режим анимации", "Economy animation mode"),
        N_("В покое кнопка вращается с частотой 20 кадров в секунду, а не с частотой экрана. "
           "Процессор нагружается меньше, но анимация становится менее плавной.",
           "When idle, the button spins at 20 fps instead of your display's refresh rate. "
           "Uses less CPU, but the animation is less smooth."), sw_for(&S.eco_fps)), FALSE, FALSE, 0);
    GtkWidget *lang = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lang), "auto", N_("Системный", "System"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lang), "ru", "Русский");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lang), "en", "English");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(lang), S.lang);
    g_signal_connect(lang, "changed", G_CALLBACK(on_lang), NULL);
    gtk_box_pack_start(GTK_BOX(c), setting_row(N_("Язык", "Language"), NULL, lang), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);

    /* connection */
    c = card();
    GtkWidget *mode = gtk_combo_box_text_new();
    for (int i = 0; i < 3; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), mode_name(i));
    gtk_combo_box_set_active(GTK_COMBO_BOX(mode), S.mode);
    g_signal_connect(mode, "changed", G_CALLBACK(on_mode), NULL);
    gtk_box_pack_start(GTK_BOX(c), setting_row(N_("Режим", "Mode"),
        N_("TUN — весь трафик системы (рекомендуется). Системный прокси — только программы, которые его читают.",
           "TUN routes all system traffic (recommended). System proxy only affects apps that honour it."),
        mode), FALSE, FALSE, 0);
#ifndef G_OS_WIN32
    GtkWidget *tunbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    st_tun_lbl = label(tun_ready() ? N_("Права выданы ✓", "Granted ✓") : N_("Права не выданы", "Not granted"), "dim");
    gtk_box_pack_start(GTK_BOX(tunbox), st_tun_lbl, FALSE, FALSE, 0);
    st_tun_btn = btn(N_("Выдать права для TUN", "Grant TUN permissions"), "flat-btn", G_CALLBACK(on_tun_setup), NULL);
    gtk_box_pack_start(GTK_BOX(tunbox), st_tun_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), setting_row("TUN",
        N_("Один раз копирует sing-box в " WL_TUN_BIN " с правом CAP_NET_ADMIN. Сам Wellide работает без root.",
           "Copies sing-box to " WL_TUN_BIN " once with CAP_NET_ADMIN. Wellide itself never runs as root."),
        tunbox), FALSE, FALSE, 0);
#endif
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);

    /* routing */
    c = card();
    GtkWidget *rg = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(rg), "", N_("Выключено — всё через VPN", "Off — everything via VPN"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(rg), "ru", N_("🇷🇺 Россия", "🇷🇺 Russia"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(rg), "ir", N_("🇮🇷 Иран", "🇮🇷 Iran"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(rg), "cn", N_("🇨🇳 Китай", "🇨🇳 China"));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(rg), S.region);
    g_signal_connect(rg, "changed", G_CALLBACK(on_region), NULL);
    GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(rbox), rg, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rbox), btn(N_("Обновить правила", "Update rules"), "flat-btn", G_CALLBACK(on_update_rules), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), setting_row(N_("Сайты своей страны напрямую", "Local sites go direct"),
        N_("Домены и IP выбранной страны идут мимо VPN: банки, госуслуги, местные сервисы.",
           "That country's domains and IPs bypass the VPN: banks, government, local services."), rbox), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), setting_row(N_("«Авто» не выбирает серверы этой страны", "“Auto” skips servers in that country"),
        N_("Такие серверы не помогают с гео-блокировками (ChatGPT и т.п.).", "They don't help with geo-blocked services."),
        sw_for(&S.auto_skip_region)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);

    /* behaviour */
    c = card();
    gtk_box_pack_start(GTK_BOX(c), setting_row(N_("Подключаться при запуске", "Connect on launch"), NULL, sw_for(&S.autoconnect)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), setting_row(N_("Запускаться свёрнутым в трей", "Start minimised to tray"), NULL, sw_for(&S.start_hidden)), FALSE, FALSE, 0);
    GtkWidget *port = port_entry(&S.port);
    gtk_box_pack_start(GTK_BOX(c), setting_row(N_("Порт прокси (HTTP + SOCKS5)", "Proxy port (HTTP + SOCKS5)"),
        N_("Применится при следующем подключении.", "Applies on next connect."), port), FALSE, FALSE, 0);
    GtkWidget *aport = port_entry(&S.api_port);
    gtk_box_pack_start(GTK_BOX(c), setting_row(N_("Порт Clash API", "Clash API port"), NULL, aport), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(outer), box);
    return outer;
}

/* ---------- about page ---------- */

static void on_open_url(GtkButton *b, gpointer url)
{
    GError *e = NULL;
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(b));
    if (gtk_show_uri_on_window(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL, url, GDK_CURRENT_TIME, &e)) return;
    g_clear_error(&e);
#ifndef G_OS_WIN32
    /* no default-handler registration (minimal WMs, broken portal): try xdg-open */
    const char *argv[] = { "xdg-open", url, NULL };
    if (g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                      G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, &e)) return;
    g_clear_error(&e);
#endif
    /* last resort: put the link on the clipboard so it isn't lost */
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), url, -1);
    ui_toast(N_("Не удалось открыть браузер — ссылка скопирована", "Couldn't open a browser — link copied"));
}

static GtkWidget *page_about(void)
{
    GtkWidget *outer = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 28);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    const Theme *th = theme_current();
    cairo_surface_t *ics = vortex_icon_surface(16, 5, "#6a4a7a", th->c[TC_GLOW]);
    gtk_box_pack_start(GTK_BOX(head), gtk_image_new_from_surface(ics), FALSE, FALSE, 0);
    cairo_surface_destroy(ics);
    GtkWidget *tv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(tv, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(tv), label("Wellide", "about-title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tv), label(N_("версия " WL_VERSION " · GPL-3.0-or-later", "version " WL_VERSION " · GPL-3.0-or-later"), "dim"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), tv, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);

    const char *paras[] = {
        N_("Wellide — лёгкий VPN-клиент. Сам он — только окно на GTK, написанное на C; "
           "всю работу с сетью делает ядро sing-box. Вместе они занимают около 125 МБ памяти, "
           "а в фоне почти не нагружают процессор.",
           "Wellide is a lightweight VPN client. The app itself is just a small GTK window written in C; "
           "all networking is done by the sing-box core. Together they use about 125 MB of RAM and next to "
           "no CPU in the background."),
        N_("Добавьте подписку или ключ, нажмите на кнопку — и всё. «Авто» сам выберет самый быстрый сервер "
           "и будет проверять его каждые две минуты. Пинг до серверов обновляется сам.",
           "Add a subscription or a key and press the button — that's it. “Auto” picks the fastest server "
           "and re-checks it every two minutes. Server pings refresh on their own."),
        N_("Режим TUN пропускает через VPN весь трафик системы. Сайты своей страны (банки, госуслуги, "
           "местные сервисы) можно пустить напрямую. Wellide не запускается с правами root: права на "
           "TUN получает только отдельная копия ядра.",
           "TUN mode sends all system traffic through the VPN. Sites of your own country (banks, "
           "government, local services) can go direct. Wellide never runs as root: only a separate copy "
           "of the core gets the TUN permission."),
        N_("Wellide ничего не собирает и никуда не отправляет. Подписки и настройки хранятся только у вас "
           "на компьютере.",
           "Wellide collects nothing and sends nothing anywhere. Subscriptions and settings stay on your computer."),
    };
    GtkWidget *c = card();
    for (guint i = 0; i < G_N_ELEMENTS(paras); i++) {
        GtkWidget *l = label(paras[i], NULL);
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 80);
        gtk_widget_set_margin_bottom(l, 6);
        gtk_box_pack_start(GTK_BOX(c), l, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);

    c = card();
    gtk_box_pack_start(GTK_BOX(c), label(N_("Под капотом", "Under the hood"), "h2"), FALSE, FALSE, 0);
    g_autofree char *core = g_strdup_printf(N_("Ядро: %s", "Core: %s"), core_bin());
    gtk_box_pack_start(GTK_BOX(c), label(core, "dim"), FALSE, FALSE, 0);
    g_autofree char *gtkv = g_strdup_printf("GTK %u.%u.%u · GLib %u.%u · libsoup %u.%u",
        gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(),
        glib_major_version, glib_minor_version, soup_get_major_version(), soup_get_minor_version());
    gtk_box_pack_start(GTK_BOX(c), label(gtkv, "dim"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), label(N_("Правила маршрутизации: SagerNet sing-geoip / sing-geosite",
                                            "Routing rules: SagerNet sing-geoip / sing-geosite"), "dim"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);

    GtkWidget *links = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(links), btn("GitHub", "accent-btn", G_CALLBACK(on_open_url), WL_REPO), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(links), btn(N_("Сообщить об ошибке", "Report a bug"), "flat-btn",
                                           G_CALLBACK(on_open_url), WL_REPO "/issues"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(links), btn("sing-box", "flat-btn", G_CALLBACK(on_open_url),
                                           "https://github.com/SagerNet/sing-box"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), links, FALSE, FALSE, 4);
    GtkWidget *lic = label(N_("© 2026 Wellbou. Свободная программа под лицензией GNU GPL версии 3 или более поздней: "
                              "её можно изменять и распространять, но только с открытым исходным кодом. "
                              "Без каких-либо гарантий.",
                              "© 2026 Wellbou. Free software under the GNU GPL version 3 or later: you may modify "
                              "and share it, but only with the source code. No warranty of any kind."), "dim");
    gtk_label_set_line_wrap(GTK_LABEL(lic), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lic), 80);
    gtk_box_pack_start(GTK_BOX(box), lic, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(outer), box);
    return outer;
}

/* ---------- window, tray, lifecycle ---------- */

static void show_window(void)
{
    gtk_widget_show(win);
    gtk_window_present(GTK_WINDOW(win));
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer ud)
{
    if (have_tray) { gtk_widget_hide(win); return TRUE; }  /* keep running in the tray */
    return FALSE;
}

static void do_quit(void)
{
    if (core_state() == ST_OFF) { g_application_quit(G_APPLICATION(app)); return; }
    quitting = TRUE;
    core_stop();
}

#ifdef HAVE_APPINDICATOR
static void on_tray_show(GtkMenuItem *i, gpointer ud) { show_window(); }
static void on_tray_toggle(GtkMenuItem *i, gpointer ud) { on_connect(NULL, NULL); }
static void on_tray_quit(GtkMenuItem *i, gpointer ud) { do_quit(); }

static void build_tray(void)
{
    tray = app_indicator_new("wellide", tray_icon(FALSE), APP_INDICATOR_CATEGORY_COMMUNICATIONS);
    app_indicator_set_status(tray, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(tray, "Wellide");
    GtkWidget *m = gtk_menu_new();
    GtkWidget *show = gtk_menu_item_new_with_label(N_("Открыть Wellide", "Open Wellide"));
    g_signal_connect(show, "activate", G_CALLBACK(on_tray_show), NULL);
    tray_toggle = gtk_menu_item_new_with_label(N_("Подключиться", "Connect"));
    g_signal_connect(tray_toggle, "activate", G_CALLBACK(on_tray_toggle), NULL);
    GtkWidget *quit = gtk_menu_item_new_with_label(N_("Выход", "Quit"));
    g_signal_connect(quit, "activate", G_CALLBACK(on_tray_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), show);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), tray_toggle);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(m), quit);
    gtk_widget_show_all(m);
    app_indicator_set_menu(tray, GTK_MENU(m));
    app_indicator_set_secondary_activate_target(tray, show);
    have_tray = TRUE;
}
#else
/* Windows / no appindicator: classic status icon (native tray on Windows) */
static void on_si_activate(GtkStatusIcon *si, gpointer ud)
{
    if (gtk_widget_get_visible(win)) gtk_widget_hide(win); else show_window();
}

static void on_si_menu(GtkStatusIcon *si, guint button, guint time, gpointer ud)
{
    GtkWidget *m = gtk_menu_new();
    GtkWidget *t = gtk_menu_item_new_with_label(core_state() == ST_ON ? N_("Отключиться", "Disconnect") : N_("Подключиться", "Connect"));
    g_signal_connect_swapped(t, "activate", G_CALLBACK(on_connect), NULL);
    GtkWidget *q = gtk_menu_item_new_with_label(N_("Выход", "Quit"));
    g_signal_connect_swapped(q, "activate", G_CALLBACK(do_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), t);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), q);
    gtk_widget_show_all(m);
    gtk_menu_popup_at_pointer(GTK_MENU(m), NULL);
}

static void build_tray(void)
{
    const Theme *th = theme_current();
    cairo_surface_t *cs = vortex_icon_surface(16, 2, "#6a4a7a", th->c[TC_GLOW]);
    g_autoptr(GdkPixbuf) pb = gdk_pixbuf_get_from_surface(cs, 0, 0, 32, 32);
    cairo_surface_destroy(cs);
    tray = gtk_status_icon_new_from_pixbuf(pb);
    gtk_status_icon_set_tooltip_text(tray, "Wellide");
    g_signal_connect(tray, "activate", G_CALLBACK(on_si_activate), NULL);
    g_signal_connect(tray, "popup-menu", G_CALLBACK(on_si_menu), NULL);
    have_tray = TRUE;
}
#endif

static GtkWidget *nav_button(GtkWidget *side, const char *glyph, const char *text, const char *page)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *g = gtk_label_new(glyph);
    gtk_widget_set_size_request(g, 16, -1);
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(h), g, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), l, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(b), h);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    add_class(b, "nav");
    g_signal_connect(b, "clicked", G_CALLBACK(on_nav), (gpointer)page);
    g_hash_table_insert(nav_btns, (gpointer)page, b);
    gtk_box_pack_start(GTK_BOX(side), b, FALSE, FALSE, 1);
    return b;
}

static gboolean auto_update(gpointer ud)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    for (guint i = 0; i < PROFILES->len; i++) {
        Profile *p = PROFILES->pdata[i];
        if (p->url && now - p->updated > (gint64)MAX(p->interval_h, 1) * 3600)
            profile_update_async(p, on_profile_done, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static void on_activate(GtkApplication *a, gpointer ud)
{
    if (win) { show_window(); return; }

    css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    nav_btns = g_hash_table_new(g_str_hash, g_str_equal);

    win = gtk_application_window_new(a);
    gtk_window_set_title(GTK_WINDOW(win), "Wellide");
    gtk_window_set_default_size(GTK_WINDOW(win), 880, 640);
    /* panel icon: the installed "wellide" icon, matched via the .desktop file */
    /* The panel takes the icon from the .desktop file (matched by
     * WM_CLASS); the window icon itself is drawn with cairo so GTK does
     * not load PNGs through gdk-pixbuf's sandboxed glycin helpers. */
    {
        GList *icons = NULL;
        const int sizes[] = { 16, 32, 48, 64 };
        for (guint i = 0; i < G_N_ELEMENTS(sizes); i++) {
            int px = sizes[i] / 16;
            cairo_surface_t *cs = vortex_icon_surface(16, px, "#6a4a7a", "#ca31cc");
            icons = g_list_append(icons, gdk_pixbuf_get_from_surface(cs, 0, 0, sizes[i], sizes[i]));
            cairo_surface_destroy(cs);
        }
        gtk_window_set_icon_list(GTK_WINDOW(win), icons);
        g_list_free_full(icons, g_object_unref);
    }
    g_signal_connect(win, "delete-event", G_CALLBACK(on_delete), NULL);

    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(side, "sidebar");
    gtk_widget_set_size_request(side, 196, -1);

    GtkWidget *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    add_class(brand, "brand");
    brand_icon = gtk_image_new();
    gtk_box_pack_start(GTK_BOX(brand), brand_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brand), label("wellide", NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(side), brand, FALSE, FALSE, 0);

    nav_button(side, "◉", N_("Главная", "Home"), "home");
    nav_button(side, "≡", N_("Серверы", "Servers"), "proxies");
    nav_button(side, "▤", N_("Профили", "Profiles"), "profiles");
    nav_button(side, "›_", N_("Логи", "Logs"), "logs");
    nav_button(side, "⚙", N_("Настройки", "Settings"), "settings");
    nav_button(side, "?", N_("О программе", "About"), "about");
    GtkWidget *quit = btn(N_("Выход", "Quit"), "flat-btn", G_CALLBACK(do_quit), NULL);
    gtk_box_pack_end(GTK_BOX(side), quit, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(root), side, FALSE, FALSE, 0);

    stack = gtk_stack_new();
    add_class(stack, "main-bg");
    gtk_stack_set_transition_type(GTK_STACK(stack), S.animations
        ? GTK_STACK_TRANSITION_TYPE_CROSSFADE : GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 180);
    gtk_stack_add_named(GTK_STACK(stack), page_home(), "home");
    gtk_stack_add_named(GTK_STACK(stack), page_proxies(), "proxies");
    gtk_stack_add_named(GTK_STACK(stack), page_profiles(), "profiles");
    gtk_stack_add_named(GTK_STACK(stack), page_logs(), "logs");
    gtk_stack_add_named(GTK_STACK(stack), page_settings(), "settings");
    gtk_stack_add_named(GTK_STACK(stack), page_about(), "about");
    gtk_box_pack_start(GTK_BOX(root), stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(overlay), root);

    toast_rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(toast_rev), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(toast_rev), 220);
    gtk_widget_set_halign(toast_rev, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(toast_rev, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(toast_rev, 18);
    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    add_class(tb, "toast");
    toast_lbl = gtk_label_new("");
    gtk_container_add(GTK_CONTAINER(tb), toast_lbl);
    gtk_container_add(GTK_CONTAINER(toast_rev), tb);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), toast_rev);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), toast_rev, TRUE);
    gtk_container_add(GTK_CONTAINER(win), overlay);

    apply_theme();
    themes_watch(on_themes_changed);
    build_tray();
    rebuild_proxies();
    rebuild_profiles();
    refresh_home();
    go(PROFILES->len ? "home" : "profiles");

    gtk_widget_show_all(overlay);
    if (!(S.start_hidden && have_tray)) show_window();
    g_application_hold(G_APPLICATION(a));

    rules_update_async(FALSE);
    auto_update(NULL);
    g_timeout_add_seconds(1800, auto_update, NULL);
    schedule_ping(1);
    if (S.autoconnect && PROFILES->len) core_start();
}

/* CLI control, forwarded to the primary instance by GApplication */
static int on_command_line(GApplication *a, GApplicationCommandLine *cl, gpointer ud)
{
    int argc;
    g_auto(GStrv) argv = g_application_command_line_get_arguments(cl, &argc);
    gboolean ui_only = TRUE;
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "--import") && i + 1 < argc) {
            if (!win) on_activate(GTK_APPLICATION(a), NULL);
            profile_add_async(argv[++i], on_profile_done, NULL);
            ui_only = FALSE;
        } else if (g_str_has_prefix(o, "wellide://import/")) {
            /* deep link: wellide://import/<url-encoded subscription> */
            if (!win) on_activate(GTK_APPLICATION(a), NULL);
            g_autofree char *u = g_uri_unescape_string(o + 17, NULL);
            if (u) profile_add_async(u, on_profile_done, NULL);
            ui_only = FALSE;
        } else if (!strcmp(o, "--connect")) {
            if (!win) on_activate(GTK_APPLICATION(a), NULL);
            if (core_state() == ST_OFF) core_start();
            ui_only = FALSE;
        } else if (!strcmp(o, "--disconnect")) {
            if (win && core_state() != ST_OFF) core_stop();
            ui_only = FALSE;
        } else if (!strcmp(o, "--toggle")) {
            if (!win) on_activate(GTK_APPLICATION(a), NULL);
            on_connect(NULL, NULL);
            ui_only = FALSE;
        } else if (!strcmp(o, "--quit")) {
            if (win) do_quit(); else g_application_quit(a);
            return 0;
        } else if (!strcmp(o, "--status")) {
            CoreState st = core_state();
            Profile *p = profile_active();
            g_application_command_line_print(cl, "%s\t%s\t%s\n",
                st == ST_ON ? "on" : st == ST_OFF ? "off" : "busy",
                p ? p->name : "-", S.selected ? S.selected : "auto");
            ui_only = FALSE;
        } else if (!strcmp(o, "--hidden")) {
            S.start_hidden = TRUE;
        } else if (!strcmp(o, "--version")) {
            g_application_command_line_print(cl, "wellide %s\n", WL_VERSION);
            return 0;
        } else if (!strcmp(o, "--help") || !strcmp(o, "-h")) {
            g_application_command_line_print(cl,
                "wellide [--hidden] [--import URL] [--connect|--disconnect|--toggle] [--status] [--quit] [--version]\n");
            return 0;
        }
    }
    if (ui_only || !win) on_activate(GTK_APPLICATION(a), NULL);
    return 0;
}

static void on_shutdown(GApplication *a, gpointer ud)
{
    /* last line of defence: never leave the desktop pointing at a dead proxy */
    sysproxy_disable();
}

/* "startup" runs only in the primary instance; remote invocations
 * (wellide --status etc.) must not touch state */
static void on_startup(GApplication *a, gpointer ud)
{
    if (S.sysproxy_set) sysproxy_disable();   /* previous run crashed */
    HTTP = soup_session_new_with_options("timeout", 30, NULL);
    profiles_load();
    themes_reload();
#ifndef G_OS_WIN32
    g_unix_signal_add(SIGINT, on_signal, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
#endif
}

static G_GNUC_UNUSED gboolean on_signal(gpointer ud)
{
    do_quit();
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    /* answered before GTK starts, so it works without a display (CI, ssh) */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--version")) { printf("wellide " WL_VERSION "\n"); return 0; }
#ifndef G_OS_WIN32
    /* `sudo wellide` would keep settings and profiles in /root, where the
     * normal launch never sees them, and root can't open the user's browser.
     * TUN never needs this: the core copy has its own capability. */
    if (geteuid() == 0 && (g_getenv("SUDO_USER") || g_getenv("PKEXEC_UID") || g_getenv("DOAS_USER"))) {
        settings_load();
        if (gtk_init_check(&argc, &argv)) {
            GtkWidget *d = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE, "%s",
                N_("Запускайте Wellide без sudo", "Run Wellide without sudo"));
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s",
                N_("От root настройки и подписки сохраняются в профиль root, а не в ваш, и ссылки не открываются. "
                   "Права для TUN выдаются один раз в настройках, root для этого не нужен.",
                   "As root, settings and subscriptions go to root's profile instead of yours, and links won't open. "
                   "TUN permissions are granted once from Settings; root isn't needed for that."));
            gtk_window_set_title(GTK_WINDOW(d), "Wellide");
            gtk_dialog_run(GTK_DIALOG(d));
        } else fprintf(stderr, "wellide: run it without sudo; TUN rights are granted from Settings\n");
        return 1;
    }
#endif
    settings_load();
    app = gtk_application_new(WL_APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}

/* Wellide — lightweight sing-box GUI (GTK3). */
#include "wellide.h"
#include <libayatana-appindicator/app-indicator.h>
#include <string.h>

gint64 core_total_up(void);
gint64 core_total_down(void);

static GtkApplication *app;
static GtkWidget *win, *stack, *toast_rev, *toast_lbl;
static GtkCssProvider *css;
static GHashTable *nav_btns;       /* page -> button */
static AppIndicator *tray;
static GtkWidget *tray_toggle;

/* home */
static GtkWidget *h_btn, *h_btn_lbl, *h_status, *h_err, *h_server, *h_ip, *h_speed,
                 *h_total, *h_prof_name, *h_prof_usage, *h_prof_bar, *h_prof_exp, *h_mode;
/* proxies */
static GtkWidget *p_list, *p_title;
/* profiles */
static GtkWidget *pr_list, *pr_entry, *pr_add;
/* logs */
static GtkTextBuffer *log_buf;
static int log_lines;
/* settings */
static GtkWidget *st_tun_btn, *st_tun_lbl;

static gboolean quitting;
static gboolean on_signal(gpointer ud);

static void rebuild_proxies(void);
static void rebuild_profiles(void);
static void refresh_home(void);

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
    gtk_label_set_text(GTK_LABEL(toast_lbl), msg);
    gtk_revealer_set_reveal_child(GTK_REVEALER(toast_rev), TRUE);
    if (toast_timer) g_source_remove(toast_timer);
    toast_timer = g_timeout_add_seconds(3, hide_toast, NULL);
}

static void apply_theme(void)
{
    g_autofree char *c = theme_css(S.theme);
    gtk_css_provider_load_from_data(css, c, -1, NULL);
}

static const char *mode_name(int m)
{
    return m == MODE_TUN ? "TUN — весь трафик" : m == MODE_PROXY ? "Системный прокси" : "Только локальный порт";
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

static void tray_update(void)
{
    if (!tray) return;
    CoreState st = core_state();
    gtk_menu_item_set_label(GTK_MENU_ITEM(tray_toggle),
        st == ST_ON ? "Отключиться" : st == ST_OFF ? "Подключиться" : "…");
    app_indicator_set_icon_full(tray, st == ST_ON ? "network-vpn" : "network-vpn-disconnected",
                                st == ST_ON ? "Подключено" : "Отключено");
}

void ui_on_state(CoreState st, const char *error)
{
    if (quitting && st == ST_OFF) { g_application_quit(G_APPLICATION(app)); return; }
    const char *txt = st == ST_ON ? "Подключено" : st == ST_STARTING ? "Подключение…"
                    : st == ST_STOPPING ? "Отключение…" : "Отключено";
    gtk_label_set_text(GTK_LABEL(h_status), txt);
    set_class(h_status, "status-on", st == ST_ON);
    set_class(h_status, "status-off", st != ST_ON);
    gtk_label_set_text(GTK_LABEL(h_btn_lbl), st == ST_ON ? "ВЫКЛ" : st == ST_OFF ? "ВКЛ" : "…");
    set_class(h_btn, "on", st == ST_ON);
    set_class(h_btn, "busy", st == ST_STARTING || st == ST_STOPPING);
    if (error) {
        gtk_label_set_text(GTK_LABEL(h_err), error);
        gtk_widget_show(h_err);
    } else if (st != ST_OFF) {
        gtk_widget_hide(h_err);
    }
    if (st != ST_ON) gtk_label_set_text(GTK_LABEL(h_ip), "");
    rebuild_proxies();   /* delay semantics differ on/off */
    tray_update();
}

void ui_on_traffic(gint64 up, gint64 down)
{
    if (up < 0) {
        gtk_label_set_text(GTK_LABEL(h_speed), "");
        gtk_label_set_text(GTK_LABEL(h_total), "");
        return;
    }
    g_autofree char *u = fmt_bytes(up), *d = fmt_bytes(down);
    g_autofree char *s = g_strdup_printf("↑ %s/с    ↓ %s/с", u, d);
    gtk_label_set_text(GTK_LABEL(h_speed), s);
    g_autofree char *tu = fmt_bytes(core_total_up()), *td = fmt_bytes(core_total_down());
    g_autofree char *t = g_strdup_printf("за сессию: ↑ %s   ↓ %s", tu, td);
    gtk_label_set_text(GTK_LABEL(h_total), t);
}

void ui_on_log(const char *line)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(log_buf, &end);
    gtk_text_buffer_insert(log_buf, &end, line, -1);
    gtk_text_buffer_get_end_iter(log_buf, &end);
    gtk_text_buffer_insert(log_buf, &end, "\n", 1);
    /* keep the log bounded: memory budget matters more than history */
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
    if (ms == -2) { gtk_label_set_text(GTK_LABEL(l), "…"); return; }
    if (ms == 0) {
        gtk_label_set_text(GTK_LABEL(l), "✕");
        gtk_style_context_add_class(sc, "ping-bad");
        return;
    }
    g_autofree char *t = g_strdup_printf("%d мс", ms);
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
    if (!ip) { gtk_label_set_text(GTK_LABEL(h_ip), "IP: не удалось проверить"); return; }
    g_autofree char *flag = cc && *cc ? flag_emoji(cc) : g_strdup("");
    g_autofree char *t = g_strdup_printf("IP: %s %s", ip, flag);
    gtk_label_set_text(GTK_LABEL(h_ip), t);
}

void ui_on_tun_setup(gboolean ok, const char *msg)
{
    ui_toast(msg);
    gtk_widget_set_sensitive(st_tun_btn, TRUE);
    gtk_label_set_text(GTK_LABEL(st_tun_lbl), tun_ready() ? "Права выданы ✓" : "Права не выданы");
    if (ok && S.mode == MODE_TUN && core_state() == ST_OFF) gtk_widget_hide(h_err);
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

static void refresh_home(void)
{
    Profile *p = profile_active();
    const char *sel = S.selected && *S.selected ? S.selected : "auto";
    Server *s = profile_find_server(p, sel);
    g_autofree char *srv = s ? g_strdup(s->name) : g_strdup("⚡ Авто (лучший пинг)");
    gtk_label_set_text(GTK_LABEL(h_server), srv);
    gtk_label_set_text(GTK_LABEL(h_mode), mode_name(S.mode));

    if (!p) {
        gtk_label_set_text(GTK_LABEL(h_prof_name), "Нет профиля");
        gtk_label_set_text(GTK_LABEL(h_prof_usage), "Добавьте подписку на вкладке «Профили»");
        gtk_widget_hide(h_prof_bar);
        gtk_label_set_text(GTK_LABEL(h_prof_exp), "");
        return;
    }
    gtk_label_set_text(GTK_LABEL(h_prof_name), p->name);
    gint64 used = p->upload + p->download;
    if (p->total > 0) {
        g_autofree char *u = fmt_bytes(used), *t = fmt_bytes(p->total);
        g_autofree char *s2 = g_strdup_printf("%s из %s", u, t);
        gtk_label_set_text(GTK_LABEL(h_prof_usage), s2);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(h_prof_bar), CLAMP((double)used / p->total, 0, 1));
        gtk_widget_show(h_prof_bar);
    } else {
        g_autofree char *u = fmt_bytes(used);
        g_autofree char *s2 = used ? g_strdup_printf("использовано %s · безлимит", u)
                                   : g_strdup_printf("%d серверов", profile_server_count(p));
        gtk_label_set_text(GTK_LABEL(h_prof_usage), s2);
        gtk_widget_hide(h_prof_bar);
    }
    g_autofree char *exp = NULL;
    if (p->expire > 0) {
        g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(p->expire);
        gint64 days = (p->expire - g_get_real_time() / G_USEC_PER_SEC) / 86400;
        g_autofree char *d = g_date_time_format(dt, "%d.%m.%Y");
        exp = g_strdup_printf("до %s (%" G_GINT64_FORMAT " дн.)", d, MAX(days, 0));
    } else exp = g_strdup("бессрочно");
    gtk_label_set_text(GTK_LABEL(h_prof_exp), exp);
}

static GtkWidget *page_home(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(box), 24);

    GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(center, 10);

    h_btn = gtk_button_new();
    h_btn_lbl = gtk_label_new("ВКЛ");
    gtk_container_add(GTK_CONTAINER(h_btn), h_btn_lbl);
    add_class(h_btn, "connect");
    gtk_widget_set_halign(h_btn, GTK_ALIGN_CENTER);
    g_signal_connect(h_btn, "clicked", G_CALLBACK(on_connect), NULL);
    gtk_box_pack_start(GTK_BOX(center), h_btn, FALSE, FALSE, 0);

    h_status = gtk_label_new("Отключено");
    add_class(h_status, "big");
    add_class(h_status, "status-off");
    gtk_box_pack_start(GTK_BOX(center), h_status, FALSE, FALSE, 4);

    h_err = gtk_label_new("");
    add_class(h_err, "error");
    gtk_label_set_line_wrap(GTK_LABEL(h_err), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(h_err), 60);
    gtk_label_set_justify(GTK_LABEL(h_err), GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable(GTK_LABEL(h_err), TRUE);
    gtk_widget_set_no_show_all(h_err, TRUE);
    gtk_box_pack_start(GTK_BOX(center), h_err, FALSE, FALSE, 0);

    h_ip = gtk_label_new("");
    add_class(h_ip, "dim");
    gtk_box_pack_start(GTK_BOX(center), h_ip, FALSE, FALSE, 0);
    h_speed = gtk_label_new("");
    add_class(h_speed, "h2");
    gtk_box_pack_start(GTK_BOX(center), h_speed, FALSE, FALSE, 0);
    h_total = gtk_label_new("");
    add_class(h_total, "dim");
    gtk_box_pack_start(GTK_BOX(center), h_total, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), center, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);

    GtkWidget *c1 = card();
    gtk_box_pack_start(GTK_BOX(c1), label("Сервер", "dim"), FALSE, FALSE, 0);
    h_server = label("", "h2");
    gtk_label_set_ellipsize(GTK_LABEL(h_server), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(c1), h_server, FALSE, FALSE, 0);
    h_mode = label("", "dim");
    gtk_box_pack_start(GTK_BOX(c1), h_mode, FALSE, FALSE, 0);
    GtkWidget *chg = btn("Выбрать сервер", "flat-btn", G_CALLBACK(on_nav), "proxies");
    gtk_widget_set_halign(chg, GTK_ALIGN_START);
    gtk_widget_set_margin_top(chg, 4);
    gtk_box_pack_start(GTK_BOX(c1), chg, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), c1, TRUE, TRUE, 0);

    GtkWidget *c2 = card();
    gtk_box_pack_start(GTK_BOX(c2), label("Профиль", "dim"), FALSE, FALSE, 0);
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
    ui_toast(core_state() == ST_ON ? "Сервер переключён" : "Сервер выбран");
}

static void on_ping_all(GtkButton *b, gpointer ud)
{
    Profile *p = profile_active();
    if (!p) return;
    for (guint i = 0; i < p->servers->len; i++) {
        Server *s = p->servers->pdata[i];
        if (s->separator) continue;
        s->delay = -2;
        if (s->delay_label) set_delay_label(s->delay_label, -2);
        core_test_delay(s->tag);
    }
}

static void on_ping_one(GtkButton *b, gpointer ud)
{
    Server *s = profile_find_server(profile_active(), ud);
    if (!s) return;
    s->delay = -2;
    if (s->delay_label) set_delay_label(s->delay_label, -2);
    core_test_delay(s->tag);
}

static void on_sort_ping(GtkButton *b, gpointer ud)
{
    Profile *p = profile_active();
    if (!p) return;
    /* sort non-separator servers by delay; unknown/failed go last */
    GPtrArray *a = p->servers;
    for (guint i = 1; i < a->len; i++) {
        for (guint j = i; j > 0; j--) {
            Server *x = a->pdata[j - 1], *y = a->pdata[j];
            int dx = x->separator ? -3 : (x->delay > 0 ? x->delay : 1 << 30);
            int dy = y->separator ? -3 : (y->delay > 0 ? y->delay : 1 << 30);
            if (dx <= dy) break;
            a->pdata[j - 1] = y; a->pdata[j] = x;
        }
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
    if (sub) gtk_box_pack_start(GTK_BOX(v), label(sub, "dim"), FALSE, FALSE, 0);
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
        gtk_widget_set_tooltip_text(pb, "Проверить пинг");
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
    g_autofree char *title = p ? g_strdup_printf("Серверы · %d", profile_server_count(p)) : g_strdup("Серверы");
    gtk_label_set_text(GTK_LABEL(p_title), title);
    if (!p) return;

    gtk_container_add(GTK_CONTAINER(p_list),
        server_row("auto", "⚡ Авто", "лучший пинг среди зарубежных серверов, перепроверка каждые 2 мин (🇷🇺 не выбирается)", NULL));
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
    p_title = label("Серверы", "h1");
    gtk_box_pack_start(GTK_BOX(hdr), p_title, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hdr), btn("Сортировать по пингу", "flat-btn", G_CALLBACK(on_sort_ping), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hdr), btn("Проверить все", "accent-btn", G_CALLBACK(on_ping_all), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
        label("Без подключения пинг — это время TCP-соединения, с подключением — реальная задержка через сервер.", "dim"),
        FALSE, FALSE, 0);

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
    gtk_widget_set_sensitive(pr_add, TRUE);
    if (error) {
        g_autofree char *m = g_strdup_printf("Ошибка: %s", error);
        ui_toast(m);
        return;
    }
    if (ud) gtk_entry_set_text(GTK_ENTRY(pr_entry), "");
    Profile *p = profile_by_id(id);
    g_autofree char *m = p ? g_strdup_printf("«%s»: %d серверов", p->name, profile_server_count(p))
                           : g_strdup("Готово");
    ui_toast(m);
    rebuild_profiles();
    rebuild_proxies();
    refresh_home();
}

static void on_add_profile(GtkWidget *w, gpointer ud)
{
    const char *t = gtk_entry_get_text(GTK_ENTRY(pr_entry));
    if (!t || !*t) {
        /* empty field: take the clipboard, like Hiddify's "add from clipboard" */
        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        g_autofree char *clip = gtk_clipboard_wait_for_text(cb);
        if (!clip || !*g_strstrip(clip)) { ui_toast("Вставьте ссылку на подписку или ключ"); return; }
        gtk_entry_set_text(GTK_ENTRY(pr_entry), clip);
        t = gtk_entry_get_text(GTK_ENTRY(pr_entry));
    }
    gtk_widget_set_sensitive(pr_add, FALSE);
    ui_toast("Загружаю подписку…");
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
}

static void on_update_profile(GtkButton *b, gpointer id)
{
    Profile *p = profile_by_id(id);
    if (!p) return;
    ui_toast("Обновляю…");
    profile_update_async(p, on_profile_done, NULL);
}

static void on_delete_profile(GtkButton *b, gpointer id)
{
    Profile *p = profile_by_id(id);
    if (!p) return;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_OK_CANCEL, "Удалить профиль «%s»?", p->name);
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
    if (!PROFILES->len) {
        gtk_box_pack_start(GTK_BOX(pr_list), label("Профилей пока нет.", "dim"), FALSE, FALSE, 0);
    }
    for (guint i = 0; i < PROFILES->len; i++) {
        Profile *p = PROFILES->pdata[i];
        GtkWidget *c = card();
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        g_autofree char *nm = g_strdup_printf("%s%s", p == act ? "● " : "", p->name);
        GtkWidget *n = label(nm, "h2");
        gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(top), n, TRUE, TRUE, 0);
        if (p != act)
            gtk_box_pack_start(GTK_BOX(top), btn("Использовать", "accent-btn", G_CALLBACK(on_use_profile), p->id), FALSE, FALSE, 0);
        if (p->url)
            gtk_box_pack_start(GTK_BOX(top), btn("Обновить", "flat-btn", G_CALLBACK(on_update_profile), p->id), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(top), btn("Удалить", "flat-btn", G_CALLBACK(on_delete_profile), p->id), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(c), top, FALSE, FALSE, 0);

        GString *info = g_string_new(NULL);
        g_string_append_printf(info, "%d серверов", profile_server_count(p));
        if (p->total > 0) {
            g_autofree char *u = fmt_bytes(p->upload + p->download), *t = fmt_bytes(p->total);
            g_string_append_printf(info, " · %s / %s", u, t);
        }
        if (p->updated) {
            g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(p->updated);
            g_autofree char *d = g_date_time_format(dt, "%d.%m %H:%M");
            g_string_append_printf(info, " · обновлено %s", d);
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
    gtk_box_pack_start(GTK_BOX(box), label("Профили", "h1"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
        label("Ссылка на подписку (https://…) или ключи vless:// vmess:// trojan:// ss:// hy2:// tuic://. "
              "Пустое поле — вставить из буфера обмена.", "dim"), FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    pr_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(pr_entry), "https://… или vless://…");
    g_signal_connect(pr_entry, "activate", G_CALLBACK(on_add_profile), NULL);
    gtk_box_pack_start(GTK_BOX(row), pr_entry, TRUE, TRUE, 0);
    pr_add = btn("Добавить", "accent-btn", G_CALLBACK(on_add_profile), NULL);
    gtk_box_pack_start(GTK_BOX(row), pr_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    pr_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(pr_list, 6);
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
    gtk_box_pack_start(GTK_BOX(hdr), label("Логи ядра", "h1"), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hdr), btn("Очистить", "flat-btn", G_CALLBACK(on_clear_log), NULL), FALSE, FALSE, 0);
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

static void on_theme(GtkComboBox *c, gpointer ud)
{
    const char *id = gtk_combo_box_get_active_id(c);
    if (!id) return;
    g_free(S.theme);
    S.theme = g_strdup(id);
    settings_save();
    apply_theme();
}

static void on_mode(GtkComboBox *c, gpointer ud)
{
    int m = gtk_combo_box_get_active(c);
    if (m < 0 || m == S.mode) return;
    /* leaving proxy mode must undo the system proxy */
    if (S.mode == MODE_PROXY && core_state() == ST_ON) sysproxy_disable();
    S.mode = m;
    settings_save();
    refresh_home();
    if (m == MODE_TUN && !tun_ready()) ui_toast("Для TUN нажмите «Выдать права для TUN» ниже");
    core_restart();
}

static void on_bool(GtkSwitch *sw, GParamSpec *ps, gpointer field)
{
    *(gboolean *)field = gtk_switch_get_active(sw);
    settings_save();
    if (field == &S.bypass_ru) {
        if (S.bypass_ru) rules_update_async(FALSE);
        core_restart();
    }
}

static void on_port(GtkSpinButton *sb, gpointer field)
{
    *(int *)field = gtk_spin_button_get_value_as_int(sb);
    settings_save();
}

static void on_tun_setup(GtkButton *b, gpointer ud)
{
    gtk_widget_set_sensitive(st_tun_btn, FALSE);
    tun_setup_async();
}

static void on_update_rules(GtkButton *b, gpointer ud)
{
    rules_update_async(TRUE);
    ui_toast("Правила RU обновляются в фоне");
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

static GtkWidget *sw_for(gboolean *field)
{
    GtkWidget *s = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(s), *field);
    g_signal_connect(s, "notify::active", G_CALLBACK(on_bool), field);
    return s;
}

static GtkWidget *page_settings(void)
{
    GtkWidget *outer = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    gtk_box_pack_start(GTK_BOX(box), label("Настройки", "h1"), FALSE, FALSE, 0);

    GtkWidget *c = card();
    GtkWidget *theme = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme), "purple", "Фиолетовая");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme), "paper", "Бумажная");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(theme), S.theme);
    g_signal_connect(theme, "changed", G_CALLBACK(on_theme), NULL);
    gtk_box_pack_start(GTK_BOX(c), setting_row("Тема", NULL, theme), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);

    c = card();
    GtkWidget *mode = gtk_combo_box_text_new();
    for (int i = 0; i < 3; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), mode_name(i));
    gtk_combo_box_set_active(GTK_COMBO_BOX(mode), S.mode);
    g_signal_connect(mode, "changed", G_CALLBACK(on_mode), NULL);
    gtk_box_pack_start(GTK_BOX(c), setting_row("Режим",
        "TUN — весь трафик системы, как VPN в Hiddify (рекомендуется). Системный прокси — только программы, которые его читают; Firefox может его игнорировать.",
        mode), FALSE, FALSE, 0);
    GtkWidget *tunbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    st_tun_lbl = label(tun_ready() ? "Права выданы ✓" : "Права не выданы", "dim");
    gtk_box_pack_start(GTK_BOX(tunbox), st_tun_lbl, FALSE, FALSE, 0);
    st_tun_btn = btn("Выдать права для TUN", "flat-btn", G_CALLBACK(on_tun_setup), NULL);
    gtk_box_pack_start(GTK_BOX(tunbox), st_tun_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), setting_row("TUN",
        "Один раз копирует sing-box в " WL_TUN_BIN " с правом CAP_NET_ADMIN (спросит пароль). "
        "Wellide при этом работает без root.", tunbox), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);

    c = card();
    GtkWidget *rules = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(rules), btn("Обновить правила", "flat-btn", G_CALLBACK(on_update_rules), NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rules), sw_for(&S.bypass_ru), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), setting_row("Российские сайты напрямую",
        "Домены .ru/.su/.рф и российские IP идут мимо VPN (Госуслуги, банки, Яндекс).", rules), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), setting_row("Подключаться при запуске", NULL, sw_for(&S.autoconnect)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), setting_row("Запускаться свёрнутым в трей", NULL, sw_for(&S.start_hidden)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);

    c = card();
    GtkWidget *port = gtk_spin_button_new_with_range(1024, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port), S.port);
    g_signal_connect(port, "value-changed", G_CALLBACK(on_port), &S.port);
    gtk_box_pack_start(GTK_BOX(c), setting_row("Порт прокси (HTTP + SOCKS5)",
        "Применится при следующем подключении.", port), FALSE, FALSE, 0);
    GtkWidget *aport = gtk_spin_button_new_with_range(1024, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(aport), S.api_port);
    g_signal_connect(aport, "value-changed", G_CALLBACK(on_port), &S.api_port);
    gtk_box_pack_start(GTK_BOX(c), setting_row("Порт Clash API", NULL, aport), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), label("Wellide " WL_VERSION " · ядро sing-box", "dim"), FALSE, FALSE, 4);
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
    if (tray) { gtk_widget_hide(win); return TRUE; }  /* keep running in tray */
    return FALSE;
}

static void do_quit(void)
{
    if (core_state() == ST_OFF) { g_application_quit(G_APPLICATION(app)); return; }
    quitting = TRUE;
    core_stop();
}

static void on_tray_show(GtkMenuItem *i, gpointer ud) { show_window(); }
static void on_tray_toggle(GtkMenuItem *i, gpointer ud) { on_connect(NULL, NULL); }
static void on_tray_quit(GtkMenuItem *i, gpointer ud) { do_quit(); }

static void build_tray(void)
{
    tray = app_indicator_new("wellide", "network-vpn-disconnected", APP_INDICATOR_CATEGORY_COMMUNICATIONS);
    app_indicator_set_status(tray, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(tray, "Wellide");
    GtkWidget *m = gtk_menu_new();
    GtkWidget *show = gtk_menu_item_new_with_label("Открыть Wellide");
    g_signal_connect(show, "activate", G_CALLBACK(on_tray_show), NULL);
    tray_toggle = gtk_menu_item_new_with_label("Подключиться");
    g_signal_connect(tray_toggle, "activate", G_CALLBACK(on_tray_toggle), NULL);
    GtkWidget *quit = gtk_menu_item_new_with_label("Выход");
    g_signal_connect(quit, "activate", G_CALLBACK(on_tray_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), show);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), tray_toggle);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(m), quit);
    gtk_widget_show_all(m);
    app_indicator_set_menu(tray, GTK_MENU(m));
    app_indicator_set_secondary_activate_target(tray, show);
}

static GtkWidget *nav_button(GtkWidget *side, const char *text, const char *page)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_container_add(GTK_CONTAINER(b), l);
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
    apply_theme();
    nav_btns = g_hash_table_new(g_str_hash, g_str_equal);

    win = gtk_application_window_new(a);
    gtk_window_set_title(GTK_WINDOW(win), "Wellide");
    gtk_window_set_default_size(GTK_WINDOW(win), 860, 600);
    gtk_window_set_icon_name(GTK_WINDOW(win), "wellide");
    g_signal_connect(win, "delete-event", G_CALLBACK(on_delete), NULL);

    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(side, "sidebar");
    gtk_widget_set_size_request(side, 190, -1);
    GtkWidget *brand = label("◆ Wellide", "brand");
    gtk_box_pack_start(GTK_BOX(side), brand, FALSE, FALSE, 0);
    nav_button(side, "Главная", "home");
    nav_button(side, "Серверы", "proxies");
    nav_button(side, "Профили", "profiles");
    nav_button(side, "Логи", "logs");
    nav_button(side, "Настройки", "settings");
    GtkWidget *quit = btn("Выход", "flat-btn", G_CALLBACK(do_quit), NULL);
    gtk_box_pack_end(GTK_BOX(side), quit, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(root), side, FALSE, FALSE, 0);

    stack = gtk_stack_new();
    add_class(stack, "main-bg");
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_add_named(GTK_STACK(stack), page_home(), "home");
    gtk_stack_add_named(GTK_STACK(stack), page_proxies(), "proxies");
    gtk_stack_add_named(GTK_STACK(stack), page_profiles(), "profiles");
    gtk_stack_add_named(GTK_STACK(stack), page_logs(), "logs");
    gtk_stack_add_named(GTK_STACK(stack), page_settings(), "settings");
    gtk_box_pack_start(GTK_BOX(root), stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(overlay), root);

    toast_rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(toast_rev), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
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

    build_tray();
    rebuild_proxies();
    rebuild_profiles();
    refresh_home();
    go(PROFILES->len ? "home" : "profiles");

    gtk_widget_show_all(overlay);
    if (!(S.start_hidden && tray)) show_window();
    g_application_hold(G_APPLICATION(a));

    if (S.bypass_ru) rules_update_async(FALSE);
    auto_update(NULL);
    g_timeout_add_seconds(1800, auto_update, NULL);
    if (S.autoconnect && PROFILES->len) core_start();
}

/* CLI control, forwarded to the primary instance by GApplication:
 *   wellide --import <url|link>  --connect  --disconnect  --toggle  --quit  --status */
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
        } else if (!strcmp(o, "--help") || !strcmp(o, "-h")) {
            g_application_command_line_print(cl,
                "wellide [--hidden] [--import URL] [--connect|--disconnect|--toggle] [--status] [--quit]\n");
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

/* "startup" runs only in the primary instance. Remote invocations
 * (wellide --status etc.) must not touch state, or they would undo the
 * running instance's system proxy. */
static void on_startup(GApplication *a, gpointer ud)
{
    if (S.sysproxy_set) sysproxy_disable();   /* previous run crashed */
    HTTP = soup_session_new_with_options("timeout", 30, NULL);
    profiles_load();
    g_unix_signal_add(SIGINT, on_signal, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
}

static gboolean on_signal(gpointer ud)
{
    do_quit();
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    settings_load();

    app = gtk_application_new(WL_APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}

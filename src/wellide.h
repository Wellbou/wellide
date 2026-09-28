/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#ifndef G_OS_WIN32
#include <glib-unix.h>
#include <unistd.h>
#endif

#define WL_VERSION "0.2.0"
#define WL_APP_ID  "io.github.wellbou.wellide"
#define WL_REPO    "https://github.com/Wellbou/wellide"
#ifndef WL_TUN_BIN
#define WL_TUN_BIN "/usr/local/lib/wellide/sing-box"
#endif

/* ---------- i18n: two languages, picked from the locale ---------- */
extern gboolean WL_RU;
#define N_(ru, en) (WL_RU ? (ru) : (en))

/* ---------- model ---------- */

typedef struct {
    char *tag;        /* unique, used as sing-box outbound tag */
    char *name;       /* display name from the link fragment */
    char *proto;      /* "vless · ws · tls" etc, for the UI */
    char *server;
    int port;
    char *cc;         /* ISO country from the flag emoji, may be NULL */
    JsonObject *ob;   /* sing-box outbound (NULL for separators) */
    gboolean separator;
    int delay;        /* -1 unknown, -2 testing, 0 failed, >0 ms */
    gboolean delay_tcp;
    GtkWidget *delay_label; /* UI only, not owned */
} Server;

typedef struct {
    char *id;
    char *name;
    char *url;        /* NULL for pasted link lists */
    gint64 upload, download, total, expire, updated;
    int interval_h;
    GPtrArray *servers; /* Server* */
} Profile;

typedef enum { MODE_PROXY = 0, MODE_TUN = 1, MODE_LOCAL = 2 } NetMode;
typedef enum { ST_OFF, ST_STARTING, ST_ON, ST_STOPPING } CoreState;

typedef struct {
    char *theme;
    char *lang;       /* "auto" | "ru" | "en" */
    int mode;
    char *region;     /* "" (off) | "ru" | "ir" | "cn": that country's sites go direct */
    gboolean auto_skip_region; /* "auto" never picks servers inside that country */
    int port;
    int api_port;
    gboolean autoconnect;
    gboolean start_hidden;
    gboolean animations;
    gboolean eco_fps;     /* idle button at 20 fps instead of the display rate */
    char *active;     /* profile id */
    char *selected;   /* outbound tag or "auto" */
    gboolean sysproxy_set;   /* crash recovery marker */
    char *kde_prev_type;
    char *gnome_prev_mode;
    int win_prev_enable;
    gint64 rules_updated;
} Settings;

extern Settings S;
extern GPtrArray *PROFILES;
extern SoupSession *HTTP;

/* settings.c */
char *wl_config_dir(void);
char *wl_cache_dir(void);
void settings_load(void);
void settings_save(void);
char *fmt_bytes(gint64 b);
char *flag_emoji(const char *cc);
char *cc_from_flag(const char *text);

/* links.c */
GPtrArray *links_parse(const char *text);   /* -> Server* (tags not unique yet) */
void server_free(Server *s);

/* profiles.c */
typedef void (*ProfileDoneCb)(const char *profile_id, const char *error, gpointer ud);
void profiles_load(void);
void profiles_save(void);
Profile *profile_active(void);
Profile *profile_by_id(const char *id);
Server *profile_find_server(Profile *p, const char *tag);
int profile_server_count(Profile *p);
void profile_add_async(const char *input, ProfileDoneCb cb, gpointer ud);
void profile_update_async(Profile *p, ProfileDoneCb cb, gpointer ud);
void profile_delete(Profile *p);
void rules_update_async(gboolean force);
char *rules_path(const char *name);
gboolean rules_available(const char *region, gboolean *geoip, gboolean *geosite);

/* core.c */
CoreState core_state(void);
void core_start(void);
void core_stop(void);
void core_restart(void);
void core_select(const char *tag);
void core_test_delay(const char *tag);
void core_test_all(void);
void core_check_ip(void);
gint64 core_total_up(void);
gint64 core_total_down(void);
const char *core_bin(void);
gboolean server_in_region(Server *s);
gboolean tun_ready(void);
void tun_setup_async(void);

/* sysproxy.c */
void sysproxy_enable(int port);
void sysproxy_disable(void);

/* theme.c — see the header comment there for the custom theme format */
typedef enum {
    TC_BG, TC_BG2, TC_CARD, TC_FG, TC_FG_DIM, TC_ACCENT, TC_ACCENT_FG,
    TC_LINE, TC_DANGER, TC_INK, TC_GLOW, TC_RULE, TC_MARGIN, TC_N
} ThemeColor;

/* every theme has its own connect button */
typedef enum { BTN_PIXEL, BTN_SKETCH, BTN_CRYSTAL, BTN_DIAL } ButtonStyle;

typedef struct {
    char *id, *name;
    char *c[TC_N];
    ButtonStyle button;
    gboolean ruled, outline, pixel, builtin;
    int radius;
    char *css_extra;
} Theme;

void themes_reload(void);
void themes_watch(void (*cb)(void));
GPtrArray *themes_list(void);
const Theme *theme_current(void);
char *theme_css(const Theme *t);
char *themes_dir(void);

/* vortex.c — the connect button: arms orbit around it, gather, flash */
GtkWidget *vortex_new(void);
void vortex_set_theme(GtkWidget *w, const Theme *t);
void vortex_set_state(GtkWidget *w, CoreState st);
void vortex_set_label(GtkWidget *w, const char *text);
void vortex_set_animated(GtkWidget *w, gboolean on);
void vortex_on_click(GtkWidget *w, void (*cb)(void));
void vortex_paint_preview(cairo_t *cr, const Theme *t, double size);
cairo_surface_t *vortex_icon_surface(int cells, int px, const char *ink, const char *glow);
gpointer vortex_sim_new(const Theme *t, const char *label);
void vortex_sim_step(gpointer sim, gint64 t_us);
void vortex_sim_state(gpointer sim, CoreState st, const char *label);
void vortex_sim_paint(gpointer sim, cairo_t *cr, double size);
void vortex_sim_free(gpointer sim);

/* graph.c — smooth traffic sparkline */
GtkWidget *graph_new(void);
void graph_push(GtkWidget *w, gint64 up, gint64 down);
void graph_clear(GtkWidget *w);
void graph_set_colors(GtkWidget *w, const char *up, const char *down, const char *grid);

/* callbacks into the UI (main.c) */
void ui_on_state(CoreState st, const char *error);
void ui_on_traffic(gint64 up, gint64 down);
void ui_on_log(const char *line);
void ui_on_delay(const char *tag, int ms, gboolean tcp);
void ui_on_ip(const char *ip, const char *cc);
void ui_on_tun_setup(gboolean ok, const char *msg);
void ui_toast(const char *msg);

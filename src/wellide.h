#pragma once

#include <gtk/gtk.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#define WL_VERSION "0.1.0"
#define WL_APP_ID  "com.wellbou.wellide"
#define WL_TUN_BIN "/usr/local/lib/wellide/sing-box"

/* ---------- model ---------- */

typedef struct {
    char *tag;        /* unique, used as sing-box outbound tag */
    char *name;       /* display name from the link fragment */
    char *proto;      /* "vless · ws" etc, for the UI */
    char *server;
    int port;
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
    char *theme;      /* "purple" | "paper" */
    int mode;
    gboolean bypass_ru;
    int port;
    int api_port;
    gboolean autoconnect;
    gboolean start_hidden;
    char *active;     /* profile id */
    char *selected;   /* outbound tag or "auto" */
    gboolean sysproxy_set;   /* crash recovery marker */
    char *kde_prev_type;
    char *gnome_prev_mode;
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

/* core.c */
CoreState core_state(void);
void core_start(void);
void core_stop(void);
void core_restart(void);
void core_select(const char *tag);
void core_test_delay(const char *tag);
void core_check_ip(void);
gboolean server_is_ru(Server *s);
gboolean tun_ready(void);
void tun_setup_async(void);

/* sysproxy.c */
void sysproxy_enable(int port);
void sysproxy_disable(void);

/* theme.c */
char *theme_css(const char *theme);

/* callbacks into the UI (main.c) */
void ui_on_state(CoreState st, const char *error);
void ui_on_traffic(gint64 up, gint64 down);
void ui_on_log(const char *line);
void ui_on_delay(const char *tag, int ms, gboolean tcp);
void ui_on_ip(const char *ip, const char *cc);
void ui_on_tun_setup(gboolean ok, const char *msg);
void ui_toast(const char *msg);

/* helpers */
char *fmt_bytes(gint64 b);
char *flag_emoji(const char *cc);

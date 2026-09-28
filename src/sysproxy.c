/* SPDX-License-Identifier: GPL-3.0-or-later */
/* System proxy for KDE (kioslaverc) and GNOME (gsettings). Firefox,
 * Chromium, Telegram etc. follow these when set to "system proxy".
 * Previous values are stored in settings so a crash can be undone on the
 * next launch (S.sysproxy_set). */
#include "wellide.h"
#include <string.h>

#ifdef G_OS_WIN32
/* ---------- Windows: WinINet per-user proxy ---------- */
#include <windows.h>
#include <wininet.h>

static void win_notify(void)
{
    InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
}

void sysproxy_enable(int port)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
            0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (!S.sysproxy_set) {
        DWORD v = 0, sz = sizeof v;
        RegQueryValueExW(k, L"ProxyEnable", NULL, NULL, (BYTE *)&v, &sz);
        S.win_prev_enable = (int)v;
    }
    wchar_t srv[64];
    swprintf(srv, 64, L"127.0.0.1:%d", port);
    DWORD one = 1;
    RegSetValueExW(k, L"ProxyServer", 0, REG_SZ, (BYTE *)srv, (DWORD)((wcslen(srv) + 1) * sizeof(wchar_t)));
    const wchar_t *bypass = L"localhost;127.*;10.*;172.16.*;192.168.*;<local>";
    RegSetValueExW(k, L"ProxyOverride", 0, REG_SZ, (BYTE *)bypass, (DWORD)((wcslen(bypass) + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"ProxyEnable", 0, REG_DWORD, (BYTE *)&one, sizeof one);
    RegCloseKey(k);
    win_notify();
    S.sysproxy_set = TRUE;
    settings_save();
}

void sysproxy_disable(void)
{
    if (!S.sysproxy_set) return;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
            0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        DWORD v = (DWORD)S.win_prev_enable;
        RegSetValueExW(k, L"ProxyEnable", 0, REG_DWORD, (BYTE *)&v, sizeof v);
        RegCloseKey(k);
        win_notify();
    }
    S.sysproxy_set = FALSE;
    settings_save();
}

#else /* ---------- Linux: KDE + GNOME ---------- */

static void run(const char *const *argv)
{
    g_autofree char *bin = g_find_program_in_path(argv[0]);
    if (!bin) return;
    g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH |
                 G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, NULL, NULL);
}

static char *capture(const char *const *argv)
{
    g_autofree char *bin = g_find_program_in_path(argv[0]);
    if (!bin) return NULL;
    char *out = NULL;
    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &out, NULL, NULL, NULL)) return NULL;
    return out ? g_strstrip(out) : NULL;
}

static void watch_start(int port);

static void kde_notify(void)
{
    run((const char *const[]){ "dbus-send", "--type=signal", "/KIO/Scheduler",
        "org.kde.KIO.Scheduler.reparseSlaveConfiguration", "string:", NULL });
}

static void kde_set(const char *key, const char *val)
{
    run((const char *const[]){ "kwriteconfig6", "--file", "kioslaverc",
        "--group", "Proxy Settings", "--key", key, val, NULL });
}

void sysproxy_enable(int port)
{
    if (!S.sysproxy_set) {
        g_autofree char *kt = capture((const char *const[]){ "kreadconfig6", "--file", "kioslaverc",
            "--group", "Proxy Settings", "--key", "ProxyType", NULL });
        g_autofree char *gm = capture((const char *const[]){ "gsettings", "get",
            "org.gnome.system.proxy", "mode", NULL });
        g_free(S.kde_prev_type);
        S.kde_prev_type = g_strdup(kt && *kt ? kt : "0");
        g_free(S.gnome_prev_mode);
        S.gnome_prev_mode = g_strdup(gm && *gm ? gm : "'none'");
        g_strdelimit(S.gnome_prev_mode, "'", ' ');
        g_strstrip(S.gnome_prev_mode);
    }

    g_autofree char *http = g_strdup_printf("http://127.0.0.1 %d", port);
    g_autofree char *socks = g_strdup_printf("socks://127.0.0.1 %d", port);
    kde_set("httpProxy", http);
    kde_set("httpsProxy", http);
    kde_set("ftpProxy", http);
    kde_set("socksProxy", socks);
    kde_set("NoProxyFor", "localhost,127.0.0.0/8,::1,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16");
    kde_set("ProxyType", "1");
    kde_notify();

    g_autofree char *ps = g_strdup_printf("%d", port);
    const char *schemes[] = { "http", "https", "socks" };
    for (int i = 0; i < 3; i++) {
        g_autofree char *schema = g_strdup_printf("org.gnome.system.proxy.%s", schemes[i]);
        run((const char *const[]){ "gsettings", "set", schema, "host", "127.0.0.1", NULL });
        run((const char *const[]){ "gsettings", "set", schema, "port", ps, NULL });
    }
    run((const char *const[]){ "gsettings", "set", "org.gnome.system.proxy", "mode", "manual", NULL });

    S.sysproxy_set = TRUE;
    settings_save();
    watch_start(port);
}

/* ---------- watchdog ----------
 * Other VPN apps (Hiddify on exit, NekoRay...) reset the desktop proxy and
 * silently send the browser direct. While we own the proxy, re-assert it
 * whenever kioslaverc or the GNOME proxy mode changes. Our own writes are
 * idempotent, so the monitor settling on "already correct" ends the loop. */

static GFileMonitor *kio_mon;
static GSettings *gnome_proxy;
static guint recheck_id;
static int watch_port;

static char *kde_proxy_type(void)
{
    g_autofree char *path = g_build_filename(g_get_user_config_dir(), "kioslaverc", NULL);
    g_autofree char *txt = NULL;
    if (!g_file_get_contents(path, &txt, NULL, NULL)) return g_strdup("0");
    gboolean in_group = FALSE;
    char **lines = g_strsplit(txt, "\n", -1);
    char *val = NULL;
    for (int i = 0; lines[i] && !val; i++) {
        char *l = g_strstrip(lines[i]);
        if (l[0] == '[') { in_group = g_str_has_prefix(l, "[Proxy Settings]"); continue; }
        if (in_group && g_str_has_prefix(l, "ProxyType")) {
            char *eq = strchr(l, '=');
            if (eq) val = g_strstrip(g_strdup(eq + 1));
        }
    }
    g_strfreev(lines);
    return val ? val : g_strdup("0");
}

static gboolean recheck(gpointer ud)
{
    recheck_id = 0;
    if (!S.sysproxy_set) return G_SOURCE_REMOVE;
    g_autofree char *kt = kde_proxy_type();
    g_autofree char *gm = gnome_proxy ? g_settings_get_string(gnome_proxy, "mode") : g_strdup("manual");
    if (strcmp(kt, "1") || strcmp(gm, "manual")) {
        sysproxy_enable(watch_port);   /* silently: nothing for the user to do */
    }
    return G_SOURCE_REMOVE;
}

static void schedule_recheck(void)
{
    if (recheck_id) g_source_remove(recheck_id);
    recheck_id = g_timeout_add(700, recheck, NULL);
}

static void on_kio_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer ud)
{
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_CREATED ||
        ev == G_FILE_MONITOR_EVENT_RENAMED || ev == G_FILE_MONITOR_EVENT_MOVED_IN)
        schedule_recheck();
}

static void on_gnome_changed(GSettings *s, const char *key, gpointer ud) { schedule_recheck(); }

static void watch_start(int port)
{
    watch_port = port;
    if (!kio_mon) {
        g_autoptr(GFile) dir = g_file_new_for_path(g_get_user_config_dir());
        /* monitor the directory: kwriteconfig replaces the file atomically */
        kio_mon = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
        if (kio_mon) g_signal_connect(kio_mon, "changed", G_CALLBACK(on_kio_changed), NULL);
    }
    if (!gnome_proxy) {
        GSettingsSchemaSource *src = g_settings_schema_source_get_default();
        g_autoptr(GSettingsSchema) sc = src ? g_settings_schema_source_lookup(src, "org.gnome.system.proxy", TRUE) : NULL;
        if (sc) {
            gnome_proxy = g_settings_new("org.gnome.system.proxy");
            g_signal_connect(gnome_proxy, "changed::mode", G_CALLBACK(on_gnome_changed), NULL);
        }
    }
}

static void watch_stop(void)
{
    if (recheck_id) { g_source_remove(recheck_id); recheck_id = 0; }
    g_clear_object(&kio_mon);
    g_clear_object(&gnome_proxy);
}

void sysproxy_disable(void)
{
    watch_stop();
    if (!S.sysproxy_set) return;
    kde_set("ProxyType", S.kde_prev_type && *S.kde_prev_type ? S.kde_prev_type : "0");
    kde_notify();
    run((const char *const[]){ "gsettings", "set", "org.gnome.system.proxy", "mode",
        S.gnome_prev_mode && *S.gnome_prev_mode ? S.gnome_prev_mode : "none", NULL });
    S.sysproxy_set = FALSE;
    settings_save();
}

#endif

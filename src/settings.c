#include "wellide.h"

Settings S;

static char *ensure_dir(const char *base, const char *sub)
{
    char *d = g_build_filename(base, sub, NULL);
    g_mkdir_with_parents(d, 0700);
    return d;
}

char *wl_config_dir(void) { return ensure_dir(g_get_user_config_dir(), "wellide"); }
char *wl_cache_dir(void)  { return ensure_dir(g_get_user_cache_dir(), "wellide"); }

static char *ini_path(void)
{
    g_autofree char *d = wl_config_dir();
    return g_build_filename(d, "settings.ini", NULL);
}

static char *kf_str(GKeyFile *kf, const char *k, const char *def)
{
    char *v = g_key_file_get_string(kf, "main", k, NULL);
    return v ? v : g_strdup(def);
}

static int kf_int(GKeyFile *kf, const char *k, int def)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, "main", k, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

static gboolean kf_bool(GKeyFile *kf, const char *k, gboolean def)
{
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, "main", k, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

void settings_load(void)
{
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_autofree char *p = ini_path();
    g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL);

    S.theme = kf_str(kf, "theme", "purple");
    S.mode = CLAMP(kf_int(kf, "mode", MODE_TUN), 0, 2);
    S.bypass_ru = kf_bool(kf, "bypass_ru", TRUE);
    S.port = CLAMP(kf_int(kf, "port", 12400), 1024, 65535);
    S.api_port = CLAMP(kf_int(kf, "api_port", 12409), 1024, 65535);
    S.autoconnect = kf_bool(kf, "autoconnect", FALSE);
    S.start_hidden = kf_bool(kf, "start_hidden", FALSE);
    S.active = kf_str(kf, "active", "");
    S.selected = kf_str(kf, "selected", "auto");
    S.sysproxy_set = kf_bool(kf, "sysproxy_set", FALSE);
    S.kde_prev_type = kf_str(kf, "kde_prev_type", "0");
    S.gnome_prev_mode = kf_str(kf, "gnome_prev_mode", "none");
    S.rules_updated = g_key_file_get_int64(kf, "main", "rules_updated", NULL);
}

void settings_save(void)
{
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_set_string(kf, "main", "theme", S.theme);
    g_key_file_set_integer(kf, "main", "mode", S.mode);
    g_key_file_set_boolean(kf, "main", "bypass_ru", S.bypass_ru);
    g_key_file_set_integer(kf, "main", "port", S.port);
    g_key_file_set_integer(kf, "main", "api_port", S.api_port);
    g_key_file_set_boolean(kf, "main", "autoconnect", S.autoconnect);
    g_key_file_set_boolean(kf, "main", "start_hidden", S.start_hidden);
    g_key_file_set_string(kf, "main", "active", S.active ? S.active : "");
    g_key_file_set_string(kf, "main", "selected", S.selected ? S.selected : "auto");
    g_key_file_set_boolean(kf, "main", "sysproxy_set", S.sysproxy_set);
    g_key_file_set_string(kf, "main", "kde_prev_type", S.kde_prev_type);
    g_key_file_set_string(kf, "main", "gnome_prev_mode", S.gnome_prev_mode);
    g_key_file_set_int64(kf, "main", "rules_updated", S.rules_updated);

    g_autofree char *p = ini_path();
    g_key_file_save_to_file(kf, p, NULL);
}

char *fmt_bytes(gint64 b)
{
    const char *u[] = { "Б", "КБ", "МБ", "ГБ", "ТБ" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    if (i == 0) return g_strdup_printf("%d %s", (int)v, u[i]);
    return g_strdup_printf(v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, u[i]);
}

char *flag_emoji(const char *cc)
{
    if (!cc || strlen(cc) != 2) return g_strdup("🌐");
    gunichar a = 0x1F1E6 + (g_ascii_toupper(cc[0]) - 'A');
    gunichar b = 0x1F1E6 + (g_ascii_toupper(cc[1]) - 'A');
    char buf[16] = { 0 };
    int n = g_unichar_to_utf8(a, buf);
    g_unichar_to_utf8(b, buf + n);
    return g_strdup(buf);
}

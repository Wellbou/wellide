#include "wellide.h"
#include <string.h>

Settings S;
gboolean WL_RU;

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

static void detect_lang(void)
{
    if (!g_strcmp0(S.lang, "ru")) { WL_RU = TRUE; return; }
    if (!g_strcmp0(S.lang, "en")) { WL_RU = FALSE; return; }
    const char *const *langs = g_get_language_names();
    WL_RU = FALSE;
    for (int i = 0; langs[i]; i++) {
        if (g_str_has_prefix(langs[i], "ru") || g_str_has_prefix(langs[i], "uk") ||
            g_str_has_prefix(langs[i], "be")) { WL_RU = TRUE; break; }
        if (g_str_has_prefix(langs[i], "en")) break;
    }
#ifdef G_OS_WIN32
    {
        char *l = g_win32_getlocale();
        WL_RU = l && (g_str_has_prefix(l, "ru") || g_str_has_prefix(l, "uk") || g_str_has_prefix(l, "be"));
        g_free(l);
    }
#endif
}

/* default region: the user's own country, if we have rules for it */
static const char *default_region(void)
{
    const char *const *langs = g_get_language_names();
    for (int i = 0; langs[i]; i++) {
        if (g_str_has_prefix(langs[i], "ru")) return "ru";
        if (g_str_has_prefix(langs[i], "fa")) return "ir";
        if (g_str_has_prefix(langs[i], "zh_CN")) return "cn";
    }
    return "";
}

void settings_load(void)
{
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_autofree char *p = ini_path();
    gboolean fresh = !g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL);

    S.lang = kf_str(kf, "lang", "auto");
    detect_lang();
    S.theme = kf_str(kf, "theme", "void");
    if (!strcmp(S.theme, "paper")) { g_free(S.theme); S.theme = g_strdup("notebook"); }  /* v0.1 name */
    S.mode = CLAMP(kf_int(kf, "mode", MODE_TUN), 0, 2);
    /* v0.1 had a boolean bypass_ru */
    if (g_key_file_has_key(kf, "main", "region", NULL))
        S.region = kf_str(kf, "region", "");
    else if (!fresh && g_key_file_has_key(kf, "main", "bypass_ru", NULL))
        S.region = g_strdup(kf_bool(kf, "bypass_ru", TRUE) ? "ru" : "");
    else
        S.region = g_strdup(default_region());
    S.auto_skip_region = kf_bool(kf, "auto_skip_region", TRUE);
    S.port = CLAMP(kf_int(kf, "port", 12400), 1024, 65535);
    S.api_port = CLAMP(kf_int(kf, "api_port", 12409), 1024, 65535);
    S.autoconnect = kf_bool(kf, "autoconnect", FALSE);
    S.start_hidden = kf_bool(kf, "start_hidden", FALSE);
    S.animations = kf_bool(kf, "animations", TRUE);
    S.active = kf_str(kf, "active", "");
    S.selected = kf_str(kf, "selected", "auto");
    S.sysproxy_set = kf_bool(kf, "sysproxy_set", FALSE);
    S.kde_prev_type = kf_str(kf, "kde_prev_type", "0");
    S.gnome_prev_mode = kf_str(kf, "gnome_prev_mode", "none");
    S.win_prev_enable = kf_int(kf, "win_prev_enable", 0);
    S.rules_updated = g_key_file_get_int64(kf, "main", "rules_updated", NULL);
    if (strcmp(S.region, "ru") && strcmp(S.region, "ir") && strcmp(S.region, "cn")) {
        g_free(S.region);
        S.region = g_strdup("");
    }
}

void settings_save(void)
{
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_set_string(kf, "main", "lang", S.lang);
    g_key_file_set_string(kf, "main", "theme", S.theme);
    g_key_file_set_integer(kf, "main", "mode", S.mode);
    g_key_file_set_string(kf, "main", "region", S.region);
    g_key_file_set_boolean(kf, "main", "auto_skip_region", S.auto_skip_region);
    g_key_file_set_integer(kf, "main", "port", S.port);
    g_key_file_set_integer(kf, "main", "api_port", S.api_port);
    g_key_file_set_boolean(kf, "main", "autoconnect", S.autoconnect);
    g_key_file_set_boolean(kf, "main", "start_hidden", S.start_hidden);
    g_key_file_set_boolean(kf, "main", "animations", S.animations);
    g_key_file_set_string(kf, "main", "active", S.active ? S.active : "");
    g_key_file_set_string(kf, "main", "selected", S.selected ? S.selected : "auto");
    g_key_file_set_boolean(kf, "main", "sysproxy_set", S.sysproxy_set);
    g_key_file_set_string(kf, "main", "kde_prev_type", S.kde_prev_type);
    g_key_file_set_string(kf, "main", "gnome_prev_mode", S.gnome_prev_mode);
    g_key_file_set_integer(kf, "main", "win_prev_enable", S.win_prev_enable);
    g_key_file_set_int64(kf, "main", "rules_updated", S.rules_updated);

    g_autofree char *p = ini_path();
    g_key_file_save_to_file(kf, p, NULL);
}

char *fmt_bytes(gint64 b)
{
    const char *ru[] = { "Б", "КБ", "МБ", "ГБ", "ТБ" };
    const char *en[] = { "B", "KB", "MB", "GB", "TB" };
    const char **u = WL_RU ? ru : en;
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

/* first regional-indicator pair in the text -> "NL" */
char *cc_from_flag(const char *text)
{
    if (!text) return NULL;
    for (const char *p = text; *p; p = g_utf8_next_char(p)) {
        gunichar a = g_utf8_get_char(p);
        if (a < 0x1F1E6 || a > 0x1F1FF) continue;
        const char *q = g_utf8_next_char(p);
        if (!*q) break;
        gunichar b = g_utf8_get_char(q);
        if (b < 0x1F1E6 || b > 0x1F1FF) continue;
        char cc[3] = { (char)('A' + (a - 0x1F1E6)), (char)('A' + (b - 0x1F1E6)), 0 };
        return g_strdup(cc);
    }
    return NULL;
}

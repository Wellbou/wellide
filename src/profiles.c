/* Profiles: subscription URL or pasted links. Stored as
 *   ~/.config/wellide/profiles.json   (metadata)
 *   ~/.config/wellide/profiles/<id>.txt (raw subscription body)
 * The raw body is re-parsed on load, so parser fixes apply retroactively. */
#include "wellide.h"
#include <string.h>

GPtrArray *PROFILES;
SoupSession *HTTP;

#define SUB_UA "Wellide/" WL_VERSION " (HiddifyNext compatible; sing-box)"

static void profile_free(Profile *p)
{
    g_free(p->id); g_free(p->name); g_free(p->url);
    if (p->servers) g_ptr_array_unref(p->servers);
    g_free(p);
}

static char *profiles_dir(void)
{
    g_autofree char *d = wl_config_dir();
    char *pd = g_build_filename(d, "profiles", NULL);
    g_mkdir_with_parents(pd, 0700);
    return pd;
}

static char *body_path(const char *id)
{
    g_autofree char *d = profiles_dir();
    g_autofree char *f = g_strdup_printf("%s.txt", id);
    return g_build_filename(d, f, NULL);
}

/* give every server a unique, stable outbound tag based on its name */
static void assign_tags(GPtrArray *servers)
{
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(seen, g_strdup("auto"));
    g_hash_table_add(seen, g_strdup("select"));
    g_hash_table_add(seen, g_strdup("direct"));
    for (guint i = 0; i < servers->len; i++) {
        Server *s = servers->pdata[i];
        char *tag = g_strdup(s->name);
        for (int n = 2; g_hash_table_contains(seen, tag); n++) {
            g_free(tag);
            tag = g_strdup_printf("%s #%d", s->name, n);
        }
        g_hash_table_add(seen, g_strdup(tag));
        g_free(s->tag);
        s->tag = tag;
        if (s->ob) json_object_set_string_member(s->ob, "tag", tag);
    }
}

static void profile_set_body(Profile *p, const char *body)
{
    if (p->servers) g_ptr_array_unref(p->servers);
    p->servers = links_parse(body);
    assign_tags(p->servers);
}

int profile_server_count(Profile *p)
{
    int n = 0;
    for (guint i = 0; p && p->servers && i < p->servers->len; i++)
        if (!((Server *)p->servers->pdata[i])->separator) n++;
    return n;
}

Profile *profile_by_id(const char *id)
{
    for (guint i = 0; id && i < PROFILES->len; i++) {
        Profile *p = PROFILES->pdata[i];
        if (!strcmp(p->id, id)) return p;
    }
    return NULL;
}

Profile *profile_active(void)
{
    Profile *p = profile_by_id(S.active);
    if (!p && PROFILES->len) p = PROFILES->pdata[0];
    return p;
}

Server *profile_find_server(Profile *p, const char *tag)
{
    for (guint i = 0; p && tag && i < p->servers->len; i++) {
        Server *s = p->servers->pdata[i];
        if (s->tag && !strcmp(s->tag, tag)) return s;
    }
    return NULL;
}

void profiles_save(void)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_array(b);
    for (guint i = 0; i < PROFILES->len; i++) {
        Profile *p = PROFILES->pdata[i];
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "id");       json_builder_add_string_value(b, p->id);
        json_builder_set_member_name(b, "name");     json_builder_add_string_value(b, p->name);
        if (p->url) { json_builder_set_member_name(b, "url"); json_builder_add_string_value(b, p->url); }
        json_builder_set_member_name(b, "upload");   json_builder_add_int_value(b, p->upload);
        json_builder_set_member_name(b, "download"); json_builder_add_int_value(b, p->download);
        json_builder_set_member_name(b, "total");    json_builder_add_int_value(b, p->total);
        json_builder_set_member_name(b, "expire");   json_builder_add_int_value(b, p->expire);
        json_builder_set_member_name(b, "updated");  json_builder_add_int_value(b, p->updated);
        json_builder_set_member_name(b, "interval"); json_builder_add_int_value(b, p->interval_h);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    g_autoptr(JsonGenerator) g = json_generator_new();
    json_generator_set_pretty(g, TRUE);
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    g_autofree char *d = wl_config_dir();
    g_autofree char *f = g_build_filename(d, "profiles.json", NULL);
    json_generator_to_file(g, f, NULL);
    g_chmod(f, 0600);
}

void profiles_load(void)
{
    PROFILES = g_ptr_array_new_with_free_func((GDestroyNotify)profile_free);
    g_autofree char *d = wl_config_dir();
    g_autofree char *f = g_build_filename(d, "profiles.json", NULL);
    g_autoptr(JsonParser) jp = json_parser_new();
    if (!json_parser_load_from_file(jp, f, NULL)) return;
    JsonNode *root = json_parser_get_root(jp);
    if (!JSON_NODE_HOLDS_ARRAY(root)) return;
    JsonArray *a = json_node_get_array(root);
    for (guint i = 0; i < json_array_get_length(a); i++) {
        JsonObject *o = json_array_get_object_element(a, i);
        Profile *p = g_new0(Profile, 1);
        p->id = g_strdup(json_object_get_string_member_with_default(o, "id", ""));
        p->name = g_strdup(json_object_get_string_member_with_default(o, "name", "Профиль"));
        const char *url = json_object_get_string_member_with_default(o, "url", NULL);
        p->url = url ? g_strdup(url) : NULL;
        p->upload = json_object_get_int_member_with_default(o, "upload", 0);
        p->download = json_object_get_int_member_with_default(o, "download", 0);
        p->total = json_object_get_int_member_with_default(o, "total", 0);
        p->expire = json_object_get_int_member_with_default(o, "expire", 0);
        p->updated = json_object_get_int_member_with_default(o, "updated", 0);
        p->interval_h = json_object_get_int_member_with_default(o, "interval", 12);
        g_autofree char *bp = body_path(p->id);
        g_autofree char *body = NULL;
        g_file_get_contents(bp, &body, NULL, NULL);
        profile_set_body(p, body);
        if (!*p->id) { profile_free(p); continue; }
        g_ptr_array_add(PROFILES, p);
    }
}

void profile_delete(Profile *p)
{
    g_autofree char *bp = body_path(p->id);
    g_unlink(bp);
    if (S.active && !strcmp(S.active, p->id)) {
        g_free(S.active);
        S.active = g_strdup("");
    }
    g_ptr_array_remove(PROFILES, p);
    profiles_save();
    settings_save();
}

/* ---------- fetching ---------- */

static void parse_userinfo(Profile *p, const char *h)
{
    if (!h) return;
    char **kv = g_strsplit(h, ";", -1);
    for (int i = 0; kv[i]; i++) {
        char *e = strchr(kv[i], '=');
        if (!e) continue;
        *e = 0;
        g_strstrip(kv[i]);
        gint64 v = g_ascii_strtoll(e + 1, NULL, 10);
        if (!strcmp(kv[i], "upload")) p->upload = v;
        else if (!strcmp(kv[i], "download")) p->download = v;
        else if (!strcmp(kv[i], "total")) p->total = v;
        else if (!strcmp(kv[i], "expire")) p->expire = v;
    }
    g_strfreev(kv);
}

static char *header_title(SoupMessageHeaders *h)
{
    const char *t = soup_message_headers_get_one(h, "profile-title");
    if (t) {
        if (g_str_has_prefix(t, "base64:")) {
            gsize n = 0;
            guchar *raw = g_base64_decode(t + 7, &n);
            char *r = g_strndup((char *)raw, n);
            g_free(raw);
            if (g_utf8_validate(r, -1, NULL)) return r;
            g_free(r);
        } else return g_strdup(t);
    }
    const char *cd = soup_message_headers_get_one(h, "content-disposition");
    const char *fn = cd ? strstr(cd, "filename=") : NULL;
    if (fn) {
        char *r = g_strdup(fn + 9);
        g_strdelimit(r, "\"", ' ');
        return g_strstrip(r);
    }
    return NULL;
}

typedef struct {
    Profile *p;       /* existing profile when updating */
    gboolean is_new;
    ProfileDoneCb cb;
    gpointer ud;
    SoupMessage *msg;
} Fetch;

static void finish_profile(Fetch *f, const char *body, SoupMessageHeaders *h)
{
    Profile *p = f->p;
    g_autoptr(GPtrArray) test = links_parse(body);
    if (test->len == 0) {
        f->cb(NULL, "в подписке не нашлось поддерживаемых серверов", f->ud);
        if (f->is_new) profile_free(p);
        return;
    }
    if (h) {
        parse_userinfo(p, soup_message_headers_get_one(h, "subscription-userinfo"));
        const char *iv = soup_message_headers_get_one(h, "profile-update-interval");
        if (iv && atoi(iv) > 0) p->interval_h = atoi(iv);
        g_autofree char *title = header_title(h);
        if (title && f->is_new) { g_free(p->name); p->name = g_steal_pointer(&title); }
    }
    p->updated = g_get_real_time() / G_USEC_PER_SEC;
    g_autofree char *bp = body_path(p->id);
    g_file_set_contents_full(bp, body, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
    profile_set_body(p, body);
    if (f->is_new) {
        g_ptr_array_add(PROFILES, p);
        if (!S.active || !*S.active || !profile_by_id(S.active)) {
            g_free(S.active);
            S.active = g_strdup(p->id);
            settings_save();
        }
    }
    profiles_save();
    f->cb(p->id, NULL, f->ud);
}

static void on_fetched(GObject *src, GAsyncResult *res, gpointer data)
{
    Fetch *f = data;
    GError *err = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    guint status = soup_message_get_status(f->msg);
    if (err || status != 200) {
        g_autofree char *e = err ? g_strdup(err->message) : g_strdup_printf("HTTP %u", status);
        f->cb(NULL, e, f->ud);
        if (f->is_new) profile_free(f->p);
    } else {
        gsize n = 0;
        const char *d = g_bytes_get_data(bytes, &n);
        g_autofree char *body = g_strndup(d, n);
        finish_profile(f, body, soup_message_get_response_headers(f->msg));
    }
    if (bytes) g_bytes_unref(bytes);
    g_clear_error(&err);
    g_object_unref(f->msg);
    g_free(f);
}

static void fetch(Fetch *f)
{
    f->msg = soup_message_new("GET", f->p->url);
    if (!f->msg) {
        f->cb(NULL, "неверная ссылка", f->ud);
        if (f->is_new) profile_free(f->p);
        g_free(f);
        return;
    }
    soup_message_headers_replace(soup_message_get_request_headers(f->msg), "User-Agent", SUB_UA);
    soup_session_send_and_read_async(HTTP, f->msg, G_PRIORITY_DEFAULT, NULL, on_fetched, f);
}

void profile_add_async(const char *input, ProfileDoneCb cb, gpointer ud)
{
    g_autofree char *in = g_strstrip(g_strdup(input));
    Fetch *f = g_new0(Fetch, 1);
    f->is_new = TRUE;
    f->cb = cb;
    f->ud = ud;
    Profile *p = f->p = g_new0(Profile, 1);
    p->id = g_uuid_string_random();
    p->interval_h = 12;

    gboolean is_url = (g_str_has_prefix(in, "http://") || g_str_has_prefix(in, "https://"))
                      && !strpbrk(in, " \n");
    if (is_url) {
        /* hiddify-style links carry the name in the fragment */
        char *hash = strchr(in, '#');
        if (hash) {
            p->name = g_uri_unescape_string(hash + 1, NULL);
            *hash = 0;
        }
        if (!p->name || !*p->name) {
            g_free(p->name);
            g_autoptr(GUri) u = g_uri_parse(in, G_URI_FLAGS_NONE, NULL);
            p->name = g_strdup(u && g_uri_get_host(u) ? g_uri_get_host(u) : "Подписка");
        }
        p->url = g_strdup(in);
        fetch(f);
    } else {
        p->name = g_strdup("Мои ссылки");
        finish_profile(f, in, NULL);
        g_free(f);
    }
}

void profile_update_async(Profile *p, ProfileDoneCb cb, gpointer ud)
{
    if (!p->url) { cb(p->id, NULL, ud); return; }
    Fetch *f = g_new0(Fetch, 1);
    f->p = p;
    f->cb = cb;
    f->ud = ud;
    fetch(f);
}

/* ---------- RU rule sets (for "bypass RU") ---------- */

static const char *RULE_URLS[][2] = {
    { "geoip-ru.srs",   "https://raw.githubusercontent.com/SagerNet/sing-geoip/rule-set/geoip-ru.srs" },
    { "geosite-ru.srs", "https://raw.githubusercontent.com/SagerNet/sing-geosite/rule-set/geosite-category-ru.srs" },
};

char *rules_path(const char *name)
{
    g_autofree char *d = wl_cache_dir();
    return g_build_filename(d, name, NULL);
}

static void on_rule(GObject *src, GAsyncResult *res, gpointer data)
{
    char *name = data;
    GError *err = NULL;
    GBytes *b = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    gsize n = 0;
    const char *d = b ? g_bytes_get_data(b, &n) : NULL;
    /* .srs files start with the "SRS" magic */
    if (!err && n > 3 && !memcmp(d, "SRS", 3)) {
        g_autofree char *p = rules_path(name);
        g_file_set_contents(p, d, n, NULL);
        S.rules_updated = g_get_real_time() / G_USEC_PER_SEC;
        settings_save();
    }
    if (b) g_bytes_unref(b);
    g_clear_error(&err);
    g_free(name);
}

void rules_update_async(gboolean force)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    for (guint i = 0; i < G_N_ELEMENTS(RULE_URLS); i++) {
        g_autofree char *p = rules_path(RULE_URLS[i][0]);
        if (!force && g_file_test(p, G_FILE_TEST_EXISTS) && now - S.rules_updated < 7 * 86400)
            continue;
        SoupMessage *m = soup_message_new("GET", RULE_URLS[i][1]);
        soup_session_send_and_read_async(HTTP, m, G_PRIORITY_LOW, NULL, on_rule, g_strdup(RULE_URLS[i][0]));
        g_object_unref(m);
    }
}

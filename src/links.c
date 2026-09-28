/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Share-link parser: vless / vmess / trojan / ss / hysteria2 / tuic
 * -> sing-box outbound objects. Also accepts base64-wrapped lists and
 * raw sing-box JSON ({"outbounds":[...]}) subscriptions. */
#include "wellide.h"
#include <string.h>

void server_free(Server *s)
{
    if (!s) return;
    g_free(s->tag); g_free(s->name); g_free(s->proto); g_free(s->server); g_free(s->cc);
    if (s->ob) json_object_unref(s->ob);
    g_free(s);
}

/* ---- small helpers ---- */

static char *unesc(const char *s)
{
    if (!s) return NULL;
    char *r = g_uri_unescape_string(s, NULL);
    return r ? r : g_strdup(s);
}

static GHashTable *parse_query(const char *q)
{
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!q) return h;
    char **parts = g_strsplit(q, "&", -1);
    for (int i = 0; parts[i]; i++) {
        if (!*parts[i]) continue;
        char *eq = strchr(parts[i], '=');
        char *k, *v;
        if (eq) { *eq = 0; k = unesc(parts[i]); v = unesc(eq + 1); }
        else    { k = unesc(parts[i]); v = g_strdup(""); }
        g_hash_table_replace(h, k, v);
    }
    g_strfreev(parts);
    return h;
}

static const char *q_get(GHashTable *q, const char *k)
{
    const char *v = g_hash_table_lookup(q, k);
    return (v && *v) ? v : NULL;
}

static gboolean q_true(GHashTable *q, const char *k)
{
    const char *v = q_get(q, k);
    return v && (!strcmp(v, "1") || !g_ascii_strcasecmp(v, "true"));
}

static char *b64_decode_loose(const char *in)
{
    GString *s = g_string_new(NULL);
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        if (g_ascii_isalnum(c) || c == '+' || c == '/') g_string_append_c(s, c);
    }
    while (s->len % 4) g_string_append_c(s, '=');
    gsize n = 0;
    guchar *raw = g_base64_decode(s->str, &n);
    g_string_free(s, TRUE);
    if (!raw) return NULL;
    char *r = g_strndup((char *)raw, n);
    g_free(raw);
    if (!g_utf8_validate(r, -1, NULL)) { g_free(r); return NULL; }
    return r;
}

/* host:port with optional [ipv6] */
static gboolean split_hostport(const char *hp, char **host, int *port)
{
    const char *colon;
    if (hp[0] == '[') {
        const char *rb = strchr(hp, ']');
        if (!rb) return FALSE;
        *host = g_strndup(hp + 1, rb - hp - 1);
        colon = (rb[1] == ':') ? rb + 1 : NULL;
    } else {
        colon = strrchr(hp, ':');
        *host = colon ? g_strndup(hp, colon - hp) : g_strdup(hp);
    }
    *port = colon ? atoi(colon + 1) : 443;
    return **host && *port > 0 && *port < 65536;
}

typedef struct {
    char *scheme, *user, *host, *query, *name;
    int port;
} Url;

static void url_clear(Url *u)
{
    g_free(u->scheme); g_free(u->user); g_free(u->host);
    g_free(u->query); g_free(u->name);
}

/* scheme://user@host:port?query#name — user may be empty */
static gboolean url_split(const char *link, Url *u)
{
    memset(u, 0, sizeof *u);
    const char *sep = strstr(link, "://");
    if (!sep) return FALSE;
    u->scheme = g_ascii_strdown(link, sep - link);
    g_autofree char *rest = g_strdup(sep + 3);

    char *hash = strchr(rest, '#');
    if (hash) { *hash = 0; u->name = unesc(hash + 1); }
    char *qm = strchr(rest, '?');
    if (qm) { *qm = 0; u->query = g_strdup(qm + 1); }
    char *slash = strchr(rest, '/');
    if (slash) *slash = 0;
    char *at = strrchr(rest, '@');
    const char *hp = rest;
    if (at) { *at = 0; u->user = unesc(rest); hp = at + 1; }
    return split_hostport(hp, &u->host, &u->port);
}

/* ---- sing-box object builders ---- */

static JsonObject *jobj(void) { return json_object_new(); }

static JsonArray *csv_array(const char *csv)
{
    JsonArray *a = json_array_new();
    char **p = g_strsplit(csv, ",", -1);
    for (int i = 0; p[i]; i++) {
        g_strstrip(p[i]);
        if (*p[i]) json_array_add_string_element(a, p[i]);
    }
    g_strfreev(p);
    return a;
}

static JsonObject *make_tls(GHashTable *q, const char *host, gboolean force)
{
    const char *sec = q_get(q, "security");
    gboolean reality = sec && !strcmp(sec, "reality");
    if (!force && !reality && !(sec && !strcmp(sec, "tls"))) return NULL;

    JsonObject *t = jobj();
    json_object_set_boolean_member(t, "enabled", TRUE);
    const char *sni = q_get(q, "sni");
    if (!sni) sni = q_get(q, "peer");
    if (!sni) sni = q_get(q, "host");
    json_object_set_string_member(t, "server_name", sni ? sni : host);
    if (q_true(q, "allowInsecure") || q_true(q, "insecure") || q_true(q, "allow_insecure"))
        json_object_set_boolean_member(t, "insecure", TRUE);
    const char *alpn = q_get(q, "alpn");
    if (alpn) json_object_set_array_member(t, "alpn", csv_array(alpn));
    const char *cs = q_get(q, "cs");
    if (cs) {
        g_autofree char *c = g_strdelimit(g_strdup(cs), ":", ',');
        json_object_set_array_member(t, "cipher_suites", csv_array(c));
    }
    const char *fp = q_get(q, "fp");
    if (fp || reality) {
        JsonObject *u = jobj();
        json_object_set_boolean_member(u, "enabled", TRUE);
        json_object_set_string_member(u, "fingerprint", fp ? fp : "chrome");
        json_object_set_object_member(t, "utls", u);
    }
    if (reality) {
        JsonObject *r = jobj();
        json_object_set_boolean_member(r, "enabled", TRUE);
        const char *pbk = q_get(q, "pbk");
        const char *sid = q_get(q, "sid");
        json_object_set_string_member(r, "public_key", pbk ? pbk : "");
        json_object_set_string_member(r, "short_id", sid ? sid : "");
        json_object_set_object_member(t, "reality", r);
    }
    return t;
}

/* returns FALSE for transports sing-box can't do (xhttp, kcp, quic-v2ray) */
static gboolean add_transport(JsonObject *ob, GHashTable *q, const char **label)
{
    const char *type = q_get(q, "type");
    if (!type) type = q_get(q, "net");
    if (!type || !strcmp(type, "tcp") || !strcmp(type, "raw")) {
        const char *ht = q_get(q, "headerType");
        if (ht && !strcmp(ht, "http")) {
            JsonObject *t = jobj();
            json_object_set_string_member(t, "type", "http");
            const char *h = q_get(q, "host");
            if (h) json_object_set_array_member(t, "host", csv_array(h));
            const char *p = q_get(q, "path");
            if (p) json_object_set_string_member(t, "path", p);
            json_object_set_object_member(ob, "transport", t);
            *label = "tcp/http";
        } else *label = "tcp";
        return TRUE;
    }
    JsonObject *t = jobj();
    const char *host = q_get(q, "host");
    const char *path = q_get(q, "path");
    if (!strcmp(type, "ws")) {
        json_object_set_string_member(t, "type", "ws");
        g_autofree char *p = g_strdup(path ? path : "/");
        char *ed = strstr(p, "?ed=");
        if (ed) {
            json_object_set_int_member(t, "max_early_data", atoi(ed + 4));
            json_object_set_string_member(t, "early_data_header_name", "Sec-WebSocket-Protocol");
            *ed = 0;
        }
        json_object_set_string_member(t, "path", p);
        if (host) {
            JsonObject *h = jobj();
            json_object_set_string_member(h, "Host", host);
            json_object_set_object_member(t, "headers", h);
        }
        *label = "ws";
    } else if (!strcmp(type, "grpc")) {
        json_object_set_string_member(t, "type", "grpc");
        const char *sn = q_get(q, "serviceName");
        json_object_set_string_member(t, "service_name", sn ? sn : (path ? path : ""));
        *label = "grpc";
    } else if (!strcmp(type, "httpupgrade")) {
        json_object_set_string_member(t, "type", "httpupgrade");
        if (host) json_object_set_string_member(t, "host", host);
        json_object_set_string_member(t, "path", path ? path : "/");
        *label = "httpupgrade";
    } else if (!strcmp(type, "http") || !strcmp(type, "h2")) {
        json_object_set_string_member(t, "type", "http");
        if (host) json_object_set_array_member(t, "host", csv_array(host));
        if (path) json_object_set_string_member(t, "path", path);
        *label = "h2";
    } else {
        json_object_unref(t);
        return FALSE;
    }
    json_object_set_object_member(ob, "transport", t);
    return TRUE;
}

static Server *new_server(const char *name, const char *scheme, const char *tr,
                          const char *host, int port, JsonObject *ob)
{
    Server *s = g_new0(Server, 1);
    s->name = g_strdup(name && *name ? name : host);
    g_strstrip(s->name);
    s->proto = tr ? g_strdup_printf("%s · %s", scheme, tr) : g_strdup(scheme);
    s->server = g_strdup(host);
    s->port = port;
    s->cc = cc_from_flag(s->name);
    s->ob = ob;
    s->delay = -1;
    /* providers put "0.0.0.0" entries as section headers */
    if (!strcmp(host, "0.0.0.0") || !strcmp(host, "127.0.0.1")) {
        s->separator = TRUE;
        if (s->ob) { json_object_unref(s->ob); s->ob = NULL; }
    }
    if (s->ob) {
        json_object_set_string_member(s->ob, "server", host);
        json_object_set_int_member(s->ob, "server_port", port);
    }
    return s;
}

/* ---- per-protocol ---- */

static Server *parse_vless_trojan(const char *link, gboolean trojan)
{
    Url u;
    if (!url_split(link, &u) || !u.user) { url_clear(&u); return NULL; }
    g_autoptr(GHashTable) q = parse_query(u.query);
    JsonObject *ob = jobj();
    const char *tr = NULL;
    if (trojan) {
        json_object_set_string_member(ob, "type", "trojan");
        json_object_set_string_member(ob, "password", u.user);
    } else {
        json_object_set_string_member(ob, "type", "vless");
        json_object_set_string_member(ob, "uuid", u.user);
        const char *flow = q_get(q, "flow");
        if (flow) json_object_set_string_member(ob, "flow", flow);
        json_object_set_string_member(ob, "packet_encoding", "xudp");
    }
    if (!add_transport(ob, q, &tr)) {
        json_object_unref(ob); url_clear(&u); return NULL;
    }
    JsonObject *tls = make_tls(q, u.host, trojan && !q_get(q, "security"));
    if (tls) json_object_set_object_member(ob, "tls", tls);
    g_autofree char *label = g_strdup_printf("%s%s", tr,
        tls ? (json_object_has_member(tls, "reality") ? " · reality" : " · tls") : "");
    Server *s = new_server(u.name, trojan ? "trojan" : "vless", label, u.host, u.port, ob);
    url_clear(&u);
    return s;
}

static Server *parse_vmess(const char *link)
{
    g_autofree char *js = b64_decode_loose(link + 8);
    if (!js) return NULL;
    g_autoptr(JsonParser) p = json_parser_new();
    if (!json_parser_load_from_data(p, js, -1, NULL)) return NULL;
    JsonNode *root = json_parser_get_root(p);
    if (!JSON_NODE_HOLDS_OBJECT(root)) return NULL;
    JsonObject *v = json_node_get_object(root);

    /* vmess json fields may be numbers or strings */
    #define VS(k) (json_object_has_member(v, k) && JSON_NODE_HOLDS_VALUE(json_object_get_member(v, k)) \
                   ? json_node_dup_string(json_object_get_member(v, k)) : NULL)
    g_autofree char *add = VS("add"), *id = VS("id"), *ps = VS("ps"), *net = VS("net"),
                    *host = VS("host"), *path = VS("path"), *tls = VS("tls"), *sni = VS("sni"),
                    *scy = VS("scy"), *alpn = VS("alpn"), *fp = VS("fp"), *type = VS("type");
    #undef VS
    int port = 0, aid = 0;
    if (json_object_has_member(v, "port")) {
        JsonNode *n = json_object_get_member(v, "port");
        port = json_node_get_value_type(n) == G_TYPE_STRING ? atoi(json_node_get_string(n)) : (int)json_node_get_int(n);
    }
    if (json_object_has_member(v, "aid")) {
        JsonNode *n = json_object_get_member(v, "aid");
        aid = json_node_get_value_type(n) == G_TYPE_STRING ? atoi(json_node_get_string(n)) : (int)json_node_get_int(n);
    }
    if (!add || !id || port <= 0) return NULL;

    g_autoptr(GHashTable) q = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    g_hash_table_insert(q, "type", g_strdup(net ? net : "tcp"));
    if (host) g_hash_table_insert(q, "host", g_strdup(host));
    if (path) g_hash_table_insert(q, "path", g_strdup(path));
    if (type) g_hash_table_insert(q, "headerType", g_strdup(type));
    if (net && !strcmp(net, "grpc") && path) g_hash_table_insert(q, "serviceName", g_strdup(path));
    if (tls && !strcmp(tls, "tls")) g_hash_table_insert(q, "security", g_strdup("tls"));
    if (sni) g_hash_table_insert(q, "sni", g_strdup(sni));
    if (alpn) g_hash_table_insert(q, "alpn", g_strdup(alpn));
    if (fp) g_hash_table_insert(q, "fp", g_strdup(fp));

    JsonObject *ob = jobj();
    json_object_set_string_member(ob, "type", "vmess");
    json_object_set_string_member(ob, "uuid", id);
    json_object_set_string_member(ob, "security", scy && *scy ? scy : "auto");
    json_object_set_int_member(ob, "alter_id", aid);
    json_object_set_string_member(ob, "packet_encoding", "xudp");
    const char *tr = NULL;
    if (!add_transport(ob, q, &tr)) { json_object_unref(ob); return NULL; }
    JsonObject *t = make_tls(q, add, FALSE);
    if (t) json_object_set_object_member(ob, "tls", t);
    g_autofree char *label = g_strdup_printf("%s%s", tr, t ? " · tls" : "");
    return new_server(ps, "vmess", label, add, port, ob);
}

static Server *parse_ss(const char *link)
{
    Url u;
    g_autofree char *work = NULL;
    /* legacy form: ss://base64(method:pass@host:port)#name */
    if (!strchr(link + 5, '@')) {
        g_autofree char *body = g_strdup(link + 5);
        char *h = strchr(body, '#');
        g_autofree char *frag = h ? g_strdup(h) : g_strdup("");
        if (h) *h = 0;
        g_autofree char *dec = b64_decode_loose(body);
        if (!dec) return NULL;
        work = g_strdup_printf("ss://%s%s", dec, frag);
        link = work;
    }
    if (!url_split(link, &u) || !u.user) { url_clear(&u); return NULL; }
    char *method = NULL, *pass = NULL;
    char *colon = strchr(u.user, ':');
    if (colon) {
        method = g_strndup(u.user, colon - u.user);
        pass = g_strdup(colon + 1);
    } else {
        g_autofree char *dec = b64_decode_loose(u.user);
        char *c2 = dec ? strchr(dec, ':') : NULL;
        if (c2) { method = g_strndup(dec, c2 - dec); pass = g_strdup(c2 + 1); }
    }
    if (!method) { url_clear(&u); g_free(pass); return NULL; }
    JsonObject *ob = jobj();
    json_object_set_string_member(ob, "type", "shadowsocks");
    json_object_set_string_member(ob, "method", method);
    json_object_set_string_member(ob, "password", pass);
    Server *s = new_server(u.name, "ss", method, u.host, u.port, ob);
    g_free(method); g_free(pass); url_clear(&u);
    return s;
}

static Server *parse_hy2(const char *link)
{
    Url u;
    if (!url_split(link, &u)) { url_clear(&u); return NULL; }
    g_autoptr(GHashTable) q = parse_query(u.query);
    JsonObject *ob = jobj();
    json_object_set_string_member(ob, "type", "hysteria2");
    json_object_set_string_member(ob, "password", u.user ? u.user : "");
    const char *obfs = q_get(q, "obfs");
    if (obfs) {
        JsonObject *o = jobj();
        json_object_set_string_member(o, "type", obfs);
        const char *op = q_get(q, "obfs-password");
        json_object_set_string_member(o, "password", op ? op : "");
        json_object_set_object_member(ob, "obfs", o);
    }
    JsonObject *tls = make_tls(q, u.host, TRUE);
    json_object_remove_member(tls, "utls");
    if (!json_object_has_member(tls, "alpn")) {
        JsonArray *a = json_array_new();
        json_array_add_string_element(a, "h3");
        json_object_set_array_member(tls, "alpn", a);
    }
    json_object_set_object_member(ob, "tls", tls);
    Server *s = new_server(u.name, "hysteria2", NULL, u.host, u.port, ob);
    url_clear(&u);
    return s;
}

static Server *parse_tuic(const char *link)
{
    Url u;
    if (!url_split(link, &u) || !u.user) { url_clear(&u); return NULL; }
    g_autoptr(GHashTable) q = parse_query(u.query);
    char *colon = strchr(u.user, ':');
    JsonObject *ob = jobj();
    json_object_set_string_member(ob, "type", "tuic");
    if (colon) {
        *colon = 0;
        json_object_set_string_member(ob, "password", colon + 1);
    }
    json_object_set_string_member(ob, "uuid", u.user);
    const char *cc = q_get(q, "congestion_control");
    json_object_set_string_member(ob, "congestion_control", cc ? cc : "bbr");
    const char *urm = q_get(q, "udp_relay_mode");
    if (urm) json_object_set_string_member(ob, "udp_relay_mode", urm);
    JsonObject *tls = make_tls(q, u.host, TRUE);
    json_object_remove_member(tls, "utls");
    if (!json_object_has_member(tls, "alpn")) {
        JsonArray *a = json_array_new();
        json_array_add_string_element(a, "h3");
        json_object_set_array_member(tls, "alpn", a);
    }
    json_object_set_object_member(ob, "tls", tls);
    Server *s = new_server(u.name, "tuic", NULL, u.host, u.port, ob);
    url_clear(&u);
    return s;
}

static Server *parse_line(const char *l)
{
    if (g_str_has_prefix(l, "vless://"))  return parse_vless_trojan(l, FALSE);
    if (g_str_has_prefix(l, "trojan://")) return parse_vless_trojan(l, TRUE);
    if (g_str_has_prefix(l, "vmess://"))  return parse_vmess(l);
    if (g_str_has_prefix(l, "ss://"))     return parse_ss(l);
    if (g_str_has_prefix(l, "hysteria2://") || g_str_has_prefix(l, "hy2://")) return parse_hy2(l);
    if (g_str_has_prefix(l, "tuic://"))   return parse_tuic(l);
    return NULL;
}

/* sing-box JSON subscription: keep proxy-type outbounds as-is */
static void parse_singbox_json(JsonObject *root, GPtrArray *out)
{
    if (!json_object_has_member(root, "outbounds")) return;
    JsonArray *arr = json_object_get_array_member(root, "outbounds");
    static const char *ok[] = { "vless", "vmess", "trojan", "shadowsocks", "hysteria2",
                                "hysteria", "tuic", "wireguard", "socks", "http",
                                "anytls", "shadowtls", "ssh", NULL };
    for (guint i = 0; i < json_array_get_length(arr); i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        const char *type = json_object_get_string_member_with_default(o, "type", "");
        if (!g_strv_contains(ok, type)) continue;
        const char *srv = json_object_get_string_member_with_default(o, "server", "");
        int port = json_object_get_int_member_with_default(o, "server_port", 0);
        const char *tag = json_object_get_string_member_with_default(o, "tag", srv);
        JsonNode *copy = json_node_copy(json_array_get_element(arr, i));
        JsonObject *ob = json_object_ref(json_node_get_object(copy));
        json_node_unref(copy);
        g_ptr_array_add(out, new_server(tag, type, NULL, srv, port, ob));
    }
}

GPtrArray *links_parse(const char *text)
{
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)server_free);
    if (!text) return out;
    g_autofree char *t = g_strstrip(g_strdup(text));

    if (t[0] == '{') {
        g_autoptr(JsonParser) p = json_parser_new();
        if (json_parser_load_from_data(p, t, -1, NULL) &&
            JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p)))
            parse_singbox_json(json_node_get_object(json_parser_get_root(p)), out);
        return out;
    }

    g_autofree char *decoded = strstr(t, "://") ? NULL : b64_decode_loose(t);
    const char *src = decoded ? decoded : t;

    char **lines = g_strsplit_set(src, "\r\n ", -1);
    for (int i = 0; lines[i]; i++) {
        g_strstrip(lines[i]);
        if (!*lines[i]) continue;
        Server *s = parse_line(lines[i]);
        if (s) g_ptr_array_add(out, s);
    }
    g_strfreev(lines);
    return out;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sing-box process management + Clash API client. */
#include "wellide.h"
#include <gio/gio.h>
#include <string.h>
#ifndef G_OS_WIN32
#include <signal.h>
#include <sys/xattr.h>
#endif

#define TEST_URL "https://www.gstatic.com/generate_204"

static GSubprocess *proc;
static CoreState state = ST_OFF;
static SoupSession *api;          /* local API, never proxied */
static GCancellable *traffic_cancel;
static guint ready_poll;
static int ready_tries;
static GString *last_err;          /* tail of core output, for error reports */
static gboolean restart_pending;
static gint64 tot_up, tot_down;

CoreState core_state(void) { return state; }

static void set_state(CoreState st, const char *err)
{
    state = st;
    ui_on_state(st, err);
}

static SoupSession *api_session(void)
{
    if (!api) api = soup_session_new_with_options("proxy-resolver", NULL, "timeout", 12, NULL);
    return api;
}

static char *api_url(const char *path)
{
    return g_strdup_printf("http://127.0.0.1:%d%s", S.api_port, path);
}

/* ---------- core binary / TUN permissions ----------
 * Linux: TUN needs CAP_NET_ADMIN. We keep a private copy of sing-box with
 * that capability (installed by install.sh or the settings button), so
 * Wellide itself never runs as root.
 * Windows: the installer ships sing-box.exe + wintun.dll next to
 * wellide.exe and the app runs elevated (manifest), so TUN just works. */

#ifdef G_OS_WIN32
static char *exe_dir(void)
{
    g_autofree char *d = g_win32_get_package_installation_directory_of_module(NULL);
    return g_build_filename(d, "bin", NULL);
}
#endif

const char *core_bin(void)
{
    static char *bin;
    if (bin) return bin;
    const char *env = g_getenv("WELLIDE_SINGBOX");
    if (env && *env) return bin = g_strdup(env);
#ifdef G_OS_WIN32
    g_autofree char *d = exe_dir();
    bin = g_build_filename(d, "sing-box.exe", NULL);
    if (!g_file_test(bin, G_FILE_TEST_EXISTS)) { g_free(bin); bin = g_strdup("sing-box.exe"); }
#else
    bin = g_find_program_in_path("sing-box");
    if (!bin && g_file_test(WL_BUNDLED_CORE, G_FILE_TEST_IS_EXECUTABLE)) bin = g_strdup(WL_BUNDLED_CORE);
    if (!bin && g_file_test(WL_TUN_BIN, G_FILE_TEST_IS_EXECUTABLE)) bin = g_strdup(WL_TUN_BIN);
    if (!bin) bin = g_strdup("sing-box");
#endif
    return bin;
}

gboolean tun_ready(void)
{
#ifdef G_OS_WIN32
    return TRUE;
#else
    if (!g_file_test(WL_TUN_BIN, G_FILE_TEST_IS_EXECUTABLE)) return FALSE;
    return getxattr(WL_TUN_BIN, "security.capability", NULL, 0) > 0;
#endif
}

#ifndef G_OS_WIN32
static void on_setup_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError *e = NULL;
    gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(src), res, &e);
    ok = ok && tun_ready();
    ui_on_tun_setup(ok, ok ? N_("TUN готов", "TUN is ready")
                           : (e ? e->message : N_("не удалось выдать права", "could not grant permissions")));
    g_clear_error(&e);
    g_object_unref(src);
}
#endif

#ifndef G_OS_WIN32
/* polkit checks the groups of the *running* session, so a group freshly
 * added by setup would only work after logging in again. Each user who
 * runs setup therefore also gets a per-user rule that works immediately. */
static const char POLKIT_GROUP_RULE[] =
    "// Wellide TUN: sing-box sets DNS on its tunnel via systemd-resolved\n"
    "polkit.addRule(function(action, subject) {\n"
    "    if ((action.id.indexOf(\"org.freedesktop.resolve1.set-\") == 0 ||\n"
    "         action.id == \"org.freedesktop.resolve1.revert\") &&\n"
    "        subject.local && subject.active && subject.isInGroup(\"wellide\"))\n"
    "        return polkit.Result.YES;\n"
    "});";

static char *polkit_user_rule(const char *user)
{
    return g_strdup_printf(
        "// Wellide TUN for %s (works before re-login; see 49-wellide.rules)\n"
        "polkit.addRule(function(action, subject) {\n"
        "    if ((action.id.indexOf(\"org.freedesktop.resolve1.set-\") == 0 ||\n"
        "         action.id == \"org.freedesktop.resolve1.revert\") &&\n"
        "        subject.local && subject.active && subject.user == \"%s\")\n"
        "        return polkit.Result.YES;\n"
        "});", user, user);
}

#endif

void tun_setup_async(void)
{
#ifdef G_OS_WIN32
    ui_on_tun_setup(TRUE, N_("TUN готов", "TUN is ready"));
#else
    g_autofree char *sb = g_find_program_in_path("sing-box");
    if (!sb && g_file_test(WL_BUNDLED_CORE, G_FILE_TEST_IS_EXECUTABLE)) sb = g_strdup(WL_BUNDLED_CORE);
    if (!sb) { ui_on_tun_setup(FALSE, N_("sing-box не найден в PATH", "sing-box not found in PATH")); return; }
    /* The capable copy is group-restricted (0750, the user's own group).
     * The polkit rule lets members of "wellide" set DNS on the TUN link
     * via systemd-resolved; polkit resolves groups from the user database,
     * so it works without logging out. */
    const char *user = g_get_user_name();
    /* user names are [a-z_][a-z0-9_-]*; refuse anything that could break quoting */
    for (const char *c = user; *c; c++)
        if (!g_ascii_isalnum(*c) && *c != '_' && *c != '-' && *c != '.') {
            ui_on_tun_setup(FALSE, "bad user name"); return;
        }
    g_autofree char *urule = polkit_user_rule(user);
    g_autofree char *script = g_strdup_printf(
        "set -e; install -D -o root -g %u -m 0750 '%s' '%s'; "
        "setcap cap_net_admin,cap_net_raw,cap_net_bind_service+ep '%s'; "
        "if [ -d /etc/polkit-1/rules.d ]; then "
        "  getent group wellide >/dev/null || groupadd -r wellide; "
        "  usermod -aG wellide '%s'; "
        "  printf '%%s\\n' \"$0\" > /etc/polkit-1/rules.d/49-wellide.rules; "
        "  printf '%%s\\n' \"$1\" > '/etc/polkit-1/rules.d/49-wellide-user-%s.rules'; "
        "fi",
        (unsigned)getgid(), sb, WL_TUN_BIN, WL_TUN_BIN, user, user);
    GError *e = NULL;
    GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &e,
                                      "pkexec", "/bin/sh", "-c", script, POLKIT_GROUP_RULE, urule, NULL);
    if (!p) { ui_on_tun_setup(FALSE, e->message); g_error_free(e); return; }
    g_subprocess_wait_check_async(p, NULL, on_setup_done, NULL);
#endif
}

/* ---------- config generation ---------- */

/* a server that exits inside the bypass country can't unblock anything */
gboolean server_in_region(Server *s)
{
    if (!S.region || !*S.region || !s->cc) return FALSE;
    return !g_ascii_strcasecmp(s->cc, S.region);
}

typedef struct { const char *region; const char *suffix[4]; const char *geoip, *geosite; } Region;
static const Region REGIONS[] = {
    { "ru", { ".ru", ".su", ".xn--p1ai", NULL }, "geoip-ru", "geosite-category-ru" },
    { "ir", { ".ir", NULL },                    "geoip-ir", "geosite-category-ir" },
    { "cn", { ".cn", NULL },                    "geoip-cn", "geosite-cn" },
};

static const Region *region_cur(void)
{
    for (guint i = 0; S.region && i < G_N_ELEMENTS(REGIONS); i++)
        if (!strcmp(REGIONS[i].region, S.region)) return &REGIONS[i];
    return NULL;
}

static void add_str_array(JsonBuilder *b, const char *name, const char *const *v)
{
    json_builder_set_member_name(b, name);
    json_builder_begin_array(b);
    for (int i = 0; v[i]; i++) json_builder_add_string_value(b, v[i]);
    json_builder_end_array(b);
}

static char *build_config(Profile *p, GError **err)
{
    int n = profile_server_count(p);
    if (n == 0) {
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_FAILED, N_("в профиле нет серверов", "the profile has no servers"));
        return NULL;
    }
    const Region *rg = region_cur();
    gboolean have_geoip = FALSE, have_geosite = FALSE;
    if (rg) rules_available(rg->region, &have_geoip, &have_geosite);
    g_autofree char *geoip = rg ? rules_path(rg->geoip) : NULL;
    g_autofree char *geosite = rg ? rules_path(rg->geosite) : NULL;
    gboolean bypass = rg != NULL;
    gboolean tun = S.mode == MODE_TUN;

    const char *sel = S.selected && *S.selected ? S.selected : "auto";
    if (strcmp(sel, "auto") && !profile_find_server(p, sel)) sel = "auto";

    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "log");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "level"); json_builder_add_string_value(b, "info");
    json_builder_set_member_name(b, "timestamp"); json_builder_add_boolean_value(b, TRUE);
    json_builder_end_object(b);

    /* DNS: remote (through the tunnel) by default, local for RU and for
     * resolving proxy server hostnames */
    json_builder_set_member_name(b, "dns");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "servers");
    json_builder_begin_array(b);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "https");
      json_builder_set_member_name(b, "tag"); json_builder_add_string_value(b, "remote");
      json_builder_set_member_name(b, "server"); json_builder_add_string_value(b, "1.1.1.1");
      json_builder_set_member_name(b, "detour"); json_builder_add_string_value(b, "select");
      json_builder_end_object(b);
      /* "local" would go through systemd-resolved, whose sockets are
       * captured by TUN again -> resolving the proxy's own hostname
       * deadlocks. A plain UDP server is bound to the physical NIC by
       * auto_detect_interface. Yandex DNS answers RU domains correctly. */
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "udp");
      json_builder_set_member_name(b, "tag"); json_builder_add_string_value(b, "local");
      json_builder_set_member_name(b, "server");
      json_builder_add_string_value(b, !g_strcmp0(S.region, "ru") ? "77.88.8.8"
                                       : !g_strcmp0(S.region, "cn") ? "223.5.5.5" : "9.9.9.9");
      json_builder_end_object(b);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "rules");
    json_builder_begin_array(b);
    if (bypass) {
        json_builder_begin_object(b);
        add_str_array(b, "domain_suffix", rg->suffix);
        json_builder_set_member_name(b, "server"); json_builder_add_string_value(b, "local");
        json_builder_end_object(b);
        if (have_geosite) {
            json_builder_begin_object(b);
            add_str_array(b, "rule_set", (const char *[]){ "geosite-local", NULL });
            json_builder_set_member_name(b, "server"); json_builder_add_string_value(b, "local");
            json_builder_end_object(b);
        }
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "final"); json_builder_add_string_value(b, "remote");
    json_builder_set_member_name(b, "strategy"); json_builder_add_string_value(b, "ipv4_only");
    json_builder_end_object(b);

    /* inbounds */
    json_builder_set_member_name(b, "inbounds");
    json_builder_begin_array(b);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "mixed");
      json_builder_set_member_name(b, "tag"); json_builder_add_string_value(b, "mixed-in");
      json_builder_set_member_name(b, "listen"); json_builder_add_string_value(b, "127.0.0.1");
      json_builder_set_member_name(b, "listen_port"); json_builder_add_int_value(b, S.port);
      json_builder_end_object(b);
    if (tun) {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "tun");
      json_builder_set_member_name(b, "tag"); json_builder_add_string_value(b, "tun-in");
      json_builder_set_member_name(b, "interface_name"); json_builder_add_string_value(b, "wellide0");
      /* IPv4 only: with an IPv6 address on the TUN, apps prefer AAAA and
       * most VPN exits (and many ISPs) can't carry IPv6 — the TLS handshake
       * then dies with "unexpected eof". Hiddify does the same. */
      add_str_array(b, "address", (const char *[]){ "172.19.0.1/30", NULL });
      json_builder_set_member_name(b, "mtu"); json_builder_add_int_value(b, 9000);
      json_builder_set_member_name(b, "auto_route"); json_builder_add_boolean_value(b, TRUE);
      json_builder_set_member_name(b, "strict_route"); json_builder_add_boolean_value(b, TRUE);
      /* gvisor: userspace TCP stack. "system"/"mixed" hand TCP to the kernel,
       * where firewalld (default zone rejects unknown ifaces) drops it. */
      json_builder_set_member_name(b, "stack"); json_builder_add_string_value(b, "gvisor");
      json_builder_end_object(b);
    }
    json_builder_end_array(b);

    /* outbounds: select -> [auto, servers...], auto = urltest */
    json_builder_set_member_name(b, "outbounds");
    json_builder_begin_array(b);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "selector");
      json_builder_set_member_name(b, "tag"); json_builder_add_string_value(b, "select");
      json_builder_set_member_name(b, "outbounds");
      json_builder_begin_array(b);
      json_builder_add_string_value(b, "auto");
      for (guint i = 0; i < p->servers->len; i++) {
          Server *s = p->servers->pdata[i];
          if (!s->separator) json_builder_add_string_value(b, s->tag);
      }
      json_builder_end_array(b);
      json_builder_set_member_name(b, "default"); json_builder_add_string_value(b, sel);
      json_builder_set_member_name(b, "interrupt_exist_connections"); json_builder_add_boolean_value(b, TRUE);
      json_builder_end_object(b);

      json_builder_begin_object(b);
      json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "urltest");
      json_builder_set_member_name(b, "tag"); json_builder_add_string_value(b, "auto");
      json_builder_set_member_name(b, "outbounds");
      json_builder_begin_array(b);
      {
          int foreign = 0;
          for (guint i = 0; i < p->servers->len; i++) {
              Server *s = p->servers->pdata[i];
              if (!s->separator && !server_in_region(s)) foreign++;
          }
          for (guint i = 0; i < p->servers->len; i++) {
              Server *s = p->servers->pdata[i];
              if (s->separator || (S.auto_skip_region && foreign && server_in_region(s))) continue;
              json_builder_add_string_value(b, s->tag);
          }
      }
      json_builder_end_array(b);
      json_builder_set_member_name(b, "url"); json_builder_add_string_value(b, TEST_URL);
      /* short interval: a dead server must fall out of "auto" quickly */
      json_builder_set_member_name(b, "interval"); json_builder_add_string_value(b, "2m");
      json_builder_set_member_name(b, "idle_timeout"); json_builder_add_string_value(b, "30m");
      json_builder_set_member_name(b, "tolerance"); json_builder_add_int_value(b, 50);
      json_builder_end_object(b);

      for (guint i = 0; i < p->servers->len; i++) {
          Server *s = p->servers->pdata[i];
          if (s->separator || !s->ob) continue;
          JsonNode *n = json_node_new(JSON_NODE_OBJECT);
          json_node_set_object(n, s->ob);
          json_builder_add_value(b, n);
      }

      json_builder_begin_object(b);
      json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "direct");
      json_builder_set_member_name(b, "tag"); json_builder_add_string_value(b, "direct");
      json_builder_end_object(b);
    json_builder_end_array(b);

    /* routing */
    json_builder_set_member_name(b, "route");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "rules");
    json_builder_begin_array(b);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "action"); json_builder_add_string_value(b, "sniff");
      json_builder_end_object(b);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "protocol"); json_builder_add_string_value(b, "dns");
      json_builder_set_member_name(b, "action"); json_builder_add_string_value(b, "hijack-dns");
      json_builder_end_object(b);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "ip_is_private"); json_builder_add_boolean_value(b, TRUE);
      json_builder_set_member_name(b, "outbound"); json_builder_add_string_value(b, "direct");
      json_builder_end_object(b);
    if (bypass) {
      json_builder_begin_object(b);
      add_str_array(b, "domain_suffix", rg->suffix);
      json_builder_set_member_name(b, "outbound"); json_builder_add_string_value(b, "direct");
      json_builder_end_object(b);
      if (have_geoip || have_geosite) {
        json_builder_begin_object(b);
        const char *sets[3] = { NULL };
        int k = 0;
        if (have_geosite) sets[k++] = "geosite-local";
        if (have_geoip) sets[k++] = "geoip-local";
        add_str_array(b, "rule_set", sets);
        json_builder_set_member_name(b, "outbound"); json_builder_add_string_value(b, "direct");
        json_builder_end_object(b);
      }
    }
    json_builder_end_array(b);
    if (have_geoip || have_geosite) {
      json_builder_set_member_name(b, "rule_set");
      json_builder_begin_array(b);
      const char *tags[] = { "geosite-local", "geoip-local" };
      const char *paths[] = { geosite, geoip };
      gboolean have[] = { have_geosite, have_geoip };
      for (int i = 0; i < 2; i++) {
        if (!have[i]) continue;
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "local");
        json_builder_set_member_name(b, "tag"); json_builder_add_string_value(b, tags[i]);
        json_builder_set_member_name(b, "format"); json_builder_add_string_value(b, "binary");
        json_builder_set_member_name(b, "path"); json_builder_add_string_value(b, paths[i]);
        json_builder_end_object(b);
      }
      json_builder_end_array(b);
    }
    json_builder_set_member_name(b, "final"); json_builder_add_string_value(b, "select");
    json_builder_set_member_name(b, "auto_detect_interface"); json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "default_domain_resolver"); json_builder_add_string_value(b, "local");
    json_builder_end_object(b);

    json_builder_set_member_name(b, "experimental");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "clash_api");
    json_builder_begin_object(b);
    g_autofree char *ctl = g_strdup_printf("127.0.0.1:%d", S.api_port);
    json_builder_set_member_name(b, "external_controller"); json_builder_add_string_value(b, ctl);
    json_builder_end_object(b);
    json_builder_end_object(b);

    json_builder_end_object(b);

    g_autoptr(JsonGenerator) g = json_generator_new();
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    json_generator_set_pretty(g, TRUE);
    return json_generator_to_data(g, NULL);
}

/* ---------- process I/O ---------- */

static void on_ignore_early(GObject *src, GAsyncResult *res, gpointer ud)
{
    GBytes *b = soup_session_send_and_read_finish(SOUP_SESSION(src), res, NULL);
    if (b) g_bytes_unref(b);
    g_object_unref(ud);
    /* the group test updates "now"; refresh the public IP afterwards */
    if (state == ST_ON) core_check_ip();
}

static char *strip_ansi(const char *s)
{
    GString *o = g_string_new(NULL);
    for (const char *p = s; *p; p++) {
        if (*p == 0x1b && p[1] == '[') {
            p += 2;
            while (*p && !g_ascii_isalpha(*p)) p++;
            if (!*p) break;
            continue;
        }
        g_string_append_c(o, *p);
    }
    return g_string_free(o, FALSE);
}

static void read_line(GDataInputStream *in);

static void on_line(GObject *src, GAsyncResult *res, gpointer ud)
{
    GDataInputStream *in = G_DATA_INPUT_STREAM(src);
    gsize n = 0;
    g_autofree char *line = g_data_input_stream_read_line_finish_utf8(in, res, &n, NULL);
    if (!line) { g_object_unref(in); return; }
    g_autofree char *clean = strip_ansi(line);
    ui_on_log(clean);
    if (strstr(clean, "FATAL") || strstr(clean, "ERROR") || strstr(clean, "start service")) {
        if (last_err->len > 2000) g_string_erase(last_err, 0, last_err->len - 1000);
        g_string_append_printf(last_err, "%s\n", clean);
    }
    read_line(in);
}

static void read_line(GDataInputStream *in)
{
    g_data_input_stream_read_line_async(in, G_PRIORITY_LOW, NULL, on_line, NULL);
}

static void traffic_start(void);

static void stop_polls(void)
{
    if (ready_poll) { g_source_remove(ready_poll); ready_poll = 0; }
    if (traffic_cancel) {
        g_cancellable_cancel(traffic_cancel);
        g_clear_object(&traffic_cancel);
    }
}

static void on_core_exit(GObject *src, GAsyncResult *res, gpointer ud)
{
    GSubprocess *p = G_SUBPROCESS(src);
    g_subprocess_wait_finish(p, res, NULL);
    if (p != proc) { g_object_unref(p); return; }   /* stale instance */
    g_clear_object(&proc);
    stop_polls();
    if (S.mode == MODE_PROXY || S.sysproxy_set) sysproxy_disable();
    ui_on_traffic(-1, -1);

    CoreState was = state;
    if (restart_pending) {
        restart_pending = FALSE;
        state = ST_OFF;
        core_start();
        return;
    }
    if (was == ST_STOPPING) {
        set_state(ST_OFF, NULL);
    } else {
        g_autofree char *e = last_err->len ? g_strdup(last_err->str)
                                           : g_strdup(N_("ядро sing-box неожиданно завершилось", "sing-box exited unexpectedly"));
        g_strstrip(e);
        set_state(ST_OFF, e);
    }
}

/* sing-box leaves urltest "now" at the first member until its first probe
 * round finishes, and that round can stall behind a dead server. Kick a
 * group delay test right away so "auto" picks a live server in ~5 s. */
static void kick_auto_test(void)
{
    g_autofree char *u = api_url("/group/auto/delay?timeout=4000&url=" TEST_URL);
    SoupMessage *m = soup_message_new("GET", u);
    soup_session_send_and_read_async(api_session(), m, G_PRIORITY_DEFAULT, NULL, on_ignore_early, m);
}

static void on_ready_reply(GObject *src, GAsyncResult *res, gpointer ud)
{
    GBytes *b = soup_session_send_and_read_finish(SOUP_SESSION(src), res, NULL);
    SoupMessage *m = ud;
    gboolean ok = b && soup_message_get_status(m) == 200;
    if (b) g_bytes_unref(b);
    g_object_unref(m);
    if (!ok || state != ST_STARTING) return;
    if (ready_poll) { g_source_remove(ready_poll); ready_poll = 0; }
    if (S.mode == MODE_PROXY) sysproxy_enable(S.port);
    set_state(ST_ON, NULL);
    kick_auto_test();
    traffic_start();
    core_check_ip();
}

static gboolean poll_ready(gpointer ud)
{
    if (state != ST_STARTING) { ready_poll = 0; return G_SOURCE_REMOVE; }
    if (++ready_tries > 60) {                      /* 15 s */
        ready_poll = 0;
        g_string_append(last_err, N_("ядро не ответило за 15 секунд\n", "the core did not respond within 15 s\n"));
        core_stop();
        return G_SOURCE_REMOVE;
    }
    g_autofree char *u = api_url("/version");
    SoupMessage *m = soup_message_new("GET", u);
    soup_session_send_and_read_async(api_session(), m, G_PRIORITY_DEFAULT, NULL, on_ready_reply, m);
    return G_SOURCE_CONTINUE;
}

void core_start(void)
{
    if (state != ST_OFF) return;
    if (!last_err) last_err = g_string_new(NULL);
    g_string_truncate(last_err, 0);

    Profile *p = profile_active();
    if (!p) { ui_on_state(ST_OFF, N_("сначала добавьте профиль (подписку)", "add a profile (subscription) first")); return; }

    const char *bin = core_bin();
    if (S.mode == MODE_TUN) {
        if (!tun_ready()) {
            ui_on_state(ST_OFF, N_("для TUN нужны права: Настройки → «Выдать права для TUN»", "TUN needs permissions: Settings → “Grant TUN permissions”"));
            return;
        }
#ifndef G_OS_WIN32
        bin = WL_TUN_BIN;
#endif
    }

    GError *e = NULL;
    g_autofree char *cfg = build_config(p, &e);
    if (!cfg) { ui_on_state(ST_OFF, e->message); g_error_free(e); return; }
    g_autofree char *dir = wl_cache_dir();
    g_autofree char *path = g_build_filename(dir, "config.json", NULL);
    if (!g_file_set_contents_full(path, cfg, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &e)) {
        ui_on_state(ST_OFF, e->message); g_error_free(e); return;
    }

    set_state(ST_STARTING, NULL);
    tot_up = tot_down = 0;
    GSubprocessLauncher *l = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_setenv(l, "NO_COLOR", "1", TRUE);
    g_subprocess_launcher_set_cwd(l, dir);
    proc = g_subprocess_launcher_spawn(l, &e, bin, "run", "-c", path, "--disable-color", NULL);
    g_object_unref(l);
    if (!proc) {
        set_state(ST_OFF, e->message);
        g_error_free(e);
        return;
    }
    GDataInputStream *in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(proc));
    read_line(in);
    g_subprocess_wait_async(proc, NULL, on_core_exit, NULL);
    ready_tries = 0;
    ready_poll = g_timeout_add(250, poll_ready, NULL);
}

static gboolean force_kill(gpointer ud)
{
    GSubprocess *p = ud;
    if (p == proc) g_subprocess_force_exit(p);
    g_object_unref(p);
    return G_SOURCE_REMOVE;
}

void core_stop(void)
{
    if (!proc || state == ST_STOPPING) return;
    stop_polls();
    if (S.mode == MODE_PROXY || S.sysproxy_set) sysproxy_disable();
    set_state(ST_STOPPING, NULL);
#ifdef G_OS_WIN32
    g_subprocess_force_exit(proc);
#else
    g_subprocess_send_signal(proc, SIGTERM);
#endif
    g_timeout_add_seconds(4, force_kill, g_object_ref(proc));
}

void core_restart(void)
{
    if (state == ST_ON || state == ST_STARTING) {
        restart_pending = TRUE;
        core_stop();
    }
}

/* ---------- API calls ---------- */

static void on_ignore(GObject *src, GAsyncResult *res, gpointer ud)
{
    GBytes *b = soup_session_send_and_read_finish(SOUP_SESSION(src), res, NULL);
    if (b) g_bytes_unref(b);
    g_object_unref(ud);
}

void core_select(const char *tag)
{
    g_free(S.selected);
    S.selected = g_strdup(tag);
    settings_save();
    if (state != ST_ON) return;
    g_autofree char *u = api_url("/proxies/select");
    SoupMessage *m = soup_message_new("PUT", u);
    /* build with json-glib: g_strescape would mangle UTF-8 (emoji flags) */
    g_autoptr(JsonBuilder) jb = json_builder_new();
    json_builder_begin_object(jb);
    json_builder_set_member_name(jb, "name");
    json_builder_add_string_value(jb, tag);
    json_builder_end_object(jb);
    g_autoptr(JsonNode) jn = json_builder_get_root(jb);
    g_autofree char *body = json_to_string(jn, FALSE);
    g_autoptr(GBytes) bb = g_bytes_new(body, strlen(body));
    soup_message_set_request_body_from_bytes(m, "application/json", bb);
    soup_session_send_and_read_async(api_session(), m, G_PRIORITY_DEFAULT, NULL, on_ignore, m);
    core_check_ip();
}

/* delay: via Clash API while running, plain TCP connect time otherwise.
 * core_test_all() feeds a queue with limited concurrency so a big list
 * doesn't stampede the network or the core. */
typedef struct { char *tag; SoupMessage *m; gint64 t0; gboolean queued; } Delay;

static GQueue ping_q = G_QUEUE_INIT;
static int ping_inflight;
static void test_one(const char *tag, gboolean queued);

static void ping_pump(void)
{
    while (ping_inflight < 6 && !g_queue_is_empty(&ping_q)) {
        g_autofree char *tag = g_queue_pop_head(&ping_q);
        ping_inflight++;
        test_one(tag, TRUE);
    }
}

static void delay_finish(Delay *d, int ms, gboolean tcp)
{
    ui_on_delay(d->tag, ms, tcp);
    if (d->queued) { ping_inflight--; ping_pump(); }
    if (d->m) g_object_unref(d->m);
    g_free(d->tag);
    g_free(d);
}

static void on_delay(GObject *src, GAsyncResult *res, gpointer ud)
{
    Delay *d = ud;
    GBytes *b = soup_session_send_and_read_finish(SOUP_SESSION(src), res, NULL);
    int ms = 0;
    if (b && soup_message_get_status(d->m) == 200) {
        g_autoptr(JsonParser) p = json_parser_new();
        gsize n;
        const char *data = g_bytes_get_data(b, &n);
        if (json_parser_load_from_data(p, data, n, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p)))
            ms = json_object_get_int_member_with_default(json_node_get_object(json_parser_get_root(p)), "delay", 0);
    }
    if (b) g_bytes_unref(b);
    delay_finish(d, ms, FALSE);
}

static void on_tcp(GObject *src, GAsyncResult *res, gpointer ud)
{
    Delay *d = ud;
    GSocketConnection *c = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res, NULL);
    int ms = c ? MAX(1, (int)((g_get_monotonic_time() - d->t0) / 1000)) : 0;
    if (c) { g_io_stream_close(G_IO_STREAM(c), NULL, NULL); g_object_unref(c); }
    g_object_unref(src);
    delay_finish(d, ms, TRUE);
}

static void test_one(const char *tag, gboolean queued)
{
    Delay *d = g_new0(Delay, 1);
    d->tag = g_strdup(tag);
    d->queued = queued;
    if (state == ST_ON) {
        g_autofree char *esc = g_uri_escape_string(tag, NULL, FALSE);
        g_autofree char *path = g_strdup_printf("/proxies/%s/delay?timeout=5000&url=%s", esc, TEST_URL);
        g_autofree char *u = api_url(path);
        d->m = soup_message_new("GET", u);
        if (!d->m) { delay_finish(d, 0, FALSE); return; }
        soup_session_send_and_read_async(api_session(), d->m, G_PRIORITY_DEFAULT, NULL, on_delay, d);
        return;
    }
    Server *s = profile_find_server(profile_active(), tag);
    if (!s || !s->server) { delay_finish(d, 0, TRUE); return; }
    GSocketClient *c = g_socket_client_new();
    g_socket_client_set_timeout(c, 4);
    g_socket_client_set_enable_proxy(c, FALSE);
    d->t0 = g_get_monotonic_time();
    g_socket_client_connect_to_host_async(c, s->server, s->port, NULL, on_tcp, d);
}

void core_test_delay(const char *tag) { test_one(tag, FALSE); }

void core_test_all(void)
{
    Profile *p = profile_active();
    if (!p) return;
    while (!g_queue_is_empty(&ping_q)) g_free(g_queue_pop_head(&ping_q));
    for (guint i = 0; i < p->servers->len; i++) {
        Server *s = p->servers->pdata[i];
        if (s->separator) continue;
        ui_on_delay(s->tag, -2, state != ST_ON);
        g_queue_push_tail(&ping_q, g_strdup(s->tag));
    }
    ping_pump();
}

/* traffic: /traffic streams one JSON object per second */
static void read_traffic(GDataInputStream *in);

static gboolean traffic_retry(gpointer ud)
{
    traffic_start();
    return G_SOURCE_REMOVE;
}

static void on_traffic_line(GObject *src, GAsyncResult *res, gpointer ud)
{
    GDataInputStream *in = G_DATA_INPUT_STREAM(src);
    gsize n = 0;
    GError *e = NULL;
    g_autofree char *line = g_data_input_stream_read_line_finish_utf8(in, res, &n, &e);
    if (!line) {
        gboolean cancelled = e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        g_clear_error(&e);
        g_object_unref(in);
        /* stream dropped while still connected: reconnect */
        if (!cancelled && state == ST_ON) g_timeout_add_seconds(1, traffic_retry, NULL);
        return;
    }
    g_autoptr(JsonParser) p = json_parser_new();
    if (json_parser_load_from_data(p, line, n, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
        JsonObject *o = json_node_get_object(json_parser_get_root(p));
        gint64 up = json_object_get_int_member_with_default(o, "up", 0);
        gint64 down = json_object_get_int_member_with_default(o, "down", 0);
        tot_up += up; tot_down += down;
        ui_on_traffic(up, down);
    }
    read_traffic(in);
}

static void read_traffic(GDataInputStream *in)
{
    g_data_input_stream_read_line_async(in, G_PRIORITY_LOW, traffic_cancel, on_traffic_line, NULL);
}

static void on_traffic_open(GObject *src, GAsyncResult *res, gpointer ud)
{
    GInputStream *s = soup_session_send_finish(SOUP_SESSION(src), res, NULL);
    g_object_unref(ud);
    if (!s) return;
    GDataInputStream *in = g_data_input_stream_new(s);
    g_object_unref(s);
    read_traffic(in);
}

static void traffic_start(void)
{
    if (state != ST_ON) return;
    if (traffic_cancel) { g_cancellable_cancel(traffic_cancel); g_object_unref(traffic_cancel); }
    traffic_cancel = g_cancellable_new();
    g_autofree char *u = api_url("/traffic");
    SoupMessage *m = soup_message_new("GET", u);
    /* the default 12s session timeout would kill the stream */
    static SoupSession *stream_sess;
    if (!stream_sess) stream_sess = soup_session_new_with_options("proxy-resolver", NULL, NULL);
    soup_session_send_async(stream_sess, m, G_PRIORITY_LOW, traffic_cancel, on_traffic_open, m);
}

gint64 core_total_up(void) { return tot_up; }
gint64 core_total_down(void) { return tot_down; }

/* public IP as seen through the tunnel; ip.sb first, ipwho.is fallback */
static const char *IP_URLS[] = { "https://api.ip.sb/geoip", "https://ipwho.is/" };

typedef struct { SoupMessage *m; int idx; guint gen; } IpReq;
static guint ip_gen;

static void ip_request(int idx, guint gen);

static void on_ip(GObject *src, GAsyncResult *res, gpointer ud)
{
    IpReq *r = ud;
    GBytes *b = soup_session_send_and_read_finish(SOUP_SESSION(src), res, NULL);
    gboolean done = FALSE;
    if (r->gen == ip_gen && b && soup_message_get_status(r->m) == 200) {
        g_autoptr(JsonParser) p = json_parser_new();
        gsize n;
        const char *d = g_bytes_get_data(b, &n);
        if (json_parser_load_from_data(p, d, n, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
            JsonObject *o = json_node_get_object(json_parser_get_root(p));
            const char *ip = json_object_get_string_member_with_default(o, "ip", NULL);
            const char *cc = json_object_get_string_member_with_default(o, "country_code", "");
            if (ip) { ui_on_ip(ip, cc); done = TRUE; }
        }
    }
    if (b) g_bytes_unref(b);
    if (!done && r->gen == ip_gen && state == ST_ON) {
        if (r->idx + 1 < (int)G_N_ELEMENTS(IP_URLS)) ip_request(r->idx + 1, r->gen);
        else ui_on_ip(NULL, NULL);
    }
    g_object_unref(r->m);
    g_object_unref(src);
    g_free(r);
}

static void ip_request(int idx, guint gen)
{
    g_autofree char *proxy = g_strdup_printf("http://127.0.0.1:%d", S.port);
    GProxyResolver *pr = g_simple_proxy_resolver_new(proxy, NULL);
    SoupSession *s = soup_session_new_with_options("proxy-resolver", pr, "timeout", 10,
                                                   "user-agent", "curl/8", NULL);
    g_object_unref(pr);
    IpReq *r = g_new0(IpReq, 1);
    r->m = soup_message_new("GET", IP_URLS[idx]);
    r->idx = idx;
    r->gen = gen;
    soup_session_send_and_read_async(s, r->m, G_PRIORITY_LOW, NULL, on_ip, r);
}

static gboolean do_check_ip(gpointer ud)
{
    if (state == ST_ON && GPOINTER_TO_UINT(ud) == ip_gen) ip_request(0, ip_gen);
    return G_SOURCE_REMOVE;
}

void core_check_ip(void)
{
    ui_on_ip("…", NULL);
    /* newer checks supersede older ones (fast server switching) */
    ip_gen++;
    g_timeout_add(600, do_check_ip, GUINT_TO_POINTER(ip_gen));
}

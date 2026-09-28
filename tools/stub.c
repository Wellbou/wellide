/* UI callbacks for tools/render (no window) */
#include "../src/wellide.h"
void ui_on_state(CoreState st, const char *e) {}
void ui_on_traffic(gint64 u, gint64 d) {}
void ui_on_log(const char *l) {}
void ui_on_delay(const char *t, int ms, gboolean tcp) {}
void ui_on_ip(const char *ip, const char *cc) {}
void ui_on_tun_setup(gboolean ok, const char *m) {}
void ui_toast(const char *m) {}

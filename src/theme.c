/* Themes are plain GTK3 CSS built from a small palette, so a new theme is
 * one table entry. "paper" adds the notebook ruling on top. */
#include "wellide.h"
#include <string.h>

typedef struct {
    const char *id;
    const char *bg, *bg2, *card, *fg, *fg_dim, *accent, *accent_fg, *on, *off, *line, *danger;
    const char *extra;  /* theme-specific CSS */
} Palette;

static const Palette THEMES[] = {
    { "purple",
      "#130a22", "#1c1030", "#241540", "#ede7f6", "#a594c4",
      "#ab47bc", "#ffffff", "#7c4dff", "#3a2a5c", "#3b2960", "#ff5277",
      ".connect.on { box-shadow: 0 0 40px 6px alpha(#b388ff, 0.45); }\n" },
    { "paper",
      "#fdfdf8", "#f4f4ec", "#ffffff", "#1d2433", "#6b7385",
      "#2b59c3", "#ffffff", "#2b59c3", "#c9cfdb", "#b8cdf0", "#d0342c",
      /* ruled paper: faint blue lines every 28px + red margin */
      ".main-bg {"
      "  background-color: #fdfdf8;"
      "  background-image:"
      "    linear-gradient(to right, transparent 46px, alpha(#e05a5a,0.55) 46px, alpha(#e05a5a,0.55) 48px, transparent 48px),"
      "    repeating-linear-gradient(to bottom, transparent 0px, transparent 27px, #c7d8f4 27px, #c7d8f4 28px);"
      "}\n"
      ".sidebar { border-right: 2px solid #1d2433; }\n"
      ".card { border: 2px solid #1d2433; box-shadow: 3px 3px 0 #1d2433; }\n"
      ".connect { border: 3px solid #1d2433; box-shadow: 5px 5px 0 #1d2433; }\n"
      ".connect.on { box-shadow: 5px 5px 0 #1d2433; }\n"
      "button.flat-btn { border: 2px solid #1d2433; }\n" },
};

char *theme_css(const char *id)
{
    const Palette *t = &THEMES[0];
    for (guint i = 0; i < G_N_ELEMENTS(THEMES); i++)
        if (id && !strcmp(id, THEMES[i].id)) t = &THEMES[i];

    return g_strdup_printf(
        "* { outline-width: 0; }\n"
        "window, .main-bg { background-color: %1$s; color: %4$s; }\n"
        "label { color: %4$s; }\n"
        ".dim { color: %5$s; }\n"
        ".sidebar { background-color: %2$s; padding: 14px 8px; }\n"
        ".brand { font-size: 20px; font-weight: 800; color: %6$s; margin: 4px 10px 18px 10px; }\n"
        ".nav { background: none; border: none; box-shadow: none; border-radius: 10px;"
        "       padding: 9px 14px; color: %5$s; font-weight: 600; }\n"
        ".nav:hover { background-color: alpha(%6$s, 0.12); color: %4$s; }\n"
        ".nav.active { background-color: %6$s; color: %7$s; }\n"
        ".nav.active label { color: %7$s; }\n"
        ".card { background-color: %3$s; border-radius: 14px; padding: 14px 16px; }\n"
        ".h1 { font-size: 22px; font-weight: 800; }\n"
        ".h2 { font-size: 15px; font-weight: 700; }\n"
        ".big { font-size: 17px; font-weight: 700; }\n"
        ".mono { font-family: monospace; font-size: 11px; }\n"
        ".connect { min-width: 170px; min-height: 170px; border-radius: 999px;"
        "           background-image: none; background-color: %9$s; border: none;"
        "           font-size: 17px; font-weight: 800; color: %4$s; }\n"
        ".connect label { color: %4$s; }\n"
        ".connect.on { background-color: %8$s; }\n"
        ".connect.on label { color: #ffffff; }\n"
        ".connect.busy { background-color: alpha(%8$s, 0.5); }\n"
        ".connect:hover { background-image: linear-gradient(alpha(#ffffff,0.06), alpha(#ffffff,0.06)); }\n"
        ".status-on { color: %8$s; font-weight: 800; }\n"
        ".status-off { color: %5$s; font-weight: 700; }\n"
        ".error { color: %11$s; }\n"
        "button.flat-btn, button.accent-btn, combobox button, spinbutton button {"
        "   background-image: none; box-shadow: none; text-shadow: none; border-radius: 9px; }\n"
        "button.flat-btn { background-color: %3$s; color: %4$s; border: 1px solid %10$s; padding: 6px 12px; }\n"
        "button.flat-btn:hover { background-color: alpha(%6$s, 0.15); }\n"
        "button.accent-btn { background-color: %6$s; color: %7$s; border: none; padding: 6px 14px; font-weight: 700; }\n"
        "button.accent-btn label { color: %7$s; }\n"
        "button.accent-btn:hover { background-color: shade(%6$s, 1.1); }\n"
        "entry, textview text, spinbutton { background-color: %3$s; color: %4$s; border-radius: 8px;"
        "   border: 1px solid %10$s; box-shadow: none; }\n"
        "textview, textview text { background-color: %3$s; }\n"
        "list, row { background-color: transparent; }\n"
        "row.server { border-radius: 10px; padding: 6px 10px; margin: 1px 0; }\n"
        "row.server:hover { background-color: alpha(%6$s, 0.10); }\n"
        "row.server.selected-srv { background-color: alpha(%6$s, 0.22); }\n"
        "row.sep label { color: %5$s; font-weight: 700; font-size: 11px; }\n"
        "row.sep { padding: 10px 10px 2px 10px; }\n"
        ".ping-good { color: #3ecf8e; font-weight: 700; }\n"
        ".ping-mid { color: #e8b339; font-weight: 700; }\n"
        ".ping-bad { color: %11$s; font-weight: 700; }\n"
        "progressbar trough { background-color: %10$s; border-radius: 6px; min-height: 8px; border: none; }\n"
        "progressbar progress { background-color: %6$s; border-radius: 6px; min-height: 8px; border: none; }\n"
        "switch { background-color: %10$s; border: none; }\n"
        "switch:checked { background-color: %6$s; }\n"
        "switch slider { background-color: #ffffff; border: none; }\n"
        "scrollbar, scrolledwindow, viewport { background-color: transparent; border: none; }\n"
        ".toast { background-color: %4$s; color: %1$s; border-radius: 10px; padding: 8px 14px; }\n"
        ".toast label { color: %1$s; }\n"
        "%12$s",
        t->bg, t->bg2, t->card, t->fg, t->fg_dim, t->accent, t->accent_fg,
        t->on, t->off, t->line, t->danger, t->extra);
}

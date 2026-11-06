/* smo_settings.c - the settings menu (F1; a pad's HOME button), in the look of the game's own menus.
 *
 * Three tabs. Display: internal resolution, window mode, window size (window modes only), vertical sync, performance
 * overlay and its position. Graphics: the GPU (the Vulkan devices of this PC), anisotropic filtering, whether a draw waits
 * for a shader not ready yet. System: the game's language, and where
 * its files are (saves, captures, settings, logs; read only).
 *
 * As the amiibo menu (smo_amiibo.c), whose machinery it shares: the game is held after a presented frame (nvn_gl.c
 * queue_present calls settings_menu_run; display.c stops the game's vsync event), the frame is read back and blurred
 * behind the menu (smo_ui.c ui_backdrop), the menu is drawn in host memory from the game's own fonts and layout textures
 * and shown over the whole window (UiMenuJob). It is composed at most 1920 pixels wide (the window scales it), so a 4K
 * internal resolution does not make it slow.
 *
 * Left / right only show another choice ("(not applied)"); + (Enter) applies them all: each is written to config/graphics.ini
 * (gfx_options_save: the line of that key, comments kept) and what the running game can take changes at once: the window
 * mode and size (display.c, on the event loop), vertical sync (the swapchain is made again, nvn_vk.c), the performance
 * overlay, the unfocused behaviour (nnmisc.c reads it while it watches). The internal resolution, the GPU, anisotropic
 * filtering, shaders and the language are read when the game starts: they then say "(after restart)", and + once more
 * starts the game again in place (boot.c smo_restart; progress since the last save is lost, the menu says so). Closing
 * the menu drops what was not applied. The language is also written to the game folder's smo.env, which ./smo reads and
 * which would otherwise win.
 *
 * Controls: up / down (stick, +Control Pad, the keyboard's arrows) select a row, left / right change its choice, + (Enter)
 * applies (or restarts), A switches a mod on or off (Mods tab), L / R (ZL / ZR, Q / E) switch tabs, B, Escape, HOME or F1 again
 * close. Labels are English, drawn with the game's fonts. TEST AID: SMO_SETTINGS_SHOT_DIR=<folder> saves each new picture
 * of the menu as a PNG. */
#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "smo_ui.h"
#include "gfx_options.h"

int nvn_menu_job(UiMenuJob *m);                           /* nvn_gl.c */
long long nvn_front_frame_next(void);
uint64_t hid_menu_buttons(void);                         /* nnhid.c */
void hid_menu_closed(void);
int hid_menu_key(int which);
int hid_menu_home(void);
void display_menu_hold(int on);                          /* display.c */
void display_menu_wait(void);
int display_closing(void);
void display_set_window(int mode, int w, int h);
void display_set_perf_overlay(int level, int corner);
int display_perf_overlay(void);
void nvn_vk_set_vsync(int opt);                          /* nvn_vk.c */
int nvn_vk_gpus(char names[][128], int max, int *used);
int nvn_vk_present_offered(int opt);
void smo_restart(void);                                  /* boot.c */
const char *oe_language(void);                           /* nnmisc.c */
const char *fs_user_root(void);                          /* nnfs.c */
int tex_write_png(const char *path, const uint8_t *rgba, int w, int h);

enum { B_A = 0, B_B = 1, B_L = 6, B_R = 7, B_PLUS = 10, B_ZL = 8, B_ZR = 9, B_LEFT = 12, B_UP = 13, B_RIGHT = 14, B_DOWN = 15,
       B_SL_LEFT = 16, B_SL_UP = 17, B_SL_RIGHT = 18, B_SL_DOWN = 19 };
#define BIT(b) (1ull << (b))

/* ---------------------------------------------------------------- the settings */
enum { K_RES, K_MODE, K_SIZE, K_VSYNC, K_PERF, K_CORNER, K_GPU, K_ANISO, K_TFILTER, K_LANG,
       K_PATH_SAVE, K_PATH_ALBUM, K_PATH_CONFIG, K_PATH_LOGS, K_PATH_MODS, K_MOD0 /* K_MOD0 + i: the i-th mod (nnfs.c fs_mod_*) */ };
#define MODS_MAX 32
enum { TAB_DISPLAY, TAB_GRAPHICS, TAB_SYSTEM, TAB_MODS, TABS };
static const char *const k_tab[TABS] = {"Display", "Graphics", "System", "Mods"};
#define VISIBLE_ROWS 7                                    /* rows that fit above the footer; a longer tab scrolls with the choice */
#define MAXCH 20
typedef struct { char value[128], label[128]; } Choice;
typedef struct {
    int key, tab, restart;                                /* restart: read when the game starts */
    const char *label;
    int n, cur, saved, start;                             /* choices; the one shown; the one in graphics.ini; the one in effect */
    Choice ch[MAXCH];
    char path[PATH_MAX];                                  /* the path rows */
} Item;
static Item g_items[16 + 1 + MODS_MAX];
static char (*g_next)[128];                               /* by key: the value saved for the next start (items_load) */
static int g_nitems;

static Item *item_add(int key, int tab, const char *label, int restart) {
    Item *it = &g_items[g_nitems++];
    memset(it, 0, sizeof *it);
    it->key = key; it->tab = tab; it->label = label; it->restart = restart;
    return it;
}
static void choice(Item *it, const char *value, const char *label) {
    if (it->n >= MAXCH) return;
    snprintf(it->ch[it->n].value, sizeof it->ch[0].value, "%s", value);
    snprintf(it->ch[it->n].label, sizeof it->ch[0].label, "%s", label);
    it->n++;
}
static int choice_of(const Item *it, const char *value) {
    for (int i = 0; i < it->n; i++) if (!strcasecmp(it->ch[i].value, value)) return i;
    return -1;
}
static void path_of(char *out, const char *p) {              /* without ".." */
    char r[PATH_MAX];
    if (realpath(p, r)) snprintf(out, PATH_MAX, "%s", r); else snprintf(out, PATH_MAX, "%s", p);
}
static int window_mode_now(void) { return gfx_options()->window_mode; }

static void items_load(void) {
    const GfxOptions *go = gfx_options();
    char buf[PATH_MAX];
    g_nitems = 0;
    /* ---- Display */
    Item *it = item_add(K_RES, TAB_DISPLAY, "Internal resolution", 1);
    static const struct { const char *v, *l; float s; } k_res[] = {
        {"720p", "720p (1280 x 720)", 0.8f}, {"900p", "900p (1600 x 900, the game's)", 1.0f}, {"1080p", "1080p (1920 x 1080)", 1.2f},
        {"1440p", "1440p (2560 x 1440)", 1.6f}, {"2160p", "2160p (3840 x 2160, 4K)", 2.4f}};
    it->cur = -1;
    for (int i = 0; i < 5; i++) { choice(it, k_res[i].v, k_res[i].l); if (fabsf(go->scale - k_res[i].s) < 0.01f) it->cur = i; }
    if (it->cur < 0) { char v[32], l[64]; snprintf(v, sizeof v, "%.2f", go->scale); snprintf(l, sizeof l, "Scale %.2f (%d x %d)", go->scale, (int)(1600 * go->scale), (int)(900 * go->scale)); choice(it, v, l); it->cur = it->n - 1; }
    it = item_add(K_MODE, TAB_DISPLAY, "Mode", 0);
    choice(it, "borderless_fullscreen", "Full screen"); choice(it, "fullscreen", "Full screen, always on top");
    choice(it, "borderless_window", "Borderless window"); choice(it, "windowed", "Window");
    static const int k_mode_of[4] = {3, 0, 1, 2};       /* gfx_options window_mode -> choice */
    it->cur = k_mode_of[window_mode_now() & 3];
    it = item_add(K_SIZE, TAB_DISPLAY, "Window size", 0);
    static const int k_sizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    it->cur = -1;
    for (int i = 0; i < 5; i++) {
        char v[32], l[32];
        snprintf(v, sizeof v, "%dx%d", k_sizes[i][0], k_sizes[i][1]); snprintf(l, sizeof l, "%d x %d", k_sizes[i][0], k_sizes[i][1]);
        choice(it, v, l);
        if (go->width == k_sizes[i][0] && go->height == k_sizes[i][1]) it->cur = i;
    }
    if (it->cur < 0) { char v[32], l[32]; snprintf(v, sizeof v, "%dx%d", go->width, go->height); snprintf(l, sizeof l, "%d x %d", go->width, go->height); choice(it, v, l); it->cur = it->n - 1; }
    it = item_add(K_VSYNC, TAB_DISPLAY, "Vertical sync", 0);
    choice(it, "clock", "Game clock (lowest latency)"); choice(it, "on", "On"); choice(it, "off", "Off"); choice(it, "mailbox", "Mailbox");
    for (int i = 1; i < 4; i++) if (!nvn_vk_present_offered(i)) snprintf(it->ch[i].label + strlen(it->ch[i].label), sizeof it->ch[0].label - strlen(it->ch[i].label), " (not offered here: as the game clock)");
    it->cur = go->present & 3;
    it = item_add(K_PERF, TAB_DISPLAY, "Performance overlay", 0);
    choice(it, "off", "Off"); choice(it, "fps", "FPS and frame time"); choice(it, "detailed", "Detailed (resolution, CPU, GPU, memory)");
    it->cur = display_perf_overlay() > 2 ? 2 : display_perf_overlay();
    it = item_add(K_CORNER, TAB_DISPLAY, "Overlay position", 0);
    choice(it, "top_left", "Top left"); choice(it, "top_right", "Top right"); choice(it, "bottom_left", "Bottom left"); choice(it, "bottom_right", "Bottom right");
    choice(it, "top_center", "Top center"); choice(it, "bottom_center", "Bottom center");
    it->cur = (go->overlay_corner >= 1 && go->overlay_corner <= 6 ? go->overlay_corner : 1) - 1;
    /* ---- Graphics */
    it = item_add(K_GPU, TAB_GRAPHICS, "GPU", 1);
    char names[8][128];
    int used = -1, n = nvn_vk_gpus(names, 8, &used);
    choice(it, "auto", "Automatic (dedicated first)");
    for (int i = 0; i < n; i++) choice(it, names[i], names[i]);
    it->cur = 0;
    if (go->gpu[0]) {
        for (int i = 0; i < n && it->cur == 0; i++) { char a[128], b[64]; snprintf(a, sizeof a, "%s", names[i]); snprintf(b, sizeof b, "%s", go->gpu);
            for (char *q = a; *q; q++) *q = (char)tolower((unsigned char)*q);
            for (char *q = b; *q; q++) *q = (char)tolower((unsigned char)*q);
            if (strstr(a, b)) it->cur = i + 1; }
        if (it->cur == 0 && strcasecmp(go->gpu, "auto")) { choice(it, go->gpu, go->gpu); it->cur = it->n - 1; }
    }
    it = item_add(K_ANISO, TAB_GRAPHICS, "Anisotropic filtering", 1);
    choice(it, "game", "The game's own"); choice(it, "2", "2x"); choice(it, "4", "4x"); choice(it, "8", "8x"); choice(it, "16", "16x");
    it->cur = go->aniso == 2 ? 1 : go->aniso == 4 ? 2 : go->aniso == 8 ? 3 : go->aniso == 16 ? 4 : 0;
    it = item_add(K_TFILTER, TAB_GRAPHICS, "Texture filtering", 1);
    choice(it, "game", "The game's own"); choice(it, "bilinear", "Bilinear"); choice(it, "trilinear", "Trilinear");
    it->cur = go->tfilter;
    /* ---- System */
    it = item_add(K_LANG, TAB_SYSTEM, "Game language", 1);
    static const char *const k_lang[][2] = {{"system", "System language"}, {"en-US", "English (US)"}, {"en-GB", "English (UK)"},
        {"es", "Spanish (Spain)"}, {"es-419", "Spanish (Latin America)"}, {"fr", "French"}, {"fr-CA", "French (Canada)"}, {"de", "German"},
        {"it", "Italian"}, {"nl", "Dutch"}, {"ru", "Russian"}, {"ja", "Japanese"}, {"ko", "Korean"}, {"zh-Hans", "Chinese (Simplified)"},
        {"zh-Hant", "Chinese (Traditional)"}};
    for (size_t i = 0; i < sizeof k_lang / sizeof k_lang[0]; i++) choice(it, k_lang[i][0], k_lang[i][1]);
    {   const char *env = getenv("SMO_LANGUAGE");           /* what the game was started with: the environment wins over the file */
        const char *want = env && env[0] ? env : go->language[0] ? go->language : "system";
        it->cur = choice_of(it, want);
        if (it->cur < 0) it->cur = 0;
        snprintf(it->ch[0].label, sizeof it->ch[0].label, "System language (now %s)", oe_language());
    }
    it = item_add(K_PATH_SAVE, TAB_SYSTEM, "Saves", 0);
    snprintf(buf, sizeof buf, "%s/save", fs_user_root()); path_of(it->path, buf);
    it = item_add(K_PATH_ALBUM, TAB_SYSTEM, "Captures", 0);
    snprintf(buf, sizeof buf, "%s/album", fs_user_root()); path_of(it->path, buf);
    it = item_add(K_PATH_CONFIG, TAB_SYSTEM, "Settings", 0);
    snprintf(buf, sizeof buf, "%s", go->path);
    { char *sl = strrchr(buf, '/'); if (sl) *sl = 0; }
    path_of(it->path, buf);
    it = item_add(K_PATH_LOGS, TAB_SYSTEM, "Logs", 0);
    snprintf(buf, sizeof buf, "%s/../logs", plat_exe_dir());
    if (access(buf, F_OK) == 0) path_of(it->path, buf); else snprintf(it->path, sizeof it->path, "the terminal that started the game");
    /* ---- Mods: every mod found (mods.c: the order of config/mods.ini), then the folder */
    {   int mods_count(void); const char *mods_name(int); int mods_is_on(int); int mods_usable(int, int *); int mods_has_files_only(int); const char *mods_dir(void);
        int nm = mods_count();
        if (nm > MODS_MAX) nm = MODS_MAX;
        for (int i = 0; i < nm; i++) {
            int files = 0, usable = mods_usable(i, &files);
            it = item_add(K_MOD0 + i, TAB_MODS, mods_name(i), 1);
            if (!usable) { choice(it, "x", "Code patches for emulators: not supported"); it->cur = 0; continue; }
            if (mods_has_files_only(i)) snprintf(buf, sizeof buf, "On (%d file%s)", files, files == 1 ? "" : "s"); else snprintf(buf, sizeof buf, "On");
            choice(it, "off", "Off"); choice(it, "on", buf);          /* a switch: right = on */
            it->cur = mods_is_on(i) ? 1 : 0;
        }
        it = item_add(K_PATH_MODS, TAB_MODS, "Mods folder", 0);
        path_of(it->path, mods_dir());
    }
    /* what the game runs with is what the file had at the first opening; a value saved since for the next start is shown */
    static char run[K_MOD0 + MODS_MAX][128], next[K_MOD0 + MODS_MAX][128];
    static int have_run;
    for (int i = 0; i < g_nitems; i++) {
        Item *e = &g_items[i];
        if (!have_run && e->n) snprintf(run[e->key], sizeof run[0], "%s", e->ch[e->cur].value);
        e->start = e->cur;
        if (e->restart && e->n) {
            int r = choice_of(e, run[e->key]), x = next[e->key][0] ? choice_of(e, next[e->key]) : -1;
            if (r >= 0) e->start = r;
            if (x >= 0) e->cur = x;
        }
        e->saved = e->cur;
    }
    have_run = 1;
    g_next = next;
}
static int item_is_path(const Item *it) { return it->key >= K_PATH_SAVE && it->key <= K_PATH_MODS; }
static int item_enabled(const Item *it) {
    if (it->key == K_SIZE) {                              /* only in the window modes: the mode shown (it may not be applied yet) */
        for (int i = 0; i < g_nitems; i++) if (g_items[i].key == K_MODE) return g_items[i].cur >= 2;
        return window_mode_now() == 0 || window_mode_now() == 3;
    }
    if (it->key >= K_MOD0) return it->n > 1;                /* a mod with code patches only: shown, not switchable */
    {   int smo_mod_feature(const char *);                 /* options of a mod: only while it is on (this run) */
        if (it->key == K_RES) return smo_mod_feature("hd_resolution");
        if (it->key == K_ANISO) return smo_mod_feature("anisotropic_filtering");
        if (it->key == K_TFILTER) return smo_mod_feature("texture_filtering");
    }
    return !item_is_path(it);
}
static int unsaved(void) {                                /* changes shown, not applied yet (+ applies them) */
    for (int i = 0; i < g_nitems; i++) if (g_items[i].cur != g_items[i].saved) return 1;
    return 0;
}
static int restart_pending(void) {                        /* changes saved that the game takes when it starts again */
    for (int i = 0; i < g_nitems; i++) if (g_items[i].restart && g_items[i].saved != g_items[i].start) return 1;
    return 0;
}
/* smo.env of the game folder (tools/make_game_folder.py): the line of SMO_LANGUAGE replaced (or taken out for the system's) */
static void env_file_language(const char *code) {
    char path[PATH_MAX], tmp[PATH_MAX + 8], line[512];
    snprintf(path, sizeof path, "%s/../smo.env", plat_exe_dir());
    FILE *in = fopen(path, "rb");
    if (!in) return;                                      /* not a game folder (a build of the repository): graphics.ini is enough */
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return; }
    while (fgets(line, sizeof line, in)) if (!strstr(line, "SMO_LANGUAGE=")) fputs(line, out);
    if (code) fprintf(out, "export SMO_LANGUAGE=%s\n", code);
    fclose(in);
    if (fclose(out) == 0 && rename(tmp, path) == 0) printf("settings menu: %s: SMO_LANGUAGE %s\n", path, code ? code : "taken out (the system's)");
}

/* a choice made: written, and applied when the running game can take it */
static void apply(Item *it) {
    GfxOptions *go = gfx_options_edit();
    const char *v = it->ch[it->cur].value;
    switch (it->key) {
    case K_RES: gfx_options_save("resolution", v); break;
    default:
        if (it->key >= K_MOD0 && it->n > 1) { void mods_set(int, int); mods_set(it->key - K_MOD0, it->cur == 1); }
        break;
    case K_MODE: {
        static const int k_mode[4] = {1, 2, 3, 0};        /* choice -> window_mode */
        go->window_mode = k_mode[it->cur & 3];
        gfx_options_save("window", v);
        display_set_window(go->window_mode, go->width, go->height);
        break;
    }
    case K_SIZE: {
        int w = 0, h = 0;
        if (sscanf(v, "%dx%d", &w, &h) == 2) {
            char b[16];
            go->width = w; go->height = h;
            snprintf(b, sizeof b, "%d", w); gfx_options_save("width", b);
            snprintf(b, sizeof b, "%d", h); gfx_options_save("height", b);
            display_set_window(go->window_mode, w, h);
        }
        break;
    }
    case K_VSYNC: go->present = it->cur; gfx_options_save("vsync", v); nvn_vk_set_vsync(it->cur); break;
    case K_PERF: go->perf_overlay = it->cur; gfx_options_save("performance_overlay", v); display_set_perf_overlay(it->cur, 0); break;
    case K_CORNER: go->overlay_corner = it->cur + 1; gfx_options_save("overlay_corner", v); display_set_perf_overlay(display_perf_overlay(), it->cur + 1); break;
    case K_GPU: gfx_options_save("gpu", v); break;
    case K_ANISO: gfx_options_save("anisotropic_filtering", v); break;
    case K_TFILTER: gfx_options_save("texture_filtering", v); break;
    case K_LANG: {
        int sys = !strcmp(v, "system");
        gfx_options_save("language", v);
        env_file_language(sys ? NULL : v);
        if (sys) unsetenv("SMO_LANGUAGE"); else setenv("SMO_LANGUAGE", v, 1);     /* for the restart: exec keeps the environment */
        break;
    }
    }
}

/* A: every change shown is written and, when the running game can take it, applied */
static void apply_all(void) {
    for (int i = 0; i < g_nitems; i++) {
        Item *it = &g_items[i];
        if (it->cur == it->saved) continue;
        apply(it);
        it->saved = it->cur;
        if (!it->restart) it->start = it->cur;
        else snprintf(g_next[it->key], 128, "%s", it->ch[it->cur].value);
    }
}
/* ---------------------------------------------------------------- the picture */
static struct {
    int tried, ok;
    UiFont *title, *msg, *dev;
    UiImage cursor, corner, corner_sh, arrow, bg, frame;
    uint16_t footer[256], footer_mods[256];
    int tab, sel;                                         /* the tab shown; the row chosen in it */
} S;
static const float k_title = 0.62f, k_tab_scale = 0.50f;  /* layout units per font pixel (TitleFont60, MessageFont38) */
static float k_msg = 0.62f;

static int menu_load(void) {
    if (S.tried) return S.ok;
    S.tried = 1;
    size_t fn = 0, sn = 0;
    uint8_t *fonts = ui_lang_arc("LayoutData/FontData.szs", &fn), *sys = ui_lang_arc("MessageData/SystemMessage.szs", &sn);
    if (fonts) { S.title = ui_font_in(fonts, fn, "TitleFont60.bffnt"); S.msg = ui_font_in(fonts, fn, "MessageFont38.bffnt"); S.dev = ui_font_in(fonts, fn, "DeviceFont80.bffnt"); }
    ui_u16(S.footer, "\uE0E4\uE0E5 Tab   \uE101 Select   \uE0E1 Close   \uE0EF Apply", 256);     /* the game's footer icons (DeviceFont) */
    ui_u16(S.footer_mods, "\uE0E4\uE0E5 Tab   \uE101 Select   \uE0E0 On / Off   \uE0E1 Close   \uE0EF Apply", 256);      /* the Mods tab: A switches a mod */
    S.ok = S.title && S.msg && S.dev &&
           ui_layout_tex("CursorParts", "CmCursor00^w", &S.cursor) && ui_layout_tex("SelectMenuParts", "CmSelectCorner^s", &S.corner) &&
           ui_layout_tex("SelectMenuParts", "CmSelectCornerSh^s", &S.corner_sh) && ui_layout_tex("CursorArrowParts", "CmArrow^s", &S.arrow);
    if (S.ok) { UiFontInfo t, m; ui_font_info(S.title, &t); ui_font_info(S.msg, &m); k_msg = 18.0f / (float)(m.ascent > 0 ? m.ascent : 30); (void)t; }
    printf("settings menu: %s (language folder %s)\n", S.ok ? "ready" : "NOT available (files of the game missing, see above)", ui_lang_folder());
    free(fonts); free(sys);
    return S.ok;
}
static int tab_rows(int tab, Item **rows) {               /* the rows of a tab */
    int n = 0;
    for (int i = 0; i < g_nitems; i++) if (g_items[i].tab == tab) rows[n++] = &g_items[i];
    return n;
}
static void text_u(UiImage *d, UiXf xf, const UiFont *f, const char *s, float x, float y, float sc, uint32_t rgba, int shadow) {
    uint16_t u[512];
    ui_u16(u, s, 512);
    if (shadow) ui_shadow_text(d, xf, f, NULL, u, x, y, sc, rgba); else ui_text(d, xf, f, NULL, u, -1, x, y, sc, rgba);
}
static float text_w(const UiFont *f, const char *s, float sc) { uint16_t u[512]; ui_u16(u, s, 512); return ui_text(NULL, ui_xf_move(0, 0), f, NULL, u, -1, 0, 0, sc, 0); }
/* a path shortened from the left ("...") to fit maxw */
static void fit_path(char *out, size_t cap, const char *p, const UiFont *f, float sc, float maxw) {
    snprintf(out, cap, "%s", p);
    size_t n = strlen(p), skip = 0;
    while (text_w(f, out, sc) > maxw && skip + 8 < n) { skip += 2; snprintf(out, cap, "...%s", p + skip); }
}
/* The list, with the padding of the game's pause and title menus (measured on their captures, as the amiibo menu): rows 56
 * apart, the chosen row in a white bar 56 high with rounded corners and a soft shadow, the cap
 * cursor 37 units into it, the label 74 units in, in TitleFont60: 29.3 units for the capitals in the chosen row, 21.3 in
 * the others. The bar spans the menu (1150 units) so the value fits on its right, in MessageFont38. The game turns its
 * lists 3 degrees; these rows are not turned: over their width that would lift each value a whole row above its label. */
#define LIST_X (-590.0f)
#define LIST_Y 126.0f
#define ROW_STEP 56.0f
#define ITEM_X 4.0f
#define BAR_W 1150.0f
#define SEL_SCALE (29.3f / 46.0f)
#define ROW_SCALE (21.3f / 46.0f)

/* a rounded bar (a circle when it is as wide as high): rows of the menu's units, each as wide as the round ends allow */
static void pill(UiImage *d, UiXf xf, float x0, float y0, float x1, float y1, uint32_t rgba) {
    float r = (y1 - y0) * 0.5f, cy = (y0 + y1) * 0.5f, step = 0.5f;
    for (float y = y0; y < y1; y += step) {
        float dy = (y + step * 0.5f) - cy, cut = r - sqrtf(r * r - dy * dy > 0 ? r * r - dy * dy : 0);
        if (x0 + cut < x1 - cut) ui_draw(d, xf, x0 + cut, y, x1 - cut, y + step, NULL, 0, 0, 0, 0, rgba);
    }
}
static void menu_draw(void) {
    UiImage *d = &S.frame;
    if (d->w != S.bg.w || d->h != S.bg.h || !d->px) { ui_image_free(d); d->w = S.bg.w; d->h = S.bg.h; d->ch = 4; d->px = malloc((size_t)d->w * d->h * 4); }
    if (!d->px || !S.bg.px) return;
    memcpy(d->px, S.bg.px, (size_t)d->w * d->h * 4);
    UiXf scr = ui_xf_screen(d->w, d->h);
    UiFontInfo mi; ui_font_info(S.msg, &mi);
    /* the title and the tabs */
    text_u(d, scr, S.title, "Settings", -590.0f, 268.0f, k_title, 0xffffffff, 1);
    float tx = -590.0f;
    for (int t = 0; t < TABS; t++) {
        float w = text_w(S.title, k_tab[t], k_tab_scale), y = 205.0f;
        if (t == S.tab) {
            ui_window(d, scr, tx - 14.0f + 2.0f, y - 14.0f - 4.0f, tx + w + 14.0f + 3.0f, y + 30.0f - 2.0f, &S.corner_sh, 12.0f, 0x00000050);
            ui_window(d, scr, tx - 14.0f, y - 14.0f, tx + w + 14.0f, y + 30.0f, &S.corner, 6.0f, 0xffffffff);
            text_u(d, scr, S.title, k_tab[t], tx, y, k_tab_scale, 0x4d4241ff, 0);
        } else text_u(d, scr, S.title, k_tab[t], tx, y, k_tab_scale, 0xffffffcc, 1);
        tx += w + 56.0f;
    }
    ui_draw(d, scr, -590.0f, 176.0f, 590.0f, 178.0f, NULL, 0, 0, 0, 0, 0xffffff70);      /* a thin line under the tabs */
    /* the rows */
    Item *rows[16 + 1 + MODS_MAX];
    int n = tab_rows(S.tab, rows);
    if (S.sel >= n) S.sel = n - 1;
    UiXf lx = ui_xf_mul(scr, ui_xf_move(LIST_X, LIST_Y));
    UiFontInfo ti; ui_font_info(S.title, &ti);
    float vsc_row = k_msg * 1.05f, rx = ITEM_X + BAR_W - 26.0f;
    int first = S.sel >= VISIBLE_ROWS ? S.sel - VISIBLE_ROWS + 1 : 0, shown = n - first < VISIBLE_ROWS ? n - first : VISIBLE_ROWS;      /* a long tab scrolls with the choice */
    for (int r = first; r < first + shown; r++) {
        Item *it = rows[r];
        int chosen = r == S.sel, enabled = item_enabled(it), path = item_is_path(it);
        int arrows = enabled && !path;
        float cy = -ROW_STEP * (float)(r - first), lsc = chosen ? SEL_SCALE : ROW_SCALE;
        float lbase = cy + 4.0f - (float)ti.ascent * 0.33f * lsc;
        char label[160], value[PATH_MAX + 8];
        snprintf(label, sizeof label, "%s", it->label);      /* (the game's fonts have no '*' or ';': plain words below) */
        float vsc = path ? vsc_row * 0.82f : vsc_row;
        if (path) fit_path(value, sizeof value, it->path, S.msg, vsc, 660.0f);
        else snprintf(value, sizeof value, "%s%s", it->ch[it->cur].label, it->cur != it->saved ? " (not applied)" : it->restart && it->saved != it->start ? " (after restart)" : "");
        if (!enabled && !path) snprintf(value, sizeof value, "%s", it->key == K_SIZE ? "Only in the window modes" : it->key == K_RES ? "Needs the HD resolution mod (Mods tab)" :
                                                                   it->key == K_ANISO ? "Needs the Anisotropic filtering mod (Mods tab)" : it->key == K_TFILTER ? "Needs the Texture filtering mod (Mods tab)" : it->ch[it->cur].label);      /* (a mod with code patches only says so) */
        float vbase = cy + 4.0f - (float)mi.ascent * 0.33f * vsc;
        uint32_t fg = chosen ? 0x4d4241ff : enabled || path ? 0xffffffff : 0xffffff80;
        if (chosen) {
            ui_window(d, lx, ITEM_X + 2.0f, cy - 28.0f - 5.0f, ITEM_X + BAR_W + 3.0f, cy + 28.0f - 3.0f, &S.corner_sh, 12.0f, 0x00000050);
            ui_window(d, lx, ITEM_X, cy - 28.0f, ITEM_X + BAR_W, cy + 28.0f, &S.corner, 6.0f, 0xffffffff);
            float cw = (float)S.cursor.w * 0.7f, ch = (float)S.cursor.h * 0.7f, ccx = ITEM_X + 37.0f, ccy = cy - 2.0f;
            ui_draw(d, lx, ccx - cw * 0.5f + 1.5f, ccy - ch * 0.5f - 2.0f, ccx + cw * 0.5f + 1.5f, ccy + ch * 0.5f - 2.0f, &S.cursor, 0, 0, (float)S.cursor.w, (float)S.cursor.h, 0x00000030);
            ui_draw(d, lx, ccx - cw * 0.5f, ccy - ch * 0.5f, ccx + cw * 0.5f, ccy + ch * 0.5f, &S.cursor, 0, 0, (float)S.cursor.w, (float)S.cursor.h, 0xffffffff);
        }
        text_u(d, lx, S.title, label, ITEM_X + 74.0f, lbase, lsc, fg, !chosen);
        if (it->key >= K_MOD0 && it->n == 2) {           /* a mod: a switch (right and gold = on), its state beside it */
            int on = it->cur == 1;
            float sw = 66.0f, sh = 32.0f, x1 = rx + 4.0f, x0 = x1 - sw, r = sh * 0.5f;
            uint32_t track = on ? 0xbaa000ff : chosen ? 0xb8b2acff : 0x8c8681c0;
            pill(d, lx, x0, cy - r, x1, cy + r, track);
            float kx = on ? x1 - r : x0 + r, kr = r - 4.0f;
            pill(d, lx, kx - kr + 1.0f, cy - kr - 2.0f, kx + kr + 1.0f, cy + kr - 2.0f, 0x00000038);      /* the knob's shadow */
            pill(d, lx, kx - kr, cy - kr, kx + kr, cy + kr, 0xffffffff);
            char st[96];
            int mods_has_files_only(int);
            snprintf(st, sizeof st, "%s", it->cur != it->saved ? "not applied" : it->restart && it->saved != it->start ? "after restart" : "");
            if (!st[0] && on && mods_has_files_only(it->key - K_MOD0)) snprintf(st, sizeof st, "%s", it->ch[1].label + 3);      /* "(12 files)" */
            if (st[0]) { float w = text_w(S.msg, st, vsc); text_u(d, lx, S.msg, st, x0 - 16.0f - w, vbase, vsc, fg, !chosen); }
            continue;
        }
        float vw = text_w(S.msg, value, vsc), vx = rx - vw - (arrows ? 30.0f : 0.0f);
        text_u(d, lx, S.msg, value, vx, vbase, vsc, fg, !chosen);
        if (arrows) {                                     /* the arrows: left when there is a choice before, right after */
            uint32_t ac = chosen ? 0x4d4241ff : 0xffffffaa;
            float ay = cy + 2.0f;
            if (it->cur > 0) { UiXf a = ui_xf_mul(lx, ui_xf_mul(ui_xf_move(vx - 18.0f, ay), ui_xf_rotate(180.0f))); ui_draw(d, a, -8.0f, -8.0f, 8.0f, 8.0f, &S.arrow, 0, 0, (float)S.arrow.w, (float)S.arrow.h, ac); }
            if (it->cur < it->n - 1) { UiXf a = ui_xf_mul(lx, ui_xf_move(rx - 8.0f, ay)); ui_draw(d, a, -8.0f, -8.0f, 8.0f, 8.0f, &S.arrow, 0, 0, (float)S.arrow.w, (float)S.arrow.h, ac); }
        }
    }
    /* under the rows, while there is something for A to do: what it does now */
    const char *todo = unsaved() ? "\uE0EF Apply the changes" :
                       restart_pending() ? "Some changes apply after a restart.   \uE0EF Restart now (progress since the last save is lost)" : NULL;
    if (!todo && n && S.sel < n && rows[S.sel]->key >= K_MOD0) {      /* a mod chosen: what it does (its mod.ini) */
        const char *mods_desc(int); const char *dsc = mods_desc(rows[S.sel]->key - K_MOD0);
        if (dsc && dsc[0]) todo = dsc;
    }
    if (todo) {
        uint16_t t[160];
        ui_u16(t, todo, 160);
        float y = LIST_Y - ROW_STEP * (float)shown - 30.0f, sc = k_msg * 1.05f;
        ui_shadow_text(d, scr, S.msg, S.dev, t, LIST_X + ITEM_X + 74.0f, y - (float)mi.ascent * 0.33f * sc, sc, 0xffffffff);
    }
    /* the footer, as the game's menus: white, a gold line on top, the guide on the right in gold */
    ui_draw(d, scr, -640.0f, -360.0f, 640.0f, -302.0f, NULL, 0, 0, 0, 0, 0xffffffff);
    ui_draw(d, scr, -640.0f, -306.0f, 640.0f, -302.0f, NULL, 0, 0, 0, 0, 0xbaa000ff);
    const uint16_t *foot = S.tab == TAB_MODS ? S.footer_mods : S.footer;      /* A only where it does something: the Mods tab */
    float fsc = 16.8f / 32.0f, fw = ui_text(NULL, scr, S.msg, S.dev, foot, -1, 0, 0, fsc, 0);
    ui_text(d, scr, S.msg, S.dev, foot, -1, 578.0f - fw, -339.0f, fsc, 0xbaa000ff);
}

/* ---------------------------------------------------------------- the menu */
static volatile int32_t g_request;
void settings_menu_request(void) { plat_xchg(&g_request, 1); }
int settings_menu_wanted(void) { return g_request != 0; }

static void save_shot(int k) {
    char p[PATH_MAX + 64];
    static int opened;
    if (k == 0) opened++;
    snprintf(p, sizeof p, "%s/settings_menu_%d_%d.png", getenv("SMO_SETTINGS_SHOT_DIR"), opened, k);
    if (tex_write_png(p, S.frame.px, S.frame.w, S.frame.h) == 0) printf("settings menu: picture saved to %s\n", p);
}

/* nvn_gl.c queue_present, the game's main thread, after a presented frame: the menu, until it is closed. pj = that frame. */
void settings_menu_run(void *pj) {
    plat_xchg(&g_request, 0);
    UiMenuJob job;
    memset(&job, 0, sizeof job);
    job.present = pj;
    job.op = UI_MENU_READ;
    if (!menu_load()) return;
    if (!nvn_menu_job(&job) || !job.out) { printf("settings menu: the frame could not be read back (the menu needs the Vulkan renderer)\n"); return; }
    int cw = job.out_w > 1920 ? 1920 : job.out_w, ch = (int)((long long)job.out_h * cw / job.out_w);
    ui_backdrop(job.out, job.out_w, job.out_h, &S.bg, cw, ch);
    free(job.out);
    items_load();
    S.sel = 0;
    display_menu_hold(1);
    menu_draw();
    printf("settings menu: open at frame %lld\n", nvn_front_frame_next());
    fflush(stdout);
    uint64_t prev = hid_menu_buttons();                    /* what is held when it opens counts only once let go */
    hid_menu_key(0); hid_menu_key(1); hid_menu_home();
    int held = 0, repeat = 0, changed = 1, restart = 0, shots = 0, want_shots = getenv("SMO_SETTINGS_SHOT_DIR") != NULL;
    for (;;) {
        if (display_closing()) break;
        uint64_t b = hid_menu_buttons(), press = b & ~prev;
        prev = b;
        int up = (b & (BIT(B_UP) | BIT(B_SL_UP))) != 0, down = (b & (BIT(B_DOWN) | BIT(B_SL_DOWN))) != 0;
        int left = (b & (BIT(B_LEFT) | BIT(B_SL_LEFT))) != 0, right = (b & (BIT(B_RIGHT) | BIT(B_SL_RIGHT))) != 0;
        int vdir = up ? -1 : down ? 1 : 0, hdir = left ? -1 : right ? 1 : 0, vstep = 0, hstep = 0;
        uint64_t dirs = BIT(B_UP) | BIT(B_SL_UP) | BIT(B_DOWN) | BIT(B_SL_DOWN) | BIT(B_LEFT) | BIT(B_SL_LEFT) | BIT(B_RIGHT) | BIT(B_SL_RIGHT);
        if ((vdir || hdir) && (press & dirs)) { vstep = vdir; hstep = hdir; held = 0; repeat = 0; }
        else if ((vdir || hdir) && ++held >= 30 && ++repeat % 5 == 1) { vstep = vdir; hstep = hdir; }   /* the pause menu's key repeat */
        if (!vdir && !hdir) held = repeat = 0;
        Item *rows[16 + 1 + MODS_MAX];
        int n = tab_rows(S.tab, rows);
        if (vstep) { S.sel = (S.sel + vstep + n) % n; changed = 1; }
        if (press & (BIT(B_L) | BIT(B_ZL))) { S.tab = (S.tab + TABS - 1) % TABS; S.sel = 0; changed = 1; }
        if (press & (BIT(B_R) | BIT(B_ZR))) { S.tab = (S.tab + 1) % TABS; S.sel = 0; changed = 1; }
        n = tab_rows(S.tab, rows);
        if (S.sel >= n) S.sel = n - 1;
        Item *it = rows[S.sel];
        if (hstep && item_enabled(it)) {                   /* left / right only show another choice */
            int c = it->cur + hstep;
            if (c >= 0 && c < it->n) { it->cur = c; changed = 1; }
        }
        if ((press & BIT(B_A)) && it->key >= K_MOD0 && it->n == 2) { it->cur ^= 1; changed = 1; }      /* A: a mod's switch on / off */
        if ((press & BIT(B_PLUS)) || hid_menu_key(1)) {     /* + (Enter) applies what is shown; with nothing left, restarts if needed */
            if (unsaved()) { apply_all(); changed = 1; }
            else if (restart_pending()) { restart = 1; break; }
        }
        if ((press & BIT(B_B)) || hid_menu_key(0) || hid_menu_home() || g_request) { plat_xchg(&g_request, 0); break; }
        if (changed) { menu_draw(); if (want_shots && shots < 40) save_shot(shots++); }
        job.op = UI_MENU_SHOW; job.rgba = S.frame.px; job.w = S.frame.w; job.h = S.frame.h; job.changed = changed;
        if (!nvn_menu_job(&job)) { printf("settings menu: the picture could not be shown; closed\n"); break; }
        changed = 0;
        display_menu_wait();
    }
    int dropped = 0;                                       /* closed without A: what was only shown is dropped */
    for (int i = 0; i < g_nitems; i++) if (g_items[i].cur != g_items[i].saved) { g_items[i].cur = g_items[i].saved; dropped++; }
    if (dropped) printf("settings menu: %d change(s) not applied, dropped\n", dropped);
    job.op = UI_MENU_END;
    nvn_menu_job(&job);
    display_menu_hold(0);
    hid_menu_closed();
    printf("settings menu: closed at frame %lld%s\n", nvn_front_frame_next(), restart ? ": restarting" : "");
    fflush(stdout);
    if (restart) smo_restart();
}

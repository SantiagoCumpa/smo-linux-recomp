/* Graphics options: config\graphics.ini.
 *
 * Path: SMO_GRAPHICS_CONFIG=<file> when set; else graphics.ini beside the file SMO_CONFIG names (SMO_CONFIG is the
 * path of controllers.ini, nnpad_sdl.c); else <exe folder>\..\config\graphics.ini. Written with the defaults and their
 * documentation when it does not exist. Read ONCE at start (the window, the device and the swapchain are made from it).
 * Environment switches of the test scripts (SMO_RENDERER, SMO_VK_DEVICE, SMO_VK_PRESENT) win over the file.
 * The controllers.ini reader of nnpad_sdl.c is built around its device sections and is not reusable here: this file has
 * its own 30-line reader (key = value, ';' or '#' comments, section headers ignored). */
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_options.h"

static const char *const k_default_ini =
"; graphics.ini: display and rendering options of the native build (read once at start by gfx_options.c).\n"
"; Delete the file to get it written again with these defaults. A line starting with ';' is a comment.\n"
"; Every value below is the default; the defaults draw exactly what the game draws on the console.\n"
"\n"
"; renderer = vulkan\n"
";   Vulkan is the only renderer of the Linux build.\n"
"renderer = vulkan\n"
"\n"
"; window = borderless_window | borderless_fullscreen | windowed | fullscreen\n"
";   borderless_window:     a window without a frame or title bar, the size below, centred by the compositor.\n"
";                          Double-click it for borderless_fullscreen and back.\n"
";   borderless_fullscreen: no frame, covers the monitor the window is on (the desktop keeps its resolution).\n"
";   windowed:              a normal window with a title bar, the client size below.\n"
";   fullscreen:            as borderless_fullscreen, and always on top. (Exclusive fullscreen with a mode change is\n"
";                          not implemented.)\n"
";   F11 switches between covering the monitor and the window while the game runs.\n"
"window = borderless_window\n"
"width = 1280\n"
"height = 720\n"
"\n"
"; unfocused = run | pause\n"
";   What the game does while its window is not the active window (another program has the focus) or is minimised.\n"
";   run:   it keeps running; pads and the keyboard count only while the window is active.\n"
";   pause: it stops until the window is active again, as the console stops the game while the HOME menu is open.\n"
"unfocused = run\n"
"\n"
"; vsync = clock | on | off | mailbox      (Vulkan)\n"
";   clock:   frames are shown at once (mailbox when the GPU offers it, else immediate) and the game is paced by the\n"
";            program's own 60 Hz clock. Lowest latency; can tear only when mailbox is missing.\n"
";   on:      the display's own vertical sync (FIFO). No tearing; on a display that is not 60 Hz the two clocks drift.\n"
";   off:     immediate presentation, tearing allowed. The game still runs at 60 frames per second.\n"
";   mailbox: mailbox or nothing (falls back to clock's choice with a message when the GPU does not offer it).\n"
"vsync = clock\n"
"\n"
"; gpu = auto | <number> | <part of the name>      (Vulkan)\n"
";   auto: a discrete GPU first, then an integrated one. The log lists the devices with their numbers:\n"
";   'nvn_vk: device 0: ...'. Examples: gpu = 1      gpu = Intel      gpu = RTX\n"
"gpu = auto\n"
"\n"
"; console_mode = docked | handheld\n"
";   What nn::oe::GetOperationMode answers. Leave it at docked: handheld is not measured on PC.\n"
"console_mode = docked\n"
"\n"
"; resolution_scale = 1.0\n"
";   Internal resolution scale, 0.25 to 1.0 (Vulkan only; OpenGL ignores it). 1.0 = the game's own (its picture is\n"
";   1600x900). 0.75 = 1200x675, 0.5 = 800x450: the main switch for a weak GPU. The whole picture is drawn smaller, the\n"
";   2D interface too (it gets softer), and is scaled to the window size.\n"
"resolution_scale = 1.0\n"
"\n"
"; anisotropic_filtering = game | 2 | 4 | 8 | 16      (Vulkan)\n"
";   game: each texture sampler keeps the value the game gave it. A number: at least that much on every sampler that\n"
";   filters between mip levels (sharper textures at flat angles; changes the picture, costs little on any GPU).\n"
"anisotropic_filtering = game\n"
"\n"
"; shader_wait = hold | skip      (Vulkan)\n"
";   What happens when a draw needs a shader or pipeline that is not ready yet (mostly the first time something is shown\n"
";   on this PC). hold: the frame waits for it (a late frame, a correct picture). skip: during play the draw is\n"
";   left out until it is ready (no late frame; the object or effect is missing for some frames). The log counts both.\n"
"shader_wait = hold\n"
"\n"
"; profile = custom | low\n"
";   custom: every key of this file as written. low: the set for a weak or integrated GPU: resolution_scale = 0.5,\n"
";   anisotropic_filtering = game, vsync = clock; those three keys of the file are then not used. The log says which\n"
";   profile is active.\n"
"profile = custom\n"
"\n"
"; frame_time_overlay = off | on\n"
";   on: the average frame time of the last second in the window title ('.. fps - 16.67 ms per frame') and as a\n"
";   'frame-time:' line in the log. (Bars drawn into the picture are not implemented yet.)\n"
"frame_time_overlay = off\n";

#define GFX_LOW_SCALE 0.5f
#define GFX_LOW_SCALE_TEXT "0.50"
static GfxOptions g_opt;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}
static void opt_path(char *out) {
    const char *e = getenv("SMO_GRAPHICS_CONFIG"), *c = getenv("SMO_CONFIG");
    char tmp[PATH_MAX], exe[PATH_MAX];
    if (e && e[0]) { snprintf(out, PATH_MAX, "%s", e); return; }
    if (c && c[0]) {
        snprintf(tmp, sizeof tmp, "%s", c);
        char *s = strrchr(tmp, '/');
        if (s) { *s = 0; snprintf(out, PATH_MAX, "%s/graphics.ini", tmp); } else snprintf(out, PATH_MAX, "graphics.ini");
        return;
    }
    snprintf(exe, sizeof exe, "%s", plat_exe_dir());
    snprintf(tmp, sizeof tmp, "%s/../config", exe);
    char real[PATH_MAX];
    if (realpath(tmp, real)) snprintf(out, PATH_MAX, "%s/graphics.ini", real);
    else snprintf(out, PATH_MAX, "%s/graphics.ini", tmp);
}
static void opt_load(void) {
    GfxOptions *o = &g_opt;
    int written = 0, unknown = 0;
    memset(o, 0, sizeof *o);
    o->width = 1280; o->height = 720; o->scale = 1.0f; o->window_mode = 3;
    opt_path(o->path);
    FILE *f = fopen(o->path, "rb");
    if (!f) {
        char b[PATH_MAX];
        snprintf(b, sizeof b, "%s", o->path);
        for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; plat_mkdir(b); *p = '/'; }
        if ((f = fopen(o->path, "wb")) != NULL) { fwrite(k_default_ini, 1, strlen(k_default_ini), f); fclose(f); written = 1; }
        f = fopen(o->path, "rb");
    }
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *eq, *key = line, *val;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[' || !(eq = strchr(line, '='))) continue;
        *eq = 0; val = eq + 1;
        trim(key); trim(val);
        if (!strcasecmp(key, "renderer")) o->renderer = !strcasecmp(val, "opengl") || !strcasecmp(val, "gl");
        else if (!strcasecmp(key, "window")) o->window_mode = !strcasecmp(val, "borderless_fullscreen") || !strcasecmp(val, "borderless") ? 1 : !strcasecmp(val, "fullscreen") ? 2 : !strcasecmp(val, "windowed") ? 0 : 3;
        else if (!strcasecmp(key, "width")) { int v = atoi(val); if (v >= 320 && v <= 16384) o->width = v; }
        else if (!strcasecmp(key, "height")) { int v = atoi(val); if (v >= 180 && v <= 16384) o->height = v; }
        else if (!strcasecmp(key, "vsync")) o->present = !strcasecmp(val, "on") ? 1 : !strcasecmp(val, "off") ? 2 : !strcasecmp(val, "mailbox") ? 3 : 0;
        else if (!strcasecmp(key, "gpu")) { if (strcasecmp(val, "auto")) snprintf(o->gpu, sizeof o->gpu, "%s", val); }
        else if (!strcasecmp(key, "console_mode")) o->handheld = !strcasecmp(val, "handheld");
        else if (!strcasecmp(key, "resolution_scale")) { float v = (float)atof(val); if (v > 0) o->scale = v; }
        else if (!strcasecmp(key, "anisotropic_filtering")) { int v = atoi(val); o->aniso = v == 2 || v == 4 || v == 8 || v == 16 ? v : 0; }
        else if (!strcasecmp(key, "frame_time_overlay")) o->overlay = !strcasecmp(val, "on") || !strcmp(val, "1");
        else if (!strcasecmp(key, "profile")) o->profile = !strcasecmp(val, "low");
        else if (!strcasecmp(key, "shader_wait")) o->shader_skip = !strcasecmp(val, "skip");
        else if (!strcasecmp(key, "unfocused")) o->pause_unfocused = !strcasecmp(val, "pause");
        else unknown++;
    }
    if (f) fclose(f);
    if (o->profile == 1) {       /* the low-end profile wins over the three keys it sets, wherever they stand in the file */
        o->scale = GFX_LOW_SCALE; o->aniso = 0; o->present = 0;
    }
    printf("graphics profile: %s\n", o->profile == 1 ? "low (weak or integrated GPU): resolution_scale=" GFX_LOW_SCALE_TEXT ", anisotropic_filtering=game, vsync=clock; these three keys of the file are not used"
                                                     : "custom (every key of graphics.ini as written; 'profile = low' is the set for a weak or integrated GPU)");
    static const char *const k_win[4] = {"windowed", "borderless_fullscreen", "fullscreen", "borderless_window"}, *const k_vs[4] = {"clock", "on", "off", "mailbox"};
    printf("graphics options: %s (%s): renderer=%s window=%s %dx%d unfocused=%s vsync=%s gpu=%s console_mode=%s resolution_scale=%.2f anisotropic_filtering=%d frame_time_overlay=%s%s\n", o->path,
           written ? "written with the defaults" : f ? "read" : "NOT readable, defaults used", o->renderer ? "opengl" : "vulkan", k_win[o->window_mode], o->width, o->height,
           o->pause_unfocused ? "pause" : "run", k_vs[o->present], o->gpu[0] ? o->gpu : "auto", o->handheld ? "handheld" : "docked", o->scale, o->aniso, o->overlay ? "on" : "off",
           unknown ? " (lines with an unknown key were ignored)" : "");
    if (o->scale != 1.0f && o->renderer) printf("graphics options: resolution_scale=%.2f is ignored by the OpenGL renderer (Vulkan only)\n", o->scale);
    if (o->scale != 1.0f && (o->scale < 0.25f || o->scale > 1.0f)) printf("graphics options: resolution_scale=%.2f is outside 0.25 .. 1.0: the nearest of the two is used\n", o->scale);
    if (o->renderer) printf("graphics options: renderer=opengl is not available on Linux: Vulkan is used\n");
    fflush(stdout);
}
const GfxOptions *gfx_options(void) {
    pthread_once(&g_once, opt_load);
    return &g_opt;
}

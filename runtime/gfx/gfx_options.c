/* Graphics options: config/graphics.ini.
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

#include "defaults_embed.h"                         /* build_exe.py: defaults/config/graphics.ini as k_default_graphics_ini */
#define k_default_ini k_default_graphics_ini

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
    o->width = 1280; o->height = 720; o->scale = 1.0f; o->window_mode = 1; o->overlay_corner = 1;
    float res_scale = 0;          /* from 'resolution': wins over resolution_scale wherever the two stand */
    int perf_set = 0, old_fps = -1;   /* fps_overlay of older files: a corner = the fps overlay there, unless performance_overlay says */
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
        else if (!strcasecmp(key, "resolution")) {
            static const struct { const char *name; float scale; } k_res[] = {
                {"720p", 0.8f}, {"900p", 1.0f}, {"1080p", 1.2f}, {"1440p", 1.6f}, {"2k", 1.6f}, {"2160p", 2.4f}, {"4k", 2.4f}};
            int found = 0;
            for (size_t i = 0; i < sizeof k_res / sizeof k_res[0]; i++) if (!strcasecmp(val, k_res[i].name)) { res_scale = k_res[i].scale; found = 1; }
            if (!found) unknown++;
        }
        else if (!strcasecmp(key, "screen_saver")) { }    /* older files: the screen now always stays on while the game runs */
        else if (!strcasecmp(key, "aspect")) o->stretch = !strcasecmp(val, "stretch");
        else if (!strcasecmp(key, "performance_overlay")) {
            o->perf_overlay = !strcasecmp(val, "fps") ? 1 : !strcasecmp(val, "detailed") ? 2 : 0;
            if (strcasecmp(val, "off") && !o->perf_overlay) unknown++;
            perf_set = 1;
        }
        else if (!strcasecmp(key, "overlay_corner")) {
            static const char *const k_pos[7] = {"", "top_left", "top_right", "bottom_left", "bottom_right", "top_center", "bottom_center"};
            int found = 0;
            for (int i = 1; i < 7; i++) if (!strcasecmp(val, k_pos[i])) found = i;
            if (found) o->overlay_corner = found; else unknown++;
        }
        else if (!strcasecmp(key, "language")) { if (strcasecmp(val, "system")) snprintf(o->language, sizeof o->language, "%s", val); }
        else if (!strcasecmp(key, "texture_filtering")) o->tfilter = !strcasecmp(val, "bilinear") ? 1 : !strcasecmp(val, "trilinear") ? 2 : 0;
        else if (!strcasecmp(key, "anisotropic_filtering")) { int v = atoi(val); o->aniso = v == 2 || v == 4 || v == 8 || v == 16 ? v : 0; }
        else if (!strcasecmp(key, "fps_overlay")) {
            static const char *const k_pos[5] = {"off", "top_left", "top_right", "bottom_left", "bottom_right"};
            int found = -1;
            for (int i = 0; i < 5; i++) if (!strcasecmp(val, k_pos[i])) found = i;
            if (!strcasecmp(val, "on")) found = 3;
            if (found < 0) unknown++; else old_fps = found;
        }
        else if (!strcasecmp(key, "frame_time_overlay")) o->overlay = !strcasecmp(val, "on") || !strcmp(val, "1");
        else if (!strcasecmp(key, "profile") || !strcasecmp(key, "unfocused")) {}      /* removed keys (older files): the game always pauses while unfocused, as the console's HOME menu does */
        else if (!strcasecmp(key, "shader_wait")) {}      /* removed: the "Asynchronous shaders" mod (defaults/mods/async-shaders) */
        else unknown++;
    }
    if (f) fclose(f);
    if (!perf_set && old_fps > 0) { o->perf_overlay = 1; o->overlay_corner = old_fps; }
    if (res_scale > 0) o->scale = res_scale;
    static const char *const k_win[4] = {"windowed", "borderless_fullscreen", "fullscreen", "borderless_window"}, *const k_vs[4] = {"clock", "on", "off", "mailbox"};
    printf("graphics options: %s (%s): renderer=%s window=%s %dx%d vsync=%s gpu=%s console_mode=%s resolution_scale=%.2f anisotropic_filtering=%d frame_time_overlay=%s performance_overlay=%s overlay_corner=%d language=%s%s\n", o->path,
           written ? "written with the defaults" : f ? "read" : "NOT readable, defaults used", o->renderer ? "opengl" : "vulkan", k_win[o->window_mode], o->width, o->height,
           k_vs[o->present], o->gpu[0] ? o->gpu : "auto", o->handheld ? "handheld" : "docked", o->scale, o->aniso, o->overlay ? "on" : "off",
           o->perf_overlay == 2 ? "detailed" : o->perf_overlay ? "fps" : "off", o->overlay_corner, o->language[0] ? o->language : "system", unknown ? " (lines with an unknown key were ignored)" : "");
    if (o->scale != 1.0f && o->renderer) printf("graphics options: resolution_scale=%.2f is ignored by the OpenGL renderer (Vulkan only)\n", o->scale);
    if (o->scale != 1.0f && (o->scale < 0.25f || o->scale > 2.4f)) printf("graphics options: resolution_scale=%.2f is outside 0.25 .. 2.4: the nearest of the two is used\n", o->scale);
    if (o->renderer) printf("graphics options: renderer=opengl is not available on Linux: Vulkan is used\n");
    fflush(stdout);
}
const GfxOptions *gfx_options(void) {
    pthread_once(&g_once, opt_load);
    return &g_opt;
}
GfxOptions *gfx_options_edit(void) {
    pthread_once(&g_once, opt_load);
    return &g_opt;
}
int gfx_options_save(const char *key, const char *value) {
    static plat_lock_t lock = PLAT_LOCK_INIT;
    const char *path = gfx_options()->path;
    char tmp[PATH_MAX + 8], line[1024], k[128];
    int done = 0, ok = 0;
    plat_lock(&lock);
    FILE *in = fopen(path, "rb");
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *out = fopen(tmp, "wb");
    if (out) {
        while (in && fgets(line, sizeof line, in)) {
            const char *p = line, *eq = strchr(line, '=');
            while (*p == ' ' || *p == '\t') p++;
            int keyline = eq && *p != ';' && *p != '#' && *p != '[';
            if (keyline) {
                size_t n = (size_t)(eq - p);
                if (n >= sizeof k) n = sizeof k - 1;
                memcpy(k, p, n); k[n] = 0; trim(k);
            }
            if (keyline && !strcasecmp(k, key)) {
                if (!done) fprintf(out, "%s = %s\n", key, value);     /* a key written twice keeps one line */
                done = 1;
            } else fputs(line, out);
        }
        if (!done) {
            long end = ftell(out);
            fprintf(out, "%s%s = %s\n", end > 0 ? "\n" : "", key, value);
        }
        ok = fclose(out) == 0 && rename(tmp, path) == 0;
    }
    if (in) fclose(in);
    plat_unlock(&lock);
    printf("graphics options: %s = %s %s %s\n", key, value, ok ? "written to" : "could NOT be written to", path);
    fflush(stdout);
    return ok;
}

/* mods.c - the mods: what the base game (the console's, as recompiled) is changed by, each one switched on or off.
 *
 * WHERE. $SMO_MODS, else <exe folder>/../mods: the game folder's mods/ (tools/make_game_folder.py puts the repository's
 * default mods there, from defaults/mods, every time it runs), or build/mods for a build run from the repository
 * (tools/lifter/build_mods.py). One folder a mod; a folder copied from an emulator's load folder
 * (mods/<title id>/<name>/...) is found one level down.
 *
 * WHAT A MOD IS. Its folder may hold:
 *   mod.ini     name, description, default (on | off: before the player switches it), and what it changes:
 *                 feature = <name>    a change the runtime itself makes when a mod that is on declares it
 *                                     (hd_resolution, anisotropic_filtering, async_shaders: smo_mod_feature())
 *                 library = <file>    native code (smo_mod_api.h): loaded when the mod is on, before the game runs
 *               No mod.ini (a mod made for an emulator): the folder's name, on.
 *   romfs/      files that replace or add files of the game's RomFS, by the same path (nnfs.c asks mods_romfs_roots)
 *   exefs/      code patches for emulators (.ips / .pchtxt): NOT applied (a mod with only that is listed as such)
 *
 * WHICH ARE ON. config/mods.ini (beside graphics.ini): "<id> = on | off", one line a mod, in the order the RomFS is
 * looked up (the first line wins when two mods hold the same file). A mod not listed yet is added at the end with its
 * default. Read once, when the first file of the game is opened or the renderer asks; the settings menu's Mods tab
 * writes the file (used from the next start). */
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"
#include "smo_mod_api.h"

#define MODS_MAX 32
#define ROMFS_ON_MAX 16
typedef struct {
    char id[160], dir[PATH_MAX], romfs[PATH_MAX];  /* id: the folder (mods.ini's key); romfs: "" when none */
    char name[96], desc[256], feature[64], library[128];
    int on, def, files, has_exefs, manifest;
    void *lib; SmoModReport report; int installed;
} Mod;
static Mod g_mods[MODS_MAX];
static int g_n, g_loaded;
static const char *g_romfs_on[ROMFS_ON_MAX];
static int g_nromfs;
static plat_lock_t g_lock = PLAT_LOCK_INIT;

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static int count_files(const char *dir) {
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    char sub[PATH_MAX];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        n += is_dir(sub) ? count_files(sub) : 1;
    }
    if (d) closedir(d);
    return n;
}
static void trim(char *s) {
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}
/* "key = value" of a line (a ';' or '#' line is a comment); 0 when the line has none */
static int kv(const char *line, char *key, size_t kn, char *val, size_t vn) {
    char buf[600];
    snprintf(buf, sizeof buf, "%s", line);
    trim(buf);
    char *eq = strchr(buf, '=');
    if (!buf[0] || buf[0] == ';' || buf[0] == '#' || buf[0] == '[' || !eq) return 0;
    *eq = 0;
    snprintf(key, kn, "%s", buf); trim(key);
    char *v = eq + 1, *c = strchr(v, ';');           /* "on   ; comment" */
    if (c && c > v && (c[-1] == ' ' || c[-1] == '\t')) *c = 0;
    snprintf(val, vn, "%s", v); trim(val);
    return key[0] != 0;
}
static int is_on(const char *v) { return !strcasecmp(v, "on") || !strcasecmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "true"); }

const char *mods_dir(void) {
    static char dir[PATH_MAX];
    if (!dir[0]) {
        const char *e = getenv("SMO_MODS");
        char tmp[PATH_MAX];
        if (e && e[0]) snprintf(tmp, sizeof tmp, "%s", e); else snprintf(tmp, sizeof tmp, "%s/../mods", plat_exe_dir());
        if (!realpath(tmp, dir)) snprintf(dir, sizeof dir, "%s", tmp);
    }
    return dir;
}
static void ini_path(char *out) {
    char cfg[PATH_MAX];
    snprintf(cfg, sizeof cfg, "%s", gfx_options()->path);
    char *sl = strrchr(cfg, '/');
    if (sl) *sl = 0; else snprintf(cfg, sizeof cfg, ".");
    snprintf(out, PATH_MAX, "%s/mods.ini", cfg);
}
static int find(const char *id) { for (int i = 0; i < g_n; i++) if (!strcmp(g_mods[i].id, id)) return i; return -1; }

/* a folder that is a mod: mod.ini, romfs/ or exefs/ in it */
static int is_mod_dir(const char *dir) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/mod.ini", dir); if (is_file(p)) return 1;
    snprintf(p, sizeof p, "%s/romfs", dir); if (is_dir(p)) return 1;
    snprintf(p, sizeof p, "%s/exefs", dir); return is_dir(p);
}
static void read_manifest(Mod *m) {
    char p[PATH_MAX], line[600], k[64], v[512];
    snprintf(m->name, sizeof m->name, "%s", m->id);
    m->def = 1;
    snprintf(p, sizeof p, "%s/mod.ini", m->dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    m->manifest = 1;
    while (fgets(line, sizeof line, f)) {
        if (!kv(line, k, sizeof k, v, sizeof v)) continue;
        if (!strcasecmp(k, "name")) snprintf(m->name, sizeof m->name, "%s", v);
        else if (!strcasecmp(k, "description")) snprintf(m->desc, sizeof m->desc, "%s", v);
        else if (!strcasecmp(k, "default")) m->def = is_on(v);
        else if (!strcasecmp(k, "feature")) snprintf(m->feature, sizeof m->feature, "%s", v);
        else if (!strcasecmp(k, "library")) snprintf(m->library, sizeof m->library, "%s", v);
    }
    fclose(f);
}
static void add(const char *id, const char *dir) {
    if (g_n >= MODS_MAX || find(id) >= 0) return;
    Mod *m = &g_mods[g_n++];
    memset(m, 0, sizeof *m);
    snprintf(m->id, sizeof m->id, "%s", id);
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    snprintf(m->romfs, sizeof m->romfs, "%s/romfs", dir);
    if (!is_dir(m->romfs)) m->romfs[0] = 0;
    char ex[PATH_MAX]; snprintf(ex, sizeof ex, "%s/exefs", dir);
    m->has_exefs = is_dir(ex);
    read_manifest(m);
    m->on = m->def;
    if (m->romfs[0]) m->files = count_files(m->romfs);
}
static void write_ini(void) {
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    ini_path(path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "; The mods (the folders of %s) and which are on: <folder> = on | off, read when the game starts.\n"
               "; The settings menu (F1, Mods) switches them. The order is the order a file of the game is looked for in\n"
               "; their romfs/ folders: the first line wins when two mods replace the same file. A new mod is added at the\n"
               "; end with its default (mod.ini; a mod without one: on).\n", mods_dir());
    for (int i = 0; i < g_n; i++) fprintf(f, "%s = %s\n", g_mods[i].id, g_mods[i].on ? "on" : "off");
    if (fclose(f) == 0) rename(tmp, path); else unlink(tmp);
}
static int name_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }
static int list_sorted(const char *dir, char **out, int max) {
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) if (e->d_name[0] != '.') out[n++] = strdup(e->d_name);
    if (d) closedir(d);
    qsort(out, (size_t)n, sizeof out[0], name_cmp);
    return n;
}
static void load_locked(void) {
    if (g_loaded) return;
    g_loaded = 1;
    const char *dir = mods_dir();
    /* 1. the folders */
    struct { char id[160], dir[PATH_MAX]; } *found = calloc(MODS_MAX, sizeof *found);
    int nf = 0;
    char *names[256];
    int nn = list_sorted(dir, names, 256);
    for (int i = 0; i < nn; i++) {
        char top[PATH_MAX];
        snprintf(top, sizeof top, "%s/%s", dir, names[i]);
        if (is_dir(top) && is_mod_dir(top)) {
            if (nf < MODS_MAX) { snprintf(found[nf].id, sizeof found[0].id, "%s", names[i]); snprintf(found[nf].dir, sizeof found[0].dir, "%s", top); nf++; }
        } else if (is_dir(top)) {                          /* an emulator's <title id>/<name>/ */
            char *sub[128];
            int ns = list_sorted(top, sub, 128);
            for (int k = 0; k < ns; k++) {
                char inner[PATH_MAX];
                snprintf(inner, sizeof inner, "%s/%s", top, sub[k]);
                if (is_dir(inner) && is_mod_dir(inner) && nf < MODS_MAX) { snprintf(found[nf].id, sizeof found[0].id, "%s/%s", names[i], sub[k]); snprintf(found[nf].dir, sizeof found[0].dir, "%s", inner); nf++; }
                free(sub[k]);
            }
        }
        free(names[i]);
    }
    /* 2. mods.ini: order and on / off of those it lists */
    char path[PATH_MAX], line[600], k[300], v[64];
    int listed = 0, lines = 0, changed = 0;
    ini_path(path);
    FILE *f = fopen(path, "r");
    int had_file = f != NULL;
    while (f && fgets(line, sizeof line, f)) {
        if (!kv(line, k, sizeof k, v, sizeof v)) continue;
        lines++;
        for (int i = 0; i < nf; i++) if (!strcmp(found[i].id, k)) { add(found[i].id, found[i].dir); g_mods[g_n - 1].on = is_on(v); listed++; break; }
    }
    if (f) fclose(f);
    /* 3. the folders not listed yet: at the end, with their default */
    for (int i = 0; i < nf; i++) if (find(found[i].id) < 0) { add(found[i].id, found[i].dir); changed = 1; }
    if (lines != listed) changed = 1;                /* a line of a mod that is gone */
    if (!had_file && g_n) changed = 1;
    if (changed) write_ini();
    free(found);
    /* the romfs folders of the mods that are on, in order */
    for (int i = 0; i < g_n; i++) {
        Mod *m = &g_mods[i];
        int romfs_used = m->on && m->romfs[0] && g_nromfs < ROMFS_ON_MAX;
        if (romfs_used) g_romfs_on[g_nromfs++] = m->romfs;
        char what[200] = "";
        size_t w = 0;
        if (m->feature[0]) w += (size_t)snprintf(what + w, sizeof what - w, " feature %s;", m->feature);
        if (m->library[0]) w += (size_t)snprintf(what + w, sizeof what - w, " code %s;", m->library);
        if (m->romfs[0]) w += (size_t)snprintf(what + w, sizeof what - w, " %d RomFS file%s;", m->files, m->files == 1 ? "" : "s");
        if (m->has_exefs) w += (size_t)snprintf(what + w, sizeof what - w, " exefs/ (code patches for emulators: not applied);");
        if (!m->feature[0] && !m->library[0] && !m->romfs[0]) snprintf(what + w, sizeof what - w, " nothing this runtime uses;");
        printf("mods: %-28s %-3s (%s)%s\n", m->id, m->on ? "on" : "off", m->name, what);
        if (m->on && m->romfs[0] && !romfs_used) printf("mods: %s: more than %d mods with romfs/ are on: its files are not used\n", m->id, ROMFS_ON_MAX);
    }
    if (g_n) printf("mods: %d in %s (on / off: %s)\n", g_n, dir, path);
    fflush(stdout);
}
static void load(void) { plat_lock(&g_lock); load_locked(); plat_unlock(&g_lock); }

/* ---- what the rest of the runtime asks */
int mods_romfs_roots(const char *const **roots) { load(); *roots = g_romfs_on; return g_nromfs; }      /* nnfs.c */
int smo_mod_feature(const char *feature) {                 /* the renderer, gfx options: a feature a mod that is on declares */
    load();
    for (int i = 0; i < g_n; i++) if (g_mods[i].on && !strcasecmp(g_mods[i].feature, feature)) return 1;
    return 0;
}
/* the settings menu (smo_settings.c) */
int mods_count(void) { load(); return g_n; }
const char *mods_name(int i) { return i >= 0 && i < g_n ? g_mods[i].name : ""; }
const char *mods_desc(int i) { return i >= 0 && i < g_n ? g_mods[i].desc : ""; }
int mods_is_on(int i) { return i >= 0 && i < g_n && g_mods[i].on; }
/* 0 nothing usable (exefs only, or empty), 1 usable; *files: its RomFS files */
int mods_usable(int i, int *files) {
    if (i < 0 || i >= g_n) return 0;
    Mod *m = &g_mods[i];
    *files = m->files;
    return m->feature[0] || m->library[0] || m->romfs[0];
}
int mods_has_files_only(int i) { return i >= 0 && i < g_n && !g_mods[i].feature[0] && !g_mods[i].library[0] && g_mods[i].romfs[0]; }
void mods_set(int i, int on) {                             /* written now, used from the next start */
    plat_lock(&g_lock);
    if (i >= 0 && i < g_n) {
        g_mods[i].on = on;
        write_ini();
        printf("mods: %s = %s written to mods.ini (from the next start)\n", g_mods[i].id, on ? "on" : "off");
        fflush(stdout);
    }
    plat_unlock(&g_lock);
}

/* ---- code mods */
static SmoGameFn api_game_fn(uint64_t guest) {             /* fn_table is sorted by guest address */
    int lo = 0, hi = fn_table_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fn_table[mid].addr == guest) return fn_table[mid].fn;
        if (fn_table[mid].addr < guest) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}
static uintptr_t api_game_fn_end(SmoGameFn fn) { return host_fn_end((uintptr_t)fn); }
/* Every code mod that is on: loaded and initialised (boot.c, after the game's code is loaded, before the game runs). */
void mods_load_code(void) {
    load();
    for (int i = 0; i < g_n; i++) {
        Mod *m = &g_mods[i];
        if (!m->on || !m->library[0]) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", m->dir, m->library);
        m->lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!m->lib) { printf("mods: %s: its code %s could not be loaded: %s\n", m->id, path, dlerror()); continue; }
        SmoModInit init = (SmoModInit)dlsym(m->lib, "smo_mod_init");
        m->report = (SmoModReport)dlsym(m->lib, "smo_mod_report");
        if (!init) { printf("mods: %s: %s has no smo_mod_init: not used\n", m->id, m->library); continue; }
        static SmoModApi api[MODS_MAX];
        api[i] = (SmoModApi){SMO_MOD_API_VERSION, m->dir, m->id, api_game_fn, api_game_fn_end, guest_trap, smo_mod_feature};
        int r = init(&api[i]);
        m->installed = r == 0;
        printf("mods: %s: code %s\n", m->id, r == 0 ? "installed" : "NOT installed (its init said so)");
    }
    fflush(stdout);
}
void mods_report(FILE *out) {                              /* hostrt.c, at exit */
    for (int i = 0; i < g_n; i++) if (g_mods[i].installed && g_mods[i].report) g_mods[i].report(out);
}

/* nn::fs on host directories, plus the minimal nn::account and nn::nifm
 * answers the game needs to start: one local user, no network.
 *
 * MOUNT TABLE. A path is "name:/dir/file"; the name must have been mounted, as in the SDK's own mount table
 * ([sdk 0x1f4960] MountTable::Find: an unknown name is fs NotMounted 0x35f202; mounting a name twice is
 * MountNameAlreadyExists 0x7802). Mount kinds and their host folders:
 *   MountRom           -> $SMO_ROMFS or <exe folder>/../romfs      (the game's extracted romfs, update applied; read-only;
 *                                                                   a name not found as written is looked up without regard to case)
 *   MountSaveData      -> <userdata>/save                           (journaled, see below)
 *   MountCacheStorage  -> <userdata>/cache                          (journaled like save data)
 * <userdata> is $SMO_USERDATA or <exe folder>/../userdata (tests point SMO_USERDATA at a copy of the user's save).
 *
 * ROMFS MODS (mods.c). A file of the RomFS is looked for in the romfs/ folder of each mod that is on, in the order of
 * config/mods.ini, then in the game's own RomFS, each with the case-insensitive fallback; a directory lists the
 * entries of all of them (the first one of a name wins).
 *
 * SAVE DATA JOURNAL. On the console, writes to save data are not permanent until nn::fs::Commit: they go to a journal,
 * Commit applies all of them together, and uncommitted writes are gone at the next mount. Here:
 *   - <userdata>/save is the COMMITTED state and is never written outside Commit.
 *   - Mounting creates the working folder <userdata>/save.work.<pid>. A file is copied into it the first time it is
 *     opened for writing (copy-on-write) or created there; deletions are recorded (tombstones); every read, existence
 *     test and directory listing sees the working folder over the committed one, minus the tombstones.
 *   - Commit ([sdk 0x1f4490] FileSystemAccessor::Commit: fails with 0x327202 while a file of the mount is open with the
 *     write mode) is two-phase: (1) the changed files are flushed and the list of changes is written to
 *     <userdata>/save.journal.<pid>, flushed, and renamed to <userdata>/save.commit -- from this rename on the commit
 *     is decided; (2) the changes are applied (files moved over the committed ones, deletions done) and the marker is
 *     removed. A process killed in phase 1 leaves the committed state untouched; one killed in phase 2 leaves the
 *     marker, and the next mount finishes the apply (roll forward) before anything is read.
 *   - Unmount, or a later mount by any process, discards the working folder of a process that no longer runs.
 * Not reproduced: the console's limit on the journal SIZE (a Commit is needed before the journal fills; the game's own
 * save code already commits within it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define STR(n) ((const char *)(uintptr_t)c->x[n])

/* nn::fs results (module 2), value = description << 9 | 2. Descriptions as published (switchbrew "Error codes",
 * the same numbers the suyu reference uses in file_sys/errors.h): 1 PathNotFound, 2 PathAlreadyExists,
 * 1002 TargetNotFound (no such save data), 3005 OutOfRange (read past the end of the file),
 * 6201 FileExtensionWithoutOpenModeAllowAppend (write past the end of a file opened without the append mode),
 * 6202 ReadNotPermitted, 6203 WriteNotPermitted (the handle was opened without that mode).
 * Read in sdk.elf: 60 MountNameAlreadyExists, 6905 NotMounted [0x1f498c], 6457 "a file is still open for writing" [0x1f44f4].
 * 7 DirectoryNotEmpty, 6400 PermissionDenied (suyu file_sys/errors.h; write to the read-only rom). */
#define RESULT_PATH_NOT_FOUND 0x202u
#define RESULT_PATH_ALREADY_EXISTS 0x402u
#define RESULT_MOUNT_NAME_EXISTS 0x7802u
#define RESULT_TARGET_NOT_FOUND 0x7d402u
#define RESULT_OUT_OF_RANGE 0x177a02u
#define RESULT_NO_APPEND 0x307202u
#define RESULT_READ_NOT_PERMITTED 0x307402u
#define RESULT_WRITE_NOT_PERMITTED 0x307602u
#define RESULT_PERMISSION_DENIED 0x320002u
#define RESULT_WRITE_FILE_OPEN 0x327202u
#define RESULT_NOT_MOUNTED 0x35f202u
#define RESULT_GENERIC_FAILURE 0x3e802u      /* a host failure with no console counterpart (disk error, permissions) */

enum { MK_ROM = 1, MK_SAVE, MK_CACHE, MK_PLAIN /* self-tests: a writable host folder without a journal */ };
typedef struct {
    char name[16];
    int kind;
    char root[PATH_MAX];                     /* committed state (or the rom folder) */
    char work[PATH_MAX];                     /* journal working folder ("" for the rom) */
    char **tomb;                             /* relative paths deleted since the last commit, '/' separators */
    int tomb_n;
} Mount;
#define MOUNT_MAX 8
static Mount g_mounts[MOUNT_MAX];
static char g_rom_root[PATH_MAX], g_user_root[PATH_MAX];
static volatile int32_t g_open_log;
static plat_lock_t g_fs_lock;
static volatile int32_t g_fs_lock_ready;

static void fs_lock(void) {
    if (plat_cas(&g_fs_lock_ready, 1, 0) == 0) { plat_lock_init(&g_fs_lock); g_fs_lock_ready = 2; }
    while (g_fs_lock_ready != 2) plat_yield();
    plat_lock(&g_fs_lock);
}
static void fs_unlock(void) { plat_unlock(&g_fs_lock); }

static void fs_roots(void) {
    if (g_rom_root[0]) return;
    const char *env = getenv("SMO_ROMFS"), *user = getenv("SMO_USERDATA");
    if (env && env[0]) snprintf(g_rom_root, PATH_MAX, "%s", env); else snprintf(g_rom_root, PATH_MAX, "%s/../romfs", plat_exe_dir());
    if (user && user[0]) snprintf(g_user_root, PATH_MAX, "%s", user); else snprintf(g_user_root, PATH_MAX, "%s/../userdata", plat_exe_dir());
    plat_mkdir(g_user_root);
}
const char *fs_user_root(void) { fs_roots(); return g_user_root; }
const char *fs_rom_root(void) { fs_roots(); return g_rom_root; }           /* smo_ui.c: the game's own files for the host menus */

/* ---- RomFS mods (mods.c): the romfs/ folders of the mods that are on, first wins */
int mods_romfs_roots(const char *const **roots);

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static void make_parents(const char *path) {             /* creates every missing folder above `path` */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; plat_mkdir(tmp); *p = '/'; }
}
static void remove_tree(const char *dir) {               /* never follows a symbolic link (lstat) */
    char sub[PATH_MAX];
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
            struct stat st;
            if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(sub);
            else unlink(sub);
        }
        closedir(d);
    }
    rmdir(dir);
}
/* The RomFS was extracted on a file system that tells case apart; the game's own paths match its names, but a name the
 * game spells differently is still found the way the console's file system (and Windows) found it: each component
 * that does not exist as written is looked for without regard to case. out = the path found; 0 when none. */
static int ci_resolve(const char *root, const char *rel, char *out) {
    char cur[PATH_MAX], part[PATH_MAX];
    snprintf(cur, sizeof cur, "%s", root);
    for (const char *p = rel; *p;) {
        size_t n = strcspn(p, "/");
        if (n >= sizeof part) return 0;
        memcpy(part, p, n); part[n] = 0;
        size_t len = strlen(cur);
        snprintf(cur + len, sizeof cur - len, "/%s", part);
        if (!exists(cur)) {
            cur[len] = 0;
            DIR *d = opendir(cur);
            int found = 0;
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) if (!strcasecmp(e->d_name, part)) { snprintf(cur + len, sizeof cur - len, "/%s", e->d_name); found = 1; break; }
                closedir(d);
            }
            if (!found) return 0;
        }
        p += n;
        while (*p == '/') p++;
    }
    snprintf(out, PATH_MAX, "%s", cur);
    return 1;
}
/* smo_ui.c: a file of the RomFS by its path in it, found as the game's own reads find it (case-insensitive fallback) */
int fs_rom_file(const char *rel, char *out) {
    char full[PATH_MAX];
    const char *const *mr; int nm = mods_romfs_roots(&mr);
    for (int i = 0; i < nm; i++) { snprintf(full, sizeof full, "%s/%s", mr[i], rel); if (exists(full)) { snprintf(out, PATH_MAX, "%s", full); return 1; } if (ci_resolve(mr[i], rel, out)) return 1; }
    snprintf(full, sizeof full, "%s/%s", fs_rom_root(), rel);
    if (exists(full)) { snprintf(out, PATH_MAX, "%s", full); return 1; }
    return ci_resolve(fs_rom_root(), rel, out);
}
static int copy_file(const char *from, const char *to) {   /* fails when `to` exists */
    int in = open(from, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    char buf[1 << 16];
    ssize_t n;
    int ok = in >= 0 && out >= 0;
    while (ok && (n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) ok = 0;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    if (!ok && out >= 0) unlink(to);
    return ok;
}

/* ---- tombstones */
static int tomb_find(Mount *m, const char *rel) {
    for (int i = 0; i < m->tomb_n; i++) if (strcasecmp(m->tomb[i], rel) == 0) return i;
    return -1;
}
static int tomb_hides(Mount *m, const char *rel) {       /* rel itself or one of its parent folders was deleted */
    for (int i = 0; i < m->tomb_n; i++) {
        size_t n = strlen(m->tomb[i]);
        if (strncasecmp(m->tomb[i], rel, n) == 0 && (rel[n] == 0 || rel[n] == '/')) return 1;
    }
    return 0;
}
static void tomb_add(Mount *m, const char *rel) {
    if (tomb_find(m, rel) >= 0) return;
    m->tomb = realloc(m->tomb, sizeof(char *) * (size_t)(m->tomb_n + 1));
    m->tomb[m->tomb_n++] = strdup(rel);
}
static void tomb_clear(Mount *m) { for (int i = 0; i < m->tomb_n; i++) free(m->tomb[i]); free(m->tomb); m->tomb = NULL; m->tomb_n = 0; }

/* ---- path resolution */
static Mount *mount_find(const char *name, size_t n) {
    for (int i = 0; i < MOUNT_MAX; i++) if (g_mounts[i].kind && strlen(g_mounts[i].name) == n && strncmp(g_mounts[i].name, name, n) == 0) return &g_mounts[i];
    return NULL;
}
/* "mount:/dir/file" -> mount and relative path with '/' separators (no leading or trailing separator). Result code. */
static uint32_t split_path(const char *guest, Mount **mount, char *rel) {
    const char *colon = guest ? strchr(guest, ':') : NULL;
    if (!colon) return RESULT_NOT_MOUNTED;
    fs_roots();
    Mount *m = mount_find(guest, (size_t)(colon - guest));
    if (!m) return RESULT_NOT_MOUNTED;
    const char *rest = colon + 1;
    while (*rest == '/' || *rest == '\\') rest++;
    snprintf(rel, PATH_MAX, "%s", rest);
    for (char *p = rel; *p; p++) if (*p == '\\') *p = '/';
    size_t len = strlen(rel);
    while (len > 0 && rel[len - 1] == '/') rel[--len] = 0;
    *mount = m;
    return 0;
}
static void join(char *out, const char *root, const char *rel) { if (rel[0]) snprintf(out, PATH_MAX, "%s/%s", root, rel); else snprintf(out, PATH_MAX, "%s", root); }
/* The host path that currently holds `rel` in the mount's view; returns 0 when it does not exist in the view. */
static int view_path(Mount *m, const char *rel, char *out) {
    if (m->work[0]) {
        if (tomb_hides(m, rel)) { join(out, m->work, rel); return exists(out); }      /* re-created after a deletion: only the working copy counts */
        join(out, m->work, rel);
        if (exists(out)) return 1;
    }
    if (m->kind == MK_ROM && rel[0]) {             /* RomFS mods first (the first one that is on and holds the file) */
        const char *const *mr; int nm = mods_romfs_roots(&mr);
        for (int i = 0; i < nm; i++) { join(out, mr[i], rel); if (exists(out) || ci_resolve(mr[i], rel, out)) return 1; }
    }
    join(out, m->root, rel);
    if (exists(out)) return 1;
    return m->kind == MK_ROM && rel[0] && ci_resolve(m->root, rel, out);
}
static int view_parent_is_dir(Mount *m, const char *rel) {
    char parent[PATH_MAX], host[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", rel);
    char *s = strrchr(parent, '/');
    if (!s) return 1;                                     /* directly under the root */
    *s = 0;
    return view_path(m, parent, host) && is_dir(host);
}
/* (nnmovie_mf.c) the host file that holds guest path "mount:/dir/file" in its mount's view; 0 if none. On the
 * console the movie library's Extractor::SetDataSource(const char*) opens such a path through nn::fs itself. */
int fs_host_path(const char *guest, char *out, size_t n) {
    Mount *m; char rel[PATH_MAX], host[PATH_MAX];
    int ok = 0;
    fs_lock();
    if (!split_path(guest, &m, rel) && view_path(m, rel, host) && !is_dir(host)) { snprintf(out, n, "%s", host); ok = 1; }
    fs_unlock();
    return ok;
}
/* Used by nnfsdir.c: the host folders to list for a guest directory (working folder first), 0 = no such directory. */
int fs_dir_roots(const char *guest, char roots[][PATH_MAX], uint32_t *result) {      /* roots: room for FS_DIR_ROOTS */
    Mount *m; char rel[PATH_MAX], host[PATH_MAX];
    int n = 0;
    fs_lock();
    *result = split_path(guest, &m, rel);
    if (!*result) {
        if (!view_path(m, rel, host) || !is_dir(host)) *result = RESULT_PATH_NOT_FOUND;
        else {
            if (m->kind == MK_ROM) { const char *const *mr; int nm = mods_romfs_roots(&mr); for (int i = 0; i < nm; i++) { join(roots[n], mr[i], rel); if (is_dir(roots[n]) || (rel[0] && ci_resolve(mr[i], rel, roots[n]) && is_dir(roots[n]))) n++; } }      /* the mods' folders first */
            if (m->work[0]) { join(roots[n], m->work, rel); if (is_dir(roots[n])) n++; }
            if (!m->work[0] || !tomb_hides(m, rel)) { join(roots[n], m->root, rel); if (is_dir(roots[n])) n++; }
        }
    }
    fs_unlock();
    return n;
}
int fs_dir_hidden(const char *guest_dir, const char *name) {      /* 1 = this entry of the committed folder was deleted in the working state */
    Mount *m; char rel[PATH_MAX], full[PATH_MAX];
    int hidden = 0;
    fs_lock();
    if (!split_path(guest_dir, &m, rel) && m->work[0]) { join(full, rel, name); hidden = tomb_hides(m, rel[0] ? full : name); }
    fs_unlock();
    return hidden;
}

/* Open files: nn::fs::FileHandle is the file descriptor itself; the open mode (1 read, 2 write, 4 allow append) and the
 * mount are kept here so reads, writes, growth and Commit behave exactly as the mode says. */
#define OPEN_MAX 1024
static struct { int h; int mode; Mount *mount; } g_open[OPEN_MAX];
static void open_remember(int h, int mode, Mount *m) {
    fs_lock();
    for (int i = 0; i < OPEN_MAX; i++) if (!g_open[i].h) { g_open[i].h = h; g_open[i].mode = mode; g_open[i].mount = m; break; }
    fs_unlock();
}
static int open_mode(int h, int forget) {             /* mode of an open handle, 7 (everything) if unknown */
    int mode = 7;
    fs_lock();
    for (int i = 0; i < OPEN_MAX; i++) if (g_open[i].h == h) { mode = g_open[i].mode; if (forget) g_open[i].h = 0; break; }
    fs_unlock();
    return mode;
}

/* ---- journal: commit and recovery */
/* one commit at a time across processes: an flock on <userdata>/.commit.lock */
static int commit_mutex(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.commit.lock", g_user_root);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) flock(fd, LOCK_EX);
    return fd;
}
static void commit_done(int fd) { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }
static void journal_list(FILE *j, const char *work, const char *rel) {     /* every file of the working folder, depth first */
    char dir[PATH_MAX], sub[PATH_MAX];
    join(dir, work, rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (rel[0]) snprintf(sub, sizeof sub, "%s/%s", rel, e->d_name); else snprintf(sub, sizeof sub, "%s", e->d_name);
        char full[PATH_MAX];
        join(full, work, sub);
        if (is_dir(full)) { fprintf(j, "M\t%s\n", sub); journal_list(j, work, sub); }
        else {
            plat_fsync_path(full);
            fprintf(j, "F\t%s\n", sub);
        }
    }
    closedir(d);
}
/* Applies a decided commit: marker lines are "W <working folder>", "R <committed folder>", then "D rel" (delete),
 * "M rel" (make folder), "F rel" (move file). Every step can be repeated, so a commit interrupted here is finished by
 * running it again. */
static int journal_apply(const char *marker) {
    char line[PATH_MAX + 8], work[PATH_MAX] = "", root[PATH_MAX] = "", a[PATH_MAX], b[PATH_MAX];
    FILE *j = fopen(marker, "r");
    int n = 0;
    if (!j) return -1;
    while (fgets(line, sizeof line, j)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (len < 3) continue;
        const char *arg = line + 2;
        if (line[0] == 'W') snprintf(work, sizeof work, "%s", arg);
        else if (line[0] == 'R') snprintf(root, sizeof root, "%s", arg);
        else if (!root[0] || !work[0]) continue;
        else if (line[0] == 'D') { join(a, root, arg); if (is_dir(a)) remove_tree(a); else unlink(a); n++; }
        else if (line[0] == 'M') { join(a, root, arg); make_parents(a); plat_mkdir(a); }
        else if (line[0] == 'F') {
            join(a, work, arg); join(b, root, arg);
            if (exists(a)) { make_parents(b); if (!plat_rename_replace(a, b)) { fclose(j); return -2; } }
            n++;
        }
    }
    fclose(j);
    plat_fsync_dir(root[0] ? root : marker);
    unlink(marker);
    return n;
}
static int pid_alive(uint32_t pid) { return kill((pid_t)pid, 0) == 0 || errno == EPERM; }
/* Before a save-type folder is mounted: finish a commit that was decided but not applied, and drop the working
 * folders and half-written journals of processes that no longer run. */
static void journal_recover(const char *root) {
    char marker[PATH_MAX], parent[PATH_MAX], path[PATH_MAX];
    int mx = commit_mutex();
    snprintf(marker, sizeof marker, "%s.commit", root);
    if (exists(marker)) {
        int n = journal_apply(marker);
        printf("nn::fs: a commit of %s was interrupted after it was decided: finished now (%d changes)\n", root, n);
        trace_printf("     fs journal: interrupted commit of %s rolled forward (%d changes)\n", root, n);
    }
    snprintf(parent, sizeof parent, "%s", root);
    char *slash = strrchr(parent, '/');
    const char *base = slash ? slash + 1 : root;
    if (slash) *slash = 0; else strcpy(parent, ".");
    size_t blen = strlen(base);
    DIR *d = opendir(parent);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *name = e->d_name;
            if (strncmp(name, base, blen) || name[blen] != '.') continue;
            const char *ext = strrchr(name, '.');
            const char *kind = ext && ext - 5 >= name && !strncmp(ext - 5, ".work", 5) ? "work" : ext && ext - 8 >= name && !strncmp(ext - 8, ".journal", 8) ? "journal" : NULL;
            if (!kind) continue;
            uint32_t pid = (uint32_t)strtoul(ext + 1, NULL, 10);
            if (!pid || pid == (uint32_t)getpid() || pid_alive(pid)) continue;
            snprintf(path, sizeof path, "%s/%s", parent, name);
            if (is_dir(path)) remove_tree(path); else unlink(path);
            trace_printf("     fs journal: uncommitted %s of process %lu discarded (%s)\n", kind, (unsigned long)pid, name);
        }
        closedir(d);
    }
    commit_done(mx);
}
static uint32_t mount_add(const char *name, int kind, const char *root) {
    Mount *m = NULL;
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= sizeof m->name) return RESULT_GENERIC_FAILURE;
    fs_lock();
    if (mount_find(name, n)) { fs_unlock(); return RESULT_MOUNT_NAME_EXISTS; }
    for (int i = 0; i < MOUNT_MAX && !m; i++) if (!g_mounts[i].kind) m = &g_mounts[i];
    if (!m) { fs_unlock(); return RESULT_GENERIC_FAILURE; }
    memset(m, 0, sizeof *m);
    strcpy(m->name, name);
    snprintf(m->root, PATH_MAX, "%s", root);
    if (kind != MK_ROM) {
        snprintf(m->work, PATH_MAX, "%s.work.%lu", root, (unsigned long)getpid());
        remove_tree(m->work);                              /* a second mount in this process starts from the committed state */
        plat_mkdir(m->work);
    }
    m->kind = kind;
    fs_unlock();
    return 0;
}

static void save_dir(char *dir, const char *leaf) { fs_roots(); snprintf(dir, PATH_MAX, "%s/%s", g_user_root, leaf); }
/* Save data of the one local user lives in <userdata>/save. It exists once EnsureSaveData has created it;
 * MountSaveData of save data that does not exist fails as on the console. */
static int save_exists(void) { char dir[PATH_MAX]; save_dir(dir, "save"); return is_dir(dir); }
static void fs_MountRom(Ctx *c) {                        /* (name, cache buffer, cache size): the buffer holds the RomFS metadata cache of the SDK's reader; the host reads the extracted tree */
    fs_roots();
    uint32_t r = mount_add(STR(0), MK_ROM, g_rom_root);
    trace_printf("     fs mount rom \"%s\" -> %s (%x)\n", STR(0), g_rom_root, r);
    RET(r);
}
static void fs_MountSaveData(Ctx *c) {                   /* (const char *name, const Uid &) */
    char dir[PATH_MAX];
    save_dir(dir, "save");
    if (!save_exists()) { trace_printf("     fs mount save data \"%s\": no save data (EnsureSaveData was not called)\n", STR(0)); RET(RESULT_TARGET_NOT_FOUND); return; }
    journal_recover(dir);
    uint32_t r = mount_add(STR(0), MK_SAVE, dir);
    trace_printf("     fs mount save data \"%s\" -> %s, journal working folder %s.work.%lu (%x)\n", STR(0), dir, dir, (unsigned long)getpid(), r);
    RET(r);
}
static void fs_Unmount(Ctx *c) {                         /* (const char *name): uncommitted changes are dropped */
    const char *name = STR(0);
    fs_lock();
    Mount *m = name ? mount_find(name, strlen(name)) : NULL;
    if (m) {
        for (int i = 0; i < OPEN_MAX; i++) if (g_open[i].h && g_open[i].mount == m) { close(g_open[i].h); g_open[i].h = 0; }   /* [sdk 0x1f4870] the accessor closes what is left open */
        if (m->work[0]) remove_tree(m->work);
        tomb_clear(m);
        trace_printf("     fs unmount \"%s\"\n", name);
        m->kind = 0;
    } else trace_printf("     fs unmount \"%s\": not mounted\n", name ? name : "(null)");
    fs_unlock();
}
static void fs_EnsureSaveData(Ctx *c) {                  /* (const Uid &): creates the user's save data if there is none */
    char dir[PATH_MAX];
    save_dir(dir, "save");
    RET(plat_mkdir(dir) ? 0 : RESULT_GENERIC_FAILURE);
}
static void fs_Commit(Ctx *c) {                          /* (const char *mount name) */
    const char *name = STR(0);
    char journal[PATH_MAX], marker[PATH_MAX];
    fs_lock();
    Mount *m = name ? mount_find(name, strlen(name)) : NULL;
    if (!m) { fs_unlock(); RET(RESULT_NOT_MOUNTED); return; }
    if (m->kind == MK_ROM) { fs_unlock(); RET(0); return; }                 /* nothing to commit on a read-only file system */
    for (int i = 0; i < OPEN_MAX; i++)
        if (g_open[i].h && g_open[i].mount == m && (g_open[i].mode & 2)) { fs_unlock(); trace_printf("     fs commit \"%s\": refused, a file is open for writing\n", name); RET(RESULT_WRITE_FILE_OPEN); return; }
    int mx = commit_mutex();
    snprintf(journal, sizeof journal, "%s.journal.%lu", m->root, (unsigned long)getpid());
    snprintf(marker, sizeof marker, "%s.commit", m->root);
    FILE *j = fopen(journal, "w");
    int ok = j != NULL, changes = -1;
    if (ok) {
        fprintf(j, "W\t%s\nR\t%s\n", m->work, m->root);
        for (int i = 0; i < m->tomb_n; i++) fprintf(j, "D\t%s\n", m->tomb[i]);
        journal_list(j, m->work, "");
        ok = fflush(j) == 0 && fsync(fileno(j)) == 0;
        fclose(j);
    }
    if (ok) ok = plat_rename_replace(journal, marker) && plat_fsync_dir(marker);      /* the commit is decided here */
    if (ok) { changes = journal_apply(marker); ok = changes >= 0; }
    if (ok) tomb_clear(m);
    commit_done(mx);
    fs_unlock();
    trace_printf("     fs commit \"%s\": %s (%d changes)\n", name, ok ? "ok" : "FAILED", changes);
    if (!ok) printf("nn::fs::Commit(\"%s\") failed on the host (%s); the committed save data is unchanged or will be completed at the next mount\n", name, strerror(errno));
    RET(ok ? 0 : RESULT_GENERIC_FAILURE);
}
static void fs_ok(Ctx *c) { RET(0); }

/* [sdk 0x2175e0] QueryMountRomCacheSize asks the RomFS reader for the working memory its metadata needs: the sizes of the
 * four tables of the RomFS image (directory hash table, directory entries, file hash table, file entries). The image
 * itself is not on this machine (only the extracted tree is), so the tables are rebuilt from the tree with the format's
 * rules: a directory entry is 0x18 bytes + its name padded to 4, a file entry 0x20 + name padded to 4, a hash table has
 * 4 bytes per bucket and the bucket count the RomFS builder derives from the entry count. NOT VERIFIED against the
 * image's own header (it would need the original romfs image). */
static uint32_t romfs_buckets(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0) n++;
    return n;
}
static void romfs_walk(const char *dir, uint32_t *dirs, uint32_t *files, uint64_t *dir_bytes, uint64_t *file_bytes) {
    char sub[PATH_MAX];
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int len = (int)strlen(e->d_name);                /* names are UTF-8 on Linux, as in the RomFS */
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        if (is_dir(sub)) {
            (*dirs)++; *dir_bytes += 0x18 + (uint64_t)((len + 3) & ~3);
            romfs_walk(sub, dirs, files, dir_bytes, file_bytes);
        } else { (*files)++; *file_bytes += 0x20 + (uint64_t)((len + 3) & ~3); }
    }
    closedir(d);
}
static void fs_QueryMountRomCacheSize(Ctx *c) {          /* (size_t *out) */
    uint32_t dirs = 1, files = 0;
    uint64_t dir_bytes = 0x18, file_bytes = 0;
    fs_roots();
    romfs_walk(g_rom_root, &dirs, &files, &dir_bytes, &file_bytes);
    {   const char *const *mr; int nm = mods_romfs_roots(&mr);
        for (int i = 0; i < nm; i++) romfs_walk(mr[i], &dirs, &files, &dir_bytes, &file_bytes);      }      /* (a file both hold counts twice: the cache is a little larger than needed) */
    uint64_t size = 4ull * romfs_buckets(dirs) + dir_bytes + 4ull * romfs_buckets(files) + file_bytes;
    trace_printf("     fs QueryMountRomCacheSize: %u directories, %u files -> %llu bytes\n", dirs, files, (unsigned long long)size);
    wr64(A0, size);
    RET(0);
}

static void fs_OpenFile(Ctx *c) {                     /* (FileHandle *out, const char *path, int mode) */
    char rel[PATH_MAX], path[PATH_MAX];
    Mount *m = NULL;
    int h = -1;
    int write = (A2 & 2) != 0;
    fs_lock();
    uint32_t r = split_path(STR(1), &m, rel);
    if (!r && write && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r) {
        int found = view_path(m, rel, path) && !is_dir(path);
        if (!found) r = RESULT_PATH_NOT_FOUND;
        else if (write && m->work[0]) {                    /* copy-on-write: the committed file is never opened for writing */
            char w[PATH_MAX];
            join(w, m->work, rel);
            if (strcmp(w, path) != 0) { make_parents(w); if (!copy_file(path, w)) r = RESULT_GENERIC_FAILURE; }
            strcpy(path, w);
        }
    }
    fs_unlock();
    if (!r) {
        h = open(path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
        if (h < 0) r = RESULT_PATH_NOT_FOUND;
    }
    if (plat_inc(&g_open_log) <= 200 || r)     /* D2: every failed open is logged, not only the first 200 */
        trace_printf("     fs open \"%s\" mode %d -> %s\n", STR(1), (int)A2, r == RESULT_NOT_MOUNTED ? "NOT MOUNTED" : r ? "NOT FOUND" : "ok");
    {   static int on = -1; if (on < 0) on = getenv("SMO_FSLOG") != NULL;       /* D2 debug: every open on stdout */
        if (on) printf("fslog: open \"%s\" mode %d -> %s\n", STR(1), (int)A2, r ? "NOT FOUND" : "ok"); }
    if (r) { wr64(A0, 0); RET(r); return; }
    open_remember(h, (int)(A2 & 7), m);
    wr64(A0, (uint64_t)h);
    RET(0);
}
static void fs_CloseFile(Ctx *c) { if (A0) { open_mode((int)A0, 1); close((int)A0); } }
extern int g_hitch_ms; void hitch_note(const char *s);      /* hitch.c: SMO_HITCH */
static uint64_t read_at(int h, int64_t offset, void *buf, uint64_t size) {
    uint64_t total = 0;
    if (g_hitch_ms) { char note[80]; snprintf(note, sizeof note, "read of %llu KB", (unsigned long long)(size >> 10)); hitch_note(note); }
    while (total < size) {
        uint64_t pos = (uint64_t)offset + total;
        size_t chunk = size - total > 0x40000000 ? 0x40000000 : (size_t)(size - total);
        ssize_t got = pread(h, (char *)buf + total, chunk, (off_t)pos);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        total += (uint64_t)got;
    }
    if (g_hitch_ms) hitch_note(NULL);
    return total;
}
static void fs_ReadFileCount(Ctx *c) {                /* (size_t *outRead, handle, offset, buffer, size) */
    uint64_t got = read_at((int)A1, (int64_t)A2, (void *)(uintptr_t)A3, A4);
    wr64(A0, got);
    if (got != A4 && getenv("SMO_FSLOG")) printf("fslog: ReadFile(count) SHORT handle %llx offset %llx size %llx got %llx from=%llx\n", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A4, (unsigned long long)got, (unsigned long long)c->x[30] - 4);   /* D2 debug */
    RET(0);
}
static void fs_WriteFile(Ctx *c) {                    /* (handle, offset, buffer, size, option) */
    int fd = (int)A0, mode = open_mode(fd, 0);
    struct stat st;
    if (!(mode & 2)) { RET(RESULT_WRITE_NOT_PERMITTED); return; }
    if (!(mode & 4) && fstat(fd, &st) == 0 && (int64_t)(A1 + A3) > (int64_t)st.st_size) { RET(RESULT_NO_APPEND); return; }   /* a file only grows in append mode */
    for (uint64_t done = 0; done < A3;) {
        ssize_t put = pwrite(fd, (const char *)(uintptr_t)A2 + done, (size_t)(A3 - done), (off_t)(A1 + done));
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) { RET(RESULT_GENERIC_FAILURE); return; }
        done += (uint64_t)put;
    }
    if (A4 && (rd32(A4) & 1)) fsync(fd);                                        /* WriteOption::Flush */
    RET(0);
}
static void fs_GetFileSize(Ctx *c) {                  /* (s64 *out, handle) */
    struct stat st;
    if (fstat((int)A1, &st)) { RET(RESULT_GENERIC_FAILURE); return; }
    wr64(A0, (uint64_t)st.st_size);
    RET(0);
}
/* SetFileSize (handle, size): the file grows (zero-filled) or is cut to `size`; only on a handle opened for
 * writing (on a journaled mount that is the working copy, committed with the rest). sead::NinFileDeviceBase::doOpen_
 * calls it when it opens a file to write it new. FlushFile (handle): what was written reaches the disk. */
static void fs_SetFileSize(Ctx *c) {
    if (!(open_mode((int)A0, 0) & 2)) { RET(RESULT_WRITE_NOT_PERMITTED); return; }
    if ((int64_t)A1 < 0) { RET(RESULT_OUT_OF_RANGE); return; }
    if (ftruncate((int)A0, (off_t)A1)) { RET(RESULT_GENERIC_FAILURE); return; }
    RET(0);
}
static void fs_FlushFile(Ctx *c) { if (open_mode((int)A0, 0) & 2) fsync((int)A0); RET(0); }
/* Where a new entry goes: the working folder of a journaled mount. Result code; `path` = host path to create. */
static uint32_t create_target(const char *guest, Mount **mount, char *rel, char *path) {
    char host[PATH_MAX];
    uint32_t r = split_path(guest, mount, rel);
    if (r) return r;
    if ((*mount)->kind == MK_ROM) return RESULT_PERMISSION_DENIED;
    if (!rel[0] || view_path(*mount, rel, host)) return RESULT_PATH_ALREADY_EXISTS;
    if (!view_parent_is_dir(*mount, rel)) return RESULT_PATH_NOT_FOUND;
    join(path, (*mount)->work, rel);
    make_parents(path);
    return 0;
}
static void fs_CreateFile(Ctx *c) {                   /* (path, size) */
    char rel[PATH_MAX], path[PATH_MAX];
    Mount *m;
    fs_lock();
    uint32_t r = create_target(STR(0), &m, rel, path);
    if (!r) {
        int h = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (h < 0) r = RESULT_GENERIC_FAILURE;
        else {
            if (ftruncate(h, (off_t)A1)) r = RESULT_GENERIC_FAILURE;
            close(h);
            /* the deletion of an older file of this name stays recorded: the commit deletes it, then moves the new one in */
        }
    }
    fs_unlock();
    RET(r);
}
static void fs_DeleteFile(Ctx *c) {
    char rel[PATH_MAX], path[PATH_MAX], w[PATH_MAX], cm[PATH_MAX];
    Mount *m;
    fs_lock();
    uint32_t r = split_path(STR(0), &m, rel);
    if (!r && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r && (!view_path(m, rel, path) || is_dir(path))) r = RESULT_PATH_NOT_FOUND;
    if (!r) {
        join(w, m->work, rel); join(cm, m->root, rel);
        if (exists(w)) unlink(w);
        if (exists(cm)) tomb_add(m, rel);
    }
    fs_unlock();
    RET(r);
}
static void fs_CreateDirectory(Ctx *c) {
    char rel[PATH_MAX], path[PATH_MAX];
    Mount *m;
    fs_lock();
    uint32_t r = create_target(STR(0), &m, rel, path);
    if (!r && !plat_mkdir(path)) r = RESULT_GENERIC_FAILURE;
    fs_unlock();
    RET(r);
}
static void fs_GetEntryType(Ctx *c) {                 /* (DirectoryEntryType *out, path): 0 directory, 1 file */
    char rel[PATH_MAX], path[PATH_MAX];
    Mount *m;
    fs_lock();
    uint32_t r = split_path(STR(1), &m, rel);
    if (!r && !view_path(m, rel, path)) r = RESULT_PATH_NOT_FOUND;
    fs_unlock();
    {   static int on = -1; if (on < 0) on = getenv("SMO_FSLOG") != NULL;       /* SMO debug: every entry-type query on stdout */
        if (on) printf("fslog: entry type \"%s\" -> %s\n", STR(1), r ? "NOT FOUND" : is_dir(path) ? "directory" : "file"); }
    if (r) { RET(r); return; }
    wr32(A0, is_dir(path) ? 0 : 1);
    RET(0);
}
static void fs_OpenDirectory(Ctx *c) { wr64(A0, 0); RET(RESULT_PATH_NOT_FOUND); }   /* replaced by nnfsdir.c */

/* ---- self-test of the mount table and of the save data journal, in a scratch folder <exe folder>/../tmp (SMO_SDK_SELFTEST) */
uint32_t fs_mount_plain(const char *name, const char *host_dir) { fs_roots(); return mount_add(name, MK_PLAIN, host_dir); }
static int st_host_is(const char *path, const char *text) {
    char buf[64] = "";
    FILE *f = fopen(path, "rb");
    if (!f) return text == NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return text && strcmp(buf, text) == 0;
}
static uint32_t st_write(Ctx *c, const char *guest, const char *text, uint64_t *keep_open) {
    uint64_t h = 0;
    uint32_t option = 1;
    c->x[0] = (uint64_t)(uintptr_t)&h; c->x[1] = (uint64_t)(uintptr_t)guest; c->x[2] = 7; fs_OpenFile(c);
    if (c->x[0]) return (uint32_t)c->x[0];
    c->x[0] = h; c->x[1] = 0; c->x[2] = (uint64_t)(uintptr_t)text; c->x[3] = strlen(text); c->x[4] = (uint64_t)(uintptr_t)&option; fs_WriteFile(c);
    uint32_t r = (uint32_t)c->x[0];
    if (keep_open) *keep_open = h; else { c->x[0] = h; fs_CloseFile(c); }
    return r;
}
static int st_read_is(Ctx *c, const char *guest, const char *text) {
    uint64_t h = 0, got = 0;
    char buf[64] = "";
    c->x[0] = (uint64_t)(uintptr_t)&h; c->x[1] = (uint64_t)(uintptr_t)guest; c->x[2] = 1; fs_OpenFile(c);
    if (c->x[0]) return text == NULL;
    c->x[0] = (uint64_t)(uintptr_t)&got; c->x[1] = h; c->x[2] = 0; c->x[3] = (uint64_t)(uintptr_t)buf; c->x[4] = sizeof buf - 1; fs_ReadFileCount(c);
    c->x[0] = h; fs_CloseFile(c);
    buf[got] = 0;
    return text && strcmp(buf, text) == 0;
}
int nnfs_selftest(Ctx *c) {
    char saved_root[PATH_MAX], t[PATH_MAX], save[PATH_MAX], a[PATH_MAX], n[PATH_MAX], work[PATH_MAX], marker[PATH_MAX], journal[PATH_MAX];
    int bad = 0;
    uint64_t uid[2] = {1, 0}, h = 0;
    #define FSCHECK(cond, text) do { if (!(cond)) { printf("  FAIL fs journal: %s\n", text); bad++; } } while (0)
    fs_roots();
    strcpy(saved_root, g_user_root);
    snprintf(t, sizeof t - 64, "%s/../tmp", plat_exe_dir());
    plat_mkdirs(t);
    snprintf(t + strlen(t), 64, "/smo_fs_selftest_%lu", (unsigned long)getpid());
    remove_tree(t);
    plat_mkdir(t);
    strcpy(g_user_root, t);
    snprintf(save, sizeof save, "%s/save", t); plat_mkdir(save);
    snprintf(a, sizeof a, "%s/a.bin", save); snprintf(n, sizeof n, "%s/sub/new.bin", save);
    snprintf(work, sizeof work, "%s.work.%lu", save, (unsigned long)getpid());
    snprintf(marker, sizeof marker, "%s.commit", save); snprintf(journal, sizeof journal, "%s.journal.%lu", save, (unsigned long)getpid());
    FILE *f = fopen(a, "wb"); if (f) { fputs("committed", f); fclose(f); }

    c->x[0] = (uint64_t)(uintptr_t)"nosuch:/a.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(c->x[0] == RESULT_NOT_MOUNTED, "a path with an unmounted name is not NotMounted");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(c->x[0] == 0, "MountSaveData failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(c->x[0] == RESULT_MOUNT_NAME_EXISTS, "mounting the same name twice is not MountNameAlreadyExists");
    /* 1. a write is visible to the game and NOT in the committed folder */
    FSCHECK(st_write(c, "st:/a.bin", "CHANGED!!", NULL) == 0, "write failed");
    FSCHECK(st_read_is(c, "st:/a.bin", "CHANGED!!"), "the game does not read back what it wrote");
    FSCHECK(st_host_is(a, "committed"), "an uncommitted write reached the committed folder");
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub"; fs_CreateDirectory(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(c->x[0] == 0 && !exists(n), "a new file went straight to the committed folder");
    /* 2. unmount without commit: everything is rolled back */
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Unmount(c);
    FSCHECK(!exists(work), "the working folder survived Unmount");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(st_read_is(c, "st:/a.bin", "committed") && st_read_is(c, "st:/sub/new.bin", NULL), "uncommitted changes survived a remount");
    /* 3. commit is refused while a file is open for writing, then applies everything */
    FSCHECK(st_write(c, "st:/a.bin", "SECOND!!!", &h) == 0, "write failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == RESULT_WRITE_FILE_OPEN && st_host_is(a, "committed"), "Commit with a file open for writing was not refused");
    c->x[0] = h; fs_CloseFile(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub"; fs_CreateDirectory(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(st_write(c, "st:/sub/new.bin", "", NULL) == 0, "open of a created file failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == 0 && st_host_is(a, "SECOND!!!") && exists(n) && !exists(marker), "Commit did not apply the changes");
    FSCHECK(st_read_is(c, "st:/a.bin", "SECOND!!!"), "the committed file is not read back after Commit");
    /* 4. deletion: hidden at once, done on the host at Commit */
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; fs_DeleteFile(c);
    FSCHECK(c->x[0] == 0 && exists(n) && st_read_is(c, "st:/sub/new.bin", NULL), "DeleteFile is not journaled");
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; fs_DeleteFile(c);
    FSCHECK(c->x[0] == RESULT_PATH_NOT_FOUND, "deleting a deleted file is not PathNotFound");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == 0 && !exists(n), "a committed deletion did not reach the host");
    /* 5. a process killed after the commit was decided: the next mount finishes it */
    FSCHECK(st_write(c, "st:/a.bin", "THIRD!!!!", NULL) == 0, "write failed");
    f = fopen(journal, "w");
    if (f) { fprintf(f, "W\t%s\nR\t%s\n", work, save); journal_list(f, work, ""); fclose(f); }
    plat_rename_replace(journal, marker);
    FSCHECK(st_host_is(a, "SECOND!!!") && exists(marker), "test setup: decided commit");
    journal_recover(save);
    FSCHECK(st_host_is(a, "THIRD!!!!") && !exists(marker), "a decided commit was not rolled forward at the next mount");
    /* 6. a process killed before the commit was decided: its working folder is dropped (a pid above pid_max: never alive) */
    {
        char dead[PATH_MAX];
        snprintf(dead, sizeof dead, "%s.work.2147483646", save);
        plat_mkdir(dead);
        snprintf(dead + strlen(dead), 32, "/a.bin");
        f = fopen(dead, "wb"); if (f) { fputs("LOST", f); fclose(f); }
        journal_recover(save);
        dead[strlen(dead) - 6] = 0;
        FSCHECK(!exists(dead) && st_host_is(a, "THIRD!!!!"), "the working folder of a dead process was not discarded");
    }
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Unmount(c);
    remove_tree(t);
    strcpy(g_user_root, saved_root);
    printf("  fs: mount table (NotMounted, MountNameAlreadyExists) and save data journal (copy-on-write, rollback at Unmount, Commit refused with a\n"
           "      writer open, two-phase Commit, journaled deletion, roll-forward of a decided commit, discard of a dead process's work): %s\n", bad ? "FAILED" : "ok");
    #undef FSCHECK
    return bad;
}

/* ---- nn::account: one local user with a fixed id */
static void acc_GetUserId(Ctx *c) { wr64(A0, 1); wr64(A0 + 8, 0); RET(0); }
/* [doc] OpenPreselectedUser(UserHandle*): the user chosen at start-up, opened (the SDK aborts if there is none; here
 * there always is the one local user {1, 0}). CloseUser(UserHandle&) closes it. GetLastOpenedUser(Uid*): the user
 * opened most recently on the console = {1, 0}. */
static void acc_OpenPreselectedUser(Ctx *c) { wr64(A0, 1); wr64(A0 + 8, 0); wr64(A0 + 16, 0); RET(1); }
static void acc_CloseUser(Ctx *c) { (void)c; }
static void acc_GetLastOpenedUser(Ctx *c) { wr64(A0, 1); wr64(A0 + 8, 0); RET(0); }
/* The nickname of the console user is the user's full name from the password database, else the login name
 * (SMO_NICKNAME overrides): UTF-8, at most 32
 * bytes, NUL-terminated, in the 33-byte Nickname. */
static void acc_GetNickname(Ctx *c) {
    static char name[33];
    static volatile int32_t ready;
    if (plat_cas(&ready, 1, 0) == 0) {
        const char *env = getenv("SMO_NICKNAME");
        char utf8[512] = "";
        if (env && env[0]) snprintf(utf8, sizeof utf8, "%s", env);
        else {
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_gecos && pw->pw_gecos[0] && pw->pw_gecos[0] != ',') { snprintf(utf8, sizeof utf8, "%s", pw->pw_gecos); utf8[strcspn(utf8, ",")] = 0; }
            if (!utf8[0] && pw && pw->pw_name) snprintf(utf8, sizeof utf8, "%s", pw->pw_name);
            if (!utf8[0] && getenv("USER")) snprintf(utf8, sizeof utf8, "%s", getenv("USER"));
        }
        if (!utf8[0]) strcpy(utf8, "Player");
        size_t len = strlen(utf8);
        if (len > 32) { len = 32; while (len > 0 && ((unsigned char)utf8[len] & 0xc0) == 0x80) len--; }   /* cut between characters */
        memcpy(name, utf8, len);
        name[len] = 0;
        ready = 2;
    }
    while (ready != 2) plat_yield();
    memset((void *)(uintptr_t)A0, 0, 33);
    strcpy((char *)(uintptr_t)A0, name);
    RET(0);
}
static void acc_out_true(Ctx *c) { wr8(A0, 1); RET(0); }
static void acc_out_false(Ctx *c) { wr8(A0, 0); RET(0); }
/* [sdk] 0x4b3a8 (EnsureNetworkServiceAccountAvailable): the SDK treats the account results 124-0200 .. 124-0269 from
 * the service as "no usable network service account" (it then offers the account-linking applet and returns the
 * result). An offline machine has no linked account: 124-0200 = 0x1907c, the first code of that range, is returned
 * (NOT VERIFIED: which code of the range a console gives; the linking applet is not shown, it needs the network). */
#define RESULT_ACCOUNT_NSA_UNAVAILABLE 0x1907cull
static void acc_unavailable(Ctx *c) { RET(RESULT_ACCOUNT_NSA_UNAVAILABLE); }
static void keep(Ctx *c) { (void)c; }

/* ---- nn::nifm: no network */
static void nifm_false(Ctx *c) { RET(0); }

#define FS(n) "_ZN2nn2fs" n
#define AC(n) "_ZN2nn7account" n
#define NI(n) "_ZN2nn4nifm" n
static const ImportDef fs_imports[] = {
    {FS("8MountRomEPKcPvm"), fs_MountRom}, {FS("13MountSaveDataEPKcRKNS_7account3UidE"), fs_MountSaveData},
    {FS("7UnmountEPKc"), fs_Unmount}, {FS("22QueryMountRomCacheSizeEPm"), fs_QueryMountRomCacheSize}, {FS("14CommitSaveDataEPKc"), fs_Commit} /* the 3.5.1 name; [sdk 0x4e72c] detail::Find + FileSystemAccessor::Commit, as Commit */, {FS("14EnsureSaveDataERKNS_7account3UidE"), fs_EnsureSaveData},
    {FS("8OpenFileEPNS0_10FileHandleEPKci"), fs_OpenFile}, {FS("9CloseFileENS0_10FileHandleE"), fs_CloseFile},
    {FS("8ReadFileEPmNS0_10FileHandleElPvm"), fs_ReadFileCount},
    {FS("9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE"), fs_WriteFile}, {FS("11GetFileSizeEPlNS0_10FileHandleE"), fs_GetFileSize},
    {FS("11SetFileSizeENS0_10FileHandleEl"), fs_SetFileSize}, {FS("9FlushFileENS0_10FileHandleE"), fs_FlushFile},
    {FS("10CreateFileEPKcl"), fs_CreateFile}, {FS("10DeleteFileEPKc"), fs_DeleteFile}, {FS("15CreateDirectoryEPKc"), fs_CreateDirectory},
    {FS("12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc"), fs_GetEntryType}, {FS("13OpenDirectoryEPNS0_15DirectoryHandleEPKci"), fs_OpenDirectory},
    {AC("10InitializeEv"), fs_ok},
    {AC("19OpenPreselectedUserEPNS0_10UserHandleE"), acc_OpenPreselectedUser},
    {AC("9CloseUserERKNS0_10UserHandleE"), acc_CloseUser}, {AC("17GetLastOpenedUserEPNS0_3UidE"), acc_GetLastOpenedUser},
    {FS("8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE"), fs_ReadFileCount},   /* ReadOption (x5) has no flags in this SDK */
    {AC("9GetUserIdEPNS0_3UidERKNS0_10UserHandleE"), acc_GetUserId}, {AC("11GetNicknameEPNS0_8NicknameERKNS0_3UidE"), acc_GetNickname},
    {AC("32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE"), acc_out_false},
    {AC("36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE"), acc_unavailable},
    {AC("12AsyncContextC1Ev"), keep}, {AC("12AsyncContextD1Ev"), keep},
    {AC("12AsyncContext7HasDoneEPb"), acc_out_true}, {AC("12AsyncContext9GetResultEv"), acc_unavailable},
    {NI("10InitializeEv"), fs_ok}, {NI("18IsNetworkAvailableEv"), nifm_false}, {NI("22IsNetworkRequestOnHoldEv"), nifm_false},
    {NI("20SubmitNetworkRequestEv"), keep}, {NI("27SubmitNetworkRequestAndWaitEv"), keep},
};

void register_fs_imports(void) {
    register_imports(fs_imports, (int)(sizeof fs_imports / sizeof fs_imports[0]));
}

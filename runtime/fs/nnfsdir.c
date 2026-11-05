/* nn::fs directory functions on host folders: OpenDirectory, ReadDirectory, CloseDirectory.
 *
 * nnfs.c has OpenDirectory as "path not found" and not the other two. A directory that is never found is wrong for
 * save data the game has written itself, so OpenDirectory is REPLACED here (this file registers after nnfs.c) and
 * the rest added.
 * The mount-name mapping is the one documented at the top of nnfs.c and must stay identical to it:
 *   rom   -> %SMO_ROMFS% or ..\romfs,   any other mount -> ..\userdata\<mount name>
 *
 * [game] 0x71037c4360: OpenDirectory(&handle, path, 3); GetDirectoryEntryCount(&count, handle);
 * allocates count * 0x310 bytes; ReadDirectory(&read, entries, handle, count); so DirectoryHandle is one
 * 8-byte value passed in a register and sizeof(DirectoryEntry) = 0x310.
 * [doc] nnheaders nn/fs: DirectoryEntry { char name[0x301]; char pad[3]; s8 type @0x304 (0 directory,
 * 1 file); s64 fileSize @0x308 }; OpenDirectoryMode 1 = directories, 2 = files, 3 = all.
 * Results (module 2): [doc] switchbrew: PathNotFound 2002-0001 (0x202) when the folder is missing; the generic
 * failure nnfs.c uses (0x3e802) for a handle that is not an open directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "plat.h"
#include "sdklift.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(v) ((uint64_t)(uintptr_t)(v))

#define RESULT_PATH_NOT_FOUND 0x202u
#define RESULT_GENERIC_FAILURE 0x3e802u
#define ENTRY_SIZE 0x310
#define DIR_MAGIC 0x52494453u                             /* "SDIR" */

typedef struct { char name[0x301]; int is_file; int64_t size; } HostEntry;
typedef struct { uint32_t magic; int count, pos; HostEntry *entries; } HostDir;

#define FS_DIR_ROOTS 18                             /* nnfs.c: up to 16 RomFS mods, or a save data working folder, + the mount's own */
int fs_dir_roots(const char *guest, char roots[][PATH_MAX], uint32_t *result);   /* nnfs.c: mount table + save data journal view + RomFS mods */
int fs_dir_hidden(const char *guest_dir, const char *name);

static int entry_cmp(const void *a, const void *b) { return strcasecmp(((const HostEntry *)a)->name, ((const HostEntry *)b)->name); }
static void fs_OpenDirectory(Ctx *c) {                    /* (DirectoryHandle *out, const char *path, int mode) -> Result */
    uint64_t out = A0;
    int mode = (int)A2;
    char (*roots)[PATH_MAX] = malloc(sizeof(char[FS_DIR_ROOTS][PATH_MAX])), pattern[PATH_MAX + 4];
    uint32_t result = 0;
    if (out) wr64(out, 0);
    /* the folders that make up this directory: for journaled save data the working folder, then the committed one */
    int nroots = fs_dir_roots((const char *)(uintptr_t)A1, roots, &result);
    if (result) { free(roots); RET(result); return; }
    HostDir *d = calloc(1, sizeof *d);
    int cap = 0;
    d->magic = DIR_MAGIC;
    for (int r = 0; r < nroots; r++) {
        DIR *dh = opendir(roots[r]);
        if (!dh) continue;
        struct dirent *de;
        while ((de = readdir(dh))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            struct stat st;
            snprintf(pattern, sizeof pattern, "%s/%s", roots[r], de->d_name);
            if (stat(pattern, &st)) continue;
            int is_dir = S_ISDIR(st.st_mode), dup = 0;
            char name[0x301];
            memset(name, 0, sizeof name);
            if (strlen(de->d_name) > 0x300) continue;
            strcpy(name, de->d_name);                        /* UTF-8 already */
            if (r == nroots - 1 && nroots == 2) {            /* committed folder under a working folder: skip what the working state replaced or deleted */
                if (fs_dir_hidden((const char *)(uintptr_t)A1, de->d_name)) continue;
            }
            for (int k = 0; k < d->count && !dup; k++) dup = strcasecmp(d->entries[k].name, name) == 0;
            if (dup) continue;
            if (is_dir ? !(mode & 1) : !(mode & 2)) {
                if (nroots < 2) continue;                    /* still remembered (below) so the committed twin is not listed either */
            }
            if (d->count == cap) { cap = cap ? cap * 2 : 32; d->entries = realloc(d->entries, sizeof(HostEntry) * (size_t)cap); }
            HostEntry *e = &d->entries[d->count];
            memset(e, 0, sizeof *e);
            memcpy(e->name, name, sizeof e->name);
            e->is_file = !is_dir;
            e->size = is_dir ? 0 : (int64_t)st.st_size;
            d->count++;
        }
        closedir(dh);
    }
    /* readdir order depends on the file system: the entries are sorted by name without regard to case (the order NTFS
     * listed them in), so every run and every PC sees the same order */
    if (d->count > 1) qsort(d->entries, (size_t)d->count, sizeof(HostEntry), entry_cmp);
    if (nroots >= 2) {                                       /* drop the entries the open mode does not ask for (several folders: save data's two, or RomFS mods over the game's) */
        int n = 0;
        for (int k = 0; k < d->count; k++) if (d->entries[k].is_file ? (mode & 2) : (mode & 1)) d->entries[n++] = d->entries[k];
        d->count = n;
    }

    trace_printf("     fs OpenDirectory \"%s\" mode %d -> %d entries\n", (const char *)(uintptr_t)A1, mode, d->count);
    wr64(out, P(d));
    free(roots);
    RET(0);
}
static HostDir *dir_of(uint64_t handle) {
    HostDir *d = (HostDir *)(uintptr_t)handle;
    return handle > 0x10000 && d->magic == DIR_MAGIC ? d : NULL;
}
static void fs_ReadDirectory(Ctx *c) {                    /* (int64_t *outCount, DirectoryEntry *entries, DirectoryHandle, int64_t max) -> Result */
    HostDir *d = dir_of(A2);
    uint64_t out = A1;
    int64_t max = (int64_t)A3, n = 0;
    if (!d) { if (A0) wr64(A0, 0); RET(RESULT_GENERIC_FAILURE); return; }
    while (n < max && d->pos < d->count) {
        HostEntry *e = &d->entries[d->pos++];
        uint8_t *g = (uint8_t *)(uintptr_t)(out + (uint64_t)n * ENTRY_SIZE);
        memset(g, 0, ENTRY_SIZE);
        memcpy(g, e->name, 0x301);
        g[0x304] = (uint8_t)e->is_file;
        memcpy(g + 0x308, &e->size, 8);
        n++;
    }
    if (A0) wr64(A0, (uint64_t)n);
    RET(0);
}
static void fs_CloseDirectory(Ctx *c) {                   /* (DirectoryHandle) */
    HostDir *d = dir_of(A0);
    if (d) { d->magic = 0; free(d->entries); free(d); }
}

#define FS(n) "_ZN2nn2fs" n
static const ImportDef fsdir_imports[] = {
    {FS("13OpenDirectoryEPNS0_15DirectoryHandleEPKci"), fs_OpenDirectory},
    {FS("13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl"), fs_ReadDirectory},
    {FS("14CloseDirectoryENS0_15DirectoryHandleE"), fs_CloseDirectory},
};
void register_nnfsdir_imports(void) {
    int n = (int)(sizeof fsdir_imports / sizeof fsdir_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(fsdir_imports[i].name)) printf("nnfsdir: \"%s\" is not an import of the game (a typo, or a function it never calls)\n", fsdir_imports[i].name);
    register_imports(fsdir_imports, n);
}

int nnfsdir_selftest(Ctx *c) {
    /* a scratch mount under <exe folder>/../tmp (never the user's save data): two files and one folder, listed with each mode */
    int ok = 1;
    char base[PATH_MAX], p[PATH_MAX];
    uint32_t fs_mount_plain(const char *name, const char *host_dir);
    snprintf(base, sizeof base - 64, "%s/../tmp", plat_exe_dir());
    plat_mkdirs(base);
    snprintf(base + strlen(base), 64, "/smo_fsdir_selftest_%lu", (unsigned long)getpid());
    plat_mkdir(base);
    snprintf(p, sizeof p, "%s/d", base); plat_mkdir(p);
    snprintf(p, sizeof p, "%s/d/sub", base); plat_mkdir(p);
    snprintf(p, sizeof p, "%s/d/a.bin", base);
    FILE *f = fopen(p, "wb"); if (f) { fwrite("12345", 1, 5, f); fclose(f); }
    snprintf(p, sizeof p, "%s/d/b.bin", base);
    f = fopen(p, "wb"); if (f) fclose(f);
    ok = fs_mount_plain("sdkselftest", base) == 0;
    static const int expect[4] = {0, 1, 2, 3};
    for (int mode = 1; mode <= 3; mode++) {
        uint64_t handle = 0;
        int64_t got = -1;
        uint8_t *entries = malloc(4 * ENTRY_SIZE + 16);
        memset(entries, 0xee, 4 * ENTRY_SIZE + 16);
        c->x[0] = P(&handle); c->x[1] = P("sdkselftest:/d"); c->x[2] = (uint64_t)mode; fs_OpenDirectory(c);
        ok = ok && c->x[0] == 0 && handle;
        c->x[0] = P(&got); c->x[1] = P(entries); c->x[2] = handle; c->x[3] = 4; fs_ReadDirectory(c);
        ok = ok && c->x[0] == 0 && got == expect[mode];
        for (int i = 0; i < got && ok; i++) {
            const char *name = (const char *)entries + i * ENTRY_SIZE;
            int is_file = entries[i * ENTRY_SIZE + 0x304];
            int64_t size;
            memcpy(&size, entries + i * ENTRY_SIZE + 0x308, 8);
            if (!strcmp(name, "a.bin")) ok = is_file == 1 && size == 5;
            else if (!strcmp(name, "b.bin")) ok = is_file == 1 && size == 0;
            else if (!strcmp(name, "sub")) ok = is_file == 0;
            else ok = 0;
        }
        for (int i = 0; i < 16; i++) if (entries[got * ENTRY_SIZE + i] != 0xee) ok = 0;
        c->x[0] = P(&got); c->x[1] = P(entries); c->x[2] = handle; c->x[3] = 4; fs_ReadDirectory(c);
        ok = ok && got == 0;                               /* the listing is consumed */
        c->x[0] = handle; fs_CloseDirectory(c);
        free(entries);
    }
    uint64_t handle = 1;
    c->x[0] = P(&handle); c->x[1] = P("sdkselftest:/missing"); c->x[2] = 3; fs_OpenDirectory(c);
    ok = ok && c->x[0] == RESULT_PATH_NOT_FOUND && handle == 0;
    snprintf(p, sizeof p, "%s/d/a.bin", base); unlink(p);
    snprintf(p, sizeof p, "%s/d/b.bin", base); unlink(p);
    snprintf(p, sizeof p, "%s/d/sub", base); rmdir(p);
    snprintf(p, sizeof p, "%s/d", base); rmdir(p);
    rmdir(base);
    printf("  fs: OpenDirectory / ReadDirectory (modes 1, 2, 3; 0x310-byte entries): %s\n", ok ? "ok" : "FAILED");
    return !ok;
}

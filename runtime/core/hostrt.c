/* Host runtime for the fully lifted game: image loader, relocation, indirect
 * call dispatch, import dispatch with tracing, and crash reporting. */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"

/* the image constants come from build/gen/image.h (generated from the dump) */
#define FILE_OFF 0x1000ULL
#define FILE_IMAGE GUEST_FILE_IMAGE
#define IMAGE_SIZE GUEST_IMAGE_SIZE
#define RELA GUEST_RELA
#define RELASZ GUEST_RELASZ
#define JMPREL GUEST_JMPREL
#define PLTRELSZ GUEST_PLTRELSZ

static ImportFn *g_handlers;
static unsigned long long *g_import_calls;
static int *g_import_order, g_import_order_count;
static FILE *g_trace;
static unsigned long long g_trace_seq;
static FnEntry *g_by_host;                 /* fn_table sorted by host code address */
void sdk_dispatch(Ctx *c, uint64_t addr);          /* sdklift_rt.c: code lifted from sdk.elf at 0x75xxxxxxxx */

void trace_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_trace) { vfprintf(g_trace, fmt, ap); fflush(g_trace); }
    va_end(ap);
}

/* ---------------------------------------------------------------- lookup */
/* guest address -> lifted function through a hash table built in runtime_init (open addressing, load below 1/2);
 * before that, and as the reference, the binary search over fn_table (sorted by address). Same answer either way. */
static struct { uint64_t addr; void (*fn)(Ctx *); } *g_fn_hash;
static unsigned g_fn_mask;
static inline unsigned fn_hash(uint64_t addr) { return (unsigned)(((addr >> 2) * 0x9E3779B97F4A7C15ULL) >> 40); }
static void fn_hash_build(void) {
    unsigned size = 1024;
    while (size < (unsigned)fn_table_count * 2u) size <<= 1;
    g_fn_hash = calloc(size, sizeof *g_fn_hash);
    for (int i = 0; i < fn_table_count; i++) {
        unsigned h = fn_hash(fn_table[i].addr) & (size - 1);
        while (g_fn_hash[h].addr) h = (h + 1) & (size - 1);
        g_fn_hash[h].addr = fn_table[i].addr; g_fn_hash[h].fn = fn_table[i].fn;
    }
    g_fn_mask = size - 1;
}
static void (*lookup_guest(uint64_t addr))(Ctx *) {
    if (g_fn_mask) {
        for (unsigned h = fn_hash(addr) & g_fn_mask;; h = (h + 1) & g_fn_mask) {
            if (g_fn_hash[h].addr == addr) return g_fn_hash[h].fn;
            if (!g_fn_hash[h].addr) return NULL;
        }
    }
    int lo = 0, hi = fn_table_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fn_table[mid].addr == addr) return fn_table[mid].fn;
        if (fn_table[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static int cmp_host(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const FnEntry *)a)->fn, y = (uintptr_t)((const FnEntry *)b)->fn;
    return x < y ? -1 : x > y;
}

/* Guest function whose host code contains `rip`, or 0. */
uint64_t guest_function_of(uint64_t rip) {
    int lo = 0, hi = fn_table_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if ((uintptr_t)g_by_host[mid].fn <= rip) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0 || rip - (uintptr_t)g_by_host[best].fn > 0x400000) return 0;
    return g_by_host[best].addr;
}

/* Where the host code of the lifted function starting at `fn` ends: the start of the next lifted function in host
 * memory (what the Windows build read from the function's unwind entry), or 0 when fn is not a lifted function's start
 * or is the last one. */
uintptr_t host_fn_end(uintptr_t fn) {
    if (!g_by_host) return 0;
    int lo = 0, hi = fn_table_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uintptr_t f = (uintptr_t)g_by_host[mid].fn;
        if (f == fn) {
            while (mid + 1 < fn_table_count && (uintptr_t)g_by_host[mid + 1].fn == fn) mid++;   /* aliases: one body */
            return mid + 1 < fn_table_count ? (uintptr_t)g_by_host[mid + 1].fn : 0;
        }
        if (f < fn) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- host functions handed to the guest
 * Function pointers the guest obtains at run time (for example through nvnBootstrapLoader) are
 * placeholder addresses in their own range; calling one runs the registered host handler. */
typedef struct { char name[72]; ImportFn fn; volatile int64_t calls; int64_t first; } HostFn;
#define MAX_HOSTFN 4096
static HostFn g_hostfn[MAX_HOSTFN];
static volatile int32_t g_hostfn_count;
static volatile int64_t g_hostfn_seq;
static plat_rwlock_t g_hostfn_lock = PLAT_RWLOCK_INIT;
static _Thread_local int t_hostfn;

static void audit_hostfn(const char *name, ImportFn fn);
static void audit_exit_counts(void);
uint64_t hostfn_address(const char *name, ImportFn fn) {
    plat_rw_wlock(&g_hostfn_lock);
    int i, fresh = 0;
    for (i = 0; i < g_hostfn_count; i++) if (strcmp(g_hostfn[i].name, name) == 0) break;
    if (i == g_hostfn_count && i < MAX_HOSTFN) {
        strncpy(g_hostfn[i].name, name, sizeof g_hostfn[i].name - 1);
        g_hostfn[i].fn = fn;
        g_hostfn_count = i + 1;
        fresh = 1;
    }
    plat_rw_wunlock(&g_hostfn_lock);
    if (fresh) audit_hostfn(name, fn);
    return HOSTFN_BASE + (uint64_t)i * 16;
}
const char *hostfn_current_name(void) { return g_hostfn[t_hostfn].name; }
unsigned long long hostfn_current_calls(void) { return (unsigned long long)g_hostfn[t_hostfn].calls; }

void print_hostfn_summary(FILE *out) {
    int n = g_hostfn_count, used = 0;
    int *order = malloc(sizeof(int) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) if (g_hostfn[i].calls) order[used++] = i;
    for (int i = 1; i < used; i++)
        for (int j = i; j > 0 && g_hostfn[order[j]].first < g_hostfn[order[j - 1]].first; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    fprintf(out, "\nrun-time host functions: %d handed out, %d called, in order of first call:\n", n, used);
    for (int i = 0; i < used; i++) fprintf(out, "  %8lld  %s\n", (long long)g_hostfn[order[i]].calls, g_hostfn[order[i]].name);
    free(order);
}

/* Called on every failed exclusive store; reports a loop that never succeeds. */
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {
    if (++c->ex_fail == 1000 || c->ex_fail == 100000) {
        printf("STXR failing %llu times in f_%llx: addr=%llx expected=%llx seen=%llx new=%llx (%s%d)\n",
               (unsigned long long)c->ex_fail, (unsigned long long)c->fn, (unsigned long long)a, (unsigned long long)expected,
               (unsigned long long)seen, (unsigned long long)v, bits < 0 ? "monitor lost, " : "", bits < 0 ? -bits : bits);
        trace_printf("STXR failing %llu times in f_%llx: addr=%llx expected=%llx seen=%llx new=%llx\n",
                     (unsigned long long)c->ex_fail, (unsigned long long)c->fn, (unsigned long long)a, (unsigned long long)expected,
                     (unsigned long long)seen, (unsigned long long)v);
    }
}

/* Debug aid: SMO_ICALLS=<start s>,<dur s> records every distinct (indirect call target, return address, context)
 * seen in that window with a count and the first x0..x2; icall_dump() prints them (boot.c, at the time limit). */
#define ICALL_MAX 16384
static struct { uint64_t target, lr, ctx, x0, x1, x2; long long n; } g_icall[ICALL_MAX];
static volatile int32_t g_icall_lock;
static int g_icall_mode = -1;
static uint64_t g_icall_start, g_icall_end;
static unsigned long long g_poke_fn, g_poke_off;
static unsigned g_poke_val;
static void icall_record(Ctx *c, uint64_t addr) {
    if (g_icall_mode < 0) {
        int s = 0, d = 0;
        const char *e = getenv("SMO_ICALLS");
        g_icall_mode = e && sscanf(e, "%d,%d", &s, &d) == 2 && d > 0;
        g_icall_start = plat_ms() + (uint64_t)s * 1000;
        g_icall_end = g_icall_start + (uint64_t)d * 1000;
        /* Experiment only (changes game DATA, never use for normal runs): SMO_EXPERIMENT_POKE=<fn hex>:<off hex>=<byte hex>
         * writes that byte at x0+off each time fn is called indirectly. */
        e = getenv("SMO_EXPERIMENT_POKE");
        if (e && sscanf(e, "%llx:%llx=%x", &g_poke_fn, &g_poke_off, &g_poke_val) == 3) { if (!g_icall_mode) g_icall_mode = 2; }
        else g_poke_fn = 0;
    }
    if (g_poke_fn && addr == g_poke_fn && rd8(c->x[0] + g_poke_off) != (uint8_t)g_poke_val) {
        printf("[poke] f_%llx: byte at x0+0x%llx (%llx) %02x -> %02x\n", g_poke_fn, g_poke_off, (unsigned long long)(c->x[0] + g_poke_off),
               rd8(c->x[0] + g_poke_off), g_poke_val);
        wr8(c->x[0] + g_poke_off, (uint8_t)g_poke_val);
    }
    if (g_icall_mode != 1) return;
    uint64_t now = plat_ms();
    if (now < g_icall_start || now > g_icall_end) return;
    uint64_t lr = c->x[30], key = addr * 31 + lr * 7 + (uint64_t)(uintptr_t)c;
    unsigned h = (unsigned)((key >> 2) * 2654435761u) % ICALL_MAX;
    while (plat_cas(&g_icall_lock, 1, 0)) plat_pause();
    for (int probe = 0; probe < ICALL_MAX; probe++, h = (h + 1) % ICALL_MAX) {
        if (g_icall[h].target == addr && g_icall[h].lr == lr && g_icall[h].ctx == (uint64_t)(uintptr_t)c) { g_icall[h].n++; break; }
        if (!g_icall[h].target) {
            g_icall[h].target = addr; g_icall[h].lr = lr; g_icall[h].ctx = (uint64_t)(uintptr_t)c;
            g_icall[h].x0 = c->x[0]; g_icall[h].x1 = c->x[1]; g_icall[h].x2 = c->x[2]; g_icall[h].n = 1;
            break;
        }
    }
    g_icall_lock = 0;
}
uint64_t icall_first_x0(uint64_t target) {
    for (int i = 0; i < ICALL_MAX; i++) if (g_icall[i].target == target) return g_icall[i].x0;
    return 0;
}
void icall_dump(FILE *out, uint64_t main_ctx) {
    if (g_icall_mode <= 0) return;
    fprintf(out, "\n[icall] indirect calls seen in the window (M = main thread)\n");
    for (int i = 0; i < ICALL_MAX; i++)
        if (g_icall[i].target)
            fprintf(out, "[icall] %c f_%llx lr=%llx n=%lld x0=%llx x1=%llx x2=%llx\n", g_icall[i].ctx == main_ctx ? 'M' : '-',
                    (unsigned long long)g_icall[i].target, (unsigned long long)g_icall[i].lr, g_icall[i].n,
                    (unsigned long long)g_icall[i].x0, (unsigned long long)g_icall[i].x1, (unsigned long long)g_icall[i].x2);
}

extern int g_nerve_mode;                              /* smo_nerve.c */
extern volatile uint64_t g_nerve_watch[2], g_stage_init, g_stage_play;
void nerve_call(Ctx *c, uint64_t addr);
void nerve_watch_hit(void);
void stage_init_hit(Ctx *c);
void stage_play_hit(Ctx *c);

void call_indirect(Ctx *c, uint64_t addr) {
    if (g_icall_mode) icall_record(c, addr);
    if (g_nerve_mode) nerve_call(c, addr);             /* smo_nerve.c: SMO_NERVELOG, SMO_ACTORPOS, SMO_SEQ @nerve: */
    if (g_nerve_watch[0] && (addr == g_nerve_watch[0] || addr == g_nerve_watch[1])) nerve_watch_hit();   /* smo_nerve.c: the amiibo menu's watch */
    if (g_stage_init && addr == g_stage_init) stage_init_hit(c);      /* smo_nerve.c: SMO_START_STAGE, SMO_STAGE_LIST (TEST ONLY): a stage scene starts */
    if (g_stage_play && addr == g_stage_play) stage_play_hit(c);      /* smo_nerve.c: SMO_STAGE_LIST (TEST ONLY): the stage change to the next stage */
    if (addr - GUEST_BASE < IMAGE_SIZE) {            /* the common case first (a function of main); the ranges tested below do not overlap it */
        void (*fn)(Ctx *) = lookup_guest(addr);
        if (!fn) guest_trap("indirect call to an address that is not a known function", addr);
        fn(c);
        return;
    }
    if (addr >= HOSTFN_BASE && addr < HOSTFN_BASE + (uint64_t)g_hostfn_count * 16) {
        int i = (int)((addr - HOSTFN_BASE) / 16);
        if (g_hostfn[i].calls++ == 0) g_hostfn[i].first = plat_inc(&g_hostfn_seq);      /* not atomic: 3 game threads made every NVN call a locked add on shared lines; the counts are for reports and traces */
        t_hostfn = i;
        const char *saved_name = c->in_import;
        uint64_t saved_from = c->import_from;
        c->in_import = g_hostfn[i].name;             /* so thread dumps can name the host function */
        c->import_from = c->x[30] - 4;
        g_hostfn[i].fn(c);
        c->in_import = saved_name;
        c->import_from = saved_from;
        return;
    }
    if (addr >= IMPORT_BASE && addr < IMPORT_BASE + (uint64_t)dyn_sym_count * IMPORT_SLOT) {
        host_import(c, (int)((addr - IMPORT_BASE) / IMPORT_SLOT));
        return;
    }
    if (addr >= 0x7500000000ULL && addr < 0x7500000000ULL + SDK_IMAGE_SIZE_GEN) { sdk_dispatch(c, addr); return; }   /* lifted SDK code (sdklift_rt.c), e.g. a vtable entry of an SDK object */
    void (*fn)(Ctx *) = lookup_guest(addr);
    if (!fn) guest_trap("indirect call to an address that is not a known function", addr);
    fn(c);
}

/* Register context of the guest code running on this host thread, for crash reports. */
static _Thread_local Ctx *t_cur_ctx;

/* debug aid SMO_WATCH=<heap offset>[,<bytes>]: every write to that range of the nn::os memory heap is logged
 * (guest function last entered, its x30, host module + offset, the value after the write). The page is made read-only;
 * a write faults, is logged, runs once with the page writable (single step) and the page is protected again. Writes by
 * another thread during that one step are not seen. Armed by smo_nn.c when the heap is made. At most 400 lines. */
static uint64_t g_watch_lo, g_watch_hi, g_watch_page, g_watch_off;
static volatile int32_t g_watch_lines;
static _Thread_local int t_watch_step, t_watch_hit;
/* "module+0xoffset" of a host code address (dladdr: the executable or a shared library, offset from its load base) */
static void host_where(uintptr_t pc, char *out, size_t size, uintptr_t *base_out) {
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_fname) {
        const char *bn = strrchr(di.dli_fname, '/');
        snprintf(out, size, "%s", bn ? bn + 1 : di.dli_fname);
        if (base_out) *base_out = (uintptr_t)di.dli_fbase;
    } else {
        snprintf(out, size, "?");
        if (base_out) *base_out = 0;
    }
}

static void watch_where(const char *what, uintptr_t rip) {
    Ctx *c = t_cur_ctx;
    char mod[PATH_MAX];
    uintptr_t base;
    if (plat_inc(&g_watch_lines) > 400) return;
    host_where(rip, mod, sizeof mod, &base);
    printf("watch: %s heap+0x%llx..: %016llx %016llx | guest f_%llx lr %llx | host %s+0x%llx | thread %lu\n", what,
           (unsigned long long)g_watch_off, (unsigned long long)rd64(g_watch_lo), (unsigned long long)rd64(g_watch_lo + 8),
           (unsigned long long)(c ? c->fn : 0), (unsigned long long)(c ? c->x[30] : 0), mod, (unsigned long long)(rip - base), (unsigned long)plat_tid());
    fflush(stdout);
}
/* SIGSEGV on the watched page: make it writable and single-step the write (the trap flag); SIGTRAP after the step:
 * log the new value and protect the page again. Returns 1 when the signal was the watch's. */
static int watch_signal(int sig, siginfo_t *si, ucontext_t *uc) {
    if (!g_watch_page) return 0;
    if (sig == SIGSEGV && ((uintptr_t)si->si_addr & ~0xfffull) == g_watch_page) {
        uint64_t a = (uint64_t)(uintptr_t)si->si_addr;
        t_watch_hit = a >= g_watch_lo && a < g_watch_hi;
        mprotect((void *)(uintptr_t)g_watch_page, 0x1000, PROT_READ | PROT_WRITE);
        t_watch_step = 1;
        uc->uc_mcontext.gregs[REG_EFL] |= 0x100;
        return 1;
    }
    if (sig == SIGTRAP && t_watch_step) {
        t_watch_step = 0;
        uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
        if (t_watch_hit) {                      /* only writes that change the watched bytes (a loop rewriting one value would fill the log) */
            static uint64_t last[2] = {~0ull, ~0ull};
            uint64_t now0 = rd64(g_watch_lo), now1 = rd64(g_watch_lo + 8);
            if (now0 != last[0] || now1 != last[1]) { last[0] = now0; last[1] = now1; watch_where("write", (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]); }
        }
        mprotect((void *)(uintptr_t)g_watch_page, 0x1000, PROT_READ);
        return 1;
    }
    return 0;
}
void smo_watch_arm(uint8_t *heap, uint64_t heap_size) {
    const char *e = getenv("SMO_WATCH");
    if (!e || g_watch_page) return;
    char *end;
    uint64_t off = strtoull(e, &end, 0), len = *end == ',' ? strtoull(end + 1, NULL, 0) : 8;
    if (off + len > heap_size || len == 0 || ((off + len - 1) & ~0xfffull) != (off & ~0xfffull)) { printf("watch: SMO_WATCH=%s is not one range inside one page of the heap\n", e); return; }
    g_watch_off = off; g_watch_lo = (uint64_t)(uintptr_t)heap + off; g_watch_hi = g_watch_lo + len; g_watch_page = g_watch_lo & ~0xfffull;
    mprotect((void *)(uintptr_t)g_watch_page, 0x1000, PROT_READ);
    printf("watch: armed on heap+0x%llx, %llu bytes\n", (unsigned long long)off, (unsigned long long)len);
}

void call_guest(Ctx *c, uint64_t addr) {
    Ctx *saved = t_cur_ctx;
    t_cur_ctx = c;
    call_indirect(c, addr);
    t_cur_ctx = saved;
}

static void dump_guest_registers(void) {
    Ctx *c = t_cur_ctx;
    if (!c) return;
    printf("last guest function entered: f_%llx\n", (unsigned long long)c->fn);
    printf("guest registers:\n");
    for (int i = 0; i < 31; i++)
        printf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 3 == 2 ? "\n" : "");
    printf("  sp  %016llx\n", (unsigned long long)c->x[31]);
}

/* ---------------------------------------------------------------- imports */
/* Every handler registered, whether the game imports its name or not: an SDK C wrapper may forward to a C++
 * name the game does not import itself (the game imports nnosGetThreadCoreMask, not nn::os::GetThreadCoreMask). The pairs
 * are copied (a caller may pass an ImportDef of its own stack); the names are the callers' lasting strings. */
static struct { const char *name; ImportFn fn; } *g_known;
static int g_known_count, g_known_cap;

void register_imports(const ImportDef *defs, int count) {
    for (int d = 0; d < count; d++) {
        if (!defs[d].name) continue;
        if (g_known_count == g_known_cap) {
            int cap = g_known_cap ? 2 * g_known_cap : 8192;
            void *p = realloc(g_known, (size_t)cap * sizeof *g_known);
            if (p) { g_known = p; g_known_cap = cap; }
        }
        if (g_known_count < g_known_cap) { g_known[g_known_count].name = defs[d].name; g_known[g_known_count].fn = defs[d].fn; g_known_count++; }
    }
    for (int d = 0; d < count; d++) {
        int found = 0;
        for (int i = 0; i < dyn_sym_count; i++) {
            if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, defs[d].name) == 0) { g_handlers[i] = defs[d].fn; found = 1; }
        }
        (void)found;
    }
}

/* Handler currently registered for an imported function, or NULL (not imported / still a stub). */
ImportFn import_lookup(const char *name) {
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0) return g_handlers[i];
    return NULL;
}

/* The handler last registered under a name, imported by the game or not; NULL if no table has it. */
ImportFn handler_by_name(const char *name) {
    for (int k = g_known_count - 1; k >= 0; k--)          /* the last registration wins, as in register_imports */
        if (strcmp(g_known[k].name, name) == 0) return g_known[k].fn;
    return NULL;
}

static uint64_t *g_import_addr;                    /* per-symbol address override, 0 = use the placeholder */
void import_set_address(const char *name, void *addr) {
    if (!g_import_addr) g_import_addr = calloc((size_t)dyn_sym_count, sizeof *g_import_addr);
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0) g_import_addr[i] = (uint64_t)(uintptr_t)addr;
}

/* Address of the placeholder that stands for a data symbol of another module (IMPORT_SLOT bytes,
 * zero-filled), so the host can give it its real value. NULL if the game does not import it. */
void *import_data(const char *name) {
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0)
            return (void *)(uintptr_t)(IMPORT_BASE + (uint64_t)i * IMPORT_SLOT);
    return NULL;
}

/* What the relocations of main use for imported symbol `sym`: its redirected address or its placeholder. */
uint64_t import_address(int sym) {
    return g_import_addr && g_import_addr[sym] ? g_import_addr[sym] : IMPORT_BASE + (uint64_t)sym * IMPORT_SLOT;
}

static int audit_strict_on(void);
/* exact call counts cost two interlocked increments on shared cache lines per import call (290 million calls in a
 * 240 s run, from every thread). They are kept only when SMO_IMPORT_COUNTS=1 or SMO_AUDIT_COUNTS is set;
 * otherwise an import is counted (and traced) for its first three calls, as before, and then called directly. */
static int g_import_count_all = 1;
/* suspension of the application by the applet manager (nnmisc.c oe_watch, with graphics.ini unfocused = pause; suyu am/applet.cpp
 * UpdateSuspensionStateLocked -> process->Suspend). While g_guest_frozen is set every guest thread stops at its next call of, or return from, an SDK function and
 * waits here (the console's kernel stops the threads wherever they are; a thread inside a kernel call finishes it there too). */
volatile int32_t g_guest_frozen;
void guest_freeze_wait(void) { while (g_guest_frozen) plat_sleep_ms(5); }
/* SMO_IMPORT_WATCH=<part of an import's name>: the calls of that one import are timed, and for every
 * frame in which they took more than 2 ms a line says how many there were, what they cost, the slowest one (with the text
 * its third argument points at when it is a printable string: the format of a formatting function) and how many different
 * return addresses called it. Off: one compare per import call. */
static void host_import_real(Ctx *c, int sym);
static int g_watch_sym = -2;
void host_import(Ctx *c, int sym) {
    if (g_watch_sym == -2) {
        const char *e = getenv("SMO_IMPORT_WATCH");
        int w = -1;
        if (e && e[0]) for (int i = 0; i < (int)dyn_sym_count; i++) if (strstr(dyn_syms[i].name, e)) { w = i; break; }
        if (w >= 0) printf("import_watch: watching \"%s\"\n", dyn_syms[w].name);
        g_watch_sym = w;
    }
    if (sym != g_watch_sym) { host_import_real(c, sym); return; }
    extern long long nvn_front_frame_next(void);
    static long long cur_frame, n, tks, max_tk, n_all; static char max_txt[72]; static uint64_t max_ra, ras[64]; static int nras; static uint64_t max_x1;
    static volatile int32_t lock;
    plat_counter q0, q1, fq;
    uint64_t ra = c->x[30], x1 = c->x[1], x2 = c->x[2];
    char txt[72]; txt[0] = 0;
    {   /* the third argument as text, when it is one (read BEFORE the call: the registers change) */
        if (x2 > 0x10000 && plat_readable((const void *)(uintptr_t)x2, 81)) {
            const unsigned char *s = (const unsigned char *)(uintptr_t)x2; int k = 0;
            for (; k < 70 && s[k] >= 32 && s[k] < 127; k++) txt[k] = (char)s[k];
            txt[k] = 0;
        }
    }
    plat_qpc(&q0);
    host_import_real(c, sym);
    plat_qpc(&q1); plat_qpf(&fq);
    long long d = q1.QuadPart - q0.QuadPart, fr = nvn_front_frame_next();
    while (plat_xchg(&lock, 1)) plat_pause();
    if (fr != cur_frame) {
        if (tks * 500 > fq.QuadPart) printf("import_watch: frame %lld: %lld calls, %.2f ms; slowest %.3f ms (argument 1 = %llu, argument 2 as text \"%s\", called from %llx); %d%s different return addresses, the first: %llx %llx %llx %llx; %lld calls since the start\n", cur_frame, n, (double)tks * 1000.0 / (double)fq.QuadPart,
                                             (double)max_tk * 1000.0 / (double)fq.QuadPart, (unsigned long long)max_x1, max_txt, (unsigned long long)max_ra, nras, nras == 64 ? "+" : "", (unsigned long long)ras[0], (unsigned long long)ras[1], (unsigned long long)ras[2], (unsigned long long)ras[3], n_all);
        cur_frame = fr; n = tks = max_tk = 0; nras = 0; memset(ras, 0, sizeof ras);
    }
    n++; n_all++; tks += d;
    if (d > max_tk) { max_tk = d; max_ra = ra; max_x1 = x1; memcpy(max_txt, txt, sizeof max_txt); }
    { int k = 0; for (; k < nras; k++) if (ras[k] == ra) break; if (k == nras && nras < 64) ras[nras++] = ra; }
    plat_xchg(&lock, 0);
}
static void host_import_real(Ctx *c, int sym) {
    static volatile int32_t order_count;
    if (g_guest_frozen) guest_freeze_wait();
    if (!g_import_count_all && g_import_calls[sym] >= 3 && g_handlers[sym]) {
        const char *saved_name = c->in_import;
        uint64_t saved_from = c->import_from;
        c->in_import = dyn_syms[sym].name;
        c->import_from = c->x[30] - 4;
        g_handlers[sym](c);
        c->in_import = saved_name;
        c->import_from = saved_from;
        if (g_guest_frozen) guest_freeze_wait();
        return;
    }
    unsigned long long n = (unsigned long long)plat_inc((volatile int64_t *)&g_import_calls[sym]);
    if (n == 1) { g_import_order[plat_inc(&order_count) - 1] = sym; g_import_order_count = order_count; }
    ImportFn fn = g_handlers[sym];
    plat_inc((volatile int64_t *)&g_trace_seq);
    if (n <= 3) {
        trace_printf("%8llu %s %s  x0=%llx x1=%llx x2=%llx x3=%llx  from=%llx\n", g_trace_seq,
                     fn ? "call " : "STUB ", dyn_syms[sym].name,
                     (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                     (unsigned long long)c->x[3], (unsigned long long)c->x[30] - 4);
    }
    const char *saved_name = c->in_import;
    uint64_t saved_from = c->import_from;
    c->in_import = dyn_syms[sym].name;
    c->import_from = c->x[30] - 4;
    if (fn) fn(c);
    else {
        /* No handler: the function was never implemented. The console would fault on the unresolved address; a silent 0
         * is a fake answer. */
        if (n == 1) {
            printf("import \"%s\" was called without a handler (caller 0x%llx)\n", dyn_syms[sym].name, (unsigned long long)c->x[30] - 4);
            trace_printf("import \"%s\" called without a handler\n", dyn_syms[sym].name);
        }
        if (audit_strict_on()) guest_trap("call to an import that has no handler (SMO_STRICT)", c->x[30] - 4);
        c->x[0] = 0;
    }
    c->in_import = saved_name;
    c->import_from = saved_from;
    if (g_guest_frozen) guest_freeze_wait();
}

int import_counts(int *stubs) {                       /* smo_nerve.c (SMO_STAGE_LIST): imports reached so far, and how many of them are stubs */
    int n = g_import_order_count, s = 0;
    for (int i = 0; i < n; i++) s += !g_handlers[g_import_order[i]];
    *stubs = s;
    return n;
}

void print_import_summary(FILE *out) {
    int stubs = 0, impl = 0;
    for (int i = 0; i < g_import_order_count; i++) { if (g_handlers[g_import_order[i]]) impl++; else stubs++; }
    fprintf(out, "\nimports reached: %d  (implemented: %d, still stubs: %d), %llu calls in total\n",
            g_import_order_count, impl, stubs, g_trace_seq);
    if (!g_import_count_all) fprintf(out, "(call counts are off: each import is counted up to its third call only; SMO_IMPORT_COUNTS=1 counts every call)\n");
    fprintf(out, "most called imports:\n");
    for (int k = 0; k < 14; k++) {                       /* small selection sort over the reached imports */
        int best = -1;
        for (int i = k; i < g_import_order_count; i++)
            if (best < 0 || g_import_calls[g_import_order[i]] > g_import_calls[g_import_order[best]]) best = i;
        if (best < 0) break;
        int t = g_import_order[k]; g_import_order[k] = g_import_order[best]; g_import_order[best] = t;
        fprintf(out, "  %10llu  %s%s\n", g_import_calls[g_import_order[k]], dyn_syms[g_import_order[k]].name,
                g_handlers[g_import_order[k]] ? "" : "   (stub)");
    }
    if (getenv("SMO_IMPORT_COUNTS") && atoi(getenv("SMO_IMPORT_COUNTS")) >= 2) {      /* D2 debug aid: every reached import with its count */
        fprintf(out, "all import counts:\n");
        for (int i = 0; i < g_import_order_count; i++) fprintf(out, "  count %10llu  %s\n", g_import_calls[g_import_order[i]], dyn_syms[g_import_order[i]].name);
    }
    fprintf(out, "unimplemented imports reached:\n");
    for (int i = 0; i < g_import_order_count; i++) {
        int s = g_import_order[i];
        if (!g_handlers[s]) fprintf(out, "  %8llu  %s\n", g_import_calls[s], dyn_syms[s].name);
    }
}

/* ---------------------------------------------------------------- traps */
void print_devinfo_summary(FILE *out);       /* nvn_stub.c */
void display_report(FILE *out);              /* display.c */

void hid_report(FILE *out);                  /* nnhid.c: synthetic XInput pad (SMO_XINPUT_SYNTH) */
void os_thread_report(FILE *out);            /* os_threads.c: every thread the game created, with its nn::os arguments */


void report_and_exit(int code) {
    audit_exit_counts();
    print_import_summary(stdout);
    print_hostfn_summary(stdout);
    print_devinfo_summary(stdout);
    { void nvn_virtual_pool_report(FILE *out); nvn_virtual_pool_report(stdout); }   /* nvn_stub.c */
    display_report(stdout);
    hid_report(stdout);
    os_thread_report(stdout);
    { void smo_heap_report(FILE *out); smo_heap_report(stdout); }   /* smo_nn.c */
    { void smo_audio_report(FILE *out); smo_audio_report(stdout); }   /* smo_audio.c */
    { void smo_fast_report(FILE *out); smo_fast_report(stdout); }     /* smo_fast.c */
    { void nerve_report(FILE *out); nerve_report(stdout); }   /* smo_nerve.c */
    { void fontll_report(FILE *out); fontll_report(stdout); }   /* nnfontll.c */
    if (g_trace) { print_import_summary(g_trace); print_hostfn_summary(g_trace); fclose(g_trace); }
    fflush(stdout);
    plat_exit((unsigned)code);
}

static void crash_report(int code, siginfo_t *si, ucontext_t *uc);
void guest_trap(const char *what, uint64_t addr) {
    const char *guest_thread_name_of(uint32_t tid);      /* os_threads.c */
    const char *tn = guest_thread_name_of(plat_tid());
    printf("\nGUEST TRAP: %s (0x%llx), on thread %s\n", what, (unsigned long long)addr, tn ? tn : "main (or a host thread)");
    trace_printf("GUEST TRAP: %s (0x%llx)\n", what, (unsigned long long)addr);
    crash_report(0xE0000001, NULL, NULL);
    report_and_exit(2);
}
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) {
    printf("\nUNTRANSLATED INSTRUCTION at 0x%llx: %08x (%s)\n", (unsigned long long)pc, insn, what);
    trace_printf("UNTRANSLATED INSTRUCTION at 0x%llx: %08x (%s)\n", (unsigned long long)pc, insn, what);
    crash_report(0xE0000002, NULL, NULL);
    report_and_exit(3);
}
void guest_svc(Ctx *c, uint32_t number) { (void)c; guest_trap("svc (direct kernel call)", number); }

/* The crash report: what faulted (a signal, or code 0xE0000001 guest trap / 0xE0000002 untranslated instruction with
 * no signal), the guest registers, the host frames (module + offset: tools/lifter/mapaddr.py names them) and the
 * guest functions among them, then report_and_exit(4). Runs on the thread that crashed, on its signal stack. */
static void crash_report(int code, siginfo_t *si, ucontext_t *uc) {
    uintptr_t pc = uc ? (uintptr_t)uc->uc_mcontext.gregs[REG_RIP] : 0;
    void *frames[64];
    int nf = backtrace(frames, 64);
    if (si) {
        printf("\nHOST EXCEPTION signal %d (%s) code %d at host address %p\n", code, strsignal(code), si->si_code, (void *)pc);
        char mod[PATH_MAX];
        uintptr_t base;
        host_where(pc, mod, sizeof mod, &base);
        printf("  in %s at offset 0x%llx\n", mod, (unsigned long long)(pc - base));
        if (code == SIGSEGV || code == SIGBUS)
            printf("  %s address 0x%llx\n", uc && (uc->uc_mcontext.gregs[REG_ERR] & 2) ? "writing" : "reading",
                   (unsigned long long)(uintptr_t)si->si_addr);
    } else {
        printf("\nHOST EXCEPTION 0x%08x (%s)\n", code, code == 0xE0000001 ? "guest trap" : "untranslated instruction");
    }
    dump_guest_registers();
    if (getenv("SMO_CRASH_DUMP") && t_cur_ctx) {   /* debug aid: what the guest registers point to (SMO_CRASH_DUMP=<bytes>, 1 = 0x60) */
        int dlen = (int)strtol(getenv("SMO_CRASH_DUMP"), NULL, 0);
        if (dlen < 0x60) dlen = 0x60;
        if (dlen > 0x1000) dlen = 0x1000;
#define READABLE(a, n) ((a) > 0x10000 && plat_readable((const void *)(uintptr_t)(a), (size_t)(n)))
        for (int r = 0; r < 31; r++) {
            uint64_t v = t_cur_ctx->x[r];
            if (!READABLE(v, dlen)) continue;
            printf("  [x%d = %llx]", r, (unsigned long long)v);
            for (int k = 0; k < dlen; k += 8) printf("%s%016llx", k % 0x20 ? " " : "\n     ", (unsigned long long)rd64(v + (uint64_t)k));
            uint64_t s = rd64(v + 0x20);
            if (READABLE(s, 61)) {
                const unsigned char *p = (const unsigned char *)(uintptr_t)s; int n = 0;
                while (n < 60 && p[n] >= 32 && p[n] < 127) n++;
                if (n >= 3 && p[n] == 0) printf("\n     [+0x20] -> \"%.*s\"", n, (const char *)p);
            }
            printf("\n");
        }
        {   /* debug aid: registers that point at a C string, and the guest frame-pointer chain (x29 -> {fp, lr}):
             * the game is built with frame pointers, so this is the guest call stack even when the host stack has no unwind info */
            for (int r = 0; r < 31; r++) {
                uint64_t v = t_cur_ctx->x[r];
                if (!READABLE(v, 64)) continue;
                const unsigned char *p = (const unsigned char *)(uintptr_t)v; int n = 0;
                while (n < 63 && p[n] >= 32 && p[n] < 127) n++;
                if (n >= 3 && p[n] == 0) printf("  x%d -> \"%.*s\"\n", r, n, (const char *)p);
            }
            uint64_t fp = t_cur_ctx->x[29];
            printf("  guest frame chain (x29): lr=%llx", (unsigned long long)t_cur_ctx->x[30]);
            for (int d = 0; d < 24 && READABLE(fp, 16); d++) {
                uint64_t lr = rd64(fp + 8), nfp = rd64(fp);
                if (!lr) break;
                printf("%s%llx", d % 8 == 7 ? "\n    " : " <- ", (unsigned long long)lr);
                if (nfp <= fp) break;
                fp = nfp;
            }
            printf("\n");
        }
#undef READABLE
    }
    /* the host frames (module + offset; tools/lifter/mapaddr.py names them from the binary's symbols). backtrace() walks
     * from this handler through the signal frame (glibc's unwinder knows it) into the code that faulted. */
    printf("host call stack (innermost first):\n");
    int start = 0;
    if (pc) for (int i = 0; i < nf; i++) if ((uintptr_t)frames[i] == pc) { start = i; break; }
    if (pc && (uintptr_t)frames[start] != pc) {
        char mod[PATH_MAX]; uintptr_t base;
        host_where(pc, mod, sizeof mod, &base);
        printf("  %s+0x%llx\n", mod, (unsigned long long)(pc - base));
    }
    for (int i = start; i < nf && i < start + 24; i++) {
        char mod[PATH_MAX]; uintptr_t base;
        host_where((uintptr_t)frames[i], mod, sizeof mod, &base);
        printf("  %s+0x%llx\n", mod, (unsigned long long)((uintptr_t)frames[i] - base));
    }
    printf("guest call stack (innermost first):\n");
    uint64_t last = 0;
    {   uint64_t g = pc ? guest_function_of(pc) : 0;
        if (g) { printf("  f_%llx\n", (unsigned long long)g); last = g; } }
    for (int i = start, shown = 0; i < nf && shown < 40; i++) {
        uint64_t g = guest_function_of((uintptr_t)frames[i]);
        if (g && g != last) { printf("  f_%llx\n", (unsigned long long)g); shown++; last = g; }
    }
    fflush(stdout);
    if (si) report_and_exit(4);
}

static volatile int32_t g_crashing;
static void crash_signal(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    if (watch_signal(sig, si, uc)) return;
    if (plat_xchg(&g_crashing, 1)) { for (;;) pause(); }   /* a second thread crashing waits for the first report */
    crash_report(sig, si, uc);
}

static void install_crash_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
    plat_thread_altstack();
}

/* ---------------------------------------------------------------- loader */
void runtime_init(const char *elf, const char *trace_path) {
    g_handlers = calloc((size_t)dyn_sym_count, sizeof *g_handlers);
    g_import_calls = calloc((size_t)dyn_sym_count, sizeof *g_import_calls);
    g_import_order = calloc((size_t)dyn_sym_count, sizeof *g_import_order);
    g_by_host = malloc((size_t)fn_table_count * sizeof *g_by_host);
    memcpy(g_by_host, fn_table, (size_t)fn_table_count * sizeof *g_by_host);
    qsort(g_by_host, (size_t)fn_table_count, sizeof *g_by_host, cmp_host);
    fn_hash_build();
    {   const char *e = getenv("SMO_IMPORT_COUNTS"), *a = getenv("SMO_AUDIT_COUNTS");
        g_import_count_all = (e && e[0] && e[0] != '0') || (a && a[0]); }
    {   /* sanity: host code addresses must be strictly increasing after the sort */
        int bad = 0;
        for (int i = 1; i < fn_table_count; i++) if ((uintptr_t)g_by_host[i].fn <= (uintptr_t)g_by_host[i - 1].fn) bad++;
        printf("host address table: %d entries, %d out of order, span %p..%p\n", fn_table_count, bad,
               (void *)g_by_host[0].fn, (void *)g_by_host[fn_table_count - 1].fn);
    }
    g_trace = fopen(trace_path, "w");
    install_crash_handlers();

    void *mem = plat_alloc((void *)GUEST_BASE, IMAGE_SIZE, PLAT_RW);
    if (mem != (void *)GUEST_BASE) { fprintf(stderr, "cannot map guest image at 0x%llx\n", GUEST_BASE); exit(1); }
    /* data symbols provided by other modules: zero-filled placeholders, IMPORT_SLOT bytes each */
    void *imp = plat_alloc((void *)IMPORT_BASE, (size_t)dyn_sym_count * IMPORT_SLOT, PLAT_RW);
    if (imp != (void *)IMPORT_BASE) { fprintf(stderr, "cannot map import area\n"); exit(1); }
    FILE *f = fopen(elf, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", elf); exit(1); }
    fseek(f, (long)FILE_OFF, SEEK_SET);
    if (fread(mem, 1, FILE_IMAGE, f) != FILE_IMAGE) { fprintf(stderr, "short read on image\n"); exit(1); }
    fclose(f);

    unsigned long long rel = 0, sym_int = 0, sym_ext = 0, sym_weak = 0, other = 0;
    const uint64_t tables[2][2] = {{RELA, RELASZ}, {JMPREL, PLTRELSZ}};
    for (int t = 0; t < 2; t++) {
        for (uint64_t i = 0; i < tables[t][1] / 24; i++) {
            uint64_t e = GUEST_BASE + tables[t][0] + i * 24;
            uint64_t off = rd64(e), info = rd64(e + 8);
            int64_t addend = (int64_t)rd64(e + 16);
            uint32_t type = (uint32_t)info, sym = (uint32_t)(info >> 32);
            if (type == 0x403) { wr64(GUEST_BASE + off, GUEST_BASE + (uint64_t)addend); rel++; }
            else if (type == 0x101 || type == 0x401 || type == 0x402) {
                uint64_t s;
                if (dyn_syms[sym].defined) { s = GUEST_BASE + dyn_syms[sym].value; sym_int++; }
                /* An undefined WEAK symbol is null unless a module defines it, so the game's `if (hook)` tests are false, as on
                 * the console (the line below counts the relocations that point at such symbols). */
                else if ((rd8(GUEST_BASE + GUEST_DYNSYM + (uint64_t)sym * 24 + 4) >> 4) == 2 && !(g_import_addr && g_import_addr[sym])) { s = 0; sym_weak++; }
                else { s = g_import_addr && g_import_addr[sym] ? g_import_addr[sym] : IMPORT_BASE + (uint64_t)sym * IMPORT_SLOT; sym_ext++; }
                wr64(GUEST_BASE + off, s + (type == 0x101 ? (uint64_t)addend : 0));
            } else other++;
        }
    }
    printf("image mapped at 0x%llx: %llu relative relocations, %llu to own symbols, %llu to imported symbols, %llu to undefined weak symbols (null), %llu unhandled\n",
           GUEST_BASE, rel, sym_int, sym_ext, sym_weak, other);
    printf("lifted functions: %d   dynamic symbols: %d\n", fn_table_count, dyn_sym_count);
    /* page protection as the loader sets it on the console: the text and read-only data segments (program headers of
     * main.elf: R-X 0x0..0x39c7e90, R-- 0x39c8000..0x4f15f60; the data segment starts at 0x4f16000) become read-only once the
     * relocations are applied (all of them land in the data segment: the image would not load on the console otherwise), so a
     * wild write into code or constants faults instead of corrupting silently. SMO_NO_PROTECT=1 leaves the old mapping. */
    if (!getenv("SMO_NO_PROTECT")) {
        if (!plat_protect((void *)GUEST_BASE, GUEST_RO_END, PLAT_R)) { fprintf(stderr, "cannot protect the image's text and read-only data\n"); exit(1); }
        printf("image protection: 0x%llx..0x%llx read-only (text, read-only data), data and bss read-write\n", GUEST_BASE, GUEST_BASE + GUEST_RO_END);
    }
}

/* ---------------------------------------------------------------- do-nothing handlers (SMO_STRICT)
 * A handler is "trivial" when its machine code does nothing but store constants into the guest context and
 * return: the generic ret0 / nothing / ret1 handlers, and every handler with a name of its own whose body is
 * the same thing. The test looks at the compiled code, so it does not depend on what a handler is called or
 * on which source file it lives in.
 *   SMO_STRICT=1        any import or run-time host function (vtable slot, NVN entry) bound to a trivial handler, and
 *                        any function import without a handler, stops the program with a list (exit code 5).
 *   SMO_STRICT=report   same list, without stopping.
 *   SMO_AUDIT_DUMP=<f>  write every import and its binding (handler RVA, trivial kind) to <f> after the
 *                        registrations and exit; "unimported" rows: handlers registered under names the game
 *                        does not import (reachable only through handler_by_name).
 *   SMO_AUDIT_COUNTS=<f> at exit, write the call count of every import and run-time host function. */

/* 0 = real code; 1 = empty body; 2 = only stores constants (x0 := *x0 when *sets_x0) */
int audit_trivial(ImportFn fn, long long *x0, int *sets_x0) {
    const unsigned char *p = (const unsigned char *)fn;
    int stores = 0;
    long long rax = 0;
    *x0 = 0; *sets_x0 = 0;
    if (!p) return 0;
    if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA) p += 4;                  /* endbr64 */
    for (int hop = 0; hop < 4 && p[0] == 0xE9; hop++) p = p + 5 + *(const int *)(p + 1);      /* linker thunk / tail jump */
    for (int step = 0; step < 12; step++) {
        const unsigned char *q = p;
        int w = 0;
        if (q[0] == 0xC3) return stores ? 2 : 1;
        if (q[0] == 0x48) { w = 1; q++; }
        if (q[0] == 0xC7 && (q[1] == 0x07 || q[1] == 0x47 || q[1] == 0x87)) {                   /* mov [rdi+d], imm32 (SysV: Ctx * in rdi) */
            long long disp = 0;
            int modrm = q[1];
            q += 2;
            if (modrm == 0x47) { disp = (signed char)q[0]; q += 1; } else if (modrm == 0x87) { disp = *(const int *)q; q += 4; }
            if (disp == 0) { *x0 = w ? (long long)*(const int *)q : (long long)*(const unsigned *)q; *sets_x0 = 1; }
            q += 4; stores++; p = q; continue;
        }
        if (w && q[0] == 0x83 && q[1] == 0x27 && q[2] == 0x00) { *x0 = 0; *sets_x0 = 1; stores++; p = q + 3; continue; }   /* and qword [rdi], 0 */
        if (w && q[0] == 0xB8) { rax = *(const long long *)(q + 1); p = q + 9; continue; }      /* movabs rax, imm64 */
        if (!w && q[0] == 0xB8) { rax = *(const unsigned *)(q + 1); p = q + 5; continue; }      /* mov eax, imm32 */
        if (!w && q[0] == 0x31 && q[1] == 0xC0) { rax = 0; p = q + 2; continue; }               /* xor eax, eax */
        if (w && q[0] == 0x89 && q[1] == 0x07) { *x0 = rax; *sets_x0 = 1; stores++; p = q + 2; continue; }   /* mov [rdi], rax */
        if (!w && q[0] == 0x0F && q[1] == 0x57 && q[2] == 0xC0) { p = q + 3; continue; }        /* xorps xmm0, xmm0 */
        if (!w && q[0] == 0x0F && (q[1] == 0x11 || q[1] == 0x29) && (q[2] == 0x47 || q[2] == 0x87)) {   /* movups/movaps [rdi+d], xmm0 */
            p = q + (q[2] == 0x47 ? 4 : 7); stores++; continue;
        }
        return 0;
    }
    return 0;
}

static int g_strict = -1;                  /* 0 off, 1 stop, 2 report */
static int g_audit_started, g_audit_hostfn_bad;   /* before audit_startup, host-function findings are only counted, so the whole list is printed */
static int audit_mode(void) {
    if (g_strict < 0) {
        const char *e = getenv("SMO_STRICT");
        g_strict = !e || !e[0] || !strcmp(e, "0") ? 0 : !strcmp(e, "report") ? 2 : 1;
    }
    return g_strict;
}

static int audit_strict_on(void) { return audit_mode() == 1; }
static void audit_describe(char *buf, size_t size, int kind, long long x0, int sets) {
    if (kind == 1) snprintf(buf, size, "empty body");
    else if (sets) snprintf(buf, size, "only returns the constant 0x%llx", (unsigned long long)x0);
    else snprintf(buf, size, "only stores constants");
}

/* A run-time host function (vtable slot, NVN entry point) was handed out: check it. */
static void audit_hostfn(const char *name, ImportFn fn) {
    long long x0; int sets, kind;
    char what[96];
    if (!audit_mode() || !fn) return;
    kind = audit_trivial(fn, &x0, &sets);
    if (!kind) return;
    audit_describe(what, sizeof what, kind, x0, sets);
    printf("STRICT: run-time host function \"%s\" is bound to a do-nothing handler (%s)\n", name, what);
    trace_printf("STRICT: run-time host function \"%s\": %s\n", name, what);
    g_audit_hostfn_bad++;
    if (g_strict == 1 && g_audit_started) { printf("STRICT: stopping (SMO_STRICT=report lists everything without stopping)\n"); fflush(stdout); plat_exit(5); }
}

static void audit_write(const char *path, int with_counts) {
    FILE *f = fopen(path, "w");
    uintptr_t base = 0;
    {   Dl_info di; if (dladdr((void *)audit_write, &di)) base = (uintptr_t)di.dli_fbase; }
    if (!f) { printf("audit: cannot write %s\n", path); return; }
    fprintf(f, "# list\tname\tcalls\thandler_rva\tkind\tx0\tdata_redirect\n");
    for (int i = 0; i < dyn_sym_count; i++) {
        long long x0 = 0; int sets = 0, kind;
        if (dyn_syms[i].defined || !dyn_syms[i].name[0]) continue;
        kind = g_handlers[i] ? audit_trivial(g_handlers[i], &x0, &sets) : -1;
        fprintf(f, "import\t%s\t%llu\t%llx\t%d\t%s%llx\t%d\n", dyn_syms[i].name, with_counts ? g_import_calls[i] : 0ull,
                g_handlers[i] ? (unsigned long long)((uintptr_t)g_handlers[i] - base) : 0ull, kind, sets ? "" : "-", (unsigned long long)x0,
                (int)(g_import_addr && g_import_addr[i]));
    }
    for (int i = 0; i < g_hostfn_count; i++) {
        long long x0 = 0; int sets = 0;
        int kind = g_hostfn[i].fn ? audit_trivial(g_hostfn[i].fn, &x0, &sets) : -1;
        fprintf(f, "hostfn\t%s\t%lld\t%llx\t%d\t%s%llx\t0\n", g_hostfn[i].name, with_counts ? (long long)g_hostfn[i].calls : 0ll,
                g_hostfn[i].fn ? (unsigned long long)((uintptr_t)g_hostfn[i].fn - base) : 0ull, kind, sets ? "" : "-", (unsigned long long)x0);
    }
    for (int k = 0; k < g_known_count; k++) {      /* a handler registered under a name the game does not import */
        int imported = 0;
        for (int i = 0; i < dyn_sym_count && !imported; i++) imported = !dyn_syms[i].defined && !strcmp(dyn_syms[i].name, g_known[k].name);
        if (!imported) fprintf(f, "unimported\t%s\t0\t%llx\t-1\t-\t0\n", g_known[k].name, (unsigned long long)((uintptr_t)g_known[k].fn - base));
    }
    fclose(f);
}

static void audit_exit_counts(void) {
    const char *p = getenv("SMO_AUDIT_COUNTS");
    if (p && p[0]) audit_write(p, 1);
}

/* Called by boot.c after the last registration. */
void audit_startup(void) {
    const char *dump = getenv("SMO_AUDIT_DUMP");
    int bad = 0, trivial = 0, unbound = 0;
    for (int i = 0; i < dyn_sym_count; i++) {
        long long x0; int sets, kind;
        char what[96];
        const char *name = dyn_syms[i].name;
        if (dyn_syms[i].defined || !name[0]) continue;
        if (!g_handlers[i]) {
            if (g_import_addr && g_import_addr[i]) continue;         /* data symbol with its real value (sdkdata.c, C++ runtime tables) */
            unbound++;
            bad++;
            if (audit_mode()) printf("STRICT: import \"%s\" has no handler and no data (a call would return 0)\n", name);
            continue;
        }
        kind = audit_trivial(g_handlers[i], &x0, &sets);
        if (!kind) continue;
        trivial++;
        bad++;
        if (audit_mode()) { audit_describe(what, sizeof what, kind, x0, sets); printf("STRICT: import \"%s\" is bound to a do-nothing handler (%s)\n", name, what); }
    }
    printf("strict check: %d imports bound to do-nothing handlers, %d without handler or data%s\n",
           trivial, unbound, audit_mode() ? "" : " (SMO_STRICT=1 lists them and stops)");
    if (dump && dump[0]) { audit_write(dump, 0); fflush(stdout); plat_exit(0); }
    g_audit_started = 1;
    bad += g_audit_hostfn_bad;
    if (bad && audit_mode() == 1) {
        printf("STRICT: %d binding(s) do nothing or have no handler: stopping (SMO_STRICT=report lists them without stopping)\n", bad);
        fflush(stdout);
        plat_exit(5);
    }
}

/* nn::os threading and synchronisation on top of pthreads (Linux).
 *
 * Guest synchronisation objects (MutexType, EventType, ...) are opaque here:
 * each one is identified by its guest address and backed by a host object
 * kept in a side table, so the guest layout never has to be known. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define TICK_HZ 19200000ULL

typedef struct Obj {
    uint64_t key;
    struct Obj *next;
    plat_lock_t cs;
    plat_cond_t cv;
    int signaled, autoclear;                 /* events */
    uint64_t *buf, cap, len, head;           /* message queues */
    plat_thread *thread;                     /* threads */
    volatile pid_t tid;                      /* its kernel thread id once it runs */
    uint64_t fn, arg, stack, stack_size, name;
    int priority, finished;
    int ideal_core, core_set;                /* threads: nn::os ideal core and affinity mask as the game set them */
    uint64_t core_mask;
    struct Obj *alias;                       /* system event attached by handle: the event object it shares */
    uint32_t rhandle, whandle;               /* kernel-style handles of a system event created here */
} Obj;

#define BUCKETS 65536
static Obj *g_table[BUCKETS];
static plat_rwlock_t g_table_lock = PLAT_RWLOCK_INIT;

static Obj *obj(uint64_t key) {
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (BUCKETS - 1);
    plat_rw_wlock(&g_table_lock);
    Obj *o = g_table[h];
    while (o && o->key != key) o = o->next;
    if (!o) {
        o = calloc(1, sizeof *o);
        o->key = key;
        plat_lock_init(&o->cs);
        plat_cond_init(&o->cv);
        o->next = g_table[h];
        g_table[h] = o;
    }
    plat_rw_wunlock(&g_table_lock);
    return o->alias ? o->alias : o;            /* a system event attached by handle shares its event */
}
/* the object itself, without following an alias (its own clear mode) */
static Obj *obj_raw(uint64_t key) {
    Obj *o = obj(key);
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (BUCKETS - 1);
    plat_rw_rlock(&g_table_lock);
    Obj *r = g_table[h];
    while (r && r->key != key) r = r->next;
    plat_rw_runlock(&g_table_lock);
    return r ? r : o;
}
/* Forget an object so a new one created at the same address starts clean. */
static void obj_reset(uint64_t key) {
    Obj *o = obj(key);
    plat_lock(&o->cs);
    o->signaled = o->autoclear = 0;
    o->len = o->head = 0;
    plat_unlock(&o->cs);
}

static uint32_t ns_to_ms(int64_t ns) { return ns <= 0 ? 0 : (uint32_t)((ns + 999999) / 1000000); }

/* nn::os::SleepThread takes nanoseconds and the console's timer is good to a few microseconds: the wait is a
 * clock_nanosleep on CLOCK_MONOTONIC (Linux high-resolution timers, ~50 us of slack). A zero or negative span yields,
 * as on the console. */
static void sleep_ns(int64_t ns) {
    if (ns <= 0) { plat_yield(); return; }
    /* SMO_SLEEP_LOG=1: a line for every sleep that lasted 2 ms longer than asked (first 200). */
    static int lg = -1; static int32_t nl;
    if (lg < 0) lg = getenv("SMO_SLEEP_LOG") && atoi(getenv("SMO_SLEEP_LOG"));
    plat_counter q0, q1, fq;
    if (lg) plat_qpc(&q0);
    plat_sleep_ns(ns);
    if (lg) {
        extern long long nvn_front_frame_next(void);
        plat_qpc(&q1); plat_qpf(&fq);
        double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart, asked = (double)ns / 1e6;
        {   /* per thread and frame: how many sleeps, asked and slept in all; a line when a frame after 1900 had over 8 ms of them */
            static _Thread_local long long fr0, cnt; static _Thread_local double a_sum, s_sum, a_max; static int32_t nf;
            long long fr = nvn_front_frame_next();
            if (fr != fr0) {
                if (fr0 > 1900 && s_sum > 8.0 && plat_inc(&nf) <= 300) printf("sleep_log: frame %lld, thread %lu: %lld sleeps in the frame, asked %.3f ms in all (longest asked %.3f), slept %.3f ms\n", fr0, (unsigned long)plat_tid(), cnt, a_sum, a_max, s_sum);
                fr0 = fr; cnt = 0; a_sum = s_sum = a_max = 0;
            }
            cnt++; a_sum += asked; s_sum += ms; if (asked > a_max) a_max = asked;
        }
        if (ms > asked + 2.0 && plat_inc(&nl) <= 600) { printf("sleep_log: frame %lld, thread %lu: asked %.3f ms, slept %.3f ms (%s)\n", nvn_front_frame_next(), (unsigned long)plat_tid(), asked, ms, "clock_nanosleep"); fflush(stdout); }
    }
}

/* nn::os thread priorities run from 0 (highest) to 31 (lowest), 16 is the default; the application's main thread
 * starts at the priority of its NPDM (44 - 28 = 16 in nn::os terms). Linux has nice values (weights, not strict
 * priorities) and SCHED_IDLE: the order is kept, the spacing is not (see host_priority). */
#define OS_DEFAULT_PRIORITY 16
#define OS_DEFAULT_CORE 0                    /* main.npdm: default core id 0 */
/* Console scheduling model (on by default; SMO_OS_CONSOLE_CORES=0 turns it off). The game creates its threads on one
 * core of the console's three (mask 1, 2 or 4) or on all three (os_thread_report lists them at the end of every run),
 * and the console runs on each core only the highest-priority ready thread of that core. With the model:
 *   - three host processors stand for the console's cores 0..2 and every guest thread's affinity is its nn::os core mask
 *     mapped onto them (set at StartThread and at every SetThreadCoreMask), so two threads the console keeps on one core
 *     never run at the same time here either;
 *   - six priority levels (<= 4, 5..8, 9..15, 16, 17, 18..31), Windows' dynamic priority boost off: the game
 *     uses 0, 7, 8, 15, 16, 17, 18, 19, 20 and
 *     22; what must stay apart is 17 against 18 on core 2: the model draw buffer update (描画バッファ更新, 17) that every
 *     scene starts at the end of a frame and waits for at the start of the next, and the archive decompression
 *     (DecompThread, 18) busy while a stage loads. At one level the update waited behind the decompression for a whole
 *     time slice, long enough for the scene being left to be deleted under it (HakoniwaStateDeleteScene runs a frame or
 *     two after the scene's last frame): "pure virtual function called" in the update's list of actors on entering a
 *     stage from the title menu, 2 times in 91 runs of the stage stress test (tests/stress_stages.py, two games on the
 *     same three processors); with 18..31 a level lower, 0 times in 276 runs of it. 17 is the last level above
 *     Windows' lowest;
 *   - GetCurrentCoreNumber / GetCurrentProcessorNumber answer the core the thread is really on.
 * NOT the console's scheduler all the same: Windows time-slices equal priorities, other processes of the PC take the
 * three processors too, and host-side threads (graphics driver, audio device, movie decoding) are not part of it.
 * ON BY DEFAULT (SMO_OS_CONSOLE_CORES=0 turns it off). Measured on 20 s boots, 6 runs each: with free
 * scheduling 4 of 6 runs failed in sead heap code on the loading path (sead::ExpHeap::tryAlloc failing inside
 * ParallelSZSDecompressor, or a sead::Heap destructor reading address -1); with the console core model 0 of 6 failed.
 * SMO depends on its threads being scheduled the console's way, so that is the default.
 * The three processors are taken from the top of a slot of six (three cores with their second hardware threads): every
 * game running at once on the PC takes the first slot no other game holds (a named mutex per slot, held while the
 * process lives), so games side by side (tests, or a test beside the copy being played) do not share processors. One
 * game alone takes slot 0, the top of the affinity mask: the processors it always had. Before the slots, four test
 * games at once all ran their console cores on the same three processors. SMO_OS_HOST_CPUS=<hexadecimal mask> chooses
 * from those processors instead (and takes no slot). */
static int g_cc = -1;
static uint64_t g_cc_core[3];
static int g_ncpu;
static uint64_t g_sibling[64];                     /* every logical CPU -> the mask of its core's hardware threads */

static uint64_t parse_cpulist(const char *s) {      /* "0-5,8,10-11" -> mask */
    uint64_t m = 0;
    while (s && *s) {
        char *e;
        long a = strtol(s, &e, 10), b = a;
        if (e == s) break;
        if (*e == '-') b = strtol(e + 1, &e, 10);
        for (long i = a; i <= b && i < 64; i++) if (i >= 0) m |= 1ull << i;
        s = *e == ',' ? e + 1 : NULL;
    }
    return m;
}
static uint64_t read_cpulist(const char *path) {
    char buf[256] = {0};
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    return parse_cpulist(buf);
}
/* this process's CPUs, their cores (thread_siblings_list) and on an Intel hybrid CPU the performance cores
 * (/sys/devices/cpu_core/cpus; the efficiency cores are /sys/devices/cpu_atom/cpus) */
static uint64_t process_cpus(uint64_t *pcores) {
    cpu_set_t set;
    uint64_t proc = 0;
    if (sched_getaffinity(0, sizeof set, &set) == 0)
        for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &set)) proc |= 1ull << i;
    g_ncpu = 0;
    for (int i = 0; i < 64; i++) {
        char path[128];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", i);
        g_sibling[i] = read_cpulist(path);
        if (!g_sibling[i]) g_sibling[i] = 1ull << i;
        if (proc >> i & 1) g_ncpu = i + 1;
    }
    uint64_t p = read_cpulist("/sys/devices/cpu_core/cpus");
    *pcores = (p & proc) ? p & proc : proc;
    return proc;
}
/* The physical cores of mask, one CPU (the first hardware thread) each, highest first. */
static int cores_of(uint64_t mask, int *first_cpu, int max) {
    int n = 0;
    uint64_t seen = 0;
    for (int b = 63; b >= 0 && n < max; b--) {
        if (!(mask >> b & 1) || (seen >> b & 1)) continue;
        uint64_t sib = g_sibling[b] & mask;
        seen |= g_sibling[b];
        first_cpu[n++] = sib ? __builtin_ctzll(sib) : b;
    }
    return n;
}
/* The first free slot of three physical cores from the top of the performance cores: an exclusive flock on
 * build/run/console_cores_<k>.lock, kept while the process lives (the kernel drops it when the process ends), so games
 * side by side (tests, or a test beside the copy being played) do not share processors. */
static uint64_t core_slot(uint64_t pcores) {
    int cpu[64], nb = cores_of(pcores, cpu, 64);
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/../run", plat_exe_dir());
    plat_mkdirs(dir);
    for (int k = 0; k < nb / 3; k++) {
        char name[4200];
        snprintf(name, sizeof name, "%s/console_cores_%d.lock", dir, k);
        int fd = open(name, O_RDWR | O_CREAT | O_CLOEXEC, 0644);   /* kept open: the slot is this process's until it ends */
        if (fd < 0) break;
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); continue; }
        uint64_t mask = 0;
        for (int i = 3 * k; i < 3 * k + 3; i++) mask |= 1ull << cpu[i];
        printf("nn::os: console cores in processor slot %d of %d (CPUs %llx; other games running at once take other slots)\n", k, nb / 3, (unsigned long long)mask);
        return mask;
    }
    printf("nn::os: every processor slot is held by other games: sharing the top of the performance cores\n");
    return pcores;
}
static int console_cores(void) {
    if (g_cc < 0) {
        int n = 0, on = 0;
        const char *cc = getenv("SMO_OS_CONSOLE_CORES"), *cpus = getenv("SMO_OS_HOST_CPUS");
        if (!cc || atoi(cc) != 0) {
            uint64_t pcores, proc = process_cpus(&pcores), pick;
            if (cpus && cpus[0]) {
                uint64_t want = strtoull(cpus, NULL, 16);
                printf("nn::os: SMO_OS_HOST_CPUS=%s: the console's cores on these processors only (%llx of %llx)\n", cpus, (unsigned long long)(want & proc), (unsigned long long)proc);
                pick = want & proc ? want & proc : pcores;
            } else pick = core_slot(pcores);
            int cpu[64], nc = cores_of(pick, cpu, 64);      /* one hardware thread of three different physical cores */
            for (int i = 0; i < nc && n < 3; i++) g_cc_core[n++] = 1ull << cpu[i];
            on = n == 3;
            printf("nn::os: console core model %s (SMO_OS_CONSOLE_CORES): host CPU masks %llx %llx %llx for cores 0 1 2\n", on ? "ON" : "NOT possible (fewer than 3 usable cores)",
                   (unsigned long long)g_cc_core[0], (unsigned long long)g_cc_core[1], (unsigned long long)g_cc_core[2]);
        }
        g_cc = on;
    }
    return g_cc;
}
/* The processors for the runtime's own busy threads (the render thread, the pipeline workers and the helper threads of the
 * renderer): the process's minus the console cores' processors and the second hardware threads of their cores, so that
 * no game thread pinned there (the main thread, the presentation thread at priority 7 ...) preempts them and they do not
 * share a core with one. Without this the render thread was found "not on a CPU" in frames that missed the 60 Hz refresh
 * (SMO_HITCH). 0 when that would leave nothing, or with SMO_HOST_PIN=0; SMO_HOST_CPUS=<hex mask> chooses them. */
uint64_t os_host_cpu_mask(void) {
    static uint64_t mask = 1;
    if (mask == 1) {
        const char *pin = getenv("SMO_HOST_PIN"), *cpus = getenv("SMO_HOST_CPUS");
        uint64_t pcores, proc = process_cpus(&pcores);
        mask = 0;
        if (pin && atoi(pin) == 0) {}
        else if (cpus && cpus[0]) mask = strtoull(cpus, NULL, 16) & proc;
        else if (console_cores()) {
            uint64_t game = 0;
            for (int i = 0; i < 3; i++) game |= g_sibling[__builtin_ctzll(g_cc_core[i])] | g_cc_core[i];
            mask = proc & ~game;
        }
        printf("nn::os: the runtime's own threads (render, pipeline workers, helpers) on processors %llx%s\n", (unsigned long long)mask,
               mask ? " (away from the console cores and their second hardware threads; SMO_HOST_PIN=0: anywhere)" : ": anywhere");
    }
    return mask;
}
/* SMO_HOST_LAYOUT=1: the render thread gets the lowest core of that mask to itself (one processor of it, the core's second
 * hardware thread left idle) and the runtime's other threads the rest, so that no helper shares a core with it. */
static int host_layout(void) { static int on = -1; if (on < 0) { const char *e = getenv("SMO_HOST_LAYOUT"); on = e && atoi(e) > 0; } return on; }
static uint64_t render_core_mask(void) {          /* every hardware thread of the lowest core in the host mask, or 0 */
    uint64_t m = os_host_cpu_mask();
    if (!m) return 0;
    return g_sibling[__builtin_ctzll(m)] & m;
}
void os_pin_host_thread(void) {
    uint64_t m = os_host_cpu_mask();
    if (m && host_layout() && (m & ~render_core_mask())) m &= ~render_core_mask();
    if (m) plat_set_affinity(0, &m, 64);
}
void os_pin_render_thread(void) {
    uint64_t rc = render_core_mask();
    if (!host_layout() || !rc) { os_pin_host_thread(); return; }
    uint64_t one = rc & (~rc + 1);                 /* the core's first processor only */
    plat_set_affinity(0, &one, 64);
    printf("nn::os: SMO_HOST_LAYOUT=1: the render thread alone on processor mask %llx, the runtime's other threads on %llx\n", (unsigned long long)one, (unsigned long long)(os_host_cpu_mask() & ~rc));
}
static void apply_sched(pid_t tid, uint64_t core_mask) {     /* tid 0: the calling thread */
    uint64_t m = 0;
    if (!console_cores() || tid < 0) return;
    for (int i = 0; i < 3; i++) if (core_mask >> i & 1) m |= g_cc_core[i];
    if (!m) m = g_cc_core[0];
    plat_set_affinity(tid, &m, 64);
}
/* Six levels (<= 4, 5..8, 9..15, 16, 17, 18..31) as nice values from the lowest this process may use (RLIMIT_NICE;
 * 0 for an ordinary user) up, and 18..31 under SCHED_IDLE with the console model, below the draw buffer update (17),
 * see above. Nice values are weights for the fair scheduler, not strict priorities. A thread can only raise its nice
 * value without RLIMIT_NICE: guest threads are created from a helper thread at the base value (spawn below) and set
 * their own level when they start; a later raise of priority beyond what the limit allows is refused (counted). */
#define PRIO_IDLE 100
static int g_nice_base = 99, g_nice_step = 1;
static volatile int32_t g_prio_refused;
static void nice_range(void) {
    if (g_nice_base != 99) return;
    struct rlimit rl;
    int lowest = 0;
    if (getrlimit(RLIMIT_NICE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) lowest = 20 - (int)rl.rlim_cur;
    else if (getrlimit(RLIMIT_NICE, &rl) == 0) lowest = -20;
    if (lowest > 0) lowest = 0;
    g_nice_base = lowest < -10 ? -10 : lowest;
    g_nice_step = g_nice_base <= -8 ? 3 : 1;
    printf("nn::os: thread priorities as nice values from %d in steps of %d (RLIMIT_NICE)%s\n", g_nice_base, g_nice_step,
           console_cores() ? ", 18..31 under SCHED_IDLE" : "");
}
static int host_priority(int prio) {
    static int split = -1;                                     /* TEST ONLY, SMO_OS_PRIO17=0: 17 shares its level with 18..27 again */
    if (split < 0) { const char *e = getenv("SMO_OS_PRIO17"); split = !e || atoi(e) != 0; }
    nice_range();
    int level;
    if (prio <= 4) level = 0;
    else if (prio <= 8) level = 1;
    else if (prio <= 15) level = 2;
    else if (prio == 16) level = 3;
    else if (prio == 17 || (!split && prio <= 27)) level = 4;
    else level = 5;
    if (level == 5 && console_cores()) return PRIO_IDLE;
    return g_nice_base + level * g_nice_step;
}
static void set_host_priority(pid_t tid, int prio) {        /* tid 0: the calling thread */
    int nice = host_priority(prio);
    struct sched_param sp = {0};
    if (nice == PRIO_IDLE) { sched_setscheduler(tid, SCHED_IDLE, &sp); return; }
    if (sched_getscheduler(tid) == SCHED_IDLE && sched_setscheduler(tid, SCHED_OTHER, &sp) != 0) { plat_inc(&g_prio_refused); return; }
    if (setpriority(PRIO_PROCESS, (id_t)(tid ? tid : plat_tid()), nice) != 0) plat_inc(&g_prio_refused);
}

/* Guest threads are created by this helper thread, which stays at nice 0 under SCHED_OTHER: a new thread inherits
 * its creator's nice value and could not go back up from a low-priority creator's. */
typedef struct SpawnReq { plat_thread_fn fn; void *arg; size_t stack; plat_thread *result; int done; struct SpawnReq *next; } SpawnReq;
static plat_lock_t g_spawn_lock = PLAT_LOCK_INIT;
static plat_cond_t g_spawn_cv = PLAT_COND_INIT, g_spawn_done_cv = PLAT_COND_INIT;
static SpawnReq *g_spawn_q;
static uint32_t spawner_main(void *arg) {
    (void)arg;
    plat_set_thread_name("smo-spawner");
    plat_lock(&g_spawn_lock);
    for (;;) {
        while (!g_spawn_q) plat_cond_wait_ms(&g_spawn_cv, &g_spawn_lock, PLAT_INFINITE);
        SpawnReq *r = g_spawn_q;
        g_spawn_q = r->next;
        plat_unlock(&g_spawn_lock);
        plat_thread *t = plat_thread_start(r->fn, r->arg, r->stack);
        plat_lock(&g_spawn_lock);
        r->result = t;
        r->done = 1;
        plat_cond_broadcast(&g_spawn_done_cv);
    }
    return 0;
}
static plat_thread *spawn(plat_thread_fn fn, void *arg, size_t stack) {
    static int32_t started;
    if (!plat_xchg(&started, 1)) plat_thread_detach(plat_thread_start(spawner_main, NULL, 1 << 20));
    SpawnReq r = {fn, arg, stack, NULL, 0, NULL};
    plat_lock(&g_spawn_lock);
    SpawnReq **tail = &g_spawn_q;
    while (*tail) tail = &(*tail)->next;
    *tail = &r;
    plat_cond_signal(&g_spawn_cv);
    while (!r.done) plat_cond_wait_ms(&g_spawn_done_cv, &g_spawn_lock, PLAT_INFINITE);
    plat_unlock(&g_spawn_lock);
    return r.result;
}

/* ---------------------------------------------------------------- threads */
static _Thread_local uint64_t t_thread;          /* guest ThreadType* of the running thread */
static _Thread_local uint64_t t_tls[64];
static uint64_t g_tls_dtor[64];                       /* guest destructor of each slot, 0 = none */
static volatile int32_t g_tls_used[64];
static void tls_run_destructors(Ctx *c);
static uint8_t g_main_thread[0x400];
uint64_t os_main_thread_type(void) { return (uint64_t)(uintptr_t)g_main_thread; }   /* boot.c: stored at thread block +0x1f8, as [sdk 0x3349dc] does */
uint64_t g_main_stack_base, g_main_stack_size;        /* set by boot.c */
static volatile int32_t g_thread_count;

static Ctx *new_ctx(uint64_t stack_top) {
    Ctx *c = plat_aligned_alloc(sizeof *c, 16);
    memset(c, 0, sizeof *c);
    c->x[31] = stack_top & ~15ull;
    c->tpidr = (uint64_t)(uintptr_t)plat_alloc(NULL, 0x10000, PLAT_RW);
    return c;
}

/* Every guest thread's context, so a report can say where each one is. */
static struct { Obj *o; Ctx *c; } g_threads[256];
static volatile int32_t g_threads_n;

void dump_guest_threads(FILE *out) {
    fprintf(out, "\nguest threads (last function entered, link register):\n");
    for (int i = 0; i < g_threads_n && i < 256; i++) {
        Obj *o = g_threads[i].o;
        Ctx *c = g_threads[i].c;
        fprintf(out, "  %-20s %s  f_%llx  lr=%llx  %s%s (from %llx)\n", o->name ? (const char *)(uintptr_t)o->name : "?",
                o->finished ? "finished" : "running ", (unsigned long long)c->fn, (unsigned long long)c->x[30],
                c->in_import ? "in " : "", c->in_import ? c->in_import : "guest code", (unsigned long long)c->import_from);
    }
}

/* hitch.c (SMO_HITCH): guest thread i -> context, name, host handle; 0 past the end. */
int guest_thread_info(int i, Ctx **c, const char **name, uint32_t *tid) {
    if (i < 0 || i >= g_threads_n || i >= 256 || !g_threads[i].o || !g_threads[i].c) return 0;
    *c = g_threads[i].c; *name = g_threads[i].o->name ? (const char *)(uintptr_t)g_threads[i].o->name : NULL; *tid = g_threads[i].o->finished ? 0 : (uint32_t)g_threads[i].o->tid;
    return 1;
}
/* Name of the guest thread running on host thread `tid`, or NULL (nvn_gl.c: CPU time per thread). */
const char *guest_thread_name_of(uint32_t tid) {
    for (int i = 0; i < g_threads_n && i < 256; i++) {
        Obj *o = g_threads[i].o;
        if (o && o->tid && (uint32_t)o->tid == tid) return o->name ? (const char *)(uintptr_t)o->name : "?";
    }
    return NULL;
}

static uint32_t thread_main(void * arg) {
    Obj *o = arg;
    t_thread = o->key;
    o->tid = plat_tid();
    {   char n[16]; snprintf(n, sizeof n, "%s", o->name ? (const char *)(uintptr_t)o->name : "guest"); plat_set_thread_name(n); }
    set_host_priority(0, o->priority);                /* before any guest code, as a thread created suspended */
    apply_sched(0, o->core_mask);
    Ctx *c = new_ctx(o->stack + o->stack_size);
    wr64(c->tpidr + 0x1f8, o->key);                   /* Horizon thread-local region: the ThreadType pointer lives at +0x1f8 */
    int32_t slot = plat_inc(&g_threads_n) - 1;
    if (slot < 256) { g_threads[slot].o = o; g_threads[slot].c = c; }
    c->x[0] = o->arg;
    trace_printf("--- thread %llx started (entry %llx, name %s)\n", (unsigned long long)o->key, (unsigned long long)o->fn,
                 o->name ? (const char *)(uintptr_t)o->name : "?");
    call_guest(c, o->fn);
    tls_run_destructors(c);
    trace_printf("--- thread %llx finished\n", (unsigned long long)o->key);
    plat_lock(&o->cs);
    o->finished = 1;
    plat_cond_broadcast(&o->cv);
    plat_unlock(&o->cs);
    return 0;
}

/* the fields of nn::os::ThreadType (SDK 3.5.1) that the game reads itself. [sdk 0x8a680]
 * detail::ThreadManager::CreateThread(this, ThreadType *t, fn, arg, stack, size, priority, core) stores priority at +0x44,
 * the stack at +0x48, the stack the thread RUNS on at +0x50 (a mapped alias of +0x48 made by CreateAliasStackUnsafe; here
 * the thread runs on the stack it was given, so the same address), the size at +0x58, arg at +0x60 and fn at +0x68.
 * sead::MemUtil::isStack (main 0xb5dad0) compares a pointer with [+0x50, +0x50 + size) of every sead thread:
 * al::createStringIfInStack (main 0x815f84) copies a string to the heap only when that says "on a stack" (the opening
 * demo's DemoActionList::initSerial keeps its action names this way). */
static void thread_type_fill(uint64_t t, uint64_t fn, uint64_t arg, uint64_t stack, uint64_t size, int priority) {
    if (!t) return;
    wr32(t + 0x44, (uint32_t)priority);
    wr64(t + 0x48, stack); wr64(t + 0x50, stack); wr64(t + 0x58, size);
    wr64(t + 0x60, arg); wr64(t + 0x68, fn);
}
static void os_CreateThread(Ctx *c) {                 /* (ThreadType*, fn, arg, stack, stackSize, priority[, core]) */
    thread_type_fill(A0, A1, A2, A3, A4, (int)c->x[5]);
    Obj *o = obj(A0);
    o->fn = A1; o->arg = A2; o->stack = A3; o->stack_size = A4; o->priority = (int)c->x[5];
    o->finished = 0; o->thread = NULL; o->tid = 0; o->name = 0;
    o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; o->core_set = 1;
    RET(0);
}
static void os_CreateThreadOnCore(Ctx *c) {           /* (ThreadType*, fn, arg, stack, stackSize, priority, idealCore) */
    uint64_t t = A0;
    int core = (int)c->x[6];
    os_CreateThread(c);
    if (core >= 0 && core < 64) { Obj *o = obj(t); o->ideal_core = core; o->core_mask = 1ull << core; }
}
/* The thread log: every thread the game
 * starts, with the nn::os arguments it was given, and every later change of priority or core mask. Printed at the end
 * of a run by os_thread_report (hostrt.c report_and_exit). */
static char g_oslog[24000];
static volatile int32_t g_oslog_n, g_oslog_lock;
static void oslog(const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    while (plat_cas(&g_oslog_lock, 1, 0)) plat_pause();
    if (g_oslog_n + n < (int32_t)sizeof g_oslog - 1) { memcpy(g_oslog + g_oslog_n, line, (size_t)n); g_oslog_n += n; }
    g_oslog_lock = 0;
}
void os_thread_report(FILE *out) {
    if (!g_oslog_n) return;
    fprintf(out, "nn::os threads of this run (priority 0 highest .. 31 lowest; cores 0..2 exist on the console):\n%.*s", (int)g_oslog_n, g_oslog);
}
static void os_StartThread(Ctx *c) {
    Obj *o = obj(A0);
    oslog("  start  %-28s entry %llx priority %2d ideal core %d mask %llx stack 0x%llx (called from %llx)\n", o->name ? (const char *)(uintptr_t)o->name : "?",
          (unsigned long long)o->fn, o->priority, o->ideal_core, (unsigned long long)o->core_mask, (unsigned long long)o->stack_size, (unsigned long long)c->x[30]);
    plat_inc(&g_thread_count);
    o->thread = spawn(thread_main, o, (size_t)256 << 20);
}
/* Windows ran the process one priority class up; Linux has no class: the levels are relative to nice 0 (or below it
 * with RLIMIT_NICE, e.g. `ulimit -e 30` or a limits.conf entry), see host_priority. */
static void process_priority(void) {
    nice_range();
}
/* boot.c, on the main guest thread before the game starts: main.npdm gives it core 0 and priority 44 - 28 = 16 */
void os_apply_main_sched(void) {
    process_priority();
    if (!console_cores()) return;
    apply_sched(0, 1ull << OS_DEFAULT_CORE);
    set_host_priority(0, OS_DEFAULT_PRIORITY);
}
static void os_WaitThread(Ctx *c) {
    Obj *o = obj(A0);
    if (!o->thread) return;
    plat_lock(&o->cs);
    while (!o->finished) plat_cond_wait_ms(&o->cv, &o->cs, PLAT_INFINITE);
    plat_unlock(&o->cs);
}
static void os_DestroyThread(Ctx *c) {
    Obj *o = obj(A0);
    if (o->thread) { plat_thread_detach(o->thread); o->thread = NULL; }
}
static void os_SetThreadName(Ctx *c) {               /* the guest string may be a temporary: keep a copy */
    char *copy = strdup((const char *)(uintptr_t)A1);
    obj(A0)->name = (uint64_t)(uintptr_t)copy;
}
static void os_GetCurrentThread(Ctx *c) {
    if (!t_thread && !rd64((uint64_t)(uintptr_t)g_main_thread + 0x50) && g_main_stack_base)      /* the main thread's ThreadType, once its stack is known (boot.c) */
        thread_type_fill((uint64_t)(uintptr_t)g_main_thread, 0, 0, g_main_stack_base, g_main_stack_size, OS_DEFAULT_PRIORITY);
    RET(t_thread ? t_thread : (uint64_t)g_main_thread);
}
static Obj *thread_obj(uint64_t key);
static void os_GetThreadPriority(Ctx *c) { RET((int64_t)thread_obj(A0 == (uint64_t)(uintptr_t)g_main_thread ? 0 : A0)->priority); }
static void os_ChangeThreadPriority(Ctx *c) {
    Obj *o = obj(A0);
    int old = o->priority;
    oslog("  change priority of %s: %d -> %d (called from %llx)\n", A0 == (uint64_t)(uintptr_t)g_main_thread ? "the main thread" : o->name ? (const char *)(uintptr_t)o->name : "?", old, (int)A1, (unsigned long long)c->x[30]);
    o->priority = (int)A1;
    if (o->thread && o->tid) set_host_priority(o->tid, o->priority);
    else if (A0 == (uint64_t)(uintptr_t)g_main_thread || A0 == t_thread) set_host_priority(0, o->priority);
    RET((int64_t)old);
}
static void os_GetThreadId(Ctx *c) { Obj *o = obj(A0); RET(o->thread && o->tid ? (uint64_t)o->tid : (uint64_t)plat_tid()); }
static void os_YieldThread(Ctx *c) { (void)c; plat_yield(); }
static void os_SleepThread(Ctx *c) { sleep_ns((int64_t)A0); }
static void os_GetCurrentStackInfo(Ctx *c) {
    Obj *o = t_thread ? obj(t_thread) : NULL;
    if (A0) wr64(A0, o ? o->stack : g_main_stack_base);
    if (A1) wr64(A1, o ? o->stack_size : g_main_stack_size);
}
/* Core numbers and affinity masks. The console runs the application on cores 0..2 (GetThreadAvailableCoreMask = 7);
 * a thread is created on one core (the NPDM default core 0, or the one given to CreateThread) and stays there until
 * SetThreadCoreMask widens its mask. The values are kept per thread and returned as set; with the console core model
 * (apply_sched) they also pin the thread to the host CPUs standing for those cores. */
static Obj *thread_obj(uint64_t key) {
    Obj *o = obj(key ? key : (uint64_t)(uintptr_t)g_main_thread);
    if (!o->core_set) { o->core_set = 1; o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; if (!o->priority) o->priority = OS_DEFAULT_PRIORITY; }
    return o;
}
static void os_SetThreadCoreMask(Ctx *c) {            /* (ThreadType*, int idealCore, Bit64 affinityMask) */
    Obj *o = thread_obj(A0);
    int ideal = (int)A1;
    uint64_t mask = A2;
    oslog("  core mask of %s: ideal %d mask %llx (was ideal %d mask %llx; %s; called from %llx)\n", A0 == (uint64_t)(uintptr_t)g_main_thread || !A0 ? "the main thread" : o->name ? (const char *)(uintptr_t)o->name : "?",
          ideal, (unsigned long long)mask, o->ideal_core, (unsigned long long)o->core_mask, o->thread ? "running" : "not started", (unsigned long long)c->x[30]);
    int is_self = t_thread ? A0 == t_thread : (!A0 || A0 == (uint64_t)(uintptr_t)g_main_thread);      /* the calling thread names itself */
    pid_t h = o->thread ? (o->tid ? o->tid : -1) : is_self ? 0 : -1;                                     /* -1: not started yet (applied when it starts) */
    if (ideal == -2) { o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; apply_sched(h, o->core_mask); return; }   /* IdealCoreUseDefaultValue: mask is ignored */
    if (ideal == -3) { o->core_mask = mask; apply_sched(h, o->core_mask); return; }                           /* IdealCoreNoUpdate */
    o->core_mask = mask;
    if (ideal == -1) { if (!(mask >> (o->ideal_core & 63) & 1)) o->ideal_core = mask ? (int)__builtin_ctzll(mask) : OS_DEFAULT_CORE; }   /* IdealCoreDontCare */
    else o->ideal_core = ideal;
    apply_sched(h, o->core_mask);
}
static void os_GetThreadCoreMask(Ctx *c) {            /* (int *outIdealCore, Bit64 *outMask, const ThreadType*) */
    Obj *o = thread_obj(A2);
    if (A0) wr32(A0, (uint32_t)o->ideal_core);
    if (A1) wr64(A1, o->core_mask);
}
static void os_GetCurrentCoreNumber(Ctx *c) {
    if (console_cores()) {                                /* the core the thread is on now, as the console answers */
        uint64_t here = 1ull << plat_cpu();
        for (int i = 0; i < 3; i++) if (g_cc_core[i] == here) { RET(i); return; }
    }
    RET((int64_t)thread_obj(t_thread)->ideal_core);
}
static void os_AvailableCoreMask(Ctx *c) { RET(7); }
static void nothing(Ctx *c) { (void)c; }

/* thread-local storage slots */
static volatile int32_t g_tls_next;
/* (TlsSlot *out, void (*destructor)(uintptr_t value)) -> Result. The destructor of a slot runs when a thread ends
 * with a non-zero value in it; FreeTlsSlot makes the slot available again. */
static void os_AllocateTlsSlot(Ctx *c) {
    (void)g_tls_next;
    for (int i = 0; i < 64; i++)
        if (plat_cas(&g_tls_used[i], 1, 0) == 0) { g_tls_dtor[i] = A1; wr32(A0, (uint32_t)i); RET(0); return; }
    guest_trap("nn::os::AllocateTlsSlot: all 64 slots are in use (the console has 32)", c->x[30] - 4);
}
static void os_FreeTlsSlot(Ctx *c) { uint32_t i = (uint32_t)A0 & 63; g_tls_dtor[i] = 0; plat_xchg(&g_tls_used[i], 0); }
static void tls_run_destructors(Ctx *c) {
    for (int i = 0; i < 64; i++) {
        uint64_t v = t_tls[i], fn = g_tls_dtor[i];
        t_tls[i] = 0;
        if (v && fn && g_tls_used[i]) { c->x[0] = v; call_guest(c, fn); }
    }
}
static void os_GetTlsValue(Ctx *c) { RET(t_tls[A0 & 63]); }
static void os_SetTlsValue(Ctx *c) { t_tls[A0 & 63] = A1; }

/* ---------------------------------------------------------------- mutexes */
static void mtx_init(Ctx *c) { (void)obj(A0); }
static void mtx_lock(Ctx *c) { plat_lock(&obj(A0)->cs); }
static void mtx_unlock(Ctx *c) { plat_unlock(&obj(A0)->cs); }
static void mtx_trylock(Ctx *c) { RET(plat_trylock(&obj(A0)->cs) ? 1 : 0); }
/* hitch.c (SMO_HITCH): host thread id that owns the guest mutex `key` now, 0 = nobody. Diagnostics only. */
uint32_t os_mutex_owner(uint64_t key) { return key ? (uint32_t)obj(key)->cs.__data.__owner : 0; }

/* ---------------------------------------------------------------- events (Event, LightEvent, SystemEvent) */
/* nn::os::EventClearMode: 0 = ManualClear, 1 = AutoClear */
static void ev_init(Ctx *c) { obj_reset(A0); Obj *o = obj(A0); o->signaled = (int)(A1 & 1); o->autoclear = A2 == 1; }
static void ev_signal(Ctx *c) {
    Obj *o = obj(A0);
    plat_lock(&o->cs);
    o->signaled = 1;
    plat_cond_broadcast(&o->cv);
    plat_unlock(&o->cs);
}
/* Host-side access to guest events (used for the display's vsync event). */
void os_init_event(uint64_t key, int autoclear) { obj_reset(key); Obj *o = obj(key); o->signaled = 0; o->autoclear = autoclear; }
void os_signal_event(uint64_t key) {
    Obj *o = obj(key);
    plat_lock(&o->cs);
    o->signaled = 1;
    plat_cond_broadcast(&o->cv);
    plat_unlock(&o->cs);
}
void os_clear_event(uint64_t key) { Obj *o = obj(key); plat_lock(&o->cs); o->signaled = 0; plat_unlock(&o->cs); }
static void ev_clear(Ctx *c) { Obj *o = obj(A0); plat_lock(&o->cs); o->signaled = 0; plat_unlock(&o->cs); }
static int ev_wait_ms(Obj *o, uint32_t ms) {
    int ok = 1;
    plat_lock(&o->cs);
    while (!o->signaled) {
        if (!plat_cond_wait_ms(&o->cv, &o->cs, ms)) { ok = o->signaled; break; }
    }
    if (ok && o->autoclear) o->signaled = 0;
    plat_unlock(&o->cs);
    return ok;
}
static void ev_wait(Ctx *c) { ev_wait_ms(obj(A0), PLAT_INFINITE); }
static void ev_trywait(Ctx *c) { RET(ev_wait_ms(obj(A0), 0)); }
static void ev_timedwait(Ctx *c) { RET(ev_wait_ms(obj(A0), ns_to_ms((int64_t)A1))); }

/* ---- system events shared through kernel handles.
 * [doc] A SystemEvent is a kernel event: CreateSystemEvent makes it with a readable and a writable handle; another
 * SystemEventType (in this or another process) can be attached to those handles, and a signal through one is seen by
 * every waiter on the event. SMO's audio server (lifted SDK code) creates the renderer's event and the client attaches
 * the game's SystemEvent to its readable handle. Model: handles name host event objects (g_handles); an attached
 * SystemEventType resolves to the event it shares (Obj.alias, followed by obj()). Each SystemEventType keeps ITS OWN
 * clear mode for its waits (AutoClear = a successful wait clears the shared event), as on the console. */
#define MAX_HANDLES 1024
static Obj *g_handles[MAX_HANDLES];
static int32_t g_handle_next = 1;
static uint32_t handle_new(Obj *o) {
    int32_t i = plat_inc(&g_handle_next) - 1;
    if (i >= MAX_HANDLES) guest_trap("os: too many system event handles (os_threads.c MAX_HANDLES)", 0);
    g_handles[i] = o;
    return 0x50000u + (uint32_t)i;
}
static Obj *handle_obj(uint32_t h) { uint32_t i = h - 0x50000u; return i < MAX_HANDLES ? g_handles[i] : NULL; }
static int sev_wait(uint64_t key, uint32_t ms) {
    Obj *raw = obj_raw(key), *o = obj(key);
    int ok = 1;
    plat_lock(&o->cs);
    while (!o->signaled) {
        if (!plat_cond_wait_ms(&o->cv, &o->cs, ms)) { ok = o->signaled; break; }
    }
    if (ok && raw->autoclear) o->signaled = 0;
    plat_unlock(&o->cs);
    return ok;
}
static void sev_create(Ctx *c) {               /* (SystemEventType*, EventClearMode, bool interprocess) -> Result */
    Obj *raw = obj_raw(A0);
    raw->alias = NULL;
    obj_reset(A0);
    raw->autoclear = A1 == 1;
    raw->rhandle = handle_new(raw);
    raw->whandle = handle_new(raw);
    RET(0);
}
static void sev_attach_readable(Ctx *c) {      /* (SystemEventType*, Handle, bool managed, EventClearMode) */
    Obj *raw = obj_raw(A0), *t = handle_obj((uint32_t)A1);
    if (!t) guest_trap("os: AttachReadableHandleToSystemEvent with a handle no event of this process has", c->x[30] - 4);
    raw->alias = t == raw ? NULL : t;
    raw->autoclear = A3 == 1;
}
/* Host side: a new system event with a handle (as a system module would create one and pass its readable handle);
 * returns the handle, *key receives a host key to signal it with os_signal_event. */
uint32_t os_system_event_new(int autoclear, uint64_t *key) {
    uint64_t k = (uint64_t)(uintptr_t)calloc(1, 64);            /* a unique key no guest object has */
    obj_reset(k);
    Obj *o = obj_raw(k);
    o->autoclear = autoclear;
    o->rhandle = handle_new(o);
    o->whandle = handle_new(o);
    if (key) *key = k;
    return o->rhandle;
}
static void sev_get_readable(Ctx *c) { Obj *o = obj(A0); if (!o->rhandle) o->rhandle = handle_new(o); RET(o->rhandle); }
static void sev_destroy(Ctx *c) { obj_raw(A0)->alias = NULL; }
static void sev_wait_h(Ctx *c) { sev_wait(A0, PLAT_INFINITE); }
static void sev_trywait_h(Ctx *c) { RET(sev_wait(A0, 0)); }
static void sev_timedwait_h(Ctx *c) { RET(sev_wait(A0, ns_to_ms((int64_t)A1))); }

/* ---------------------------------------------------------------- condition variables */
static void cv_wait(Ctx *c) { plat_cond_wait_ms(&obj(A0)->cv, &obj(A1)->cs, PLAT_INFINITE); }
static void cv_timedwait(Ctx *c) {                    /* returns ConditionVariableStatus: 0 timeout, 1 signalled */
    RET(plat_cond_wait_ms(&obj(A0)->cv, &obj(A1)->cs, ns_to_ms((int64_t)A2)) ? 1 : 0);
}
static void cv_signal(Ctx *c) { plat_cond_signal(&obj(A0)->cv); }
static void cv_broadcast(Ctx *c) { plat_cond_broadcast(&obj(A0)->cv); }

/* ---------------------------------------------------------------- message queues (of 64-bit values) */
static void mq_init(Ctx *c) {                         /* (queue, buffer, count): host-side storage is used */
    obj_reset(A0);
    Obj *o = obj(A0);
    free(o->buf);
    o->cap = A2 ? A2 : 1;
    o->buf = calloc((size_t)o->cap, 8);
}
static int mq_put(Obj *o, uint64_t v, int front, uint32_t ms) {
    int ok = 1;
    plat_lock(&o->cs);
    while (o->len == o->cap) {
        if (!plat_cond_wait_ms(&o->cv, &o->cs, ms)) { ok = o->len < o->cap; break; }
    }
    if (ok) {
        if (front) { o->head = (o->head + o->cap - 1) % o->cap; o->buf[o->head] = v; }
        else o->buf[(o->head + o->len) % o->cap] = v;
        o->len++;
        plat_cond_broadcast(&o->cv);
    }
    plat_unlock(&o->cs);
    return ok;
}
static int mq_get(Obj *o, uint64_t out, uint32_t ms) {
    int ok = 1;
    plat_lock(&o->cs);
    while (o->len == 0) {
        if (!plat_cond_wait_ms(&o->cv, &o->cs, ms)) { ok = o->len > 0; break; }
    }
    if (ok) {
        wr64(out, o->buf[o->head]);
        o->head = (o->head + 1) % o->cap;
        o->len--;
        plat_cond_broadcast(&o->cv);
    }
    plat_unlock(&o->cs);
    return ok;
}
static void mq_send(Ctx *c) { mq_put(obj(A0), A1, 0, PLAT_INFINITE); }
static void mq_trysend(Ctx *c) { RET(mq_put(obj(A0), A1, 0, 0)); }
static void mq_jam(Ctx *c) { mq_put(obj(A0), A1, 1, PLAT_INFINITE); }
static void mq_tryjam(Ctx *c) { RET(mq_put(obj(A0), A1, 1, 0)); }
static void mq_receive(Ctx *c) { mq_get(obj(A1), A0, PLAT_INFINITE); }
static void mq_tryreceive(Ctx *c) { RET(mq_get(obj(A1), A0, 0)); }

/* ---------------------------------------------------------------- fibers: the game makes none (nn::os::InitializeFiber is
 * not one of its imports), so no fiber is ever running */
static void fib_current(Ctx *c) { RET(0); }

/* ---------------------------------------------------------------- misc */
static void os_ConvertToTick(Ctx *c) { RET((uint64_t)((double)(int64_t)A0 * (double)TICK_HZ / 1e9)); }
static void os_ConvertToTimeSpan(Ctx *c) { RET((uint64_t)((double)(int64_t)A0 * 1e9 / (double)TICK_HZ)); }
/* The console fills the buffer from a generator seeded with kernel entropy: unpredictable bytes. Same here, from the
 * kernel's generator (getrandom). */
/* Test switch SMO_FIXED_SEED=<n>: the bytes come from a splitmix64 stream seeded with n instead, so
 * that a scripted run sees the same random choices in every run. Each call is logged (first 16). */
int host_fixed_random(void *buffer, uint64_t size, const char *who, uint64_t lr) {
    static int mode = -1;
    static volatile int32_t lock, calls;
    static uint64_t state;
    if (mode < 0) { const char *e = getenv("SMO_FIXED_SEED"); if (e && *e) { state = strtoull(e, NULL, 0) * 0x9E3779B97F4A7C15ull + 1; mode = 1; } else mode = 0; }
    if (!mode) return 0;
    while (plat_xchg(&lock, 1)) plat_pause();
    for (uint64_t i = 0; i < size; i += 8) {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
        memcpy((uint8_t *)buffer + i, &z, size - i < 8 ? (size_t)(size - i) : 8);
    }
    int32_t n = ++calls;
    plat_xchg(&lock, 0);
    if (n <= 16) printf("fixed seed: %s call %d, %llu bytes, caller %llx\n", who, n, (unsigned long long)size, (unsigned long long)lr);
    return 1;
}
static void os_GenerateRandomBytes(Ctx *c) {
    uint8_t *p = (uint8_t *)(uintptr_t)A0;
    if (host_fixed_random(p, A1, "nn::os::GenerateRandomBytes", c->x[30] - 4)) return;
    for (uint64_t done = 0; done < A1;) {
        uint32_t n = A1 - done > 0x10000000 ? 0x10000000 : (uint32_t)(A1 - done);
        if (!plat_random(p + done, n)) guest_trap("nn::os::GenerateRandomBytes: the system random generator failed", c->x[30] - 4);
        done += n;
    }
}

#define OS(n) "_ZN2nn2os" n
static const ImportDef thread_imports[] = {
    {OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"), os_CreateThread},
    {OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii"), os_CreateThreadOnCore},
    {OS("20GetCurrentCoreNumberEv"), os_GetCurrentCoreNumber},
    {OS("11StartThreadEPNS0_10ThreadTypeE"), os_StartThread}, {OS("10WaitThreadEPNS0_10ThreadTypeE"), os_WaitThread},
    {OS("13DestroyThreadEPNS0_10ThreadTypeE"), os_DestroyThread}, {OS("13SetThreadNameEPNS0_10ThreadTypeEPKc"), os_SetThreadName},
    {OS("17SetThreadCoreMaskEPNS0_10ThreadTypeEim"), os_SetThreadCoreMask},
    {OS("17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE"), os_GetThreadCoreMask},   /* not an import of the game: nnosGetThreadCoreMask reaches it (nnmisc.c FWD) */
    {OS("26GetThreadAvailableCoreMaskEv"), os_AvailableCoreMask},
    {OS("16GetCurrentThreadEv"), os_GetCurrentThread}, {OS("17GetThreadPriorityEPKNS0_10ThreadTypeE"), os_GetThreadPriority},
    {OS("20ChangeThreadPriorityEPNS0_10ThreadTypeEi"), os_ChangeThreadPriority}, {OS("11GetThreadIdEPKNS0_10ThreadTypeE"), os_GetThreadId},
    {OS("11YieldThreadEv"), os_YieldThread}, {OS("11SleepThreadENS_8TimeSpanE"), os_SleepThread},
    {OS("19GetCurrentStackInfoEPmS1_"), os_GetCurrentStackInfo},
    {OS("15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE"), os_AllocateTlsSlot},
    {OS("11FreeTlsSlotENS0_7TlsSlotE"), os_FreeTlsSlot}, {OS("11GetTlsValueENS0_7TlsSlotE"), os_GetTlsValue}, {OS("11SetTlsValueENS0_7TlsSlotEm"), os_SetTlsValue},
    {OS("15InitializeMutexEPNS0_9MutexTypeEbi"), mtx_init}, {OS("13FinalizeMutexEPNS0_9MutexTypeE"), nothing},
    {OS("9LockMutexEPNS0_9MutexTypeE"), mtx_lock}, {OS("11UnlockMutexEPNS0_9MutexTypeE"), mtx_unlock}, {OS("12TryLockMutexEPNS0_9MutexTypeE"), mtx_trylock},
    {OS("15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE"), ev_init}, {OS("13FinalizeEventEPNS0_9EventTypeE"), nothing},
    {OS("11SignalEventEPNS0_9EventTypeE"), ev_signal}, {OS("10ClearEventEPNS0_9EventTypeE"), ev_clear}, {OS("9WaitEventEPNS0_9EventTypeE"), ev_wait},
    {OS("20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE"), ev_init}, {OS("18FinalizeLightEventEPNS0_14LightEventTypeE"), nothing},
    {OS("16SignalLightEventEPNS0_14LightEventTypeE"), ev_signal}, {OS("15ClearLightEventEPNS0_14LightEventTypeE"), ev_clear},
    {OS("14WaitLightEventEPNS0_14LightEventTypeE"), ev_wait},
    {OS("18DestroySystemEventEPNS0_15SystemEventTypeE"), sev_destroy},
    {OS("18TryWaitSystemEventEPNS0_15SystemEventTypeE"), sev_trywait_h}, {OS("15WaitSystemEventEPNS0_15SystemEventTypeE"), sev_wait_h},
    {OS("20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE"), sev_timedwait_h},
    {OS("27InitializeConditionVariableEPNS0_21ConditionVariableTypeE"), mtx_init}, {OS("25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE"), nothing},
    {OS("21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE"), cv_wait},
    {OS("26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE"), cv_timedwait},
    {OS("23SignalConditionVariableEPNS0_21ConditionVariableTypeE"), cv_signal}, {OS("26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE"), cv_broadcast},
    {OS("22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm"), mq_init}, {OS("20FinalizeMessageQueueEPNS0_16MessageQueueTypeE"), nothing},
    {OS("16SendMessageQueueEPNS0_16MessageQueueTypeEm"), mq_send}, {OS("19TrySendMessageQueueEPNS0_16MessageQueueTypeEm"), mq_trysend},
    {OS("15JamMessageQueueEPNS0_16MessageQueueTypeEm"), mq_jam}, {OS("18TryJamMessageQueueEPNS0_16MessageQueueTypeEm"), mq_tryjam},
    {OS("19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"), mq_receive}, {OS("22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"), mq_tryreceive},
    {OS("15GetCurrentFiberEv"), fib_current},
    {OS("13ConvertToTickENS_8TimeSpanE"), os_ConvertToTick}, {OS("17ConvertToTimeSpanENS0_4TickE"), os_ConvertToTimeSpan},
    {OS("19GenerateRandomBytesEPvm"), os_GenerateRandomBytes},
};

/* SDK 3.5.1 forms the game imports. SetThreadNamePointer(ThreadType*, const char*): [doc] the thread's name is
 * the given string (kept by pointer); recorded for the thread reports. */
static void os_SetThreadNamePointer(Ctx *c) { obj(A0)->name = A1; }
/* GetThreadNamePointer(thread): the name set for it; a thread never named gets "Thread_0x<address>" as nn::os names it
 * (kept, so the pointer stays valid). Read by sead::nnsdk::PrintThreadInformation. */
static void os_GetThreadNamePointer(Ctx *c) {
    Obj *o = obj(A0);
    if (!o->name) { char *s = malloc(32); if (s) { snprintf(s, 32, "Thread_0x%llx", (unsigned long long)A0); o->name = (uint64_t)(uintptr_t)s; } }
    RET(o->name);
}
static const ImportDef smo_thread_imports[] = {
    {OS("19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE"), ev_timedwait},
    {OS("20SetThreadNamePointerEPNS0_10ThreadTypeEPKc"), os_SetThreadNamePointer},
    {"nnosSetThreadNamePointer", os_SetThreadNamePointer},
    /* TryWaitEvent: the movie player (MovieDecoderPlayer::update
     * polls its decoder's buffer events with it) and the sound library's loaders (nn::atk ...Loader::IsInUse, Finalize). */
    {OS("12TryWaitEventEPNS0_9EventTypeE"), ev_trywait},
    {OS("20GetThreadNamePointerEPKNS0_10ThreadTypeE"), os_GetThreadNamePointer},
};
/* the system-event entry points, also for lifted SDK callers (sdklift_rules.txt cuts them; installed below with
 * sdk_host_override). Create, AttachReadableHandle, Signal, Clear and GetReadableHandle are not imports of the game:
 * only the lifted SDK code calls them (the audio renderer's events). */
static const ImportDef sev_all[] = {
    {OS("17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb"), sev_create},
    {OS("33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"), sev_attach_readable},
    {OS("18DestroySystemEventEPNS0_15SystemEventTypeE"), sev_destroy},
    {OS("15WaitSystemEventEPNS0_15SystemEventTypeE"), sev_wait_h},
    {OS("18TryWaitSystemEventEPNS0_15SystemEventTypeE"), sev_trywait_h},
    {OS("20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE"), sev_timedwait_h},
    {OS("17SignalSystemEventEPNS0_15SystemEventTypeE"), ev_signal},
    {OS("16ClearSystemEventEPNS0_15SystemEventTypeE"), ev_clear},
    {OS("30GetReadableHandleOfSystemEventEPKNS0_15SystemEventTypeE"), sev_get_readable},
};
void sdk_host_override(const char *name, void (*fn)(Ctx *));   /* sdklift_rt.c */
void register_thread_imports(void) {
    register_imports(thread_imports, (int)(sizeof thread_imports / sizeof thread_imports[0]));
    register_imports(sev_all, (int)(sizeof sev_all / sizeof sev_all[0]));
    register_imports(smo_thread_imports, (int)(sizeof smo_thread_imports / sizeof smo_thread_imports[0]));
    for (int i = 0; i < (int)(sizeof sev_all / sizeof sev_all[0]); i++) sdk_host_override(sev_all[i].name, sev_all[i].fn);
}
int guest_thread_count(void) { return (int)g_thread_count; }

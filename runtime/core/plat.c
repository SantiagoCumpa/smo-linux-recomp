/* The host platform layer: see plat.h. Linux only. */
#include "plat.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <execinfo.h>
#include <ucontext.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/sysinfo.h>

/* ---- locks ---- */
void plat_lock_init(plat_lock_t *l) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(l, &a);
    pthread_mutexattr_destroy(&a);
}

static void deadline_after(struct timespec *ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

int plat_cond_wait_ms(plat_cond_t *c, plat_lock_t *l, uint32_t ms) {
    if (ms == PLAT_INFINITE) return pthread_cond_wait(c, l) == 0;
    struct timespec ts;
    deadline_after(&ts, ms);
    return pthread_cond_timedwait(c, l, &ts) == 0;
}

struct plat_event { pthread_mutex_t m; pthread_cond_t c; int manual, set; };

plat_event *plat_event_new(int manual, int initially_set) {
    plat_event *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->manual = manual;
    e->set = initially_set;
    return e;
}

void plat_event_free(plat_event *e) {
    if (!e) return;
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
    free(e);
}

void plat_event_set(plat_event *e) {
    pthread_mutex_lock(&e->m);
    e->set = 1;
    if (e->manual) pthread_cond_broadcast(&e->c); else pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}

void plat_event_reset(plat_event *e) {
    pthread_mutex_lock(&e->m);
    e->set = 0;
    pthread_mutex_unlock(&e->m);
}

int plat_event_wait(plat_event *e, uint32_t ms) {
    struct timespec ts;
    if (ms != PLAT_INFINITE) deadline_after(&ts, ms);
    pthread_mutex_lock(&e->m);
    while (!e->set) {
        int r = ms == PLAT_INFINITE ? pthread_cond_wait(&e->c, &e->m) : pthread_cond_timedwait(&e->c, &e->m, &ts);
        if (r == ETIMEDOUT) break;
    }
    int got = e->set;
    if (got && !e->manual) e->set = 0;
    pthread_mutex_unlock(&e->m);
    return got;
}

struct plat_sem { sem_t s; };

plat_sem *plat_sem_new(int initial) {
    plat_sem *s = calloc(1, sizeof *s);
    sem_init(&s->s, 0, (unsigned)initial);
    return s;
}

void plat_sem_post(plat_sem *s, int n) {
    while (n-- > 0) sem_post(&s->s);
}

int plat_sem_wait(plat_sem *s, uint32_t ms) {
    if (ms == PLAT_INFINITE) {
        while (sem_wait(&s->s) != 0) if (errno != EINTR) return 0;
        return 1;
    }
    struct timespec ts;
    deadline_after(&ts, ms);
    while (sem_timedwait(&s->s, &ts) != 0) if (errno != EINTR) return 0;
    return 1;
}

/* ---- time ---- */
void plat_sleep_ns(int64_t ns) {
    if (ns <= 0) { sched_yield(); return; }
    struct timespec ts = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

void plat_sleep_ms(uint32_t ms) {
    if (ms == 0) { sched_yield(); return; }
    plat_sleep_ns((int64_t)ms * 1000000LL);
}

void plat_sleep_until(int64_t ticks) {
    struct timespec ts = {(time_t)(ticks / 1000000000LL), (long)(ticks % 1000000000LL)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

int64_t plat_unix_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- threads ---- */
struct plat_thread {
    pthread_t th;
    plat_thread_fn fn;
    void *arg;
    volatile pid_t tid;
    pthread_mutex_t m;
    pthread_cond_t c;
    int done, detached;
};

static void thread_free(plat_thread *t) {
    pthread_mutex_destroy(&t->m);
    pthread_cond_destroy(&t->c);
    free(t);
}

static void *thread_entry(void *p) {
    plat_thread *t = p;
    plat_thread_altstack();
    __atomic_store_n(&t->tid, gettid(), __ATOMIC_RELEASE);
    t->fn(t->arg);
    pthread_mutex_lock(&t->m);
    t->done = 1;
    int detached = t->detached;
    pthread_cond_broadcast(&t->c);
    pthread_mutex_unlock(&t->m);
    if (detached) thread_free(t);
    return NULL;
}

plat_thread *plat_thread_start(plat_thread_fn fn, void *arg, size_t stack) {
    plat_thread *t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (stack) pthread_attr_setstacksize(&a, (stack + 0xffff) & ~(size_t)0xffff);
    int r = pthread_create(&t->th, &a, thread_entry, t);
    pthread_attr_destroy(&a);
    if (r) { thread_free(t); return NULL; }
    return t;
}

int plat_thread_join(plat_thread *t, uint32_t ms) {
    if (!t) return 1;
    pthread_mutex_lock(&t->m);
    if (ms == PLAT_INFINITE) {
        while (!t->done) pthread_cond_wait(&t->c, &t->m);
    } else {
        struct timespec ts;
        deadline_after(&ts, ms);
        while (!t->done) if (pthread_cond_timedwait(&t->c, &t->m, &ts) == ETIMEDOUT) break;
    }
    int done = t->done;
    pthread_mutex_unlock(&t->m);
    if (!done) return 0;
    pthread_join(t->th, NULL);
    thread_free(t);
    return 1;
}

void plat_thread_detach(plat_thread *t) {
    if (!t) return;
    pthread_mutex_lock(&t->m);
    int done = t->done;
    t->detached = 1;
    pthread_mutex_unlock(&t->m);
    pthread_detach(t->th);
    if (done) thread_free(t);
}

pid_t plat_thread_tid(plat_thread *t) {
    pid_t id;
    while (!(id = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE))) sched_yield();
    return id;
}

void plat_set_thread_name(const char *name) {
    char n[16];
    snprintf(n, sizeof n, "%s", name);
    prctl(PR_SET_NAME, n, 0, 0, 0);
}

int plat_set_affinity(pid_t tid, const uint64_t *mask, int ncpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    for (int i = 0; i < ncpu && i < CPU_SETSIZE; i++)
        if (mask[i / 64] >> (i % 64) & 1) CPU_SET(i, &s);
    return sched_setaffinity(tid, sizeof s, &s) == 0;
}

int plat_cpu(void) { int c = sched_getcpu(); return c < 0 ? 0 : c; }
uint64_t plat_thread_cpu_ns(pid_t tid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", (int)tid);
    FILE *f = fopen(path, "r");
    unsigned long long ns = 0;
    if (f) { if (fscanf(f, "%llu", &ns) != 1) ns = 0; fclose(f); }
    return ns;
}

void plat_system_cpu(uint64_t *busy, uint64_t *idle) {
    unsigned long long v[10] = {0};
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) < 4) memset(v, 0, sizeof v);
        fclose(f);
    }
    *idle = v[3] + v[4];
    *busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
}

void plat_process_cpu(uint64_t *ticks, uint64_t *faults) {
    char buf[1024];
    *ticks = *faults = 0;
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');                             /* after the command name, which may hold spaces */
    if (!p) return;
    unsigned long long minflt = 0, majflt = 0, ut = 0, st = 0;
    /* fields from 3: state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %llu %*u %llu %*u %llu %llu", &minflt, &majflt, &ut, &st) == 4) {
        *ticks = ut + st;
        *faults = minflt + majflt;
    }
}

int plat_cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }

/* ---- memory ---- */
static int prot_of(int p) {
    return (p & 1 ? PROT_READ : 0) | (p & 2 ? PROT_WRITE : 0) | (p & 4 ? PROT_EXEC : 0);
}

void *plat_reserve(void *addr, size_t size) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (addr ? MAP_FIXED_NOREPLACE : 0);
    void *p = mmap(addr, size, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (addr && p != addr) { munmap(p, size); return NULL; }   /* an old kernel that ignored the hint */
    return p;
}

int plat_commit(void *addr, size_t size, int prot) { return mprotect(addr, size, prot_of(prot)) == 0; }
int plat_protect(void *addr, size_t size, int prot) { return mprotect(addr, size, prot_of(prot)) == 0; }

int plat_decommit(void *addr, size_t size) {
    void *p = mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    return p == addr;
}

void plat_release(void *addr, size_t size) { if (addr) munmap(addr, size); }

void *plat_alloc(void *addr, size_t size, int prot) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (addr ? MAP_FIXED_NOREPLACE : 0);
    void *p = mmap(addr, size, prot_of(prot), flags, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (addr && p != addr) { munmap(p, size); return NULL; }
    return p;
}

int plat_query(const void *addr, uintptr_t *base, size_t *size, int *prot) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t a = (uintptr_t)addr;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) != 3) continue;
        if (a >= lo && a < hi) {
            if (base) *base = lo;
            if (size) *size = hi - lo;
            if (prot) *prot = (perm[0] == 'r' ? 1 : 0) | (perm[1] == 'w' ? 2 : 0) | (perm[2] == 'x' ? 4 : 0);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int plat_readable(const void *addr, size_t n) {
    char buf[256];
    for (size_t done = 0; done < n;) {
        size_t k = n - done < sizeof buf ? n - done : sizeof buf;
        struct iovec local = {buf, k}, remote = {(void *)((uintptr_t)addr + done), k};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)k) return 0;
        done += k;
    }
    return 1;
}

static _Thread_local void *t_altstack;
void plat_thread_altstack(void) {
    if (t_altstack) return;
    size_t size = 256 << 10;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    stack_t ss = {.ss_sp = p, .ss_size = size, .ss_flags = 0};
    if (sigaltstack(&ss, NULL) == 0) t_altstack = p; else munmap(p, size);
}

uint64_t plat_rss_bytes(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long total = 0, rss = 0;
    if (f) { if (fscanf(f, "%lu %lu", &total, &rss) != 2) rss = 0; fclose(f); }
    return (uint64_t)rss * (uint64_t)sysconf(_SC_PAGESIZE);
}

uint64_t plat_total_memory(void) {
    struct sysinfo si;
    if (sysinfo(&si)) return 0;
    return (uint64_t)si.totalram * si.mem_unit;
}

/* ---- files ---- */
static char g_exe[PATH_MAX], g_exe_dir[PATH_MAX];
static pthread_once_t g_exe_once = PTHREAD_ONCE_INIT;

static void exe_init(void) {
    ssize_t n = readlink("/proc/self/exe", g_exe, sizeof g_exe - 1);
    g_exe[n > 0 ? n : 0] = 0;
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", g_exe);
    char *s = strrchr(g_exe_dir, '/');
    if (s) *s = 0; else snprintf(g_exe_dir, sizeof g_exe_dir, ".");
}

const char *plat_exe_path(void) { pthread_once(&g_exe_once, exe_init); return g_exe; }
const char *plat_exe_dir(void) { pthread_once(&g_exe_once, exe_init); return g_exe_dir; }
const char *plat_exe_name(void) {
    const char *p = plat_exe_path(), *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int plat_is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
int plat_is_file(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISREG(st.st_mode); }
int64_t plat_file_size(const char *path) { struct stat st; return stat(path, &st) == 0 ? (int64_t)st.st_size : -1; }

int plat_mkdir(const char *path) { return mkdir(path, 0755) == 0 || (errno == EEXIST && plat_is_dir(path)); }

int plat_mkdirs(const char *path) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        mkdir(p, 0755);
        *s = '/';
    }
    return plat_mkdir(p);
}

int plat_rename_replace(const char *from, const char *to) { return rename(from, to) == 0; }

int plat_fsync_path(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

int plat_fsync_dir(const char *path_in_dir) {
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s", path_in_dir);
    char *s = strrchr(d, '/');
    if (s) *s = 0; else snprintf(d, sizeof d, ".");
    int fd = open(d[0] ? d : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

void plat_exit(int code) {
    fflush(NULL);
    _exit(code);
}

int plat_argc;
char **plat_argv;

/* glibc calls .init_array functions with (argc, argv, envp) */
static void plat_args(int argc, char **argv, char **envp) { (void)envp; plat_argc = argc; plat_argv = argv; }
__attribute__((section(".init_array"), used)) static void (*const plat_args_init)(int, char **, char **) = plat_args;

/* ---- sampling another thread's stack ---- */
#define SAMPLE_SIG (SIGRTMIN + 4)
static pthread_mutex_t g_sample_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uintptr_t *pcs; int max, n; volatile int done; } g_sample;

static void sample_handler(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = ucv;
    void *frames[96];
    int n = backtrace(frames, 96), k = 0;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    if (g_sample.max > 0) g_sample.pcs[k++] = pc;
    int start = 0;
    for (int i = 0; i < n; i++) if ((uintptr_t)frames[i] == pc) { start = i + 1; break; }
    for (int i = start; i < n && k < g_sample.max; i++) g_sample.pcs[k++] = (uintptr_t)frames[i];
    g_sample.n = k;
    __atomic_store_n(&g_sample.done, 1, __ATOMIC_RELEASE);
}

int plat_thread_backtrace(pid_t tid, uintptr_t *pcs, int max) {
    static int installed;
    pthread_mutex_lock(&g_sample_lock);
    if (!installed) {
        void *warm[2]; backtrace(warm, 2);                  /* loads the unwinder outside a signal handler */
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = sample_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SAMPLE_SIG, &sa, NULL);
        installed = 1;
    }
    g_sample.pcs = pcs; g_sample.max = max; g_sample.n = 0; g_sample.done = 0;
    int n = 0;
    if (syscall(SYS_tgkill, getpid(), tid, SAMPLE_SIG) == 0) {
        int64_t end = plat_ticks() + 200000000LL;
        while (!__atomic_load_n(&g_sample.done, __ATOMIC_ACQUIRE) && plat_ticks() < end) sched_yield();
        if (g_sample.done) n = g_sample.n;
    }
    g_sample.max = 0;
    pthread_mutex_unlock(&g_sample_lock);
    return n;
}

int plat_random(void *buf, size_t n) {
    for (size_t done = 0; done < n;) {
        ssize_t r = getrandom((uint8_t *)buf + done, n - done, 0);
        if (r < 0) { if (errno == EINTR) continue; return 0; }
        done += (size_t)r;
    }
    return 1;
}

/* The host platform layer (Linux only): the few patterns the runtime repeats in dozens of files, written once over
 * POSIX/pthreads/Linux system calls. Everything else that is specific to a file (the window, audio, fonts, ...) is
 * written directly in that file.
 *
 *   locks       plat_lock_t (recursive mutex), plat_rwlock_t (reader/writer lock), plat_cond_t (condition variable
 *               used with a plat_lock_t), plat_event (manual or auto-reset event), plat_sem (counting semaphore)
 *   atomics     plat_inc / plat_dec / plat_add (return the new value), plat_xadd / plat_xchg / plat_cas (return the
 *               old value): sequentially consistent __atomic builtins on any integer or pointer width
 *   time        plat_ticks() / PLAT_TICK_HZ (CLOCK_MONOTONIC in ns), plat_ms(), plat_sleep_ms(), plat_sleep_until(),
 *               plat_qpc / plat_qpf (a counter and its frequency in a plat_counter, for the runtime's
 *               "now.QuadPart - t0.QuadPart" arithmetic)
 *   threads     plat_thread_start(fn, arg, stack) with fn returning uint32_t, plat_thread_join, plat_tid() (gettid),
 *               plat_set_thread_name, plat_set_affinity, plat_cpu() (sched_getcpu), plat_pause() (spin-wait hint)
 *   memory      plat_reserve / plat_commit / plat_protect / plat_release over mmap/mprotect/munmap (PLAT_R, PLAT_RW,
 *               PLAT_RX, PLAT_NONE), plat_query (a host address -> its mapping, from /proc/self/maps),
 *               plat_aligned_alloc / plat_aligned_free, plat_rss_bytes (/proc/self/statm)
 *   files       plat_exe_path / plat_exe_dir (/proc/self/exe), plat_mkdir, plat_mkdirs, plat_is_dir, plat_is_file,
 *               plat_file_size, plat_rename_replace, plat_fsync_path, plat_fsync_dir
 *   process     plat_exit (flushes stdio, then _exit: no atexit handlers, like ExitProcess from a worker thread)
 */
#ifndef SMO_PLAT_H
#define SMO_PLAT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- locks ---- */
typedef pthread_mutex_t plat_lock_t;                    /* recursive: the same thread may take it again */
#define PLAT_LOCK_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
void plat_lock_init(plat_lock_t *l);
static inline void plat_lock_destroy(plat_lock_t *l) { pthread_mutex_destroy(l); }
static inline void plat_lock(plat_lock_t *l) { pthread_mutex_lock(l); }
static inline void plat_unlock(plat_lock_t *l) { pthread_mutex_unlock(l); }
static inline int plat_trylock(plat_lock_t *l) { return pthread_mutex_trylock(l) == 0; }

typedef pthread_rwlock_t plat_rwlock_t;
#define PLAT_RWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void plat_rw_init(plat_rwlock_t *l) { pthread_rwlock_init(l, NULL); }
static inline void plat_rw_wlock(plat_rwlock_t *l) { pthread_rwlock_wrlock(l); }
static inline void plat_rw_wunlock(plat_rwlock_t *l) { pthread_rwlock_unlock(l); }
static inline void plat_rw_rlock(plat_rwlock_t *l) { pthread_rwlock_rdlock(l); }
static inline void plat_rw_runlock(plat_rwlock_t *l) { pthread_rwlock_unlock(l); }
static inline int plat_rw_trywlock(plat_rwlock_t *l) { return pthread_rwlock_trywrlock(l) == 0; }
static inline int plat_rw_tryrlock(plat_rwlock_t *l) { return pthread_rwlock_tryrdlock(l) == 0; }

typedef pthread_cond_t plat_cond_t;                     /* CLOCK_REALTIME based; waits are short and re-checked */
#define PLAT_COND_INIT PTHREAD_COND_INITIALIZER
static inline void plat_cond_init(plat_cond_t *c) { pthread_cond_init(c, NULL); }
static inline void plat_cond_signal(plat_cond_t *c) { pthread_cond_signal(c); }
static inline void plat_cond_broadcast(plat_cond_t *c) { pthread_cond_broadcast(c); }
#define PLAT_INFINITE 0xFFFFFFFFu
/* waits on c with l held (once); 1 when woken, 0 after ms milliseconds (PLAT_INFINITE: no limit) */
int plat_cond_wait_ms(plat_cond_t *c, plat_lock_t *l, uint32_t ms);

typedef struct plat_event plat_event;                   /* manual: stays set until reset; auto: one waiter clears it */
plat_event *plat_event_new(int manual, int initially_set);
void plat_event_free(plat_event *e);
void plat_event_set(plat_event *e);
void plat_event_reset(plat_event *e);
int plat_event_wait(plat_event *e, uint32_t ms);         /* 1 signalled, 0 timed out */

typedef struct plat_sem plat_sem;
plat_sem *plat_sem_new(int initial);
void plat_sem_post(plat_sem *s, int n);
int plat_sem_wait(plat_sem *s, uint32_t ms);             /* 1 taken, 0 timed out */

/* ---- atomics (any integer or pointer type; all sequentially consistent) ---- */
#define plat_inc(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define plat_dec(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define plat_add(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define plat_xadd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define plat_or(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define plat_and(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define plat_xchg(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define plat_load(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define plat_store(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
/* compare and swap: if *p == expected, *p = desired; returns the old value (the Interlocked convention) */
#define plat_cas(p, desired, expected) __extension__({ __typeof__(*(p)) _e = (expected); \
    __atomic_compare_exchange_n((p), &_e, (desired), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); _e; })
#define plat_fence() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define plat_compiler_barrier() __asm__ __volatile__("" ::: "memory")
static inline void plat_pause(void) { __builtin_ia32_pause(); }
static inline void plat_yield(void) { sched_yield(); }

/* ---- time ---- */
#define PLAT_TICK_HZ 1000000000LL
static inline int64_t plat_ticks(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static inline uint64_t plat_ms(void) { return (uint64_t)(plat_ticks() / 1000000); }
typedef union { int64_t QuadPart; } plat_counter;       /* the counter and its frequency, as the runtime reads them */
static inline void plat_qpc(plat_counter *c) { c->QuadPart = plat_ticks(); }
static inline void plat_qpf(plat_counter *c) { c->QuadPart = PLAT_TICK_HZ; }
void plat_sleep_ms(uint32_t ms);
void plat_sleep_ns(int64_t ns);
void plat_sleep_until(int64_t ticks);                    /* absolute plat_ticks() deadline, CLOCK_MONOTONIC */
int64_t plat_unix_time_ns(void);                         /* CLOCK_REALTIME */

/* ---- threads ---- */
typedef struct plat_thread plat_thread;
typedef uint32_t (*plat_thread_fn)(void *arg);
/* stack 0: the default (8 MB); big stacks are only reserved (the kernel commits pages on use) */
plat_thread *plat_thread_start(plat_thread_fn fn, void *arg, size_t stack);
int plat_thread_join(plat_thread *t, uint32_t ms);        /* 1 when it ended (and is freed), 0 timed out */
void plat_thread_detach(plat_thread *t);
pid_t plat_thread_tid(plat_thread *t);                    /* its kernel thread id (waits until the thread runs) */
static inline pid_t plat_tid(void) { return gettid(); }
void plat_set_thread_name(const char *name);
int plat_set_affinity(pid_t tid, const uint64_t *mask, int ncpu);   /* tid 0: this thread */
int plat_cpu(void);
int plat_cpu_count(void);

/* ---- memory ---- */
enum { PLAT_NONE = 0, PLAT_R = 1, PLAT_RW = 3, PLAT_RX = 5, PLAT_RWX = 7 };
/* address space, no access: at addr exactly (fixed, fails if anything is there) or anywhere (addr NULL) */
void *plat_reserve(void *addr, size_t size);
int plat_commit(void *addr, size_t size, int prot);       /* zero-filled on first touch */
int plat_protect(void *addr, size_t size, int prot);
int plat_decommit(void *addr, size_t size);               /* back to no access, pages dropped */
void plat_release(void *addr, size_t size);
void *plat_alloc(void *addr, size_t size, int prot);      /* reserve + commit; addr NULL = anywhere */
/* the mapping holding addr: its start, size and protection (PLAT_*); 0 when nothing is mapped there */
int plat_query(const void *addr, uintptr_t *base, size_t *size, int *prot);
static inline void *plat_aligned_alloc(size_t size, size_t align) {
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    return posix_memalign(&p, align, size ? size : 1) ? NULL : p;
}
static inline void plat_aligned_free(void *p) { free(p); }
uint64_t plat_rss_bytes(void);
uint64_t plat_total_memory(void);

/* ---- files and paths ---- */
const char *plat_exe_path(void);                          /* absolute, from /proc/self/exe */
const char *plat_exe_dir(void);                           /* its folder, no trailing slash */
const char *plat_exe_name(void);                          /* its file name */
int plat_mkdir(const char *path);                         /* 1 when made or already a folder */
int plat_mkdirs(const char *path);                        /* every missing parent too */
int plat_is_dir(const char *path);
int plat_is_file(const char *path);
int64_t plat_file_size(const char *path);                 /* -1 when missing */
int plat_rename_replace(const char *from, const char *to);/* 1 on success */
int plat_fsync_path(const char *path);
int plat_fsync_dir(const char *path_in_dir);              /* the folder holding the file (a rename's durability) */
void plat_exit(int code) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif

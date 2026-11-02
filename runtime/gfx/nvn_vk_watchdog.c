/* nvn_vk_watchdog.c (included at the end of nvn_vk_draw.c: not a build unit of its own).
 *
 * THE FREEZE WATCHDOG. On by default in the Vulkan renderer (SMO_VK_WATCHDOG=0 off, =<seconds> another limit; default 5).
 * A thread looks twice a second at the time of the last frame that reached the window. When no frame was presented for the
 * limit while the process is alive, a report is printed and appended to <exe name>_freeze.txt next to the exe:
 *   - what the renderer was doing (frames, present attempts, failures, queues of the workers);
 *   - our locks (g_sh_lock, g_gpl_lock): the thread that owns each;
 *   - every wait of ours that was entered through WD_SLEEP (function, line, how long);
 *   - EVERY thread of the process (/proc/self/task): its name (guest thread name / render / main / the kernel's name),
 *     whether it used CPU in the last 300 ms, and its call stack (taken by a signal the thread answers itself:
 *     plat_thread_backtrace): module + offset (tools/lifter/mapaddr.py names them from the binary's symbols), a name when
 *     the dynamic linker knows one, and functions of the lifted game as "guest f_<address>".
 * The report is repeated after 4x the time (5 s, 20 s, 80 s, ...), at most 6 times a run; a line says when frames come back.
 * It only reads: nothing is unblocked or killed. A thread blocked in the kernel answers the signal and goes back to its wait
 * (system calls restart). */
const char *guest_thread_name_of(uint32_t tid);       /* os_threads.c */
int display_menu_held(void);                       /* display.c */
extern volatile int32_t g_guest_frozen;               /* hostrt.c */
/* fn_table / fn_table_count (runtime.h; -Dfn_table=(*game_fn_table_p)): the lifted game's functions, by guest address */
static volatile int32_t g_wd_quit;
static plat_thread *g_wd_thread;
static FILE *g_wd_fp;
static uint32_t g_wd_main_tid;
static void wdp(const char *fmt, ...) {
    char b[1400];
    va_list ap;
    va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, stdout);
    if (g_wd_fp) fputs(b, g_wd_fp);
}
/* The module of an address (dladdr), the game library's lifted functions by fn_table. */
static int wd_in_game(uint64_t pc) {
    Dl_info a, b;
    return dladdr((void *)(uintptr_t)pc, &a) && dladdr((void *)&fn_table[0], &b) && a.dli_fbase == b.dli_fbase;
}
static void wd_addr(uint64_t pc, char *out, size_t n) {
    Dl_info di;
    char sym[160] = "";
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_fname) { snprintf(out, n, "%llx (in no module)", (unsigned long long)pc); return; }
    const char *bn = strrchr(di.dli_fname, '/');
    bn = bn ? bn + 1 : di.dli_fname;
    uint64_t off = pc - (uint64_t)(uintptr_t)di.dli_fbase;
    if (wd_in_game(pc)) {
        uint64_t best = 0, guest = 0;
        for (int i = 0; i < fn_table_count; i++) { uint64_t f = (uint64_t)(uintptr_t)fn_table[i].fn; if (f <= pc && f > best) { best = f; guest = fn_table[i].addr; } }
        if (best && pc - best < 0x400000) snprintf(sym, sizeof sym, "guest f_%llx (host +0x%x)", (unsigned long long)guest, (unsigned)(pc - best));
    } else if (di.dli_sname) snprintf(sym, sizeof sym, "%.100s+0x%llx", di.dli_sname, (unsigned long long)(pc - (uint64_t)(uintptr_t)di.dli_saddr));
    snprintf(out, n, "%s+0x%llx%s%s%s", bn, (unsigned long long)off, sym[0] ? " [" : "", sym, sym[0] ? "]" : "");
}
static const char *wd_tname(uint32_t tid, char *buf, size_t n) {
    const char *g = tid == g_render_tid ? "render (host)" : tid == g_wd_main_tid ? "the process's first thread (boot.c main: the window's event loop)" : guest_thread_name_of(tid);
    if (g) return g;
    for (int i = 0; i < g_pw_nthread; i++) if (g_pw_tid[i] && (uint32_t)g_pw_tid[i] == tid) { snprintf(buf, n, "shader worker %d (host)", i); return buf; }
    char path[64], comm[32] = "";
    snprintf(path, sizeof path, "/proc/self/task/%u/comm", tid);
    FILE *f = fopen(path, "r");
    if (f) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); comm[strcspn(comm, "\n")] = 0; }
    snprintf(buf, n, "host thread \"%s\"", comm);
    return buf;
}
static void wd_owner(const char *name, plat_lock_t *cs) {
    char tb[64];
    uint32_t o = (uint32_t)cs->__data.__owner;
    if (o) wdp("  lock %s: OWNED by thread %lu (%s), recursion %u\n", name, (unsigned long)o, wd_tname(o, tb, sizeof tb), (unsigned)cs->__data.__count);
    else wdp("  lock %s: free\n", name);
}
/* the threads of this process: /proc/self/task */
static int wd_tasks(uint32_t *tids, int max) {
    DIR *d = opendir("/proc/self/task");
    int n = 0;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) if (e->d_name[0] >= '0' && e->d_name[0] <= '9') tids[n++] = (uint32_t)strtoul(e->d_name, NULL, 10);
    closedir(d);
    return n;
}
static int wd_nice(uint32_t tid) { errno = 0; int v = getpriority(PRIO_PROCESS, tid); return errno ? 0 : v; }
static void wd_dump(double stall_s, int64_t tries_since) {
    static struct { uint32_t tid; uint64_t cpu0, cpu1; int prio, nfr; uint64_t pc[28]; } th[400];
    char path[PATH_MAX + 16], tb[64], ab[400];
    plat_counter fq; plat_qpf(&fq);
    time_t now_t = time(NULL);
    struct tm st; localtime_r(&now_t, &st);
    uint32_t me = plat_tid(), pid = getpid();
    int nt = 0;
    snprintf(path, sizeof path, "%s_freeze.txt", plat_exe_path());
    g_wd_fp = fopen(path, "ab");
    wdp("\nnvn_vk_watchdog: ======== NO FRAME PRESENTED FOR %.1f s ======== %04d-%02d-%02d %02d:%02d:%02d, process %lu, game frame %lld\n", stall_s, st.tm_year + 1900, st.tm_mon + 1, st.tm_mday, st.tm_hour, st.tm_min, st.tm_sec, (unsigned long)pid, (long long)nvn_front_frame_next());
    wdp("  presents: %lld frames reached the window so far; present attempts since the last one that did: %lld (%s); frames not presented %lld, failed Vulkan calls %lld, swapchain made again %lld times; window %s\n",
        (long long)g_vk_frames, (long long)tries_since, tries_since ? "the game still presents but nothing reaches the window: see the failures" : "none: the game's present never came, or the render thread is held",
        (long long)g_vk_skipped, (long long)g_vk_errors, (long long)g_vk_recreated, display_focus_flags() & 2 ? "MINIMISED" : "not minimised");
    wdp("  render thread %lu, place in its present: %s; %s; scene near: %s\n", (unsigned long)g_render_tid, g_wd_where == 1 ? "the loading barrier (waits for the workers, at most 20 s)" : g_wd_where == 2 ? "vkAcquireNextImageKHR" : g_wd_where == 3 ? "blit and submit" : g_wd_where == 4 ? "vkQueuePresentKHR" : "not in a present",
        g_in_scene ? "the last frame drew a scene (compute dispatches)" : "the last frame drew no scene", scene_recent() ? "yes" : "no");
    wdp("  workers: %d threads; foreground jobs %lld queued / %lld done, background %lld / %lld; loading-barrier holds %lld (time-outs %lld)\n",
        g_pw_nthread, (long long)g_jq_queued, (long long)g_jq_done, (long long)g_bq_queued, (long long)g_bq_done,
        (long long)g_bar_n, (long long)g_bar_timeout);
    wd_owner("g_sh_lock (stage, pipeline and job tables)", &g_sh_lock);
    wd_owner("g_gpl_lock (vertex-input / fragment-output libraries, universal layout)", &g_gpl_lock);
    {   int any = 0;
        for (int i = 0; i < 64; i++) {
            WdWait w = g_wd_wait[i];
            if (!w.tid) continue;
            wdp("  wait of ours: thread %lu (%s) in %s, line %d of nvn_vk_pipelines.c / nvn_vk_draw.c, for %.1f s\n", (unsigned long)w.tid, wd_tname((uint32_t)w.tid, tb, sizeof tb), w.fn ? w.fn : "?", w.line, (double)(tk() - w.since) / (double)fq.QuadPart);
            any = 1;
        }
        if (!any) wdp("  waits of ours (stage / library / pipeline being made by another thread, queues): none entered\n");
    }
    g_wd_main_tid = pid;                           /* the process's first thread has the process's id */
    for (int pass = 0; pass < 2; pass++) {         /* CPU time of every thread, twice, 300 ms apart: who is running and who is blocked */
        static uint32_t tids[400];
        int nn = wd_tasks(tids, 400);
        for (int k = 0; k < nn; k++) {
            if (tids[k] == me) continue;
            int i;
            for (i = 0; i < nt && th[i].tid != tids[k]; i++) {}
            if (i == nt) { if (pass || nt == 400) continue; th[nt].tid = tids[k]; th[nt].cpu0 = th[nt].cpu1 = 0; th[nt].nfr = 0; th[nt].prio = 0; nt++; }
            uint64_t cpu = plat_thread_cpu_ns((pid_t)tids[k]);
            if (pass) th[i].cpu1 = cpu; else th[i].cpu0 = th[i].cpu1 = cpu;
            th[i].prio = wd_nice(tids[k]);
        }
        if (!pass) plat_sleep_ms(300);
    }
    for (int i = 0; i < nt; i++) {                 /* the stacks: each thread records its own when signalled */
        uintptr_t pcs[28];
        int np = plat_thread_backtrace((pid_t)th[i].tid, pcs, 28);
        for (int f = 0; f < np; f++) th[i].pc[f] = pcs[f];
        th[i].nfr = np;
    }
    wdp("  threads: %d (the watchdog's own not listed); \"cpu\" = CPU time used in 300 ms: 0.0 means blocked or idle\n", nt);
    for (int i = 0; i < nt; i++) {
        wdp("  thread %lu  %s  nice %d  cpu %.1f ms of 300\n", (unsigned long)th[i].tid, wd_tname(th[i].tid, tb, sizeof tb), th[i].prio, (double)(th[i].cpu1 - th[i].cpu0) / 1e6);
        for (int f = 0; f < th[i].nfr; f++) { wd_addr(th[i].pc[f], ab, sizeof ab); wdp("      %2d  %s\n", f, ab); }
    }
    wdp("nvn_vk_watchdog: ======== end of the report%s%s ========\n\n", g_wd_fp ? "; also appended to " : "", g_wd_fp ? path : "");
    fflush(stdout);
    if (g_wd_fp) { fclose(g_wd_fp); g_wd_fp = NULL; }
}
static uint32_t wd_main(void *arg) {
    double limit = *(double *)arg, next = limit, reported = 0;
    int64_t seen = 0, try0 = 0;
    int dumps = 0;
    plat_counter fq; plat_qpf(&fq);
    while (!g_wd_quit) {
        plat_sleep_ms(500);
        int64_t b = g_wd_beat;
        if (!b || !V.alive || g_wd_quit) continue;  /* armed by the first frame that reaches the window */
        if (b != seen) {
            if (reported > 0) { printf("nvn_vk_watchdog: frames reach the window again (the stall lasted between %.1f and %.1f s)\n", reported, reported + (double)(b - seen) / (double)fq.QuadPart); fflush(stdout); }
            seen = b; try0 = g_wd_try; next = limit; reported = 0;
            continue;
        }
        double s = (double)(tk() - b) / (double)fq.QuadPart;
        if (s < next || dumps >= 6) continue;
        if ((display_focus_flags() & 2) && g_wd_try != try0) { next = s + limit; continue; }      /* a minimised window: presents are skipped, the game runs */
        if (display_menu_held()) { next = s + limit; continue; }       /* the game is held on purpose (the Help page, smo_guide.c) */
        if (g_guest_frozen) { next = s + limit; continue; }            /* stopped on purpose (graphics.ini unfocused = pause, nnmisc.c oe_watch) */
        dumps++;
        wd_dump(s, g_wd_try - try0);
        reported = s; next = s * 4;
    }
    return 0;
}
/* SMO_HOSTPROF=<start s>,<seconds>[,<threads>] (debug aid): samples the stacks of the busiest threads (default 4, by CPU
 * time in half a second at the start) about every millisecond for that long, then prints per thread the functions met
 * most often: "self" when the sampled pc is in the function, "total" when the function is anywhere on the stack, in % of
 * the thread's samples. Names as in the watchdog's report: module + offset (and a dynamic symbol when there is one), the
 * lifted game's as "guest f_<address>" (tools/dev/profnames.py adds their names from the function list). Each sample is
 * a signal the thread answers with its own backtrace (plat_thread_backtrace). */
#define HP_SLOTS 32768
#define HP_DEPTH 160
typedef struct { uint64_t key; unsigned self, total; } HpEnt;
typedef struct { uint32_t tid; unsigned samples, used, waits, wused; HpEnt *t, *w; char name[80]; double cpu_ms; } HpThread;
static int hp_cmp_self(const void *a, const void *b) { const HpEnt *x = a, *y = b; return x->self < y->self ? 1 : x->self > y->self ? -1 : 0; }
static int hp_cmp_total(const void *a, const void *b) { const HpEnt *x = a, *y = b; return x->total < y->total ? 1 : x->total > y->total ? -1 : 0; }
static uint64_t hp_func(uint64_t pc) {             /* the start of the function holding pc (pc itself when not known) */
    if (wd_in_game(pc)) {
        uint64_t best = 0;
        for (int i = 0; i < fn_table_count; i++) { uint64_t f = (uint64_t)(uintptr_t)fn_table[i].fn; if (f <= pc && f > best) best = f; }
        return best && pc - best < 0x400000 ? best : pc;
    }
    Dl_info di;
    return dladdr((void *)(uintptr_t)pc, &di) && di.dli_saddr ? (uint64_t)(uintptr_t)di.dli_saddr : pc;
}
static int hp_in_libc(uint64_t pc) {                /* a thread blocked in a system call samples inside libc */
    Dl_info di;
    return dladdr((void *)(uintptr_t)pc, &di) && di.dli_fname && strstr(di.dli_fname, "libc.so");
}
static void hp_window(double secs, int want) {
    uint32_t me = plat_tid();
    static struct { uint32_t tid; uint64_t c0, c1; } th[512];
    int nt = 0;
    for (int pass = 0; pass < 2; pass++) {         /* CPU time of every thread, half a second apart */
        static uint32_t tids[512];
        int nn = wd_tasks(tids, 512);
        for (int k = 0; k < nn; k++) {
            if (tids[k] == me) continue;
            int i;
            for (i = 0; i < nt && th[i].tid != tids[k]; i++) {}
            if (i == nt) { if (pass || nt == 512) continue; th[nt].tid = tids[k]; th[nt].c0 = th[nt].c1 = 0; nt++; }
            uint64_t cpu = plat_thread_cpu_ns((pid_t)tids[k]);
            if (pass) th[i].c1 = cpu; else th[i].c0 = cpu;
        }
        if (!pass) plat_sleep_ms(500);
    }
    static HpThread hp[8];
    int n = 0;
    memset(hp, 0, sizeof hp);
    for (int k = 0; k < want; k++) {
        int best = -1;
        for (int i = 0; i < nt; i++) if (th[i].c1 > th[i].c0 && (best < 0 || th[i].c1 - th[i].c0 > th[best].c1 - th[best].c0)) best = i;
        if (best < 0) break;
        if ((hp[n].t = calloc(HP_SLOTS, sizeof(HpEnt))) != NULL && (hp[n].w = calloc(HP_SLOTS, sizeof(HpEnt))) != NULL) {
            char tb[80];
            hp[n].tid = th[best].tid;
            hp[n].cpu_ms = (double)(th[best].c1 - th[best].c0) / 1e6;
            snprintf(hp[n].name, sizeof hp[n].name, "%s", wd_tname(hp[n].tid, tb, sizeof tb));
            n++;
        }
        th[best].c1 = th[best].c0;
    }
    plat_counter fq, t0, now;
    plat_qpf(&fq);
    plat_qpc(&t0);
    printf("hostprof: sampling %d threads for %.0f s\n", n, secs);
    fflush(stdout);
    static uint64_t pcs[HP_DEPTH], seen[HP_DEPTH];
    static uintptr_t raw[HP_DEPTH];
    /* a sample whose pc is in libc is a thread blocked in a system call: it is also counted under the innermost function
     * of the lifted game on its stack ("waits under"), to see what the game was waiting for */
    for (;;) {
        plat_qpc(&now);
        if ((double)(now.QuadPart - t0.QuadPart) / (double)fq.QuadPart >= secs || g_wd_quit) break;
        for (int i = 0; i < n; i++) {
            int np = 0, ns = 0;
            np = plat_thread_backtrace((pid_t)hp[i].tid, raw, HP_DEPTH);
            for (int f = 0; f < np; f++) pcs[f] = raw[f];
            if (!np) continue;
            hp[i].samples++;
            if (hp_in_libc(pcs[0])) {
                uint64_t key = 0;
                for (int f = 1; f < np && !key; f++) if (wd_in_game(pcs[f])) key = hp_func(pcs[f]);
                if (!key) key = 1;                 /* no game function on the stack (a host thread's own wait) */
                unsigned h = (unsigned)((key >> 4) * 2654435761u) % HP_SLOTS;
                while (hp[i].w[h].key && hp[i].w[h].key != key) h = (h + 1) % HP_SLOTS;
                if (hp[i].w[h].key || hp[i].wused < HP_SLOTS * 3 / 4) {
                    if (!hp[i].w[h].key) { hp[i].w[h].key = key; hp[i].wused++; }
                    hp[i].w[h].self++;
                    hp[i].waits++;
                }
            }
            for (int f = 0; f < np; f++) {
                uint64_t key = hp_func(pcs[f]);
                int dup = 0;
                for (int s = 0; s < ns && !dup; s++) dup = seen[s] == key;
                if (dup) continue;
                seen[ns++] = key;
                unsigned h = (unsigned)((key >> 4) * 2654435761u) % HP_SLOTS;
                while (hp[i].t[h].key && hp[i].t[h].key != key) h = (h + 1) % HP_SLOTS;
                if (!hp[i].t[h].key) { if (hp[i].used >= HP_SLOTS * 3 / 4) continue; hp[i].t[h].key = key; hp[i].used++; }
                hp[i].t[h].total++;
                if (f == 0) hp[i].t[h].self++;
            }
        }
        plat_sleep_ns(1000000);
    }
    for (int i = 0; i < n; i++) {
        HpEnt *e = calloc(hp[i].used ? hp[i].used : 1, sizeof *e);
        int m = 0;
        if (!e) break;
        for (int s = 0; s < HP_SLOTS; s++) if (hp[i].t[s].key) e[m++] = hp[i].t[s];
        printf("hostprof: thread %lu (%s): %.0f ms of CPU in the half second before sampling; %u samples\n", (unsigned long)hp[i].tid, hp[i].name, hp[i].cpu_ms, hp[i].samples);
        for (int pass = 0; pass < 2 && hp[i].samples; pass++) {
            qsort(e, (size_t)m, sizeof *e, pass ? hp_cmp_total : hp_cmp_self);
            printf("hostprof:   %s\n", pass ? "most often anywhere on the stack (total %, self %):" : "most often running (self %, total %):");
            for (int k = 0; k < m && k < (pass ? 40 : 30); k++) {
                char ab[300];
                wd_addr(e[k].key, ab, sizeof ab);
                double sf = 100.0 * e[k].self / hp[i].samples, tt = 100.0 * e[k].total / hp[i].samples;
                printf("hostprof:   %5.1f %5.1f  %s\n", pass ? tt : sf, pass ? sf : tt, ab);
            }
        }
        if (hp[i].waits) {
            int mw = 0;
            HpEnt *we = calloc(hp[i].wused ? hp[i].wused : 1, sizeof *we);
            if (we) {
                for (int s = 0; s < HP_SLOTS; s++) if (hp[i].w[s].key) we[mw++] = hp[i].w[s];
                qsort(we, (size_t)mw, sizeof *we, hp_cmp_self);
                printf("hostprof:   blocked in a system call %.1f%% of the samples; by the innermost game function on the stack (%% of all samples):\n", 100.0 * hp[i].waits / hp[i].samples);
                for (int k = 0; k < mw && k < 12; k++) {
                    char ab[300];
                    if (we[k].key == 1) snprintf(ab, sizeof ab, "(no game function on the stack)"); else wd_addr(we[k].key, ab, sizeof ab);
                    printf("hostprof:   %5.1f        %s\n", 100.0 * we[k].self / hp[i].samples, ab);
                }
                free(we);
            }
        }
        free(e);
        free(hp[i].t);
        free(hp[i].w);
    }
    fflush(stdout);
}
static uint32_t hp_main(void *arg) {           /* SMO_HOSTPROF=<start>,<seconds>[,<threads>][;<start>,<seconds>[,<threads>] ...] */
    char spec[256];
    plat_counter fq, t0, now;
    (void)arg;
    snprintf(spec, sizeof spec, "%s", getenv("SMO_HOSTPROF"));
    plat_qpf(&fq);
    plat_qpc(&t0);
    for (char *w = spec; w && *w && !g_wd_quit; w = strchr(w, ';') ? strchr(w, ';') + 1 : NULL) {
        double start = 0, secs = 0;
        int want = 4;
        if (sscanf(w, "%lf,%lf,%d", &start, &secs, &want) < 2 || secs <= 0) continue;
        if (want < 1) want = 1;
        if (want > 8) want = 8;
        plat_qpc(&now);
        double el = (double)(now.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
        if (start > el) plat_sleep_ms((uint32_t)((start - el) * 1000));
        if (g_wd_quit) break;
        printf("hostprof: window from %.0f s after the renderer started, frame %lld\n", start, nvn_front_frame_next());
        hp_window(secs, want);
    }
    return 0;
}
static void hp_start(void) {
    static int done;
    if (done || !getenv("SMO_HOSTPROF")) return;
    done = 1;
    plat_thread_detach(plat_thread_start(hp_main, NULL, 0));
}
static void wd_start(void) {
    static double limit = 5.0;
    hp_start();
    const char *e = getenv("SMO_VK_WATCHDOG");
    if (e) limit = atof(e);
    if (limit <= 0) { printf("nvn_vk_watchdog: off (SMO_VK_WATCHDOG=0)\n"); return; }
    if (limit < 1) limit = 1;
    g_wd_thread = plat_thread_start(wd_main, &limit, 0);
    printf("nvn_vk_watchdog: on: a report of every thread (locks, waits, call stacks) is written to the log and to <exe>_freeze.txt when no frame reaches the window for %.0f s (SMO_VK_WATCHDOG=<seconds>, 0 = off)\n", limit);
}
static void wd_stop(void) {
    g_wd_quit = 1;
    if (g_wd_thread) { if (!plat_thread_join(g_wd_thread, 3000)) plat_thread_detach(g_wd_thread); g_wd_thread = NULL; }
}

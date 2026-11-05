/* log.c - a level on every line of the log: [INFO], [WARN] or [ERROR] (SMO_LOG_TAGS=1: the game folder's ./smo sets it).
 *
 * stdout and stderr are replaced by streams of our own (glibc fopencookie): everything printed to them (printf,
 * fprintf, puts, fwrite) reaches line_write, which keeps a line until its end, then writes it to the original
 * descriptor with its level in front. One place for the whole runtime and the lifted game's own logging: no call site
 * changes. What the level is comes from the line itself (level_of): the runtime's messages say what went wrong in their
 * words (an abort, a crash report, a FAIL; NOT, cannot, could not, refused, not found ...). A line on stderr is at
 * least a warning. Writes that go round stdio (write(1, ...) of a signal handler, a child process's output) are not
 * tagged. Off (the default for builds run from the repository: their tests read the log's lines as they always were),
 * nothing changes. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

typedef struct { int fd, min_level; char buf[8192]; size_t n; } LogStream;
static LogStream g_out = {1, 0}, g_err = {2, 1};

/* a clause of a counters line that says "none": it starts with 0 ("0 refused", "0 not presented (... a failed call)") or ends
 * with one ("failed Vulkan calls 0"). Clauses are what lies between , ; and : (parentheses belong to the clause around them). */
static int zero_clause(const char *s, const char *at) {
    const char *b = at, *e = at;
    while (b > s && b[-1] != ',' && b[-1] != ';' && b[-1] != ':') b--;
    while (*e && *e != ',' && *e != ';') e++;
    while (*b == ' ') b++;
    while (e > b && (e[-1] == ' ' || e[-1] == ')' || e[-1] == '.')) e--;
    if (*b == '0' && (b[1] == ' ' || b[1] == '.')) return 1;
    for (const char *q = at; q > b; q--) if (q[-1] == '(') { if (q[0] == '0' && q[1] == ' ') return 1; break; }      /* "(0 failed, ..." */
    return e > b && e[-1] == '0' && (e - 1 == b || e[-2] == ' ');
}
/* the first place a word appears that is not in a "none" clause, or NULL */
static const char *real(const char *s, const char *w) {
    for (const char *p = strstr(s, w); p; p = strstr(p + 1, w)) if (!zero_clause(s, p)) return p;
    return NULL;
}
/* 0 INFO, 1 WARN, 2 ERROR */
static int level_of(const char *s) {
    static const char *const err[] = {"nn::diag abort", "assertion failed", "CRASH", "crash report", "host exception", "Segmentation fault",
                                      "C++ RUNTIME", "FATAL", "fatal error", "FAILED", "  FAIL ", "out of memory", "could not start", "SDL_Init failed"};
    static const char *const warn[] = {"NOT ", "NOT:", "WARNING", "cannot ", "could not", "refused", "not found", "SKIPPED", "on battery",
                                       "river updated", "not translated", "not usable", "is not available", "not implemented", "failed"};
    for (size_t i = 0; i < sizeof err / sizeof *err; i++) if (real(s, err[i])) return 2;
    for (size_t i = 0; i < sizeof warn / sizeof *warn; i++) if (real(s, warn[i])) return 1;
    return 0;
}
static void put_all(int fd, const char *p, size_t n) {
    while (n) { ssize_t w = write(fd, p, n); if (w <= 0) return; p += w; n -= (size_t)w; }
}
static void emit(LogStream *ls, const char *line, size_t n) {      /* one whole line (without its '\n') */
    static const char *const tag[3] = {"[INFO] ", "[WARN] ", "[ERROR] "};
    char tmp[8192 + 16];
    int lv;
    if (n >= sizeof ls->buf) n = sizeof ls->buf - 1;
    memcpy(tmp + 8, line, n); tmp[8 + n] = 0;
    lv = level_of(tmp + 8);
    if (lv < ls->min_level) lv = ls->min_level;
    if (n == 0) { put_all(ls->fd, "\n", 1); return; }   /* an empty line stays empty */
    size_t tl = strlen(tag[lv]);
    char *start = tmp + 8 - tl;
    memcpy(start, tag[lv], tl);
    tmp[8 + n] = '\n';
    put_all(ls->fd, start, tl + n + 1);
}
static ssize_t line_write(void *cookie, const char *buf, size_t size) {
    LogStream *ls = cookie;
    for (size_t i = 0; i < size; i++) {
        char c = buf[i];
        if (c == '\n' || ls->n == sizeof ls->buf - 1) { emit(ls, ls->buf, ls->n); ls->n = 0; if (c == '\n') continue; }
        if (c == '\r') continue;                          /* (a progress line rewritten in place: kept as one line) */
        ls->buf[ls->n++] = c;
    }
    return (ssize_t)size;
}
static FILE *tagged(LogStream *ls) {
    cookie_io_functions_t io = {NULL, line_write, NULL, NULL};
    int fd = dup(ls->fd);                                 /* the original terminal / file: writes keep going there */
    if (fd < 0) return NULL;
    ls->fd = fd;
    FILE *f = fopencookie(ls, "w", io);
    if (f) setvbuf(f, NULL, _IONBF, 0);                   /* every printf reaches line_write at once (whole lines go out at their '\n') */
    return f;
}
void smo_log_init(void) {                                 /* boot.c, first thing */
    const char *e = getenv("SMO_LOG_TAGS");
    if (!e || !atoi(e)) return;
    FILE *o = tagged(&g_out), *r = tagged(&g_err);
    if (o) stdout = o;
    if (r) stderr = r;
}

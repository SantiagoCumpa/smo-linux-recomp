/* perf_stats.c - what the detailed performance overlay shows (nvn_vk_draw.c fps_overlay_draw, performance_overlay =
 * detailed): CPU load and temperature, GPU load, temperature and video memory, RAM. Sampled at most twice a second on
 * the render thread; every source is optional and a missing one shows as -1.
 *
 *  CPU load        /proc/stat, the "cpu" line: busy share of all CPUs since the last sample; the game's own share from
 *                  /proc/self/stat (utime + stime) over the same time and the CPU count
 *  CPU temperature /sys/class/hwmon/hwmon*: the coretemp (Intel) "Package id 0" input, k10temp / zenpower (AMD) Tctl/Tdie,
 *                  else the first temp1_input of those drivers
 *  RAM             /proc/meminfo MemTotal - MemAvailable (the whole PC) and /proc/self/status VmRSS (the game)
 *  GPU (NVIDIA)    NVML, libnvidia-ml.so.1 (the driver's own library, opened with dlopen; not linked): utilisation,
 *                  temperature, memory used / total of the first device whose name matches the Vulkan device's
 *  GPU (AMD)       /sys/class/drm/card<n>/device: gpu_busy_percent, mem_info_vram_used / _total, hwmon temp1_input
 *  power supply    /sys/class/power_supply/<n>/type and online: on battery when a Battery is there and no Mains or USB
 *                  supply is online (perf_on_battery); the GPU's power limit from NVML (nvmlDeviceGetEnforcedPowerLimit)
 * Nothing is written anywhere. */
#include <dirent.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "perf_stats.h"

static int read_line(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = 0;
    return ok;
}
static long long read_ll(const char *path) { char b[64]; return read_line(path, b, sizeof b) ? atoll(b) : -1; }

/* ---- CPU */
static int cpu_load(int *game_pct) {
    static unsigned long long last_busy, last_total, last_self;
    static int have;
    unsigned long long v[10] = {0}, busy, total, self = 0;
    char line[512];
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    int ok = fgets(line, sizeof line, f) && sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) >= 4;
    fclose(f);
    if (!ok) return -1;
    total = 0; for (int i = 0; i < 8; i++) total += v[i];       /* guest times (8, 9) are inside user / nice already */
    busy = total - v[3] - v[4];                                  /* idle and iowait */
    if ((f = fopen("/proc/self/stat", "r"))) {
        if (fgets(line, sizeof line, f)) {
            const char *p = strrchr(line, ')');                  /* the name may hold spaces: fields after its ')' */
            unsigned long long ut = 0, st = 0;
            if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2) self = ut + st;
        }
        fclose(f);
    }
    int pct = -1;
    *game_pct = -1;
    if (have && total > last_total) {
        pct = (int)((busy - last_busy) * 100 / (total - last_total));
        /* the process's ticks are of one CPU; /proc/stat's total counts every CPU */
        *game_pct = (int)((self - last_self) * 100 / (total - last_total));
    }
    last_busy = busy; last_total = total; last_self = self; have = 1;
    return pct;
}
static char g_cpu_temp_path[300];
static int cpu_temp(void) {
    static int searched;
    if (!searched) {
        searched = 1;
        DIR *d = opendir("/sys/class/hwmon");
        struct dirent *e;
        while (d && (e = readdir(d)) && !g_cpu_temp_path[0]) {
            char p[300], name[64];
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
            if (!read_line(p, name, sizeof name)) continue;
            if (strcmp(name, "coretemp") && strcmp(name, "k10temp") && strcmp(name, "zenpower")) continue;
            for (int i = 1; i <= 16 && !g_cpu_temp_path[0]; i++) {          /* the package / Tctl sensor by its label */
                char lab[64];
                snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp%d_label", e->d_name, i);
                if (read_line(p, lab, sizeof lab) && (!strncmp(lab, "Package", 7) || !strcmp(lab, "Tctl") || !strcmp(lab, "Tdie")))
                    snprintf(g_cpu_temp_path, sizeof g_cpu_temp_path, "/sys/class/hwmon/%s/temp%d_input", e->d_name, i);
            }
            if (!g_cpu_temp_path[0]) snprintf(g_cpu_temp_path, sizeof g_cpu_temp_path, "/sys/class/hwmon/%s/temp1_input", e->d_name);
        }
        if (d) closedir(d);
    }
    if (!g_cpu_temp_path[0]) return -1;
    long long m = read_ll(g_cpu_temp_path);
    return m < 0 ? -1 : (int)(m / 1000);
}

/* ---- RAM */
static void ram(int *used_mb, int *total_mb, int *game_mb) {
    char line[256];
    long long tot = -1, avail = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "MemTotal:", 9)) tot = atoll(line + 9);
        else if (!strncmp(line, "MemAvailable:", 13)) avail = atoll(line + 13);
    }
    if (f) fclose(f);
    *total_mb = tot > 0 ? (int)(tot / 1024) : -1;
    *used_mb = tot > 0 && avail >= 0 ? (int)((tot - avail) / 1024) : -1;
    *game_mb = -1;
    if ((f = fopen("/proc/self/status", "r"))) {
        while (fgets(line, sizeof line, f)) if (!strncmp(line, "VmRSS:", 6)) *game_mb = (int)(atoll(line + 6) / 1024);
        fclose(f);
    }
}

/* ---- GPU: NVML (NVIDIA) */
typedef struct { unsigned gpu, memory; } NvmlUtil;
typedef struct { unsigned long long total, free, used; } NvmlMem;
static struct {
    int tried, ok;
    void *dev;
    int (*util)(void *, NvmlUtil *);
    int (*temp)(void *, int, unsigned *);
    int (*mem)(void *, NvmlMem *);
    int (*plimit)(void *, unsigned *);             /* milliwatts */
} N;
static void nvml_open(const char *vk_name) {
    N.tried = 1;
    void *h = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) return;
    int (*init)(void) = (int (*)(void))dlsym(h, "nvmlInit_v2");
    int (*count)(unsigned *) = (int (*)(unsigned *))dlsym(h, "nvmlDeviceGetCount_v2");
    int (*byidx)(unsigned, void **) = (int (*)(unsigned, void **))dlsym(h, "nvmlDeviceGetHandleByIndex_v2");
    int (*name)(void *, char *, unsigned) = (int (*)(void *, char *, unsigned))dlsym(h, "nvmlDeviceGetName");
    N.util = (int (*)(void *, NvmlUtil *))dlsym(h, "nvmlDeviceGetUtilizationRates");
    N.temp = (int (*)(void *, int, unsigned *))dlsym(h, "nvmlDeviceGetTemperature");
    N.mem = (int (*)(void *, NvmlMem *))dlsym(h, "nvmlDeviceGetMemoryInfo");
    N.plimit = (int (*)(void *, unsigned *))dlsym(h, "nvmlDeviceGetEnforcedPowerLimit");
    unsigned n = 0;
    if (!init || !count || !byidx || !N.util || init() != 0 || count(&n) != 0 || !n) return;
    for (unsigned i = 0; i < n && !N.dev; i++) {          /* the device Vulkan uses, by name; else the first */
        void *d = NULL; char nm[128] = "";
        if (byidx(i, &d) != 0) continue;
        if (name) name(d, nm, sizeof nm);
        if (!vk_name || !vk_name[0] || (nm[0] && (strstr(vk_name, nm) || strstr(nm, vk_name)))) N.dev = d;
    }
    if (!N.dev) byidx(0, &N.dev);
    N.ok = N.dev != NULL;
    printf("perf_stats: GPU figures from NVML%s\n", N.ok ? "" : " not available");
}

/* ---- GPU: amdgpu sysfs */
static char g_amd_dev[200], g_amd_temp[300];
static int amd_find(void) {
    static int searched;
    if (searched) return g_amd_dev[0] != 0;
    searched = 1;
    for (int c = 0; c < 8 && !g_amd_dev[0]; c++) {
        char p[200];
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/gpu_busy_percent", c);
        if (access(p, R_OK) == 0) snprintf(g_amd_dev, sizeof g_amd_dev, "/sys/class/drm/card%d/device", c);
    }
    if (g_amd_dev[0]) {
        char p[260];
        snprintf(p, sizeof p, "%s/hwmon", g_amd_dev);
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d))) if (!strncmp(e->d_name, "hwmon", 5)) snprintf(g_amd_temp, sizeof g_amd_temp, "%s/%s/temp1_input", p, e->d_name);
        if (d) closedir(d);
    }
    return g_amd_dev[0] != 0;
}

int perf_on_battery(void) {
    const char *root = getenv("SMO_POWER_SUPPLY");        /* TEST ONLY: a folder laid out as /sys/class/power_supply */
    if (!root || !root[0]) root = "/sys/class/power_supply";
    DIR *d = opendir(root);
    struct dirent *e;
    int battery = 0, supply_known = 0, online = 0;
    char p[300], v[64];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s/type", root, e->d_name);
        if (!read_line(p, v, sizeof v)) continue;
        if (!strcmp(v, "Battery")) { battery = 1; continue; }
        if (strcmp(v, "Mains") && strncmp(v, "USB", 3)) continue;
        snprintf(p, sizeof p, "%s/%s/online", root, e->d_name);
        long long on = read_ll(p);
        if (on >= 0) supply_known = 1;
        if (on > 0) online = 1;
    }
    if (d) closedir(d);
    if (online) return 0;
    return battery && supply_known ? 1 : -1;
}

void perf_stats_sample(PerfStats *s, const char *gpu_name) {
    memset(s, 0xff, sizeof *s);                            /* every figure -1 until read */
    s->on_battery = perf_on_battery();
    s->cpu_load = cpu_load(&s->game_cpu);
    s->cpu_temp = cpu_temp();
    ram(&s->ram_used_mb, &s->ram_total_mb, &s->game_ram_mb);
    int nvidia = gpu_name && (strstr(gpu_name, "NVIDIA") || strstr(gpu_name, "GeForce") || strstr(gpu_name, "RTX") || strstr(gpu_name, "Quadro"));
    if (nvidia) {
        if (!N.tried) nvml_open(gpu_name);
        if (N.ok) {
            NvmlUtil u; NvmlMem m; unsigned t;
            if (N.util(N.dev, &u) == 0) s->gpu_load = (int)u.gpu;
            if (N.temp && N.temp(N.dev, 0, &t) == 0) s->gpu_temp = (int)t;
            if (N.mem && N.mem(N.dev, &m) == 0) { s->vram_used_mb = (int)(m.used >> 20); s->vram_total_mb = (int)(m.total >> 20); }
            unsigned mw;
            if (N.plimit && N.plimit(N.dev, &mw) == 0) s->gpu_power_limit_w = (int)((mw + 500) / 1000);
        }
    } else if (amd_find()) {
        char p[260];
        snprintf(p, sizeof p, "%s/gpu_busy_percent", g_amd_dev); s->gpu_load = (int)read_ll(p);
        snprintf(p, sizeof p, "%s/mem_info_vram_used", g_amd_dev); long long u = read_ll(p);
        snprintf(p, sizeof p, "%s/mem_info_vram_total", g_amd_dev); long long t = read_ll(p);
        if (u >= 0) s->vram_used_mb = (int)(u >> 20);
        if (t >= 0) s->vram_total_mb = (int)(t >> 20);
        if (g_amd_temp[0]) { long long m = read_ll(g_amd_temp); s->gpu_temp = m < 0 ? -1 : (int)(m / 1000); }
    }
}

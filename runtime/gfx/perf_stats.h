/* perf_stats.h - the figures of the detailed performance overlay (perf_stats.c). -1 = not known on this PC. */
#ifndef PERF_STATS_H
#define PERF_STATS_H
typedef struct {
    int cpu_load, game_cpu, cpu_temp;          /* % of all CPUs (the PC, the game), degrees C */
    int gpu_load, gpu_temp;                    /* %, degrees C */
    int vram_used_mb, vram_total_mb;
    int ram_used_mb, ram_total_mb, game_ram_mb;
    int on_battery;                            /* 1 no charger (perf_on_battery), 0 charger, -1 not known */
    int gpu_power_limit_w;                     /* the power the GPU is held to now (NVML enforced limit), watts */
} PerfStats;
/* The figures now (the loads: since the previous call). gpu_name: the Vulkan device's name, which picks the source. */
void perf_stats_sample(PerfStats *s, const char *gpu_name);
/* 1: the PC runs on its battery (a battery, and no mains or USB-C supply online: a laptop then holds its GPU and CPU to
 * less power), 0: a charger or mains supply is online, -1: no power supply information (a desktop, or no /sys). */
int perf_on_battery(void);
#endif

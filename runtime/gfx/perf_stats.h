/* perf_stats.h - the figures of the detailed performance overlay (perf_stats.c). -1 = not known on this PC. */
#ifndef PERF_STATS_H
#define PERF_STATS_H
typedef struct {
    int cpu_load, game_cpu, cpu_temp;          /* % of all CPUs (the PC, the game), degrees C */
    int gpu_load, gpu_temp;                    /* %, degrees C */
    int vram_used_mb, vram_total_mb;
    int ram_used_mb, ram_total_mb, game_ram_mb;
} PerfStats;
/* The figures now (the loads: since the previous call). gpu_name: the Vulkan device's name, which picks the source. */
void perf_stats_sample(PerfStats *s, const char *gpu_name);
#endif

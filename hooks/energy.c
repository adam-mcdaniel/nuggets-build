/*
 * Instrumentation hook: per-region wall time and GPU energy.
 *
 * Built with the same phase-analysis-pass and interval as the analysis
 * binary, so regions end at the same instruction counts. Instead of
 * basic-block vectors, each region boundary records:
 *
 *     region_inst,r,0,N      IR instructions in region r
 *     wall_ns,r,0,NS         wall time of region r
 *     gpu_energy_uj,r,0,...  energy of each GPU during region r (ROCm SMI), uJ
 *     gpu_counter_uj,r,0,... raw energy counter of each GPU at the end of region r, uJ
 *     gpu_counter_ts,r,0,... ROCm SMI timestamp of that counter value (ns)
 *     t_end_ns,r,0,NS        CLOCK_MONOTONIC at the end of region r
 *
 * Each boundary reads the clock and the counters once, and that reading is both
 * the end of region r and the start of region r+1, so the regions tile the ROI
 * with no gaps (the hook's own overhead is charged to the next region). An
 * earlier version read the counters twice per boundary and left the time
 * between the reads (and any counter step in it) out of every region.
 *
 * Rows go to energy-output.csv ($NUGGET_ENERGY_OUTPUT) as each region ends.
 * The region length is the interval compiled in by phase-analysis-pass; build
 * it with the same interval as the analysis binary. The GPU energy counter
 * updates about every 8 ms on
 * MI210/MI250X, so regions should be much longer than that.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int (*rsmi_init_fn)(uint64_t);
typedef int (*rsmi_num_fn)(uint32_t *);
typedef int (*rsmi_energy_fn)(uint32_t, uint64_t *, float *, uint64_t *);
#define MAX_GPUS 16

static FILE *out;
static int active, in_hook;
static uint64_t inst, total_inst, region, t_start;
static rsmi_energy_fn rsmi_energy;
static uint32_t n_gpus;
static uint64_t e_start[MAX_GPUS], e_now[MAX_GPUS], ts_now[MAX_GPUS], t_roi;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t energy_uj(uint32_t g, uint64_t *ts_out) {
    uint64_t count = 0, ts = 0;
    float res = 0;
    if (rsmi_energy(g, &count, &res, &ts) != 0) return 0;
    if (ts_out) *ts_out = ts;
    return (uint64_t)((double)count * res);
}

/* One reading per boundary: the clock, then every GPU's counter. */
static uint64_t read_boundary(void) {
    uint64_t t = now_ns();
    for (uint32_t g = 0; g < n_gpus; g++) e_now[g] = energy_uj(g, &ts_now[g]);
    return t;
}

static void energy_init(void) {
    void *h = dlopen("librocm_smi64.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("librocm_smi64.so.7", RTLD_NOW | RTLD_GLOBAL);
    rsmi_init_fn init = h ? (rsmi_init_fn)dlsym(h, "rsmi_init") : NULL;
    rsmi_num_fn num = h ? (rsmi_num_fn)dlsym(h, "rsmi_num_monitor_devices") : NULL;
    rsmi_energy = h ? (rsmi_energy_fn)dlsym(h, "rsmi_dev_energy_count_get") : NULL;
    if (!(init && num && rsmi_energy && init(0) == 0 && num(&n_gpus) == 0)) n_gpus = 0;
    if (n_gpus > MAX_GPUS) n_gpus = MAX_GPUS;
    printf("GPU energy: %u device(s)%s\n", n_gpus, n_gpus ? "" : " (ROCm SMI not available)");
}

static void start_region(uint64_t t) {
    for (uint32_t g = 0; g < n_gpus; g++) e_start[g] = e_now[g];
    t_start = t;
}

static void end_region(void) {
    uint64_t t = read_boundary();
    fprintf(out, "region_inst,%llu,0,%llu\n", (unsigned long long)region, (unsigned long long)inst);
    fprintf(out, "wall_ns,%llu,0,%llu\n", (unsigned long long)region, (unsigned long long)(t - t_start));
    fprintf(out, "t_end_ns,%llu,0,%llu\n", (unsigned long long)region, (unsigned long long)(t - t_roi));
    if (n_gpus) {
        fprintf(out, "gpu_energy_uj,%llu,0", (unsigned long long)region);
        for (uint32_t g = 0; g < n_gpus; g++) fprintf(out, ",%llu", (unsigned long long)(e_now[g] - e_start[g]));
        fprintf(out, "\ngpu_counter_uj,%llu,0", (unsigned long long)region);
        for (uint32_t g = 0; g < n_gpus; g++) fprintf(out, ",%llu", (unsigned long long)e_now[g]);
        fprintf(out, "\ngpu_counter_ts,%llu,0", (unsigned long long)region);
        for (uint32_t g = 0; g < n_gpus; g++) fprintf(out, ",%llu", (unsigned long long)ts_now[g]);
        fputc('\n', out);
    }
    total_inst += inst;
    inst = 0;
    region++;
    start_region(t);
}

/* Inserted by phase-analysis-pass at the end of nugget_roi_begin_. */
void nugget_init(uint64_t total_bb_count) {
    (void)total_bb_count;
    t_roi = read_boundary();
    start_region(t_roi);
    active = 1;
}

void nugget_roi_begin_(void) {
    const char *path = getenv("NUGGET_ENERGY_OUTPUT");
    out = fopen(path && *path ? path : "energy-output.csv", "w");
    if (!out) {
        perror("energy-output.csv");
        exit(1);
    }
    fprintf(out, "type,region,thread,data\n");
    energy_init();
    printf("ROI begin\n");
}

void nugget_roi_end_(void) {
    if (!active) return;
    active = 0;
    end_region();
    fclose(out);
    printf("ROI end: %llu regions, %llu IR instructions, %.6f s\n", (unsigned long long)region,
           (unsigned long long)total_inst, (now_ns() - t_roi) * 1e-9);
}

void nugget_bb_hook(uint64_t inst_count, uint64_t bb_id, uint64_t threshold) {
    (void)bb_id;
    if (!active || in_hook) return;
    in_hook = 1;
    inst += inst_count;
    if (inst > threshold) end_region();
    in_hook = 0;
}

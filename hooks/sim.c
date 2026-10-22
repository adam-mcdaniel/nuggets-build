/*
 * Simulator hook: region boundaries on stdout, for GPU simulators that run
 * the host natively (GPGPU-Sim). Built with the same phase-analysis-pass and
 * interval as the analysis binary, so regions match the analysis run.
 *
 * At each boundary it prints, on the same stdout the simulator logs to,
 *     [nugget] region <r> begin
 * so every kernel launch the simulator logs afterwards belongs to region r.
 *
 * NUGGET_REGIONS=r1,r2,... : after the last listed region ends, exit (the
 * rest of the run is not needed, e.g. when resuming from a checkpoint just
 * before a representative region).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_REGIONS 4096

static int active, in_hook, n_wanted, n_done;
static uint64_t inst, region, total_inst;
static uint64_t wanted[MAX_REGIONS];

static int is_wanted(uint64_t r) {
    for (int i = 0; i < n_wanted; i++)
        if (wanted[i] == r) return 1;
    return 0;
}

static void begin_region(uint64_t r) {
    printf("[nugget] region %llu begin\n", (unsigned long long)r);
    fflush(stdout);
}

static void end_region(uint64_t r) {
    printf("[nugget] region %llu end\n", (unsigned long long)r);
    fflush(stdout);
    if (n_wanted && is_wanted(r) && ++n_done == n_wanted) {
        printf("[nugget] last listed region done, exiting\n");
        fflush(NULL);
        _exit(0);
    }
}

void nugget_init(uint64_t total_bb_count) {
    (void)total_bb_count;
    active = 1;
    begin_region(0);
}

void nugget_roi_begin_(void) {
    const char *list = getenv("NUGGET_REGIONS");
    for (const char *p = list ? list : ""; *p && n_wanted < MAX_REGIONS;) {
        char *end;
        unsigned long long r = strtoull(p, &end, 10);
        if (end == p) break;
        wanted[n_wanted++] = r;
        p = *end ? end + 1 : end;
    }
}

void nugget_roi_end_(void) {
    if (!active) return;
    active = 0;
    total_inst += inst;
    end_region(region);
    printf("[nugget] ROI end: %llu regions, %llu IR instructions\n", (unsigned long long)(region + 1),
           (unsigned long long)total_inst);
    fflush(stdout);
}

void nugget_bb_hook(uint64_t inst_count, uint64_t bb_id, uint64_t threshold) {
    (void)bb_id;
    if (!active || in_hook) return;
    in_hook = 1;
    inst += inst_count;
    if (inst > threshold) {
        total_inst += inst;
        inst = 0;
        end_region(region);
        region++;
        begin_region(region);
    }
    in_hook = 0;
}

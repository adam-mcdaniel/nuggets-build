/*
 * Checkpoint hook: one gem5 checkpoint at the start of each representative
 * region, in a single run, without markers.
 *
 * Built with the same phase-analysis-pass and interval as the analysis
 * binary. HIP sources are not instrumented, so the instrumented host code
 * executes the same IR instructions in every run and region r starts at
 * exactly the same point as in the analysis run. The hook counts regions the
 * same way and, when region r is one of $NUGGET_REGIONS, calls m5_checkpoint
 * (gem5 then writes cpt.<tick>/; restore it and simulate region r from there).
 * After the last listed region has ended it calls m5_exit, unless
 * NUGGET_CONTINUE=1.
 *
 *     NUGGET_REGIONS=0,42,381 gem5 ... app.checkpoint <args>
 *
 * Every event is also logged to checkpoint-log.csv ($NUGGET_CHECKPOINT_LOG):
 *     event,region,ir_instructions_before,ns_since_roi
 * so checkpoints (taken in order) map to regions. Built with -DNUGGET_NO_M5
 * the m5 calls are only logged, to check the binary on real hardware.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef NUGGET_NO_M5
#include <gem5/m5ops.h>
#define M5(call) call
#else
#define M5(call) ((void)0)
#endif

#define MAX_REGIONS 4096

static FILE *log_file;
static int active, in_hook, keep_going;
static uint64_t inst, total_inst, region, t0;
static uint64_t wanted[MAX_REGIONS];
static int n_wanted, n_done;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int is_wanted(uint64_t r) {
    for (int i = 0; i < n_wanted; i++)
        if (wanted[i] == r) return 1;
    return 0;
}

static void event(const char *what, uint64_t r) {
    fprintf(log_file, "%s,%llu,%llu,%llu\n", what, (unsigned long long)r, (unsigned long long)total_inst,
            (unsigned long long)(now_ns() - t0));
    fflush(log_file);
}

/* Region r begins: checkpoint it if it is a representative. */
static void begin_region(uint64_t r) {
    if (is_wanted(r)) {
        event("checkpoint", r);
        M5(m5_checkpoint(0, 0));
    }
}

/* Region r ended. */
static void end_region(uint64_t r) {
    if (is_wanted(r)) {
        event("end", r);
        if (++n_done == n_wanted && !keep_going) {
            event("exit", r);
            M5(m5_exit(0));
        }
    }
}

/* Inserted by phase-analysis-pass at the end of nugget_roi_begin_. */
void nugget_init(uint64_t total_bb_count) {
    (void)total_bb_count;
    active = 1;
    begin_region(0);
}

void nugget_roi_begin_(void) {
    const char *path = getenv("NUGGET_CHECKPOINT_LOG");
    log_file = fopen(path && *path ? path : "checkpoint-log.csv", "w");
    if (!log_file) {
        perror("checkpoint-log.csv");
        exit(1);
    }
    fprintf(log_file, "event,region,ir_instructions_before,ns_since_roi\n");
    const char *list = getenv("NUGGET_REGIONS");
    for (const char *p = list ? list : ""; *p && n_wanted < MAX_REGIONS;) {
        char *end;
        unsigned long long r = strtoull(p, &end, 10);
        if (end == p) break;
        wanted[n_wanted++] = r;
        p = *end ? end + 1 : end;
    }
    const char *k = getenv("NUGGET_CONTINUE");
    keep_going = k && !strcmp(k, "1");
    t0 = now_ns();
    printf("ROI begin: checkpointing %d region(s)\n", n_wanted);
}

void nugget_roi_end_(void) {
    if (!active) return;
    active = 0;
    end_region(region);
    total_inst += inst;
    event("roi_end", region);
    fclose(log_file);
    printf("ROI end: %llu regions, %llu IR instructions\n", (unsigned long long)(region + 1),
           (unsigned long long)total_inst);
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

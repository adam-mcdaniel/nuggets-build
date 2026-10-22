/*
 * gem5 hook for the instrumentation run: phase-bound-pass calls these at the
 * three marker blocks of one region. Counts are executions of each marker
 * block since nugget_roi_begin_; 0 means "at the ROI boundary" (start 0 = at
 * ROI begin, end 0 = at ROI end).
 *
 *   warmup marker  m5_checkpoint   (restore here; the warm-up runs before the region)
 *   start marker   m5_checkpoint if there was no warm-up marker, then m5_reset_stats
 *   end marker     m5_dump_stats, m5_exit
 *
 * Change the m5 calls below to fit your gem5 flow. Built with -DNUGGET_NO_M5,
 * the hook prints instead, so the binary can be checked on real hardware.
 */
#include <stdint.h>
#include <stdio.h>

#ifndef NUGGET_NO_M5
#include <gem5/m5ops.h>
#define M5(call, name) call
#else
#define M5(call, name) fprintf(stderr, "[nugget-gem5] %s\n", name)
#endif

static uint64_t warmup_target, start_target, end_target;
static uint64_t warmup_seen, start_seen, end_seen;
static int active, started, checkpointed;

static void start_event(void) {
    if (!checkpointed) {
        M5(m5_checkpoint(0, 0), "m5_checkpoint");
        checkpointed = 1;
    }
    M5(m5_reset_stats(0, 0), "m5_reset_stats");
    started = 1;
}

static void end_event(void) {
    M5(m5_dump_stats(0, 0), "m5_dump_stats");
    M5(m5_exit(0), "m5_exit");
    active = 0;
}

/* phase-bound-pass inserts this call at the end of nugget_roi_begin_. */
void nugget_init(uint64_t warmup, uint64_t start, uint64_t end) {
    warmup_target = warmup;
    start_target = start;
    end_target = end;
    active = 1;
    if (start == 0) start_event();
}

void nugget_roi_begin_(void) {}

void nugget_roi_end_(void) {
    if (active && started) end_event(); /* the last region ends with the ROI */
}

void nugget_warmup_marker_hook(void) {
    if (active && ++warmup_seen == warmup_target) {
        M5(m5_checkpoint(0, 0), "m5_checkpoint (warm-up)");
        checkpointed = 1;
    }
}

void nugget_start_marker_hook(void) {
    if (active && ++start_seen == start_target) start_event();
}

void nugget_end_marker_hook(void) {
    if (active && ++end_seen == end_target && started) end_event();
}

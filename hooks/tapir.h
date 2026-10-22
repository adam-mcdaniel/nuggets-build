/*
 * tapir.h: C ABI of tapir (Tracing And Profiling Interposition Runtime).
 *
 * Every tapir-based library exports the tapir_* functions below. Instrumented
 * code should call them through the tapir_try_* helpers, which look the
 * symbols up at runtime with dlsym(RTLD_DEFAULT). The helpers are no-ops
 * (returning -1 or 0) when no tapir library is loaded, so the same binary runs
 * with and without tapir, and nothing extra is needed at link time (apart
 * from -ldl on glibc < 2.34).
 *
 * Online sampling at the caller's own boundaries (e.g. phase or region ends):
 * take a profiler's current interval as numbers (tapir_try_vector) or as its
 * own text (tapir_try_snapshot), or have tapir write it to its own output,
 * aligned by index (tapir_try_interval). Report activity tapir does not wrap
 * with tapir_try_record.
 *
 * Define TAPIR_DIRECT before including this header to also get plain
 * prototypes, for code that links against a tapir library directly.
 */
#ifndef TAPIR_H
#define TAPIR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <dlfcn.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the exported ABI --------------------------------------------------- */

/* 1 if a tapir runtime is initialized in this process. */
typedef int (*tapir_active_fn)(void);
/* Pause (0) / resume (non-zero) recording, e.g. outside a region of interest. */
typedef void (*tapir_enable_fn)(int on);
/* Close the current interval in every profiler; index < 0 means "next".
   Returns the index closed. */
typedef int64_t (*tapir_interval_fn)(int64_t index);
/* Copy the current interval of profiler `name` as text into buf
   (NUL-terminated if cap > 0). take != 0 also closes that interval without
   tapir writing it: the caller owns the data. Returns the full text length
   (call with cap = 0 to size), or -1 if no profiler is called `name`. */
typedef int64_t (*tapir_snapshot_fn)(const char *name, int take, char *buf, size_t cap);
/* The current interval of profiler `name` as a sparse vector of up to `cap`
   (ids[i], weights[i]) pairs; returns the pair count, or -1 if no profiler
   `name` has a vector form. take != 0 also closes the interval without tapir
   writing it. If more than `cap` pairs exist nothing is taken and the count is
   returned, so grow the buffers and retry. For the bbv profiler, ids are
   stable 64-bit block hashes (same across ranks and runs). */
typedef int64_t (*tapir_vector_fn)(const char *name, int take, uint64_t *ids, double *weights, size_t cap);
/* A named region marker delivered to all profilers. */
typedef void (*tapir_marker_fn)(const char *name, int begin);
/* Report a call, or any named activity with a time span, that tapir does not
   wrap itself (another tool's callback, an asynchronous operation, a manually
   instrumented region). Profilers see it as a call of `func` in library `lib`.
   arg_names[i] may carry a tag after a colon ("n:size"), which key rules can
   select ("@size:log2"). Timestamps come from tapir_now_ns(); pass 0, 0 to
   stamp "now". */
typedef void (*tapir_record_fn)(const char *lib, const char *func, int nargs,
                                const char *const *arg_names, const int64_t *arg_values,
                                uint64_t t_start, uint64_t t_end);
/* Write end-of-run output now (otherwise done at exit). */
typedef void (*tapir_flush_fn)(void);
/* tapir's clock: CLOCK_MONOTONIC in nanoseconds. */
typedef uint64_t (*tapir_now_ns_fn)(void);

#ifdef TAPIR_DIRECT
int tapir_active(void);
void tapir_enable(int on);
int64_t tapir_interval(int64_t index);
int64_t tapir_snapshot(const char *name, int take, char *buf, size_t cap);
int64_t tapir_vector(const char *name, int take, uint64_t *ids, double *weights, size_t cap);
void tapir_marker(const char *name, int begin);
void tapir_record(const char *lib, const char *func, int nargs, const char *const *arg_names,
                  const int64_t *arg_values, uint64_t t_start, uint64_t t_end);
void tapir_flush(void);
uint64_t tapir_now_ns(void);
#endif

/* ---- runtime-resolved helpers ------------------------------------------- */

/* Resolve a tapir symbol, caching hits. Misses are retried on the next call
   (cheap; these are called at interval granularity) so a tapir library that
   is dlopen'ed late is still found. */
#define TAPIR__SYM(type, name)                                              \
    static type tapir__##name##_p;                                          \
    if (!tapir__##name##_p)                                                 \
        tapir__##name##_p = (type)dlsym(RTLD_DEFAULT, #name);

static inline int tapir_try_active(void) {
    TAPIR__SYM(tapir_active_fn, tapir_active)
    return tapir__tapir_active_p ? tapir__tapir_active_p() : 0;
}
static inline void tapir_try_enable(int on) {
    TAPIR__SYM(tapir_enable_fn, tapir_enable)
    if (tapir__tapir_enable_p) tapir__tapir_enable_p(on);
}
static inline int64_t tapir_try_interval(int64_t index) {
    TAPIR__SYM(tapir_interval_fn, tapir_interval)
    return tapir__tapir_interval_p ? tapir__tapir_interval_p(index) : -1;
}
static inline int64_t tapir_try_snapshot(const char *name, int take, char *buf, size_t cap) {
    TAPIR__SYM(tapir_snapshot_fn, tapir_snapshot)
    return tapir__tapir_snapshot_p ? tapir__tapir_snapshot_p(name, take, buf, cap) : -1;
}
static inline int64_t tapir_try_vector(const char *name, int take, uint64_t *ids, double *weights, size_t cap) {
    TAPIR__SYM(tapir_vector_fn, tapir_vector)
    return tapir__tapir_vector_p ? tapir__tapir_vector_p(name, take, ids, weights, cap) : -1;
}
static inline void tapir_try_marker(const char *name, int begin) {
    TAPIR__SYM(tapir_marker_fn, tapir_marker)
    if (tapir__tapir_marker_p) tapir__tapir_marker_p(name, begin);
}
static inline void tapir_try_record(const char *lib, const char *func, int nargs,
                                    const char *const *arg_names, const int64_t *arg_values,
                                    uint64_t t_start, uint64_t t_end) {
    TAPIR__SYM(tapir_record_fn, tapir_record)
    if (tapir__tapir_record_p)
        tapir__tapir_record_p(lib, func, nargs, arg_names, arg_values, t_start, t_end);
}
static inline void tapir_try_flush(void) {
    TAPIR__SYM(tapir_flush_fn, tapir_flush)
    if (tapir__tapir_flush_p) tapir__tapir_flush_p();
}
static inline uint64_t tapir_try_now_ns(void) {
    TAPIR__SYM(tapir_now_ns_fn, tapir_now_ns)
    return tapir__tapir_now_ns_p ? tapir__tapir_now_ns_p() : 0;
}

#undef TAPIR__SYM

#ifdef __cplusplus
}
#endif

#endif /* TAPIR_H */

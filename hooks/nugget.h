/*
 * nugget.h: optional region-of-interest markers for nuggetkit.
 *
 * Without them, the region of interest is the whole run. To restrict it
 * (e.g. skip setup and I/O), bracket the work:
 *
 *     #include "nugget.h"
 *     ...
 *     NUGGET_ROI_BEGIN();
 *     solve();
 *     NUGGET_ROI_END();
 *
 * The symbols are weak, so the program builds and runs normally without
 * nuggetkit. From Fortran: `call nugget_roi_begin()` / `call nugget_roi_end()`
 * (the trailing underscore of the C names is Fortran's name mangling).
 */
#ifndef NUGGET_H
#define NUGGET_H

#ifdef __cplusplus
extern "C" {
#endif
void nugget_roi_begin_(void) __attribute__((weak));
void nugget_roi_end_(void) __attribute__((weak));
#ifdef __cplusplus
}
#endif

#define NUGGET_ROI_BEGIN() do { if (nugget_roi_begin_) nugget_roi_begin_(); } while (0)
#define NUGGET_ROI_END() do { if (nugget_roi_end_) nugget_roi_end_(); } while (0)

#endif

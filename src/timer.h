/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/* timer.h -- monotonic wall-clock in seconds, as a double. */
#ifndef SUNBENCH_TIMER_H
#define SUNBENCH_TIMER_H
#include "port.h"

/* Seconds since some fixed point.  Only differences are meaningful.
 * Uses gethrtime() where available (nanosecond, cheap). */
double sb_now P_((void));

/* Seconds via gettimeofday() (microsecond).  The CPU and memory benchmarks
 * time their kernels with this one everywhere, for one consistent clock
 * across the whole fleet (SunOS 4 has no gethrtime, so sb_now falls back to
 * gettimeofday there anyway -- this makes the choice explicit). */
double sb_gtod P_((void));

/* Resolution of sb_gtod/sb_now in seconds (1e-6, or 1/CLK_TCK on Minix). */
double sb_timer_res P_((void));

/* Minimum acceptable calibrated batch: max(50ms, 40 clock ticks). */
double sb_min_batch P_((void));

#endif

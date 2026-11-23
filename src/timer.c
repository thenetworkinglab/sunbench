/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * timer.c -- portable timing.
 *
 * gethrtime() is a nanosecond monotonic counter, ideal but Solaris 2.5+.
 * gettimeofday() is everywhere (SunOS 4 onward) at microsecond resolution,
 * which is plenty for disk work (we measure in milliseconds and up).
 * Minix 2 has neither; there the POSIX times() elapsed-tick count is the
 * only wall clock, at CLK_TCK (60Hz) resolution -- usable only because
 * sb_min_batch() stretches every calibrated batch to many ticks.
 */
#include "port.h"
#include "timer.h"

#ifdef HAVE_GETTIMEOFDAY
#include <sys/time.h>

static double wall()
{
    struct timeval tv;

    (void)gettimeofday(&tv, (struct timezone *)0);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1.0e6;
}

double sb_timer_res()
{
    return 1.0e-6;
}

#else /* no gettimeofday: POSIX times(), clock-tick resolution */
#include <sys/types.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

static double tick_s()
{
#ifdef CLK_TCK
    return 1.0 / (double)CLK_TCK;
#else
    return 1.0 / (double)sysconf(_SC_CLK_TCK);
#endif
}

static double wall()
{
    struct tms t;

    return (double)times(&t) * tick_s();
}

double sb_timer_res()
{
    return tick_s();
}
#endif

#ifdef HAVE_GETHRTIME
double sb_now()
{
    /* hrtime_t is long long, but only referenced under this guard. */
    return (double)gethrtime() / 1.0e9;
}
#else
double sb_now()
{
    return wall();
}
#endif

/* One consistent kernel-timing clock for the CPU/memory benchmarks. */
double sb_gtod()
{
    return wall();
}

/* Shortest batch a calibration loop may accept: 50ms normally, but at
 * least 40 clock ticks, so quantisation error stays under ~2.5% even on
 * a 60Hz times() clock (where 50ms would be three ticks, +-33%). */
double sb_min_batch()
{
    double m = 40.0 * sb_timer_res();

    return m > 0.05 ? m : 0.05;
}

#ifndef HAVE_STRERROR
#include <stdio.h>          /* sprintf for the unknown-errno case */
/* SunOS 4 libc predates ANSI strerror(); map errno through the BSD
 * error table.  port.h #defines strerror to this when the probe fails. */
extern int sys_nerr;
extern char *sys_errlist[];

char *sb_strerror(e)
    int e;
{
    static char ebuf[24];
    if (e >= 0 && e < sys_nerr)
        return sys_errlist[e];
    (void)sprintf(ebuf, "error %d", e);
    return ebuf;
}
#endif

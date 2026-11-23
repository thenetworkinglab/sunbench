/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * dhry_ref.c -- standalone Dhrystone 2.1 reference, for cross-checking the
 * figure sunbench's `cpu` command reports.  It links the SAME kernel objects
 * (dhry_1.o + dhry_2.o) as sunbench, so the compiled Dhrystone code is
 * identical; only the measurement harness differs.  Build it the same way you
 * build sunbench (same CC/MCPU) so the codegen matches:
 *
 *     make CC=gcc MCPU=-mcpu=v8 dhry-ref
 *     ./dhry-ref
 *
 * It times the SAME amortized loop two ways:
 *
 *   wall clock : elapsed real time (gettimeofday) -- the classic Dhrystone
 *               convention and what most published vintage figures use.
 *   CPU time   : process user+system time (times()) -- what sunbench divides
 *               by.  It excludes time the process spent OFF the CPU, so on a
 *               machine with any background activity it reads HIGHER than wall.
 *
 * The "CPU time" row should match `sunbench cpu`.  The gap between the two
 * rows is the whole story: if CPU time >> wall, the box was not 100% idle (or
 * times() is under-counting), and that -- not the CPU or the code -- is why a
 * Dhrystone figure can differ between builds that measured different clocks.
 * A `per-call` wall figure is also shown, to rule out batch-size effects.
 */
#include "port.h"
#include "dhry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>

/* Wall-clock seconds (gettimeofday is everywhere from SunOS 4 to Solaris 10). */
static double wnow()
{
    struct timeval tv;
    (void)gettimeofday(&tv, (struct timezone *)0);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1.0e6;
}

/* Process CPU seconds (user+system) -- exactly sunbench's cpu_seconds(). */
static double cnow()
{
    struct tms t;
    long tck;
    (void)times(&t);
    tck = sysconf(_SC_CLK_TCK);
    if (tck <= 0) tck = 100;
    return (double)(t.tms_utime + t.tms_stime) / (double)tck;
}

/* High-res seconds via gethrtime() -- EXACTLY sunbench's sb_now().  If this
 * disagrees with wnow() (gettimeofday) for the same work, the platform's
 * gethrtime is miscalibrated and that alone skews every sunbench timing. */
#ifdef HAVE_GETHRTIME
static double hnow()
{
    return (double)gethrtime() / 1.0e9;
}
#else
static double hnow()
{
    return wnow();               /* no gethrtime: fall back so rows match */
}
#endif

int main(argc, argv)
    int argc;
    char **argv;
{
    unsigned long runs, i;
    double w0, w1, c0, c1, h0, h1, wall, cpu, hwallc, aw, pcw, target;
    double dps_wall, dps_cpu, dps_h, dps_pc;

    target = (argc > 1) ? atof(argv[1]) : 4.0;   /* seconds per measurement */
    if (target < 0.5) target = 0.5;

    dhry_init();

    /* Calibrate `runs` so one amortized dhry_loop(runs) takes about `target`
     * seconds -- long enough to swamp clock granularity on a slow box. */
    runs = 30000L;
    for (;;) {
        w0 = wnow();
        dhry_loop(runs);
        wall = wnow() - w0;
        if (wall >= target || runs >= 400000000L)
            break;
        if (wall > 0.02)
            runs = (unsigned long)((double)runs * (target * 1.3 / wall)) + 1;
        else
            runs <<= 3;
    }

    /* Amortized loop, timed by ALL THREE clocks at once. */
    h0 = hnow(); w0 = wnow(); c0 = cnow();
    dhry_loop(runs);
    c1 = cnow(); w1 = wnow(); h1 = hnow();
    wall   = w1 - w0;
    aw     = wall;               /* keep the amortized elapsed for the summary */
    cpu    = c1 - c0;
    hwallc = h1 - h0;
    dps_wall = (wall   > 0.0) ? (double)runs / wall   : 0.0;
    dps_cpu  = (cpu    > 0.0) ? (double)runs / cpu    : 0.0;
    dps_h    = (hwallc > 0.0) ? (double)runs / hwallc : 0.0;

    /* Per-call (batch=1), wall clock, to rule out batch-size effects. */
    w0 = wnow();
    for (i = 0; i < runs; i++)
        dhry_loop(1L);
    pcw = wnow() - w0;
    dps_pc = (pcw > 0.0) ? (double)runs / pcw : 0.0;

    /* Sunbench-replica: EXACTLY sb_cpu_run's structure -- calibrate a batch,
     * then a wall-bounded loop of dhry_loop(batch) calls, and divide the total
     * runs by the times() CPU delta.  Reported both ways.  If the CPU-time row
     * here reproduces `sunbench cpu` while the wall row matches the amortized
     * figure above, the batched-loop + times() combination is the culprit. */
    {
        unsigned long b, dn;
        double e, hc0, hce, hwall, dps_hc, dps_hw;
        b = 1L;
        for (;;) {
            w0 = wnow();
            dhry_loop(b);
            if (wnow() - w0 >= 0.05 || b >= 1000000000L) break;
            b <<= 1;
        }
        dn = 0;
        w0 = wnow(); hc0 = cnow();
        for (;;) {
            dhry_loop(b);
            dn += b;
            e = wnow() - w0;
            if (e >= target) break;
        }
        hce = cnow() - hc0;
        hwall = e;
        dps_hc = (hce   > 0.0) ? (double)dn / hce   : 0.0;
        dps_hw = (hwall > 0.0) ? (double)dn / hwall : 0.0;

        printf("Dhrystone 2.1 reference (same kernel objects as sunbench)\n");
        printf("  runs per measurement : %lu   (batch %lu)\n\n", runs, b);
        printf("  %-32s %14s %10s\n", "method", "Dhrystones/s", "VAX-MIPS");
        printf("  %-32s %14.0f %10.2f\n",
               "amortized, gettimeofday wall", dps_wall, dps_wall / 1757.0);
        printf("  %-32s %14.0f %10.2f\n",
               "amortized, times() CPU", dps_cpu, dps_cpu / 1757.0);
        printf("  %-32s %14.0f %10.2f   <- sunbench's clock (sb_now)\n",
               "amortized, gethrtime wall", dps_h, dps_h / 1757.0);
        printf("  %-32s %14.0f %10.2f\n",
               "per-call (batch=1), gtod wall", dps_pc, dps_pc / 1757.0);
        printf("  %-32s %14.0f %10.2f\n",
               "sunbench-replica, gtod wall", dps_hw, dps_hw / 1757.0);
        printf("  %-32s %14.0f %10.2f\n",
               "sunbench-replica, times() CPU", dps_hc, dps_hc / 1757.0);
        printf("\n");
        printf("  For the SAME work over ~%.0fs, the three clocks measured:\n",
               target);
        printf("    gettimeofday %.3fs   times() %.3fs   gethrtime %.3fs\n",
               aw, cpu, hwallc);
        if (aw > 0.0)
            printf("    gethrtime/gettimeofday ratio : %.3f  (1.000 = agree; <1 = gethrtime runs FAST/short)\n",
                   hwallc / aw);
        printf("\n");
        printf("  If 'gethrtime wall' is much HIGHER than 'gettimeofday wall',\n");
        printf("  gethrtime under-measures elapsed time on this machine, which\n");
        printf("  inflates every sunbench rate (sunbench times with gethrtime).\n");

        /* Code layout: on the microSPARC's 4KB direct-mapped I-cache, two hot
         * functions whose addresses collide modulo 4096 evict each other every
         * iteration.  Comparing these values across binaries (dhry-ref vs
         * dhry-ref-sb vs sunbench) shows whether a score difference is caused
         * by link layout rather than by the code or the clock. */
        printf("\n  code layout (addr, and addr mod 4096 = I-cache line set):\n");
        printf("    dhry_loop  %08lx  mod4k %04lx\n",
               (unsigned long)dhry_loop, (unsigned long)dhry_loop & 0xfffL);
        printf("    Proc_6     %08lx  mod4k %04lx\n",
               (unsigned long)Proc_6, (unsigned long)Proc_6 & 0xfffL);
        printf("    Proc_7     %08lx  mod4k %04lx\n",
               (unsigned long)Proc_7, (unsigned long)Proc_7 & 0xfffL);
        printf("    Proc_8     %08lx  mod4k %04lx\n",
               (unsigned long)Proc_8, (unsigned long)Proc_8 & 0xfffL);
        printf("    Func_1     %08lx  mod4k %04lx\n",
               (unsigned long)Func_1, (unsigned long)Func_1 & 0xfffL);
        printf("    Func_2     %08lx  mod4k %04lx\n",
               (unsigned long)Func_2, (unsigned long)Func_2 & 0xfffL);
        printf("    strcpy     %08lx  mod4k %04lx\n",
               (unsigned long)strcpy, (unsigned long)strcpy & 0xfffL);
        printf("    strcmp     %08lx  mod4k %04lx\n",
               (unsigned long)strcmp, (unsigned long)strcmp & 0xfffL);
    }
    return 0;
}

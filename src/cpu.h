/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * cpu.h -- CPU benchmark kernels.
 *
 * Unlike the disk benchmark, CPU results depend on the compiler and the
 * target instruction set.  In particular sun4c (SPARCstation 1/IPC/IPX) is
 * SPARC V7 with NO hardware integer multiply/divide -- it does them in
 * software -- while sun4m/sun4u are V8/V9 with hardware mul/div.  The
 * mul/div kernel is deliberately split from the plain-ALU kernel so that gap
 * is visible.  Build target (compiler + -mcpu) is recorded alongside results.
 */
#ifndef SUNBENCH_CPU_H
#define SUNBENCH_CPU_H
#include "port.h"
#include "bench_disk.h"     /* sb_reporter */

/* Kernel identifiers. */
#define SBK_INT_ALU    0    /* add/shift/logic -- ISA-neutral, scales w/ clock */
#define SBK_INT_MULDIV 1    /* multiply/divide -- exposes V7 (soft) vs V8 (hw) */
#define SBK_FP_DOUBLE  2    /* double-precision float -- exercises the FPU */
#define SBK_SAXPY      3    /* single-precision y+=a*x, Linpack-style MFLOPS */
#define SBK_DAXPY      4    /* double-precision y+=a*x, Linpack-style MFLOPS */
#define SBK_SAXPY_MEM  5    /* saxpy over a > any-cache footprint (streaming) */
#define SBK_DAXPY_MEM  6    /* daxpy over a > any-cache footprint (streaming) */
#define SBK_DHRYSTONE  7    /* Dhrystone 2.1 -- classic integer, VAX MIPS */
#define SBK_WHETSTONE  8    /* Whetstone -- classic floating point, MWIPS */
#define SBK_NKERNELS   9

#define SBK_IS_XPY(k)  ((k) >= SBK_SAXPY && (k) <= SBK_DAXPY_MEM)

/* The plain ?axpy kernels stream two N-element vectors (the Linpack inner
 * loop) sized like the classic Linpack-100 working set: ~40KB single /
 * ~80KB double.  That is bigger than a sun4c cache (so those figures are
 * memory-honest, like the era's published numbers) but FITS in a big
 * external cache (HyperSPARC 512KB, SuperCache, Ultra E$) -- faithfully
 * reproducing how Linpack-100 behaved there too.  The *_MEM variants run
 * the same loop over runtime-sized vectors big enough to overflow ANY
 * fleet cache; the gap between the pairs IS the cache benefit.
 * 2 FLOPs per element; per-pass FLOPs via sb_cpu_xpy_flops(). */
#define SBK_XPY_N     5000
#define SBK_XPY_FLOPS (2.0 * (double)SBK_XPY_N)   /* per pass, plain pair */

/* FLOPs per pass for any of the four ?axpy kernels (the streaming pair's
 * vector length is chosen at runtime from RAM). */
double sb_cpu_xpy_flops P_((int kernel));

typedef struct sb_cpu_result {
    char   name[24];
    double iters;           /* total loop iterations performed */
    double seconds;
    double iters_per_s;     /* iterations/second */
    int    ok;
} sb_cpu_result;

const char *sb_cpu_kernel_name P_((int kernel));

#ifdef SB_SEGMENTED
/* SBDOS: give the ?axpy vectors back to DOS after a cpu run. */
void sb_cpu_release P_((void));
#endif

/* Run one kernel for about `secs` wall seconds; fill *r.  rep may be NULL. */
int sb_cpu_run P_((int kernel, double secs, sb_cpu_result *r, sb_reporter *rep));

/* Build-target identity ("gcc, -mcpu=v7" etc.), recorded with results. */
const char *sb_cpu_build_id P_((void));

/* --- multiprocessor (fork-based) mode ---------------------------------- *
 * SunOS 4 has no threads, so parallelism is by fork(): the parent runs one
 * process alone to get a single-CPU baseline, then forks N children that run
 * the same kernel concurrently and report their iteration counts back over a
 * pipe.  Comparing the N-way aggregate to N x the baseline exposes memory-bus
 * (MBus/XBus) contention -- the reason two 150 MHz CPUs are not twice one.
 * Both figures are WALL-clock rates, so they are directly comparable (the
 * single-process `cpu` report uses CPU time instead; see cpu.c).            */

/* Online processor count (sysconf where available, else 1). */
int sb_cpu_ncpus P_((void));

typedef struct sb_cpu_mp {
    char   name[24];
    int    ncpus;           /* number of concurrent processes actually run */
    double single_ips;      /* one process alone, iterations/second (wall) */
    double agg_ips;         /* N processes, summed iterations/second (wall) */
    double speedup;         /* agg_ips / single_ips (ideal = ncpus) */
    double efficiency;      /* speedup / ncpus (ideal = 1.0) */
    int    ok;
} sb_cpu_mp;

/* Baseline one process, then `ncpus` forked concurrently; fill *r. */
int sb_cpu_run_mp P_((int kernel, double secs, int ncpus, sb_cpu_mp *r));

#endif

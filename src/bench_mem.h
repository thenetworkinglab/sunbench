/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * bench_mem.h -- memory bandwidth and latency benchmark.
 *
 * The flagship view is a WORKING-SET SWEEP: each kernel is run over buffers
 * from 1KB up to several MB, doubling each step.  On classic Sun hardware the
 * result is a photograph of the cache hierarchy -- bandwidth falls off a
 * cliff at each cache boundary (sun4c 64KB unified; microSPARC 4K/2K;
 * SuperSPARC/HyperSPARC L1 + optional MBus-module cache; UltraSPARC L1 +
 * E-cache), and latency steps up from cache-hit to DRAM.
 *
 * Kernels:
 *   read    sum a buffer of unsigned longs -- 4-byte load bandwidth, what
 *           ordinary integer code gets.
 *   read8   sum a buffer of doubles -- 8-byte (ldd) load bandwidth; on
 *           32-bit SPARC the FPU path moves twice the bytes per instruction,
 *           so read8 vs read shows the peak-vs-typical gap.
 *   write   fill with unsigned longs -- store bandwidth.
 *   copy    libc memcpy of half the working set onto the other half; MB/s
 *           counts bytes COPIED once (not read+written).  This is the "real
 *           application" figure and uses whatever tricks libc has.  The
 *           destination is skewed 512 bytes so src and dst never alias in
 *           a direct-mapped cache (size/2 apart = same index for every
 *           whole-MB set; the mid-line ping-pong measured 3.8 MB/s on a
 *           SPARCstation 10's 512KB E$ before the skew).
 *   latency random pointer-chase at 64-byte stride, ns per dependent load --
 *           each load's address depends on the previous load's data, so
 *           nothing can be overlapped or prefetched.  The honest cache/DRAM
 *           latency.
 *
 * Rates are measured like the CPU module: sum of per-batch wall time
 * (sb_gtod), volatile sinks against dead-code elimination, idle box assumed.
 */
#ifndef SUNBENCH_BENCH_MEM_H
#define SUNBENCH_BENCH_MEM_H
#include "port.h"
#include "cpu.h"        /* sb_cpu_mp: the fork-based MP result shape */

#define SBMEM_READ     0
#define SBMEM_READ8    1
#define SBMEM_WRITE    2
#define SBMEM_COPY     3
#define SBMEM_LAT      4
#define SBMEM_NKERNELS 5

typedef struct sb_mem_result {
    double mb_per_s;        /* bandwidth kernels; 0 for latency */
    double ns_per_load;     /* latency kernel; 0 for bandwidth  */
    int    ok;
} sb_mem_result;

const char *sb_mem_kernel_name P_((int kernel));

/* Run one kernel over the first `size` bytes of `buf` (page-aligned, at
 * least `size` big) for about `secs` seconds of compute; fill *r. */
int sb_mem_run P_((int kernel, char *buf, unsigned long size, double secs,
                   sb_mem_result *r));

/* MP scaling: one process running `kern` over a `size` buffer alone, then
 * `ncpus` processes concurrently, each with its OWN buffer.  Aggregate vs
 * single shows how much memory bandwidth the interconnect (sun4m MBus,
 * sun4u UPA, sun4c's own memory path) really has beyond one CPU's demand.
 * Bandwidth kernels only (read/read8/write/copy); use sb_mem_lat_mp for
 * latency. */
int sb_mem_run_mp P_((int kern, unsigned long size, double secs, int ncpus,
                      sb_cpu_mp *r));

/* Loaded latency: the pointer chase alone, then again while ncpus-1
 * processes stream sequential reads through private `size` buffers.
 * single_ips/agg_ips carry ns per load (alone/loaded); speedup carries the
 * slowdown factor; efficiency is -1 (not applicable). */
int sb_mem_lat_mp P_((unsigned long size, double secs, int ncpus,
                      sb_cpu_mp *r));

#endif

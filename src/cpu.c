/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * cpu.c -- integer and floating-point CPU kernels.
 *
 * Each kernel is a tight loop over a fixed body.  To stop the compiler from
 * (a) deleting the loop as dead code or (b) precomputing its closed form, the
 * running value is seeded from a volatile sink at the start of every batch and
 * stored back to a volatile sink at the end.  The reported unit is loop
 * iterations per second, which is directly comparable across machines for the
 * SAME kernel; the point of the tool is cross-machine ratios, not an absolute
 * "MIPS" that would only invite compiler arguments.
 */
#include "port.h"
#include "cpu.h"
#include "dhry.h"
#include "whet.h"
#include "timer.h"
#include "diskio.h"     /* sb_alloc_aligned for the streaming ?axpy pair */
#include "sysinfo.h"    /* sb_physmem_mb sizes those vectors */

#include <stdio.h>
#include <string.h>
#ifndef SB_NOFORK
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Wall clock for CPU measurement, via sb_gtod(): gettimeofday() from SunOS 4
 * up (microsecond resolution is ample for second-long runs), times() ticks
 * on Minix 2, where sb_min_batch() keeps calibration honest.  The rate is the
 * sum of wall time spent inside the kernel batches only (see sb_cpu_run), so
 * the live display is never part of the timed region and a slow console
 * cannot skew the result.  Assumes an otherwise idle machine.
 *
 * Historical note: a ~1.5x Dhrystone swing between sunbench builds on a
 * SPARCstation LX was chased through times()/gethrtime/reporter theories
 * before being root-caused to CODE LAYOUT.  On the microSPARC's 4KB
 * direct-mapped I-cache the score depends on where the linker places the hot
 * Dhrystone functions and libc's strcpy/strcmp: identical objects measured
 * 40,012 Dhrystones/s in one link order and 58,425 in another (see the
 * dhry-ref vs dhry-ref-sb make targets, which prove and demonstrate this).
 * The clocks were never at fault -- gettimeofday, times() and gethrtime all
 * agree on that machine.  The custom kernels are small enough to be immune. */
static double cpu_wall()
{
    return sb_gtod();
}

/* Volatile sinks: reading/writing these across batch boundaries defeats
 * dead-code elimination and closed-form folding. */
static volatile unsigned long g_uslot = 1L;
static volatile double        g_dslot = 1.0;

static const char *knames[SBK_NKERNELS] = {
    "int alu", "int muldiv", "fp double", "saxpy sp", "daxpy dp",
    "saxpy mem", "daxpy mem", "dhrystone", "whetstone"
};

/* Linpack-inner-loop vectors (see cpu.h).  Static, initialized lazily:
 * ~40KB float + ~80KB double of BSS, well within the 28MB floor. */
#ifdef SB_SEGMENTED
/* SBDOS (MS-DOS, 640 KB): 120 KB of static vectors would sit in memory
 * through every other test, so there they come from DOS (sb_seg_alloc,
 * one block per vector, each under 64 KB) on first use, and go back via
 * sb_cpu_release() when the cpu run is over. */
char *sb_seg_alloc P_((unsigned long sz));
void  sb_seg_free P_((char *p));
static float  *sax_x = (float *)0, *sax_y = (float *)0;
static double *dax_x = (double *)0, *dax_y = (double *)0;
#else
static float  sax_x[SBK_XPY_N], sax_y[SBK_XPY_N];
static double dax_x[SBK_XPY_N], dax_y[SBK_XPY_N];
#endif
static int    xpy_ready = 0;

#ifdef SB_SEGMENTED
static int xpy_alloc()
{
    if (sax_x) return 0;
    sax_x = (float *)sb_seg_alloc((unsigned long)SBK_XPY_N * sizeof(float));
    sax_y = (float *)sb_seg_alloc((unsigned long)SBK_XPY_N * sizeof(float));
    dax_x = (double *)sb_seg_alloc((unsigned long)SBK_XPY_N * sizeof(double));
    dax_y = (double *)sb_seg_alloc((unsigned long)SBK_XPY_N * sizeof(double));
    if (sax_x && sax_y && dax_x && dax_y) return 0;
    sb_cpu_release();
    return -1;
}

void sb_cpu_release()
{
    sb_seg_free((char *)sax_x); sb_seg_free((char *)sax_y);
    sb_seg_free((char *)dax_x); sb_seg_free((char *)dax_y);
    sax_x = sax_y = (float *)0;
    dax_x = dax_y = (double *)0;
    xpy_ready = 0;
}
#endif

static void xpy_init()
{
    int i;
    for (i = 0; i < SBK_XPY_N; i++) {
        sax_x[i] = 1.0;  sax_y[i] = 0.0;
        dax_x[i] = 1.0;  dax_y[i] = 0.0;
    }
    xpy_ready = 1;
}

/* One pass of y += a*x.  With a = 0.5 and x = 1.0, y grows by 0.5 per pass:
 * even hours of passes stay far from overflow, and never denormal. */
static void k_saxpy(n)
    unsigned long n;
{
    unsigned long p;
    int i;
    for (p = 0; p < n; p++)
        for (i = 0; i < SBK_XPY_N; i++)
            sax_y[i] += (float)0.5 * sax_x[i];
    g_dslot = (double)sax_y[0];
}

static void k_daxpy(n)
    unsigned long n;
{
    unsigned long p;
    int i;
    for (p = 0; p < n; p++)
        for (i = 0; i < SBK_XPY_N; i++)
            dax_y[i] += 0.5 * dax_x[i];
    g_dslot = dax_y[0];
}

/* --- streaming ?axpy: vectors big enough to overflow ANY fleet cache ----
 * Sized at runtime: min(4MB, RAM/8) per vector (floor 256KB, default 1MB
 * when RAM is unknown), so the PAIR overflows even an Ultra 60's 4MB
 * E-cache while a 28MB SPARCstation 1 stays comfortable.  Both precisions
 * reuse the same two buffers; a mode flag re-initializes on switch. */
static char *xm_x = (char *)0, *xm_y = (char *)0;
static unsigned long xm_bytes = 0;      /* per vector */
static int xm_mode = 0;                 /* 0 none, 1 floats, 2 doubles */
static int xm_failed = 0;

static int xm_alloc()
{
    unsigned long mb;
    if (xm_x != (char *)0) return 0;
    if (xm_failed) return -1;
    mb = sb_physmem_mb();
    if (mb == 0) xm_bytes = 1024L * 1024L;
    else {
        xm_bytes = (mb / 8L) * 1024L * 1024L;
        if (xm_bytes > 4L * 1024L * 1024L)
            xm_bytes = 4L * 1024L * 1024L;
        if (xm_bytes < 256L * 1024L)
            xm_bytes = 256L * 1024L;
    }
    xm_x = sb_alloc_aligned(xm_bytes);
    xm_y = sb_alloc_aligned(xm_bytes);
    if (xm_x == (char *)0 || xm_y == (char *)0) {
        xm_failed = 1;
        return -1;
    }
    return 0;
}

static void xm_init(dbl)
    int dbl;
{
    unsigned long i, n;
    if (dbl) {
        double *x = (double *)xm_x, *y = (double *)xm_y;
        n = xm_bytes / sizeof(double);
        for (i = 0; i < n; i++) { x[i] = 1.0; y[i] = 0.0; }
        xm_mode = 2;
    } else {
        float *x = (float *)xm_x, *y = (float *)xm_y;
        n = xm_bytes / sizeof(float);
        for (i = 0; i < n; i++) { x[i] = 1.0; y[i] = 0.0; }
        xm_mode = 1;
    }
}

static void k_saxpy_mem(np)
    unsigned long np;
{
    float *x = (float *)xm_x, *y = (float *)xm_y;
    unsigned long p, i, n;
    n = xm_bytes / sizeof(float);
    for (p = 0; p < np; p++)
        for (i = 0; i < n; i++)
            y[i] += (float)0.5 * x[i];
    g_dslot = (double)y[0];
}

static void k_daxpy_mem(np)
    unsigned long np;
{
    double *x = (double *)xm_x, *y = (double *)xm_y;
    unsigned long p, i, n;
    n = xm_bytes / sizeof(double);
    for (p = 0; p < np; p++)
        for (i = 0; i < n; i++)
            y[i] += 0.5 * x[i];
    g_dslot = y[0];
}

double sb_cpu_xpy_flops(kernel)
    int kernel;
{
    if (kernel == SBK_SAXPY || kernel == SBK_DAXPY)
        return SBK_XPY_FLOPS;
    if (kernel == SBK_SAXPY_MEM)
        return 2.0 * (double)(xm_bytes / sizeof(float));
    if (kernel == SBK_DAXPY_MEM)
        return 2.0 * (double)(xm_bytes / sizeof(double));
    return 0.0;
}

const char *sb_cpu_kernel_name(kernel)
    int kernel;
{
    if (kernel < 0 || kernel >= SBK_NKERNELS) return "?";
    return knames[kernel];
}

/* --- the kernels: each runs `n` iterations, seeded from `s`, returns result */

static unsigned long k_alu(n, s)
    unsigned long n, s;
{
    unsigned long a, b, c, d, i;
    a = s | 1L; b = s ^ 0x9e3779b9L; c = s + 3L; d = ~s;
    for (i = 0; i < n; i++) {
        a += b; b ^= c; c += (a << 1); d -= (b >> 1);
        a |= d; b += c; c ^= (d << 3); d += (a >> 2);
    }
    return a + b + c + d;
}

static unsigned long k_muldiv(n, s)
    unsigned long n, s;
{
    unsigned long x, c, i;
    x = s | 1L; c = 7L;
    for (i = 0; i < n; i++) {
        x = x * 1103515245L + 12345L;   /* multiply */
        c = x / 97L;                      /* divide   */
        x = x ^ (c * 3L);                 /* multiply */
        c = x % 131L;                     /* modulo (divide) */
        x = x + c;
    }
    return x + c;
}

/* The recurrence is a CONTRACTION, provably bounded in ~[0.1, 1.5] for any
 * seed in [0, 4] (verified numerically for 10M iterations across the whole
 * seed range).  This matters: an earlier version diverged to Inf by
 * iteration 15 and computed NaN forever after -- invisible on FPUs that
 * handle specials in hardware (modern CPUs, microSPARC), but on a
 * trap-to-kernel-emulation FPU (the SPARCstation 1's Weitek 3170) every NaN
 * op trapped and the kernel measured ~1000x slow.  The seed check also
 * self-heals a NaN handed in via the volatile sink (NaN compares false). */
static double k_fp(n, s)
    unsigned long n;
    double s;
{
    double x, y, z;
    unsigned long i;
    if (!(s >= 0.0 && s <= 4.0)) s = 1.0;
    x = s + 1.0; y = 2.0; z = 3.0;
    for (i = 0; i < n; i++) {
        x = (x + y) * 0.5;
        y = (z - x) * 0.5;
        z = x * y * 0.25 + 1.0;
        x = z / (y + 2.0) + 1.0;
    }
    return x + y + z;
}

/* Run one batch of `n` iterations of `kernel`, threading the volatile sink. */
static void run_batch(kernel, n)
    int kernel;
    unsigned long n;
{
    if (kernel == SBK_DHRYSTONE)
        dhry_loop(n);                 /* opaque (separate TU): not optimised out */
    else if (kernel == SBK_WHETSTONE)
        g_dslot = whet_loop(n);
    else if (kernel == SBK_FP_DOUBLE)
        g_dslot = k_fp(n, (double)g_dslot);
    else if (kernel == SBK_SAXPY) {
        if (!xpy_ready) xpy_init();
        k_saxpy(n);
    }
    else if (kernel == SBK_DAXPY) {
        if (!xpy_ready) xpy_init();
        k_daxpy(n);
    }
    else if (kernel == SBK_SAXPY_MEM) {
        if (xm_alloc() != 0) return;
        if (xm_mode != 1) xm_init(0);
        k_saxpy_mem(n);
    }
    else if (kernel == SBK_DAXPY_MEM) {
        if (xm_alloc() != 0) return;
        if (xm_mode != 2) xm_init(1);
        k_daxpy_mem(n);
    }
    else if (kernel == SBK_INT_MULDIV)
        g_uslot = k_muldiv(n, (unsigned long)g_uslot);
    else
        g_uslot = k_alu(n, (unsigned long)g_uslot);
}

/* Grow a batch from 1 until one takes at least ~50ms; return that batch size.
 * We start small because a "unit" ranges from a few ALU ops to a whole
 * Whetstone pass -- a large initial batch could run for minutes. */
static unsigned long calibrate(kernel)
    int kernel;
{
    unsigned long batch;
    batch = 1L;
    for (;;) {
        double a = cpu_wall();
        run_batch(kernel, batch);
        if (cpu_wall() - a >= sb_min_batch() || batch >= 1000000000L)
            break;
        batch <<= 1;
    }
    return batch;
}

int sb_cpu_run(kernel, secs, r, rep)
    int kernel;
    double secs;
    sb_cpu_result *r;
    sb_reporter *rep;
{
    unsigned long batch, done;
    double t0, a, b, now, compute;
    char *label;

    r->ok = 0;
    label = (char *)sb_cpu_kernel_name(kernel);
    if (kernel == SBK_DHRYSTONE)
        dhry_init();                  /* one-time global setup before timing */
    if ((kernel == SBK_SAXPY_MEM || kernel == SBK_DAXPY_MEM) &&
        xm_alloc() != 0)
        return -1;                    /* no buffers: skip, don't spin */
#ifdef SB_SEGMENTED
    if ((kernel == SBK_SAXPY || kernel == SBK_DAXPY) && xpy_alloc() != 0)
        return -1;
#endif

    /* Rate = iterations / time spent COMPUTING.  We time each kernel batch
     * individually (cpu_wall around run_batch) and sum only those intervals,
     * so the reporter -- however slow the console -- is never inside the timed
     * region and cannot skew the result.  The run is bounded by accumulated
     * compute time, giving a consistent measurement window regardless of how
     * long the display takes to paint.  See cpu_wall() for the clock choice. */
    batch = calibrate(kernel);
    done = 0;
    compute = 0.0;
    t0 = cpu_wall();
    if (rep && rep->begin) rep->begin(rep->ctx, label, t0);
    for (;;) {
        a = cpu_wall();
        run_batch(kernel, batch);
        b = cpu_wall();
        compute += b - a;                 /* only the compute counts */
        done += batch;
        if (rep && rep->tick) {
            double ips = compute > 0.0 ? (double)done / compute : 0.0;
            rep->tick(rep->ctx, label, compute / secs, b, ips / 1.0e6, ips);
        }
        if (compute >= secs)
            break;
    }
    now = cpu_wall();

    strncpy(r->name, label, sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    r->iters = (double)done;
    r->seconds = compute;
    r->iters_per_s = compute > 0.0 ? (double)done / compute : 0.0;
    r->ok = 1;
    if (rep && rep->end) rep->end(rep->ctx, label, now, (sb_result *)0);
    return 0;
}

/* Build identity from compile-time defines injected by the Makefile. */
const char *sb_cpu_build_id()
{
    static char id[96];
#ifndef SB_CCNAME
#define SB_CCNAME "cc"
#endif
#ifndef SB_ARCH
#define SB_ARCH ""
#endif
    if (SB_ARCH[0] != '\0')
        sprintf(id, "%s %s", SB_CCNAME, SB_ARCH);
    else
        sprintf(id, "%s (default arch)", SB_CCNAME);
    return id;
}

#ifndef SB_NOFORK  /* MP mode needs fork(); SBDOS (MS-DOS) has none */
/* --- multiprocessor (fork-based) mode ---------------------------------- */

/* Run `kernel` for about `secs` seconds of compute; return iterations
 * completed and the compute seconds.  No reporter here (children are silent),
 * so summing per-batch wall equals the loop wall; we still use cpu_wall()
 * (gettimeofday) for the same reason sb_cpu_run does.  The single-process
 * baseline and each concurrent child are measured identically, so MBus
 * contention shows up as reduced aggregate throughput. */
static void run_wall(kernel, secs, iters_out, wall_out)
    int kernel;
    double secs;
    double *iters_out, *wall_out;
{
    unsigned long batch, done;
    double t0, elapsed;

    if (kernel == SBK_DHRYSTONE)
        dhry_init();
    if ((kernel == SBK_SAXPY_MEM || kernel == SBK_DAXPY_MEM) &&
        xm_alloc() != 0) {
        *iters_out = 0.0;
        *wall_out = 0.0;
        return;
    }
    batch = calibrate(kernel);
    done = 0;
    elapsed = 0.0;
    t0 = cpu_wall();
    for (;;) {
        run_batch(kernel, batch);
        done += batch;
        elapsed = cpu_wall() - t0;
        if (elapsed >= secs)
            break;
    }
    *iters_out = (double)done;
    *wall_out = elapsed;
}

int sb_cpu_ncpus()
{
#ifdef HAVE_SC_NPROCESSORS_ONLN
    long n;
    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    return (int)n;
#else
    return 1;
#endif
}

/* One fixed-size record per child, small enough (16 bytes << PIPE_BUF) that
 * each child's write to the shared pipe is atomic and never interleaves. */
struct mprec { double iters; double wall; };

/* Read exactly n bytes (a pipe read may return a short count). */
static int read_full(fd, buf, n)
    int fd; char *buf; int n;
{
    int off, k;
    off = 0;
    while (off < n) {
        k = (int)read(fd, buf + off, (unsigned)(n - off));
        if (k <= 0) break;
        off += k;
    }
    return off;
}

#define SB_MAXCPUS 64

int sb_cpu_run_mp(kernel, secs, ncpus, r)
    int kernel;
    double secs;
    int ncpus;
    sb_cpu_mp *r;
{
    struct mprec rec;
    double s_iters, s_wall, agg;
    int fds[2];
    int pids[SB_MAXCPUS];
    int i, got, live;

    r->ok = 0;
    strncpy(r->name, sb_cpu_kernel_name(kernel), sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    if (ncpus < 1) ncpus = 1;
    if (ncpus > SB_MAXCPUS) ncpus = SB_MAXCPUS;

    /* 1. Single-process baseline, alone on the machine (wall-clock rate). */
    run_wall(kernel, secs, &s_iters, &s_wall);
    r->single_ips = s_wall > 0.0 ? s_iters / s_wall : 0.0;

    /* 2. Fork ncpus children that run the same kernel concurrently. */
    if (pipe(fds) != 0)
        return -1;
    live = ncpus;
    for (i = 0; i < ncpus; i++) {
        int pid = fork();
        if (pid == 0) {                     /* child */
            struct mprec out;
            double ci, cw;
            (void)close(fds[0]);
            run_wall(kernel, secs, &ci, &cw);
            out.iters = ci; out.wall = cw;
            (void)write(fds[1], (char *)&out, sizeof(out));
            (void)close(fds[1]);
            _exit(0);
        }
        if (pid < 0) {                      /* fork failed: run with fewer */
            live = i;
            break;
        }
        pids[i] = pid;
    }
    (void)close(fds[1]);                    /* parent only reads */

    agg = 0.0;
    for (got = 0; got < live; got++) {
        if (read_full(fds[0], (char *)&rec, (int)sizeof(rec))
                != (int)sizeof(rec))
            break;
        if (rec.wall > 0.0)
            agg += rec.iters / rec.wall;
    }
    (void)close(fds[0]);
    for (i = 0; i < live; i++)
        (void)wait((int *)0);

    if (live < 1) return -1;
    r->ncpus = live;
    r->agg_ips = agg;
    r->speedup = r->single_ips > 0.0 ? agg / r->single_ips : 0.0;
    r->efficiency = r->speedup / (double)live;
    r->ok = 1;
    return 0;
}

#endif /* SB_NOFORK */

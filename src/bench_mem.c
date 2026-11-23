/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * bench_mem.c -- memory bandwidth and latency kernels.  See bench_mem.h.
 *
 * Measurement mirrors cpu.c: calibrate a batch (whole passes over the
 * working set) to >= ~50ms, then repeat batches summing per-batch wall time
 * (sb_gtod) until the requested compute time is reached.  The reporter/UI is
 * never inside a timed region.  Volatile sinks keep the compiler honest.
 */
#include "port.h"
#include "bench_mem.h"
#include "diskio.h"     /* sb_alloc_aligned for the MP children */
#include "timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef SB_NOFORK
#include <signal.h>     /* SIGTERM stops the loaded-latency streamers */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Chase stride: one hop per 64 bytes.  64 covers every line size in the
 * fleet (sun4c 16/32B, microSPARC 32B, UltraSPARC E$ 64B), so consecutive
 * hops never share a cache line. */
#define SB_CHASE_STRIDE 64L
#define SB_COPY_SKEW    512L   /* dst offset breaking direct-mapped alias */

/* 16-bit real mode (SBDOS, MS-DOS): a far pointer reaches only 64 KB.
 * Working sets bigger than that are walked one 64 KB segment at a time,
 * each segment by the SAME kernel as everywhere else; one extra call per
 * 64 KB is noise.  The platform supplies sb_seg_ptr(base, off): base+off
 * as a normalized far pointer.  Sets up to 64 KB take the ordinary path,
 * and on every other platform MEM_AT is plain pointer arithmetic. */
#ifdef SB_SEGMENTED
#define SB_SEG 65536L
char *sb_seg_ptr P_((char *base, unsigned long off));
#define MEM_AT(b, off) sb_seg_ptr((b), (unsigned long)(off))
#else
#define MEM_AT(b, off) ((b) + (off))
#endif

/* Loop index for the bandwidth kernels.  A pass never exceeds one 64 KB
 * segment in the DOS build, so there it is a 16-bit unsigned: a 32-bit
 * index in 8086 code costs more cycles than the load it addresses and
 * flattened the cache step out of the read/write curves (measured on a
 * PS/2 Model 70).  Everywhere else it stays unsigned long. */
#ifdef SB_SEGMENTED
typedef unsigned sb_idx;
#else
typedef unsigned long sb_idx;
#endif

static volatile unsigned long g_msink = 0;
static volatile double        g_dsink = 0.0;
/* NB: the POINTER must be volatile (char * volatile), not the pointee --
 * `volatile char *` would make the store a dead write and let the compiler
 * delete the whole dependent-load chain it sinks. */
static char * volatile        g_psink = (char *)0;

static const char *mknames[SBMEM_NKERNELS] = {
    "read", "read8", "write", "copy", "latency"
};

const char *sb_mem_kernel_name(kernel)
    int kernel;
{
    if (kernel < 0 || kernel >= SBMEM_NKERNELS) return "?";
    return mknames[kernel];
}

/* --- bandwidth kernels: one call = one full pass over the working set --- */

static unsigned long k_read(p, words)
    unsigned long *p;
    unsigned long words;
{
    unsigned long s0, s1, s2, s3;
    sb_idx i, n;
    n = (sb_idx)words;
    s0 = s1 = s2 = s3 = 0L;
    for (i = 0; i < n; i += 8) {
        s0 += p[i];     s1 += p[i + 1]; s2 += p[i + 2]; s3 += p[i + 3];
        s0 += p[i + 4]; s1 += p[i + 5]; s2 += p[i + 6]; s3 += p[i + 7];
    }
    return s0 + s1 + s2 + s3;
}

static double k_read8(dp, dwords)
    double *dp;
    unsigned long dwords;
{
    double s0, s1, s2, s3;
    sb_idx i, n;
    n = (sb_idx)dwords;
    s0 = s1 = s2 = s3 = 0.0;
    for (i = 0; i < n; i += 8) {
        s0 += dp[i];     s1 += dp[i + 1]; s2 += dp[i + 2]; s3 += dp[i + 3];
        s0 += dp[i + 4]; s1 += dp[i + 5]; s2 += dp[i + 6]; s3 += dp[i + 7];
    }
    return s0 + s1 + s2 + s3;
}

static void k_write(p, words, v)
    unsigned long *p;
    unsigned long words, v;
{
    sb_idx i, n;
    n = (sb_idx)words;
    for (i = 0; i < n; i += 8) {
        p[i]     = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
    }
}

/* --- latency: pointer chase over a random ring at SB_CHASE_STRIDE --- */

/* Small deterministic PRNG (same chain on every machine and every run, so
 * results are comparable; no dependence on the platform's random()). */
static unsigned long mem_rand(s)
    unsigned long *s;
{
    *s = (*s * 1664525L + 1013904223L) & 0xffffffffL;
    return *s;
}

/* Link every stride-slot of buf into one random cycle; return the entry
 * point.  Fisher-Yates over a temporary index array (freed before timing),
 * then slot[i] -> slot[i+1] in shuffled order -- a single cycle covering
 * all slots, so one lap = nslots dependent loads over the whole set. */
static char *build_chain(buf, size)
    char *buf;
    unsigned long size;
{
    unsigned long nslots, i, j, t, seed;
    unsigned long *idx;

    nslots = size / SB_CHASE_STRIDE;
    idx = (unsigned long *)malloc(nslots * sizeof(unsigned long));
    if (idx == (unsigned long *)0)
        return (char *)0;
    for (i = 0; i < nslots; i++)
        idx[i] = i;
    seed = 0x5bd1e995L;
    for (i = nslots - 1; i > 0; i--) {
        j = mem_rand(&seed) % (i + 1);
        t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    for (i = 0; i < nslots; i++) {
        char **slot = (char **)MEM_AT(buf, idx[i] * SB_CHASE_STRIDE);
        char  *next = MEM_AT(buf, idx[(i + 1) % nslots] * SB_CHASE_STRIDE);
        *slot = next;
    }
    t = idx[0];
    free((char *)idx);
    return MEM_AT(buf, t * SB_CHASE_STRIDE);
}

/* `n` dependent loads (n is a multiple of 8).  Every load's address is the
 * previous load's data: nothing to prefetch, nothing to overlap. */
static char *k_chase(start, n)
    char *start;
    unsigned long n;
{
    char *p;
    unsigned long i;
    p = start;
    for (i = 0; i < n; i += 8) {
        p = *(char **)p; p = *(char **)p; p = *(char **)p; p = *(char **)p;
        p = *(char **)p; p = *(char **)p; p = *(char **)p; p = *(char **)p;
    }
    return p;
}

#ifdef SB_SEGMENTED
/* The bandwidth kernels over a > 64 KB set, segment by segment.  copy
 * moves the same bytes as the flat version, in 32 KB pieces (memcpy's
 * size_t is 16 bits). */
static double seg_pass(kernel, buf, size)
    int kernel;
    char *buf;
    unsigned long size;
{
    unsigned long o, n, half, skew;
    char *p;

    if (kernel == SBMEM_COPY) {
        half = size / 2;
        skew = SB_COPY_SKEW;
        for (o = 0; o < half - skew; o += n) {
            n = half - skew - o;
            if (n > 32768L) n = 32768L;
            (void)memcpy(MEM_AT(buf, half + skew + o), MEM_AT(buf, o),
                         (unsigned)n);
        }
        return (double)(half - skew);
    }
    for (o = 0; o < size; o += SB_SEG) {
        n = size - o;
        if (n > SB_SEG) n = SB_SEG;
        p = MEM_AT(buf, o);
        if (kernel == SBMEM_READ)
            g_msink = k_read((unsigned long *)p, n / sizeof(unsigned long));
        else if (kernel == SBMEM_READ8)
            g_dsink = k_read8((double *)p, n / sizeof(double));
        else
            k_write((unsigned long *)p, n / sizeof(unsigned long),
                    (unsigned long)g_msink | 1L);
    }
    return (double)size;
}

/* sb_mem_run's pre-fill, segment by segment. */
static void seg_fill(kernel, buf, size)
    int kernel;
    char *buf;
    unsigned long size;
{
    unsigned long o, n, di;
    double *dp;

    for (o = 0; o < size; o += SB_SEG) {
        n = size - o;
        if (n > SB_SEG) n = SB_SEG;
        if (kernel == SBMEM_READ8) {
            dp = (double *)MEM_AT(buf, o);
            for (di = 0; di < n / sizeof(double); di++)
                dp[di] = 1.0 + (double)(di & 7L);
        } else if (kernel == SBMEM_READ)
            k_write((unsigned long *)MEM_AT(buf, o),
                    n / sizeof(unsigned long), 3L);
    }
}
#endif

/* One pass of `kernel` over the working set; returns units done (bytes for
 * bandwidth kernels, loads for the chase). */
static double mem_pass(kernel, buf, size, chain)
    int kernel;
    char *buf;
    unsigned long size;
    char *chain;
{
    unsigned long words  = size / sizeof(unsigned long);
    unsigned long dwords = size / sizeof(double);
    unsigned long half   = size / 2;
    unsigned long nslots = size / SB_CHASE_STRIDE;

#ifdef SB_SEGMENTED
    if (size > SB_SEG && kernel != SBMEM_LAT)
        return seg_pass(kernel, buf, size);
#endif
    if (kernel == SBMEM_READ) {
        g_msink = k_read((unsigned long *)buf, words);
        return (double)size;
    }
    if (kernel == SBMEM_READ8) {
        g_dsink = k_read8((double *)buf, dwords);
        return (double)size;
    }
    if (kernel == SBMEM_WRITE) {
        k_write((unsigned long *)buf, words, (unsigned long)g_msink | 1L);
        return (double)size;
    }
    if (kernel == SBMEM_COPY) {
        /* Skew the destination so src and dst never sit exactly size/2
         * apart: for every whole-MB working set, size/2 is a multiple of
         * every direct-mapped cache size in the fleet, so src and dst
         * lines shared a cache index and evicted each other MID-LINE --
         * a SPARCstation 10 (512KB direct-mapped E$) measured 3.8 MB/s
         * where a plain DRAM copy runs ~20.  512 bytes clears the alias
         * for every line size while keeping the copy comfortably inside
         * the buffer; tiny sets (<= 2KB halves) keep the old layout. */
        unsigned long skew = (half > 4L * SB_COPY_SKEW) ? SB_COPY_SKEW : 0L;
        (void)memcpy(buf + half + skew, buf, half - skew);
        return (double)(half - skew);   /* bytes copied, counted once */
    }
    /* SBMEM_LAT: the ring is cyclic, so chasing PAST one lap just goes
     * around again -- still every load dependent on the last.  At small
     * working sets one lap is only a handful of loads (1KB = 16 slots), and
     * the call/dispatch overhead of mem_pass would be charged across those
     * few loads, inflating the reported latency (seen on a 50MHz LX: 1KB
     * read HIGHER than 2KB).  So chase at least ~4K loads per call; the
     * per-call overhead amortizes to nothing.  nslots is a multiple of 8
     * (size >= 1KB, stride 64), and so is the scaled count. */
    {
        unsigned long loads = nslots;
        while (loads < 4096L)
            loads <<= 1;
        g_psink = k_chase(chain, loads);
        return (double)loads;
    }
}

int sb_mem_run(kernel, buf, size, secs, r)
    int kernel;
    char *buf;
    unsigned long size;
    double secs;
    sb_mem_result *r;
{
    unsigned long reps, i;
    double t0, dt, compute, units, u;
    char *chain;

    r->ok = 0;
    r->mb_per_s = 0.0;
    r->ns_per_load = 0.0;
    chain = (char *)0;
#ifdef SB_SEGMENTED
    if (size > SB_SEG && kernel != SBMEM_LAT)
        seg_fill(kernel, buf, size);
    else
#endif
    if (kernel == SBMEM_LAT) {
        chain = build_chain(buf, size);
        if (chain == (char *)0)
            return -1;
    } else if (kernel == SBMEM_READ8) {
        /* Fill with ORDINARY doubles.  An integer fill pattern reinterpreted
         * as doubles is a DENORMAL (~6e-315), and on FPUs that trap specials
         * to kernel emulation (the SS1's Weitek 3170) every add then costs
         * thousands of cycles -- read8 measured ~0 there until this fix. */
        double *dp = (double *)buf;
        unsigned long dw = size / sizeof(double);
        unsigned long di;
        for (di = 0; di < dw; di++)
            dp[di] = 1.0 + (double)(di & 7L);
    } else if (kernel == SBMEM_READ) {
        /* Deterministic content (and touched pages) before reading. */
        k_write((unsigned long *)buf, size / sizeof(unsigned long), 3L);
    }

    /* Calibrate: grow reps (passes per batch) until a batch >= ~50ms
     * (or 40 clock ticks on a coarse clock -- see sb_min_batch). */
    reps = 1L;
    for (;;) {
        t0 = sb_gtod();
        for (i = 0; i < reps; i++)
            (void)mem_pass(kernel, buf, size, chain);
        dt = sb_gtod() - t0;
        if (dt >= sb_min_batch() || reps >= 1000000000L)
            break;
        reps <<= 1;
    }

    compute = 0.0;
    units = 0.0;
    while (compute < secs) {
        t0 = sb_gtod();
        u = 0.0;
        for (i = 0; i < reps; i++)
            u += mem_pass(kernel, buf, size, chain);
        compute += sb_gtod() - t0;
        units += u;
    }

    if (compute <= 0.0)
        return -1;
    if (kernel == SBMEM_LAT)
        r->ns_per_load = compute / units * 1.0e9;
    else
        r->mb_per_s = units / compute / (1024.0 * 1024.0);
    r->ok = 1;
    return 0;
}

#ifndef SB_NOFORK  /* MP mode needs fork(); SBDOS (MS-DOS) has none */
/* --- MP mode: N concurrent readers, each with its own buffer ------------ */

/* Fixed-size record; well under PIPE_BUF, so each child's write is atomic. */
struct memrec { double bytes; double wall; };

static int mem_read_full(fd, cbuf, n)
    int fd; char *cbuf; int n;
{
    int off, k;
    off = 0;
    while (off < n) {
        k = (int)read(fd, cbuf + off, (unsigned)(n - off));
        if (k <= 0) break;
        off += k;
    }
    return off;
}

/* Allocate a private buffer, run one bandwidth kernel for ~secs of
 * compute, report bytes and wall seconds.  Used by the baseline and by
 * every forked child, so all parties are measured identically. */
static int mem_wall(kern, size, secs, bytes_out, wall_out)
    int kern;
    unsigned long size;
    double secs;
    double *bytes_out, *wall_out;
{
    sb_mem_result r;
    char *b;

    *bytes_out = 0.0; *wall_out = 0.0;
    b = sb_alloc_aligned(size);
    if (b == (char *)0)
        return -1;
    if (sb_mem_run(kern, b, size, secs, &r) != 0 || !r.ok) {
        free(b);
        return -1;
    }
    free(b);
    /* Reconstruct bytes/wall from the rate over the nominal window; the
     * ratio is what matters and it is exact. */
    *bytes_out = r.mb_per_s * (1024.0 * 1024.0) * secs;
    *wall_out = secs;
    return 0;
}

#define SB_MEM_MAXCPUS 64

int sb_mem_run_mp(kern, size, secs, ncpus, r)
    int kern;
    unsigned long size;
    double secs;
    int ncpus;
    sb_cpu_mp *r;
{
    struct memrec rec;
    double sb, sw, agg;
    int fds[2];
    int i, got, live;

    r->ok = 0;
    strncpy(r->name, sb_mem_kernel_name(kern), sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    if (ncpus < 1) ncpus = 1;
    if (ncpus > SB_MEM_MAXCPUS) ncpus = SB_MEM_MAXCPUS;

    /* 1. One process alone. */
    if (mem_wall(kern, size, secs, &sb, &sw) != 0)
        return -1;
    r->single_ips = sw > 0.0 ? sb / sw : 0.0;   /* bytes/s in the ips slot */

    /* 2. N processes concurrently, each with a private buffer. */
    if (pipe(fds) != 0)
        return -1;
    live = ncpus;
    for (i = 0; i < ncpus; i++) {
        int pid = fork();
        if (pid == 0) {                          /* child */
            struct memrec out;
            (void)close(fds[0]);
            if (mem_wall(kern, size, secs, &out.bytes, &out.wall) != 0) {
                out.bytes = 0.0; out.wall = 0.0;
            }
            (void)write(fds[1], (char *)&out, sizeof(out));
            (void)close(fds[1]);
            _exit(0);
        }
        if (pid < 0) {                           /* fork failed: run fewer */
            live = i;
            break;
        }
    }
    (void)close(fds[1]);

    agg = 0.0;
    for (got = 0; got < live; got++) {
        if (mem_read_full(fds[0], (char *)&rec, (int)sizeof(rec))
                != (int)sizeof(rec))
            break;
        if (rec.wall > 0.0)
            agg += rec.bytes / rec.wall;
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

/* Chase over a private buffer for ~secs; ns per dependent load, or -1. */
static double lat_wall(size, secs)
    unsigned long size;
    double secs;
{
    sb_mem_result r;
    char *b;
    double ns;

    b = sb_alloc_aligned(size);
    if (b == (char *)0)
        return -1.0;
    if (sb_mem_run(SBMEM_LAT, b, size, secs, &r) != 0 || !r.ok) {
        free(b);
        return -1.0;
    }
    free(b);
    ns = r.ns_per_load;
    return ns > 0.0 ? ns : -1.0;
}

int sb_mem_lat_mp(size, secs, ncpus, r)
    unsigned long size;
    double secs;
    int ncpus;
    sb_cpu_mp *r;
{
    int pids[SB_MEM_MAXCPUS];
    double alone, loaded;
    int i, nstream, live;

    r->ok = 0;
    strncpy(r->name, "lat loaded", sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    if (ncpus < 1) ncpus = 1;
    if (ncpus > SB_MEM_MAXCPUS) ncpus = SB_MEM_MAXCPUS;
    nstream = ncpus - 1;

    /* 1. The chase alone. */
    alone = lat_wall(size, secs);
    if (alone < 0.0)
        return -1;

    /* 2. The same chase while nstream processes stream sequential reads
     * through private buffers.  The streamers run until killed; a short
     * warmup chase lets them ramp before the timed run. */
    live = 0;
    for (i = 0; i < nstream; i++) {
        int pid = fork();
        if (pid == 0) {                          /* child: stream until shot */
            char *b = sb_alloc_aligned(size);
            sb_mem_result sr;
            if (b == (char *)0)
                _exit(1);
            for (;;)
                (void)sb_mem_run(SBMEM_READ, b, size, 1.0, &sr);
            /* NOTREACHED */
        }
        if (pid < 0)
            break;                               /* fork failed: fewer */
        pids[live++] = pid;
    }
    (void)lat_wall(size, secs * 0.25 + 0.1);     /* warmup / ramp */
    loaded = lat_wall(size, secs);
    for (i = 0; i < live; i++)
        (void)kill(pids[i], SIGTERM);
    for (i = 0; i < live; i++)
        (void)wait((int *)0);
    if (loaded < 0.0)
        return -1;

    r->ncpus = live + 1;                         /* chaser + streamers */
    r->single_ips = alone;                       /* ns in the ips slots */
    r->agg_ips = loaded;
    r->speedup = alone > 0.0 ? loaded / alone : 0.0;   /* slowdown factor */
    r->efficiency = -1.0;                        /* scaling n/a for latency */
    r->ok = 1;
    return 0;
}

#endif /* SB_NOFORK */

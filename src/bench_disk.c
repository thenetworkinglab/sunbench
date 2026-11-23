/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * bench_disk.c -- one time-bounded engine for seq/random read/write.
 *
 * Memory discipline: one reusable aligned block buffer, streaming stats,
 * no per-operation sample arrays.  Fits comfortably on a 28MB SS1.
 */
#include "port.h"
#include "bench_disk.h"
#include "diskio.h"
#include "timer.h"

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static char *mode_name(mode)
    int mode;
{
    if (mode & SBM_RAND)
        return (mode & SBM_WRITE) ? "rand write" : "rand read";
    return (mode & SBM_WRITE) ? "seq write" : "seq read";
}

static void fill_result(r, name, bytes, ops, secs)
    sb_result *r;
    char *name;
    double bytes;
    double ops;
    double secs;
{
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    r->bytes    = (unsigned long)bytes;
    r->seconds  = secs;
    r->mb_per_s = secs > 0.0 ? (bytes / (1024.0 * 1024.0)) / secs : 0.0;
    r->iops     = secs > 0.0 ? ops / secs : 0.0;
    r->avg_lat_ms = ops > 0.0 ? (secs / ops) * 1000.0 : 0.0;
    r->ok = 1;
}

/* Private PRNG for random-test offsets: random()/srandom() are BSD
 * functions missing from early Solaris libc (2.3: ucblib only) -- and a
 * local generator means the offset stream never depends on the platform.
 * Same LCG family as bench_mem's chase shuffle; 31-bit positive result. */
static unsigned long dk_state = 1;

static long dk_random()
{
    dk_state = (dk_state * 1664525L + 1013904223L) & 0xffffffffL;
    return (long)((dk_state >> 1) & 0x7fffffffL);
}

/* --- cold-region discipline for the SB_SIZE method ---------------------
 * Without directio the only cache defeat is the file being bigger than
 * RAM -- and short tests silently undo it: a 4-second sweep cell that
 * restarts at offset 0 re-reads exactly what the previous cell just
 * cached (a Solaris 2.3 SS2 "measured" 7.7 MB/s sequential through a
 * 5 MB/s SCSI-1 bus).  Two rules restore cold access without growing
 * the file:
 *   - the sequential position PERSISTS across time-bounded runs
 *     (wrapping), so consecutive cells stream fresh regions and a block
 *     recurs only after a full file tour has evicted it;
 *   - random offsets follow a random PERMUTATION of the blocks
 *     (Fisher-Yates over the same PRNG), so no block repeats within a
 *     tour -- with file > RAM, every revisit is cold by construction.
 * Both engage only for SB_SIZE; SB_DIRECTIO behaviour is unchanged. */
static unsigned long cold_seqblk = 0;
static unsigned long *cold_perm = (unsigned long *)0;
static unsigned long cold_n = 0, cold_i = 0;

static void cold_perm_setup(nblocks)
    unsigned long nblocks;
{
    unsigned long i, j, t;
    if (cold_perm != (unsigned long *)0 && cold_n == nblocks)
        return;
    if (cold_perm != (unsigned long *)0)
        free((char *)cold_perm);
    cold_perm = (unsigned long *)malloc(nblocks * sizeof(unsigned long));
    cold_n = 0; cold_i = 0;
    if (cold_perm == (unsigned long *)0)
        return;                     /* fall back to plain PRNG offsets */
    cold_n = nblocks;
    for (i = 0; i < nblocks; i++)
        cold_perm[i] = i;
    for (i = nblocks - 1; i > 0; i--) {
        j = (unsigned long)dk_random() % (i + 1);
        t = cold_perm[i]; cold_perm[i] = cold_perm[j]; cold_perm[j] = t;
    }
}

int sb_disk_run(path, method, filesz, blocksz, maxsecs, mode, create, r, h, rep)
    char *path;
    int method;
    unsigned long filesz, blocksz;
    double maxsecs;
    int mode, create;
    sb_result *r;
    sb_hist *h;
    sb_reporter *rep;
{
    char *buf, *label;
    int fd, n, is_write, is_rand;
    unsigned long nblocks, ops, seqblk;
    double t0, now, op0, bytes, frac, elapsed;
    double ui_ovh, a, effective;   /* time spent painting the UI, excluded */

    is_write = mode & SBM_WRITE;
    is_rand  = mode & SBM_RAND;
    label = mode_name(mode);
    r->ok = 0;
    if (h) sb_hist_init(h);

    nblocks = filesz / blocksz;
    if (nblocks == 0) {
        fprintf(stderr, "sunbench: file too small for %lu-byte blocks\n",
                blocksz);
        return -1;
    }

    buf = sb_alloc_aligned(blocksz);
    if (buf == (char *)0) { perror("sunbench: valloc"); return -1; }
    if (is_write) memset(buf, 0x5a, (int)blocksz);

    if (is_write && create) fd = sb_open_write(path, method);
    else if (is_write)      fd = sb_open_update(path, method);
    else                    fd = sb_open_read(path, method);
    if (fd < 0) { free(buf); return -1; }

    dk_state = (unsigned long)getpid() | 1L;
    ops = 0; bytes = 0.0; seqblk = 0; ui_ovh = 0.0;
    if (method == SB_SIZE) {
        if (!create)                 /* fresh file needs no purge */
            sb_cache_purge(path, filesz);
        if (is_rand)
            cold_perm_setup(nblocks);
        else if (maxsecs > 0.0)     /* whole-file passes still start at 0 */
            seqblk = cold_seqblk % nblocks;
    }
    t0 = sb_now();
    if (rep && rep->begin) {
        a = sb_now();
        rep->begin(rep->ctx, label, t0);
        ui_ovh += sb_now() - a;     /* don't charge the benchmark for this */
    }

    for (;;) {
        off_t off;

        if (is_rand) {
            unsigned long rb;
            if (method == SB_SIZE && cold_perm != (unsigned long *)0
                && cold_n == nblocks) {
                rb = cold_perm[cold_i];
                cold_i++;
                if (cold_i >= cold_n) cold_i = 0;
            } else
                rb = (unsigned long)(dk_random() % (long)nblocks);
            off = (off_t)rb * (off_t)blocksz;
        } else {
            if (seqblk >= nblocks) {          /* hit EOF */
                if (maxsecs <= 0.0) break;    /* whole-file pass: done */
                seqblk = 0;                   /* time-bounded: wrap around */
            }
            off = (off_t)seqblk * (off_t)blocksz;
            seqblk++;
        }
        if (lseek(fd, off, SEEK_SET) == (off_t)-1) {
            fprintf(stderr, "sunbench: lseek: %s\n", strerror(errno));
            break;
        }

        op0 = sb_now();
        n = is_write ? write(fd, buf, (unsigned)blocksz)
                     : read(fd, buf, (unsigned)blocksz);
        now = sb_now();
        if (n <= 0) {
            if (n < 0)
                fprintf(stderr, "sunbench: %s: %s\n",
                        is_write ? "write" : "read", strerror(errno));
            break;                            /* error, or read EOF */
        }
        if (h) sb_hist_add(h, now - op0);
        ops++;
        bytes += (double)n;

        elapsed = now - t0;
        frac = (maxsecs > 0.0) ? elapsed / maxsecs
                               : bytes / (double)filesz;
        if (rep && rep->tick) {
            /* Live figures use I/O time so far, excluding paint overhead. */
            double eff  = elapsed - ui_ovh;
            double mb_s = eff > 0.0 ? (bytes / (1024.0 * 1024.0)) / eff : 0.0;
            double iops = eff > 0.0 ? (double)ops / eff : 0.0;
            a = sb_now();
            rep->tick(rep->ctx, label, frac, now, mb_s, iops);
            ui_ovh += sb_now() - a;
        }
        if (maxsecs > 0.0 && elapsed >= maxsecs)
            break;
    }

    if (is_write)
#ifdef HAVE_FSYNC
        (void)fsync(fd);       /* the drain IS real I/O -- keep it counted */
#else
        sync();                /* whole-system flush: coarser, same intent */
#endif
    now = sb_now();

    if (method == SB_SIZE && !is_rand && maxsecs > 0.0)
        cold_seqblk = seqblk % nblocks;   /* next run continues cold */
    (void)close(fd);
    free(buf);

    /* Report throughput over I/O time only: subtract the time we spent
     * painting progress/TUI, so a slow serial console can't understate MB/s.
     * Per-op latency (the histogram) never included paint time to begin with. */
    effective = (now - t0) - ui_ovh;
    if (effective < 1.0e-6)
        effective = now - t0;
    fill_result(r, label, bytes, (double)ops, effective);
    if (rep && rep->end) rep->end(rep->ctx, label, now, r);
    return 0;
}

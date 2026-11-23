/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/* bench_disk.h -- disk benchmark module (unified engine). */
#ifndef SUNBENCH_BENCH_DISK_H
#define SUNBENCH_BENCH_DISK_H
#include "port.h"
#include "histogram.h"

/* One line of results.  Kept tiny -- we NEVER store per-op samples
 * (a latency run emits millions; on a 28MB SPARCstation that matters).
 * Averages are accumulated in streaming fashion instead. */
typedef struct sb_result {
    char          name[24];
    double        mb_per_s;
    double        iops;
    double        avg_lat_ms;
    unsigned long bytes;
    double        seconds;
    int           ok;
} sb_result;

/* Mode bits for sb_disk_run. */
#define SBM_WRITE 1     /* else read  */
#define SBM_RAND  2     /* else sequential */

/*
 * Progress observer.  sb_disk_run drives these callbacks so the same engine
 * feeds either the plain stderr progress lines or the live TUI.  Any pointer
 * may be NULL.  ctx is opaque per-reporter state.  Throttling (how often to
 * actually paint) is the reporter's business, not the engine's.
 */
typedef struct sb_reporter {
    char *ctx;
    void (*begin) P_((char *ctx, char *label, double t0));
    void (*tick)  P_((char *ctx, char *label, double frac, double now,
                      double mb_s, double iops));
    void (*end)   P_((char *ctx, char *label, double now, sb_result *r));
} sb_reporter;

/*
 * One engine for all four tests.  Sizes are in BYTES.
 *   maxsecs > 0  : run for that many wall seconds (required for random;
 *                  sequential wraps at EOF).  Emits progress + ETA.
 *   maxsecs == 0 : one whole-file sequential pass.
 *   create != 0  : open with create+truncate (the first write pass);
 *                  otherwise writes update the existing file in place.
 *   h            : filled with the per-op latency distribution (may be NULL).
 * Fills *r.  Returns 0 on success, -1 on error.
 */
int sb_disk_run P_((char *path, int method, unsigned long filesz,
                    unsigned long blocksz, double maxsecs, int mode,
                    int create, sb_result *r, sb_hist *h, sb_reporter *rep));

#endif

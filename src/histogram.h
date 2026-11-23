/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * histogram.h -- latency distribution in fixed log2 buckets.
 *
 * An average hides the tail; on the SS1 a random op averages ~98ms but is
 * actually bimodal (~33ms buffer hits vs ~131ms misses).  We keep counters
 * plus exact min/max/sum.  Buckets are log2 with SB_SUB linear sub-buckets
 * per octave (HdrHistogram-style), so ~33ms and ~131ms land in clearly
 * distinct bins and percentiles are trustworthy.  Still a couple hundred
 * bytes total -- no per-op sample arrays, so a million-op run is free.
 */
#ifndef SUNBENCH_HISTOGRAM_H
#define SUNBENCH_HISTOGRAM_H
#include "port.h"
#include <stdio.h>

#define SB_SUB      4                    /* sub-buckets per octave */
#define SB_OCTAVES  32
#define SB_NBUCKETS (SB_SUB * SB_OCTAVES)

typedef struct sb_hist {
    unsigned long count[SB_NBUCKETS];
    unsigned long n;
    double        min_s;
    double        max_s;
    double        sum_s;
} sb_hist;

void   sb_hist_init P_((sb_hist *h));
void   sb_hist_add  P_((sb_hist *h, double secs));
double sb_hist_mean P_((sb_hist *h));            /* seconds */
double sb_hist_pct  P_((sb_hist *h, double frac)); /* frac 0..1 -> seconds */
void   sb_hist_bars P_((sb_hist *h, char *label, FILE *out)); /* bar dump */
double sb_hist_bucket_low_ms P_((int b));   /* bucket lower edge, ms */

#endif

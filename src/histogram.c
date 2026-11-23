/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * histogram.c -- log2-bucketed latency stats.  No math.h (K&R friendly);
 * bucket index is found by shifting.
 */
#include "port.h"
#include "histogram.h"

#include <stdio.h>

void sb_hist_init(h)
    sb_hist *h;
{
    int i;
    for (i = 0; i < SB_NBUCKETS; i++)
        h->count[i] = 0;
    h->n = 0;
    h->min_s = 0.0;
    h->max_s = 0.0;
    h->sum_s = 0.0;
}

/*
 * Bucket index = octave*SB_SUB + sub, where octave e = floor(log2(us)) and
 * sub in [0,SB_SUB) is the linear position within [2^e, 2^(e+1)) us.
 */
static int bucket_of(secs)
    double secs;
{
    double us, base;
    unsigned long u;
    int e, sub, idx;

    us = secs * 1.0e6;
    if (us < 1.0)
        return 0;
    u = (unsigned long)us;
    e = 0;
    while ((u >> e) > 1L && e < SB_OCTAVES - 1)
        e++;                            /* e = floor(log2(u)) */
    base = (double)(1L << e);          /* 2^e microseconds */
    sub = (int)((us - base) * (double)SB_SUB / base);
    if (sub < 0)        sub = 0;
    if (sub >= SB_SUB)  sub = SB_SUB - 1;
    idx = e * SB_SUB + sub;
    if (idx >= SB_NBUCKETS) idx = SB_NBUCKETS - 1;
    return idx;
}

/* Lower/upper edge of bucket idx, in seconds. */
static double bucket_low_s(idx)
    int idx;
{
    int e, sub;
    double base;
    e = idx / SB_SUB;
    sub = idx % SB_SUB;
    base = (double)(1L << e);
    return (base + base * (double)sub / (double)SB_SUB) / 1.0e6;
}

void sb_hist_add(h, secs)
    sb_hist *h;
    double secs;
{
    if (secs < 0.0)
        secs = 0.0;
    if (h->n == 0 || secs < h->min_s) h->min_s = secs;
    if (h->n == 0 || secs > h->max_s) h->max_s = secs;
    h->sum_s += secs;
    h->count[bucket_of(secs)]++;
    h->n++;
}

double sb_hist_mean(h)
    sb_hist *h;
{
    return h->n ? h->sum_s / (double)h->n : 0.0;
}

/* Upper edge of bucket b == lower edge of the next bucket. */
static double bucket_top_s(b)
    int b;
{
    return bucket_low_s(b + 1);
}

double sb_hist_pct(h, frac)
    sb_hist *h;
    double frac;
{
    unsigned long target, cum;
    int b;

    if (h->n == 0)
        return 0.0;
    target = (unsigned long)(frac * (double)h->n);
    if (target == 0)
        target = 1;
    cum = 0;
    for (b = 0; b < SB_NBUCKETS; b++) {
        cum += h->count[b];
        if (cum >= target) {
            /* Don't report a bound past the real max. */
            double top = bucket_top_s(b);
            return top < h->max_s ? top : h->max_s;
        }
    }
    return h->max_s;
}

void sb_hist_bars(h, label, out)
    sb_hist *h;
    char *label;
    FILE *out;
{
    int b, lo, hi, i, len;
    unsigned long peak;

    if (h->n == 0)
        return;
    /* Trim to the populated range. */
    lo = 0; while (lo < SB_NBUCKETS && h->count[lo] == 0) lo++;
    hi = SB_NBUCKETS - 1; while (hi > lo && h->count[hi] == 0) hi--;
    peak = 1;
    for (b = lo; b <= hi; b++)
        if (h->count[b] > peak) peak = h->count[b];

    fprintf(out, "  %s latency distribution (%lu ops):\n", label, h->n);
    for (b = lo; b <= hi; b++) {
        /* Bucket lower edge in ms for readability. */
        double lo_ms = bucket_low_s(b) * 1000.0;
        len = (int)((double)h->count[b] * 40.0 / (double)peak);
        fprintf(out, "  %8.2f ms |", lo_ms);
        for (i = 0; i < len; i++) putc('#', out);
        fprintf(out, " %lu\n", h->count[b]);
    }
}

double sb_hist_bucket_low_ms(b)
    int b;
{
    return bucket_low_s(b) * 1000.0;
}

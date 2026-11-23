/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * progress.h -- throttled progress lines for slow runs.
 *
 * On a ~10 IOPS SPARCstation a run takes minutes; without feedback you
 * can't tell if it's working.  We print a newline-terminated line to
 * stderr every few seconds: label, percent done, and an ETA.  Newline
 * (not \r) keeps a tee'd log readable.
 */
#ifndef SUNBENCH_PROGRESS_H
#define SUNBENCH_PROGRESS_H
#include "port.h"

typedef struct sb_prog {
    char   label[24];
    double t0;
    double next;      /* wall time of next allowed print */
    double interval;  /* seconds between prints */
    int    on;
} sb_prog;

void sb_prog_quiet P_((int on));    /* --quiet: silence all progress lines */
void sb_prog_begin P_((sb_prog *p, char *label, double t0, int on));
void sb_prog_tick  P_((sb_prog *p, double frac, double now)); /* frac 0..1 */
void sb_prog_end   P_((sb_prog *p, double now));

/* Fill *rep with the plain (stderr progress-line) reporter. */
struct sb_reporter;
void sb_plain_reporter P_((struct sb_reporter *rep));

#endif

/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/* progress.c -- see progress.h.  Also provides the "plain" reporter. */
#include "port.h"
#include "progress.h"
#include "bench_disk.h"

#include <stdio.h>
#include <string.h>

/* --quiet: suppress all progress lines from plain reporters. */
static int g_prog_quiet = 0;

void sb_prog_quiet(on)
    int on;
{
    g_prog_quiet = on;
}

void sb_prog_begin(p, label, t0, on)
    sb_prog *p;
    char *label;
    double t0;
    int on;
{
    if (g_prog_quiet) on = 0;
    strncpy(p->label, label, sizeof(p->label) - 1);
    p->label[sizeof(p->label) - 1] = '\0';
    p->t0 = t0;
    p->interval = 4.0;      /* a line every ~4s: enough to reassure, not spam */
    p->next = t0 + p->interval;
    p->on = on;
}

void sb_prog_tick(p, frac, now)
    sb_prog *p;
    double frac;
    double now;
{
    double elapsed, eta;

    if (!p->on || now < p->next)
        return;
    if (frac < 0.0001) frac = 0.0001;
    if (frac > 1.0)    frac = 1.0;
    elapsed = now - p->t0;
    eta = elapsed / frac - elapsed;   /* remaining = total_est - elapsed */
    fprintf(stderr, "    %-10s %3d%%   ~%3.0fs left\n",
            p->label, (int)(frac * 100.0 + 0.5), eta);
    (void)fflush(stderr);
    p->next = now + p->interval;
}

void sb_prog_end(p, now)
    sb_prog *p;
    double now;
{
    if (!p->on)
        return;
    fprintf(stderr, "    %-10s done in %.1fs\n", p->label, now - p->t0);
    (void)fflush(stderr);
}

/* ---- plain reporter: the throttled stderr progress lines ---------------- */

static sb_prog g_plain;

static void plain_begin(ctx, label, t0)
    char *ctx; char *label; double t0;
{
    (void)ctx;
    sb_prog_begin(&g_plain, label, t0, 1);
}

static void plain_tick(ctx, label, frac, now, mb_s, iops)
    char *ctx; char *label; double frac, now, mb_s, iops;
{
    (void)ctx; (void)label; (void)mb_s; (void)iops;
    sb_prog_tick(&g_plain, frac, now);
}

static void plain_end(ctx, label, now, r)
    char *ctx; char *label; double now; sb_result *r;
{
    (void)ctx; (void)label; (void)r;
    sb_prog_end(&g_plain, now);
}

void sb_plain_reporter(rep)
    sb_reporter *rep;
{
    rep->ctx   = (char *)0;
    rep->begin = plain_begin;
    rep->tick  = plain_tick;
    rep->end   = plain_end;
}

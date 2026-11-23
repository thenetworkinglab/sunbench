/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * tui.c -- live ANSI/vt100 dashboard for the four-test disk run.
 *
 * Layout is fixed and repainted from the home position each frame (not a
 * full clear), with each line terminated by ESC[K so stale text is wiped
 * without flicker on slow serial consoles.
 */
#include "port.h"
#include "version.h"
#include "tui.h"
#include "timer.h"
#include "sysinfo.h"
#include "cpu.h"
#include "whet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>     /* leave the alternate screen on Ctrl-C */

#define TUI_MAX   8
#define BARW      16      /* progress bar cells */
#define PENDING   0
#define RUNNING   1
#define DONE      2

typedef struct {
    char   label[16];
    int    state;
    double frac;
    double mb_s;
    double iops;
    double lat_ms;
    double t0;
} tui_row;

static tui_row rows[TUI_MAX];
static int     nrows;
static char    hdr_method[80];
static char    hdr_file[128];
static double  last_draw;

/* --- terminal helpers (raw escape sequences) ---
 *
 * The dashboards paint inside the xterm ALTERNATE SCREEN (ESC[?1049h/l,
 * what vi and less use), so quitting restores the user's scrollback
 * instead of leaving it clobbered -- important over telnet.  Terminals
 * that don't know the sequence (real vt100s, the Sun console) ignore it
 * and get exactly the old behaviour.  Because the alternate screen is
 * discarded on exit, each *_finish() leaves it and repaints the final
 * frame PLAINLY into the normal screen: t_live gates every escape, so
 * the same draw functions serve both modes. */
static int  t_live = 1;   /* 0 while dumping a plain copy of a final frame */
static int  t_alt  = 0;   /* alternate screen currently active */
static void home()      { if (t_live) fputs("\033[H", stdout); }
static void clr_eol()   { if (t_live) fputs("\033[K", stdout); }
static void clr_below() { if (t_live) fputs("\033[J", stdout); }
static void clr_all()   { if (t_live) fputs("\033[2J", stdout); }

/* Ctrl-C during a dashboard: leave the alternate screen before dying, or
 * the user's shell would be stranded in it.  (The menu's raw-mode handler
 * in keys.c does its own cleanup and also calls sb_tui_alt_leave.) */
static void alt_intr(sig)
    int sig;
{
    fputs("\033[?1049l", stdout);
    (void)fflush(stdout);
    (void)signal(sig, SIG_DFL);
    _exit(1);
}

static void alt_enter()
{
    if (!t_alt) {
        fputs("\033[?1049h", stdout);
        t_alt = 1;
        (void)signal(SIGINT, alt_intr);
    }
}

void sb_tui_alt_leave()
{
    if (t_alt) {
        fputs("\033[?1049l", stdout);
        t_alt = 0;
        (void)fflush(stdout);
    }
}

int sb_tui_supported()
{
    char *term;
    if (!isatty(fileno(stdout)))
        return 0;
    term = getenv("TERM");
    if (term == (char *)0 || term[0] == '\0')
        return 0;
    if (strcmp(term, "dumb") == 0)
        return 0;
    return 1;
}

/* --- machine synopsis, painted into every dashboard header so a screenshot
 *     or copy-paste is self-describing (same content as the --log preamble). */
static char g_sys[SB_SYSLINES][SB_SYSLINE];
static int  g_sysn = -1;        /* -1 = not gathered yet */

void sb_tui_sysinfo(path)       /* gather once; probes (iostat/psrinfo) are slow */
    char *path;
{
    if (g_sysn < 0)
        g_sysn = sb_sysinfo_lines(path, g_sys, SB_SYSLINES);
}

/* Paint the cached synopsis.  disk_ok == 0 skips the scratch-disk lines,
 * which are irrelevant on the CPU view.  Each line is cleared to EOL. */
static void sys_paint(disk_ok)
    int disk_ok;
{
    int i;
    for (i = 0; i < g_sysn; i++) {
        if (!disk_ok &&
            (strncmp(g_sys[i], "  disk", 6) == 0 ||
             strncmp(g_sys[i], "  drive", 7) == 0))
            continue;
        fputs(g_sys[i], stdout);
        clr_eol(); fputs("\n", stdout);
    }
}

void sb_tui_start(method, file_desc)
    char *method, *file_desc;
{
    strncpy(hdr_method, method, sizeof(hdr_method) - 1);
    hdr_method[sizeof(hdr_method) - 1] = '\0';
    strncpy(hdr_file, file_desc, sizeof(hdr_file) - 1);
    hdr_file[sizeof(hdr_file) - 1] = '\0';
    nrows = 0;
    last_draw = 0.0;
    alt_enter();
    clr_all();
    home();
    (void)fflush(stdout);
}

void sb_tui_add(label)
    char *label;
{
    tui_row *r;
    if (nrows >= TUI_MAX)
        return;
    r = &rows[nrows++];
    strncpy(r->label, label, sizeof(r->label) - 1);
    r->label[sizeof(r->label) - 1] = '\0';
    r->state = PENDING;
    r->frac = r->mb_s = r->iops = r->lat_ms = r->t0 = 0.0;
}

static tui_row *find(label)
    char *label;
{
    int i;
    for (i = 0; i < nrows; i++)
        if (strcmp(rows[i].label, label) == 0)
            return &rows[i];
    return (tui_row *)0;
}

/* Build the 24-wide progress/status field for a (state, frac) into buf. */
static void bar_field(buf, state, frac)
    char *buf;
    int state;
    double frac;
{
    char *p = buf;
    int i, n;

    if (state == PENDING) {
        strcpy(buf, "waiting");
        return;
    }
    if (state == DONE) {
        strcpy(buf, "done");
        return;
    }
    *p++ = '[';
    n = (int)(frac * (double)BARW + 0.5);
    if (n > BARW) n = BARW;
    for (i = 0; i < BARW; i++)
        *p++ = (i < n) ? '#' : '.';
    *p++ = ']';
    sprintf(p, " %3d%%", (int)(frac * 100.0 + 0.5));
}

static void field(buf, r)
    char *buf;
    tui_row *r;
{
    bar_field(buf, r->state, r->frac);
}

static void draw()
{
    int i;
    char fbuf[40];
    tui_row *r;

    home();
    printf("  sunbench %s -- disk benchmark", SB_VERSION);
    clr_eol(); fputs("\n", stdout);
    fputs("  ==========================================", stdout);
    clr_eol(); fputs("\n", stdout);
    sys_paint(1);
    printf("  method : %s", hdr_method); clr_eol(); fputs("\n", stdout);
    printf("  file   : %s", hdr_file);   clr_eol(); fputs("\n", stdout);
    clr_eol(); fputs("\n", stdout);
    printf("  %-11s %-24s %8s %8s %7s", "test", "progress", "MB/s", "IOPS", "lat");
    clr_eol(); fputs("\n", stdout);

    for (i = 0; i < nrows; i++) {
        r = &rows[i];
        field(fbuf, r);
        if (r->state == PENDING)
            printf("  %-11s %-24s", r->label, fbuf);
        else
            printf("  %-11s %-24s %8.2f %8.1f %5.1fms",
                   r->label, fbuf, r->mb_s, r->iops, r->lat_ms);
        clr_eol(); fputs("\n", stdout);
    }
    clr_eol(); fputs("\n", stdout);
    clr_below();
    (void)fflush(stdout);
}

/* --- reporter callbacks --- */

static void tui_begin(ctx, label, t0)
    char *ctx; char *label; double t0;
{
    tui_row *r;
    (void)ctx;
    r = find(label);
    if (r) { r->state = RUNNING; r->t0 = t0; r->frac = 0.0; }
    draw();
    last_draw = t0;
}

static void tui_tick(ctx, label, frac, now, mb_s, iops)
    char *ctx; char *label; double frac, now, mb_s, iops;
{
    tui_row *r;
    (void)ctx;
    r = find(label);
    if (!r) return;
    r->frac = frac;
    r->mb_s = mb_s;
    r->iops = iops;
    r->lat_ms = iops > 0.0 ? 1000.0 / iops : 0.0;
    if (now - last_draw >= 0.4) {       /* throttle repaints */
        draw();
        last_draw = now;
    }
}

static void tui_end(ctx, label, now, res)
    char *ctx; char *label; double now; sb_result *res;
{
    tui_row *r;
    (void)ctx;
    r = find(label);
    if (r) {
        r->state = DONE;
        r->frac = 1.0;
        r->mb_s = res->mb_per_s;
        r->iops = res->iops;
        r->lat_ms = res->avg_lat_ms;
    }
    draw();
    last_draw = now;
}

void sb_tui_reporter(rep)
    sb_reporter *rep;
{
    rep->ctx   = (char *)0;
    rep->begin = tui_begin;
    rep->tick  = tui_tick;
    rep->end   = tui_end;
}

void sb_tui_finish()
{
    draw();
    if (t_alt) {                /* back to the real screen, then a plain
                                 * copy of the final table for scrollback */
        sb_tui_alt_leave();
        t_live = 0;
        draw();
        t_live = 1;
    }
    /* Cursor is already just below the table (draw ends there); add a
     * blank line so following text / the shell prompt is clear. */
    fputs("\n", stdout);
    (void)fflush(stdout);
}

void sb_tui_menu_draw(items, n, sel, hint)
    char **items;
    int n, sel;
    char *hint;
{
    int i;

    if (!t_alt) {               /* first draw after start or a module run */
        alt_enter();
        clr_all();
    }
    home();
    printf("  sunbench %s", SB_VERSION);      clr_eol(); fputs("\n", stdout);
    fputs("  =========================", stdout); clr_eol(); fputs("\n", stdout);
    sys_paint(0);
    clr_eol(); fputs("\n", stdout);
    fputs("  Choose a benchmark  (Up/Down, Enter -- q quits):", stdout);
    clr_eol(); fputs("\n", stdout);
    clr_eol(); fputs("\n", stdout);
    for (i = 0; i < n; i++) {
        if (items[i][0] == '\0') {              /* separator: blank line */
            clr_eol(); fputs("\n", stdout);
            continue;
        }
        if (i == sel)
            printf("   \033[7m  %-46s \033[0m", items[i]); /* reverse */
        else
            printf("     %-46s ", items[i]);
        clr_eol(); fputs("\n", stdout);
    }
    clr_eol(); fputs("\n", stdout);
    if (hint) { printf("  %s", hint); clr_eol(); fputs("\n", stdout); }
    clr_below();
    (void)fflush(stdout);
}

void sb_tui_prompt(msg)
    char *msg;
{
    printf("\n  %s", msg);
    clr_eol();
    (void)fflush(stdout);
}

/* ======================= block-size sweep dashboard ===================== */

#define SW_MAX 12

typedef struct {
    unsigned long kb;
    int    state;
    double v[5];        /* seqW, seqR, randR, randW, randR-IOPS; <0 = unknown */
} sw_row;

static sw_row sw_rows[SW_MAX];
static int    sw_n, sw_cur, sw_preparing;
static char   sw_method[80], sw_file[128], sw_label[16];
static double sw_frac, sw_t0, sw_now, sw_last;

static void sweep_draw()
{
    int i, j, pct;
    double elapsed, eta;
    sw_row *R;

    home();
    printf("  sunbench %s -- block-size sweep", SB_VERSION);
    clr_eol(); fputs("\n", stdout);
    fputs("  ==========================================", stdout);
    clr_eol(); fputs("\n", stdout);
    sys_paint(1);
    printf("  method : %s", sw_method); clr_eol(); fputs("\n", stdout);
    printf("  file   : %s", sw_file);   clr_eol(); fputs("\n", stdout);
    clr_eol(); fputs("\n", stdout);
    printf("   %7s %8s %8s %8s %8s %10s",
           "blk(KB)", "seqW", "seqR", "randR", "randW", "rndR-IOPS");
    clr_eol(); fputs("\n", stdout);

    for (i = 0; i < sw_n; i++) {
        R = &sw_rows[i];
        printf("  %c%7lu", (i == sw_cur) ? '>' : ' ', R->kb);
        for (j = 0; j < 4; j++) {
            if (R->v[j] < 0.0) printf(" %8s", "-");
            else               printf(" %8.2f", R->v[j]);
        }
        if (R->v[4] < 0.0) printf(" %10s", "-");
        else               printf(" %10.1f", R->v[4]);
        clr_eol(); fputs("\n", stdout);
    }
    clr_eol(); fputs("\n", stdout);

    elapsed = sw_now - sw_t0;
    eta = (sw_frac > 0.001) ? elapsed / sw_frac - elapsed : 0.0;
    pct = (int)(sw_frac * 100.0 + 0.5);
    if (sw_preparing)
        printf("  preparing test file   %3d%%  ~%3.0fs left", pct, eta);
    else if (sw_cur >= 0 && sw_cur < sw_n)
        printf("  block %lu KB  (%d/%d)   %-10s %3d%%  ~%3.0fs left",
               sw_rows[sw_cur].kb, sw_cur + 1, sw_n, sw_label, pct, eta);
    else
        printf("  complete.");
    clr_eol(); fputs("\n", stdout);
    clr_below();
    (void)fflush(stdout);
}

void sb_sweep_start(method, fdesc, kbs, n)
    char *method, *fdesc;
    unsigned long *kbs;
    int n;
{
    int i, j;
    strncpy(sw_method, method, sizeof(sw_method) - 1);
    sw_method[sizeof(sw_method) - 1] = '\0';
    strncpy(sw_file, fdesc, sizeof(sw_file) - 1);
    sw_file[sizeof(sw_file) - 1] = '\0';
    if (n > SW_MAX) n = SW_MAX;
    sw_n = n;
    for (i = 0; i < n; i++) {
        sw_rows[i].kb = kbs[i];
        sw_rows[i].state = PENDING;
        for (j = 0; j < 5; j++) sw_rows[i].v[j] = -1.0;
    }
    sw_cur = -1; sw_preparing = 0; sw_label[0] = '\0';
    sw_frac = sw_t0 = sw_now = sw_last = 0.0;
    alt_enter();
    clr_all();
    sweep_draw();
}

void sb_sweep_preparing(on)
    int on;
{
    sw_preparing = on;
    sw_cur = -1;
    sweep_draw();
}

void sb_sweep_cell(i)
    int i;
{
    sw_preparing = 0;
    sw_cur = i;
    if (i >= 0 && i < sw_n) sw_rows[i].state = RUNNING;
    sw_label[0] = '\0';
    sw_frac = 0.0;
    sweep_draw();
}

static void sweep_store(label, res)
    char *label; sb_result *res;
{
    sw_row *R;
    if (sw_cur < 0 || sw_cur >= sw_n) return;   /* prep phase: nothing to store */
    R = &sw_rows[sw_cur];
    if (strcmp(label, "seq write") == 0)      R->v[0] = res->mb_per_s;
    else if (strcmp(label, "seq read") == 0)  R->v[1] = res->mb_per_s;
    else if (strcmp(label, "rand read") == 0) { R->v[2] = res->mb_per_s;
                                                R->v[4] = res->iops; }
    else if (strcmp(label, "rand write") == 0) { R->v[3] = res->mb_per_s;
                                                 R->state = DONE; }
}

static void sweep_begin(ctx, label, t0)
    char *ctx; char *label; double t0;
{
    (void)ctx;
    strncpy(sw_label, label, sizeof(sw_label) - 1);
    sw_label[sizeof(sw_label) - 1] = '\0';
    sw_t0 = t0; sw_now = t0; sw_frac = 0.0;
    sweep_draw();
    sw_last = t0;
}

static void sweep_tick(ctx, label, frac, now, mb_s, iops)
    char *ctx; char *label; double frac, now, mb_s, iops;
{
    (void)ctx; (void)label; (void)mb_s; (void)iops;
    sw_frac = frac; sw_now = now;
    if (now - sw_last >= 0.4) { sweep_draw(); sw_last = now; }
}

static void sweep_end(ctx, label, now, res)
    char *ctx; char *label; double now; sb_result *res;
{
    (void)ctx;
    sweep_store(label, res);
    sw_now = now; sw_frac = 1.0;
    sweep_draw();
    sw_last = now;
}

void sb_sweep_reporter(rep)
    sb_reporter *rep;
{
    rep->ctx   = (char *)0;
    rep->begin = sweep_begin;
    rep->tick  = sweep_tick;
    rep->end   = sweep_end;
}

void sb_sweep_finish()
{
    sw_cur = -1; sw_preparing = 0;
    sweep_draw();
    if (t_alt) {
        sb_tui_alt_leave();
        t_live = 0;
        sweep_draw();
        t_live = 1;
    }
    fputs("\n", stdout);
    (void)fflush(stdout);
}

int sb_sweep_export(kbout, vout, iopsout, max)
    unsigned long *kbout;
    double (*vout)[4];
    double *iopsout;
    int max;
{
    int i, j, n;
    n = (sw_n < max) ? sw_n : max;
    for (i = 0; i < n; i++) {
        kbout[i] = sw_rows[i].kb;
        for (j = 0; j < 4; j++)
            vout[i][j] = sw_rows[i].v[j];
        iopsout[i] = sw_rows[i].v[4];
    }
    return n;
}

void sb_sweep_write_log(out)
    FILE *out;
{
    int i, j;
    sw_row *R;
    fprintf(out, "sunbench -- disk block-size sweep\n");
    fprintf(out, "  method : %s\n", sw_method);
    fprintf(out, "  file   : %s\n", sw_file);
    fprintf(out, "  %8s %8s %8s %8s %8s %10s\n",
            "blk(KB)", "seqW", "seqR", "randR", "randW", "rndR-IOPS");
    for (i = 0; i < sw_n; i++) {
        R = &sw_rows[i];
        fprintf(out, "  %8lu", R->kb);
        for (j = 0; j < 4; j++) {
            if (R->v[j] < 0.0) fprintf(out, " %8s", "-");
            else               fprintf(out, " %8.2f", R->v[j]);
        }
        if (R->v[4] < 0.0) fprintf(out, " %10s", "-");
        else               fprintf(out, " %10.1f", R->v[4]);
        fprintf(out, "\n");
    }
}

/* ========================== CPU live dashboard ========================== */

#define CT_MAX 10

typedef struct {
    char   label[16];
    int    kern;        /* SBK_* -- selects the display unit */
    int    state;
    double frac;
    double ips;         /* running rate during a run; final rate when DONE */
} ct_row;

static ct_row ct_rows[CT_MAX];
static int    ct_n;
static char   ct_build[96];
static char   ct_clock[32];
static unsigned long ct_mhz;    /* 0 = unknown; enables the per-clock figure */
static double ct_last;

/* Format one kernel's rate cell in its native unit, matching the plain report:
 * Dhrystone adds VAX-MIPS, Whetstone is MWIPS, the custom kernels add a
 * per-clock figure when the clock is known. */
static void ct_fmt(buf, kern, ips)
    char *buf; int kern; double ips;
{
    if (kern == SBK_DHRYSTONE)
        sprintf(buf, "%.0f Dhrystones/s  %.2f VAX-MIPS", ips, ips / 1757.0);
    else if (kern == SBK_WHETSTONE)
        sprintf(buf, "%.2f MWIPS", ips * WHET_INSTR_PER_LOOP / 1.0e6);
    else if (SBK_IS_XPY(kern))
        sprintf(buf, "%.2f MFLOPS", ips * sb_cpu_xpy_flops(kern) / 1.0e6);
    else {
        /* sprintf-then-strlen: SunOS 4 sprintf returns char *, not int */
        char *p = buf;
        (void)sprintf(p, "%9.3f Miter/s", ips / 1.0e6);
        p += strlen(p);
        if (ct_mhz)
            (void)sprintf(p, "   %.4f/clk", ips / ((double)ct_mhz * 1.0e6));
    }
}

static ct_row *ct_find(label)
    char *label;
{
    int i;
    for (i = 0; i < ct_n; i++)
        if (strcmp(ct_rows[i].label, label) == 0)
            return &ct_rows[i];
    return (ct_row *)0;
}

static void ct_draw()
{
    int i;
    char fbuf[40], rbuf[64];    /* rbuf: dhrystone row can exceed 40 chars
                                 * on fast (modern build-test) machines */
    ct_row *r;

    home();
    printf("  sunbench %s -- cpu benchmark", SB_VERSION);
    clr_eol(); fputs("\n", stdout);
    fputs("  ==========================================", stdout);
    clr_eol(); fputs("\n", stdout);
    sys_paint(0);
    printf("  build  : %s", ct_build); clr_eol(); fputs("\n", stdout);
    if (ct_clock[0]) { printf("  clock  : %s", ct_clock);
                       clr_eol(); fputs("\n", stdout); }
    clr_eol(); fputs("\n", stdout);
    printf("  %-11s %-24s %s", "kernel", "progress", "rate");
    clr_eol(); fputs("\n", stdout);

    for (i = 0; i < ct_n; i++) {
        r = &ct_rows[i];
        bar_field(fbuf, r->state, r->frac);
        if (r->state == PENDING)
            printf("  %-11s %-24s", r->label, fbuf);
        else {
            ct_fmt(rbuf, r->kern, r->ips);
            printf("  %-11s %-24s %s", r->label, fbuf, rbuf);
        }
        clr_eol(); fputs("\n", stdout);
    }
    clr_eol(); fputs("\n", stdout);
    clr_below();
    (void)fflush(stdout);
}

void sb_cpu_tui_start(build, mhz)
    char *build; unsigned long mhz;
{
    strncpy(ct_build, build, sizeof(ct_build) - 1);
    ct_build[sizeof(ct_build) - 1] = '\0';
    ct_mhz = mhz;
    if (mhz) sprintf(ct_clock, "%lu MHz", mhz);
    else     ct_clock[0] = '\0';
    ct_n = 0;
    ct_last = 0.0;
    alt_enter();
    clr_all();
    home();
    (void)fflush(stdout);
}

void sb_cpu_tui_add(label, kern)
    char *label; int kern;
{
    ct_row *r;
    if (ct_n >= CT_MAX) return;
    r = &ct_rows[ct_n++];
    strncpy(r->label, label, sizeof(r->label) - 1);
    r->label[sizeof(r->label) - 1] = '\0';
    r->kern = kern;
    r->state = PENDING;
    r->frac = 0.0;
    r->ips = 0.0;
}

/* --- reporter callbacks (res is NULL for CPU; never dereferenced) --- */

static void ctui_begin(ctx, label, t0)
    char *ctx; char *label; double t0;
{
    ct_row *r;
    (void)ctx;
    r = ct_find(label);
    if (r) { r->state = RUNNING; r->frac = 0.0; }
    ct_draw();
    ct_last = t0;
}

static void ctui_tick(ctx, label, frac, now, mb_s, ips)
    char *ctx; char *label; double frac, now, mb_s, ips;
{
    ct_row *r;
    (void)ctx; (void)mb_s;
    r = ct_find(label);
    if (!r) return;
    r->frac = frac;
    r->ips = ips;                       /* wall-clock running estimate */
    if (now - ct_last >= 0.4) { ct_draw(); ct_last = now; }
}

static void ctui_end(ctx, label, now, res)
    char *ctx; char *label; double now; sb_result *res;
{
    ct_row *r;
    (void)ctx; (void)res;               /* CPU passes no sb_result */
    r = ct_find(label);
    if (r) { r->state = DONE; r->frac = 1.0; }
    ct_draw();
    ct_last = now;
}

void sb_cpu_tui_reporter(rep)
    sb_reporter *rep;
{
    rep->ctx   = (char *)0;
    rep->begin = ctui_begin;
    rep->tick  = ctui_tick;
    rep->end   = ctui_end;
}

/* Overwrite a finished row with the authoritative CPU-time rate (the live
 * ticks show a wall-clock estimate; the real figure is measured post-loop). */
void sb_cpu_tui_set(kern, ips)
    int kern; double ips;
{
    int i;
    for (i = 0; i < ct_n; i++)
        if (ct_rows[i].kern == kern) {
            ct_rows[i].state = DONE;
            ct_rows[i].frac = 1.0;
            ct_rows[i].ips = ips;
            break;
        }
    ct_draw();
}

void sb_cpu_tui_finish()
{
    ct_draw();
    if (t_alt) {
        sb_tui_alt_leave();
        t_live = 0;
        ct_draw();
        t_live = 1;
    }
    fputs("\n", stdout);
    (void)fflush(stdout);
}

/* ================== memory working-set sweep dashboard ================== */

#define MM_MAX 20
#define MM_COLS 5           /* read, read8, write, copy (MB/s), lat (ns) */

static unsigned long mm_kb[MM_MAX];
static double mm_v[MM_MAX][MM_COLS];
static int    mm_n, mm_cur;
static char   mm_desc[96];

static void memtui_draw()
{
    int i, k;

    home();
    printf("  sunbench %s -- memory working-set sweep", SB_VERSION);
    clr_eol(); fputs("\n", stdout);
    fputs("  ==========================================", stdout);
    clr_eol(); fputs("\n", stdout);
    sys_paint(0);
    printf("  set    : %s", mm_desc); clr_eol(); fputs("\n", stdout);
    clr_eol(); fputs("\n", stdout);
    printf("   %7s %8s %8s %8s %8s %9s",
           "set(KB)", "read", "read8", "write", "copy", "lat(ns)");
    clr_eol(); fputs("\n", stdout);
    printf("   %7s %8s %8s %8s %8s %9s",
           "", "MB/s", "MB/s", "MB/s", "MB/s", "");
    clr_eol(); fputs("\n", stdout);

    for (i = 0; i < mm_n; i++) {
        printf("  %c%7lu", (i == mm_cur) ? '>' : ' ', mm_kb[i]);
        for (k = 0; k < MM_COLS - 1; k++) {
            if (mm_v[i][k] < 0.0) printf(" %8s", "-");
            else                  printf(" %8.1f", mm_v[i][k]);
        }
        if (mm_v[i][MM_COLS - 1] < 0.0) printf(" %9s", "-");
        else                            printf(" %9.1f", mm_v[i][MM_COLS - 1]);
        clr_eol(); fputs("\n", stdout);
    }
    clr_eol(); fputs("\n", stdout);
    if (mm_cur >= 0 && mm_cur < mm_n)
        printf("  measuring %lu KB  (%d/%d)", mm_kb[mm_cur], mm_cur + 1, mm_n);
    else
        printf("  complete.");
    clr_eol(); fputs("\n", stdout);
    clr_below();
    (void)fflush(stdout);
}

void sb_memtui_start(kbs, n, desc)
    unsigned long *kbs;
    int n;
    char *desc;
{
    int i, k;
    if (n > MM_MAX) n = MM_MAX;
    mm_n = n;
    for (i = 0; i < n; i++) {
        mm_kb[i] = kbs[i];
        for (k = 0; k < MM_COLS; k++)
            mm_v[i][k] = -1.0;
    }
    strncpy(mm_desc, desc, sizeof(mm_desc) - 1);
    mm_desc[sizeof(mm_desc) - 1] = '\0';
    mm_cur = -1;
    alt_enter();
    clr_all();
    memtui_draw();
}

void sb_memtui_cell(i)
    int i;
{
    mm_cur = i;
    memtui_draw();
}

void sb_memtui_set(i, col, val)
    int i, col;
    double val;
{
    if (i >= 0 && i < mm_n && col >= 0 && col < MM_COLS)
        mm_v[i][col] = val;
    memtui_draw();
}

void sb_memtui_finish()
{
    mm_cur = -1;
    memtui_draw();
    if (t_alt) {
        sb_tui_alt_leave();
        t_live = 0;
        memtui_draw();
        t_live = 1;
    }
    fputs("\n", stdout);
    (void)fflush(stdout);
}

/* ======================= network test dashboard ========================= */

/* Rows: 0..2 TCP rtt/up/down, then the TCP write-size sweep, then the TCP
 * read-size sweep, then UDP upload sizes, then UDP download sizes. */
#define NT_FIXED 3
#define NT_MAX   (NT_FIXED + 8 + 8 + 16 + 16)

static char   nt_label[NT_MAX][24];
static double nt_val[NT_MAX];      /* ms for row 0, Mbit/s elsewhere; <0 = - */
static double nt_aux[NT_MAX];      /* loss % for UDP rows; <0 = none */
static int    nt_n, nt_cur;
static char   nt_desc[128];

static void nettui_draw()
{
    int i;

    home();
    printf("  sunbench %s -- network benchmark", SB_VERSION);
    clr_eol(); fputs("\n", stdout);
    fputs("  ==========================================", stdout);
    clr_eol(); fputs("\n", stdout);
    sys_paint(0);
    printf("  server : %s", nt_desc); clr_eol(); fputs("\n", stdout);
    clr_eol(); fputs("\n", stdout);
    printf("   %-20s %12s %8s", "test", "result", "loss");
    clr_eol(); fputs("\n", stdout);

    for (i = 0; i < nt_n; i++) {
        printf("  %c%-20s", (i == nt_cur) ? '>' : ' ', nt_label[i]);
        if (nt_val[i] < 0.0)
            printf(" %12s", "-");
        else if (i == 0)
            printf(" %9.2f ms", nt_val[i]);
        else
            printf(" %7.2f Mb/s", nt_val[i]);
        if (nt_aux[i] >= 0.0)
            printf(" %7.1f%%", nt_aux[i]);
        clr_eol(); fputs("\n", stdout);
    }
    clr_eol(); fputs("\n", stdout);
    if (nt_cur >= 0 && nt_cur < nt_n)
        printf("  running %s  (%d/%d)", nt_label[nt_cur], nt_cur + 1, nt_n);
    else
        printf("  complete.");
    clr_eol(); fputs("\n", stdout);
    clr_below();
    (void)fflush(stdout);
}

void sb_nettui_start(desc, wsz, wn, rsz, rn, usz, un, dn)
    char *desc;
    unsigned long *wsz;
    int wn;
    unsigned long *rsz;
    int rn;
    unsigned long *usz;
    int un, dn;
{
    int i;
    strncpy(nt_desc, desc, sizeof(nt_desc) - 1);
    nt_desc[sizeof(nt_desc) - 1] = '\0';
    if (wn > 8) wn = 8;
    if (rn > 8) rn = 8;
    if (un > 16) un = 16;
    if (dn > 16) dn = 16;
    nt_n = NT_FIXED + wn + rn + un + dn;
    strcpy(nt_label[0], "TCP round trip");
    strcpy(nt_label[1], "TCP up (64K wr)");
    strcpy(nt_label[2], "TCP down (64K rd)");
    for (i = 0; i < wn; i++)
        sprintf(nt_label[NT_FIXED + i], "TCP write %lu B", wsz[i]);
    for (i = 0; i < rn; i++)
        sprintf(nt_label[NT_FIXED + wn + i], "TCP read %lu B", rsz[i]);
    for (i = 0; i < un; i++)
        sprintf(nt_label[NT_FIXED + wn + rn + i], "UDP up %lu B", usz[i]);
    for (i = 0; i < dn; i++)
        sprintf(nt_label[NT_FIXED + wn + rn + un + i], "UDP down %lu B",
                usz[i]);
    for (i = 0; i < nt_n; i++) {
        nt_val[i] = -1.0;
        nt_aux[i] = -1.0;
    }
    nt_cur = -1;
    alt_enter();
    clr_all();
    nettui_draw();
}

void sb_nettui_cell(i)
    int i;
{
    nt_cur = i;
    nettui_draw();
}

void sb_nettui_set(i, val, aux)
    int i;
    double val, aux;
{
    if (i >= 0 && i < nt_n) {
        nt_val[i] = val;
        nt_aux[i] = aux;
    }
    nettui_draw();
}

void sb_nettui_finish()
{
    nt_cur = -1;
    nettui_draw();
    if (t_alt) {
        sb_tui_alt_leave();
        t_live = 0;
        nettui_draw();
        t_live = 1;
    }
    fputs("\n", stdout);
    (void)fflush(stdout);
}

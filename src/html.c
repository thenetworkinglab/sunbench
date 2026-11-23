/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * html.c -- self-contained HTML report writer.  See html.h.
 *
 * Everything is plain fprintf/fputs; the CSS is inlined line by line (old
 * K&R compilers dislike very long string literals).  No JavaScript, no
 * external fonts or images: the file is complete in itself.
 */
#include "port.h"
#include "html.h"
#include "version.h"
#include "sysinfo.h"
#include "svgchart.h"
#include "whet.h"       /* WHET_INSTR_PER_LOOP for the MWIPS conversion */

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------ state --------------------------------- */

static int  h_on = 0;
static char h_path[512];
static char h_scratch[512];

/* disk standard */
static int       h_have_std = 0;
static char      h_std_method[80], h_std_file[128];
static sb_result h_std[4];
static int       h_have_lat[2];              /* [0]=rand read, [1]=rand write */
static double    h_lat[2][5];                /* mn p50 p90 p99 mx (ms) */

/* disk standard, min/median/max variant */
static int    h_have_mmm = 0;
static char   h_mmm_method[80], h_mmm_file[128];
static int    h_mmm_passes, h_mmm_n;
static char   h_mmm_label[4][16];
static double h_mmm_v[4][3];                 /* mn md mx */

/* disk sweep */
static int           h_have_dsw = 0;
static char          h_dsw_method[80], h_dsw_file[128];
static int           h_dsw_passes, h_dsw_n;
static unsigned long h_dsw_kb[16];
static double        h_dsw_v[16][4];
static double        h_dsw_iops[16];

/* cpu */
static int           h_have_cpu = 0;
static char          h_cpu_build[96];
static unsigned long h_cpu_mhz;
static int           h_cpu_passes;
static double        h_cpu_rates[SBK_NKERNELS];

static int       h_have_cmp = 0;
static int       h_cmp_ncpus, h_cmp_n;
static sb_cpu_mp h_cmp[SBK_NKERNELS];

/* memory */
static int           h_have_mem = 0;
static int           h_mem_n;
static unsigned long h_mem_kb[20];
static double        h_mem_v[20][SBMEM_NKERNELS];

static int       h_have_mmp = 0;
static int       h_mmp_ncpus;
static sb_cpu_mp h_mmp[6];
static int       h_mmp_n = 0;

/* network */
static int           h_have_net = 0;
static char          h_net_server[128];
static int           h_net_mtu;
static double        h_net_up, h_net_down, h_net_rtt;
static int           h_net_wn;
static unsigned long h_net_wsz[8];
static double        h_net_wmbps[8], h_net_rmbps[8];
static int           h_net_n;
static unsigned long h_net_sz[16];
static double        h_net_mbps[16], h_net_loss[16];
static double        h_net_dmbps[16], h_net_dloss[16];
static char          h_net_rttlab[32] = "TCP round trip";

/* ----------------------------- feeders -------------------------------- */

void sb_html_enable(path, scratch)
    char *path, *scratch;
{
    strncpy(h_path, path, sizeof(h_path) - 1);
    h_path[sizeof(h_path) - 1] = '\0';
    strncpy(h_scratch, scratch, sizeof(h_scratch) - 1);
    h_scratch[sizeof(h_scratch) - 1] = '\0';
    h_on = 1;
}

int sb_html_on()
{
    return h_on;
}

void sb_html_disable()
{
    h_on = 0;
}

static char h_note[256];

void sb_html_note(note)
    char *note;
{
    /* Stored regardless of h_on, so --note works no matter when the
     * report is enabled (CLI at startup, or the menu toggle later). */
    if (note == (char *)0) note = "";
    strncpy(h_note, note, sizeof(h_note) - 1);
    h_note[sizeof(h_note) - 1] = '\0';
}

void sb_html_disk_std(method, fdesc, w, r, rr, rw)
    char *method, *fdesc;
    sb_result *w, *r, *rr, *rw;
{
    if (!h_on) return;
    strncpy(h_std_method, method, sizeof(h_std_method) - 1);
    h_std_method[sizeof(h_std_method) - 1] = '\0';
    strncpy(h_std_file, fdesc, sizeof(h_std_file) - 1);
    h_std_file[sizeof(h_std_file) - 1] = '\0';
    h_std[0] = *w; h_std[1] = *r; h_std[2] = *rr; h_std[3] = *rw;
    h_have_std = 1;
}

#define H_HISTMAX 40
static int    h_have_hist[2] = { 0, 0 };
static int    h_hist_n[2];
static unsigned long h_hist_ops[2];
static double h_hist_ms[2][H_HISTMAX];
static unsigned long h_hist_ct[2][H_HISTMAX];

void sb_html_disk_hist(write_test, hist)
    int write_test;
    sb_hist *hist;
{
    int i, lo, hi, b, n;
    i = write_test ? 1 : 0;
    if (!h_on || hist->n == 0) return;
    lo = 0; while (lo < SB_NBUCKETS && hist->count[lo] == 0) lo++;
    hi = SB_NBUCKETS - 1; while (hi > lo && hist->count[hi] == 0) hi--;
    n = 0;
    for (b = lo; b <= hi && n < H_HISTMAX; b++) {
        h_hist_ms[i][n] = sb_hist_bucket_low_ms(b);
        h_hist_ct[i][n] = hist->count[b];
        n++;
    }
    /* Coalesce adjacent buckets into at most ~10 rows: merge in groups
     * of ceil(n/10), which lands on the target instead of overshooting
     * the way repeated halving would.  Merging neighbours just coarsens
     * the log2 sub-bucket resolution; the ASCII version in the log
     * keeps the full detail. */
    if (n > 10) {
        int w, r2, g, k;
        k = (n + 9) / 10;
        w = 0;
        for (r2 = 0; r2 < n; r2 += k) {
            h_hist_ms[i][w] = h_hist_ms[i][r2];
            h_hist_ct[i][w] = 0;
            for (g = r2; g < r2 + k && g < n; g++)
                h_hist_ct[i][w] += h_hist_ct[i][g];
            w++;
        }
        n = w;
    }
    h_hist_n[i] = n;
    h_hist_ops[i] = hist->n;
    h_have_hist[i] = 1;
}

void sb_html_disk_lat(write_test, mn, p50, p90, p99, mx)
    int write_test;
    double mn, p50, p90, p99, mx;
{
    int i = write_test ? 1 : 0;
    if (!h_on) return;
    h_lat[i][0] = mn;  h_lat[i][1] = p50; h_lat[i][2] = p90;
    h_lat[i][3] = p99; h_lat[i][4] = mx;
    h_have_lat[i] = 1;
}

void sb_html_disk_mmm(method, fdesc, passes, labels, mn, md, mx, n)
    char *method, *fdesc;
    int passes;
    char **labels;
    double *mn, *md, *mx;
    int n;
{
    int i;
    if (!h_on) return;
    strncpy(h_mmm_method, method, sizeof(h_mmm_method) - 1);
    h_mmm_method[sizeof(h_mmm_method) - 1] = '\0';
    strncpy(h_mmm_file, fdesc, sizeof(h_mmm_file) - 1);
    h_mmm_file[sizeof(h_mmm_file) - 1] = '\0';
    h_mmm_passes = passes;
    if (n > 4) n = 4;
    h_mmm_n = n;
    for (i = 0; i < n; i++) {
        strncpy(h_mmm_label[i], labels[i], sizeof(h_mmm_label[i]) - 1);
        h_mmm_label[i][sizeof(h_mmm_label[i]) - 1] = '\0';
        h_mmm_v[i][0] = mn[i]; h_mmm_v[i][1] = md[i]; h_mmm_v[i][2] = mx[i];
    }
    h_have_mmm = 1;
}

void sb_html_disk_sweep(method, fdesc, passes, kb, med, iops, n)
    char *method, *fdesc;
    int passes;
    unsigned long *kb;
    double (*med)[4];
    double *iops;
    int n;
{
    int i, j;
    if (!h_on) return;
    strncpy(h_dsw_method, method, sizeof(h_dsw_method) - 1);
    h_dsw_method[sizeof(h_dsw_method) - 1] = '\0';
    strncpy(h_dsw_file, fdesc, sizeof(h_dsw_file) - 1);
    h_dsw_file[sizeof(h_dsw_file) - 1] = '\0';
    h_dsw_passes = passes;
    if (n > 16) n = 16;
    h_dsw_n = n;
    for (i = 0; i < n; i++) {
        h_dsw_kb[i] = kb[i];
        for (j = 0; j < 4; j++)
            h_dsw_v[i][j] = med[i][j];
        h_dsw_iops[i] = iops[i];
    }
    h_have_dsw = 1;
}

void sb_html_cpu(build, mhz, passes, rates)
    char *build;
    unsigned long mhz;
    int passes;
    double *rates;
{
    int k;
    if (!h_on) return;
    strncpy(h_cpu_build, build, sizeof(h_cpu_build) - 1);
    h_cpu_build[sizeof(h_cpu_build) - 1] = '\0';
    h_cpu_mhz = mhz;
    h_cpu_passes = passes;
    for (k = 0; k < SBK_NKERNELS; k++)
        h_cpu_rates[k] = rates[k];
    h_have_cpu = 1;
}

void sb_html_cpu_mp(ncpus, rows, n)
    int ncpus;
    sb_cpu_mp *rows;
    int n;
{
    int i;
    if (!h_on) return;
    h_cmp_ncpus = ncpus;
    if (n > SBK_NKERNELS) n = SBK_NKERNELS;
    h_cmp_n = n;
    for (i = 0; i < n; i++)
        h_cmp[i] = rows[i];
    h_have_cmp = 1;
}

void sb_html_mem(kb, val, n)
    unsigned long *kb;
    double (*val)[SBMEM_NKERNELS];
    int n;
{
    int i, k;
    if (!h_on) return;
    if (n > 20) n = 20;
    h_mem_n = n;
    for (i = 0; i < n; i++) {
        h_mem_kb[i] = kb[i];
        for (k = 0; k < SBMEM_NKERNELS; k++)
            h_mem_v[i][k] = val[i][k];
    }
    h_have_mem = 1;
}

void sb_html_mem_mp(ncpus, m, n)
    int ncpus;
    sb_cpu_mp *m;
    int n;
{
    int i;
    if (!h_on) return;
    if (n > 6) n = 6;
    h_mmp_ncpus = ncpus;
    for (i = 0; i < n; i++)
        h_mmp[i] = m[i];
    h_mmp_n = n;
    h_have_mmp = 1;
}

void sb_html_net(server, mtu, up, down, rtt_ms, wsz, wmbps, rmbps, wn,
                 usz, umbps, uloss, dmbps, dloss, un)
    char *server;
    int mtu;
    double up, down, rtt_ms;
    unsigned long *wsz;
    double *wmbps, *rmbps;
    int wn;
    unsigned long *usz;
    double *umbps, *uloss, *dmbps, *dloss;
    int un;
{
    int i;
    if (!h_on) return;
    strncpy(h_net_server, server, sizeof(h_net_server) - 1);
    h_net_server[sizeof(h_net_server) - 1] = '\0';
    h_net_mtu = mtu;
    h_net_up = up; h_net_down = down; h_net_rtt = rtt_ms;
    if (wn > 8) wn = 8;
    h_net_wn = wn;
    for (i = 0; i < wn; i++) {
        h_net_wsz[i] = wsz[i];
        h_net_wmbps[i] = wmbps[i];
        h_net_rmbps[i] = rmbps[i];
    }
    if (un > 16) un = 16;
    h_net_n = un;
    for (i = 0; i < un; i++) {
        h_net_sz[i] = usz[i];
        h_net_mbps[i] = umbps[i];
        h_net_loss[i] = uloss[i];
        h_net_dmbps[i] = dmbps[i];
        h_net_dloss[i] = dloss[i];
    }
    h_have_net = 1;
}

/* SBDOS measures its round trip over UDP (no TCP there). */
void sb_html_net_rtt_label(label)
    char *label;
{
    strncpy(h_net_rttlab, label, sizeof(h_net_rttlab) - 1);
    h_net_rttlab[sizeof(h_net_rttlab) - 1] = '\0';
}

/* ----------------------------- emitters ------------------------------- */

/* Size label for chart axes: input in KB ("4K".."16M") or bytes. */
static void szlab_kb(buf, kb)
    char *buf;
    unsigned long kb;
{
    if (kb >= 1024L) sprintf(buf, "%luM", kb / 1024L);
    else              sprintf(buf, "%luK", kb);
}

static void szlab_b(buf, b)
    char *buf;
    unsigned long b;
{
    if (b >= 1024L && b % 1024L == 0L)
        sprintf(buf, "%luK", b / 1024L);
    else
        sprintf(buf, "%lu", b);
}

/* Write s with &<> escaped (hostnames, notes, model strings). */
static void esc(f, s)
    FILE *f;
    char *s;
{
    char *p;
    for (p = s; *p; p++) {
        if (*p == '&')      fputs("&amp;", f);
        else if (*p == '<') fputs("&lt;", f);
        else if (*p == '>') fputs("&gt;", f);
        else                putc(*p, f);
    }
}

static void css(f)
    FILE *f;
{
    fputs("<style>\n", f);
    fputs("*{margin:0;padding:0;box-sizing:border-box}\n", f);
    fputs("body{font-family:-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background:#eef0f4;color:#1a2030;padding:24px;line-height:1.45}\n", f);
    fputs(".wrap{max-width:860px;margin:0 auto}\n", f);
    fputs("h1{font-size:22px;font-weight:700;letter-spacing:-.01em}\n", f);
    fputs("h1 .v{color:#5661d6;font-weight:600}\n", f);
    fputs(".sub{color:#5a6478;font-size:13px;margin:2px 0 18px}\n", f);
    fputs(".card{background:#fff;border-radius:10px;padding:18px 20px;margin:0 0 16px;box-shadow:0 1px 3px rgba(20,24,40,.08)}\n", f);
    fputs("h2{font-size:15px;font-weight:650;margin:0 0 2px}\n", f);
    fputs(".meta{color:#5a6478;font-size:12px;margin:0 0 10px}\n", f);
    fputs("table{border-collapse:collapse;width:100%;font-size:13px;font-variant-numeric:tabular-nums}\n", f);
    fputs("th{text-align:right;font-weight:600;color:#5a6478;font-size:11px;text-transform:uppercase;letter-spacing:.04em;padding:6px 10px;border-bottom:1px solid #e3e6ee}\n", f);
    fputs("th.l,td.l{text-align:left}\n", f);
    fputs("td{text-align:right;padding:5px 10px;border-bottom:1px solid #f0f2f7}\n", f);
    fputs("tr:last-child td{border-bottom:0}\n", f);
    fputs("td.b{position:relative;overflow:hidden}\n", f);
    fputs("td.b span.bar{position:absolute;right:6px;top:15%;height:70%;background:#dfe3fb;border-radius:3px;z-index:0}\n", f);
    fputs("td.hb{text-align:left;white-space:nowrap}\n", f);
    fputs("table.hist td{padding:1px 10px;border-bottom:0;font-size:12px}\n", f);
    fputs("td.hb span.hbar{display:inline-block;height:11px;background:#dfe3fb;border-radius:3px;vertical-align:-1px}\n", f);
    fputs("td.b span.tx{position:relative;z-index:1}\n", f);
    fputs(".grid{display:table;width:100%;font-size:13px}\n", f);
    fputs(".grid .row{display:table-row}\n", f);
    fputs(".grid .k{display:table-cell;color:#5a6478;padding:2px 14px 2px 0;white-space:nowrap;width:1%}\n", f);
    fputs(".grid .val{display:table-cell;padding:2px 0}\n", f);
    fputs(".foot{color:#8a92a6;font-size:11px;margin-top:14px}\n", f);
    fputs("@media print{body{background:#fff;padding:0}.card{box-shadow:none;border:1px solid #d5d9e4;page-break-inside:avoid}td.b span.bar,td.hb span.hbar{-webkit-print-color-adjust:exact;print-color-adjust:exact}}\n", f);
    fputs("</style>\n", f);
}

/* A numeric cell with a proportional background bar.  The bar is inset from
 * the cell edges (right:6px in the CSS, width capped below 100%) so bars in
 * adjacent cells can never touch and fuse into one band across the row. */
static void bar_td(f, val, pct, dec)
    FILE *f;
    double val;
    int pct, dec;
{
    if (val < 0.0) {
        fputs("<td>-</td>", f);
        return;
    }
    if (pct < 0) pct = 0;
    pct = pct * 88 / 100;               /* 100% of scale = 88% of the cell */
    if (pct > 88) pct = 88;
    fprintf(f, "<td class=b><span class=bar style=\"width:%d%%\"></span><span class=tx>%.*f</span></td>", pct, dec, val);
}

/* Histogram cell: an inline bar with the count IMMEDIATELY after its
 * tip, like the ASCII version ("#### 43") -- the adjacency is what makes
 * the distribution readable at a glance.  Width is a percentage of the
 * cell, capped so the largest count still fits beside the peak bar. */
static void barl_td(f, count, pct)
    FILE *f;
    unsigned long count;
    int pct;
{
    if (pct < 0) pct = 0;
    pct = pct * 82 / 100;
    if (pct > 82) pct = 82;
    fprintf(f, "<td class=hb><span class=hbar style=\"width:%d%%\"></span> %lu</td>", pct, count);
}

static int pct_of(v, max)
    double v, max;
{
    if (v <= 0.0 || max <= 0.0) return 0;
    return (int)(v / max * 100.0 + 0.5);
}

static void section_open(f, title, meta1, meta2)
    FILE *f;
    char *title, *meta1, *meta2;
{
    fputs("<div class=card>\n", f);
    fprintf(f, "<h2>%s</h2>\n", title);
    if (meta1 && meta1[0]) {
        fputs("<p class=meta>", f);
        esc(f, meta1);
        if (meta2 && meta2[0]) {
            fputs(" &middot; ", f);
            esc(f, meta2);
        }
        fputs("</p>\n", f);
    }
}

int sb_html_write()
{
    FILE *f;
    char lines[SB_SYSLINES][SB_SYSLINE];
    char host[64];
    time_t now;
    int nl, i, j, k;

    if (!h_on)
        return -1;
    f = fopen(h_path, "w");
    if (f == (FILE *)0)
        return -1;

    host[0] = '\0';
    sb_host(host, (int)sizeof(host));
    (void)time(&now);

    fputs("<!DOCTYPE html>\n<html>\n<head>\n", f);
    fputs("<meta charset=\"utf-8\">\n", f);
    fputs("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n", f);
    fputs("<title>sunbench &mdash; ", f);
    esc(f, host);
    fputs("</title>\n", f);
    css(f);
    fputs("</head>\n<body>\n<div class=wrap>\n", f);

    fputs("<h1>sunbench <span class=v>", f);
    esc(f, SB_VERSION);
    fputs("</span> &mdash; ", f);
    esc(f, host);
    fputs("</h1>\n", f);
    fprintf(f, "<p class=sub>%s</p>\n", ctime(&now));

    /* machine synopsis card */
    section_open(f, "Machine", "", "");
    fputs("<div class=grid>\n", f);
    nl = sb_sysinfo_lines(h_scratch, lines, SB_SYSLINES);
    for (i = 0; i < nl; i++) {
        char *c = strchr(lines[i], ':');
        if (c == (char *)0) continue;
        *c = '\0';
        fputs("<div class=row><div class=k>", f);
        esc(f, lines[i] + 2);               /* skip the two-space indent */
        fputs("</div><div class=val>", f);
        esc(f, c + 2);
        fputs("</div></div>\n", f);
    }
    if (h_note[0]) {
        fputs("<div class=row><div class=k>note</div><div class=val>", f);
        esc(f, h_note);
        fputs("</div></div>\n", f);
    }
    fputs("</div>\n</div>\n", f);

    /* disk standard */
    if (h_have_std) {
        section_open(f, "Disk &mdash; standard run", h_std_method, h_std_file);
        fputs("<table>\n<tr><th class=l>test</th><th>MB/s</th><th>IOPS</th><th>latency (ms)</th></tr>\n", f);
        {
            double mx = 0.0;
            for (i = 0; i < 4; i++)
                if (h_std[i].ok && h_std[i].mb_per_s > mx)
                    mx = h_std[i].mb_per_s;
            for (i = 0; i < 4; i++) {
                if (!h_std[i].ok) continue;
                fprintf(f, "<tr><td class=l>%s</td>", h_std[i].name);
                bar_td(f, h_std[i].mb_per_s,
                       pct_of(h_std[i].mb_per_s, mx), 2);
                fprintf(f, "<td>%.1f</td><td>%.3f</td></tr>\n",
                        h_std[i].iops, h_std[i].avg_lat_ms);
            }
        }
        fputs("</table>\n", f);
        if (h_have_lat[0] || h_have_lat[1]) {
            fputs("<table style=\"margin-top:12px\">\n<tr><th class=l>random latency</th><th>min</th><th>p50</th><th>p90</th><th>p99</th><th>max (ms)</th></tr>\n", f);
            for (i = 0; i < 2; i++) {
                if (!h_have_lat[i]) continue;
                fprintf(f, "<tr><td class=l>%s</td>",
                        i ? "rand write" : "rand read");
                for (j = 0; j < 5; j++)
                    fprintf(f, "<td>%.2f</td>", h_lat[i][j]);
                fputs("</tr>\n", f);
            }
            fputs("</table>\n", f);
        }
        for (i = 0; i < 2; i++) {
            unsigned long peak; int b;
            if (!h_have_hist[i]) continue;
            peak = 1;
            for (b = 0; b < h_hist_n[i]; b++)
                if (h_hist_ct[i][b] > peak) peak = h_hist_ct[i][b];
            fprintf(f, "<table class=hist style=\"margin-top:12px\">\n<tr><th class=l style=\"width:6em\">%s latency distribution</th><th class=l>%lu ops</th></tr>\n",
                    i ? "rand write" : "rand read", h_hist_ops[i]);
            for (b = 0; b < h_hist_n[i]; b++) {
                fprintf(f, "<tr><td class=l>%.2f ms</td>", h_hist_ms[i][b]);
                barl_td(f, h_hist_ct[i][b],
                        pct_of((double)h_hist_ct[i][b], (double)peak));
                fputs("</tr>\n", f);
            }
            fputs("</table>\n", f);
        }
        fputs("</div>\n", f);
    }

    /* disk standard, min/median/max */
    if (h_have_mmm) {
        char meta[64];
        sprintf(meta, "%d passes; MB/s min / median / max", h_mmm_passes);
        section_open(f, "Disk &mdash; repeated runs", h_mmm_method, meta);
        fputs("<table>\n<tr><th class=l>test</th><th>min</th><th>median</th><th>max (MB/s)</th></tr>\n", f);
        {
            double mx = 0.0;
            for (i = 0; i < h_mmm_n; i++)
                if (h_mmm_v[i][1] > mx) mx = h_mmm_v[i][1];
            for (i = 0; i < h_mmm_n; i++) {
                fprintf(f, "<tr><td class=l>%s</td><td>%.2f</td>",
                        h_mmm_label[i], h_mmm_v[i][0]);
                bar_td(f, h_mmm_v[i][1], pct_of(h_mmm_v[i][1], mx), 2);
                fprintf(f, "<td>%.2f</td></tr>\n", h_mmm_v[i][2]);
            }
        }
        fputs("</table>\n</div>\n", f);
    }

    /* disk sweep */
    if (h_have_dsw) {
        char meta[80];
        if (h_dsw_passes > 1)
            sprintf(meta, "median of %d passes per cell", h_dsw_passes);
        else
            meta[0] = '\0';
        section_open(f, "Disk &mdash; block-size sweep",
                     h_dsw_method, meta[0] ? meta : h_dsw_file);
        fputs("<table>\n<tr><th class=l>block</th><th>seq write</th><th>seq read</th><th>rand read</th><th>rand write (MB/s)</th><th>rndR IOPS</th></tr>\n", f);
        {
            double mx = 0.0, mxi = 0.0;
            for (i = 0; i < h_dsw_n; i++) {
                for (j = 0; j < 4; j++)
                    if (h_dsw_v[i][j] > mx) mx = h_dsw_v[i][j];
                if (h_dsw_iops[i] > mxi) mxi = h_dsw_iops[i];
            }
            for (i = 0; i < h_dsw_n; i++) {
                fprintf(f, "<tr><td class=l>%lu KB</td>", h_dsw_kb[i]);
                for (j = 0; j < 4; j++)
                    bar_td(f, h_dsw_v[i][j], pct_of(h_dsw_v[i][j], mx), 2);
                bar_td(f, h_dsw_iops[i], pct_of(h_dsw_iops[i], mxi), 1);
                fputs("</tr>\n", f);
            }
        }
        fputs("</table>\n", f);
        {
            static char *sn[4];
            static char lb[16][12];
            char *lp[16];
            double v[4 * 16];
            sn[0] = "seq write"; sn[1] = "seq read";
            sn[2] = "rand read"; sn[3] = "rand write";
            for (i = 0; i < h_dsw_n; i++) {
                szlab_kb(lb[i], h_dsw_kb[i]);
                lp[i] = lb[i];
                for (j = 0; j < 4; j++)
                    v[j * h_dsw_n + i] = h_dsw_v[i][j];
            }
            sb_svg_chart(f, "Throughput vs block size", "MB/s",
                         lp, h_dsw_n, sn, v, 4, 0);
        }
        fputs("</div>\n", f);
    }

    /* cpu */
    if (h_have_cpu) {
        char meta[96];
        if (h_cpu_mhz)
            sprintf(meta, "%lu MHz", h_cpu_mhz);
        else
            meta[0] = '\0';
        if (h_cpu_passes > 1)
            sprintf(meta + strlen(meta), "%smedian of %d passes",
                    meta[0] ? ", " : "", h_cpu_passes);
        section_open(f, "CPU", h_cpu_build, meta);
        fputs("<table>\n<tr><th class=l>kernel</th><th>result</th><th class=l>unit</th></tr>\n", f);
        for (k = 0; k < SBK_NKERNELS; k++) {
            double v = h_cpu_rates[k];
            if (v < 0.0) continue;
            fprintf(f, "<tr><td class=l>%s</td>", sb_cpu_kernel_name(k));
            if (k == SBK_DHRYSTONE)
                fprintf(f, "<td>%.0f</td><td class=l>Dhrystones/s (%.2f VAX MIPS)</td>", v, v / 1757.0);
            else if (SBK_IS_XPY(k)) {
                static char *xl[4];
                xl[0] = "single"; xl[1] = "double";
                xl[2] = "single, streaming"; xl[3] = "double, streaming";
                fprintf(f, "<td>%.2f</td><td class=l>MFLOPS (Linpack-style %s)</td>",
                        v * sb_cpu_xpy_flops(k) / 1.0e6,
                        xl[k - SBK_SAXPY]);
            }
            else if (k == SBK_WHETSTONE)
                fprintf(f, "<td>%.2f</td><td class=l>MWIPS</td>",
                        v * WHET_INSTR_PER_LOOP / 1.0e6);
            else if (h_cpu_mhz)
                fprintf(f, "<td>%.3f</td><td class=l>Miter/s (%.4f/clk)</td>", v / 1.0e6,
                        v / ((double)h_cpu_mhz * 1.0e6));
            else
                fprintf(f, "<td>%.3f</td><td class=l>Miter/s</td>",
                        v / 1.0e6);
            fputs("</tr>\n", f);
        }
        fputs("</table>\n</div>\n", f);
    }

    /* cpu MP */
    if (h_have_cmp) {
        char meta[64];
        sprintf(meta, "%d concurrent processes, wall-clock rates",
                h_cmp_ncpus);
        section_open(f, "CPU &mdash; multiprocessor scaling", meta, "");
        fputs("<table>\n<tr><th class=l>kernel</th><th>1-proc</th><th>N-proc</th><th>speedup</th><th>scaling</th></tr>\n", f);
        for (i = 0; i < h_cmp_n; i++) {
            /* Native units per kernel, as in the text table: a ?axpy
             * "iteration" is a whole vector pass, so raw Miter/s printed
             * as 0.000 there.  Rows are matched to kernels by name since a
             * failed kernel leaves no row. */
            double s = h_cmp[i].single_ips, a = h_cmp[i].agg_ips;
            char *unit = "Miter/s";
            int k;
            for (k = 0; k < SBK_NKERNELS; k++)
                if (strcmp(h_cmp[i].name, sb_cpu_kernel_name(k)) == 0)
                    break;
            if (k < SBK_NKERNELS && SBK_IS_XPY(k)) {
                s = s * sb_cpu_xpy_flops(k) / 1.0e6;
                a = a * sb_cpu_xpy_flops(k) / 1.0e6;
                unit = "MFLOPS";
            } else if (k == SBK_WHETSTONE) {
                s = s * WHET_INSTR_PER_LOOP / 1.0e6;
                a = a * WHET_INSTR_PER_LOOP / 1.0e6;
                unit = "MWIPS";
            } else if (k == SBK_DHRYSTONE) {
                s /= 1.0e3; a /= 1.0e3;
                unit = "kDhry/s";
            } else {
                s /= 1.0e6; a /= 1.0e6;
            }
            fprintf(f, "<tr><td class=l>%s</td><td>%.2f %s</td><td>%.2f %s</td><td>%.2f&times;</td>",
                    h_cmp[i].name, s, unit, a, unit, h_cmp[i].speedup);
            bar_td(f, h_cmp[i].efficiency * 100.0,
                   (int)(h_cmp[i].efficiency * 100.0 + 0.5), 1);
            fputs("</tr>\n", f);
        }
        fputs("</table>\n</div>\n", f);
    }

    /* memory sweep */
    if (h_have_mem) {
        section_open(f, "Memory &mdash; working-set sweep",
                     "bandwidth MB/s; latency = ns per dependent load", "");
        fputs("<table>\n<tr><th class=l>set</th><th>read</th><th>read8</th><th>write</th><th>copy</th><th>latency (ns)</th></tr>\n", f);
        {
            double mx = 0.0, mxl = 0.0;
            for (i = 0; i < h_mem_n; i++) {
                for (k = 0; k < SBMEM_NKERNELS - 1; k++)
                    if (h_mem_v[i][k] > mx) mx = h_mem_v[i][k];
                if (h_mem_v[i][SBMEM_LAT] > mxl)
                    mxl = h_mem_v[i][SBMEM_LAT];
            }
            for (i = 0; i < h_mem_n; i++) {
                if (h_mem_kb[i] >= 1024)
                    fprintf(f, "<tr><td class=l>%lu MB</td>",
                            h_mem_kb[i] / 1024);
                else
                    fprintf(f, "<tr><td class=l>%lu KB</td>", h_mem_kb[i]);
                for (k = 0; k < SBMEM_NKERNELS - 1; k++)
                    bar_td(f, h_mem_v[i][k], pct_of(h_mem_v[i][k], mx), 1);
                bar_td(f, h_mem_v[i][SBMEM_LAT],
                       pct_of(h_mem_v[i][SBMEM_LAT], mxl), 1);
                fputs("</tr>\n", f);
            }
        }
        fputs("</table>\n", f);
        {
            static char *sn[4];
            static char *ln[1];
            static char lb[20][12];
            char *lp[20];
            double v[4 * 20], lv[20];
            sn[0] = "read"; sn[1] = "read8"; sn[2] = "write";
            sn[3] = "copy";
            ln[0] = "latency";
            for (i = 0; i < h_mem_n; i++) {
                szlab_kb(lb[i], h_mem_kb[i]);
                lp[i] = lb[i];
                for (k = 0; k < 4; k++)
                    v[k * h_mem_n + i] = h_mem_v[i][k];
                lv[i] = h_mem_v[i][SBMEM_LAT];
            }
            sb_svg_chart(f, "Bandwidth vs working set", "MB/s",
                         lp, h_mem_n, sn, v, 4, 0);
            sb_svg_chart(f, "Load latency vs working set", "ns",
                         lp, h_mem_n, ln, lv, 1, 1);
        }
        fputs("</div>\n", f);
    }

    /* memory MP */
    if (h_have_mmp) {
        char meta[96];
        int i;
        sprintf(meta, "%d concurrent processes, private buffers; lat loaded = chase under %d streaming reader%s",
                h_mmp_ncpus, h_mmp_ncpus - 1, h_mmp_ncpus == 2 ? "" : "s");
        section_open(f, "Memory &mdash; multiprocessor scaling", meta, "");
        fputs("<table>\n<tr><th class=l>test</th><th>1-proc</th><th>N-proc</th><th>speedup</th><th>scaling</th></tr>\n", f);
        for (i = 0; i < h_mmp_n; i++) {
            sb_cpu_mp *m = &h_mmp[i];
            if (m->efficiency < 0.0) {           /* loaded latency, in ns */
                fprintf(f, "<tr><td class=l>%s</td><td>%.1f ns</td><td>%.1f ns</td><td>%.2f&times;</td><td>&mdash;</td></tr>\n",
                        m->name, m->single_ips, m->agg_ips, m->speedup);
                continue;
            }
            fprintf(f, "<tr><td class=l>%s</td><td>%.1f MB/s</td><td>%.1f MB/s</td><td>%.2f&times;</td>",
                    m->name, m->single_ips / (1024.0 * 1024.0),
                    m->agg_ips / (1024.0 * 1024.0), m->speedup);
            bar_td(f, m->efficiency * 100.0,
                   (int)(m->efficiency * 100.0 + 0.5), 1);
            fputs("</tr>\n", f);
        }
        fputs("</table>\n</div>\n", f);
    }

    /* network */
    if (h_have_net) {
        char meta[192];
        if (h_net_mtu)
            sprintf(meta, "server %.128s, local interface MTU %d",
                    h_net_server, h_net_mtu);
        else
            sprintf(meta, "server %.128s", h_net_server);
        section_open(f, "Network", meta, "");
        fputs("<table>\n<tr><th class=l>test</th><th>result</th><th class=l>unit</th></tr>\n", f);
        {
            double mx = h_net_up > h_net_down ? h_net_up : h_net_down;
            if (h_net_up > 0.0) {
                fputs("<tr><td class=l>TCP upload (64 KB writes)</td>", f);
                bar_td(f, h_net_up, pct_of(h_net_up, mx), 2);
                fputs("<td class=l>Mbit/s</td></tr>\n", f);
            }
            if (h_net_down > 0.0) {
                fputs("<tr><td class=l>TCP download (64 KB reads)</td>", f);
                bar_td(f, h_net_down, pct_of(h_net_down, mx), 2);
                fputs("<td class=l>Mbit/s</td></tr>\n", f);
            }
            if (h_net_rtt > 0.0)
                fprintf(f, "<tr><td class=l>%s</td><td>%.2f</td><td class=l>ms</td></tr>\n",
                        h_net_rttlab, h_net_rtt);
        }
        fputs("</table>\n", f);
        if (h_net_wn > 0) {
            double mx = 0.0;
            fputs("<table style=\"margin-top:12px\">\n<tr><th class=l>TCP chunk size</th><th>upload (Mbit/s)</th><th>download (Mbit/s)</th></tr>\n", f);
            for (i = 0; i < h_net_wn; i++) {
                if (h_net_wmbps[i] > mx) mx = h_net_wmbps[i];
                if (h_net_rmbps[i] > mx) mx = h_net_rmbps[i];
            }
            for (i = 0; i < h_net_wn; i++) {
                if (h_net_wmbps[i] < 0.0 && h_net_rmbps[i] < 0.0)
                    continue;
                fprintf(f, "<tr><td class=l>%lu bytes</td>", h_net_wsz[i]);
                bar_td(f, h_net_wmbps[i], pct_of(h_net_wmbps[i], mx), 2);
                bar_td(f, h_net_rmbps[i], pct_of(h_net_rmbps[i], mx), 2);
                fputs("</tr>\n", f);
            }
            fputs("</table>\n", f);
            {
                static char *sn[2];
                static char lb[8][12];
                char *lp[8];
                double v[2 * 8];
                sn[0] = "upload"; sn[1] = "download";
                for (i = 0; i < h_net_wn; i++) {
                    szlab_b(lb[i], h_net_wsz[i]);
                    lp[i] = lb[i];
                    v[0 * h_net_wn + i] = h_net_wmbps[i];
                    v[1 * h_net_wn + i] = h_net_rmbps[i];
                }
                sb_svg_chart(f, "TCP throughput vs chunk size", "Mbit/s",
                             lp, h_net_wn, sn, v, 2, 0);
            }
            fputs("<p class=meta style=\"margin-top:6px\">TCP chunk sizes are application write()/read() sizes, not wire packets (the kernel segments to the MSS); the sweep shows per-syscall cost on the CPU, in each direction.</p>\n", f);
        }
        if (h_net_n > 0) {
            double mx = 0.0;
            fputs("<table style=\"margin-top:12px\">\n<tr><th class=l>UDP datagram</th><th>upload (Mbit/s)</th><th>loss</th><th>download (Mbit/s)</th><th>loss</th></tr>\n", f);
            for (i = 0; i < h_net_n; i++) {
                if (h_net_mbps[i] > mx) mx = h_net_mbps[i];
                if (h_net_dmbps[i] > mx) mx = h_net_dmbps[i];
            }
            for (i = 0; i < h_net_n; i++) {
                if (h_net_mbps[i] < 0.0 && h_net_dmbps[i] < 0.0)
                    continue;
                fprintf(f, "<tr><td class=l>%lu bytes</td>", h_net_sz[i]);
                bar_td(f, h_net_mbps[i], pct_of(h_net_mbps[i], mx), 2);
                if (h_net_mbps[i] < 0.0) fputs("<td>-</td>", f);
                else fprintf(f, "<td>%.1f%%</td>", h_net_loss[i]);
                bar_td(f, h_net_dmbps[i], pct_of(h_net_dmbps[i], mx), 2);
                if (h_net_dmbps[i] < 0.0) fputs("<td>-</td>", f);
                else fprintf(f, "<td>%.1f%%</td>", h_net_dloss[i]);
                fputs("</tr>\n", f);
            }
            fputs("</table>\n", f);
            {
                static char *sn[2];
                static char lb[16][12];
                char *lp[16];
                double v[2 * 16];
                sn[0] = "up"; sn[1] = "down";
                for (i = 0; i < h_net_n; i++) {
                    szlab_b(lb[i], h_net_sz[i]);
                    lp[i] = lb[i];
                    v[0 * h_net_n + i] = h_net_mbps[i];
                    v[1 * h_net_n + i] = h_net_dmbps[i];
                }
                sb_svg_chart(f, "UDP throughput vs datagram size",
                             "Mbit/s", lp, h_net_n, sn, v, 2, 0);
            }
            fputs("<p class=meta style=\"margin-top:6px\">UDP download is paced by the server to 1.5&times; the upload result for the same size, halved and rerun while loss exceeds 40%, so the column shows the highest rate this machine's receive path actually sustains.</p>\n", f);
        }
        fputs("</div>\n", f);
    }

    fputs("<p class=foot>Generated by sunbench ", f);
    esc(f, SB_VERSION);
    fputs(" &mdash; disk, CPU, memory and network benchmark for classic Sun hardware.</p>\n", f);
    fputs("</div>\n</body>\n</html>\n", f);
    (void)fclose(f);
    return 0;
}

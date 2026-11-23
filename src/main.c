/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * main.c -- sunbench driver.
 *
 * Usage:
 *   sunbench [options] [disk|menu]
 *     --path FILE    scratch file           (default ./sunbench.dat)
 *     --size MB      test file size         (default: auto)
 *     --block KB     I/O block size         (default 64)
 *     --time SEC     duration of each timed test  (default 8)
 *     --sweep        block-size sweep: 4KB..1MB, chart throughput vs block
 *     --hist         print full latency distribution for random tests
 *     --log FILE     append a timestamped clean report to FILE (works w/ TUI)
 *     --csv FILE     append a wide spreadsheet row per sweep (MB/s per block)
 *     --note "TEXT"  free-text column for the CSV (SCSI chip, OBP rev, ...)
 *     --plain        force plain text output (no TUI), for scripting
 *     --menu         force the interactive menu
 *     --full         unattended: run standard test + sweep, log everything
 *     --keep         don't delete the scratch file afterwards
 *
 * Run `sunbench disk` for a direct benchmark (live TUI on a terminal, plain
 * text when piped/redirected/--plain).  Run bare `sunbench` on a terminal to
 * get an arrow-key menu.  --log captures a clean report either way.
 */
#include "port.h"
#include "version.h"
#include "diskio.h"
#include "sysinfo.h"
#include "bench_disk.h"
#include "histogram.h"
#include "progress.h"
#include "tui.h"
#include "keys.h"
#include "cpu.h"
#include "whet.h"
#include "bench_mem.h"
#include "bench_net.h"
#include "html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#define MB (1024L * 1024L)

typedef struct sb_cfg {
    char *path;
    char *logpath;
    char *csvpath;
    char *note;
    unsigned long size_mb;
    int    size_set;
    unsigned long block_kb;
    double secs;
    int    time_set;
    int    hist;
    int    keep;
    int    plain;
    int    method;
    int    passes;         /* repeat each test N times (min/med/max) */
    unsigned long mhz;      /* CPU clock override for --mhz (0 = auto) */
    int    mp;             /* cpu/mem: run the fork-based MP scaling test */
    int    cpus;           /* cpu/mem: process count for MP (0 = auto) */
    unsigned long mem_mb;   /* mem: max working set in MB (0 = auto) */
    char  *htmlpath;        /* --html: self-contained report file */
    int    quiet;           /* --quiet: no stderr progress/notices */
    char  *server;          /* net: sunbench-netd host */
    int    netport;         /* net: control port (default SBN_PORT) */
    unsigned long netmax;   /* net: max UDP payload (0 = interface MTU) */
} sb_cfg;

/* --quiet: progress and success notices are suppressed; warnings and errors
 * always print.  Set once at startup from the config. */
static int g_quiet = 0;

/* --note text, echoed into every log preamble (and the HTML machine card)
 * so the fact it records travels with every artifact, not just the CSVs. */
static char *g_note = (char *)0;

/* Rewrite the HTML report from everything collected so far. */
static void html_flush(c)
    sb_cfg *c;
{
    if (!sb_html_on())
        return;
    if (sb_html_write() == 0) {
        if (!g_quiet)
            fprintf(stderr, "sunbench: HTML report written to %s\n",
                    c->htmlpath ? c->htmlpath : "report");
    } else
        fprintf(stderr, "sunbench: cannot write HTML report\n");
}

/* Collected sweep results, for CSV export.  Each metric index is
 * {seqW,seqR,randR,randW}; with --passes>1 we keep the spread. */
#define SW_CELLS 16
typedef struct sweepdata {
    unsigned long kb[SW_CELLS];
    double        vmin[SW_CELLS][4];
    double        vmed[SW_CELLS][4];
    double        vmax[SW_CELLS][4];
    double        iops[SW_CELLS];    /* median rndR-IOPS */
    int           n;
    int           passes;
} sweepdata;

/* Insertion sort (ascending) for a small array of doubles. */
static void sortd(a, n)
    double *a; int n;
{
    int i, j;
    double x;
    for (i = 1; i < n; i++) {
        x = a[i];
        for (j = i - 1; j >= 0 && a[j] > x; j--)
            a[j + 1] = a[j];
        a[j + 1] = x;
    }
}

/* Run one test `passes` times; return min/median/max MB/s and median IOPS. */
#define SB_MAXPASS 25
static void run_passes(path, method, filesz, blocksz, secs, mode, create,
                       passes, mn, md, mx, iops_med)
    char *path; int method; unsigned long filesz, blocksz; double secs;
    int mode, create, passes;
    double *mn, *md, *mx, *iops_med;
{
    double mb[SB_MAXPASS], ip[SB_MAXPASS];
    sb_reporter rep;
    sb_result r;
    int p;

    if (passes < 1) passes = 1;
    if (passes > SB_MAXPASS) passes = SB_MAXPASS;
    sb_plain_reporter(&rep);
    for (p = 0; p < passes; p++) {
        if (sb_disk_run(path, method, filesz, blocksz, secs, mode, create,
                        &r, (sb_hist *)0, &rep) != 0) {
            mb[p] = 0.0; ip[p] = 0.0;
        } else {
            mb[p] = r.mb_per_s; ip[p] = r.iops;
        }
    }
    sortd(mb, passes);
    sortd(ip, passes);
    *mn = mb[0];
    *mx = mb[passes - 1];
    *md = mb[passes / 2];
    *iops_med = ip[passes / 2];
}

/* Separator + timestamp + system synopsis, written to `out` and identically
 * to `tee` if non-NULL.  So a --log run can show the same preamble on screen
 * as it writes to the file, with a single (matching) timestamp. */
static void log_stamp(out, tee, path)
    FILE *out, *tee;
    char *path;
{
    time_t now;
    char stamp[160];
    (void)time(&now);
    sprintf(stamp,
        "\n========================================================\nsunbench %s   run: %s", SB_VERSION, ctime(&now));
    fputs(stamp, out);
    if (tee) fputs(stamp, tee);
    sb_write_sysinfo(out, tee, path);
    if (g_note && g_note[0]) {
        fprintf(out, "  note   : %s\n", g_note);
        if (tee) fprintf(tee, "  note   : %s\n", g_note);
    }
}

static void usage()
{
    fprintf(stderr,
        "usage: sunbench [options] [disk|cpu|mem|full|menu]   (try `sunbench --help`)\n");
}

static char *help_lines[] = {
    "\n",
    "usage: sunbench [options] [command]\n",
    "\n",
    "commands:\n",
    "  disk           standard four-test run (seq/random x read/write)\n",
    "  cpu            integer, floating-point, Dhrystone, Whetstone\n",
    "  mem            memory bandwidth/latency vs working-set size (cache map)\n",
    "  net            TCP up/down/RTT + UDP size sweep against sunbench-netd\n",
    "                 (build the server with `make netd`, run it on a modern box)\n",
    "  full           unattended full suite: disk+sweep+cpu+mem (+net w/ --server)\n",
    "  menu           interactive arrow-key menu (the default on a terminal)\n",
    "\n",
    "options:\n",
    "  --path FILE    scratch file             (default ./sunbench.dat)\n",
    "  --size MB      test file size           (default: auto)\n",
    "  --block KB     I/O block size           (default 64)\n",
    "  --time SEC     duration of each timed test (default 8)\n",
    "  --sweep        block-size sweep 4KB..1MB instead of the standard run\n",
    "  --diskfull     disk: standard run AND block-size sweep, back to back\n",
    "  --hist         print the full latency distribution (standard run)\n",
    "  --log FILE     append a timestamped report + system synopsis to FILE\n",
    "  --csv FILE     append one wide spreadsheet row per run (per-module schema;\n",
    "                 `full` derives FILE-disk/-cpu/-mem names, schemas differ)\n",
    "  --html FILE    write a self-contained styled HTML report of everything\n",
    "                 run this session (no JS, prints cleanly)\n",
    "  --quiet        no stderr progress or notices -- for scripts and cron\n",
    "  --note TEXT    free-text column for the CSV (OBP rev, SCSI chip, ...)\n",
    "  --mhz N        CPU clock for per-clock normalization (cpu; else auto)\n",
    "  --mp           cpu/mem: fork-based MP scaling across all online CPUs\n",
    "                 (full runs the MP stages automatically on a multiprocessor)\n",
    "  --cpus N       cpu/mem: MP scaling with N processes (implies --mp)\n",
    "  --memsize MB   mem: max working-set size (default: min(256, RAM/4) MB)\n",
    "  --server HOST  net: machine running sunbench-netd\n",
    "  --netport N    net: control port (default 5202)   [alias: --port]\n",
    "  --netmax B     net: max UDP payload bytes (default: interface MTU;\n",
    "                 larger values explore IP fragmentation)\n",
    "  --passes N     repeat each test N times; disk reports min/median/max,\n",
    "                 cpu reports the median rate (tames SD-card / Dhrystone\n",
    "                 run-to-run scatter)\n",
    "  --plain        force plain text (no TUI) -- for scripts and pipes\n",
    "  --menu         force the interactive menu\n",
    "  --full         same as the `full` command\n",
    "  --keep         keep the scratch file instead of deleting it\n",
    "  --version      print the version and exit\n",
    "  --help         print this help and exit\n",
    "\n",
    "notes:\n",
    "  Point --path at a REAL DISK; on Solaris /tmp is tmpfs (RAM) and would\n",
    "  measure memory, not storage.  Run `df -k` and pick a /dev/dsk filesystem.\n",
    "  The live TUI is used automatically on a terminal; output that is piped,\n",
    "  redirected or --plain stays plain text, so scripting is unaffected.\n",
    "\n",
    "examples:\n",
    "  sunbench --path /export/home/sb.dat disk\n",
    "  sunbench --path /export/home/sb.dat --sweep --hist disk\n",
    "  sunbench --path /export/home/sb.dat --diskfull --hist --log d.log --html d.html disk\n",
    "  sunbench --path /export/home/sb.dat --log fleet.log --csv fleet.csv \\\n",
    "           --note \"OBP 1.0; Emulex 2400138; BlueSCSI Pico\" full\n",
    (char *)0
};

static void help()
{
    int i;
    printf("sunbench %s -- disk, CPU and memory benchmark for SunOS 4 / Solaris 2.x on SPARC\n", SB_VERSION);
    for (i = 0; help_lines[i] != (char *)0; i++)
        fputs(help_lines[i], stdout);
}

static void print_latency(out, label, h)
    FILE *out; char *label; sb_hist *h;
{
    if (h->n == 0) return;
    fprintf(out,
           "  %-10s min %.2f  p50 %.2f  p90 %.2f  p99 %.2f  max %.2f  (ms)\n",
           label, h->min_s * 1000.0,
           sb_hist_pct(h, 0.50) * 1000.0, sb_hist_pct(h, 0.90) * 1000.0,
           sb_hist_pct(h, 0.99) * 1000.0, h->max_s * 1000.0);
}

static void print_row(out, r)
    FILE *out; sb_result *r;
{
    if (!r->ok) return;
    fprintf(out, "  %-10s %10.2f %12.1f %10.3f\n",
           r->name, r->mb_per_s, r->iops, r->avg_lat_ms);
}

static void print_detail(out, hist, hr, hw)
    FILE *out; int hist; sb_hist *hr, *hw;
{
    fprintf(out, "\n");
    print_latency(out, "rand read", hr);
    print_latency(out, "rand write", hw);
    if (hist) {
        fprintf(out, "\n");
        sb_hist_bars(hr, "rand read", out);
        fprintf(out, "\n");
        sb_hist_bars(hw, "rand write", out);
    }
}

/* Full plain-text report from a completed run -- used for --log files. */
static void write_report(out, method_str, fdesc, hist, w, r, rr, rw, hr, hw)
    FILE *out; char *method_str, *fdesc; int hist;
    sb_result *w, *r, *rr, *rw; sb_hist *hr, *hw;
{
    fprintf(out, "sunbench -- disk\n");
    fprintf(out, "  method : %s\n", method_str);
    fprintf(out, "  file   : %s\n", fdesc);
    fprintf(out, "  %-10s %10s %12s %10s\n", "test", "MB/s", "IOPS", "lat(ms)");
    print_row(out, w);
    print_row(out, r);
    print_row(out, rr);
    print_row(out, rw);
    print_detail(out, hist, hr, hw);
}

/* Plain four-test run; report goes to `out`, teed to `logf` if non-NULL. */
static void run_normal(out, logf, path, method, filesz, blocksz, secs, hist,
                       passes)
    FILE *out, *logf; char *path; int method;
    unsigned long filesz, blocksz; double secs; int hist, passes;
{
    sb_reporter rep;
    sb_result w, r, rr, rw;
    sb_hist hr, hw;
    char fdesc[300];

    sprintf(fdesc, "%.220s  (%lu MB, %lu KB blocks)", path,
            filesz / MB, blocksz / 1024L);

    if (passes > 1) {
        static char *mlabels[4];
        static int mmodes[4];
        double mn[4], md[4], mx[4], ip;
        int t;
        mlabels[0] = "seq write";  mmodes[0] = SBM_WRITE;
        mlabels[1] = "seq read";   mmodes[1] = 0;
        mlabels[2] = "rand read";  mmodes[2] = SBM_RAND;
        mlabels[3] = "rand write"; mmodes[3] = SBM_RAND | SBM_WRITE;
        fprintf(out, "  (%d passes; MB/s min / median / max)\n", passes);
        fprintf(out, "  %-10s %10s %10s %10s\n", "test", "min", "median", "max");
        if (logf) {
            fprintf(logf, "  (%d passes; MB/s min / median / max)\n", passes);
            fprintf(logf, "  %-10s %10s %10s %10s\n", "test", "min", "median",
                    "max");
        }
        for (t = 0; t < 4; t++) {
            double tsec = (mmodes[t] & SBM_RAND) ? secs : 0.0;
            int create = (mmodes[t] == SBM_WRITE) ? 1 : 0;
            run_passes(path, method, filesz, blocksz, tsec, mmodes[t],
                       create, passes, &mn[t], &md[t], &mx[t], &ip);
            fprintf(out, "  %-10s %10.2f %10.2f %10.2f\n",
                    mlabels[t], mn[t], md[t], mx[t]);
            if (logf)
                fprintf(logf, "  %-10s %10.2f %10.2f %10.2f\n",
                        mlabels[t], mn[t], md[t], mx[t]);
        }
        sb_html_disk_mmm((char *)sb_method_name(method), fdesc, passes,
                         mlabels, mn, md, mx, 4);
        return;
    }

    sb_plain_reporter(&rep);
    fprintf(out, "  %-10s %10s %12s %10s\n", "test", "MB/s", "IOPS", "lat(ms)");
    if (logf)
        fprintf(logf, "  %-10s %10s %12s %10s\n", "test", "MB/s", "IOPS",
                "lat(ms)");

    if (sb_disk_run(path, method, filesz, blocksz, 0.0, SBM_WRITE, 1, &w,
                    (sb_hist *)0, &rep) == 0) {
        print_row(out, &w); if (logf) print_row(logf, &w);
    }
    if (sb_disk_run(path, method, filesz, blocksz, 0.0, 0, 0, &r,
                    (sb_hist *)0, &rep) == 0) {
        print_row(out, &r); if (logf) print_row(logf, &r);
    }
    if (sb_disk_run(path, method, filesz, blocksz, secs, SBM_RAND, 0, &rr,
                    &hr, &rep) == 0) {
        print_row(out, &rr); if (logf) print_row(logf, &rr);
    }
    if (sb_disk_run(path, method, filesz, blocksz, secs, SBM_RAND | SBM_WRITE,
                    0, &rw, &hw, &rep) == 0) {
        print_row(out, &rw); if (logf) print_row(logf, &rw);
    }

    print_detail(out, hist, &hr, &hw);
    if (logf) print_detail(logf, hist, &hr, &hw);

    sb_html_disk_std((char *)sb_method_name(method), fdesc, &w, &r, &rr, &rw);
    if (hr.n > 0)
        sb_html_disk_lat(0, hr.min_s * 1000.0,
                         sb_hist_pct(&hr, 0.50) * 1000.0,
                         sb_hist_pct(&hr, 0.90) * 1000.0,
                         sb_hist_pct(&hr, 0.99) * 1000.0, hr.max_s * 1000.0);
    if (hw.n > 0)
        sb_html_disk_lat(1, hw.min_s * 1000.0,
                         sb_hist_pct(&hw, 0.50) * 1000.0,
                         sb_hist_pct(&hw, 0.90) * 1000.0,
                         sb_hist_pct(&hw, 0.99) * 1000.0, hw.max_s * 1000.0);
    if (hist) {
        if (hr.n > 0) sb_html_disk_hist(0, &hr);
        if (hw.n > 0) sb_html_disk_hist(1, &hw);
    }
}

/* Live-TUI four-test run; also writes a clean report to logf if non-NULL. */
static void run_tui(path, method, filesz, blocksz, secs, hist, method_str,
                    fdesc, logf)
    char *path; int method;
    unsigned long filesz, blocksz; double secs; int hist;
    char *method_str, *fdesc; FILE *logf;
{
    sb_reporter rep;
    sb_result w, r, rr, rw;
    sb_hist hr, hw;

    sb_tui_sysinfo(path);
    sb_tui_reporter(&rep);
    sb_tui_start(method_str, fdesc);
    sb_tui_add("seq write");
    sb_tui_add("seq read");
    sb_tui_add("rand read");
    sb_tui_add("rand write");

    (void)sb_disk_run(path, method, filesz, blocksz, 0.0, SBM_WRITE, 1, &w,
                      (sb_hist *)0, &rep);
    (void)sb_disk_run(path, method, filesz, blocksz, 0.0, 0, 0, &r,
                      (sb_hist *)0, &rep);
    (void)sb_disk_run(path, method, filesz, blocksz, secs, SBM_RAND, 0, &rr,
                      &hr, &rep);
    (void)sb_disk_run(path, method, filesz, blocksz, secs, SBM_RAND | SBM_WRITE,
                      0, &rw, &hw, &rep);

    sb_tui_finish();
    print_detail(stdout, hist, &hr, &hw);
    if (logf)
        write_report(logf, method_str, fdesc, hist, &w, &r, &rr, &rw, &hr, &hw);

    sb_html_disk_std(method_str, fdesc, &w, &r, &rr, &rw);
    if (hr.n > 0)
        sb_html_disk_lat(0, hr.min_s * 1000.0,
                         sb_hist_pct(&hr, 0.50) * 1000.0,
                         sb_hist_pct(&hr, 0.90) * 1000.0,
                         sb_hist_pct(&hr, 0.99) * 1000.0, hr.max_s * 1000.0);
    if (hw.n > 0)
        sb_html_disk_lat(1, hw.min_s * 1000.0,
                         sb_hist_pct(&hw, 0.50) * 1000.0,
                         sb_hist_pct(&hw, 0.90) * 1000.0,
                         sb_hist_pct(&hw, 0.99) * 1000.0, hw.max_s * 1000.0);
    if (hist) {
        if (hr.n > 0) sb_html_disk_hist(0, &hr);
        if (hw.n > 0) sb_html_disk_hist(1, &hw);
    }
}

/* Live-TUI block-size sweep: compact in-place table; log written if logf. */
static void run_sweep_tui(path, method, filesz, cell_secs, method_str, fdesc,
                          logf, sd)
    char *path; int method; unsigned long filesz; double cell_secs;
    char *method_str, *fdesc; FILE *logf; sweepdata *sd;
{
    static unsigned long kbs[] = { 4, 8, 16, 32, 64, 128, 256, 512, 1024 };
    int n = (int)(sizeof(kbs) / sizeof(kbs[0])), i;
    sb_reporter rep;
    sb_result prep, sw, sr, rr, rw;

    sb_tui_sysinfo(path);
    sb_sweep_reporter(&rep);
    sb_sweep_start(method_str, fdesc, kbs, n);

    sb_sweep_preparing(1);
    (void)sb_disk_run(path, method, filesz, 64L * 1024L, 0.0, SBM_WRITE, 1,
                      &prep, (sb_hist *)0, &rep);
    sb_sweep_preparing(0);

    for (i = 0; i < n; i++) {
        unsigned long bs = kbs[i] * 1024L;
        if (bs > filesz) continue;
        sb_sweep_cell(i);
        (void)sb_disk_run(path, method, filesz, bs, cell_secs, SBM_WRITE, 0,
                          &sw, (sb_hist *)0, &rep);
        (void)sb_disk_run(path, method, filesz, bs, cell_secs, 0, 0,
                          &sr, (sb_hist *)0, &rep);
        (void)sb_disk_run(path, method, filesz, bs, cell_secs, SBM_RAND, 0,
                          &rr, (sb_hist *)0, &rep);
        (void)sb_disk_run(path, method, filesz, bs, cell_secs,
                          SBM_RAND | SBM_WRITE, 0, &rw, (sb_hist *)0, &rep);
    }

    sb_sweep_finish();
    if (logf)
        sb_sweep_write_log(logf);
    if (sd) {
        double tmp[SW_CELLS][4], iops[SW_CELLS];
        int m, k, j;
        m = sb_sweep_export(sd->kb, tmp, iops, SW_CELLS);
        sd->n = m; sd->passes = 1;
        for (k = 0; k < m; k++) {
            for (j = 0; j < 4; j++)
                sd->vmin[k][j] = sd->vmed[k][j] = sd->vmax[k][j] = tmp[k][j];
            sd->iops[k] = iops[k];
        }
    }
}

/* Sweep block size across 4KB..1MB.  With passes>1, each cell's four tests
 * are repeated and min/median/max recorded (the table shows median). */
static int run_sweep(out, logf, path, method, filesz, secs, sd, passes)
    FILE *out, *logf; char *path; int method; unsigned long filesz;
    double secs; sweepdata *sd; int passes;
{
    static unsigned long kbs[] = { 4, 8, 16, 32, 64, 128, 256, 512, 1024 };
    int ncell = (int)(sizeof(kbs) / sizeof(kbs[0]));
    int i, k;
    sb_reporter rep;
    sb_result prep;
    double wmn, wmd, wmx, rmn, rmd, rmx;
    double amn, amd, amx, xmn, xmd, xmx, ip, dummy;

    sb_plain_reporter(&rep);
    if (sd) { sd->n = 0; sd->passes = passes; }
    if (!g_quiet)
        fprintf(stderr, "  preparing %lu MB test file...\n", filesz / MB);
    if (sb_disk_run(path, method, filesz, 64L * 1024L, 0.0, SBM_WRITE, 1,
                    &prep, (sb_hist *)0, &rep) != 0)
        return -1;

    if (passes > 1) {
        fprintf(out, "  (%d passes per cell; table shows median, min/max in CSV)\n", passes);
        if (logf)
            fprintf(logf, "  (%d passes per cell; table shows median, min/max in CSV)\n", passes);
    }
    fprintf(out, "  %8s %8s %8s %8s %8s %10s\n",
           "blk(KB)", "seqW", "seqR", "randR", "randW", "rndR-IOPS");
    if (logf)
        fprintf(logf, "  %8s %8s %8s %8s %8s %10s\n",
               "blk(KB)", "seqW", "seqR", "randR", "randW", "rndR-IOPS");
    for (i = 0; i < ncell; i++) {
        unsigned long bs = kbs[i] * 1024L;
        if (bs > filesz) continue;
        if (!g_quiet)
            fprintf(stderr, "  [%d/%d] block %lu KB\n", i + 1, ncell, kbs[i]);
        run_passes(path, method, filesz, bs, secs, SBM_WRITE, 0, passes,
                   &wmn, &wmd, &wmx, &dummy);
        run_passes(path, method, filesz, bs, secs, 0, 0, passes,
                   &rmn, &rmd, &rmx, &dummy);
        run_passes(path, method, filesz, bs, secs, SBM_RAND, 0, passes,
                   &amn, &amd, &amx, &ip);
        run_passes(path, method, filesz, bs, secs, SBM_RAND | SBM_WRITE, 0,
                   passes, &xmn, &xmd, &xmx, &dummy);
        fprintf(out, "  %8lu %8.2f %8.2f %8.2f %8.2f %10.1f\n",
               kbs[i], wmd, rmd, amd, xmd, ip);
        if (logf)
            fprintf(logf, "  %8lu %8.2f %8.2f %8.2f %8.2f %10.1f\n",
                   kbs[i], wmd, rmd, amd, xmd, ip);
        (void)fflush(out);
        if (sd && sd->n < SW_CELLS) {
            k = sd->n;
            sd->kb[k] = kbs[i];
            sd->vmin[k][0] = wmn; sd->vmed[k][0] = wmd; sd->vmax[k][0] = wmx;
            sd->vmin[k][1] = rmn; sd->vmed[k][1] = rmd; sd->vmax[k][1] = rmx;
            sd->vmin[k][2] = amn; sd->vmed[k][2] = amd; sd->vmax[k][2] = amx;
            sd->vmin[k][3] = xmn; sd->vmed[k][3] = xmd; sd->vmax[k][3] = xmx;
            sd->iops[k] = ip;
            sd->n++;
        }
    }
    return 0;
}

/* Quote a CSV text field (model strings like "SUNW,Sun_4_60" contain commas). */
static void csv_str(f, s)
    FILE *f; char *s;
{
    char *p;
    putc('"', f);
    for (p = s; *p; p++) {
        if (*p == '"') putc('"', f);
        putc(*p, f);
    }
    putc('"', f);
}

/* Append one wide, block-major sweep row (metadata + per-block MB/s). */
static void write_csv(csvpath, path, note, sd, file_mb, cell_s)
    char *csvpath, *path, *note; sweepdata *sd;
    unsigned long file_mb; double cell_s;
{
    FILE *f;
    long sz;
    int i, j;
    char host[64], os[80], model[80], dev[128], drive[160], fstype[32], ts[32];
    time_t now;

    if (sd->n == 0) return;
    f = fopen(csvpath, "a");
    if (f == (FILE *)0) {
        fprintf(stderr, "sunbench: cannot write %s\n", csvpath);
        return;
    }
    (void)fseek(f, 0L, SEEK_END);
    sz = ftell(f);

    if (sz <= 0) {                 /* header only for a fresh file */
        static char *mets[4];
        mets[0] = "seqW"; mets[1] = "seqR"; mets[2] = "randR"; mets[3] = "randW";
        fprintf(f, "timestamp,sunbench_ver,host,os,model,ram_mb,disk_dev,drive,fstype,file_mb,cell_s,passes,note");
        for (i = 0; i < sd->n; i++)
            for (j = 0; j < 4; j++) {
                if (sd->passes > 1)
                    fprintf(f, ",%luK_%s_min,%luK_%s_med,%luK_%s_max",
                            sd->kb[i], mets[j], sd->kb[i], mets[j],
                            sd->kb[i], mets[j]);
                else
                    fprintf(f, ",%luK_%s_MBps", sd->kb[i], mets[j]);
            }
        fprintf(f, "\n");
    }

    (void)time(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    host[0] = os[0] = model[0] = dev[0] = drive[0] = fstype[0] = '\0';
    sb_host(host, (int)sizeof(host));
    sb_os(os, (int)sizeof(os));
    sb_model(model, (int)sizeof(model));
    sb_fs_type(path, fstype, (int)sizeof(fstype));
    sb_fs_device(path, dev, (int)sizeof(dev));
    if (dev[0]) sb_disk_model(dev, drive, (int)sizeof(drive));

    csv_str(f, ts);          putc(',', f);
    csv_str(f, SB_VERSION);  putc(',', f);
    csv_str(f, host);    putc(',', f);
    csv_str(f, os);      putc(',', f);
    csv_str(f, model);   putc(',', f);
    fprintf(f, "%lu,", sb_physmem_mb());
    csv_str(f, dev);     putc(',', f);
    csv_str(f, drive);   putc(',', f);
    csv_str(f, fstype);  putc(',', f);
    fprintf(f, "%lu,%.0f,%d,", file_mb, cell_s, sd->passes);
    csv_str(f, note ? note : "");
    for (i = 0; i < sd->n; i++)
        for (j = 0; j < 4; j++) {
            if (sd->passes > 1)
                fprintf(f, ",%.2f,%.2f,%.2f",
                        sd->vmin[i][j], sd->vmed[i][j], sd->vmax[i][j]);
            else if (sd->vmed[i][j] < 0.0)
                fprintf(f, ",");
            else
                fprintf(f, ",%.2f", sd->vmed[i][j]);
        }
    fprintf(f, "\n");
    (void)fclose(f);
    if (!g_quiet)
        fprintf(stderr, "sunbench: CSV row appended to %s\n", csvpath);
}

/* Append one CPU row: metadata + clock + build target + per-kernel figures.
 * Its own schema (distinct from the disk sweep CSV), so cross-machine CPU
 * comparisons drop straight into a spreadsheet.  rates[] holds median
 * iterations/second per kernel (-1 if a kernel did not run). */
static void write_cpu_csv(csvpath, path, note, mhz, passes, build, rates)
    char *csvpath, *path, *note; unsigned long mhz; int passes;
    char *build; double *rates;
{
    FILE *f;
    long sz;
    double alu, muldiv, fp, dhry, whet;
    char host[64], os[80], model[80], ts[32];
    time_t now;

    f = fopen(csvpath, "a");
    if (f == (FILE *)0) {
        fprintf(stderr, "sunbench: cannot write %s\n", csvpath);
        return;
    }
    (void)fseek(f, 0L, SEEK_END);
    sz = ftell(f);
    if (sz <= 0)                   /* header only for a fresh file */
        fprintf(f, "timestamp,sunbench_ver,host,os,model,ram_mb,mhz,build,passes,note,alu_Miterps,muldiv_Miterps,fp_Miterps,dhrystones_s,dhry_vaxmips,whet_mwips,saxpy_mflops,daxpy_mflops,saxpy_mem_mflops,daxpy_mem_mflops\n");

    (void)time(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    host[0] = os[0] = model[0] = '\0';
    sb_host(host, (int)sizeof(host));
    sb_os(os, (int)sizeof(os));
    sb_model(model, (int)sizeof(model));

    csv_str(f, ts);          putc(',', f);
    csv_str(f, SB_VERSION);  putc(',', f);
    csv_str(f, host);        putc(',', f);
    csv_str(f, os);          putc(',', f);
    csv_str(f, model);       putc(',', f);
    fprintf(f, "%lu,%lu,", sb_physmem_mb(), mhz);
    csv_str(f, build ? build : "");  putc(',', f);
    fprintf(f, "%d,", passes);
    csv_str(f, note ? note : "");

    alu    = rates[SBK_INT_ALU];
    muldiv = rates[SBK_INT_MULDIV];
    fp     = rates[SBK_FP_DOUBLE];
    dhry   = rates[SBK_DHRYSTONE];
    whet   = rates[SBK_WHETSTONE];
    /* alu/muldiv/fp in Miter/s; dhrystones/s and its VAX-MIPS; whet in MWIPS. */
    if (alu >= 0.0)    fprintf(f, ",%.3f", alu / 1.0e6);    else putc(',', f);
    if (muldiv >= 0.0) fprintf(f, ",%.3f", muldiv / 1.0e6); else putc(',', f);
    if (fp >= 0.0)     fprintf(f, ",%.3f", fp / 1.0e6);     else putc(',', f);
    if (dhry >= 0.0)   fprintf(f, ",%.0f,%.2f", dhry, dhry / 1757.0);
    else               fprintf(f, ",,");
    if (whet >= 0.0)   fprintf(f, ",%.2f", whet * WHET_INSTR_PER_LOOP / 1.0e6);
    else               putc(',', f);
    if (rates[SBK_SAXPY] >= 0.0)
        fprintf(f, ",%.2f", rates[SBK_SAXPY] * SBK_XPY_FLOPS / 1.0e6);
    else putc(',', f);
    if (rates[SBK_DAXPY] >= 0.0)
        fprintf(f, ",%.2f", rates[SBK_DAXPY] * SBK_XPY_FLOPS / 1.0e6);
    else putc(',', f);
    if (rates[SBK_SAXPY_MEM] >= 0.0)
        fprintf(f, ",%.2f",
                rates[SBK_SAXPY_MEM] * sb_cpu_xpy_flops(SBK_SAXPY_MEM)
                / 1.0e6);
    else putc(',', f);
    if (rates[SBK_DAXPY_MEM] >= 0.0)
        fprintf(f, ",%.2f",
                rates[SBK_DAXPY_MEM] * sb_cpu_xpy_flops(SBK_DAXPY_MEM)
                / 1.0e6);
    else putc(',', f);
    fprintf(f, "\n");
    (void)fclose(f);
    if (!g_quiet)
        fprintf(stderr, "sunbench: CPU CSV row appended to %s\n", csvpath);
}

/* Compute file size and dispatch one run (TUI or plain, disk or sweep). */
static void run_action(c, sweep, interactive)
    sb_cfg *c; int sweep, interactive;
{
    unsigned long size_mb, blocksz, filesz, free_mb;
    double cell_secs;
    int use_tui;
    char fdesc[300];
    sweepdata sd;

    sd.n = 0;
    blocksz = c->block_kb * 1024L;
    size_mb = c->size_mb;
    if (!c->size_set) {
        if (sweep) size_mb = 256;   /* span a representative disk region */
        else if (c->method == SB_DIRECTIO) size_mb = 512;
        else {
            unsigned long ram = sb_physmem_mb();
            if (ram == 0) {
                size_mb = 96;
                fprintf(stderr, "sunbench: cannot read RAM size; using %lu MB (override with --size)\n", size_mb);
            } else size_mb = ram * 2 + ram / 2;
            /* 2.5x RAM: at the old 1.25x, each test inherited ~75% of the
             * file cached by the PREVIOUS test (cross-test carryover the
             * cold-tour discipline cannot prevent); at 2.5x the carryover
             * fraction is small and ages out mid-test. */
        }
    }
    free_mb = sb_freespace_mb(c->path);
    if (free_mb > 0 && size_mb > free_mb / 2) {
        fprintf(stderr, "sunbench: capping test file %lu -> %lu MB (only %lu MB free)\n", size_mb, free_mb / 2, free_mb);
        size_mb = free_mb / 2;
    }
    /* The file size in bytes is an unsigned long and offsets are off_t, so
     * a 32-bit build tops out below 4 GB (the byte count wraps -- 8192 MB
     * became 0) and, with a 32-bit off_t, at 2 GB.  Refuse an explicit
     * --size past the limit; cap the automatic RAM-based size to it. */
    {
        unsigned long lim;
        lim = ((unsigned long)-1) / MB;
        if (sizeof(off_t) <= 4 && lim > 2047L)
            lim = 2047L;
        if (size_mb > lim) {
            if (c->size_set) {
                fprintf(stderr, "sunbench: --size %lu MB exceeds this build's %lu MB file limit\n", size_mb, lim);
                fprintf(stderr, "          (a 32-bit build; for larger files rebuild 64-bit, e.g. CC=\"gcc -m64\")\n");
                return;
            }
            fprintf(stderr, "sunbench: capping test file %lu -> %lu MB (this build's file limit)\n", size_mb, lim);
            size_mb = lim;
        }
    }
    /* A small file means random I/O only scatters over a small region, which
     * may not reflect real-world random access across a full disk. */
    if (size_mb <= 64)
        fprintf(stderr,
"sunbench: NOTE -- test file is only %lu MB, so random I/O spans a small\n          region; results may not reflect real-world random access across a\n          full disk.  Use a larger --size if space allows (256 MB+ ideal).\n",
            size_mb);
    if (sweep) filesz = size_mb * MB;
    else       filesz = (size_mb * MB / blocksz) * blocksz;
    cell_secs = c->time_set ? c->secs : 4.0;

    /* --passes>1 is a characterization run (min/median/max) -- plain only. */
    use_tui = interactive && !c->plain && c->passes <= 1 && sb_tui_supported();

    if (use_tui) {
        FILE *logf = c->logpath ? fopen(c->logpath, "a") : (FILE *)0;
        if (c->logpath && logf == (FILE *)0)
            fprintf(stderr, "sunbench: cannot write %s\n", c->logpath);
        if (logf) log_stamp(logf, (FILE *)0, c->path);  /* screen has the TUI */
        if (sweep) {
            sprintf(fdesc, "%.220s   %lu MB, %.0fs per cell",
                    c->path, filesz / MB, cell_secs);
            run_sweep_tui(c->path, c->method, filesz, cell_secs,
                          (char *)sb_method_name(c->method), fdesc, logf, &sd);
        } else {
            sprintf(fdesc, "%.220s   %lu MB, %lu KB blocks",
                    c->path, filesz / MB, c->block_kb);
            run_tui(c->path, c->method, filesz, blocksz, c->secs, c->hist,
                    (char *)sb_method_name(c->method), fdesc, logf);
        }
        if (logf) {
            (void)fclose(logf);
            if (!g_quiet)
                fprintf(stderr, "sunbench: report written to %s\n", c->logpath);
        }
    } else {
        /* Report to stdout, teed to the --log file if given. */
        FILE *logf = c->logpath ? fopen(c->logpath, "a") : (FILE *)0;
        int j;
        FILE *dst[2];
        if (c->logpath && logf == (FILE *)0)
            fprintf(stderr, "sunbench: cannot write %s\n", c->logpath);
        if (logf) log_stamp(stdout, logf, c->path);  /* preamble to both */

        dst[0] = stdout; dst[1] = logf;
        for (j = 0; j < 2; j++) {
            FILE *o = dst[j];
            if (o == (FILE *)0) continue;
            fprintf(o, "sunbench -- disk%s\n", sweep ? " block-size sweep" : "");
            fprintf(o, "  method : %s\n", sb_method_name(c->method));
            if (sweep)
                fprintf(o, "  file   : %s  (%lu MB, %.0fs per cell)\n",
                       c->path, filesz / MB, cell_secs);
            else
                fprintf(o, "  file   : %s  (%lu MB, %lu KB blocks)\n",
                       c->path, filesz / MB, c->block_kb);
        }
        if (sweep) run_sweep(stdout, logf, c->path, c->method, filesz,
                             cell_secs, &sd, c->passes);
        else       run_normal(stdout, logf, c->path, c->method, filesz, blocksz,
                              c->secs, c->hist, c->passes);
        if (logf) {
            (void)fclose(logf);
            if (!g_quiet)
                fprintf(stderr, "sunbench: report also written to %s\n",
                        c->logpath);
        }
    }

    /* CSV row for the spreadsheet -- sweeps only (the tabular use case). */
    if (sweep && c->csvpath && sd.n > 0)
        write_csv(c->csvpath, c->path, c->note, &sd, filesz / MB, cell_secs);

    if (sweep && sd.n > 0) {
        sprintf(fdesc, "%.220s  (%lu MB, %.0fs per cell)",
                c->path, filesz / MB, cell_secs);
        sb_html_disk_sweep((char *)sb_method_name(c->method), fdesc,
                           sd.passes, sd.kb, sd.vmed, sd.iops, sd.n);
    }
    html_flush(c);

    if (!c->keep)
        (void)unlink(c->path);
}

/* Print one CPU result line to a stream, in the kernel's native unit, given
 * the rate `ips` (iterations/second) and kernel name.  `spread` >= 0 appends a
 * "+/- X%" scatter figure (from --passes); pass a negative value to omit it. */
static void cpu_line(f, k, name, ips, mhz, spread)
    FILE *f; int k; char *name; double ips; unsigned long mhz; double spread;
{
    fprintf(f, "  %-11s", name);
    if (k == SBK_DHRYSTONE)
        fprintf(f, " %12.0f Dhrystones/s  %7.2f VAX-MIPS", ips, ips / 1757.0);
    else if (k == SBK_WHETSTONE)
        fprintf(f, " %12.2f MWIPS", ips * WHET_INSTR_PER_LOOP / 1.0e6);
    else if (SBK_IS_XPY(k))
        fprintf(f, " %12.2f MFLOPS", ips * sb_cpu_xpy_flops(k) / 1.0e6);
    else
        fprintf(f, " %12.3f Miter/s", ips / 1.0e6);
    if (mhz && SBK_IS_XPY(k))
        fprintf(f, "   %8.4f flop/clk",
                ips * sb_cpu_xpy_flops(k) / ((double)mhz * 1.0e6));
    else if (mhz && k != SBK_DHRYSTONE && k != SBK_WHETSTONE)
        fprintf(f, "   %8.4f/clk", ips / ((double)mhz * 1.0e6));
    if (spread >= 0.0)
        fprintf(f, "   +/-%4.1f%%", spread);
    fprintf(f, "\n");
}

/* CPU kernels.  Report goes to `out`, and is teed to `logf` if non-NULL.
 * With passes>1 each kernel is run repeatedly and the MEDIAN rate is reported,
 * plus a spread figure -- the classic macrobenchmarks (Dhrystone, Whetstone)
 * have ~10% cache-sensitive scatter that a single run can't smooth out. */
static void run_cpu(out, logf, secs, mhz, passes, rates)
    FILE *out, *logf; double secs; unsigned long mhz; int passes; double *rates;
{
    sb_reporter rep;
    sb_cpu_result r;
    char *build;
    int k, p;

    if (passes < 1) passes = 1;
    if (passes > SB_MAXPASS) passes = SB_MAXPASS;
    if (rates)
        for (k = 0; k < SBK_NKERNELS; k++) rates[k] = -1.0;
    sb_plain_reporter(&rep);
    build = (char *)sb_cpu_build_id();
    fprintf(out, "  build  : %s\n", build);
    if (logf) fprintf(logf, "  build  : %s\n", build);
    if (mhz) {
        fprintf(out, "  clock  : %lu MHz\n", mhz);
        if (logf) fprintf(logf, "  clock  : %lu MHz\n", mhz);
    }
    if (passes > 1) {
        fprintf(out, "  (%d passes; median rate, +/- = half the min..max spread)\n", passes);
        if (logf)
            fprintf(logf, "  (%d passes; median rate, +/- = half the min..max spread)\n", passes);
    }

    for (k = 0; k < SBK_NKERNELS; k++) {
        double ips[SB_MAXPASS], med, spread;

        if (passes <= 1) {
            if (sb_cpu_run(k, secs, &r, &rep) != 0) continue;
            cpu_line(out, k, r.name, r.iters_per_s, mhz, -1.0);
            if (logf) cpu_line(logf, k, r.name, r.iters_per_s, mhz, -1.0);
            if (rates) rates[k] = r.iters_per_s;
            continue;
        }
        for (p = 0; p < passes; p++) {
            if (sb_cpu_run(k, secs, &r, &rep) != 0) { ips[p] = 0.0; continue; }
            ips[p] = r.iters_per_s;
            if (!g_quiet)
                fprintf(stderr, "  %-11s pass %d/%d\r", r.name, p + 1, passes);
        }
        if (!g_quiet)
            fprintf(stderr, "                              \r");
        sortd(ips, passes);
        med = ips[passes / 2];
        spread = med > 0.0
            ? (ips[passes - 1] - ips[0]) / (2.0 * med) * 100.0 : 0.0;
        cpu_line(out, k, (char *)sb_cpu_kernel_name(k), med, mhz, spread);
        if (logf)
            cpu_line(logf, k, (char *)sb_cpu_kernel_name(k), med, mhz, spread);
        if (rates) rates[k] = med;
    }
}

/* One line of the MP scaling table (teed to logf if non-NULL).  Values are
 * shown in each kernel's native unit -- a ?axpy "iteration" is a whole
 * vector pass, so raw Miter/s would print as 0.000 there. */
static void cpu_mp_line(out, logf, k, m)
    FILE *out, *logf; int k; sb_cpu_mp *m;
{
    int j;
    double s, a;
    char *unit;
    FILE *dst[2];

    s = m->single_ips;
    a = m->agg_ips;
    if (SBK_IS_XPY(k)) {
        s = s * sb_cpu_xpy_flops(k) / 1.0e6;
        a = a * sb_cpu_xpy_flops(k) / 1.0e6;
        unit = "MFLOPS";
    } else if (k == SBK_WHETSTONE) {
        s = s * WHET_INSTR_PER_LOOP / 1.0e6;
        a = a * WHET_INSTR_PER_LOOP / 1.0e6;
        unit = "MWIPS";
    } else if (k == SBK_DHRYSTONE) {
        s = s / 1.0e3;
        a = a / 1.0e3;
        unit = "kDhry/s";
    } else {
        s = s / 1.0e6;
        a = a / 1.0e6;
        unit = "Miter/s";
    }
    dst[0] = out; dst[1] = logf;
    for (j = 0; j < 2; j++) {
        FILE *f = dst[j];
        if (f == (FILE *)0) continue;
        fprintf(f, "  %-11s %10.2f %10.2f %9.2fx %9.1f%%  %s\n",
                m->name, s, a, m->speedup, m->efficiency * 100.0, unit);
    }
}

/* MP scaling run: baseline vs N concurrent processes for each kernel.  All
 * figures are wall-clock Miter/s; the point is the speedup and efficiency,
 * which are unit-independent, so every kernel shares one table. */
static void run_cpu_mp(out, logf, secs, ncpus)
    FILE *out, *logf; double secs; int ncpus;
{
    sb_cpu_mp rows[SBK_NKERNELS];
    int j, k, nrows;
    FILE *dst[2];

    dst[0] = out; dst[1] = logf;
    for (j = 0; j < 2; j++) {
        FILE *f = dst[j];
        if (f == (FILE *)0) continue;
        fprintf(f, "  %-11s %10s %10s %10s %10s\n",
                "test", "1-proc", "N-proc", "speedup", "scaling");
    }
    nrows = 0;
    for (k = 0; k < SBK_NKERNELS; k++) {
        if (!g_quiet)
            fprintf(stderr, "  [%d/%d] %s: 1 proc + %d procs...\n",
                    k + 1, SBK_NKERNELS, sb_cpu_kernel_name(k), ncpus);
        if (sb_cpu_run_mp(k, secs, ncpus, &rows[nrows]) != 0) continue;
        cpu_mp_line(out, logf, k, &rows[nrows]);
        nrows++;
    }
    if (nrows > 0)
        sb_html_cpu_mp(ncpus, rows, nrows);
}

/* Live CPU dashboard.  The screen shows the animated table; a plain report is
 * written to logf (as with the disk TUI), and a CSV row if --csv was given.
 * Each kernel's live bar is driven by the reporter's wall-clock ticks, but the
 * finished figure is the authoritative CPU-time rate from sb_cpu_run. */
static void run_cpu_tui(c, mhz, logf)
    sb_cfg *c; unsigned long mhz; FILE *logf;
{
    sb_reporter rep;
    sb_cpu_result r;
    double rates[SBK_NKERNELS];
    char *build;
    int k;

    build = (char *)sb_cpu_build_id();
    sb_tui_sysinfo(c->path);
    sb_cpu_tui_reporter(&rep);
    sb_cpu_tui_start(build, mhz);
    for (k = 0; k < SBK_NKERNELS; k++)
        sb_cpu_tui_add((char *)sb_cpu_kernel_name(k), k);

    for (k = 0; k < SBK_NKERNELS; k++) {
        rates[k] = -1.0;
        if (sb_cpu_run(k, c->secs, &r, &rep) != 0) continue;
        rates[k] = r.iters_per_s;
        sb_cpu_tui_set(k, r.iters_per_s);   /* final compute-time rate */
    }
    sb_cpu_tui_finish();
    sb_html_cpu(build, mhz, 1, rates);

    if (logf) {                             /* clean report for the log file */
        fprintf(logf, "sunbench -- cpu\n");
        fprintf(logf, "  build  : %s\n", build);
        if (mhz) fprintf(logf, "  clock  : %lu MHz\n", mhz);
        for (k = 0; k < SBK_NKERNELS; k++)
            if (rates[k] >= 0.0)
                cpu_line(logf, k, (char *)sb_cpu_kernel_name(k), rates[k],
                         mhz, -1.0);
    }
    if (c->csvpath)
        write_cpu_csv(c->csvpath, c->path, c->note, mhz, c->passes, build,
                      rates);
}

/* Top-level CPU run: report to stdout, and tee a timestamped copy to --log. */
static void run_cpu_top(c)
    sb_cfg *c;
{
    FILE *logf = c->logpath ? fopen(c->logpath, "a") : (FILE *)0;
    unsigned long mhz;
    int use_tui;

    if (c->logpath && logf == (FILE *)0)
        fprintf(stderr, "sunbench: cannot write %s\n", c->logpath);
    mhz = c->mhz ? c->mhz : sb_cpu_mhz();

    /* MP and --passes force plain output (like the disk characterization run). */
    use_tui = !c->mp && c->cpus == 0 && !c->plain && c->passes <= 1
              && sb_tui_supported();

    if (c->mp || c->cpus > 0) {
        int ncpus = c->cpus > 0 ? c->cpus : sb_cpu_ncpus();
        char *build = (char *)sb_cpu_build_id();
        if (logf) log_stamp(stdout, logf, c->path);   /* preamble to both */
        fprintf(stdout, "sunbench -- cpu (MP: %d process%s, wall-clock rates)\n",
                ncpus, ncpus == 1 ? "" : "es");
        fprintf(stdout, "  build  : %s\n", build);
        if (logf) {
            fprintf(logf, "sunbench -- cpu (MP: %d process%s, wall-clock rates)\n", ncpus, ncpus == 1 ? "" : "es");
            fprintf(logf, "  build  : %s\n", build);
        }
        if (ncpus == 1)
            fprintf(stderr, "sunbench: NOTE -- only 1 CPU detected; the scaling column is trivially 100%%.\n");
        run_cpu_mp(stdout, logf, c->secs, ncpus);
    } else if (use_tui) {
        if (logf) log_stamp(logf, (FILE *)0, c->path);  /* screen has the TUI */
        run_cpu_tui(c, mhz, logf);
    } else {
        double rates[SBK_NKERNELS];
        if (logf) log_stamp(stdout, logf, c->path);     /* preamble to both */
        fprintf(stdout, "sunbench -- cpu\n");
        if (logf) fprintf(logf, "sunbench -- cpu\n");
        run_cpu(stdout, logf, c->secs, mhz, c->passes, rates);
        if (c->csvpath)
            write_cpu_csv(c->csvpath, c->path, c->note, mhz, c->passes,
                          (char *)sb_cpu_build_id(), rates);
        sb_html_cpu((char *)sb_cpu_build_id(), mhz, c->passes, rates);
    }
    if (logf) {
        (void)fclose(logf);
        if (!g_quiet)
            fprintf(stderr, "sunbench: report also written to %s\n",
                    c->logpath);
    }
    html_flush(c);
}

/* ====================== memory benchmark driver ======================== */

#define MEM_CELLS 20

/* One row of the memory sweep table (val[k] < 0 = not measured). */
static void mem_row(f, kb, v)
    FILE *f; unsigned long kb; double *v;
{
    int k;
    fprintf(f, "  %8lu", kb);
    for (k = 0; k < SBMEM_NKERNELS - 1; k++) {
        if (v[k] < 0.0) fprintf(f, " %8s", "-");
        else            fprintf(f, " %8.1f", v[k]);
    }
    if (v[SBMEM_LAT] < 0.0) fprintf(f, " %9s", "-");
    else                    fprintf(f, " %9.1f", v[SBMEM_LAT]);
    fprintf(f, "\n");
}

static void mem_header(f, minkb, maxkb, cell_s, passes)
    FILE *f; unsigned long minkb, maxkb; double cell_s; int passes;
{
    fprintf(f, "sunbench -- memory (working-set sweep)\n");
    fprintf(f, "  set    : %lu KB .. %lu KB, %.1fs per kernel-cell\n",
            minkb, maxkb, cell_s);
    if (passes > 1)
        fprintf(f, "  (%d passes per cell; table shows the median)\n", passes);
    fprintf(f, "  %8s %8s %8s %8s %8s %9s\n",
            "set(KB)", "read", "read8", "write", "copy", "lat(ns)");
    fprintf(f, "  %8s %8s %8s %8s %8s %9s\n",
            "", "MB/s", "MB/s", "MB/s", "MB/s", "");
}

/* Append one wide CSV row: metadata, then per-size read/read8/write/copy/lat. */
static void write_mem_csv(csvpath, note, cell_s, passes, kb, val, n)
    char *csvpath, *note; double cell_s; int passes;
    unsigned long *kb; double (*val)[SBMEM_NKERNELS]; int n;
{
    FILE *f;
    long sz;
    int i, k;
    char host[64], os[80], model[80], ts[32];
    time_t now;
    static char *cols[SBMEM_NKERNELS];

    cols[0] = "read_MBps"; cols[1] = "read8_MBps"; cols[2] = "write_MBps";
    cols[3] = "copy_MBps"; cols[4] = "lat_ns";

    f = fopen(csvpath, "a");
    if (f == (FILE *)0) {
        fprintf(stderr, "sunbench: cannot write %s\n", csvpath);
        return;
    }
    (void)fseek(f, 0L, SEEK_END);
    sz = ftell(f);
    if (sz <= 0) {
        fprintf(f, "timestamp,sunbench_ver,host,os,model,ram_mb,cell_s,passes,note");
        for (i = 0; i < n; i++)
            for (k = 0; k < SBMEM_NKERNELS; k++)
                fprintf(f, ",%luK_%s", kb[i], cols[k]);
        fprintf(f, "\n");
    }

    (void)time(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    host[0] = os[0] = model[0] = '\0';
    sb_host(host, (int)sizeof(host));
    sb_os(os, (int)sizeof(os));
    sb_model(model, (int)sizeof(model));

    csv_str(f, ts);          putc(',', f);
    csv_str(f, SB_VERSION);  putc(',', f);
    csv_str(f, host);        putc(',', f);
    csv_str(f, os);          putc(',', f);
    csv_str(f, model);       putc(',', f);
    fprintf(f, "%lu,%.1f,%d,", sb_physmem_mb(), cell_s, passes);
    csv_str(f, note ? note : "");
    for (i = 0; i < n; i++)
        for (k = 0; k < SBMEM_NKERNELS; k++) {
            if (val[i][k] < 0.0) fprintf(f, ",");
            else if (k == SBMEM_LAT) fprintf(f, ",%.1f", val[i][k]);
            else fprintf(f, ",%.2f", val[i][k]);
        }
    fprintf(f, "\n");
    (void)fclose(f);
    if (!g_quiet)
        fprintf(stderr, "sunbench: memory CSV row appended to %s\n", csvpath);
}

/* Memory benchmark: working-set sweep (or MP scaling with --mp/--cpus). */
static void run_mem_top(c)
    sb_cfg *c;
{
    FILE *logf = c->logpath ? fopen(c->logpath, "a") : (FILE *)0;
    unsigned long maxsz, ram, kb[MEM_CELLS];
    double val[MEM_CELLS][SBMEM_NKERNELS];
    double cell_s;
    char *buf;
    int n, i, k, p, passes, use_tui;

    if (c->logpath && logf == (FILE *)0)
        fprintf(stderr, "sunbench: cannot write %s\n", c->logpath);

    /* Max working set: past the biggest cache, within a small box's RAM.
     * The ceiling was 16 MB -- ample for every SPARC E-cache -- but modern
     * last-level caches reach 16-32 MB (an M4's shared L2 is 16 MB), so the
     * whole sweep stayed in cache and "DRAM" read 95 GB/s at 11 ns.  256 MB
     * clears them all; RAM/4 still bounds small machines, which see no
     * change at 64 MB of RAM or less. */
    ram = sb_physmem_mb();
    if (c->mem_mb)
        maxsz = c->mem_mb * MB;
    else if (ram) {
        unsigned long m = ram / 4;
        if (m > 256) m = 256;
        if (m < 1)  m = 1;
        maxsz = m * MB;
    } else {
        maxsz = 4 * MB;
        fprintf(stderr, "sunbench: cannot read RAM size; using a 4 MB max working set (override with --memsize)\n");
    }
    if (maxsz < 64L * 1024L) maxsz = 64L * 1024L;

    if (c->mp || c->cpus > 0) {
        /* MP scaling: N concurrent processes, each with a PRIVATE buffer of
         * the sweep's max size -- shrink it if N buffers won't fit in RAM.
         * Five experiments: a 16 KB read (in-cache control -- private L1s
         * should scale flat at N.00x; if not, the run is noisy), read /
         * write / copy at DRAM size (write and copy add the coherency and
         * two-way traffic a shared bus or crossbar handles differently),
         * and the pointer chase alone vs under N-1 streaming readers --
         * the loaded-latency figure. */
        static struct { int kern; unsigned long kb; char *label; } mpjob[] = {
            { SBMEM_READ,  16, "read 16K" },
            { SBMEM_READ,   0, "read"     },
            { SBMEM_WRITE,  0, "write"    },
            { SBMEM_COPY,   0, "copy"     },
        };
        sb_cpu_mp rows[6];
        int nrows = 0;
        int ncpus = c->cpus > 0 ? c->cpus : sb_cpu_ncpus();
        int j, ji;
        double secs = c->time_set ? c->secs : 4.0;
        FILE *dst[2];
        if (ram && (unsigned long)(ncpus + 1) * (maxsz / MB) > ram / 2) {
            unsigned long per = ram / 2 / (unsigned long)(ncpus + 1);
            if (per < 1) per = 1;
            fprintf(stderr, "sunbench: shrinking per-process buffer to %lu MB so %d buffers fit in RAM\n", per, ncpus);
            maxsz = per * MB;
        }
        if (logf) log_stamp(stdout, logf, c->path);
        dst[0] = stdout; dst[1] = logf;
        for (j = 0; j < 2; j++) {
            FILE *o = dst[j];
            if (o == (FILE *)0) continue;
            fprintf(o, "sunbench -- memory (MP: %d process%s, %lu MB private buffer each)\n",
                    ncpus, ncpus == 1 ? "" : "es", maxsz / MB);
            fprintf(o, "  %-10s %12s %12s %10s %10s\n",
                    "test", "1-proc", "N-proc", "speedup", "scaling");
        }
        if (ncpus == 1)
            fprintf(stderr, "sunbench: NOTE -- only 1 CPU detected; the scaling column is trivially 100%%.\n");
        for (ji = 0; ji < (int)(sizeof(mpjob) / sizeof(mpjob[0])); ji++) {
            unsigned long sz = mpjob[ji].kb ? mpjob[ji].kb * 1024L : maxsz;
            sb_cpu_mp *m = &rows[nrows];
            if (sz > maxsz) continue;
            if (!g_quiet)
                fprintf(stderr, "  %s: 1 proc + %d procs...\n",
                        mpjob[ji].label, ncpus);
            if (sb_mem_run_mp(mpjob[ji].kern, sz, secs, ncpus, m) != 0)
                continue;
            strncpy(m->name, mpjob[ji].label, sizeof(m->name) - 1);
            m->name[sizeof(m->name) - 1] = '\0';
            for (j = 0; j < 2; j++) {
                FILE *o = dst[j];
                if (o == (FILE *)0) continue;
                fprintf(o, "  %-10s %7.1f MB/s %7.1f MB/s %9.2fx %9.1f%%\n",
                        m->name, m->single_ips / (1024.0 * 1024.0),
                        m->agg_ips / (1024.0 * 1024.0),
                        m->speedup, m->efficiency * 100.0);
            }
            nrows++;
        }
        if (!g_quiet)
            fprintf(stderr, "  lat loaded: chase alone + under %d streamer%s...\n", ncpus - 1, ncpus == 2 ? "" : "s");
        if (sb_mem_lat_mp(maxsz, secs, ncpus, &rows[nrows]) == 0) {
            sb_cpu_mp *m = &rows[nrows];
            for (j = 0; j < 2; j++) {
                FILE *o = dst[j];
                if (o == (FILE *)0) continue;
                fprintf(o, "  %-10s %9.1f ns %9.1f ns %9.2fx %10s\n",
                        m->name, m->single_ips, m->agg_ips, m->speedup, "-");
                fprintf(o, "  (lat loaded: ns per dependent load, alone vs while %d process%s stream%s reads)\n",
                        ncpus - 1, ncpus == 2 ? "" : "es",
                        ncpus == 2 ? "s" : "");
            }
            nrows++;
        }
        if (nrows > 0)
            sb_html_mem_mp(ncpus, rows, nrows);
        if (logf) {
            (void)fclose(logf);
            if (!g_quiet)
                fprintf(stderr, "sunbench: report also written to %s\n",
                        c->logpath);
        }
        html_flush(c);
        return;
    }

    /* Working-set sizes: 1KB doubling up to maxsz.  1KB matters: the
     * microSPARC D-cache is only 2KB, so a sweep starting at 4KB would
     * ALREADY miss at every size and show no cliff at all. */
    n = 0;
    {
        unsigned long s = 1024L;
        while (s <= maxsz && n < MEM_CELLS) {
            kb[n++] = s / 1024L;
            s <<= 1;
        }
        /* If --memsize is not a power of two, test it exactly as a final
         * row -- needed to bisect sharp walls (e.g. the sun4c PMEG limit)
         * that sit between the power-of-two steps. */
        if (n > 0 && n < MEM_CELLS &&
            kb[n - 1] * 1024L < maxsz && maxsz >= 1024L)
            kb[n++] = maxsz / 1024L;
    }
    for (i = 0; i < n; i++)
        for (k = 0; k < SBMEM_NKERNELS; k++)
            val[i][k] = -1.0;

    buf = sb_alloc_aligned(maxsz);
    if (buf == (char *)0) {
        fprintf(stderr, "sunbench: cannot allocate %lu MB buffer\n",
                maxsz / MB);
        if (logf) (void)fclose(logf);
        return;
    }

    cell_s = c->time_set ? c->secs : 0.5;
    passes = c->passes;
    if (passes < 1) passes = 1;
    if (passes > SB_MAXPASS) passes = SB_MAXPASS;
    use_tui = !c->plain && passes <= 1 && sb_tui_supported();

    if (use_tui) {
        char desc[96];
        sprintf(desc, "%lu KB .. %lu KB, %.1fs per kernel-cell", kb[0],
                kb[n - 1], cell_s);
        if (logf) log_stamp(logf, (FILE *)0, c->path);
        sb_tui_sysinfo(c->path);
        sb_memtui_start(kb, n, desc);
    } else {
        if (logf) log_stamp(stdout, logf, c->path);
        mem_header(stdout, kb[0], kb[n - 1], cell_s, passes);
        if (logf) mem_header(logf, kb[0], kb[n - 1], cell_s, passes);
    }

    for (i = 0; i < n; i++) {
        unsigned long size = kb[i] * 1024L;
        if (!use_tui && !g_quiet)
            fprintf(stderr, "  [%d/%d] %lu KB\n", i + 1, n, kb[i]);
        else
            sb_memtui_cell(i);
        for (k = 0; k < SBMEM_NKERNELS; k++) {
            double vs[SB_MAXPASS];
            sb_mem_result r;
            for (p = 0; p < passes; p++) {
                if (sb_mem_run(k, buf, size, cell_s, &r) != 0 || !r.ok)
                    vs[p] = -1.0;
                else
                    vs[p] = (k == SBMEM_LAT) ? r.ns_per_load : r.mb_per_s;
            }
            sortd(vs, passes);
            val[i][k] = vs[passes / 2];
            if (use_tui)
                sb_memtui_set(i, k, val[i][k]);
        }
        if (!use_tui) {
            mem_row(stdout, kb[i], val[i]);
            if (logf) mem_row(logf, kb[i], val[i]);
            (void)fflush(stdout);
        }
    }

    if (use_tui) {
        sb_memtui_finish();
        if (logf) {
            mem_header(logf, kb[0], kb[n - 1], cell_s, passes);
            for (i = 0; i < n; i++)
                mem_row(logf, kb[i], val[i]);
        }
    }
    if (logf) {
        (void)fclose(logf);
        if (!g_quiet)
            fprintf(stderr, "sunbench: report also written to %s\n",
                    c->logpath);
    }
    if (c->csvpath)
        write_mem_csv(c->csvpath, c->note, cell_s, passes, kb, val, n);
    sb_html_mem(kb, val, n);
    html_flush(c);
    free(buf);
}

/* ====================== network benchmark driver ======================= */

#define NET_SIZES 16

/* Append one CSV row: metadata + server + TCP figures + TCP write-size
 * sweep + per-size UDP.  (TCP write sizes are application write() sizes,
 * not wire packets.) */
static void write_net_csv(csvpath, note, server, mtu, up, down, rtt,
                          wsz, wmbps, rmbps, wn, sz, mbps, loss,
                          dmbps, dloss, n)
    char *csvpath, *note, *server; int mtu;
    double up, down, rtt;
    unsigned long *wsz; double *wmbps, *rmbps; int wn;
    unsigned long *sz; double *mbps, *loss, *dmbps, *dloss; int n;
{
    FILE *f;
    long fsz;
    int i;
    char host[64], os[80], model[80], ts[32];
    time_t now;

    f = fopen(csvpath, "a");
    if (f == (FILE *)0) {
        fprintf(stderr, "sunbench: cannot write %s\n", csvpath);
        return;
    }
    (void)fseek(f, 0L, SEEK_END);
    fsz = ftell(f);
    if (fsz <= 0) {
        fprintf(f, "timestamp,sunbench_ver,host,os,model,server,mtu,tcp_up_mbps,tcp_down_mbps,rtt_ms,note");
        for (i = 0; i < wn; i++)
            fprintf(f, ",tcp_wr_%luB_mbps", wsz[i]);
        for (i = 0; i < wn; i++)
            fprintf(f, ",tcp_rd_%luB_mbps", wsz[i]);
        for (i = 0; i < n; i++)
            fprintf(f, ",udp_up_%luB_mbps,udp_up_%luB_loss_pct",
                    sz[i], sz[i]);
        for (i = 0; i < n; i++)
            fprintf(f, ",udp_dn_%luB_mbps,udp_dn_%luB_loss_pct",
                    sz[i], sz[i]);
        fprintf(f, "\n");
    }
    (void)time(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    host[0] = os[0] = model[0] = '\0';
    sb_host(host, (int)sizeof(host));
    sb_os(os, (int)sizeof(os));
    sb_model(model, (int)sizeof(model));

    csv_str(f, ts);          putc(',', f);
    csv_str(f, SB_VERSION);  putc(',', f);
    csv_str(f, host);        putc(',', f);
    csv_str(f, os);          putc(',', f);
    csv_str(f, model);       putc(',', f);
    csv_str(f, server);      putc(',', f);
    fprintf(f, "%d,", mtu);
    if (up > 0.0)   fprintf(f, "%.3f,", up);   else fprintf(f, ",");
    if (down > 0.0) fprintf(f, "%.3f,", down); else fprintf(f, ",");
    if (rtt > 0.0)  fprintf(f, "%.3f,", rtt);  else fprintf(f, ",");
    csv_str(f, note ? note : "");
    for (i = 0; i < wn; i++) {
        if (wmbps[i] < 0.0) fprintf(f, ",");
        else fprintf(f, ",%.3f", wmbps[i]);
    }
    for (i = 0; i < wn; i++) {
        if (rmbps[i] < 0.0) fprintf(f, ",");
        else fprintf(f, ",%.3f", rmbps[i]);
    }
    for (i = 0; i < n; i++) {
        if (mbps[i] < 0.0) fprintf(f, ",,");
        else fprintf(f, ",%.3f,%.2f", mbps[i], loss[i]);
    }
    for (i = 0; i < n; i++) {
        if (dmbps[i] < 0.0) fprintf(f, ",,");
        else fprintf(f, ",%.3f,%.2f", dmbps[i], dloss[i]);
    }
    fprintf(f, "\n");
    (void)fclose(f);
    if (!g_quiet)
        fprintf(stderr, "sunbench: network CSV row appended to %s\n",
                csvpath);
}

/* Network benchmark: TCP up/down/RTT + UDP size sweep against
 * sunbench-netd on c->server. */
static void run_net_top(c)
    sb_cfg *c;
{
    FILE *logf;
    unsigned long sz[NET_SIZES];
    double mbps[NET_SIZES], loss[NET_SIZES];
    double dmbps[NET_SIZES], dloss[NET_SIZES];
    static unsigned long wsz[7];
    double wmbps[7], rmbps[7];
    double up, down, rtt, v, l;
    static unsigned long ladder[8];
    unsigned long cap;
    int mtu, n, wn, i, j, secs, port, use_tui;
    char desc[192];
    FILE *dst[2];

    if (c->server == (char *)0 || c->server[0] == '\0') {
        fprintf(stderr,
"sunbench: the network test needs a server.  On a modern machine in the\n          same network, build and start the bundled server:\n              make netd && ./sunbench-netd\n          then run:  sunbench --server HOST net\n");
        return;
    }
    port = c->netport > 0 ? c->netport : SBN_PORT;
    if (sb_net_check(c->server, port) != 0) {
#ifdef HAVE_SOCKETS
        fprintf(stderr, "sunbench: is sunbench-netd running on %s (port %d)?\n", c->server, port);
#endif
        return;
    }

    /* UDP payload ladder, capped at the local interface MTU by default
     * (Ethernet ~1472, FDDI ~4324, token ring / ATM larger); --netmax
     * overrides, e.g. to explore IP fragmentation deliberately. */
    mtu = sb_net_mtu(c->server);
    cap = c->netmax ? c->netmax : (unsigned long)((mtu ? mtu : 1500) - 28);
    if (cap < 64) cap = 64;
    if (cap > SBN_MAXDGRAM) cap = SBN_MAXDGRAM;
    ladder[0] = 64;   ladder[1] = 256;  ladder[2] = 512;  ladder[3] = 1024;
    ladder[4] = 1472; ladder[5] = 2048; ladder[6] = 4096; ladder[7] = 8192;
    n = 0;
    for (i = 0; i < 8; i++)
        if (ladder[i] < cap && n < NET_SIZES)
            sz[n++] = ladder[i];
    if (n < NET_SIZES)
        sz[n++] = cap;
    for (i = 0; i < n; i++) {
        mbps[i] = -1.0;  loss[i] = 0.0;
        dmbps[i] = -1.0; dloss[i] = 0.0;
    }

    /* TCP chunk-size ladder: application write()/read() sizes, NOT wire
     * packets (the kernel segments to the MSS); the two sweeps show the
     * per-syscall/copy cost of each direction -- receive is usually the
     * expensive one on this hardware. */
    wsz[0] = 64;   wsz[1] = 256;   wsz[2] = 1024; wsz[3] = 4096;
    wsz[4] = 8192; wsz[5] = 32768; wsz[6] = 65536;
    wn = 7;
    for (i = 0; i < wn; i++) { wmbps[i] = -1.0; rmbps[i] = -1.0; }
    up = down = rtt = 0.0;

    secs = c->time_set ? (int)(c->secs + 0.5) : 5;
    if (secs < 1) secs = 1;

    logf = c->logpath ? fopen(c->logpath, "a") : (FILE *)0;
    if (c->logpath && logf == (FILE *)0)
        fprintf(stderr, "sunbench: cannot write %s\n", c->logpath);

    if (mtu)
        sprintf(desc, "%.128s port %d   local MTU %d", c->server, port, mtu);
    else
        sprintf(desc, "%.128s port %d", c->server, port);

    use_tui = !c->plain && sb_tui_supported();
    if (use_tui) {
        if (logf) log_stamp(logf, (FILE *)0, c->path);
        sb_tui_sysinfo(c->path);
        sb_nettui_start(desc, wsz, wn, wsz, wn, sz, n, n);
    } else {
        if (logf) log_stamp(stdout, logf, c->path);
        dst[0] = stdout; dst[1] = logf;
        for (j = 0; j < 2; j++) {
            FILE *o = dst[j];
            if (o == (FILE *)0) continue;
            fprintf(o, "sunbench -- network\n");
            fprintf(o, "  server : %s\n", desc);
        }
    }

    /* 1. round trip (quick), 2. TCP both ways, 3. UDP sweep */
    if (use_tui) sb_nettui_cell(0);
    else if (!g_quiet) fprintf(stderr, "  TCP round trip...\n");
    if (sb_net_rtt(c->server, port, secs < 3 ? secs : 3, &rtt) != 0)
        rtt = 0.0;
    if (use_tui) sb_nettui_set(0, rtt > 0.0 ? rtt : -1.0, -1.0);

    if (use_tui) sb_nettui_cell(1);
    else if (!g_quiet) fprintf(stderr, "  TCP upload (%ds)...\n", secs);
    if (sb_net_tcp(c->server, port, 0, secs, SBN_BUF, &up) != 0) up = 0.0;
    if (use_tui) sb_nettui_set(1, up > 0.0 ? up : -1.0, -1.0);

    if (use_tui) sb_nettui_cell(2);
    else if (!g_quiet) fprintf(stderr, "  TCP download (%ds)...\n", secs);
    if (sb_net_tcp(c->server, port, 1, secs, SBN_BUF, &down) != 0)
        down = 0.0;
    if (use_tui) sb_nettui_set(2, down > 0.0 ? down : -1.0, -1.0);

    if (!use_tui) {
        for (j = 0; j < 2; j++) {
            FILE *o = dst[j];
            if (o == (FILE *)0) continue;
            if (rtt > 0.0)
                fprintf(o, "  %-26s %9.2f ms\n", "TCP round trip", rtt);
            if (up > 0.0)
                fprintf(o, "  %-26s %9.2f Mbit/s\n",
                        "TCP upload (64 KB writes)", up);
            if (down > 0.0)
                fprintf(o, "  %-26s %9.2f Mbit/s\n",
                        "TCP download (64 KB reads)", down);
            fprintf(o, "  TCP write-size sweep (upload, %ds per size; write() sizes, not wire packets):\n", secs);
            fprintf(o, "  %8s %10s\n", "bytes", "Mbit/s");
        }
        (void)fflush(stdout);
    }

    /* TCP write-size sweep: upload with small application writes -- the
     * per-syscall cost that dominates real programs on a slow CPU. */
    for (i = 0; i < wn; i++) {
        if (use_tui) sb_nettui_cell(3 + i);
        else if (!g_quiet)
            fprintf(stderr, "  [%d/%d] TCP write %lu bytes\n",
                    i + 1, wn, wsz[i]);
        if (sb_net_tcp(c->server, port, 0, secs, (int)wsz[i], &v) == 0)
            wmbps[i] = v;
        if (use_tui)
            sb_nettui_set(3 + i, wmbps[i], -1.0);
        else {
            for (j = 0; j < 2; j++) {
                FILE *o = dst[j];
                if (o == (FILE *)0) continue;
                if (wmbps[i] < 0.0)
                    fprintf(o, "  %8lu %10s\n", wsz[i], "-");
                else
                    fprintf(o, "  %8lu %10.2f\n", wsz[i], wmbps[i]);
            }
            (void)fflush(stdout);
        }
    }

    /* TCP read-size sweep: download with small application reads -- the
     * receive side, usually the expensive direction on old Sun NICs. */
    if (!use_tui) {
        for (j = 0; j < 2; j++) {
            FILE *o = dst[j];
            if (o == (FILE *)0) continue;
            fprintf(o, "  TCP read-size sweep (download, %ds per size; read() sizes, not wire packets):\n", secs);
            fprintf(o, "  %8s %10s\n", "bytes", "Mbit/s");
        }
        (void)fflush(stdout);
    }
    for (i = 0; i < wn; i++) {
        if (use_tui) sb_nettui_cell(3 + wn + i);
        else if (!g_quiet)
            fprintf(stderr, "  [%d/%d] TCP read %lu bytes\n",
                    i + 1, wn, wsz[i]);
        if (sb_net_tcp(c->server, port, 1, secs, (int)wsz[i], &v) == 0)
            rmbps[i] = v;
        if (use_tui)
            sb_nettui_set(3 + wn + i, rmbps[i], -1.0);
        else {
            for (j = 0; j < 2; j++) {
                FILE *o = dst[j];
                if (o == (FILE *)0) continue;
                if (rmbps[i] < 0.0)
                    fprintf(o, "  %8lu %10s\n", wsz[i], "-");
                else
                    fprintf(o, "  %8lu %10.2f\n", wsz[i], rmbps[i]);
            }
            (void)fflush(stdout);
        }
    }

    if (!use_tui) {
        for (j = 0; j < 2; j++) {
            FILE *o = dst[j];
            if (o == (FILE *)0) continue;
            fprintf(o, "  UDP upload sweep (%ds per size):\n", secs);
            fprintf(o, "  %8s %10s %8s\n", "bytes", "Mbit/s", "loss");
        }
        (void)fflush(stdout);
    }

    for (i = 0; i < n; i++) {
        if (use_tui) sb_nettui_cell(3 + 2 * wn + i);
        else if (!g_quiet)
            fprintf(stderr, "  [%d/%d] UDP up %lu bytes\n", i + 1, n, sz[i]);
        if (sb_net_udp(c->server, port, secs, (int)sz[i], &v, &l) == 0) {
            mbps[i] = v;
            loss[i] = l;
        }
        if (use_tui)
            sb_nettui_set(3 + 2 * wn + i, mbps[i],
                          mbps[i] >= 0.0 ? loss[i] : -1.0);
        else {
            for (j = 0; j < 2; j++) {
                FILE *o = dst[j];
                if (o == (FILE *)0) continue;
                if (mbps[i] < 0.0)
                    fprintf(o, "  %8lu %10s %8s\n", sz[i], "-", "-");
                else
                    fprintf(o, "  %8lu %10.2f %7.1f%%\n",
                            sz[i], mbps[i], loss[i]);
            }
            (void)fflush(stdout);
        }
    }

    /* UDP download sweep: the server sends, paced to 1.5x our upstream
     * result for the same size, so loss reflects OUR receive ceiling and
     * not the raw speed mismatch with a fast server.  1.5x assumes the
     * receive path is as fast as the send path; where it is not (8-bit
     * shared-memory cards, Lance rx-buffer starvation) the ring overflows
     * continuously and near-nothing survives -- so on heavy loss the pace
     * is halved and the cell rerun, converging on the highest rate the
     * receiver actually sustains. */
    if (!use_tui) {
        for (j = 0; j < 2; j++) {
            FILE *o = dst[j];
            if (o == (FILE *)0) continue;
            fprintf(o, "  UDP download sweep (%ds per size; paced to 1.5x upload, halved while loss > 40%%):\n", secs);
            fprintf(o, "  %8s %10s %8s\n", "bytes", "Mbit/s", "loss");
        }
        (void)fflush(stdout);
    }
    for (i = 0; i < n; i++) {
        int kbps;
        if (mbps[i] > 0.0)
            kbps = (int)(mbps[i] * 1500.0);        /* 1.5x, in kbit/s */
        else
            kbps = 10000;                          /* no baseline: 10 Mb/s */
        if (kbps < 64) kbps = 64;
        if (use_tui) sb_nettui_cell(3 + 2 * wn + n + i);
        else if (!g_quiet)
            fprintf(stderr, "  [%d/%d] UDP down %lu bytes\n",
                    i + 1, n, sz[i]);
        {
            int dtry;
            for (dtry = 0; dtry < 5; dtry++) {
                if (sb_net_udp_down(c->server, port, secs, (int)sz[i],
                                    kbps, &v, &l) != 0)
                    break;
                dmbps[i] = v;
                dloss[i] = l;
                if (l <= 40.0 || kbps <= 64)
                    break;
                kbps /= 2;
                if (kbps < 64) kbps = 64;
                if (!use_tui && !g_quiet)
                    fprintf(stderr,
                            "  [%d/%d] UDP down %lu bytes: %.0f%% loss, repacing at %d kbit/s\n",
                            i + 1, n, sz[i], l, kbps);
            }
        }
        if (use_tui)
            sb_nettui_set(3 + 2 * wn + n + i, dmbps[i],
                          dmbps[i] >= 0.0 ? dloss[i] : -1.0);
        else {
            for (j = 0; j < 2; j++) {
                FILE *o = dst[j];
                if (o == (FILE *)0) continue;
                if (dmbps[i] < 0.0)
                    fprintf(o, "  %8lu %10s %8s\n", sz[i], "-", "-");
                else
                    fprintf(o, "  %8lu %10.2f %7.1f%%\n",
                            sz[i], dmbps[i], dloss[i]);
            }
            (void)fflush(stdout);
        }
    }

    if (use_tui) {
        sb_nettui_finish();
        if (logf) {
            fprintf(logf, "sunbench -- network\n");
            fprintf(logf, "  server : %s\n", desc);
            if (rtt > 0.0)
                fprintf(logf, "  %-26s %9.2f ms\n", "TCP round trip", rtt);
            if (up > 0.0)
                fprintf(logf, "  %-26s %9.2f Mbit/s\n",
                        "TCP upload (64 KB writes)", up);
            if (down > 0.0)
                fprintf(logf, "  %-26s %9.2f Mbit/s\n",
                        "TCP download (64 KB reads)", down);
            fprintf(logf, "  TCP write-size sweep (upload, %ds per size; write() sizes, not wire packets):\n", secs);
            fprintf(logf, "  %8s %10s\n", "bytes", "Mbit/s");
            for (i = 0; i < wn; i++) {
                if (wmbps[i] < 0.0)
                    fprintf(logf, "  %8lu %10s\n", wsz[i], "-");
                else
                    fprintf(logf, "  %8lu %10.2f\n", wsz[i], wmbps[i]);
            }
            fprintf(logf, "  TCP read-size sweep (download, %ds per size; read() sizes, not wire packets):\n", secs);
            fprintf(logf, "  %8s %10s\n", "bytes", "Mbit/s");
            for (i = 0; i < wn; i++) {
                if (rmbps[i] < 0.0)
                    fprintf(logf, "  %8lu %10s\n", wsz[i], "-");
                else
                    fprintf(logf, "  %8lu %10.2f\n", wsz[i], rmbps[i]);
            }
            fprintf(logf, "  UDP upload sweep (%ds per size):\n", secs);
            fprintf(logf, "  %8s %10s %8s\n", "bytes", "Mbit/s", "loss");
            for (i = 0; i < n; i++) {
                if (mbps[i] < 0.0)
                    fprintf(logf, "  %8lu %10s %8s\n", sz[i], "-", "-");
                else
                    fprintf(logf, "  %8lu %10.2f %7.1f%%\n",
                            sz[i], mbps[i], loss[i]);
            }
            fprintf(logf, "  UDP download sweep (%ds per size; paced to 1.5x upload, halved while loss > 40%%):\n", secs);
            fprintf(logf, "  %8s %10s %8s\n", "bytes", "Mbit/s", "loss");
            for (i = 0; i < n; i++) {
                if (dmbps[i] < 0.0)
                    fprintf(logf, "  %8lu %10s %8s\n", sz[i], "-", "-");
                else
                    fprintf(logf, "  %8lu %10.2f %7.1f%%\n",
                            sz[i], dmbps[i], dloss[i]);
            }
        }
    }
    if (logf) {
        (void)fclose(logf);
        if (!g_quiet)
            fprintf(stderr, "sunbench: report also written to %s\n",
                    c->logpath);
    }
    if (c->csvpath)
        write_net_csv(c->csvpath, c->note, c->server, mtu, up, down, rtt,
                      wsz, wmbps, rmbps, wn, sz, mbps, loss,
                      dmbps, dloss, n);
    sb_html_net(c->server, mtu, up, down, rtt, wsz, wmbps, rmbps, wn,
                sz, mbps, loss, dmbps, dloss, n);
    html_flush(c);
}

/* Derive a per-module CSV name: "fleet.csv" + "cpu" -> "fleet-cpu.csv".
 * The disk sweep, CPU and memory schemas differ, so a full-suite run cannot
 * append them all to one file. */
static void csv_derive(base, tag, out)
    char *base, *tag, *out;
{
    char *dot = strrchr(base, '.');
    char *sl  = strrchr(base, '/');
    if (dot != (char *)0 && (sl == (char *)0 || dot > sl))
        sprintf(out, "%.*s-%s%s", (int)(dot - base), base, tag, dot);
    else
        sprintf(out, "%s-%s.csv", base, tag);
}

/* Full unattended suite: disk standard run (with histogram) + block-size
 * sweep + CPU + memory, batch (plain) output so it streams over any link and
 * appends cleanly to --log.  Each sub-run gets its own timestamped log
 * section; with --csv, each module writes its own derived file (schemas
 * differ): fleet.csv -> fleet-disk.csv / fleet-cpu.csv / fleet-mem.csv. */
static void run_full(c)
    sb_cfg *c;
{
    sb_cfg t;
    char csvd[560], csvc[560], csvm[560];
    int mp;

    t = *c;
    t.plain = 1;
    t.hist = 1;
    /* The suite proper is always single-process, so machines compare on
     * identical terms; MP scaling runs as extra stages afterwards --
     * automatically on a multiprocessor, or when --mp/--cpus asks. */
    mp = c->mp || c->cpus > 0 || sb_cpu_ncpus() > 1;
    t.mp = 0;
    t.cpus = 0;
    if (c->csvpath) {
        csv_derive(c->csvpath, "disk", csvd);
        csv_derive(c->csvpath, "cpu",  csvc);
        csv_derive(c->csvpath, "mem",  csvm);
    }
    t.csvpath = c->csvpath ? csvd : (char *)0;
    run_action(&t, 0, 0);   /* standard four-test run */
    run_action(&t, 1, 0);   /* block-size sweep */
    t.csvpath = c->csvpath ? csvc : (char *)0;
    run_cpu_top(&t);
    t.csvpath = c->csvpath ? csvm : (char *)0;
    run_mem_top(&t);
    if (mp) {
        sb_cfg m;
        m = t;
        m.mp = c->mp;
        m.cpus = c->cpus;
        if (!m.mp && m.cpus == 0) m.mp = 1;     /* auto-detected */
        m.csvpath = (char *)0;                  /* MP tables have no CSV */
        run_cpu_top(&m);
        run_mem_top(&m);
    } else if (!g_quiet)
        fprintf(stderr, "sunbench: skipping MP scaling (1 CPU)\n");
    if (c->server && c->server[0]) {
        char csvn[560];
        if (c->csvpath) {
            csv_derive(c->csvpath, "net", csvn);
            t.csvpath = csvn;
        } else
            t.csvpath = (char *)0;
        run_net_top(&t);
    } else if (!g_quiet)
        fprintf(stderr, "sunbench: skipping network (no --server given)\n");
}

/* Menu row indices (MI_SEPn are non-selectable separators between the
 * disk, CPU and memory sections). */
#define MI_STANDARD 0
#define MI_SWEEP    1
#define MI_FULL     2
#define MI_SEP1     3
#define MI_CPU      4
#define MI_CPU_MP   5
#define MI_SEP2     6
#define MI_MEM      7
#define MI_MEM_MP   8
#define MI_SEP3     9
#define MI_NET      10
#define MI_SEP4     11
#define MI_OLOG     12
#define MI_OCSV     13
#define MI_OHTML    14
#define MI_HIST     15
#define MI_QUIT     16
#define MI_N        17

/* Move the selection one step, skipping separator (empty) rows. */
static int menu_step(items, sel, dir)
    char **items; int sel, dir;
{
    int i = sel;
    do {
        i = (i + dir + MI_N) % MI_N;
    } while (items[i][0] == '\0' && i != sel);
    return i;
}

/* Interactive arrow-key menu. */
static void menu_loop(c)
    sb_cfg *c;
{
    char *items[MI_N];
    char histitem[64], logitem[96], csvitem[96], htmlitem[96], netitem[96];
    static char nethost[128];
    int sel = MI_STANDARD, running = 1, k;
    char hint[96];
    char *save_log, *save_csv, *save_html;

    /* Remember any CLI-given output names so a toggle restores them. */
    save_log  = c->logpath;
    save_csv  = c->csvpath;
    save_html = c->htmlpath;

    items[MI_STANDARD] = "Standard disk benchmark";
    items[MI_SWEEP]    = "Block-size sweep";
    items[MI_FULL]     = "Full suite (disk + sweep + cpu + mem)";
    items[MI_SEP1]     = "";              /* non-selectable separator */
    items[MI_CPU]      = "CPU benchmark";
    items[MI_CPU_MP]   = "CPU multiprocessor scaling";
    items[MI_SEP2]     = "";              /* non-selectable separator */
    items[MI_MEM]      = "Memory benchmark (working-set sweep)";
    items[MI_MEM_MP]   = "Memory multiprocessor scaling";
    items[MI_SEP3]     = "";              /* non-selectable separator */
    items[MI_NET]      = netitem;         /* shows the chosen server */
    items[MI_SEP4]     = "";              /* non-selectable separator */
    items[MI_OLOG]     = logitem;         /* rebuilt each frame (show state) */
    items[MI_OCSV]     = csvitem;
    items[MI_OHTML]    = htmlitem;
    items[MI_HIST]     = histitem;
    items[MI_QUIT]     = "Quit";

    if (sb_keys_raw_on() != 0) {          /* no raw keys: just run once */
        run_action(c, 0, 1);
        return;
    }
    sb_tui_sysinfo(c->path);              /* header banner on every menu frame */
    fputs("\033[2J", stdout);
    while (running) {
        sprintf(histitem, "Toggle disk benchmark latency histogram (current: %s)",
                c->hist ? "ON" : "OFF");
        if (c->logpath)
            sprintf(logitem, "Log to text file        (current: %s)",
                    c->logpath);
        else
            strcpy(logitem, "Log to text file        (current: OFF)");
        if (c->csvpath)
            sprintf(csvitem, "CSV for spreadsheets    (current: %s)",
                    c->csvpath);
        else
            strcpy(csvitem, "CSV for spreadsheets    (current: OFF)");
        if (c->htmlpath)
            sprintf(htmlitem, "HTML report             (current: %s)",
                    c->htmlpath);
        else
            strcpy(htmlitem, "HTML report             (current: OFF)");
        if (c->server && c->server[0])
            sprintf(netitem, "Network benchmark       (server: %.60s)",
                    c->server);
        else
            strcpy(netitem, "Network benchmark       (asks for a server)");
        sprintf(hint, "path %s   size %s   block %luKB",
                c->path, c->size_set ? "fixed" : "auto", c->block_kb);
        sb_tui_menu_draw(items, MI_N, sel, hint);
        k = sb_key_get();
        if (k == SBK_UP)         sel = menu_step(items, sel, -1);
        else if (k == SBK_DOWN)  sel = menu_step(items, sel, 1);
        else if (k == SBK_QUIT)  running = 0;
        else if (k == SBK_ENTER) {
            if (sel == MI_QUIT) running = 0;
            else if (sel == MI_HIST) c->hist = !c->hist;
            else if (sel == MI_OLOG)
                c->logpath = c->logpath ? (char *)0
                           : (save_log ? save_log : "sunbench.log");
            else if (sel == MI_OCSV)
                c->csvpath = c->csvpath ? (char *)0
                           : (save_csv ? save_csv : "sunbench.csv");
            else if (sel == MI_OHTML) {
                if (c->htmlpath) {
                    c->htmlpath = (char *)0;
                    sb_html_disable();
                } else {
                    c->htmlpath = save_html ? save_html : "sunbench.html";
                    sb_html_enable(c->htmlpath, c->path);
                }
            }
            else {
                sb_keys_raw_off();
                /* modules run on the NORMAL screen so their final plain
                 * tables land in the terminal's scrollback; the menu
                 * re-enters the alternate screen on its next draw */
                sb_tui_alt_leave();
                if (sel == MI_FULL) run_full(c);
                else if (sel == MI_CPU) run_cpu_top(c);
                else if (sel == MI_CPU_MP) {
                    sb_cfg t;
                    t = *c;
                    t.mp = 1;               /* auto-detect the CPU count */
                    run_cpu_top(&t);
                } else if (sel == MI_MEM) run_mem_top(c);
                else if (sel == MI_MEM_MP) {
                    sb_cfg t;
                    t = *c;
                    t.mp = 1;
                    run_mem_top(&t);
                } else if (sel == MI_NET) {
                    /* cooked-mode line input: no raw-mode editor needed */
                    if (c->server == (char *)0 || c->server[0] == '\0') {
                        char *nl;
                        printf("  server running sunbench-netd (hostname or IP): ");
                        (void)fflush(stdout);
                        if (fgets(nethost, (int)sizeof(nethost), stdin)
                                != (char *)0) {
                            nl = strchr(nethost, '\n');
                            if (nl) *nl = '\0';
                            if (nethost[0])
                                c->server = nethost;
                        }
                    }
                    if (c->server && c->server[0])
                        run_net_top(c);
                } else
                    run_action(c, sel == MI_SWEEP, 1);
                sb_tui_prompt("-- press a key to return to the menu --");
                sb_keys_raw_on();
                (void)sb_key_get();
                fputs("\n", stdout);
            }
        }
    }
    sb_keys_raw_off();
    sb_tui_alt_leave();          /* restores the pre-menu screen contents */
    (void)fflush(stdout);
}

int main(argc, argv)
    int argc;
    char **argv;
{
    sb_cfg c;
    int i, sweep = 0, diskfull = 0, menu = 0, full = 0;
    int have_disk = 0, have_cpu = 0, have_mem = 0, have_net = 0;
    int menu_capable;

    c.path = "./sunbench.dat";
    c.logpath = (char *)0;
    c.csvpath = (char *)0;
    c.note = (char *)0;
    c.size_mb = 0; c.size_set = 0;
    c.block_kb = 64;
    c.secs = 8.0; c.time_set = 0;
    c.hist = 0; c.keep = 0; c.plain = 0;
    c.passes = 1;
    c.mhz = 0;
    c.mp = 0;
    c.cpus = 0;
    c.mem_mb = 0;
    c.htmlpath = (char *)0;
    c.quiet = 0;
    c.server = (char *)0;
    c.netport = 0;
    c.netmax = 0;

    (void)setvbuf(stdout, (char *)0, _IOLBF, 0);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--path") == 0 && i + 1 < argc)
            c.path = argv[++i];
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            c.size_mb = (unsigned long)atol(argv[++i]); c.size_set = 1;
        } else if (strcmp(argv[i], "--block") == 0 && i + 1 < argc)
            c.block_kb = (unsigned long)atol(argv[++i]);
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            c.secs = atof(argv[++i]); c.time_set = 1;
        } else if (strcmp(argv[i], "--sweep") == 0) sweep = 1;
        else if (strcmp(argv[i], "--diskfull") == 0) diskfull = 1;
        else if (strcmp(argv[i], "--keep") == 0)    c.keep = 1;
        else if (strcmp(argv[i], "--hist") == 0)    c.hist = 1;
        else if (strcmp(argv[i], "--plain") == 0)   c.plain = 1;
        else if (strcmp(argv[i], "--menu") == 0)    menu = 1;
        else if (strcmp(argv[i], "--full") == 0)    full = 1;
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            c.logpath = argv[++i];
        else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)
            c.csvpath = argv[++i];
        else if (strcmp(argv[i], "--note") == 0 && i + 1 < argc)
            c.note = argv[++i];
        else if (strcmp(argv[i], "--mhz") == 0 && i + 1 < argc)
            c.mhz = (unsigned long)atol(argv[++i]);
        else if (strcmp(argv[i], "--mp") == 0) c.mp = 1;
        else if (strcmp(argv[i], "--cpus") == 0 && i + 1 < argc) {
            c.cpus = atoi(argv[++i]);
            if (c.cpus < 0) c.cpus = 0;
            c.mp = 1;
        }
        else if (strcmp(argv[i], "--memsize") == 0 && i + 1 < argc)
            c.mem_mb = (unsigned long)atol(argv[++i]);
        else if (strcmp(argv[i], "--html") == 0 && i + 1 < argc)
            c.htmlpath = argv[++i];
        else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            c.server = argv[++i];
        else if ((strcmp(argv[i], "--port") == 0 ||
                  strcmp(argv[i], "--netport") == 0) && i + 1 < argc)
            c.netport = atoi(argv[++i]);
        else if (strcmp(argv[i], "--netmax") == 0 && i + 1 < argc)
            c.netmax = (unsigned long)atol(argv[++i]);
        else if (strcmp(argv[i], "--quiet") == 0 ||
                 strcmp(argv[i], "-q") == 0)
            c.quiet = 1;
        else if (strcmp(argv[i], "--passes") == 0 && i + 1 < argc) {
            c.passes = atoi(argv[++i]);
            if (c.passes < 1) c.passes = 1;
        }
        else if (strcmp(argv[i], "--help") == 0 ||
                 strcmp(argv[i], "-h") == 0) { help(); return 0; }
        else if (strcmp(argv[i], "--version") == 0 ||
                 strcmp(argv[i], "-V") == 0) {
            printf("sunbench %s\n", SB_VERSION); return 0;
        }
        else if (strcmp(argv[i], "disk") == 0)      have_disk = 1;
        else if (strcmp(argv[i], "cpu") == 0)       have_cpu = 1;
        else if (strcmp(argv[i], "mem") == 0)       have_mem = 1;
        else if (strcmp(argv[i], "net") == 0)       have_net = 1;
        else if (strcmp(argv[i], "menu") == 0)      menu = 1;
        else if (strcmp(argv[i], "full") == 0)      full = 1;
        else { usage(); return 2; }
    }

    /* Guardrail: a disk benchmark on tmpfs (Solaris /tmp) measures RAM. */
    if (sb_fs_is_memory(c.path)) {
        fprintf(stderr,
"sunbench: WARNING -- %s is on a memory-backed filesystem (tmpfs/swap).\n          You would be benchmarking RAM, not a disk.  On Solaris /tmp is\n          tmpfs; point --path at a real disk (run 'df -k').\n\n", c.path);
    }

    g_quiet = c.quiet;
    g_note = c.note;
    sb_prog_quiet(c.quiet);
    if (c.quiet) c.plain = 1;          /* quiet implies no full-screen TUI */
    if (c.htmlpath)
        sb_html_enable(c.htmlpath, c.path);
    sb_html_note(c.note);

    c.method = sb_choose_method(c.path);
    menu_capable = sb_tui_supported() && sb_keys_available();

    if (menu) { menu_loop(&c); return 0; }
    if (full) { run_full(&c); return 0; }
    if (have_cpu) { run_cpu_top(&c); return 0; }
    if (have_mem) { run_mem_top(&c); return 0; }
    if (have_net) { run_net_top(&c); return 0; }
    if (have_disk) {
        if (diskfull) {
            /* both disk stages, as `full` runs them, plain so both tables
             * scroll past (the TUI would clear between them) */
            c.plain = 1;
            run_action(&c, 0, 0);
            run_action(&c, 1, 0);
            return 0;
        }
        run_action(&c, sweep, sb_tui_supported());
        return 0;
    }
    if (menu_capable) { menu_loop(&c); return 0; }

    usage();
    return 2;
}

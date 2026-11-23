/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * html.h -- self-contained HTML report writer.
 *
 * Collects results from whichever benchmarks run this session and, on each
 * sb_html_write(), (re)writes ONE self-contained HTML file: inline CSS, no
 * JavaScript, no external references of any kind -- it must survive being
 * written by a SunOS 4 box and opened decades later on anything.  Pure-CSS
 * proportional bars visualize the sweep tables; an @media print stylesheet
 * keeps it printable.
 *
 * Usage: sb_html_enable(path, scratch) once; feed sections as their runs
 * complete; call sb_html_write() after each run (it rewrites the whole file,
 * so a multi-run session -- e.g. the menu, or `full` -- grows the report
 * incrementally and never loses finished results to an abort).
 */
#ifndef SUNBENCH_HTML_H
#define SUNBENCH_HTML_H
#include "port.h"
#include "bench_disk.h"     /* sb_result */
#include "cpu.h"            /* sb_cpu_mp, SBK_NKERNELS */
#include "histogram.h"      /* sb_hist for the latency histogram */
#include "bench_mem.h"      /* SBMEM_NKERNELS */
#include <stdio.h>

/* Enable reporting to `path`; `scratch` is the benchmark file path, used to
 * gather the machine synopsis (filesystem, device, drive). */
void sb_html_enable P_((char *path, char *scratch));
void sb_html_disable P_((void));
int  sb_html_on P_((void));

/* Free-text --note, shown in the Machine card after the probed fields. */
void sb_html_note P_((char *note));

/* --- disk standard run (single-pass values) --- */
void sb_html_disk_std P_((char *method, char *fdesc,
                          sb_result *w, sb_result *r,
                          sb_result *rr, sb_result *rw));
/* Latency percentiles for the random tests (ms). */
void sb_html_disk_lat P_((int write_test, double mn, double p50, double p90,
                          double p99, double mx));

/* Full latency distribution for a random test (the ASCII histogram's
 * HTML twin); feed after sb_html_disk_lat when --hist is on. */
void sb_html_disk_hist P_((int write_test, sb_hist *hist));

/* --- disk standard run, --passes>1 variant (min/median/max MB/s) --- */
void sb_html_disk_mmm P_((char *method, char *fdesc, int passes,
                          char **labels, double *mn, double *md, double *mx,
                          int n));

/* --- disk block-size sweep (median MB/s per cell + rndR IOPS) --- */
void sb_html_disk_sweep P_((char *method, char *fdesc, int passes,
                            unsigned long *kb, double (*med)[4],
                            double *iops, int n));

/* --- cpu kernels (iters/sec; Dhrystone/Whetstone converted for display) --- */
void sb_html_cpu P_((char *build, unsigned long mhz, int passes,
                     double *rates));
void sb_html_cpu_mp P_((int ncpus, sb_cpu_mp *rows, int n));

/* --- memory sweep (MB/s x4 + latency ns per size) and MP --- */
void sb_html_mem P_((unsigned long *kb, double (*val)[SBMEM_NKERNELS],
                     int n));
void sb_html_mem_mp P_((int ncpus, sb_cpu_mp *m, int n));

/* --- network: TCP up/down (64K chunks) Mbit/s, RTT ms, TCP write-size
 *     sweep (per-syscall cost), UDP datagram sweep (Mbit/s + loss%) --- */
void sb_html_net P_((char *server, int mtu, double up, double down,
                     double rtt_ms,
                     unsigned long *wsz, double *wmbps, double *rmbps,
                     int wn,
                     unsigned long *usz, double *umbps, double *uloss,
                     double *dmbps, double *dloss, int un));

/* Label for the round-trip row (default "TCP round trip"; SBDOS: UDP). */
void sb_html_net_rtt_label P_((char *label));

/* Rewrite the report file from everything collected so far.
 * Returns 0 on success, -1 if disabled or the file cannot be written. */
int sb_html_write P_((void));

#endif

/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * tui.h -- self-contained ANSI/vt100 live view (no ncurses).
 *
 * Output-only: we paint with escape sequences and never read the keyboard,
 * so there's no termios/raw-mode dependency to get wrong across SunOS 4 ..
 * Solaris 10.  Ctrl-C aborts as usual.  Every target terminal (Sun console,
 * xterm, dtterm, serial VT100) understands the handful of sequences used.
 */
#ifndef SUNBENCH_TUI_H
#define SUNBENCH_TUI_H
#include "port.h"
#include "bench_disk.h"

/* Is a live TUI appropriate? isatty(stdout) and a usable $TERM. */
int  sb_tui_supported P_((void));

/* Leave the xterm alternate screen if it is active (restores the user's
 * scrollback).  Safe to call at any time, from signal handlers included. */
void sb_tui_alt_leave P_((void));

/* Gather the machine synopsis once (uname/model/RAM/disk) so every dashboard
 * header can paint it; probes are slow, so this caches on first call. */
void sb_tui_sysinfo P_((char *path));

/* Begin a screen: clears it and stores the header lines. */
void sb_tui_start P_((char *method, char *file_desc));

/* Pre-register a test row (shown "waiting" until it runs). */
void sb_tui_add P_((char *label));

/* Fill *rep with the TUI reporter (drives the live rows). */
void sb_tui_reporter P_((sb_reporter *rep));

/* Final repaint and drop the cursor below the table. */
void sb_tui_finish P_((void));

/* Draw a selection menu (selected row shown in reverse video). */
void sb_tui_menu_draw P_((char **items, int n, int sel, char *hint));

/* Prompt line, used to pause after a run before returning to the menu. */
void sb_tui_prompt P_((char *msg));

/* --- block-size sweep dashboard (compact, in-place live table) --- */
void sb_sweep_start P_((char *method, char *fdesc, unsigned long *kbs, int n));
void sb_sweep_preparing P_((int on));   /* show the "preparing file" phase */
void sb_sweep_cell P_((int i));         /* mark row i as the active one */
void sb_sweep_reporter P_((sb_reporter *rep));
void sb_sweep_finish P_((void));
void sb_sweep_write_log P_((FILE *out)); /* dump the finished table to a log */
/* Copy finished rows out (kb + seqW/seqR/randR/randW + randR-IOPS);
 * returns row count. */
int  sb_sweep_export P_((unsigned long *kbout, double (*vout)[4],
                         double *iopsout, int max));

/* --- CPU live dashboard (rows in each kernel's native unit) --- */
void sb_cpu_tui_start    P_((char *build, unsigned long mhz));
void sb_cpu_tui_add      P_((char *label, int kern));
void sb_cpu_tui_reporter P_((sb_reporter *rep));
void sb_cpu_tui_set      P_((int kern, double ips));  /* final CPU-time rate */
void sb_cpu_tui_finish   P_((void));

/* --- memory working-set sweep dashboard (rows = sizes, cols = kernels) --- */
void sb_memtui_start  P_((unsigned long *kbs, int n, char *desc));
void sb_memtui_cell   P_((int i));                  /* mark row i active */
void sb_memtui_set    P_((int i, int col, double val));
void sb_memtui_finish P_((void));

/* --- network dashboard: rows 0..2 = RTT/up/down, then the TCP write-size
 *     sweep rows, then the TCP read-size rows, then the UDP rows --- */
void sb_nettui_start  P_((char *desc, unsigned long *wsz, int wn,
                          unsigned long *rsz, int rn,
                          unsigned long *usz, int un, int dn));
void sb_nettui_cell   P_((int i));
void sb_nettui_set    P_((int i, double val, double aux)); /* aux<0: none */
void sb_nettui_finish P_((void));

#endif



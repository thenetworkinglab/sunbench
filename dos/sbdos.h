/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * sbdos.h -- what sbdos.c shares with the other DOS-only modules
 * (sbnet.c, pktnet.c).
 */
#ifndef SBDOS_H
#define SBDOS_H

/* The 8254 clock (sbdos.c). */
double now_s(void);
void   now_raw(unsigned long *ticks, unsigned *elapsed);
double raw_s(unsigned long ticks, unsigned elapsed);

/* Report output: screen and --log; the log preamble; the HTML rewrite. */
void out(char *s);
void stamp(int disk);
void html_flush(void);

extern double o_secs;           /* --time */
extern int    o_secs_set;

/* sbnet.c: the network test. */
int   sbnet_opt(char *name, char *val);  /* 1 if it took the option */
int   sbnet_ready(void);                 /* --server given */
int   sbnet_run(void);
char *sbnet_syn(void);                   /* "net :" synopsis, or NULL */

#endif

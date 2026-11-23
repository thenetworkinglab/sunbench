/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * dosport.h -- stands in for src/port.h in the DOS build.
 *
 * Force-included (wcc -fi=) ahead of every source.  Defining port.h's
 * include guard keeps the shared sources (histogram.c, cpu.c, html.c ...)
 * from pulling in port.h, and with it the HOST machine's generated
 * config.h, which describes the Mac, not DOS.
 */
#ifndef SUNBENCH_PORT_H
#define SUNBENCH_PORT_H

#define P_(args) args

/* No fork(): compiles out the MP sections of cpu.c and bench_mem.c. */
#define SB_NOFORK 1

/* 64 KB far pointers: bench_mem.c walks bigger sets segment by segment
 * through sb_seg_ptr() (sbdos.c). */
#define SB_SEGMENTED 1

/* sb_cpu_build_id() reports these, as the Makefile's -D's do on Unix. */
#define SB_CCNAME "Open Watcom wcc, large model"
#define SB_ARCH "-0 (8086 code)"

#endif

/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * whet.h -- Whetstone double-precision floating-point benchmark.
 * See whet.c for the full Painter Engineering / netlib attribution.
 *
 * whet_loop(passes) runs the standard eight modules `passes` times with an
 * internal LOOP of 1; each such pass is, by the benchmark's definition,
 * 100000 Whetstone instructions.  The harness times it; MWIPS is then
 * (passes-per-second * WHET_INSTR_PER_LOOP) / 1e6.
 */
#ifndef SUNBENCH_WHET_H
#define SUNBENCH_WHET_H
#include "port.h"

#define WHET_INSTR_PER_LOOP 100000.0

double whet_loop P_((unsigned long passes));

#endif

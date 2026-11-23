/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * svgchart.h -- self-contained SVG line charts for the HTML report.
 *
 * Emits one <svg> element per call, with every style inline (no CSS or
 * JavaScript dependencies), so the same chart works in a freshly generated
 * report AND when injected into an old one by sunbench-htmlgraph.  The x
 * axis is categorical (one slot per data point): our sweeps are log ladders,
 * so index spacing IS log spacing, and no math library tricks are needed.
 */
#ifndef SUNBENCH_SVGCHART_H
#define SUNBENCH_SVGCHART_H
#include "port.h"
#include <stdio.h>

#define SB_SVG_MAXSERIES 6
#define SB_SVG_MAXPTS    24

/*
 * One multi-series line chart.
 *   title   chart heading (plain text; caller escapes if needed)
 *   yunit   y-axis unit label, e.g. "MB/s", "ns", "Mbit/s" ("" = none)
 *   xlab    x tick label per point, e.g. "4K", "1M", "64B"
 *   nx      number of points (clamped to SB_SVG_MAXPTS)
 *   snames  series names for the legend
 *   vals    series-major values: vals[s * nx + i]; value < 0 = missing
 *   ns      number of series (clamped to SB_SVG_MAXSERIES)
 *   logy    non-zero = logarithmic y axis with decade ticks.  Use for
 *           latency: a cache-hit-to-PMEG-wall span (100ns .. 300us) is
 *           3+ decades, which flattens a linear axis into a floor.
 */
void sb_svg_chart P_((FILE *f, char *title, char *yunit,
                      char **xlab, int nx,
                      char **snames, double *vals, int ns, int logy));

#endif

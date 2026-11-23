/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * svgchart.c -- self-contained SVG line charts.  See svgchart.h.
 *
 * Geometry is a fixed 720x280 viewBox scaled to the container width.  All
 * text/stroke/fill styling is inline attributes, deliberately: charts must
 * survive being injected into reports whose <style> block predates them.
 */
#include "port.h"
#include "svgchart.h"

#include <stdio.h>
#include <string.h>
#include <math.h>       /* log10 for the logarithmic y axis (-lm anyway) */

#define CW   720                 /* viewBox width  */
#define CH   280                 /* viewBox height */
#define ML   62                  /* margins: left, right, top, bottom */
#define MR   16
#define MT   34
#define MB   40
#define PW   (CW - ML - MR)      /* plot area */
#define PH   (CH - MT - MB)

static char *sb_svg_colors[SB_SVG_MAXSERIES] = {
    "#5661d6", "#2e9e63", "#d97706", "#d64545", "#8b5cf6", "#0e7490"
};

/* Format a tick/axis number compactly. */
static void fmtnum(buf, v)
    char *buf;
    double v;
{
    if (v >= 1000.0)
        sprintf(buf, "%.0f", v);
    else if (v >= 100.0)
        sprintf(buf, "%.0f", v);
    else if (v >= 10.0)
        sprintf(buf, "%.1f", v);
    else if (v >= 1.0)
        sprintf(buf, "%.2f", v);
    else
        sprintf(buf, "%.3f", v);
}

/* Escape &<> when echoing caller text into SVG. */
static void sesc(f, s)
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

/* y pixel for value v.  Linear: 0..yr maps onto the plot height.  Log:
 * decades d0..d1 map onto it; v is clamped to the decade range. */
static int ypix(v, logy, yr, d0, d1)
    double v;
    int logy;
    double yr;
    int d0, d1;
{
    double frac;
    if (logy) {
        double lo = (double)d0, hi = (double)d1, lv;
        if (v < 1.0e-30) v = 1.0e-30;
        lv = log10(v);
        if (lv < lo) lv = lo;
        if (lv > hi) lv = hi;
        frac = (lv - lo) / (hi - lo);
    } else
        frac = v / yr;
    return MT + PH - (int)((double)PH * frac);
}

/* Compact tick label: 1000 -> "1K", 1000000 -> "1M". */
static void fmttick(buf, v)
    char *buf;
    double v;
{
    if (v >= 1.0e6 && v == (double)(long)(v / 1.0e6) * 1.0e6)
        sprintf(buf, "%.0fM", v / 1.0e6);
    else if (v >= 1.0e3 && v == (double)(long)(v / 1.0e3) * 1.0e3)
        sprintf(buf, "%.0fK", v / 1.0e3);
    else if (v >= 1.0)
        sprintf(buf, "%.0f", v);
    else if (v >= 0.1)
        sprintf(buf, "%.1f", v);
    else
        sprintf(buf, "%.2f", v);
}

void sb_svg_chart(f, title, yunit, xlab, nx, snames, vals, ns, logy)
    FILE *f;
    char *title, *yunit;
    char **xlab;
    int nx;
    char **snames;
    double *vals;
    int ns;
    int logy;
{
    double ymax, ymin, step, mag, norm, yr, v;
    int s, i, nyt, xstep, lx, d0, d1;
    char nb[40];

    if (nx > SB_SVG_MAXPTS) nx = SB_SVG_MAXPTS;
    if (ns > SB_SVG_MAXSERIES) ns = SB_SVG_MAXSERIES;
    if (nx < 2 || ns < 1)
        return;

    /* y scale: 0 .. a nice number just above the data maximum (linear),
     * or whole decades spanning min..max (log). */
    ymax = 0.0;
    ymin = 1.0e30;
    for (s = 0; s < ns; s++)
        for (i = 0; i < nx; i++) {
            v = vals[s * nx + i];
            if (v > ymax) ymax = v;
            if (v > 0.0 && v < ymin) ymin = v;
        }
    if (ymax <= 0.0)
        return;
    d0 = d1 = 0;
    step = yr = 1.0;
    nyt = 1;
    if (logy) {
        d0 = (int)floor(log10(ymin));
        d1 = (int)ceil(log10(ymax));
        if (d1 <= d0) d1 = d0 + 1;
    } else {
        mag = 1.0;
        norm = ymax / 4.0;
        while (norm >= 10.0) { norm /= 10.0; mag *= 10.0; }
        while (norm < 1.0)   { norm *= 10.0; mag /= 10.0; }
        if (norm <= 1.0)      step = 1.0 * mag;
        else if (norm <= 2.0) step = 2.0 * mag;
        else if (norm <= 5.0) step = 5.0 * mag;
        else                  step = 10.0 * mag;
        nyt = (int)(ymax / step) + 1;
        yr = step * (double)nyt;      /* y range top */
    }

    fprintf(f, "<svg viewBox=\"0 0 %d %d\" width=\"100%%\" style=\"margin-top:10px;display:block\" xmlns=\"http://www.w3.org/2000/svg\">\n", CW, CH);

    /* title + unit */
    fprintf(f, "<text x=\"%d\" y=\"16\" font-family=\"sans-serif\" font-size=\"13\" font-weight=\"bold\" fill=\"#1a2030\">",
            ML);
    sesc(f, title);
    fputs("</text>\n", f);
    if (yunit && yunit[0]) {
        fprintf(f, "<text x=\"%d\" y=\"%d\" font-family=\"sans-serif\" font-size=\"10\" fill=\"#8a92a6\" transform=\"rotate(-90 12 %d)\" text-anchor=\"middle\">",
                12, MT + PH / 2, MT + PH / 2);
        sesc(f, yunit);
        fputs("</text>\n", f);
    }

    /* grid + y tick labels */
    if (logy) {
        double tv = pow(10.0, (double)d0);
        for (i = d0; i <= d1; i++) {
            int y = MT + PH
                  - (int)((double)PH * (double)(i - d0) / (double)(d1 - d0));
            fprintf(f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"%s\" stroke-width=\"1\"/>\n",
                    ML, y, ML + PW, y, i == d0 ? "#9aa1b5" : "#e7eaf2");
            fmttick(nb, tv);
            fprintf(f, "<text x=\"%d\" y=\"%d\" font-family=\"sans-serif\" font-size=\"10\" fill=\"#5a6478\" text-anchor=\"end\">%s</text>\n",
                    ML - 6, y + 3, nb);
            tv *= 10.0;
        }
    } else {
        for (i = 0; i <= nyt; i++) {
            int y = MT + PH - (int)((double)PH * (double)i / (double)nyt);
            fprintf(f, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"%s\" stroke-width=\"1\"/>\n",
                    ML, y, ML + PW, y, i == 0 ? "#9aa1b5" : "#e7eaf2");
            fmtnum(nb, step * (double)i);
            fprintf(f, "<text x=\"%d\" y=\"%d\" font-family=\"sans-serif\" font-size=\"10\" fill=\"#5a6478\" text-anchor=\"end\">%s</text>\n",
                    ML - 6, y + 3, nb);
        }
    }

    /* x tick labels (categorical; label every xstep-th point) */
    xstep = (nx + 7) / 8;
    for (i = 0; i < nx; i++) {
        int x = ML + (int)((double)PW * (double)i / (double)(nx - 1));
        if (i % xstep != 0 && i != nx - 1)
            continue;
        fprintf(f, "<text x=\"%d\" y=\"%d\" font-family=\"sans-serif\" font-size=\"10\" fill=\"#5a6478\" text-anchor=\"middle\">", x, MT + PH + 16);
        sesc(f, xlab[i]);
        fputs("</text>\n", f);
    }

    /* series: polyline segments (broken at missing points) + markers */
    for (s = 0; s < ns; s++) {
        int open = 0;
        for (i = 0; i < nx; i++) {
            v = vals[s * nx + i];
            if (v < 0.0) {
                if (open) { fputs("\"/>\n", f); open = 0; }
                continue;
            }
            {
                int x = ML + (int)((double)PW * (double)i
                                   / (double)(nx - 1));
                int y = ypix(v, logy, yr, d0, d1);
                if (!open) {
                    fprintf(f, "<polyline fill=\"none\" stroke=\"%s\" stroke-width=\"2\" points=\"",
                            sb_svg_colors[s]);
                    open = 1;
                }
                fprintf(f, "%d,%d ", x, y);
            }
        }
        if (open) fputs("\"/>\n", f);
        for (i = 0; i < nx; i++) {
            v = vals[s * nx + i];
            if (v < 0.0) continue;
            {
                int x = ML + (int)((double)PW * (double)i
                                   / (double)(nx - 1));
                int y = ypix(v, logy, yr, d0, d1);
                fprintf(f, "<circle cx=\"%d\" cy=\"%d\" r=\"2.5\" fill=\"%s\"/>\n", x, y, sb_svg_colors[s]);
            }
        }
    }

    /* legend: right-aligned swatch+name pairs on the title line */
    lx = CW - MR;
    for (s = ns - 1; s >= 0; s--) {
        int w = (int)strlen(snames[s]) * 7 + 18;
        lx -= w;
        fprintf(f, "<rect x=\"%d\" y=\"8\" width=\"10\" height=\"10\" rx=\"2\" fill=\"%s\"/>\n", lx, sb_svg_colors[s]);
        fprintf(f, "<text x=\"%d\" y=\"17\" font-family=\"sans-serif\" font-size=\"11\" fill=\"#5a6478\">", lx + 14);
        sesc(f, snames[s]);
        fputs("</text>\n", f);
    }

    fputs("</svg>\n", f);
}

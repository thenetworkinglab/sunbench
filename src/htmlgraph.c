/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * htmlgraph.c -- sunbench-htmlgraph: inject SVG line charts into an
 * already-generated sunbench HTML report.
 *
 *     make htmlgraph
 *     ./sunbench-htmlgraph report.html [out.html]
 *
 * With no output name the report is rewritten in place (the original is
 * saved as report.html.bak first).  Runs anywhere -- it is meant for the
 * modern machine where the reports are viewed, so old reports collected
 * before charts existed can be upgraded without re-running the benchmarks.
 *
 * It knows nothing about specific sunbench versions: any table whose rows
 * are a size ladder (first column starts with a digit, four or more rows)
 * gets a chart of its numeric columns, so it tolerates schema evolution.
 * Columns whose header mentions loss or IOPS are left out of the chart;
 * a latency column gets its own chart.  Tables already followed by an
 * <svg> are skipped, so running the tool twice is harmless.
 */
#include "port.h"
#include "svgchart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXCOL  8
#define MAXROW  SB_SVG_MAXPTS
#define CELLLEN 64

static char hdr[MAXCOL][CELLLEN];
static char lab[MAXROW][CELLLEN];
static double cell[MAXROW][MAXCOL];
static int ncol, nrow;
static char sect[128];

/* Copy the text content of an HTML fragment: tags dropped, a few entities
 * decoded, whitespace squeezed. */
static void striptext(src, len, dst, max)
    char *src; int len; char *dst; int max;
{
    int i, o, intag, sp;
    o = 0; intag = 0; sp = 0;
    for (i = 0; i < len && o < max - 1; i++) {
        char ch = src[i];
        if (ch == '<') { intag = 1; continue; }
        if (ch == '>') { intag = 0; continue; }
        if (intag) continue;
        if (ch == '&') {
            if (strncmp(src + i, "&amp;", 5) == 0)      { ch = '&'; i += 4; }
            else if (strncmp(src + i, "&mdash;", 7) == 0) { ch = '-'; i += 6; }
            else if (strncmp(src + i, "&times;", 7) == 0) { ch = 'x'; i += 6; }
            else if (strncmp(src + i, "&lt;", 4) == 0)  { ch = '<'; i += 3; }
            else if (strncmp(src + i, "&gt;", 4) == 0)  { ch = '>'; i += 3; }
            else {                        /* unknown entity: skip it */
                while (i < len && src[i] != ';') i++;
                continue;
            }
        }
        if (ch == '\n' || ch == '\t') ch = ' ';
        if (ch == ' ') {
            if (sp || o == 0) continue;
            sp = 1;
        } else
            sp = 0;
        dst[o++] = ch;
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = '\0';
}

/* Case-insensitive substring test. */
static int hasci(hay, needle)
    char *hay, *needle;
{
    int nl = (int)strlen(needle);
    char *p;
    for (p = hay; *p; p++) {
        int i;
        for (i = 0; i < nl; i++) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (i == nl) return 1;
        if (!p[i] && i < nl) return 0;
    }
    return 0;
}

/* Parse the cells of one table (text between <table...> and </table>).
 * Returns 0 if it doesn't look like a chartable size ladder. */
static int parse_table(t, tlen)
    char *t; int tlen;
{
    char *p, *end, *cs, *ce;
    int inhdr;
    char txt[CELLLEN];

    ncol = 0; nrow = 0;
    end = t + tlen;
    p = t;
    inhdr = 1;
    while (p < end) {
        char *tr = strstr(p, "<tr");
        char *tre;
        int col = 0;
        if (tr == (char *)0 || tr >= end) break;
        tre = strstr(tr, "</tr>");
        if (tre == (char *)0 || tre > end) tre = end;
        cs = tr;
        for (;;) {
            char *td = strstr(cs, "<td");
            char *th = strstr(cs, "<th");
            char *c = td;
            if (th != (char *)0 && (c == (char *)0 || th < c)) c = th;
            if (c == (char *)0 || c >= tre) break;
            cs = strchr(c, '>');
            if (cs == (char *)0 || cs >= tre) break;
            cs++;
            ce = strstr(cs, c[1] == 't' && c[2] == 'h' ? "</th" : "</td");
            if (ce == (char *)0 || ce > tre) ce = tre;
            striptext(cs, (int)(ce - cs), txt, (int)sizeof(txt));
            if (inhdr) {
                if (col < MAXCOL)
                    strcpy(hdr[col], txt);
            } else if (nrow < MAXROW) {
                if (col == 0)
                    strcpy(lab[nrow], txt);
                else if (col < MAXCOL) {
                    char *ep;
                    double v = strtod(txt, &ep);
                    cell[nrow][col] = (ep == txt) ? -1.0 : v;
                }
            }
            col++;
            cs = ce;
        }
        if (inhdr) {
            ncol = col;
            inhdr = 0;
        } else if (col > 0 && nrow < MAXROW)
            nrow++;
        p = tre + 1;
    }
    if (ncol < 2 || nrow < 4)
        return 0;
    if (lab[0][0] < '0' || lab[0][0] > '9')
        return 0;                          /* not a size ladder */
    return 1;
}

/* "64 KB" -> "64K", "1 MB" -> "1M", "65536 bytes" -> "64K", "1472 b.." -> */
static void mklab(in, out)
    char *in, *out;
{
    unsigned long v;
    char *p;
    v = strtoul(in, &p, 10);
    while (*p == ' ') p++;
    if (*p == 'K' || *p == 'k')      sprintf(out, "%luK", v);
    else if (*p == 'M' || *p == 'm') sprintf(out, "%luM", v);
    else if (v >= 1024L && v % 1024L == 0L)
        sprintf(out, "%luK", v / 1024L);
    else
        sprintf(out, "%lu", v);
}

/* Emit chart(s) for the parsed table into f. */
static void emit_charts(f)
    FILE *f;
{
    static char xl[MAXROW][12];
    char *lp[MAXROW];
    char *sn[MAXCOL], *ln[1];
    double v[MAXCOL * MAXROW], lv[MAXROW];
    char unit[24];
    int i, j, ns, latcol;

    for (i = 0; i < nrow; i++) {
        mklab(lab[i], xl[i]);
        lp[i] = xl[i];
    }

    /* pick bandwidth series and (at most one) latency column */
    ns = 0; latcol = -1;
    unit[0] = '\0';
    for (j = 1; j < ncol; j++) {
        if (hasci(hdr[j], "loss") || hasci(hdr[j], "iops"))
            continue;
        if (hasci(hdr[j], "lat")) {
            latcol = j;
            continue;
        }
        if (ns < MAXCOL) {
            sn[ns] = hdr[j][0] ? hdr[j] : "value";
            for (i = 0; i < nrow; i++)
                v[ns * nrow + i] = cell[i][j];
            ns++;
            if (unit[0] == '\0') {
                char *op = strchr(hdr[j], '(');
                if (op != (char *)0) {
                    char *cp = strchr(op, ')');
                    if (cp != (char *)0 &&
                        (int)(cp - op) < (int)sizeof(unit)) {
                        memcpy(unit, op + 1, cp - op - 1);
                        unit[cp - op - 1] = '\0';
                    }
                }
            }
        }
    }
    if (unit[0] == '\0')
        strcpy(unit, hasci(sect, "network") ? "Mbit/s" : "MB/s");

    if (ns > 0)
        sb_svg_chart(f, sect[0] ? sect : "sweep", unit, lp, nrow, sn, v, ns, 0);
    if (latcol >= 0) {
        char lt[160];
        ln[0] = "latency";
        for (i = 0; i < nrow; i++)
            lv[i] = cell[i][latcol];
        sprintf(lt, "%.120s - latency", sect[0] ? sect : "sweep");
        sb_svg_chart(f, lt, "ns", lp, nrow, ln, lv, 1, 1);
    }
}

int main(argc, argv)
    int argc;
    char **argv;
{
    FILE *f;
    char *buf, *p, *outpath;
    char bak[1100];
    long sz;
    int charted = 0;

    if (argc < 2 || argc > 3) {
        fprintf(stderr,
            "usage: sunbench-htmlgraph report.html [out.html]\nadds SVG charts to an existing sunbench HTML report\n(in place if no output name; the original goes to .bak)\n");
        return 2;
    }
    f = fopen(argv[1], "r");
    if (f == (FILE *)0) { perror(argv[1]); return 1; }
    (void)fseek(f, 0L, SEEK_END);
    sz = ftell(f);
    rewind(f);
    buf = (char *)malloc((unsigned)(sz + 1));
    if (buf == (char *)0) { fprintf(stderr, "out of memory\n"); return 1; }
    if ((long)fread(buf, 1, (unsigned)sz, f) != sz) {
        fprintf(stderr, "short read\n");
        return 1;
    }
    buf[sz] = '\0';
    (void)fclose(f);

    outpath = (argc == 3) ? argv[2] : argv[1];
    if (argc == 2) {                      /* in place: keep a backup */
        sprintf(bak, "%.1000s.bak", argv[1]);
        f = fopen(bak, "w");
        if (f != (FILE *)0) {
            (void)fwrite(buf, 1, (unsigned)sz, f);
            (void)fclose(f);
        }
    }
    f = fopen(outpath, "w");
    if (f == (FILE *)0) { perror(outpath); return 1; }

    sect[0] = '\0';
    p = buf;
    for (;;) {
        char *h2 = strstr(p, "<h2>");
        char *tb = strstr(p, "<table");
        if (tb == (char *)0 || (h2 != (char *)0 && h2 < tb)) {
            if (h2 == (char *)0) break;
            {
                char *he = strstr(h2, "</h2>");
                if (he == (char *)0) break;
                striptext(h2 + 4, (int)(he - h2 - 4), sect,
                          (int)sizeof(sect));
                (void)fwrite(p, 1, (unsigned)(he + 5 - p), f);
                p = he + 5;
            }
            continue;
        }
        {
            char *te = strstr(tb, "</table>");
            char *after, *q;
            if (te == (char *)0) break;
            after = te + 8;
            (void)fwrite(p, 1, (unsigned)(after - p), f);
            p = after;
            q = after;
            while (*q == '\n' || *q == ' ' || *q == '\t') q++;
            if (strncmp(q, "<svg", 4) == 0)
                continue;                  /* already charted: idempotent */
            if (parse_table(tb, (int)(te - tb))) {
                fputs("\n", f);
                emit_charts(f);
                charted++;
            }
        }
    }
    (void)fwrite(p, 1, strlen(p), f);
    (void)fclose(f);
    free(buf);
    printf("sunbench-htmlgraph: %d chart group%s added to %s\n",
           charted, charted == 1 ? "" : "s", outpath);
    return 0;
}

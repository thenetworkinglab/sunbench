/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * sbnet.c -- SBDOS's network test: sunbench's UDP round trip and UDP
 * upload/download sweeps, against sunbench-netd over its UDP control
 * protocol (src/netproto.h), through a DOS packet driver (pktnet.c).
 *
 * The method mirrors src/bench_net.c, so the numbers line up with the
 * Unix machines' UDP rows:
 *   upload    rate over the SERVER's first-to-last arrival span (with
 *             n arrivals spanning n-1 gaps), loss = sent vs received;
 *   download  server paced to 1.5x our upload rate for the size, halved
 *             while loss > 40%; rate over OUR arrival span.
 * TCP is not measured (no TCP stack here); the log and HTML say "UDP
 * round trip" for the RTT, which is a datagram echo, not a TCP one.
 *
 * IP settings come from mTCP's configuration file (MTCPCFG), the way
 * the rest of a DOS network setup gets them; options override.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "pktnet.h"
#include "sbdos.h"
#include "html.h"

#define NET_SIZES 8

static char          *n_server = (char *)0;
static unsigned char  srv_ip[4];
static unsigned       srv_port = 5202;
static int            n_pktint = 0;            /* 0: from config, or scan */
static char          *n_ip = (char *)0, *n_mask = (char *)0, *n_gw = (char *)0;
static int            n_mtu = 0;
static int            n_ran = 0;
static char           n_syn[200];

static unsigned char  c_ip[4], c_mask[4], c_gw[4];
static int            c_mtu = 1500;
static int            c_have_ip = 0, c_have_mask = 0, c_have_gw = 0;
static char           c_file[96];

static pn_dgram dg;

int sbnet_opt(char *name, char *val)
{
    if (strcmp(name, "--server") == 0) n_server = val;
    else if (strcmp(name, "--port") == 0) srv_port = (unsigned)atoi(val);
    else if (strcmp(name, "--pktint") == 0) n_pktint = (int)strtol(val, (char **)0, 16);
    else if (strcmp(name, "--ip") == 0) n_ip = val;
    else if (strcmp(name, "--mask") == 0) n_mask = val;
    else if (strcmp(name, "--gw") == 0) n_gw = val;
    else if (strcmp(name, "--mtu") == 0) n_mtu = atoi(val);
    else return 0;
    return 1;
}

int sbnet_ready(void) { return n_server != (char *)0; }

char *sbnet_syn(void) { return n_ran ? n_syn : (char *)0; }

static int quad(char *s, unsigned char *q)
{
    int a, b, c, d;
    if (s == (char *)0 || sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4 ||
        a < 0 || a > 255 || b < 0 || b > 255 ||
        c < 0 || c > 255 || d < 0 || d > 255)
        return -1;
    q[0] = (unsigned char)a; q[1] = (unsigned char)b;
    q[2] = (unsigned char)c; q[3] = (unsigned char)d;
    return 0;
}

/* mTCP's config file: "KEYWORD value" lines.  PACKETINT, IPADDR,
 * NETMASK, GATEWAY and MTU are the ones that matter here (mTCP's DHCP
 * client writes the IP ones into the same file). */
static void read_mtcp_cfg(void)
{
    char *path = getenv("MTCPCFG");
    char line[160], key[32], val[96];
    FILE *f;
    int i;

    if (path == (char *)0) return;
    f = fopen(path, "r");
    if (f == (FILE *)0) {
        fprintf(stderr, "sbdos: cannot read MTCPCFG file %s\n", path);
        return;
    }
    strncpy(c_file, path, sizeof(c_file) - 1);
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%31s %95s", key, val) != 2) continue;
        for (i = 0; key[i]; i++) key[i] = (char)toupper(key[i]);
        if (strcmp(key, "PACKETINT") == 0 && n_pktint == 0)
            n_pktint = (int)strtol(val, (char **)0, 16);
        else if (strcmp(key, "IPADDR") == 0)  c_have_ip = quad(val, c_ip) == 0;
        else if (strcmp(key, "NETMASK") == 0) c_have_mask = quad(val, c_mask) == 0;
        else if (strcmp(key, "GATEWAY") == 0) c_have_gw = quad(val, c_gw) == 0;
        else if (strcmp(key, "MTU") == 0) c_mtu = atoi(val);
    }
    fclose(f);
}

static unsigned long g_id = 0;
static long new_id(void)
{
    if (g_id == 0) g_id = ((unsigned long)time((time_t *)0) & 0xFFFFFL) * 37L + 1L;
    g_id += 7919L;
    return (long)(g_id & 0x3FFFFFFFL);
}

/* Is dg a control reply "<word> <id>"? */
static int is_reply(char *word, long id)
{
    char want[40];
    int n;
    if (dg.sport != srv_port || memcmp(dg.src, srv_ip, 4) != 0) return 0;
    sprintf(want, "%s %ld", word, id);
    n = (int)strlen(want);
    return strncmp(dg.data, want, n) == 0 &&
           (dg.data[n] == ' ' || dg.data[n] == '\n' || dg.data[n] == '\0');
}

static int wait_reply(char *word, long id, double tmo)
{
    double t0 = now_s();
    while (now_s() - t0 < tmo)
        while (pn_poll(&dg))
            if (is_reply(word, id))
                return 1;
    return 0;
}

static int transact(char *cmd, char *word, long id, int tries, double tmo)
{
    int i;
    for (i = 0; i < tries; i++) {
        (void)pn_send(srv_port, cmd, (unsigned)strlen(cmd));
        if (wait_reply(word, id, tmo))
            return 1;
    }
    return 0;
}

/* BIOS ticks (18.2 Hz): the cheap loop-end check for a blast. */
static unsigned long bios_ticks(void)
{
    unsigned long t;
    unsigned e;
    now_raw(&t, &e);
    return t;
}

/* --- round trip: datagram echoes for ~secs (at most 1000) --------------- */
static int udp_rtt(int secs, double *ms, int *lost)
{
    double t0, a, sum = 0.0;
    int ok = 0, n;
    char cmd[40];

    *lost = 0;
    t0 = now_s();
    for (n = 0; n < 1000 && now_s() - t0 < (double)secs; n++) {
        sprintf(cmd, "SBN1 PING %d\n", n);
        a = now_s();
        (void)pn_send(srv_port, cmd, (unsigned)strlen(cmd));
        if (wait_reply("PONG", (long)n, 1.0)) {
            sum += now_s() - a;
            ok++;
        } else
            (*lost)++;
    }
    if (ok == 0) return -1;
    *ms = sum / ok * 1000.0;
    return 0;
}

/* --- UDP upload ------------------------------------------------------------ */
static int udp_up(int secs, int size, double *mbps, double *loss)
{
    char cmd[64];
    long id = new_id();
    unsigned dport, k = 0;
    unsigned long tend, t1;
    unsigned e1;
    double spkts = 0.0, rpkts = 0.0, rbytes = 0.0, rspan = 0.0, el, t0, win;

    sprintf(cmd, "SBN1 UUP %d %d %ld\n", secs, size, id);
    if (!transact(cmd, "GO", id, 4, 1.0)) return -1;
    if (sscanf(dg.data, "GO %*ld %u", &dport) != 1) return -1;

    pn_blast_prep(dport, (unsigned)size);
    t0 = now_s();
    tend = bios_ticks() + (unsigned long)((double)secs * 18.2065 + 0.5);
    for (;;) {
        if (pn_blast() == 0) spkts += 1.0;
        if ((++k & 15) == 0) {
            while (pn_poll(&dg)) ;      /* answer ARP; nothing else is due */
            now_raw(&t1, &e1);
            if (t1 >= tend) break;
        }
    }
    el = raw_s(t1, e1) - t0;

    sprintf(cmd, "SBN1 UEND %ld\n", id);
    if (!transact(cmd, "RES", id, 6, 1.5)) return -1;
    if (sscanf(dg.data, "RES %*ld %lf %lf %lf", &rpkts, &rbytes, &rspan) < 2)
        return -1;
    if (spkts <= 0.0 || el <= 0.0) return -1;
    win = el;
    if (rspan > 0.0 && rpkts >= 2.0 && rspan / 1.0e6 > el * 0.5) {
        win = rspan / 1.0e6;
        rbytes = rbytes * (rpkts - 1.0) / rpkts;
    }
    *mbps = rbytes * 8.0 / win / 1.0e6;
    *loss = (spkts - rpkts) / spkts * 100.0;
    if (*loss < 0.0) *loss = 0.0;
    return 0;
}

/* --- UDP download ---------------------------------------------------------- */
static int udp_down_once(int secs, int size, long kbps, double *mbps, double *loss)
{
    char cmd[80];
    long id = new_id();
    int tries, got_res = 0, started = 0;
    unsigned long tend, ft = 0, lt = 0, nt;
    unsigned fe = 0, le = 0;
    double rpkts = 0.0, rbytes = 0.0, spkts = 0.0, sbytes = 0.0, el;

    sprintf(cmd, "SBN1 UDN %d %d 0 %ld %ld\n", secs, size, kbps, id);
    for (tries = 0; tries < 4 && !started; tries++) {
        (void)pn_send(srv_port, cmd, (unsigned)strlen(cmd));
        tend = bios_ticks() + 19;                 /* ~1 s for GO or data */
        while (!started && bios_ticks() < tend)
            while (pn_poll(&dg)) {
                if (is_reply("GO", id)) { started = 1; break; }
                if (dg.len == (unsigned)size && dg.data[0] == 17) {
                    started = 1;                  /* GO lost; data came */
                    rpkts = 1.0; rbytes = (double)size;
                    now_raw(&ft, &fe); lt = ft; le = fe;
                    break;
                }
            }
    }
    if (!started) return -1;

    tend = bios_ticks() + (unsigned long)((double)(secs * 2 + 15) * 18.2065);
    for (;;) {
        if (pn_poll(&dg)) {
            if (dg.len == (unsigned)size && dg.data[0] == 17) {
                now_raw(&lt, &le);
                if (rpkts == 0.0) { ft = lt; fe = le; }
                rpkts += 1.0;
                rbytes += (double)size;
            } else if (is_reply("RES", id)) {
                if (sscanf(dg.data, "RES %*ld %lf %lf", &spkts, &sbytes) == 2)
                    got_res = 1;
                break;
            }
        } else if (bios_ticks() >= tend)
            break;
    }
    if (!got_res) {                              /* RES lost: ask again */
        sprintf(cmd, "SBN1 URES %ld\n", id);
        if (transact(cmd, "RES", id, 3, 1.5) &&
            sscanf(dg.data, "RES %*ld %lf %lf", &spkts, &sbytes) == 2)
            got_res = 1;
    }
    nt = bios_ticks() + 6;                       /* stragglers, ~300 ms */
    while (bios_ticks() < nt)
        if (pn_poll(&dg) && dg.len == (unsigned)size && dg.data[0] == 17) {
            now_raw(&lt, &le);
            rpkts += 1.0;
            rbytes += (double)size;
        }
    if (!got_res || spkts <= 0.0) return -1;
    el = (double)secs;
    if (rpkts >= 2.0 && raw_s(lt, le) - raw_s(ft, fe) > (double)secs * 0.5) {
        el = raw_s(lt, le) - raw_s(ft, fe);
        rbytes = rbytes * (rpkts - 1.0) / rpkts;
    }
    *mbps = rbytes * 8.0 / el / 1.0e6;
    *loss = (spkts - rpkts) / spkts * 100.0;
    if (*loss < 0.0) *loss = 0.0;
    return 0;
}

static void row3(unsigned long sz, double v, double l)
{
    char line[80];
    if (v < 0.0) sprintf(line, "  %8lu %10s %8s\n", sz, "-", "-");
    else         sprintf(line, "  %8lu %10.2f %7.1f%%\n", sz, v, l);
    out(line);
}

int sbnet_run(void)
{
    static unsigned long ladder[5] = { 64, 256, 512, 1024, 1472 };
    unsigned long sz[NET_SIZES], cap;
    double mbps[NET_SIZES], loss[NET_SIZES], dmbps[NET_SIZES], dloss[NET_SIZES];
    double rtt = 0.0, v, l;
    int i, n, secs, lost = 0, mtu;
    unsigned lport;
    unsigned long f0, d0;
    char line[240];

    read_mtcp_cfg();
    if (n_ip && quad(n_ip, c_ip) == 0) c_have_ip = 1;
    if (n_mask && quad(n_mask, c_mask) == 0) c_have_mask = 1;
    if (n_gw && quad(n_gw, c_gw) == 0) c_have_gw = 1;
    if (n_mtu) c_mtu = n_mtu;
    if (quad(n_server, srv_ip) != 0) {
        fprintf(stderr, "sbdos: --server needs the netd machine's IP address (a.b.c.d)\n");
        return -1;
    }
    if (!c_have_ip) {
        fprintf(stderr, "sbdos: no IP address: set MTCPCFG (run mTCP's DHCP first) or use --ip\n");
        return -1;
    }
    if (!c_have_mask) { c_mask[0] = c_mask[1] = c_mask[2] = 255; c_mask[3] = 0; }
    if (!c_have_gw) memset(c_gw, 0, 4);

    if (pn_open(n_pktint) != 0) return -1;
    atexit(pn_close);                     /* the driver must not call into
                                           * us after we have exited */
    lport = 20000 + (unsigned)(time((time_t *)0) & 0x1FFF);
    pn_config(c_ip, c_mask, c_gw, c_mtu, lport);
    mtu = pn_mtu();
    fprintf(stderr, "sbdos: %s\n", pn_desc());
    if (pn_route(srv_ip) != 0) return -1;
    n_ran = 1;
    sprintf(n_syn, "  net    : %s", pn_desc());

    cap = (unsigned long)(mtu - 28);
    n = 0;
    for (i = 0; i < 5; i++)
        if (ladder[i] < cap) sz[n++] = ladder[i];
    sz[n++] = cap;
    for (i = 0; i < n; i++) { mbps[i] = dmbps[i] = -1.0; loss[i] = dloss[i] = 0.0; }
    secs = o_secs_set ? (int)(o_secs + 0.5) : 5;
    if (secs < 1) secs = 1;
    f0 = pn_rx_frames(); d0 = pn_rx_drops();

    fprintf(stderr, "  UDP round trip...\n");
    if (udp_rtt(secs < 3 ? secs : 3, &rtt, &lost) != 0) {
        fprintf(stderr, "sbdos: no PONG from %s port %u: is sunbench-netd (this version or later) running?\n",
                n_server, srv_port);
        return -1;
    }
    for (i = 0; i < n; i++) {
        fprintf(stderr, "  [%d/%d] UDP up %lu bytes\n", i + 1, n, sz[i]);
        if (udp_up(secs, (int)sz[i], &v, &l) == 0) { mbps[i] = v; loss[i] = l; }
    }
    for (i = 0; i < n; i++) {
        long kbps;                  /* long: int is 16 bits here, and
                                     * 1.5 x 27 Mbit/s is 40500 kbit/s */
        int t;
        kbps = mbps[i] > 0.0 ? (long)(mbps[i] * 1500.0) : 10000L;
        if (kbps < 64) kbps = 64;
        fprintf(stderr, "  [%d/%d] UDP down %lu bytes\n", i + 1, n, sz[i]);
        for (t = 0; t < 5; t++) {
            if (udp_down_once(secs, (int)sz[i], kbps, &v, &l) != 0) break;
            dmbps[i] = v; dloss[i] = l;
            if (l <= 40.0 || kbps <= 64) break;
            kbps /= 2;
            if (kbps < 64) kbps = 64;
            fprintf(stderr, "  [%d/%d] UDP down %lu bytes: %.0f%% loss, repacing at %ld kbit/s\n",
                    i + 1, n, sz[i], l, kbps);
        }
    }

    stamp(0);
    out("sunbench -- network\n");
    sprintf(line, "  server : %s port %u   local MTU %d\n", n_server, srv_port, mtu);
    out(line);
    sprintf(line, "  driver : %s\n", pn_desc());
    out(line);
    sprintf(line, "  rx     : %lu frames, %lu refused (SBDOS receive ring full)\n",
            pn_rx_frames() - f0, pn_rx_drops() - d0);
    out(line);
    sprintf(line, "  %-26s %9.2f ms%s\n", "UDP round trip", rtt,
            lost ? "   (some echoes lost)" : "");
    out(line);
    sprintf(line, "  UDP upload sweep (%ds per size):\n", secs);
    out(line);
    sprintf(line, "  %8s %10s %8s\n", "bytes", "Mbit/s", "loss");
    out(line);
    for (i = 0; i < n; i++) row3(sz[i], mbps[i], loss[i]);
    sprintf(line, "  UDP download sweep (%ds per size; paced to 1.5x upload, halved while loss > 40%%):\n", secs);
    out(line);
    sprintf(line, "  %8s %10s %8s\n", "bytes", "Mbit/s", "loss");
    out(line);
    for (i = 0; i < n; i++) row3(sz[i], dmbps[i], dloss[i]);

    sprintf(line, "%s port %u", n_server, srv_port);
    sb_html_net_rtt_label("UDP round trip");
    sb_html_net(line, mtu, 0.0, 0.0, rtt, (unsigned long *)0, (double *)0,
                (double *)0, 0, sz, mbps, loss, dmbps, dloss, n);
    html_flush();
    return 0;
}

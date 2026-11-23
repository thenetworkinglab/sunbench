/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * bench_net.h -- network benchmark client (iperf-like, over IP only).
 *
 * The client runs on the Sun under test; sunbench-netd runs on a modern
 * machine so the far end is never the bottleneck.  Everything rides plain
 * TCP/UDP over IP, so ANY IP-capable interface works the same way --
 * Ethernet, token ring, FDDI, ATM LANE, PPP -- with no link-layer code.
 *
 * Tests:
 *   TCP upload    client streams for N secs; the SERVER's received-byte
 *                 count is the result (what actually got through).
 *   TCP download  server streams; client counts.
 *   TCP RTT       1-byte echo round trips, time-bounded; average ms.
 *   UDP sweep     datagrams of a given size blasted for N secs; achieved
 *                 Mbit/s AND loss%.  Swept across sizes up to the local
 *                 interface's MTU by default (Ethernet 1500, FDDI 4352,
 *                 token ring / ATM larger) -- the size where each interface
 *                 shows its personality.
 *
 * All rates are Mbit/s (network convention), 1 Mbit = 1e6 bits.
 */
#ifndef SUNBENCH_BENCH_NET_H
#define SUNBENCH_BENCH_NET_H
#include "port.h"
#include "netproto.h"

/* Resolve + probe the server once; returns 0 and fills a connection handle
 * cookie (opaque to callers) or -1 with a message on stderr. */
int sb_net_check P_((char *host, int port));

/* Local interface MTU on the route to `host` (via SIOCGIFCONF/SIOCGIFMTU),
 * or 0 if it cannot be determined (caller assumes 1500). */
int sb_net_mtu P_((char *host));

/* TCP stream for `secs`; direction 0 = upload (client->server), 1 = down.
 * `chunk` is the application write()/read() size (clamped to SBN_BUF) --
 * NOTE this is NOT the wire packet size (the kernel segments to the MSS and
 * coalesces small writes); sweeping it measures the per-syscall/copy cost
 * that dominates on slow CPUs.  Fills *mbps.  Returns 0 on success. */
int sb_net_tcp P_((char *host, int port, int down, int secs, int chunk,
                   double *mbps));

/* Echo round trips for about `secs` (at most 1000); fills *ms (avg RTT). */
int sb_net_rtt P_((char *host, int port, int secs, double *ms));

/* UDP blast of `size`-byte datagrams for `secs`; fills achieved *mbps
 * (server-received bytes over the send window) and *loss_pct. */
int sb_net_udp P_((char *host, int port, int secs, int size,
                   double *mbps, double *loss_pct));

/* UDP download: the server sends `size`-byte datagrams to us, PACED to
 * `kbps` kilobits/s (ask for ~1.5x your upstream result so loss reflects
 * the local receive ceiling, not the raw mismatch with a fast server).
 * Fills received *mbps and *loss_pct vs the server's sent count. */
int sb_net_udp_down P_((char *host, int port, int secs, int size, int kbps,
                        double *mbps, double *loss_pct));

#endif

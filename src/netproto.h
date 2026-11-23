/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * netproto.h -- the tiny wire protocol between `sunbench net` (client, runs
 * on the Sun) and sunbench-netd (server, runs on a modern machine so it is
 * never the bottleneck).
 *
 * Control is ASCII lines over TCP, one fresh connection per test, so the
 * server stays a trivial iterative loop and old clients keep working:
 *
 *   "SBN1 UP <secs>\n"          client streams payload for <secs>, then
 *                               shuts down its write side; server counts and
 *                               replies "RES <bytes> <usecs>\n".
 *   "SBN1 DOWN <secs>\n"        server replies GO, streams for <secs>,
 *                               closes; client counts.
 *   "SBN1 RTT\n"                server replies GO, then echoes every byte
 *                               until EOF; client times round trips.
 *   "SBN1 UDP <secs> <size>\n"  server opens an ephemeral UDP socket and
 *                               replies "GO <udpport>\n"; client blasts
 *                               <size>-byte datagrams for <secs>, then sends
 *                               "END\n" on the control connection; server
 *                               drains, replies "RES <pkts> <bytes>\n".
 *   "SBN1 UDPD <secs> <size> <port> <kbps>\n"
 *                               download direction: server replies "GO\n",
 *                               then sends <size>-byte datagrams to the
 *                               client's address at UDP <port>, PACED to
 *                               <kbps> kilobits/s (the client asks for ~1.5x
 *                               its own upstream result, so loss reflects
 *                               the client's receive ceiling rather than the
 *                               raw speed mismatch with a fast server), for
 *                               <secs>; then "RES <pkts> <bytes>\n" (sent).
 *
 * UDP control -- for clients with no TCP (SBDOS's packet-driver stack).
 * The same server also takes commands as datagrams on the control port
 * (UDP 5202) and replies to the sender.  <id> is a client-chosen number;
 * repeating a command is always safe (a lost reply is just asked for
 * again, and a repeat of the test just finished returns its RES):
 *
 *   "SBN1 PING <seq>"           -> "PONG <seq>"  (round trip, at once)
 *   "SBN1 UUP <secs> <size> <id>"
 *                               -> "GO <id> <udpport>"; the client blasts
 *                               <size>-byte datagrams at <udpport>, then
 *                               sends "SBN1 UEND <id>" (repeat until
 *                               answered) -> "RES <id> <pkts> <bytes> <span_us>"
 *   "SBN1 UDN <secs> <size> <port> <kbps> <id>"
 *                               -> "GO <id>", then paced <size>-byte
 *                               datagrams FROM the control port to the
 *                               client's <port> (0 = its source port) for
 *                               <secs>, then "RES <id> <pkts> <bytes>";
 *                               "SBN1 URES <id>" asks again if it was lost.
 *                               Payload bytes start with 17, never "RES".
 *
 * After UP/DOWN/RTT headers the server sends exactly "GO\n" (3 bytes) before
 * any payload, so the client can switch from line reading to raw counting.
 * Everything is ASCII decimal: no byte-order concerns, debuggable w/ telnet.
 */
#ifndef SUNBENCH_NETPROTO_H
#define SUNBENCH_NETPROTO_H

#define SBN_PORT     5202        /* default control (TCP) port */
#define SBN_MAGIC    "SBN1"
#define SBN_BUF      65536       /* stream chunk size */
#define SBN_MAXDGRAM 65000       /* refuse UDP sizes above this */
#define SBN_LINE     128         /* max control-line length */

#endif

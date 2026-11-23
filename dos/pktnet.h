/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * pktnet.h -- SBDOS's minimal IPv4/UDP over a DOS packet driver.
 *
 * Just enough network for sunbench's UDP tests against sunbench-netd:
 * the packet driver interface (PC/TCP spec 1.09), ARP, IPv4 (no
 * fragmentation, no options) and UDP.  No TCP -- the tests use netd's
 * UDP control protocol (see src/netproto.h).  One local UDP port.
 *
 * Link layers: class 1 (DIX Ethernet).  Token ring (class 3) is planned;
 * the link header is built and parsed in one place each (pktnet.c) so
 * it can be added without touching the rest.
 */
#ifndef SBDOS_PKTNET_H
#define SBDOS_PKTNET_H

#define PN_CAP 128              /* payload bytes copied out per datagram */

typedef struct pn_dgram {
    unsigned char src[4];       /* sender's IP */
    unsigned      sport;        /* sender's UDP port */
    unsigned      len;          /* UDP payload length */
    char          data[PN_CAP + 1];  /* first PN_CAP bytes, NUL-ended */
} pn_dgram;

/* Find the packet driver (vec 0 = scan 0x60-0x80), check its class,
 * register for IP and ARP, read our MAC.  0, or -1 with a message. */
int  pn_open(int vec);
void pn_close(void);            /* release the handles: MUST run at exit */

/* Our address, netmask, gateway (dotted quads already parsed), MTU. */
void pn_config(unsigned char *ip, unsigned char *mask, unsigned char *gw,
               int mtu, unsigned lport);

/* Driver description for the log ("NE2000 v11, class 1 Ethernet, ..."). */
char *pn_desc(void);
int   pn_mtu(void);

/* ARP for the next hop toward dst (gateway if off-subnet): 0 when known. */
int  pn_route(unsigned char *dst);

/* One UDP datagram to the routed destination.  0 sent, -1 driver error. */
int  pn_send(unsigned dport, char *data, unsigned len);

/* Blast path: build one datagram of `len` pattern bytes to dport once,
 * then pn_blast() re-sends it with a fresh IP id. */
void pn_blast_prep(unsigned dport, unsigned len);
int  pn_blast(void);

/* Service the receive ring (answering ARP); 1 when a datagram for our
 * port has been copied to *d, 0 when the ring is empty. */
int  pn_poll(pn_dgram *d);

/* Counters since pn_open. */
unsigned long pn_rx_frames(void);   /* frames the driver handed us */
unsigned long pn_rx_drops(void);    /* refused: our receive ring was full */

#endif

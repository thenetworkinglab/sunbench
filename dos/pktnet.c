/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * pktnet.c -- SBDOS's minimal IPv4/UDP over a DOS packet driver.
 * See pktnet.h.  The receive upcall itself is dos/rxup.asm.
 */
#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include "pktnet.h"

extern double now_s(void);          /* sbdos.c: the 8254 clock */

/* --- the receive ring, filled by rxup.asm ------------------------------ */
#define RX_SLOTS 16
#define RX_SLOT  2048

struct rxq_t {
    unsigned head, tail, drops, frames;
    unsigned bseg;                  /* the slots: 32 KB from DOS */
    unsigned len[RX_SLOTS];
};
extern volatile struct rxq_t __far rxq;
#pragma aux rxq "rxq_"
extern void __far rx_upcall(void);
#pragma aux rx_upcall "rx_upcall_"

/* --- state --------------------------------------------------------------- */
static int pd_vec = 0;              /* packet driver interrupt */
static int pd_class, pd_type, pd_ver, pd_func;
static char pd_name[32];
static int h_ip = -1, h_arp = -1;   /* access_type handles */
static unsigned char my_mac[6];
static unsigned char my_ip[4], my_mask[4], my_gw[4];
static int my_mtu = 1500;
static unsigned my_port = 0;
static unsigned rx_frames_last = 0;
static unsigned long rx_frames_total = 0;
static unsigned long rx_drops_total = 0;
static unsigned rx_drops_last = 0;

static unsigned char dst_ip[4];     /* where pn_send goes */
static unsigned char hop_ip[4];     /* next hop: dst or gateway */
static unsigned char hop_mac[6];
static int hop_known = 0;
static unsigned ip_id = 1;

static unsigned char txbuf[1600];   /* UDP sends (control and blast) */
static unsigned blast_len = 0;      /* frame length prepared in txbuf */
static unsigned char arpbuf[60];

#define LHL 14                      /* link header length (Ethernet) */

/* --- packet driver calls ------------------------------------------------ */
static int pd_call(r, s)
    union REGS *r; struct SREGS *s;
{
    int86x(pd_vec, r, r, s);
    return r->x.cflag ? -1 : 0;
}

static int pd_found(vec)
    int vec;
{
    unsigned char far *p;
    p = *(unsigned char far * far *)MK_FP(0, vec * 4);
    if (p == (unsigned char far *)0)
        return 0;
    return _fmemcmp(p + 3, "PKT DRVR", 8) == 0;
}

static int pd_access(type)
    unsigned type;
{
    union REGS r;
    struct SREGS s;
    static unsigned char tb[2][2];
    static int which = 0;
    unsigned char *t = tb[which++ & 1];

    t[0] = (unsigned char)(type >> 8);      /* ethertype, network order */
    t[1] = (unsigned char)type;
    segread(&s);
    r.h.ah = 2;
    r.h.al = (unsigned char)pd_class;
    r.x.bx = 0xFFFF;                        /* any interface type */
    r.h.dl = 0;                             /* interface number 0 */
    s.ds = FP_SEG(t);
    r.x.si = FP_OFF(t);
    r.x.cx = 2;
    s.es = FP_SEG(rx_upcall);
    r.x.di = FP_OFF(rx_upcall);
    if (pd_call(&r, &s) != 0)
        return -1;
    return r.x.ax;
}

static void pd_release(h)
    int h;
{
    union REGS r;
    struct SREGS s;
    if (h < 0) return;
    segread(&s);
    r.h.ah = 3;
    r.x.bx = (unsigned)h;
    (void)pd_call(&r, &s);
}

static int pd_send(buf, len)
    unsigned char *buf; unsigned len;
{
    union REGS r;
    struct SREGS s;
    segread(&s);
    r.h.ah = 4;
    s.ds = FP_SEG(buf);
    r.x.si = FP_OFF(buf);
    r.x.cx = len;
    return pd_call(&r, &s);
}

int pn_open(vec)
    int vec;
{
    union REGS r;
    struct SREGS s;
    char far *nm;
    int i;

    if (vec == 0) {
        for (vec = 0x60; vec <= 0x80; vec++)
            if (pd_found(vec)) break;
        if (vec > 0x80) {
            fprintf(stderr, "sbdos: no packet driver found (INT 60h-80h); load one first\n");
            return -1;
        }
    } else if (!pd_found(vec)) {
        fprintf(stderr, "sbdos: no packet driver at INT %02Xh\n", vec);
        return -1;
    }
    pd_vec = vec;

    segread(&s);
    r.x.ax = 0x01FF;                        /* driver_info */
    r.x.bx = 0;
    if (pd_call(&r, &s) != 0) {
        fprintf(stderr, "sbdos: packet driver: driver_info failed\n");
        return -1;
    }
    pd_ver = r.x.bx;
    pd_class = r.h.ch;
    pd_type = r.x.dx;
    pd_func = r.h.al;
    nm = (char far *)MK_FP(s.ds, r.x.si);
    for (i = 0; i < (int)sizeof(pd_name) - 1 && nm[i]; i++)
        pd_name[i] = nm[i];
    pd_name[i] = '\0';

    if (pd_class != 1) {
        fprintf(stderr, "sbdos: packet driver %s is class %d; SBDOS speaks class 1 "
                "(Ethernet) so far -- token ring (class 3) is next\n",
                pd_name, pd_class);
        return -1;
    }

    {                               /* the ring: only while net runs */
        unsigned seg;
        /* in paragraphs, computed in long: 16 x 2048 = 32768 overflows
         * a 16-bit int (and asked DOS for ~1 MB) */
        if (_dos_allocmem((unsigned)(((long)RX_SLOTS * RX_SLOT) >> 4), &seg) != 0) {
            fprintf(stderr, "sbdos: cannot allocate 32 KB for the receive ring\n");
            return -1;
        }
        rxq.head = rxq.tail = 0;
        rxq.bseg = seg;
    }
    rx_drops_last = rxq.drops;
    rx_frames_last = rxq.frames;
    h_ip = pd_access(0x0800);
    h_arp = pd_access(0x0806);
    if (h_ip < 0 || h_arp < 0) {
        fprintf(stderr, "sbdos: packet driver refused IP/ARP access (in use by another program?)\n");
        pn_close();
        return -1;
    }

    segread(&s);
    r.h.ah = 6;                             /* get_address */
    r.x.bx = (unsigned)h_ip;
    s.es = FP_SEG(my_mac);
    r.x.di = FP_OFF(my_mac);
    r.x.cx = 6;
    if (pd_call(&r, &s) != 0) {
        fprintf(stderr, "sbdos: packet driver: get_address failed\n");
        pn_close();
        return -1;
    }
    return 0;
}

void pn_close()
{
    unsigned seg;
    pd_release(h_ip);
    pd_release(h_arp);
    h_ip = h_arp = -1;
    seg = rxq.bseg;                 /* only now can no frame arrive */
    rxq.bseg = 0;
    if (seg) _dos_freemem(seg);
}

void pn_config(ip, mask, gw, mtu, lport)
    unsigned char *ip, *mask, *gw; int mtu; unsigned lport;
{
    memcpy(my_ip, ip, 4);
    memcpy(my_mask, mask, 4);
    memcpy(my_gw, gw, 4);
    my_mtu = mtu > 1500 ? 1500 : (mtu < 576 ? 576 : mtu);
    my_port = lport;
}

int pn_mtu() { return my_mtu; }

char *pn_desc()
{
    static char b[160];
    sprintf(b, "%s (packet driver v%u, INT %02Xh), class %d Ethernet, "
            "MAC %02X:%02X:%02X:%02X:%02X:%02X, IP %u.%u.%u.%u",
            pd_name, pd_ver, pd_vec, pd_class,
            my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5],
            my_ip[0], my_ip[1], my_ip[2], my_ip[3]);
    return b;
}

/* Counters: the ring's are 16-bit and wrap; fold them in on every poll. */
static void count_rx()
{
    unsigned d = rxq.drops, f = rxq.frames;
    rx_drops_total += (unsigned)(d - rx_drops_last);
    rx_frames_total += (unsigned)(f - rx_frames_last);
    rx_drops_last = d;
    rx_frames_last = f;
}
unsigned long pn_rx_frames() { count_rx(); return rx_frames_total; }
unsigned long pn_rx_drops()  { count_rx(); return rx_drops_total; }

/* --- link layer (the one place that knows it is Ethernet) --------------- */
static void link_hdr(f, dmac, type)
    unsigned char *f, *dmac; unsigned type;
{
    memcpy(f, dmac, 6);
    memcpy(f + 6, my_mac, 6);
    f[12] = (unsigned char)(type >> 8);
    f[13] = (unsigned char)type;
}

static int link_type(f, n)
    unsigned char *f; unsigned n;
{
    if (n < LHL) return -1;
    return (f[12] << 8) | f[13];
}

/* --- ARP ------------------------------------------------------------------ */
static void arp_send(op, tmac, tip)
    int op; unsigned char *tmac, *tip;
{
    static unsigned char bcast[6] = { 255, 255, 255, 255, 255, 255 };
    unsigned char *a = arpbuf + LHL;

    memset(arpbuf, 0, sizeof(arpbuf));
    link_hdr(arpbuf, op == 1 ? bcast : tmac, 0x0806);
    a[0] = 0; a[1] = 1;                     /* Ethernet */
    a[2] = 0x08; a[3] = 0x00;               /* IPv4 */
    a[4] = 6; a[5] = 4;
    a[6] = 0; a[7] = (unsigned char)op;
    memcpy(a + 8, my_mac, 6);
    memcpy(a + 14, my_ip, 4);
    if (op == 2) memcpy(a + 18, tmac, 6);
    memcpy(a + 24, tip, 4);
    (void)pd_send(arpbuf, 60);              /* minimum frame, padded */
}

static void arp_in(a, n)
    unsigned char *a; unsigned n;
{
    if (n < 28 || a[2] != 0x08 || a[3] != 0x00 || a[4] != 6 || a[5] != 4)
        return;
    if (memcmp(a + 14, hop_ip, 4) == 0) {   /* learn the next hop */
        memcpy(hop_mac, a + 8, 6);
        hop_known = 1;
    }
    if (a[7] == 1 && memcmp(a + 24, my_ip, 4) == 0)
        arp_send(2, a + 8, a + 14);         /* who-has us: answer */
}

/* --- IP / UDP ------------------------------------------------------------- */
static unsigned ip_sum(p, n)
    unsigned char *p; int n;
{
    unsigned long s = 0;
    int i;
    for (i = 0; i < n; i += 2)
        s += ((unsigned)p[i] << 8) | p[i + 1];
    while (s >> 16)
        s = (s & 0xFFFFL) + (s >> 16);
    return (unsigned)~s & 0xFFFF;
}

/* Build link + IP + UDP headers for `len` payload bytes in txbuf. */
static unsigned udp_hdrs(dport, len)
    unsigned dport, len;
{
    unsigned char *ip = txbuf + LHL, *u = ip + 20;
    unsigned tot = 20 + 8 + len, ck;

    link_hdr(txbuf, hop_mac, 0x0800);
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (unsigned char)(tot >> 8); ip[3] = (unsigned char)tot;
    ip[4] = (unsigned char)(ip_id >> 8); ip[5] = (unsigned char)ip_id;
    ip_id++;
    ip[6] = 0x40; ip[7] = 0;                /* DF: we never fragment */
    ip[8] = 64; ip[9] = 17;
    ip[10] = ip[11] = 0;
    memcpy(ip + 12, my_ip, 4);
    memcpy(ip + 16, dst_ip, 4);
    ck = ip_sum(ip, 20);
    ip[10] = (unsigned char)(ck >> 8); ip[11] = (unsigned char)ck;
    u[0] = (unsigned char)(my_port >> 8); u[1] = (unsigned char)my_port;
    u[2] = (unsigned char)(dport >> 8);   u[3] = (unsigned char)dport;
    u[4] = (unsigned char)((len + 8) >> 8); u[5] = (unsigned char)(len + 8);
    u[6] = u[7] = 0;                        /* no UDP checksum (legal) */
    tot += LHL;
    if (tot < 60) {
        memset(txbuf + tot, 0, 60 - tot);
        tot = 60;
    }
    return tot;
}

int pn_route(dst)
    unsigned char *dst;
{
    int i, tries;
    double t0;
    pn_dgram d;

    memcpy(dst_ip, dst, 4);
    for (i = 0; i < 4; i++)
        if ((dst[i] & my_mask[i]) != (my_ip[i] & my_mask[i])) break;
    memcpy(hop_ip, i < 4 ? my_gw : dst, 4);
    hop_known = 0;
    for (tries = 0; tries < 4 && !hop_known; tries++) {
        arp_send(1, (unsigned char *)0, hop_ip);
        t0 = now_s();
        while (!hop_known && now_s() - t0 < 1.0)
            (void)pn_poll(&d);
    }
    if (!hop_known) {
        fprintf(stderr, "sbdos: no ARP reply from %u.%u.%u.%u%s\n",
                hop_ip[0], hop_ip[1], hop_ip[2], hop_ip[3],
                i < 4 ? " (the gateway)" : "");
        return -1;
    }
    return 0;
}

int pn_send(dport, data, len)
    unsigned dport; char *data; unsigned len;
{
    unsigned tot;
    if (len > (unsigned)my_mtu - 28) len = (unsigned)my_mtu - 28;
    memcpy(txbuf + LHL + 28, data, len);
    tot = udp_hdrs(dport, len);
    blast_len = 0;                          /* txbuf no longer the blast */
    return pd_send(txbuf, tot);
}

void pn_blast_prep(dport, len)
    unsigned dport, len;
{
    unsigned i;
    unsigned char *p = txbuf + LHL + 28;
    if (len > (unsigned)my_mtu - 28) len = (unsigned)my_mtu - 28;
    for (i = 0; i < len; i++)
        p[i] = (unsigned char)(i * 131 + 17);   /* netd's pattern */
    blast_len = udp_hdrs(dport, len);
}

int pn_blast()
{
    unsigned char *ip = txbuf + LHL;
    unsigned ck;
    if (blast_len == 0) return -1;
    ip[4] = (unsigned char)(ip_id >> 8); ip[5] = (unsigned char)ip_id;
    ip_id++;
    ip[10] = ip[11] = 0;
    ck = ip_sum(ip, 20);
    ip[10] = (unsigned char)(ck >> 8); ip[11] = (unsigned char)ck;
    return pd_send(txbuf, blast_len);
}

/* One received frame: 1 if it was a UDP datagram for us (copied to *d). */
static int frame_in(f, n, d)
    unsigned char *f; unsigned n; pn_dgram *d;
{
    unsigned char *ip, *u;
    unsigned ihl, tot, ulen, c;
    int type = link_type(f, n);

    if (type == 0x0806) {
        arp_in(f + LHL, n - LHL);
        return 0;
    }
    if (type != 0x0800 || n < LHL + 28)
        return 0;
    ip = f + LHL;
    if ((ip[0] >> 4) != 4 || ip[9] != 17 || memcmp(ip + 16, my_ip, 4) != 0)
        return 0;
    if ((ip[6] & 0x3F) != 0 || ip[7] != 0)
        return 0;                           /* a fragment: not ours */
    ihl = (ip[0] & 15) * 4;
    tot = (ip[2] << 8) | ip[3];
    if (tot + LHL > n || tot < ihl + 8)
        return 0;
    u = ip + ihl;
    if ((((unsigned)u[2] << 8) | u[3]) != my_port)
        return 0;
    ulen = (((unsigned)u[4] << 8) | u[5]);
    if (ulen < 8 || ulen > tot - ihl)
        return 0;
    ulen -= 8;
    memcpy(d->src, ip + 12, 4);
    d->sport = ((unsigned)u[0] << 8) | u[1];
    d->len = ulen;
    c = ulen < PN_CAP ? ulen : PN_CAP;
    memcpy(d->data, u + 8, c);
    d->data[c] = '\0';
    return 1;
}

int pn_poll(d)
    pn_dgram *d;
{
    unsigned t;
    int r;
    for (;;) {
        t = rxq.tail;
        if (t == rxq.head)
            return 0;
        r = frame_in((unsigned char *)MK_FP(rxq.bseg, t * (unsigned)RX_SLOT),
                     rxq.len[t], d);
        rxq.tail = (t + 1) & (RX_SLOTS - 1);
        if (r)
            return 1;
    }
}

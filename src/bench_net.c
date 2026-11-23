/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * bench_net.c -- network benchmark client.  See bench_net.h / netproto.h.
 *
 * Timing uses sb_gtod() (gettimeofday) like the CPU and memory modules.
 * UDP sending treats ENOBUFS as backpressure from the interface queue (the
 * normal case on a slow link like PPP): back off briefly and continue, so
 * "loss" reflects the network, not the local queue.
 */
#include "port.h"
#include "bench_net.h"
#include "timer.h"

#ifdef HAVE_SOCKETS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#if defined(sun) || defined(__sun)
#include <sys/sockio.h>         /* SIOCGIFMTU/SIOCGIFCONF on SunOS/Solaris;
                                 * without this the MTU probe silently
                                 * compiled out and fell back to 1500 */
#endif
#include <net/if.h>

/* Solaris returns SIOCGIFMTU's answer in ifr_metric and never defines the
 * BSD ifr_mtu alias; supply it so the MTU probe compiles in there too. */
#if defined(SIOCGIFMTU) && !defined(ifr_mtu) && defined(ifr_metric)
#define ifr_mtu ifr_metric
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

extern int errno;

static char nbuf[SBN_BUF];

/* Resolve `host` into *sa (port set separately).  0 ok, -1 fail. */
static int resolve(host, sa)
    char *host;
    struct sockaddr_in *sa;
{
    struct hostent *he;
    unsigned long a;

    memset((char *)sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    a = inet_addr(host);
    if (a != (unsigned long)-1) {
        sa->sin_addr.s_addr = a;
        return 0;
    }
    he = gethostbyname(host);
    if (he == (struct hostent *)0) {
        fprintf(stderr, "sunbench: cannot resolve %s\n", host);
        return -1;
    }
    memcpy((char *)&sa->sin_addr, he->h_addr, sizeof(sa->sin_addr));
    return 0;
}

/* --- whole-test deadline -----------------------------------------------
 * On 10Mbit half-duplex with broadcast collisions, TCP can back off into
 * a stall that blocks the client in connect/read/write INDEFINITELY (a
 * Solaris 2.6 SS20 froze mid-upload; SunOS 4/2.3/V240 runs showed the
 * milder forms: absurd one-off cells and "-" rows).  Every public test
 * runs under alarm(2*secs+30): if it fires we longjmp here, close the
 * test's sockets, and the caller retries ONCE before recording failure.
 * A collision storm now costs at most one marked cell, never a hang. */
static jmp_buf nt_dead;
static int nt_fd_a = -1;                 /* control/TCP socket */
static int nt_fd_b = -1;                 /* UDP data socket */

static void nt_alarm(sig)
    int sig;
{
    (void)sig;
    longjmp(nt_dead, 1);
}

static void nt_close()
{
    if (nt_fd_a >= 0) { (void)close(nt_fd_a); nt_fd_a = -1; }
    if (nt_fd_b >= 0) { (void)close(nt_fd_b); nt_fd_b = -1; }
}

static int tcp_connect(host, port)
    char *host;
    int port;
{
    struct sockaddr_in sa;
    int fd;

    if (resolve(host, &sa) != 0)
        return -1;
    sa.sin_port = htons((unsigned short)port);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("sunbench: socket");
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "sunbench: cannot connect to %s port %d\n",
                host, port);
        (void)close(fd);
        return -1;
    }
    return fd;
}

static int read_line(fd, line, len)
    int fd; char *line; int len;
{
    int n = 0;
    char ch;
    for (;;) {
        if (read(fd, &ch, 1) != 1) return 0;
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (n >= len - 1) return 0;
        line[n++] = ch;
    }
    line[n] = '\0';
    return 1;
}

static void send_str(fd, s)
    int fd; char *s;
{
    (void)write(fd, s, (unsigned)strlen(s));
}

/* Wait for the exact 3-byte "GO\n" handshake before payload flows. */
static int wait_go(fd)
    int fd;
{
    char go[3];
    int n = 0, k;
    while (n < 3) {
        k = (int)read(fd, go + n, (unsigned)(3 - n));
        if (k <= 0) return -1;
        n += k;
    }
    return strncmp(go, "GO\n", 3) == 0 ? 0 : -1;
}

static int sb_net_check_once(host, port)
    char *host;
    int port;
{
    int fd;
    char line[32];
    (void)signal(SIGPIPE, SIG_IGN);
    fd = tcp_connect(host, port);
    if (fd < 0)
        return -1;
    sprintf(line, "%s NOP\n", SBN_MAGIC); /* unknown cmd: server says ERR */
    send_str(fd, line);
    (void)close(fd);
    return 0;
}

static int sb_net_tcp_once(host, port, down, secs, chunk, mbps)
    char *host;
    int port, down, secs, chunk;
    double *mbps;
{
    int fd, k;
    double t0, el, bytes;
    char line[SBN_LINE];

    *mbps = 0.0;
    if (chunk < 1 || chunk > (int)sizeof(nbuf))
        chunk = (int)sizeof(nbuf);
    fd = tcp_connect(host, port);
    if (fd < 0) return -1;
    sprintf(line, "%s %s %d\n", SBN_MAGIC, down ? "DOWN" : "UP", secs);
    send_str(fd, line);
    if (wait_go(fd) != 0) { (void)close(fd); return -1; }

    if (down) {
        bytes = 0.0;
        t0 = 0.0;
        for (;;) {
            k = (int)read(fd, nbuf, (unsigned)chunk);
            if (k <= 0) break;
            if (t0 == 0.0) t0 = sb_gtod();
            bytes += (double)k;
        }
        el = t0 > 0.0 ? sb_gtod() - t0 : 0.0;
        if (el > 0.0) *mbps = bytes * 8.0 / el / 1.0e6;
    } else {
        double b, us;
        t0 = sb_gtod();
        while (sb_gtod() - t0 < (double)secs)
            if (write(fd, nbuf, (unsigned)chunk) <= 0)
                break;
        (void)shutdown(fd, 1);            /* our EOF stops the server clock */
        if (read_line(fd, line, (int)sizeof(line)) &&
            sscanf(line, "RES %lf %lf", &b, &us) == 2 && us > 0.0)
            *mbps = b * 8.0 / us;         /* bytes*8 / usecs == Mbit/s */
    }
    (void)close(fd);
    return *mbps > 0.0 ? 0 : -1;
}

static int sb_net_rtt_once(host, port, secs, ms)
    char *host;
    int port, secs;
    double *ms;
{
    int fd, n, on;
    double t0, el;
    char line[SBN_LINE], ch;

    *ms = 0.0;
    fd = tcp_connect(host, port);
    if (fd < 0) return -1;
#ifdef TCP_NODELAY
    on = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&on, sizeof(on));
#endif
    sprintf(line, "%s RTT\n", SBN_MAGIC);
    send_str(fd, line);
    if (wait_go(fd) != 0) { (void)close(fd); return -1; }

    ch = 'x';
    n = 0;
    t0 = sb_gtod();
    while (n < 1000) {
        if (write(fd, &ch, 1) != 1) break;
        if (read(fd, &ch, 1) != 1) break;
        n++;
        el = sb_gtod() - t0;
        if (el >= (double)secs && n >= 8)
            break;
    }
    el = sb_gtod() - t0;
    (void)close(fd);
    if (n <= 0) return -1;
    *ms = el / (double)n * 1000.0;
    return 0;
}

static int sb_net_udp_once(host, port, secs, size, mbps, loss_pct)
    char *host;
    int port, secs, size;
    double *mbps, *loss_pct;
{
    struct sockaddr_in sa;
    int fd, us, udpport, k;
    double t0, el, sent, spkts, rpkts, rbytes, rspan, win;
    char line[SBN_LINE];

    *mbps = 0.0;
    *loss_pct = 0.0;
    if (size < 16 || size > SBN_MAXDGRAM)
        return -1;
    fd = tcp_connect(host, port);
    if (fd < 0) return -1;
    sprintf(line, "%s UDP %d %d\n", SBN_MAGIC, secs, size);
    send_str(fd, line);
    if (!read_line(fd, line, (int)sizeof(line)) ||
        sscanf(line, "GO %d", &udpport) != 1) {
        (void)close(fd);
        return -1;
    }

    if (resolve(host, &sa) != 0) { (void)close(fd); return -1; }
    sa.sin_port = htons((unsigned short)udpport);
    us = socket(AF_INET, SOCK_DGRAM, 0);
    nt_fd_b = us;
    if (us < 0) { (void)close(fd); return -1; }

    sent = 0.0; spkts = 0.0;
    t0 = sb_gtod();
    while (sb_gtod() - t0 < (double)secs) {
        k = (int)sendto(us, nbuf, (unsigned)size, 0,
                        (struct sockaddr *)&sa, sizeof(sa));
        if (k == size) {
            spkts += 1.0;
            sent += (double)k;
        } else {
            /* ENOBUFS = the interface queue is full (normal on slow links):
             * that is backpressure, not loss.  Yield briefly and go on. */
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 2000;
            (void)select(0, (fd_set *)0, (fd_set *)0, (fd_set *)0, &tv);
        }
    }
    el = sb_gtod() - t0;
    (void)close(us);

    send_str(fd, "END\n");
    rpkts = 0.0; rbytes = 0.0; rspan = 0.0;
    if (read_line(fd, line, (int)sizeof(line)))
        (void)sscanf(line, "RES %lf %lf %lf", &rpkts, &rbytes, &rspan);
    (void)close(fd);

    if (spkts <= 0.0 || el <= 0.0)
        return -1;
    /* Rate over the server's arrival span when it reports one: a sender
     * faster than the wire is still draining when its own loop ends, so
     * bytes over `el` overstates.  n arrivals span n-1 gaps, hence the
     * (rpkts-1)/rpkts.  Old servers send no span: fall back to `el`. */
    win = el;
    if (rspan > 0.0 && rpkts >= 2.0 && rspan / 1.0e6 > el * 0.5) {
        win = rspan / 1.0e6;
        rbytes = rbytes * (rpkts - 1.0) / rpkts;
    }
    *mbps = rbytes * 8.0 / win / 1.0e6;
    *loss_pct = (spkts - rpkts) / spkts * 100.0;
    if (*loss_pct < 0.0) *loss_pct = 0.0;
    return 0;
}

static int sb_net_udp_down_once(host, port, secs, size, kbps, mbps, loss_pct)
    char *host;
    int port, secs, size, kbps;
    double *mbps, *loss_pct;
{
    struct sockaddr_in sa;
    int fd, us, salen, k, maxfd, got_res;
    double t0, el, rpkts, rbytes, spkts, sbytes, tfirst, tlast;
    fd_set rf;
    struct timeval tv;
    char line[SBN_LINE];

    *mbps = 0.0;
    *loss_pct = 0.0;
    if (size < 16 || size > SBN_MAXDGRAM || kbps <= 0)
        return -1;

    /* Our receiving socket first, so its port can go in the header. */
    us = socket(AF_INET, SOCK_DGRAM, 0);
    nt_fd_b = us;
    if (us < 0) return -1;
    memset((char *)&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = 0;
    if (bind(us, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        (void)close(us);
        return -1;
    }
    salen = (int)sizeof(sa);
    (void)getsockname(us, (struct sockaddr *)&sa, &salen);
    {
        int rb = 1 << 18;      /* as big a receive buffer as the OS allows */
        (void)setsockopt(us, SOL_SOCKET, SO_RCVBUF, (char *)&rb,
                         sizeof(rb));
    }

    fd = tcp_connect(host, port);
    if (fd < 0) { (void)close(us); return -1; }
    sprintf(line, "%s UDPD %d %d %d %d\n",
            SBN_MAGIC, secs, size, (int)ntohs(sa.sin_port), kbps);
    send_str(fd, line);
    if (!read_line(fd, line, (int)sizeof(line)) ||
        strncmp(line, "GO", 2) != 0) {
        (void)close(us); (void)close(fd);
        return -1;
    }

    /* Count datagrams until the server's RES arrives on control. */
    rpkts = 0.0; rbytes = 0.0;
    got_res = 0;
    spkts = 0.0; sbytes = 0.0;
    tfirst = 0.0; tlast = 0.0;
    t0 = sb_gtod();
    maxfd = (fd > us ? fd : us) + 1;
    while (!got_res) {
        FD_ZERO(&rf);
        FD_SET(us, &rf);
        FD_SET(fd, &rf);
        tv.tv_sec = secs * 2 + 15;        /* belt and braces */
        tv.tv_usec = 0;
        if (select(maxfd, &rf, (fd_set *)0, (fd_set *)0, &tv) <= 0)
            break;
        if (FD_ISSET(us, &rf)) {
            k = (int)recv(us, nbuf, sizeof(nbuf), 0);
            if (k > 0) {
                rpkts += 1.0; rbytes += (double)k;
                tlast = sb_now();
                if (tfirst == 0.0) tfirst = tlast;
            }
        }
        if (FD_ISSET(fd, &rf)) {
            if (read_line(fd, line, (int)sizeof(line)) &&
                sscanf(line, "RES %lf %lf", &spkts, &sbytes) == 2)
                got_res = 1;
            else
                break;
        }
    }
    for (;;) {                            /* drain stragglers for ~300ms */
        FD_ZERO(&rf);
        FD_SET(us, &rf);
        tv.tv_sec = 0;
        tv.tv_usec = 300000;
        if (select(us + 1, &rf, (fd_set *)0, (fd_set *)0, &tv) <= 0)
            break;
        k = (int)recv(us, nbuf, sizeof(nbuf), 0);
        if (k > 0) {
            rpkts += 1.0; rbytes += (double)k;
            tlast = sb_now();
            if (tfirst == 0.0) tfirst = tlast;
        }
    }
    el = sb_gtod() - t0;
    (void)close(us);
    (void)close(fd);

    if (!got_res || spkts <= 0.0 || el <= 0.0)
        return -1;
    /* Rate over OUR first-to-last arrival span.  When the pacing outruns
     * the link, the switch queues the surplus and keeps delivering after
     * the server stops, so bytes over the server's send window overstated
     * the rate (an LX "received" 9.76 Mbit/s of UDP payload on 10 Mbit
     * Ethernet, where ~9.57 is the ceiling).  n arrivals span n-1 gaps.
     * sb_now() is gethrtime on Solaris -- cheap enough per datagram. */
    if (rpkts >= 2.0 && tlast > tfirst && tlast - tfirst > (double)secs * 0.5) {
        el = tlast - tfirst;
        rbytes = rbytes * (rpkts - 1.0) / rpkts;
    } else if (el > (double)secs)
        el = (double)secs;                /* too few arrivals: send window */
    *mbps = rbytes * 8.0 / el / 1.0e6;
    *loss_pct = (spkts - rpkts) / spkts * 100.0;
    if (*loss_pct < 0.0) *loss_pct = 0.0;
    return 0;
}

/* --- local MTU on the route to the server ------------------------------ */

int sb_net_mtu(host)
    char *host;
{
#if defined(SIOCGIFMTU) && defined(ifr_mtu)
    struct sockaddr_in sa, loc;
    struct ifconf ifc;
    struct ifreq *ifr, req;
    char cbuf[2048];
    int us, len, n, i, mtu;

    if (resolve(host, &sa) != 0)
        return 0;
    sa.sin_port = htons(9);               /* discard; never actually sent */
    us = socket(AF_INET, SOCK_DGRAM, 0);
    if (us < 0) return 0;
    if (connect(us, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        (void)close(us);
        return 0;
    }
    len = (int)sizeof(loc);
    if (getsockname(us, (struct sockaddr *)&loc, &len) < 0) {
        (void)close(us);
        return 0;
    }

    ifc.ifc_len = (int)sizeof(cbuf);
    ifc.ifc_buf = cbuf;
    if (ioctl(us, SIOCGIFCONF, (char *)&ifc) < 0) {
        (void)close(us);
        return 0;
    }
    mtu = 0;
    n = ifc.ifc_len / (int)sizeof(struct ifreq);
    ifr = (struct ifreq *)cbuf;
    for (i = 0; i < n; i++) {
        struct sockaddr_in *ia = (struct sockaddr_in *)&ifr[i].ifr_addr;
        if (ia->sin_family == AF_INET &&
            ia->sin_addr.s_addr == loc.sin_addr.s_addr) {
            memcpy(req.ifr_name, ifr[i].ifr_name, sizeof(req.ifr_name));
            if (ioctl(us, SIOCGIFMTU, (char *)&req) >= 0)
                mtu = (int)req.ifr_mtu;
            break;
        }
    }
    (void)close(us);
    return mtu;
#else
    return 0;
#endif
}

/* --- public wrappers: deadline + one retry (see nt_dead above) --- */

int sb_net_check(host, port)
    char *host;
    int port;
{
    int nt_try, nt_r;
    for (nt_try = 0; nt_try < 1; nt_try++) {
        nt_fd_a = -1;                /* forget stale registrations: only */
        nt_fd_b = -1;                /* THIS attempt's sockets are ours   */
        if (setjmp(nt_dead)) {
            nt_r = -1;               /* deadline fired mid-attempt */
        } else {
            (void)signal(SIGALRM, nt_alarm);
            alarm((unsigned)(30));
            nt_r = sb_net_check_once(host, port);
        }
        alarm(0);
        nt_close();
        if (nt_r == 0)
            return 0;
    }
    return -1;
}

int sb_net_tcp(host, port, down, secs, chunk, mbps)
    char *host;
    int port, down, secs, chunk;
    double *mbps;
{
    int nt_try, nt_r;
    for (nt_try = 0; nt_try < 2; nt_try++) {
        nt_fd_a = -1;                /* forget stale registrations: only */
        nt_fd_b = -1;                /* THIS attempt's sockets are ours   */
        if (setjmp(nt_dead)) {
            nt_r = -1;               /* deadline fired mid-attempt */
        } else {
            (void)signal(SIGALRM, nt_alarm);
            alarm((unsigned)(secs * 2 + 30));
            nt_r = sb_net_tcp_once(host, port, down, secs, chunk, mbps);
        }
        alarm(0);
        nt_close();
        if (nt_r == 0)
            return 0;
    }
    return -1;
}

int sb_net_rtt(host, port, secs, ms)
    char *host;
    int port, secs;
    double *ms;
{
    int nt_try, nt_r;
    for (nt_try = 0; nt_try < 2; nt_try++) {
        nt_fd_a = -1;                /* forget stale registrations: only */
        nt_fd_b = -1;                /* THIS attempt's sockets are ours   */
        if (setjmp(nt_dead)) {
            nt_r = -1;               /* deadline fired mid-attempt */
        } else {
            (void)signal(SIGALRM, nt_alarm);
            alarm((unsigned)(secs * 2 + 30));
            nt_r = sb_net_rtt_once(host, port, secs, ms);
        }
        alarm(0);
        nt_close();
        if (nt_r == 0)
            return 0;
    }
    return -1;
}

int sb_net_udp(host, port, secs, size, mbps, loss_pct)
    char *host;
    int port, secs, size;
    double *mbps, *loss_pct;
{
    int nt_try, nt_r;
    for (nt_try = 0; nt_try < 2; nt_try++) {
        nt_fd_a = -1;                /* forget stale registrations: only */
        nt_fd_b = -1;                /* THIS attempt's sockets are ours   */
        if (setjmp(nt_dead)) {
            nt_r = -1;               /* deadline fired mid-attempt */
        } else {
            (void)signal(SIGALRM, nt_alarm);
            alarm((unsigned)(secs * 2 + 30));
            nt_r = sb_net_udp_once(host, port, secs, size, mbps, loss_pct);
        }
        alarm(0);
        nt_close();
        if (nt_r == 0)
            return 0;
    }
    return -1;
}

int sb_net_udp_down(host, port, secs, size, kbps, mbps, loss_pct)
    char *host;
    int port, secs, size, kbps;
    double *mbps, *loss_pct;
{
    int nt_try, nt_r;
    for (nt_try = 0; nt_try < 2; nt_try++) {
        nt_fd_a = -1;                /* forget stale registrations: only */
        nt_fd_b = -1;                /* THIS attempt's sockets are ours   */
        if (setjmp(nt_dead)) {
            nt_r = -1;               /* deadline fired mid-attempt */
        } else {
            (void)signal(SIGALRM, nt_alarm);
            alarm((unsigned)(secs * 2 + 30));
            nt_r = sb_net_udp_down_once(host, port, secs, size, kbps, mbps, loss_pct);
        }
        alarm(0);
        nt_close();
        if (nt_r == 0)
            return 0;
    }
    return -1;
}


#else  /* !HAVE_SOCKETS */

/* No BSD sockets (Minix 2 does TCP/IP through /dev/tcp + ioctls instead).
 * Every entry point fails cleanly; sb_net_check says why, once, so `net`
 * and `full --server` report the module unavailable rather than breaking
 * the build or the rest of the suite. */
#include <stdio.h>

int sb_net_check(host, port)
    char *host; int port;
{
    (void)host; (void)port;
    fprintf(stderr, "sunbench: built without BSD sockets; network tests unavailable on this system\n");
    return -1;
}

int sb_net_mtu(host)
    char *host;
{
    (void)host;
    return 0;
}

int sb_net_tcp(host, port, down, secs, chunk, mbps)
    char *host; int port, down, secs, chunk; double *mbps;
{
    (void)host; (void)port; (void)down; (void)secs; (void)chunk;
    *mbps = 0.0;
    return -1;
}

int sb_net_rtt(host, port, secs, ms)
    char *host; int port, secs; double *ms;
{
    (void)host; (void)port; (void)secs;
    *ms = 0.0;
    return -1;
}

int sb_net_udp(host, port, secs, size, mbps, loss_pct)
    char *host; int port, secs, size; double *mbps, *loss_pct;
{
    (void)host; (void)port; (void)secs; (void)size;
    *mbps = 0.0; *loss_pct = 0.0;
    return -1;
}

int sb_net_udp_down(host, port, secs, size, kbps, mbps, loss_pct)
    char *host; int port, secs, size, kbps; double *mbps, *loss_pct;
{
    (void)host; (void)port; (void)secs; (void)size; (void)kbps;
    *mbps = 0.0; *loss_pct = 0.0;
    return -1;
}
#endif /* HAVE_SOCKETS */

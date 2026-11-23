/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * netd.c -- sunbench network test server (sunbench-netd).
 *
 * Runs on a MODERN machine (so the server is never the bottleneck) and
 * serves one test at a time to `sunbench net` clients on the Sun boxes.
 * Iterative, single-process, no configuration, no state between tests.
 * See netproto.h for the wire protocol.
 *
 * Build (from the sunbench tree, after ./configure):
 *     make netd                        # -> ./sunbench-netd
 * On Solaris add:  make netd NETLIBS='-lsocket -lnsl'
 *
 * Run:  ./sunbench-netd [port]         # default port 5202
 *
 * Kept in the project's K&R+P_() dialect so the same source also builds on
 * the vintage boxes if ever needed.
 */
#include "port.h"
#include "netproto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static char buf[SBN_BUF];
static jmp_buf next_client;

static double dnow()
{
    struct timeval tv;
    (void)gettimeofday(&tv, (struct timezone *)0);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1.0e6;
}

/* A wedged or vanished client must never wedge the server. */
static void on_alarm(sig)
    int sig;
{
    (void)sig;
    longjmp(next_client, 1);
}

/* Read one \n-terminated line (CR stripped); 0 on EOF/overflow. */
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

/* --- the four tests ---------------------------------------------------- */

static void do_up(fd, secs)
    int fd; int secs;
{
    double t0, bytes, tstart;
    int k;
    char line[SBN_LINE];

    alarm((unsigned)(secs * 2 + 30));
    send_str(fd, "GO\n");
    bytes = 0.0;
    t0 = 0.0;
    tstart = dnow();
    for (;;) {
        k = (int)read(fd, buf, sizeof(buf));
        if (k <= 0) break;
        if (t0 == 0.0) t0 = dnow();      /* clock starts at first payload */
        bytes += (double)k;
        if (dnow() - tstart > (double)(secs * 2 + 25))
            break;                       /* trickling client: bail below alarm */
    }
    sprintf(line, "RES %.0f %.0f\n",
            bytes, t0 > 0.0 ? (dnow() - t0) * 1.0e6 : 0.0);
    send_str(fd, line);
}

static void do_down(fd, secs)
    int fd; int secs;
{
    double t0;
    alarm((unsigned)(secs * 2 + 30));
    send_str(fd, "GO\n");
    t0 = dnow();
    while (dnow() - t0 < (double)secs)
        if (write(fd, buf, sizeof(buf)) <= 0)
            return;
    /* EOF marks the end; the client stops its clock at close. */
}

static void do_rtt(fd)
    int fd;
{
    char ch;
    alarm(120);
    send_str(fd, "GO\n");
    while (read(fd, &ch, 1) == 1)
        if (write(fd, &ch, 1) != 1)
            break;
}

/* Read every datagram already queued on the (non-blocking) socket; returns
 * how many.  Batching matters at gigabit: a clock read or select() per
 * datagram costs enough that the server itself starts dropping small
 * packets, and the "loss" becomes the server's, not the client's. */
static int drain_udp(us, pkts, bytes)
    int us; double *pkts, *bytes;
{
    int k, n = 0;
    for (;;) {
        k = (int)recv(us, buf, sizeof(buf), 0);
        if (k <= 0)
            break;
        *pkts += 1.0;
        *bytes += (double)k;
        n++;
    }
    return n;
}

static void do_udp(fd, secs, size)
    int fd; int secs, size;
{
    struct sockaddr_in sa;
    int us, salen, maxfd;
    double pkts, bytes, tstart, tfirst, tlast;
    fd_set rf;
    struct timeval tv;
    char line[SBN_LINE];

    if (size < 16 || size > SBN_MAXDGRAM) {
        send_str(fd, "ERR size\n");
        return;
    }
    alarm((unsigned)(secs * 2 + 30));
    us = socket(AF_INET, SOCK_DGRAM, 0);
    if (us < 0) {
        send_str(fd, "ERR socket\n");
        return;
    }
    memset((char *)&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = 0;                      /* ephemeral: no stale packets */
    if (bind(us, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        send_str(fd, "ERR bind\n");
        (void)close(us);
        return;
    }
    salen = (int)sizeof(sa);
    (void)getsockname(us, (struct sockaddr *)&sa, &salen);
    {
        int rb = 1 << 20;                 /* big receive buffer: count, don't drop */
        (void)setsockopt(us, SOL_SOCKET, SO_RCVBUF, (char *)&rb, sizeof(rb));
    }
    (void)fcntl(us, F_SETFL, fcntl(us, F_GETFL, 0) | O_NONBLOCK);
    sprintf(line, "GO %d\n", (int)ntohs(sa.sin_port));
    send_str(fd, line);

    pkts = 0.0; bytes = 0.0;
    tfirst = 0.0; tlast = 0.0;
    tstart = dnow();
    maxfd = (fd > us ? fd : us) + 1;
    for (;;) {                            /* until END arrives on control */
        if (dnow() - tstart > (double)(secs * 2 + 25))
            break;
        FD_ZERO(&rf);
        FD_SET(us, &rf);
        FD_SET(fd, &rf);
        if (select(maxfd, &rf, (fd_set *)0, (fd_set *)0,
                   (struct timeval *)0) <= 0)
            break;
        if (FD_ISSET(us, &rf)) {
            if (tfirst == 0.0)
                tfirst = dnow();          /* woke for the first arrivals */
            if (drain_udp(us, &pkts, &bytes) > 0)
                tlast = dnow();           /* one clock read per batch */
        }
        if (FD_ISSET(fd, &rf))
            break;                        /* END (or EOF) on control */
    }
    for (;;) {                            /* drain stragglers for ~300ms */
        FD_ZERO(&rf);
        FD_SET(us, &rf);
        tv.tv_sec = 0;
        tv.tv_usec = 300000;
        if (select(us + 1, &rf, (fd_set *)0, (fd_set *)0, &tv) <= 0)
            break;
        if (tfirst == 0.0)
            tfirst = dnow();
        if (drain_udp(us, &pkts, &bytes) > 0)
            tlast = dnow();
    }
    (void)close(us);
    /* Third field: first-to-last arrival span (us).  A sender faster than
     * the wire is still draining its queues when its own clock stops, so
     * bytes over the SENDER's window overstates throughput (an Opteron
     * "achieved" 999 Mbit/s of payload on gigabit, where ~957 is the
     * ceiling).  Old clients ignore the extra field. */
    sprintf(line, "RES %.0f %.0f %.0f\n", pkts, bytes,
            tlast > tfirst ? (tlast - tfirst) * 1.0e6 : 0.0);
    send_str(fd, line);
}

/* Download direction: paced datagrams to the client's address.  Pacing to
 * the requested kbps keeps a fast server from measuring the switch queue
 * instead of the client's receive path. */
static void do_udpd(fd, secs, size, port, kbps)
    int fd; int secs, size, port, kbps;
{
    struct sockaddr_in peer;
    int us, plen;
    double t0, el, pkts, bytes, target_Bps;
    struct timeval tv;
    char line[SBN_LINE];

    if (kbps > 950000)
        kbps = 950000;                   /* never pace past gigabit physics */
    if (size < 16 || size > SBN_MAXDGRAM || port <= 0 || kbps <= 0) {
        send_str(fd, "ERR args\n");
        return;
    }
    alarm((unsigned)(secs * 2 + 30));
    plen = (int)sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) < 0) {
        send_str(fd, "ERR peer\n");
        return;
    }
    peer.sin_port = htons((unsigned short)port);
    us = socket(AF_INET, SOCK_DGRAM, 0);
    if (us < 0) {
        send_str(fd, "ERR socket\n");
        return;
    }
    send_str(fd, "GO\n");

    target_Bps = (double)kbps * 125.0;    /* kilobits/s -> bytes/s */
    pkts = 0.0; bytes = 0.0;
    t0 = dnow();
    for (;;) {
        el = dnow() - t0;
        if (el >= (double)secs)
            break;
        if (bytes > target_Bps * el) {    /* ahead of schedule: yield */
            tv.tv_sec = 0;
            tv.tv_usec = 2000;
            (void)select(0, (fd_set *)0, (fd_set *)0, (fd_set *)0, &tv);
            continue;
        }
        if (sendto(us, buf, (unsigned)size, 0,
                   (struct sockaddr *)&peer, sizeof(peer)) == size) {
            pkts += 1.0;
            bytes += (double)size;
        }
    }
    (void)close(us);
    sprintf(line, "RES %.0f %.0f\n", pkts, bytes);
    send_str(fd, line);
}

/* --- UDP control (SBN1 over datagrams, for clients with no TCP) --------
 * SBDOS on MS-DOS drives a packet driver with its own small IP/UDP code
 * and has no TCP, so the round-trip and UDP tests can also be run over
 * UDP alone: commands arrive as datagrams on the control port, replies
 * go back to the sender.  Datagrams can be lost, so every command carries
 * a client-chosen id and is safe to repeat: a repeated command for the
 * test just finished gets its result again.  See netproto.h. */

static int    uc = -1;                 /* UDP control socket */
static long   u_last_id = -1;          /* last finished UDP-control test */
static char   u_last_res[SBN_LINE];

static void u_reply(peer, s)
    struct sockaddr_in *peer; char *s;
{
    (void)sendto(uc, s, (unsigned)strlen(s), 0, (struct sockaddr *)peer,
                 sizeof(*peer));
}

static void u_finish(peer, id, res)
    struct sockaddr_in *peer; long id; char *res;
{
    u_last_id = id;
    strncpy(u_last_res, res, sizeof(u_last_res) - 1);
    u_last_res[sizeof(u_last_res) - 1] = '\0';
    u_reply(peer, u_last_res);
}

static int same_peer(a, b)
    struct sockaddr_in *a, *b;
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

/* UUP: the client blasts datagrams at a fresh ephemeral port; UEND on
 * the control port ends it; RES carries the same three numbers (and the
 * arrival span) as the TCP-controlled UDP test. */
static void u_up(peer, secs, size, id)
    struct sockaddr_in *peer; int secs, size; long id;
{
    struct sockaddr_in sa, from;
    int us, salen, flen, maxfd, k;
    double pkts, bytes, tstart, tfirst, tlast;
    fd_set rf;
    struct timeval tv;
    char line[SBN_LINE], go[SBN_LINE], cmd[16];
    long rid;

    us = socket(AF_INET, SOCK_DGRAM, 0);
    if (us < 0) { u_reply(peer, "ERR socket\n"); return; }
    memset((char *)&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = 0;
    if (bind(us, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        u_reply(peer, "ERR bind\n");
        (void)close(us);
        return;
    }
    salen = (int)sizeof(sa);
    (void)getsockname(us, (struct sockaddr *)&sa, &salen);
    {
        int rb = 1 << 20;
        (void)setsockopt(us, SOL_SOCKET, SO_RCVBUF, (char *)&rb, sizeof(rb));
    }
    (void)fcntl(us, F_SETFL, fcntl(us, F_GETFL, 0) | O_NONBLOCK);
    sprintf(go, "GO %ld %d\n", id, (int)ntohs(sa.sin_port));
    u_reply(peer, go);

    pkts = 0.0; bytes = 0.0; tfirst = 0.0; tlast = 0.0;
    tstart = dnow();
    maxfd = (uc > us ? uc : us) + 1;
    for (;;) {
        if (dnow() - tstart > (double)(secs * 2 + 25))
            break;                        /* client gone: give up */
        FD_ZERO(&rf);
        FD_SET(us, &rf);
        FD_SET(uc, &rf);
        tv.tv_sec = 1; tv.tv_usec = 0;
        if (select(maxfd, &rf, (fd_set *)0, (fd_set *)0, &tv) < 0)
            break;
        if (FD_ISSET(us, &rf)) {
            if (tfirst == 0.0) tfirst = dnow();
            if (drain_udp(us, &pkts, &bytes) > 0) tlast = dnow();
        }
        if (FD_ISSET(uc, &rf)) {
            flen = (int)sizeof(from);
            k = (int)recvfrom(uc, line, sizeof(line) - 1, 0,
                              (struct sockaddr *)&from, &flen);
            if (k <= 0 || !same_peer(&from, peer)) continue;
            line[k] = '\0';
            cmd[0] = '\0'; rid = -1;
            (void)sscanf(line, "SBN1 %15s", cmd);
            if (strcmp(cmd, "UEND") == 0 &&
                sscanf(line, "SBN1 UEND %ld", &rid) == 1 && rid == id)
                break;
            if (strcmp(cmd, "UUP") == 0)
                u_reply(peer, go);        /* our GO was lost: again */
        }
    }
    for (;;) {                            /* stragglers, ~300 ms */
        FD_ZERO(&rf);
        FD_SET(us, &rf);
        tv.tv_sec = 0; tv.tv_usec = 300000;
        if (select(us + 1, &rf, (fd_set *)0, (fd_set *)0, &tv) <= 0)
            break;
        if (tfirst == 0.0) tfirst = dnow();
        if (drain_udp(us, &pkts, &bytes) > 0) tlast = dnow();
    }
    (void)close(us);
    sprintf(line, "RES %ld %.0f %.0f %.0f\n", id, pkts, bytes,
            tlast > tfirst ? (tlast - tfirst) * 1.0e6 : 0.0);
    u_finish(peer, id, line);
}

/* UDN: paced datagrams to the client, sent FROM the control socket, so a
 * NAT or a minimal client stack sees replies from the port it sent to.
 * Payload is buf's pattern (first byte 17), never mistaken for "RES". */
static void u_down(peer, secs, size, port, kbps, id)
    struct sockaddr_in *peer; int secs, size, port, kbps; long id;
{
    struct sockaddr_in dst;
    double t0, el, pkts, bytes, target_Bps;
    struct timeval tv;
    char line[SBN_LINE];

    if (kbps > 950000) kbps = 950000;
    dst = *peer;
    if (port > 0) dst.sin_port = htons((unsigned short)port);
    sprintf(line, "GO %ld\n", id);
    u_reply(peer, line);
    target_Bps = (double)kbps * 125.0;
    pkts = 0.0; bytes = 0.0;
    t0 = dnow();
    for (;;) {
        el = dnow() - t0;
        if (el >= (double)secs) break;
        if (bytes > target_Bps * el) {
            tv.tv_sec = 0; tv.tv_usec = 2000;
            (void)select(0, (fd_set *)0, (fd_set *)0, (fd_set *)0, &tv);
            continue;
        }
        if (sendto(uc, buf, (unsigned)size, 0, (struct sockaddr *)&dst,
                   sizeof(dst)) == size) {
            pkts += 1.0;
            bytes += (double)size;
        }
    }
    tv.tv_sec = 0; tv.tv_usec = 100000;   /* let the tail land before RES */
    (void)select(0, (fd_set *)0, (fd_set *)0, (fd_set *)0, &tv);
    sprintf(line, "RES %ld %.0f %.0f\n", id, pkts, bytes);
    u_finish(peer, id, line);
}

/* One control datagram: answered at once, or a whole test run inline
 * (one test at a time, exactly like the TCP side). */
static void u_command(served)
    unsigned long *served;
{
    struct sockaddr_in peer;
    int k, plen, a1, a2, a3, a4;
    char line[SBN_LINE], cmd[16], out[SBN_LINE];
    long id;

    plen = (int)sizeof(peer);
    k = (int)recvfrom(uc, line, sizeof(line) - 1, 0,
                      (struct sockaddr *)&peer, &plen);
    if (k <= 0) return;
    line[k] = '\0';
    if (k > 0 && line[k - 1] == '\n') line[k - 1] = '\0';
    cmd[0] = '\0';
    if (sscanf(line, "SBN1 %15s", cmd) != 1) return;
    if (strcmp(cmd, "PING") == 0) {       /* round trip: echo at once */
        sprintf(out, "PONG%s\n", line + 9);
        u_reply(&peer, out);
        return;
    }
    a1 = a2 = a3 = a4 = 0; id = -1;
    if (strcmp(cmd, "UUP") == 0 &&
        sscanf(line, "SBN1 UUP %d %d %ld", &a1, &a2, &id) == 3) {
        if (id == u_last_id) { u_reply(&peer, u_last_res); return; }
        if (a1 <= 0 || a1 > 120 || a2 < 16 || a2 > SBN_MAXDGRAM) {
            u_reply(&peer, "ERR args\n");
            return;
        }
        (*served)++;
        printf("sunbench-netd: [%lu] %s (udp control from %s)\n", *served,
               line, inet_ntoa(peer.sin_addr));
        (void)fflush(stdout);
        u_up(&peer, a1, a2, id);
    } else if (strcmp(cmd, "UDN") == 0 &&
               sscanf(line, "SBN1 UDN %d %d %d %d %ld",
                      &a1, &a2, &a3, &a4, &id) == 5) {
        if (id == u_last_id) { u_reply(&peer, u_last_res); return; }
        if (a1 <= 0 || a1 > 120 || a2 < 16 || a2 > SBN_MAXDGRAM || a4 <= 0) {
            u_reply(&peer, "ERR args\n");
            return;
        }
        (*served)++;
        printf("sunbench-netd: [%lu] %s (udp control from %s)\n", *served,
               line, inet_ntoa(peer.sin_addr));
        (void)fflush(stdout);
        u_down(&peer, a1, a2, a3, a4, id);
    } else if ((strcmp(cmd, "UEND") == 0 || strcmp(cmd, "URES") == 0) &&
               sscanf(line + 10, "%ld", &id) == 1 && id == u_last_id)
        u_reply(&peer, u_last_res);       /* our RES was lost: again */
}

/* ----------------------------------------------------------------------- */

int main(argc, argv)
    int argc;
    char **argv;
{
    struct sockaddr_in sa;
    int ls, fd, port, on;
    char line[SBN_LINE], cmd[SBN_LINE], fmt[64];
    int a1, a2, a3, a4, nf;
    unsigned long served = 0;

    port = (argc > 1) ? atoi(argv[1]) : SBN_PORT;
    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGALRM, on_alarm);

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    on = 1;
    (void)setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
    memset((char *)&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(ls, 4) < 0) { perror("listen"); return 1; }
    uc = socket(AF_INET, SOCK_DGRAM, 0);
    if (uc >= 0 && bind(uc, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind (UDP control)");
        (void)close(uc);
        uc = -1;                          /* TCP clients still served */
    }
    if (uc >= 0) {
        int rb = 1 << 20;
        (void)setsockopt(uc, SOL_SOCKET, SO_RCVBUF, (char *)&rb, sizeof(rb));
    }
    printf("sunbench-netd: listening on TCP/UDP port %d (one test at a time)\n", port);
    (void)fflush(stdout);

    /* fill the DOWN-test payload with something incompressible-ish */
    {
        int i;
        for (i = 0; i < (int)sizeof(buf); i++)
            buf[i] = (char)(i * 131 + 17);
    }

    if (setjmp(next_client))
        printf("sunbench-netd: client timed out; next.\n");

    for (;;) {
        alarm(0);
        if (uc >= 0) {                    /* wait for TCP or UDP control */
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(ls, &rf);
            FD_SET(uc, &rf);
            if (select((ls > uc ? ls : uc) + 1, &rf, (fd_set *)0,
                       (fd_set *)0, (struct timeval *)0) <= 0)
                continue;
            if (FD_ISSET(uc, &rf))
                u_command(&served);
            if (!FD_ISSET(ls, &rf))
                continue;
        }
        fd = accept(ls, (struct sockaddr *)0, (int *)0);
        if (fd < 0)
            continue;
        alarm(30);                        /* header must arrive promptly */
        if (read_line(fd, line, (int)sizeof(line))) {
            a1 = a2 = a3 = a4 = 0;
            /* format composed at runtime: SBN_MAGIC pasted into a scanf
             * format is ANSI string concatenation, which K&R cc rejects */
            sprintf(fmt, "%s %%s %%d %%d %%d %%d", SBN_MAGIC);
            nf = sscanf(line, fmt, cmd, &a1, &a2, &a3, &a4);
            if (nf >= 1) {
                served++;
                printf("sunbench-netd: [%lu] %s\n", served, line);
                (void)fflush(stdout);
                if (strcmp(cmd, "UP") == 0 && a1 > 0 && a1 <= 120)
                    do_up(fd, a1);
                else if (strcmp(cmd, "DOWN") == 0 && a1 > 0 && a1 <= 120)
                    do_down(fd, a1);
                else if (strcmp(cmd, "RTT") == 0)
                    do_rtt(fd);
                else if (strcmp(cmd, "UDP") == 0 && a1 > 0 && a1 <= 120)
                    do_udp(fd, a1, a2);
                else if (strcmp(cmd, "UDPD") == 0 && a1 > 0 && a1 <= 120)
                    do_udpd(fd, a1, a2, a3, a4);
                else
                    send_str(fd, "ERR cmd\n");
            }
        }
        alarm(0);
        (void)close(fd);
    }
    /* NOTREACHED */
}

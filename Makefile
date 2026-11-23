# Copyright (C) 2026 thenetworkinglab
# SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE)
#
# sunbench Makefile -- portable make (SunOS 4 make .. Solaris 10 make).
#
# No GNU-make-isms.  Run ./configure first (creates src/config.h).
#
#   ./configure && make
#   CC=gcc ./configure && make      # on later Solaris with gcc installed
#   make install PREFIX=/opt/sunbench
#
# Keep VERSION in sync with SB_VERSION in src/version.h.
#
VERSION = 2026.11.22-4

CC      = cc
#
# MCPU: target instruction set.  Empty by default (safe on every compiler,
# including SunOS 4's bundled K&R cc, which does not understand -mcpu).
#
# For a *fleet* binary that must run on the OLDEST SPARC architecture you own
# -- sun4c (SPARCstation 1/IPC/IPX), which is SPARC V7 with NO hardware
# integer multiply/divide -- build with gcc and force V7 so no V8-only
# instructions (that would trap on sun4c) are emitted:
#
#     make CC=gcc MCPU=-mcpu=v7
#
# A V7 binary runs on sun4c, sun4m and sun4u alike.  ./configure prints this
# hint when your compiler accepts the flag.
#
MCPU    =
CFLAGS  = -O $(MCPU)
# Record the build target (compiler + arch flag) in the binary, so CPU
# results can be labelled -- a v7 build and a native build measure different
# things on the same machine.
CPPFLAGS = -DSB_CCNAME='"$(CC)"' -DSB_ARCH='"$(MCPU)"'
LDFLAGS =
#
# NETLIBS: extra libraries for the socket code.  Empty on SunOS 4 (sockets
# live in libc) and on modern systems; Solaris 2.x needs the classic pair:
#
#     make NETLIBS='-lsocket -lnsl'
#
# ./configure prints this hint when it detects the need.
#
NETLIBS =
LIBS    = -lm $(NETLIBS)

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
MANDIR  = $(PREFIX)/man/man1

SRC = src/timer.c src/sysinfo.c src/diskio.c src/histogram.c \
      src/progress.c src/tui.c src/keys.c src/bench_disk.c src/cpu.c \
      src/dhry_1.c src/dhry_2.c src/whet.c src/bench_mem.c src/bench_net.c src/svgchart.c src/html.c src/main.c
OBJ = src/timer.o src/sysinfo.o src/diskio.o src/histogram.o \
      src/progress.o src/tui.o src/keys.o src/bench_disk.o src/cpu.o \
      src/dhry_1.o src/dhry_2.o src/whet.o src/bench_mem.o src/bench_net.o src/svgchart.o src/html.o src/main.o

all: sunbench

sunbench: src/config.h $(OBJ)
	$(CC) $(LDFLAGS) -o sunbench $(OBJ) $(LIBS)

# Every object depends on the portability headers.
$(OBJ): src/port.h src/config.h src/version.h

# src/config.cflags: dialect flags found by ./configure (e.g. -std=gnu17
# where the compiler defaults to C23); read with backquotes because old
# makes (SunOS 4) have no include directive.
.c.o:
	$(CC) $(CFLAGS) `cat src/config.cflags 2>/dev/null` $(CPPFLAGS) -c $< -o $@

src/config.h:
	@echo "run ./configure first"; exit 1

install: sunbench
	-mkdir -p $(BINDIR)
	-mkdir -p $(MANDIR)
	cp sunbench $(BINDIR)/sunbench
	chmod 755 $(BINDIR)/sunbench
	cp sunbench.1 $(MANDIR)/sunbench.1
	chmod 644 $(MANDIR)/sunbench.1
	@echo "installed sunbench $(VERSION) in $(BINDIR)"

uninstall:
	rm -f $(BINDIR)/sunbench $(MANDIR)/sunbench.1

# SVR4 datastream package with a PREBUILT binary (Solaris 2.0-10).
# Build on the OLDEST Solaris you support: binaries run forward, not backward.
pkg: sunbench
	sh pkg/mkpkg.sh $(VERSION)

# Standalone Dhrystone 2.1 reference, linking the SAME kernel objects as
# sunbench, to cross-check the Dhrystone figure the `cpu` command reports.
# Build with the same CC/MCPU you build sunbench with, then run ./dhry-ref.
dhry-ref: src/config.h src/dhry_1.o src/dhry_2.o src/dhry_ref.o
	$(CC) $(LDFLAGS) -o dhry-ref src/dhry_1.o src/dhry_2.o src/dhry_ref.o $(LIBS)

# The same reference harness linked with sunbench's FULL object list in
# sunbench's exact link order (dhry_ref.o simply replaces main.o), so
# dhry_1.o/dhry_2.o land at (nearly) the same addresses as inside sunbench.
# On a direct-mapped-cache machine (microSPARC: 4KB I / 2KB D), comparing
# ./dhry-ref with ./dhry-ref-sb separates code-LAYOUT effects (conflict
# misses) from everything else: same source, same objects, different placement.
NOMAIN = src/timer.o src/sysinfo.o src/diskio.o src/histogram.o \
         src/progress.o src/tui.o src/keys.o src/bench_disk.o src/cpu.o \
         src/dhry_1.o src/dhry_2.o src/whet.o src/bench_mem.o src/bench_net.o src/svgchart.o src/html.o
dhry-ref-sb: src/config.h $(NOMAIN) src/dhry_ref.o
	$(CC) $(LDFLAGS) -o dhry-ref-sb $(NOMAIN) src/dhry_ref.o $(LIBS)

# Network test server: run this on a MODERN machine (Mac, Linux, anything
# with a C compiler) so the far end is never the bottleneck, then point the
# Sun at it:  sunbench net --server <host>.
netd: src/config.h src/netd.o
	$(CC) $(LDFLAGS) -o sunbench-netd src/netd.o $(NETLIBS)

# Chart injector: add SVG line charts to an ALREADY-generated HTML report
# (for reports collected before charts existed).  Runs on any machine.
htmlgraph: src/config.h src/htmlgraph.o src/svgchart.o
	$(CC) $(LDFLAGS) -o sunbench-htmlgraph src/htmlgraph.o src/svgchart.o -lm

# SBDOS.EXE: sunbench for MS-DOS -- disk, cpu, mem, HTML (16-bit, 8086 up), cross-
# compiled with Open Watcom v2.  See dos/build.sh for the WATCOM setting.
dos:
	sh dos/build.sh

# Source tarball for SunOS 4 / Solaris 1.x (bundled cc) and general use.
srctar:
	sh pkg/mksrctar.sh $(VERSION)

dist: srctar
	@echo "for the Solaris package, run 'make pkg' on a Solaris host"

clean:
	rm -f sunbench sunbench-netd sunbench-htmlgraph dhry-ref dhry-ref-sb $(OBJ) \
	      src/dhry_ref.o src/netd.o src/htmlgraph.o
	rm -rf pkgbuild pkginfo prototype

distclean: clean
	rm -f src/config.h src/config.cflags sunbench.dat *.pkg *-src.tar

.PHONY: all install uninstall pkg srctar dist clean distclean dos

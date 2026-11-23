#!/bin/sh
# Copyright (C) 2026 thenetworkinglab
# SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE)
#
# build.sh -- cross-compile SBDOS.EXE (16-bit DOS, 8086 code, runs on
# any PC) with Open Watcom v2.  Run from the sunbench top directory:
#
#     sh dos/build.sh
#
# WATCOM must point at an Open Watcom install; on the Mac the default is
# ~/opt/watcom (the armo64 host binaries are used on Apple Silicon).
#
WATCOM=${WATCOM:-$HOME/opt/watcom}
export WATCOM
case `uname -s`-`uname -m` in
    Darwin-arm64)  BIN=$WATCOM/armo64 ;;
    Darwin-*)      BIN=$WATCOM/bino64 ;;
    Linux-x86_64)  BIN=$WATCOM/binl64 ;;
    Linux-aarch64) BIN=$WATCOM/arml64 ;;
    *)             BIN=$WATCOM/binl ;;
esac
PATH=$BIN:$PATH
INCLUDE=$WATCOM/h
export PATH INCLUDE

# dhry_1.c and dhry_2.c stay separate compilations (no cross-file
# optimisation), as Dhrystone requires.
SRC="dos/sbdos.c dos/sbnet.c dos/pktnet.c rxup.o xms.o src/histogram.c
     src/cpu.c src/dhry_1.c src/dhry_2.c src/whet.c src/bench_mem.c
     src/html.c src/svgchart.c"

# -ml large model; -0 8086 code; -fpi inline x87 with the emulator
# linked, so no coprocessor is required; -zt512 puts every object over 512
# bytes in its own far segment, keeping the 64 KB DGROUP (near data and
# stack) for the small stuff; -k16384 a 16 KB stack.  dosport.h is
# force-included in place of src/port.h (see its comment).
# The packet driver's receive upcall is assembly (see its comment).
wasm -q -0 -ml -fo=rxup.o dos/rxup.asm || exit 1
wasm -q -0 -ml -fo=xms.o dos/xms.asm || exit 1
wcl -q -bt=dos -ml -0 -fpi -ox -zt512 -k16384 -fi=dos/dosport.h \
    -i=src -i=dos -fe=SBDOS.EXE $SRC || exit 1
rm -f sbdos.o sbnet.o pktnet.o rxup.o xms.o histogram.o cpu.o dhry_1.o \
      dhry_2.o whet.o bench_mem.o html.o svgchart.o
ls -l SBDOS.EXE

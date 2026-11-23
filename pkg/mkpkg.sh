#!/bin/sh
# Copyright (C) 2026 thenetworkinglab
# SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE)
#
# mkpkg.sh -- build an SVR4 datastream package containing a PREBUILT sunbench.
#
# Run this ON A SOLARIS BOX, after `make`, e.g.:
#     CC=gcc ./configure && make CC=gcc
#     sh pkg/mkpkg.sh 2026.11.22-4
#
# IMPORTANT: build on the OLDEST Solaris you intend to support.  Solaris keeps
# backward binary compatibility, so a binary built on 2.6 runs on 2.6, 7, 8, 9
# and 10 -- but a binary built on 10 will NOT run on 2.6.
#
# The result is a single file, installable with no compiler on the target:
#     pkgadd -d sunbench-<version>-sol-sparc.pkg
#
VERSION=${1:-2026.11.22-4}
PKG=sunbench
ARCH=`uname -p 2>/dev/null || echo sparc`
BUILD=./pkgbuild
OUT=${PKG}-${VERSION}-sol-${ARCH}.pkg
HERE=`pwd`

if [ ! -f ./sunbench ]; then
    echo "mkpkg: ./sunbench not built -- run make first" >&2
    exit 1
fi
if [ ! -f ./sunbench.1 ]; then
    echo "mkpkg: ./sunbench.1 missing" >&2
    exit 1
fi

rm -rf "$BUILD" pkginfo prototype
mkdir -p "$BUILD" || exit 1

# BASEDIR makes the package relocatable: pkgadd installs under /usr/local by
# default, which is already on PATH/MANPATH on most of these machines.
cat > pkginfo <<EOF
PKG=$PKG
NAME=sunbench - disk benchmark for vintage SPARC systems
ARCH=$ARCH
VERSION=$VERSION
CATEGORY=application
VENDOR=sunbench
BASEDIR=/usr/local
CLASSES=none
PSTAMP=`uname -n`-`date +%Y%m%d%H%M%S`
EOF

# Paths are relative so BASEDIR relocation works.
cat > prototype <<EOF
i pkginfo
d none bin 0755 root bin
f none bin/sunbench=sunbench 0755 root bin
d none man 0755 root bin
d none man/man1 0755 root bin
f none man/man1/sunbench.1=sunbench.1 0644 root bin
d none share 0755 root bin
d none share/sunbench 0755 root bin
f none share/sunbench/LICENSE=LICENSE 0644 root bin
f none share/sunbench/NOTICE=NOTICE 0644 root bin
EOF

pkgmk -o -r "$HERE" -d "$BUILD" -f prototype || {
    echo "mkpkg: pkgmk failed" >&2; exit 1; }
rm -f "$HERE/$OUT"
pkgtrans -s "$BUILD" "$HERE/$OUT" $PKG || {
    echo "mkpkg: pkgtrans failed" >&2; exit 1; }

rm -rf "$BUILD" pkginfo prototype
echo ""
echo "created $OUT"
echo "install with:   pkgadd -d `pwd`/$OUT"
echo "remove with:    pkgrm $PKG"
echo "relocate with:  pkgadd -a none -d ... (or answer the BASEDIR prompt)"

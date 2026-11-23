/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * diskio.h -- cache-bypass strategy + aligned buffers.
 *
 * Two methods, chosen at runtime by sb_choose_method():
 *
 *   SB_DIRECTIO  Solaris 2.6+.  directio() takes the file's data path off
 *                the page cache, so timings are honest REGARDLESS of RAM.
 *                On macOS, fcntl(F_NOCACHE) plays the same role.
 *                This is what saves your Ultra 60 from grinding through
 *                1.5GB just to defeat caching -- a ~512MB file is enough.
 *
 *   SB_SIZE      SunOS 4 / Solaris 2.5.  No directio, so we defeat the
 *                cache the old way: a test file larger than physical RAM.
 *                These are low-RAM boxes, so that file is small and fast.
 */
#ifndef SUNBENCH_DISKIO_H
#define SUNBENCH_DISKIO_H
#include "port.h"

#define SB_DIRECTIO 1
#define SB_SIZE     2

/* Best cache-bypass method actually available AND working at runtime,
 * probed on the filesystem that holds `path` (NOT /tmp). */
int sb_choose_method P_((char *path));
const char *sb_method_name P_((int method));

/* Open the scratch file.  Applies directio / O_DSYNC per method.
 * Returns an fd, or -1 (message already printed). */
int sb_open_write P_((char *path, int method));   /* create + truncate */
int sb_open_read  P_((char *path, int method));
int sb_open_update P_((char *path, int method));  /* existing file, no truncate */

/* Page-aligned buffer (directio demands device-block alignment; page
 * alignment satisfies every SPARC disk we care about). */
char *sb_alloc_aligned P_((unsigned long sz));

/* Purge the file's cached pages (msync MS_INVALIDATE); no-op if absent.
 * Used by SB_SIZE runs so timed tests start cold regardless of what the
 * prep write or previous test left in the page cache. */
void sb_cache_purge P_((char *path, unsigned long filesz));

#endif

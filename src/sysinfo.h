/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/* sysinfo.h -- discover the machine, mostly to auto-size the test file. */
#ifndef SUNBENCH_SYSINFO_H
#define SUNBENCH_SYSINFO_H
#include "port.h"
#include <stdio.h>

/* Physical RAM in megabytes, or 0 if we can't tell (e.g. SunOS 4). */
unsigned long sb_physmem_mb P_((void));

/* Free space (MB) on the filesystem that holds `path`, or 0 if unknown.
 * `path` need not exist yet; its containing directory is used. */
unsigned long sb_freespace_mb P_((char *path));

/* 1 if `path` lives on a memory-backed filesystem (tmpfs/swap) -- i.e.
 * benchmarking it would measure RAM, not a disk.  0 otherwise/unknown. */
int sb_fs_is_memory P_((char *path));

/* Copy the filesystem type name for `path` into buf (e.g. "ufs","tmpfs").
 * buf is always NUL-terminated; empty string if unknown. */
void sb_fs_type P_((char *path, char *buf, int buflen));

/* Total capacity (MB) of the filesystem holding `path`, or 0 if unknown. */
unsigned long sb_fs_total_mb P_((char *path));

/* Disk device backing `path` (e.g. "/dev/dsk/c0t3d0s7") into buf; empty if
 * unknown (needs mnttab -- Solaris). */
void sb_fs_device P_((char *path, char *buf, int buflen));

/* SCSI drive model "VENDOR PRODUCT (rev R)" for a cNtNdN device, via
 * `iostat -En`; empty if unavailable (needs popen -- Solaris). */
void sb_disk_model P_((char *device, char *buf, int buflen));

/* CPU clock in MHz via `psrinfo -v`, or 0 if unknown (needs popen). */
unsigned long sb_cpu_mhz P_((void));

/* Write a multi-line system synopsis (uname/model/RAM/disk) to `out`, and
 * identically to `tee` if non-NULL (probed once, so both get the same text). */
void sb_write_sysinfo P_((FILE *out, FILE *tee, char *path));

/* The same synopsis as an array of newline-free lines, for on-screen headers
 * (the live TUI paints them so a screenshot is self-describing).  Each line
 * begins with a two-space "  label : " prefix, exactly as sb_write_sysinfo.
 * Fills up to `maxlines` rows of `lines` and returns the count.  Lines about
 * the scratch disk begin "  disk"/"  drive"; callers not benchmarking disk
 * (the CPU view) skip those. */
#define SB_SYSLINE  200      /* max chars per synopsis line (+NUL) */
#define SB_SYSLINES 8   /* system, model, cpu, memory, disk, drive, net (+slack) */
int sb_sysinfo_lines P_((char *path, char (*lines)[SB_SYSLINE], int maxlines));

/* Individual synopsis fields, for CSV columns (buf NUL-terminated, "" if
 * unknown). host = uname nodename; os = "sysname release"; model = the
 * SI_PLATFORM string (Solaris). */
void sb_host  P_((char *buf, int buflen));
void sb_os    P_((char *buf, int buflen));
void sb_model P_((char *buf, int buflen));

/* One-line CPU description ("2 x Quad-Core AMD Opteron(tm) Processor 2376",
 * "UltraSPARC-IIIi") from psrinfo -pv; empty where unavailable. */
void sb_cpu_desc P_((char *buf, int buflen));

#endif

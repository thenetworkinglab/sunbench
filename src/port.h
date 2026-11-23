/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * port.h -- portability foundation for sunbench.
 *
 * The single most important file in the project: everything else includes
 * this first.  Its job is to let the SAME source compile under:
 *
 *   - SunOS 4.x bundled /usr/bin/cc  (K&R: no prototypes, no const/volatile)
 *   - gcc / SunPro cc on Solaris 2-10 (ANSI C89)
 *
 * Rules for all .c files in this project (so we don't regress K&R support):
 *   - Use K&R-style function DEFINITIONS (args listed, then declared).
 *   - Declare prototypes with the P_((...)) macro.
 *   - Old C comments only:  / * ... * /   never //
 *   - Declare every local at the TOP of its block (no mixed decl/code).
 *   - No `long long` except behind HAVE_GETHRTIME.
 *   - No `void *` in public APIs; use `char *` and cast.
 */
#ifndef SUNBENCH_PORT_H
#define SUNBENCH_PORT_H

/*
 * MUST come before any system header.  On Solaris, gcc compiling in
 * standard mode (__STDC__==1) causes the headers to HIDE non-ANSI
 * functions -- gethrtime, directio, mkstemp, random, fsync -- unless
 * __EXTENSIONS__ is defined.  Without it, gethrtime() would be implicitly
 * declared as int and its 64-bit result truncated: silently wrong timings.
 * Harmless on SunOS 4 (its headers don't consult this macro).
 */
#ifndef __EXTENSIONS__
# define __EXTENSIONS__ 1
#endif

/* config.h is emitted by ./configure.  Always present after a build. */
#include "config.h"

/* Minix's headers declare POSIX (and Minix) interfaces only when asked.
 * Without these, popen/fileno/fsync/kill are implicit ints, and popen's
 * FILE * gets converted through int.  Like __EXTENSIONS__, this must
 * precede every system header, hence here. */
#ifdef HAVE_MINIX
# ifndef _MINIX
#  define _MINIX 1
# endif
# ifndef _POSIX_SOURCE
#  define _POSIX_SOURCE 1
# endif
#endif

/* SunOS 4 libc has no ANSI strerror(); sb_strerror (timer.c) maps errno
 * through the BSD sys_errlist table.  Callers just use strerror(). */
#ifndef HAVE_STRERROR
extern char *sb_strerror();
#define strerror sb_strerror
#endif

/*
 * P_((args)) -- prototype args on ANSI, empty on K&R.
 *   int f P_((int a, char *b));
 * Also neutralise const/volatile on K&R compilers that don't know them.
 */
#if defined(__STDC__) || defined(__cplusplus)
# define P_(args) args
#else
# define P_(args) ()
# define const
# define volatile
#endif

#endif /* SUNBENCH_PORT_H */

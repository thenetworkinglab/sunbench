/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * diskio.c -- the heart of honest disk numbers on Sun hardware.
 */
#include "port.h"
#include "diskio.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifdef HAVE_DIRECTIO
#include <sys/fcntl.h>   /* directio(), DIRECTIO_ON */
#endif

/* valloc() returns page-aligned memory on both BSD and SVR4.  Modern
 * <stdlib.h> declares it (void *valloc(size_t)); we do NOT redeclare it,
 * which would clash there.  On SunOS 4's K&R cc it may be undeclared, but
 * on that 32-bit target int and char* are the same width, so the implicit
 * declaration returns the pointer intact -- just a harmless warning. */
#ifdef HAVE_VALLOC
char *sb_alloc_aligned(sz)
    unsigned long sz;
{
    return valloc((unsigned int)sz);
}
#else
#include <stdlib.h>
/* No valloc (Minix): callers free() these buffers, so no over-allocate-
 * and-round trick.  Page alignment only matters for directio, which such
 * systems lack; malloc's natural alignment suffices for everything else. */
char *sb_alloc_aligned(sz)
    unsigned long sz;
{
    return (char *)malloc((unsigned int)sz);
}
#endif

/* Purge a file's pages from the page cache -- the honest complement to
 * the size>RAM method.  Prep-write residue survives the page scanner in
 * patterns no model predicts (a Solaris 2.3 SS2 kept ~84%% of an
 * early-file region resident through a 7-minute 256MB write), so
 * SB_SIZE runs purge before every timed test instead of guessing.
 * Two mechanisms, each used where present (directio systems never call
 * this; with neither, it is a silent no-op):
 *   - mmap+msync(MS_INVALIDATE) in chunks: SVR4/Solaris and SunOS 4.
 *   - posix_fadvise(POSIX_FADV_DONTNEED): Linux, which compiles msync
 *     MS_INVALIDATE but ignores it for file pages.  DONTNEED drops only
 *     CLEAN pages, so the prep write's dirty pages are flushed first --
 *     without the fsync most of a freshly written file stays cached. */
#if defined(HAVE_MSYNC_INV) || defined(HAVE_FADVISE)
#ifdef HAVE_MSYNC_INV
#include <sys/mman.h>
#endif

void sb_cache_purge(path, filesz)
    char *path;
    unsigned long filesz;
{
    int fd;
#ifdef HAVE_MSYNC_INV
    unsigned long off, chunk, len;
    char *m;
#endif

    fd = open(path, 0);                  /* O_RDONLY */
    if (fd < 0)
        return;
#ifdef HAVE_MSYNC_INV
    chunk = 8L * 1024L * 1024L;
    for (off = 0; off < filesz; off += chunk) {
        len = filesz - off;
        if (len > chunk) len = chunk;
        m = (char *)mmap((char *)0, len, PROT_READ, MAP_SHARED,
                         fd, (off_t)off);
        if (m == (char *)-1)
            break;
        (void)msync(m, len, MS_INVALIDATE);
        (void)munmap(m, len);
    }
#else
    (void)filesz;
#endif
#ifdef HAVE_FADVISE
#ifdef HAVE_FSYNC
    (void)fsync(fd);
#else
    sync();
#endif
    (void)posix_fadvise(fd, (off_t)0, (off_t)0, POSIX_FADV_DONTNEED);
#endif
    (void)close(fd);
}

#else

void sb_cache_purge(path, filesz)
    char *path;
    unsigned long filesz;
{
}
#endif  /* HAVE_MSYNC_INV || HAVE_FADVISE */

const char *sb_method_name(method)
    int method;
{
#if !defined(HAVE_DIRECTIO) && defined(HAVE_F_NOCACHE)
    if (method == SB_DIRECTIO) return "F_NOCACHE (page-cache bypass)";
#endif
    if (method == SB_DIRECTIO) return "directio (page-cache bypass)";
    return "size > RAM (cache defeated by file size)";
}

/*
 * sb_choose_method: prefer directio, but only if it actually works on the
 * filesystem that will hold OUR scratch file.  This must probe that same
 * filesystem -- NOT /tmp, which on Solaris is tmpfs (swap-backed RAM) where
 * directio always fails.  We create a throwaway file next to `path`, test
 * directio on it, then fall back to SB_SIZE.
 */
int sb_choose_method(path)
    char *path;
{
#ifdef HAVE_DIRECTIO
    char dir[1024];
    char tmpl[1040];
    char *slash;
    int fd, ok;

    /* Containing directory of the scratch path. */
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    slash = strrchr(dir, '/');
    if (slash == dir)      dir[1] = '\0';    /* "/foo" -> "/"  */
    else if (slash != 0)   *slash = '\0';    /* "a/b"  -> "a"  */
    else                   strcpy(dir, ".");  /* "foo"  -> "."  */

    sprintf(tmpl, "%s/.sbdioXXXXXX", dir);
    fd = mkstemp(tmpl);
    if (fd >= 0) {
        ok = directio(fd, DIRECTIO_ON);
        (void)close(fd);
        (void)unlink(tmpl);
        if (ok == 0)
            return SB_DIRECTIO;
    }
#else
#ifdef HAVE_F_NOCACHE
    /* macOS: fcntl(F_NOCACHE) keeps a file's data out of the unified
     * buffer cache -- its directio.  Neither posix_fadvise nor msync
     * invalidation works there, so without this a Mac serves the 256 MB
     * sweep from RAM (an M4 "read" 24 GB/s from a ~3 GB/s SSD).  Probed
     * on the scratch file's own filesystem, like directio. */
    char dir[1024];
    char tmpl[1040];
    char *slash;
    int fd, ok;

    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    slash = strrchr(dir, '/');
    if (slash == dir)      dir[1] = '\0';
    else if (slash != 0)   *slash = '\0';
    else                   strcpy(dir, ".");

    sprintf(tmpl, "%s/.sbdioXXXXXX", dir);
    fd = mkstemp(tmpl);
    if (fd >= 0) {
        ok = fcntl(fd, F_NOCACHE, 1);
        (void)close(fd);
        (void)unlink(tmpl);
        if (ok != -1)
            return SB_DIRECTIO;
    }
#endif
#endif
    return SB_SIZE;
}

static int open_common(path, oflag, method)
    char *path;
    int oflag;
    int method;
{
    int fd;

#ifdef HAVE_O_DSYNC
    /* Under SB_SIZE, force writes to platter so we don't just measure the
     * write-back cache.  Harmless (ignored) for read opens. */
    if (method == SB_SIZE && (oflag & O_WRONLY))
        oflag |= O_DSYNC;
#endif

    fd = open(path, oflag, 0644);
    if (fd < 0) {
        fprintf(stderr, "sunbench: open %s: %s\n", path, strerror(errno));
        return -1;
    }

#ifdef HAVE_DIRECTIO
    if (method == SB_DIRECTIO) {
        if (directio(fd, DIRECTIO_ON) != 0) {
            fprintf(stderr, "sunbench: directio %s: %s\n",
                    path, strerror(errno));
            (void)close(fd);
            return -1;
        }
    }
#else
#ifdef HAVE_F_NOCACHE
    if (method == SB_DIRECTIO) {
        if (fcntl(fd, F_NOCACHE, 1) == -1) {
            fprintf(stderr, "sunbench: F_NOCACHE %s: %s\n",
                    path, strerror(errno));
            (void)close(fd);
            return -1;
        }
    }
#endif
#endif
    return fd;
}

int sb_open_write(path, method)
    char *path;
    int method;
{
    return open_common(path, O_WRONLY | O_CREAT | O_TRUNC, method);
}

int sb_open_read(path, method)
    char *path;
    int method;
{
    return open_common(path, O_RDONLY, method);
}

int sb_open_update(path, method)
    char *path;
    int method;
{
    /* Overwrite blocks in place: no O_CREAT, no O_TRUNC. */
    return open_common(path, O_WRONLY, method);
}

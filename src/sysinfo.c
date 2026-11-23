/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * sysinfo.c -- physical memory detection.
 *
 * On Solaris this is a clean sysconf() call.  SunOS 4 has no portable
 * userland way to read physmem without groveling /dev/kmem, so we return
 * 0 there and the caller asks the user for --size (their boxes are tiny
 * anyway, so a sensible default also works).
 */
#include "port.h"
#include "sysinfo.h"

#ifdef HAVE_SC_PHYS_PAGES
#include <unistd.h>

unsigned long sb_physmem_mb()
{
    long pages;
    long pgsz;

    pages = sysconf(_SC_PHYS_PAGES);
    pgsz  = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || pgsz <= 0)
        return 0;
    /* pages * pgsz can overflow 32 bits at 4GB; divide first. */
    return (unsigned long)pages / (1024L * 1024L / (unsigned long)pgsz);
}

#else
#ifdef HAVE_SYSCTL_HW_PHYSMEM
#include <sys/param.h>
#include <sys/sysctl.h>

/* 4.4BSD lineage (NetBSD etc.): HW_PHYSMEM reports bytes in a C int, so
 * this undercounts past 2GB -- not a concern on the machines that lack
 * _SC_PHYS_PAGES in the first place. */
unsigned long sb_physmem_mb()
{
    int mib[2];
    unsigned long bytes;
    size_t len;

    mib[0] = CTL_HW;
    mib[1] = HW_PHYSMEM;
    bytes = 0;
    len = sizeof(bytes);
    if (sysctl(mib, 2, (char *)&bytes, &len, (char *)0, 0) < 0)
        return 0;
    return bytes / (1024L * 1024L);
}

#else

unsigned long sb_physmem_mb()
{
    return 0;
}
#endif
#endif


/* --------------- Linux /proc and /sys fallbacks (no probes) ---------------
 * Plain file reads: on systems without these files every helper leaves its
 * output empty, so the header simply omits the line, as before. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* Copy src into dst (size n), squeezing runs of blanks to one -- Intel
 * brand strings arrive space-padded. */
static void squeeze(dst, src, n)
    char *dst; char *src; int n;
{
    int k = 0, sp = 0;
    while (*src == ' ' || *src == '\t' || *src == '\n') src++;
    for (; *src && k < n - 1; src++) {
        if (*src == ' ' || *src == '\t' || *src == '\n') { sp = 1; continue; }
        if (sp && k > 0 && k < n - 2) dst[k++] = ' ';
        sp = 0;
        dst[k++] = *src;
    }
    dst[k] = '\0';
}

/* First line (or NUL-terminated string) of a small file, squeezed. */
static int read_small(fname, buf, n)
    char *fname; char *buf; int n;
{
    FILE *f;
    char raw[256];
    int k;

    buf[0] = '\0';
    f = fopen(fname, "r");
    if (f == (FILE *)0)
        return 0;
    k = (int)fread(raw, 1, sizeof(raw) - 1, f);
    (void)fclose(f);
    if (k <= 0)
        return 0;
    raw[k] = '\0';
    raw[strcspn(raw, "\n")] = '\0';           /* device-tree strings end in NUL */
    squeeze(buf, raw, n);
    return buf[0] != '\0';
}

#ifdef HAVE_SYSCTLBYNAME
/* ------------- macOS / BSD: sysctlbyname fallbacks ------------------- */
#include <sys/types.h>
#include <sys/sysctl.h>

static int sctl_str(name, buf, n)
    char *name; char *buf; int n;
{
    size_t len;

    buf[0] = '\0';
    len = (size_t)(n - 1);
    if (sysctlbyname(name, buf, &len, (char *)0, (size_t)0) != 0)
        return 0;
    buf[len < (size_t)n ? len : (size_t)(n - 1)] = '\0';
    return buf[0] != '\0';
}

/* Integer sysctl of either width (hw.cpufrequency is 64-bit). */
static long sctl_long(name)
    char *name;
{
    char v[16];
    size_t len;
    long l;
    int i;

    len = sizeof(v);
    if (sysctlbyname(name, v, &len, (char *)0, (size_t)0) != 0)
        return -1L;
    if (len == sizeof(long)) { memcpy((char *)&l, v, sizeof(long)); return l; }
    if (len == sizeof(int))  { memcpy((char *)&i, v, sizeof(int));  return (long)i; }
    return -1L;
}

static int is_darwin()
{
    char os[32];
    return sctl_str("kern.ostype", os, (int)sizeof(os)) &&
           strcmp(os, "Darwin") == 0;
}

/* "MacBook Air (Mac14,2)": system_profiler has the marketing name; the
 * hw.model identifier alone is the fallback.  (On FreeBSD hw.model is the
 * CPU brand, so this is Darwin-only.) */
static void mac_model(buf, n)
    char *buf; int n;
{
    static char cache[160];
    static int done = 0;
    char ident[64], name[96], line[256], *v;
#ifdef HAVE_POPEN
    FILE *p;
#endif

    if (done) {                     /* system_profiler takes ~0.5s */
        strncpy(buf, cache, n - 1);
        buf[n - 1] = '\0';
        return;
    }
    done = 1;
    cache[0] = '\0';
    buf[0] = '\0';
    if (!is_darwin() || !sctl_str("hw.model", ident, (int)sizeof(ident)))
        return;
    name[0] = '\0';
#ifdef HAVE_POPEN
    p = popen("/usr/sbin/system_profiler SPHardwareDataType 2>/dev/null", "r");
    if (p != (FILE *)0) {
        while (fgets(line, (int)sizeof(line), p) != (char *)0) {
            v = strstr(line, "Model Name:");
            if (v != (char *)0 && !name[0])
                squeeze(name, v + 11, (int)sizeof(name));
        }
        (void)pclose(p);
    }
#else
    (void)line; (void)v;
#endif
    if (name[0] && (int)(strlen(name) + strlen(ident)) + 4 < (int)sizeof(cache))
        (void)sprintf(cache, "%s (%s)", name, ident);
    else
        strcpy(cache, ident);
    strncpy(buf, cache, n - 1);
    buf[n - 1] = '\0';
}

/* "Apple M2 (4P + 4E cores)" on Apple Silicon, "2 x <brand>" on multi-
 * socket Intel Macs, "<brand> (N cores)" otherwise.  FreeBSD keeps the
 * brand in hw.model rather than machdep.cpu.brand_string. */
static void mac_cpu(buf, n)
    char *buf; int n;
{
    char brand[128];
    long pc, ec, cores, pkgs;

    buf[0] = '\0';
    if (!sctl_str("machdep.cpu.brand_string", brand, (int)sizeof(brand))) {
        if (is_darwin() || !sctl_str("hw.model", brand, (int)sizeof(brand)))
            return;
    }
    squeeze(brand, brand, (int)sizeof(brand));
    if ((int)strlen(brand) + 32 >= n)
        brand[n - 33 > 0 ? n - 33 : 0] = '\0';
    pc = sctl_long("hw.perflevel0.physicalcpu");
    ec = sctl_long("hw.perflevel1.physicalcpu");
    cores = sctl_long("hw.physicalcpu");
    if (cores <= 0) cores = sctl_long("hw.ncpu");
    pkgs = sctl_long("hw.packages");
    if (pc > 0 && ec > 0)
        (void)sprintf(buf, "%s (%ldP + %ldE cores)", brand, pc, ec);
    else if (pkgs > 1)
        (void)sprintf(buf, "%ld x %s", pkgs, brand);
    else if (cores > 1)
        (void)sprintf(buf, "%s (%ld cores)", brand, cores);
    else
        strcpy(buf, brand);
}

/* Intel Macs report hw.cpufrequency (Hz); FreeBSD dev.cpu.0.freq (MHz).
 * Apple Silicon reports neither -- the clock stays unknown there. */
static unsigned long mac_mhz()
{
    long hz, mhz;

    hz = sctl_long("hw.cpufrequency");
    if (hz > 0)
        return (unsigned long)(hz / 1000000L);
    mhz = sctl_long("dev.cpu.0.freq");
    return mhz > 0 ? (unsigned long)mhz : 0;
}
#endif /* HAVE_SYSCTLBYNAME */

/* Absolute directory holding `path` (relative paths resolve via cwd). */
static int abs_dir(path, abs, n)
    char *path; char *abs; int n;
{
    char dir[1024], *slash;

    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    slash = strrchr(dir, '/');
    if (slash == dir)      dir[1] = '\0';
    else if (slash != 0)   *slash = '\0';
    else                   dir[0] = '\0';
    if (dir[0] == '/') {
        strncpy(abs, dir, n - 1);
        abs[n - 1] = '\0';
        return 1;
    }
    if (getcwd(abs, n) == (char *)0)
        return 0;
    if (dir[0] != '\0') {
        strncat(abs, "/", n - strlen(abs) - 1);
        strncat(abs, dir, n - strlen(abs) - 1);
    }
    return 1;
}

/* Linux /proc/mounts: the longest mount point prefixing `path` gives the
 * backing device and filesystem type.  Either output may be null. */
static int proc_mounts(path, dev, devlen, type, typelen)
    char *path; char *dev; int devlen; char *type; int typelen;
{
    FILE *f;
    char abs[1024], line[1024], d[256], m[512], t[32];
    char bd[256], bt[32];
    int L, best;

    if (dev) dev[0] = '\0';
    if (type) type[0] = '\0';
    if (!abs_dir(path, abs, (int)sizeof(abs)))
        return 0;
    f = fopen("/proc/mounts", "r");
    if (f == (FILE *)0)
        return 0;
    best = -1;
    bd[0] = '\0'; bt[0] = '\0';
    while (fgets(line, (int)sizeof(line), f) != (char *)0) {
        if (sscanf(line, "%255s %511s %31s", d, m, t) != 3)
            continue;
        L = (int)strlen(m);
        if (strncmp(abs, m, L) == 0 &&
            (L == 1 || abs[L] == '/' || abs[L] == '\0') && L > best) {
            best = L;
            strcpy(bd, d);
            strcpy(bt, t);
        }
    }
    (void)fclose(f);
    if (best < 0)
        return 0;
    if (dev) { strncpy(dev, bd, devlen - 1); dev[devlen - 1] = '\0'; }
    if (type) { strncpy(type, bt, typelen - 1); type[typelen - 1] = '\0'; }
    return 1;
}

/* Board/system name: the device tree on ARM boards ("Raspberry Pi 4
 * Model B Rev 1.4"), DMI on PCs ("Dell Inc. OptiPlex 790"). */
static void linux_model(buf, n)
    char *buf; int n;
{
    char vendor[96], product[96];

    if (read_small("/proc/device-tree/model", buf, n))
        return;
    buf[0] = '\0';
    if (read_small("/sys/class/dmi/id/product_name", product,
                   (int)sizeof(product))) {
        if (read_small("/sys/class/dmi/id/sys_vendor", vendor,
                       (int)sizeof(vendor)) && (int)strlen(vendor) + 2 < n) {
            strcpy(buf, vendor);
            strcat(buf, " ");
        }
        strncat(buf, product, n - strlen(buf) - 1);
    }
#ifdef HAVE_SYSCTLBYNAME
    if (!buf[0])
        mac_model(buf, n);
#endif
}

/* ARM Ltd (implementer 0x41) core part numbers from /proc/cpuinfo.  The
 * 32-bit Raspberry Pi kernel names every core "ARMv7 Processor rev N",
 * so the part number is the only way to tell an A72 from an A53. */
static char *arm_part_name(part)
    long part;
{
    static long parts[] = { 0xb76, 0xc07, 0xc08, 0xc09, 0xc0f, 0xd03,
                            0xd04, 0xd05, 0xd07, 0xd08, 0xd09, 0xd0a,
                            0xd0b, 0xd0d, 0xd41, 0 };
    static char *names[] = { "ARM1176", "Cortex-A7", "Cortex-A8",
                             "Cortex-A9", "Cortex-A15", "Cortex-A53",
                             "Cortex-A35", "Cortex-A55", "Cortex-A57",
                             "Cortex-A72", "Cortex-A73", "Cortex-A75",
                             "Cortex-A76", "Cortex-A77", "Cortex-A78", 0 };
    int i;

    for (i = 0; parts[i]; i++)
        if (parts[i] == part)
            return names[i];
    return (char *)0;
}

/* One-line CPU description from /proc/cpuinfo, in the same "N x name"
 * shape psrinfo gives on Solaris: sockets for x86 (distinct physical id),
 * a core count for ARM system-on-chips. */
static void proc_cpu_desc(buf, n)
    char *buf; int n;
{
    FILE *f;
    char line[256], model[128], *v, *arm;
    long part, impl, id, maxid;
    int ncpu;

    buf[0] = '\0';
    f = fopen("/proc/cpuinfo", "r");
    if (f == (FILE *)0) {
#ifdef HAVE_SYSCTLBYNAME
        mac_cpu(buf, n);
#endif
        return;
    }
    model[0] = '\0';
    part = -1; impl = -1; maxid = -1; ncpu = 0;
    while (fgets(line, (int)sizeof(line), f) != (char *)0) {
        v = strchr(line, ':');
        if (v == (char *)0)
            continue;
        v++;
        if (strncmp(line, "processor", 9) == 0)
            ncpu++;
        else if (strncmp(line, "model name", 10) == 0 && !model[0])
            squeeze(model, v, (int)sizeof(model));
        else if (strncmp(line, "physical id", 11) == 0) {
            id = atol(v);
            if (id > maxid) maxid = id;
        } else if (strncmp(line, "CPU implementer", 15) == 0 && impl < 0)
            impl = strtol(v, (char **)0, 0);
        else if (strncmp(line, "CPU part", 8) == 0 && part < 0)
            part = strtol(v, (char **)0, 0);
    }
    (void)fclose(f);
    arm = (impl == 0x41) ? arm_part_name(part) : (char *)0;
    if (arm != (char *)0) {
        if (ncpu > 1 && (int)strlen(arm) + 24 < n)
            (void)sprintf(buf, "ARM %s (%d cores)", arm, ncpu);
        else if ((int)strlen(arm) + 5 < n)
            (void)sprintf(buf, "ARM %s", arm);
        return;
    }
    if (!model[0])
        return;
    if (maxid >= 1 && (int)strlen(model) + 8 < n)
        (void)sprintf(buf, "%ld x %s", maxid + 1, model);
    else {
        strncpy(buf, model, n - 1);
        buf[n - 1] = '\0';
    }
}

/* Rated clock in MHz: cpufreq's maximum (the rated speed, not the current
 * throttled one), else /proc/cpuinfo's "cpu MHz" (x86). */
static unsigned long linux_mhz()
{
    FILE *f;
    char line[256], *v;
    unsigned long mhz = 0;

    if (read_small("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
                   line, (int)sizeof(line)))
        return (unsigned long)atol(line) / 1000L;
    f = fopen("/proc/cpuinfo", "r");
    if (f == (FILE *)0) {
#ifdef HAVE_SYSCTLBYNAME
        return mac_mhz();
#else
        return 0;
#endif
    }
    while (fgets(line, (int)sizeof(line), f) != (char *)0) {
        if (strncmp(line, "cpu MHz", 7) == 0 &&
            (v = strchr(line, ':')) != (char *)0) {
            mhz = (unsigned long)atol(v + 1);
            break;
        }
    }
    (void)fclose(f);
    return mhz;
}

#if defined(HAVE_STATVFS) || defined(HAVE_STATFS_MOUNT) || defined(HAVE_STATFS_VFS)
#include <string.h>

/* statvfs/statfs need an existing path; reduce a file path to its dir. */
static void dirof(path, dir, len)
    char *path;
    char *dir;
    int len;
{
    char *slash;

    strncpy(dir, path, len - 1);
    dir[len - 1] = '\0';
    slash = strrchr(dir, '/');
    if (slash == dir)      dir[1] = '\0';    /* "/foo"  -> "/"  */
    else if (slash != 0)   *slash = '\0';    /* "a/b"   -> "a"  */
    else                   strcpy(dir, "."); /* "foo"   -> "."  */
}
#endif

#ifdef HAVE_STATVFS
#include <sys/statvfs.h>
#ifdef HAVE_STATFS_MOUNT
#include <sys/param.h>
#include <sys/mount.h>
#endif

unsigned long sb_freespace_mb(path)
    char *path;
{
    struct statvfs v;
    char dir[1024];
    double bytes;

    dirof(path, dir, (int)sizeof(dir));
    if (statvfs(dir, &v) != 0)
        return 0;
    bytes = (double)v.f_bavail * (double)v.f_frsize;
    return (unsigned long)(bytes / (1024.0 * 1024.0));
}

void sb_fs_type(path, buf, buflen)
    char *path;
    char *buf;
    int buflen;
{
    struct statvfs v;
    char dir[1024];

    buf[0] = '\0';
    dirof(path, dir, (int)sizeof(dir));
    if (statvfs(dir, &v) != 0)
        return;
#ifdef HAVE_F_BASETYPE
    /* f_basetype is the SVR4/Solaris fs-type name, e.g. "ufs","tmpfs". */
    strncpy(buf, v.f_basetype, buflen - 1);
    buf[buflen - 1] = '\0';
#else
    (void)v;
    (void)proc_mounts(path, (char *)0, 0, buf, buflen);   /* Linux */
#ifdef HAVE_STATFS_MOUNT
    if (!buf[0]) {                         /* macOS / BSD: "apfs", "ffs" */
        struct statfs sf;
        if (statfs(dir, &sf) == 0) {
            strncpy(buf, sf.f_fstypename, buflen - 1);
            buf[buflen - 1] = '\0';
        }
    }
#endif
#endif
}

int sb_fs_is_memory(path)
    char *path;
{
    char t[32];

    sb_fs_type(path, t, (int)sizeof(t));
    /* tmpfs is swap-backed RAM on Solaris; "swap" appears in df output. */
    return strcmp(t, "tmpfs") == 0 || strcmp(t, "swap") == 0;
}

unsigned long sb_fs_total_mb(path)
    char *path;
{
    struct statvfs v;
    char dir[1024];

    dirof(path, dir, (int)sizeof(dir));
    if (statvfs(dir, &v) != 0)
        return 0;
    return (unsigned long)((double)v.f_blocks * (double)v.f_frsize
                           / (1024.0 * 1024.0));
}

#else
#if defined(HAVE_STATFS_MOUNT) || defined(HAVE_STATFS_VFS)
/* Pre-statvfs systems: 4.4BSD (NetBSD) keeps statfs in <sys/mount.h>,
 * SunOS 4 in <sys/vfs.h>.  Same call, same fields we need. */
#ifdef HAVE_STATFS_MOUNT
#include <sys/param.h>
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif

unsigned long sb_freespace_mb(path)
    char *path;
{
    struct statfs v;
    char dir[1024];
    double bytes;

    dirof(path, dir, (int)sizeof(dir));
    if (statfs(dir, &v) != 0)
        return 0;
    if ((long)v.f_bavail < 0)
        return 0;   /* over-full fs: only reserved blocks remain */
    bytes = (double)v.f_bavail * (double)v.f_bsize;
    return (unsigned long)(bytes / (1024.0 * 1024.0));
}

void sb_fs_type(path, buf, buflen)
    char *path;
    char *buf;
    int buflen;
{
    struct statfs v;
    char dir[1024];

    buf[0] = '\0';
    dirof(path, dir, (int)sizeof(dir));
    if (statfs(dir, &v) != 0)
        return;
#ifdef HAVE_STATFS_MOUNT
    /* 4.4BSD statfs carries the fs-type name, e.g. "ffs","mfs". */
    strncpy(buf, v.f_fstypename, buflen - 1);
    buf[buflen - 1] = '\0';
#else
    (void)v;   /* SunOS 4: type name unavailable */
#endif
}

int sb_fs_is_memory(path)
    char *path;
{
    char t[32];

    sb_fs_type(path, t, (int)sizeof(t));
    /* mfs is the 4.4BSD memory filesystem; tmpfs/swap are the SVR4 ones. */
    return strcmp(t, "tmpfs") == 0 || strcmp(t, "swap") == 0
        || strcmp(t, "mfs") == 0;
}

unsigned long sb_fs_total_mb(path)
    char *path;
{
    struct statfs v;
    char dir[1024];

    dirof(path, dir, (int)sizeof(dir));
    if (statfs(dir, &v) != 0)
        return 0;
    return (unsigned long)((double)v.f_blocks * (double)v.f_bsize
                           / (1024.0 * 1024.0));
}

#else   /* neither statvfs nor statfs */

unsigned long sb_freespace_mb(path)
    char *path;
{
    return 0;   /* caller falls back to --size */
}

void sb_fs_type(path, buf, buflen)
    char *path; char *buf; int buflen;
{
    if (buflen > 0) buf[0] = '\0';
}

int sb_fs_is_memory(path)
    char *path;
{
    return 0;
}

unsigned long sb_fs_total_mb(path)
    char *path;
{
    return 0;
}
#endif  /* HAVE_STATFS_* */
#endif  /* HAVE_STATVFS */


/* =================== system synopsis (for the log) ==================== */

#include <sys/utsname.h>
#include <string.h>
#ifdef HAVE_SYSINFO
#include <sys/systeminfo.h>
#endif

void sb_host(buf, buflen)
    char *buf; int buflen;
{
    struct utsname u;
    if (buflen > 0) buf[0] = '\0';
    if (uname(&u) >= 0) {
        strncpy(buf, u.nodename, buflen - 1);
        buf[buflen - 1] = '\0';
    }
}

void sb_os(buf, buflen)
    char *buf; int buflen;
{
    struct utsname u;
    if (buflen > 0) buf[0] = '\0';
    if (uname(&u) >= 0) {
        strncpy(buf, u.sysname, buflen - 1);
        buf[buflen - 1] = '\0';
        strncat(buf, " ", buflen - strlen(buf) - 1);
        strncat(buf, u.release, buflen - strlen(buf) - 1);
    }
}

#if defined(HAVE_SYSINFO) && defined(HAVE_POPEN)
#include <string.h>
static void x86_system P_((char *buf, int buflen));
#endif

/* SI_PLATFORM names SPARC machines well ("SUNW,SPARCstation-20") but
 * every x86 box as "i86pc"; there prtdiag's first line carries the real
 * system name ("Sun Microsystems Sun Fire X2200 M2"). */
void sb_model(buf, buflen)
    char *buf; int buflen;
{
    if (buflen > 0) buf[0] = '\0';
#ifdef HAVE_SYSINFO
    (void)sysinfo(SI_PLATFORM, buf, (long)buflen);
#ifdef HAVE_POPEN
    if (strcmp(buf, "i86pc") == 0)
        x86_system(buf, buflen);
#endif
#else
    if (buflen > 0)
        linux_model(buf, buflen);
#endif
}

#ifdef HAVE_MNTTAB
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mnttab.h>

/* Longest mount point that prefixes `path` gives the backing device. */
void sb_fs_device(path, buf, buflen)
    char *path; char *buf; int buflen;
{
    FILE *f;
    struct mnttab m;
    char abs[1024], dir[1024], best[256];
    char *slash;
    int bestlen;

    if (buflen > 0) buf[0] = '\0';

    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    slash = strrchr(dir, '/');
    if (slash == dir)      dir[1] = '\0';
    else if (slash != 0)   *slash = '\0';
    else                   dir[0] = '\0';   /* relative -> cwd */

    if (dir[0] == '/') {
        strncpy(abs, dir, sizeof(abs) - 1);
        abs[sizeof(abs) - 1] = '\0';
    } else {
        if (getcwd(abs, (int)sizeof(abs)) == (char *)0)
            return;
        if (dir[0] != '\0') {
            strncat(abs, "/", sizeof(abs) - strlen(abs) - 1);
            strncat(abs, dir, sizeof(abs) - strlen(abs) - 1);
        }
    }

    f = fopen("/etc/mnttab", "r");
    if (f == (FILE *)0)
        return;
    bestlen = -1; best[0] = '\0';
    while (getmntent(f, &m) == 0) {
        int L = (int)strlen(m.mnt_mountp);
        if (strncmp(abs, m.mnt_mountp, L) == 0 &&
            (L == 1 || abs[L] == '/' || abs[L] == '\0')) {
            if (L > bestlen) {
                bestlen = L;
                strncpy(best, m.mnt_special, sizeof(best) - 1);
                best[sizeof(best) - 1] = '\0';
            }
        }
    }
    (void)fclose(f);
    if (bestlen >= 0) {
        strncpy(buf, best, buflen - 1);
        buf[buflen - 1] = '\0';
    }
}

#else

void sb_fs_device(path, buf, buflen)
    char *path; char *buf; int buflen;
{
    if (buflen > 0) buf[0] = '\0';
    (void)proc_mounts(path, buf, buflen, (char *)0, 0);   /* Linux */
#ifdef HAVE_STATFS_MOUNT
    if (!buf[0]) {                  /* macOS / BSD: "/dev/disk3s1s1" */
        struct statfs sf;
        char dir[1024];
        dirof(path, dir, (int)sizeof(dir));
        if (statfs(dir, &sf) == 0) {
            strncpy(buf, sf.f_mntfromname, buflen - 1);
            buf[buflen - 1] = '\0';
        }
    }
#endif
}
#endif  /* HAVE_MNTTAB */

#ifdef HAVE_POPEN
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>     /* readlink, for /dev/dsk -> driver-instance names */

unsigned long sb_cpu_mhz()
{
    FILE *p;
    char line[256], *s;
    unsigned long mhz = 0;

    p = popen("psrinfo -v 2>/dev/null", "r");
    if (p == (FILE *)0)
        return linux_mhz();
    while (fgets(line, (int)sizeof(line), p) != (char *)0) {
        s = strstr(line, "operates at");
        if (s != (char *)0) {
            s += 11;
            while (*s == ' ') s++;
            mhz = (unsigned long)atol(s);
            break;
        }
    }
    (void)pclose(p);
    return mhz ? mhz : linux_mhz();
}

static void rtrim(s)
    char *s;
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n'))
        s[--n] = '\0';
}

/* Resolve /dev/dsk/cXtXdXsX to its driver instance name ("dad0", "sd3")
 * via the /devices symlink and /etc/path_to_inst.  Solaris IDE disks show
 * up in `iostat -En` under the instance name, NOT cXtXdX, so matching by
 * ctd alone found nothing there (or worse, the wrong device).  0 = ok. */
static int inst_name(device, out, outlen)
    char *device; char *out; int outlen;
{
    char lnk[512], phys[512], line[600], drv[32];
    char *p, *q1, *q2, *colon;
    int n, inst;
    FILE *f;

    if (outlen < 8) return -1;
#ifdef HAVE_READLINK
    n = (int)readlink(device, lnk, sizeof(lnk) - 1);
#else
    n = 0;                                    /* no symlinks: no /devices */
#endif
    if (n <= 0) return -1;
    lnk[n] = '\0';
    p = strstr(lnk, "/devices/");
    if (p == (char *)0) return -1;
    strncpy(phys, p + 8, sizeof(phys) - 1);   /* keep leading '/' after it */
    phys[sizeof(phys) - 1] = '\0';
    colon = strrchr(phys, ':');
    if (colon) *colon = '\0';                 /* drop the minor (":a") */

    f = fopen("/etc/path_to_inst", "r");
    if (f == (FILE *)0) return -1;
    while (fgets(line, (int)sizeof(line), f) != (char *)0) {
        q1 = strchr(line, '"');
        if (q1 == (char *)0) continue;
        q2 = strchr(q1 + 1, '"');
        if (q2 == (char *)0) continue;
        *q2 = '\0';
        if (strcmp(q1 + 1, phys) != 0) continue;
        if (sscanf(q2 + 1, " %d \"%31[^\"]\"", &inst, drv) == 2) {
            (void)fclose(f);
            if ((int)strlen(drv) + 8 > outlen) return -1;
            sprintf(out, "%s%d", drv, inst);
            return 0;
        }
    }
    (void)fclose(f);
    return -1;
}

/* Find the start of an iostat -En device header line for `name` in the
 * captured output; NULL if absent. */
static char *iostat_dev(io, name)
    char *io, *name;
{
    char *p = io;
    int L = (int)strlen(name);
    while (p != (char *)0 && *p) {
        if (strncmp(p, name, (unsigned)L) == 0 &&
            (p[L] == ' ' || p[L] == '\t'))
            return p;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return (char *)0;
}

void sb_disk_model(device, buf, buflen)
    char *device; char *buf; int buflen;
{
    static char io[16384];
    FILE *p;
    char ctd[64], inst[40], line[300], vend[32], prod[48], rev[16];
    char full[160];
    char *base, *s, *v, *pr, *rv, *hdr, *le;
    int L, n, lines, have_inst;

    if (buflen > 0) buf[0] = '\0';
    if (device == (char *)0 || device[0] == '\0') return;

    /* /dev/dsk/c0t3d0s7 -> basename c0t3d0s7 -> strip slice -> c0t3d0 */
    base = strrchr(device, '/');
    base = base ? base + 1 : device;
    strncpy(ctd, base, sizeof(ctd) - 1);
    ctd[sizeof(ctd) - 1] = '\0';
    s = strrchr(ctd, 's');
    if (s) *s = '\0';
    if (ctd[0] == '\0') return;

    have_inst = inst_name(device, inst, (int)sizeof(inst)) == 0;

    p = popen("iostat -En 2>/dev/null", "r");
    if (p == (FILE *)0) return;
    n = (int)fread(io, 1, sizeof(io) - 1, p);
    if (n < 0) n = 0;
    io[n] = '\0';
    (void)pclose(p);

    /* The instance name is authoritative (it is how iostat labels IDE
     * disks, and it cannot match an unrelated device); ctd is the classic
     * SCSI label and the fallback. */
    hdr = (char *)0;
    if (have_inst)
        hdr = iostat_dev(io, inst);
    if (hdr == (char *)0)
        hdr = iostat_dev(io, ctd);
    if (hdr == (char *)0)
        return;

    /* Scan the next few lines of this entry for the identity line:
     * SCSI:  "Vendor: X  Product: Y  Revision: Z ..."
     * IDE:   "Model: X  Revision: Z ..." */
    s = strchr(hdr, '\n');
    for (lines = 0; s != (char *)0 && lines < 5; lines++) {
        s++;
        le = strchr(s, '\n');
        L = le ? (int)(le - s) : (int)strlen(s);
        if (L > (int)sizeof(line) - 1) L = (int)sizeof(line) - 1;
        strncpy(line, s, (unsigned)L);
        line[L] = '\0';
        s = le;

        v  = strstr(line, "Vendor:");
        pr = strstr(line, "Product:");
        rv = strstr(line, "Revision:");
        if (v != (char *)0 && pr != (char *)0) {
            v += 7; while (*v == ' ') v++;
            n = (int)(pr - v);
            if (n > (int)sizeof(vend) - 1) n = sizeof(vend) - 1;
            if (n < 0) n = 0;
            strncpy(vend, v, (unsigned)n); vend[n] = '\0'; rtrim(vend);
            pr += 8; while (*pr == ' ') pr++;
            n = rv ? (int)(rv - pr) : (int)strlen(pr);
            if (n > (int)sizeof(prod) - 1) n = sizeof(prod) - 1;
            if (n < 0) n = 0;
            strncpy(prod, pr, (unsigned)n); prod[n] = '\0'; rtrim(prod);
        } else {
            pr = strstr(line, "Model:");
            if (pr == (char *)0)
                continue;                    /* not the identity line yet */
            pr += 6; while (*pr == ' ') pr++;
            n = rv ? (int)(rv - pr) : (int)strlen(pr);
            if (n > (int)sizeof(prod) - 1) n = sizeof(prod) - 1;
            if (n < 0) n = 0;
            strncpy(prod, pr, (unsigned)n); prod[n] = '\0'; rtrim(prod);
            vend[0] = '\0';
        }
        rev[0] = '\0';
        if (rv) {
            rv += 9; while (*rv == ' ') rv++;
            n = 0;
            while (rv[n] && rv[n] != ' ' && rv[n] != '\t' &&
                   n < (int)sizeof(rev) - 1) { rev[n] = rv[n]; n++; }
            rev[n] = '\0';
        }
        if (vend[0] && rev[0])
            sprintf(full, "%s %s (rev %s)", vend, prod, rev);
        else if (vend[0])
            sprintf(full, "%s %s", vend, prod);
        else if (rev[0])
            sprintf(full, "%s (rev %s)", prod, rev);
        else
            sprintf(full, "%s", prod);
        strncpy(buf, full, buflen - 1);
        buf[buflen - 1] = '\0';
        return;
    }
}

/* psrinfo -pv (Solaris 9+) prints one block per physical chip:
 *   The physical processor has 4 virtual processors (0-3)
 *     x86 (chipid 0x0 AuthenticAMD 100F42 family 16 ... clock 2300 MHz)
 *           Quad-Core AMD Opteron(tm) Processor 2376
 * or on SPARC
 *   The physical processor has 1 virtual processor (0)
 *     UltraSPARC-IIIi (portid 0 impl 0x16 ver 0x34 clock 1503 MHz)
 * x86 puts the brand string on its own line; SPARC names the chip before
 * the parenthesis.  Result is cached: the header is printed per section. */
void sb_cpu_desc(buf, buflen)
    char *buf; int buflen;
{
    static char cache[160];
    static int done = 0;
    FILE *p;
    char line[256], brand[128], *s, *e;
    int nphys, found;

    if (buflen > 0) buf[0] = '\0';
    if (!done) {
        done = 1;
        cache[0] = '\0';
        brand[0] = '\0';
        nphys = 0;
        found = 0;
        p = popen("/usr/sbin/psrinfo -pv 2>/dev/null", "r");
        if (p != (FILE *)0) {
            while (fgets(line, (int)sizeof(line), p) != (char *)0) {
                rtrim(line);
                if (strncmp(line, "The physical processor", 22) == 0) {
                    nphys++;
                    continue;
                }
                if (found || nphys == 0)
                    continue;
                s = line;
                while (*s == ' ' || *s == '\t') s++;
                if (*s == '\0')
                    continue;
                if (strncmp(s, "x86 (", 5) == 0 || strncmp(s, "i386 (", 6) == 0)
                    continue;             /* brand string is the next line */
                e = strstr(s, " (");
                if (e) *e = '\0';        /* SPARC: chip name only */
                squeeze(brand, s, (int)sizeof(brand));
                found = brand[0] != '\0';
            }
            (void)pclose(p);
        }
        if (found) {
            if (nphys > 1)
                (void)sprintf(cache, "%d x %s", nphys, brand);
            else
                strcpy(cache, brand);
        } else
            proc_cpu_desc(cache, (int)sizeof(cache));      /* Linux */
    }
    if (buflen > 0) {
        strncpy(buf, cache, buflen - 1);
        buf[buflen - 1] = '\0';
    }
}

#ifdef HAVE_SYSINFO
static void x86_system(buf, buflen)
    char *buf; int buflen;
{
    static char cache[128];
    static int done = 0;
    FILE *p;
    char line[256], *s;

    if (!done) {
        done = 1;
        cache[0] = '\0';
        p = popen("/usr/sbin/prtdiag 2>/dev/null", "r");
        if (p != (FILE *)0) {
            while (fgets(line, (int)sizeof(line), p) != (char *)0) {
                rtrim(line);
                s = strstr(line, "System Configuration:");
                if (s != (char *)0) {
                    squeeze(cache, s + 21, (int)sizeof(cache));
                    break;
                }
            }
            (void)pclose(p);
        }
    }
    if (cache[0] && buflen > 0) {
        strncpy(buf, cache, buflen - 1);
        buf[buflen - 1] = '\0';
    }
}
#endif

#else

void sb_disk_model(device, buf, buflen)
    char *device; char *buf; int buflen;
{
    if (buflen > 0) buf[0] = '\0';
}

unsigned long sb_cpu_mhz()
{
    return linux_mhz();
}

void sb_cpu_desc(buf, buflen)
    char *buf; int buflen;
{
    if (buflen > 0) buf[0] = '\0';
    if (buflen > 0)
        proc_cpu_desc(buf, buflen);
}
#endif  /* HAVE_POPEN */

/* --------------------- network interface synopsis ---------------------- */

#ifdef HAVE_SOCKETS
#include <sys/socket.h>
#include <sys/ioctl.h>
#if defined(sun) || defined(__sun)
#include <sys/sockio.h>         /* SIOCGIF* live here on SunOS/Solaris --
                                 * sys/ioctl.h does NOT pull them in as it
                                 * does on BSD-flavoured systems */
#endif
#include <net/if.h>
#include <netinet/in.h>         /* sockaddr_in for the AF_INET filter */
#endif  /* HAVE_SOCKETS */

/* Solaris returns SIOCGIFMTU's answer in ifr_metric and never defines the
 * BSD ifr_mtu alias; supply it so the MTU code compiles in there too. */
#if defined(SIOCGIFMTU) && !defined(ifr_mtu) && defined(ifr_metric)
#define ifr_mtu ifr_metric
#endif

/* Fill dst with "le0 (mtu 1500), hme0 (mtu 1500)" -- every interface that
 * is UP and not loopback.  Returns the string length (0 = nothing found).
 * The driver name is the archaeology: le is a Lance, hme a Happy Meal,
 * ce a GigaSwift, tr token ring.  Fixed-stride ifreq walk as in
 * bench_net.c -- correct for SunOS 4 and Solaris; only AF_INET entries
 * are considered and logical aliases (hme0:1) are skipped. */
static int net_line(dst, max)
    char *dst; int max;
{
    int len = 0;
#if defined(HAVE_SOCKETS) && defined(SIOCGIFCONF)
    struct ifconf ifc;
    struct ifreq *ifr, req;
    char cbuf[2048];
    int s, n, i;

    dst[0] = '\0';
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return 0;
    ifc.ifc_len = (int)sizeof(cbuf);
    ifc.ifc_buf = cbuf;
    if (ioctl(s, SIOCGIFCONF, (char *)&ifc) < 0) {
        (void)close(s);
        return 0;
    }
    n = ifc.ifc_len / (int)sizeof(struct ifreq);
    ifr = (struct ifreq *)cbuf;
    for (i = 0; i < n; i++) {
        struct sockaddr_in *ia = (struct sockaddr_in *)&ifr[i].ifr_addr;
        char one[48];
        int ol;
        if (ia->sin_family != AF_INET)
            continue;
        if (strchr(ifr[i].ifr_name, ':') != (char *)0)
            continue;                        /* logical alias */
        memcpy(req.ifr_name, ifr[i].ifr_name, sizeof(req.ifr_name));
        if (ioctl(s, SIOCGIFFLAGS, (char *)&req) < 0)
            continue;
        if (!(req.ifr_flags & IFF_UP) || (req.ifr_flags & IFF_LOOPBACK))
            continue;
        one[0] = '\0';
#if defined(SIOCGIFMTU) && defined(ifr_mtu)
        memcpy(req.ifr_name, ifr[i].ifr_name, sizeof(req.ifr_name));
        if (ioctl(s, SIOCGIFMTU, (char *)&req) >= 0)
            (void)sprintf(one, "%.16s (mtu %d)", ifr[i].ifr_name,
                          (int)req.ifr_mtu);
#endif
        if (one[0] == '\0')
            (void)sprintf(one, "%.16s", ifr[i].ifr_name);
        ol = (int)strlen(one);
        if (len + ol + 3 >= max)
            break;
        if (len > 0) {
            strcpy(dst + len, ", ");
            len += 2;
        }
        strcpy(dst + len, one);
        len += ol;
    }
    (void)close(s);
#else
    dst[0] = '\0';
    (void)max;
#endif
    return len;
}

/* Emit one prepared line to out and (if set) tee. */
static void emit2(out, tee, s)
    FILE *out, *tee; char *s;
{
    fputs(s, out);
    if (tee) fputs(s, tee);
}

/* Build the synopsis into `lines` (newline-free), returning the line count.
 * One place so the log preamble and the on-screen header never diverge. */
int sb_sysinfo_lines(path, lines, maxlines)
    char *path; char (*lines)[SB_SYSLINE]; int maxlines;
{
    struct utsname u;
    unsigned long ram, tot, freem;
    char fstype[32], dev[256], model[160], *p;
    int n = 0;

    if (n < maxlines && uname(&u) >= 0)
        (void)sprintf(lines[n++], "  system : %s %s %s %s %s",
                u.sysname, u.nodename, u.release, u.version, u.machine);
    {
        char plat[128], cpu[160];
        sb_model(plat, (int)sizeof(plat));
        if (n < maxlines && plat[0])
            (void)sprintf(lines[n++], "  model  : %s", plat);
        sb_cpu_desc(cpu, (int)sizeof(cpu));
        if (n < maxlines && cpu[0])
            (void)sprintf(lines[n++], "  cpu    : %s", cpu);
    }
    ram = sb_physmem_mb();
    if (n < maxlines && ram)
        (void)sprintf(lines[n++], "  memory : %lu MB", ram);

    fstype[0] = '\0'; dev[0] = '\0'; model[0] = '\0';
    sb_fs_type(path, fstype, (int)sizeof(fstype));
    sb_fs_device(path, dev, (int)sizeof(dev));
    tot   = sb_fs_total_mb(path);
    freem = sb_freespace_mb(path);
    /* The scratch path itself is NOT repeated here -- the disk report's
     * "file :" line already names it.  This line is about the filesystem:
     *   disk   : ufs on /dev/dsk/c0t3d0s0, 2215 MB total, 430 MB free   */
    if (n < maxlines && (fstype[0] || dev[0] || tot)) {
        /* sprintf-then-strlen, NOT p += sprintf(): on SunOS 4 sprintf
         * returns char * (old BSD), not the ANSI character count. */
        p = lines[n];
        (void)sprintf(p, "  disk   :");
        p += strlen(p);
        if (fstype[0]) {
            (void)sprintf(p, " %s", fstype);
            p += strlen(p);
        }
        if (dev[0]) {
            (void)sprintf(p, "%s %s", fstype[0] ? " on" : "", dev);
            p += strlen(p);
        }
        if (tot)
            (void)sprintf(p, "%s %lu MB total, %lu MB free",
                          (fstype[0] || dev[0]) ? "," : "", tot, freem);
        n++;
    }
    if (n < maxlines && dev[0]) {
        sb_disk_model(dev, model, (int)sizeof(model));
        if (model[0])
            (void)sprintf(lines[n++], "  drive  : %s", model);
    }
    if (n < maxlines) {
        char nb[SB_SYSLINE - 16];
        if (net_line(nb, (int)sizeof(nb)) > 0)
            (void)sprintf(lines[n++], "  net    : %s", nb);
    }
    return n;
}

void sb_write_sysinfo(out, tee, path)
    FILE *out, *tee; char *path;
{
    char lines[SB_SYSLINES][SB_SYSLINE];
    char buf[SB_SYSLINE + 2];
    int n, i;

    n = sb_sysinfo_lines(path, lines, SB_SYSLINES);
    for (i = 0; i < n; i++) {
        (void)sprintf(buf, "%s\n", lines[i]);
        emit2(out, tee, buf);
    }
}

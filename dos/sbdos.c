/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * sbdos.c -- sunbench for MS-DOS (16-bit real mode): disk, cpu, memory.
 *
 * Built on the Mac with Open Watcom (dos/build.sh) as 8086 code, so one
 * SBDOS.EXE runs on every PC from a Model 25 up.  (A 386 build was tried
 * and dropped: Watcom's 16-bit -3 still does 32-bit multiply/divide through
 * the same 16-bit library helpers, so it measured the same.)
 *
 * cpu and mem are sunbench's own kernels (src/cpu.c, src/bench_mem.c,
 * Dhrystone, Whetstone) compiled unchanged for the large model; the HTML
 * report is src/html.c.  The small shim below supplies the timer, host
 * and machine-synopsis hooks they expect.
 *
 * Disk has two layers:
 *   raw   BIOS INT 13h sector I/O on a whole drive: the drive (or a drive
 *         emulator) plus the BIOS's own driver, nothing else.  Read-only
 *         unless --rewrite, which times writes of data just read back to
 *         the same sectors.
 *   file  DOS file I/O (INT 21h) on a scratch file: what programs get.
 *
 * Timing reads the 8254 timer's counter directly (0.84 us resolution):
 * DOS's own clock ticks only 18.2 times a second, far too coarse for
 * per-operation latency histograms.
 *
 * Output follows sunbench's log format exactly ("sunbench -- disk", the
 * block-size sweep table, latency histograms), so sunbench-fleet.py reads
 * DOS logs like any other machine's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <io.h>
#include <dos.h>
#include <i86.h>
#include <conio.h>
#include "version.h"
#include "histogram.h"
#include "timer.h"
#include "diskio.h"
#include "sysinfo.h"
#include "cpu.h"
#include "bench_mem.h"
#include "whet.h"
#include "html.h"
#include "sbdos.h"

#define PIT_HZ      1193182.0
#define TICKS_DAY   0x1800B0L         /* BIOS ticks per 24 hours */
#define BUFSZ       65536L            /* I/O buffer: one 64 KB DMA page */
#define CHUNK       32768L            /* max bytes per read()/write() */
#define MB          (1024.0 * 1024.0)

#define T_WRITE     1
#define T_RAND      2

typedef struct res {
    char   name[16];
    double mbps, iops, lat_ms;
    int    ok;
} res;

/* --- options ------------------------------------------------------------ */
static int    o_raw = 1;              /* raw INT 13h (else DOS file) */
static int    o_drive = 0x80;
static char  *o_file = "SBDOS.DAT";
static long   o_size_mb = 32;         /* file mode */
static long   o_block_kb = 64;
double o_secs = 8.0;
int    o_secs_set = 0;
static int    o_rewrite = 0;          /* raw: non-destructive write tests */
static int    o_maxsec = 64;          /* sectors per INT 13h call */
static int    o_hist = 1;
static char  *o_log = (char *)0;
static char  *o_note = (char *)0;
static char  *o_host = "dos";
static char  *o_model = (char *)0;
static char  *o_html = (char *)0;
static unsigned long o_mhz = 0;       /* 0: from the model table */

static FILE  *logf = (FILE *)0;

/* --- 8254 timer --------------------------------------------------------- */
/* Channel 0 is switched from mode 3 (square wave, counts by 2) to mode 2
 * (rate generator, counts by 1) at the SAME 65536 divisor, so the 18.2 Hz
 * BIOS tick is unchanged and the counter reads as a clean position within
 * the current tick.  Restored on every exit path. */
static int pit_changed = 0;
static double day_offset = 0.0, last_t = 0.0;

static void pit_mode2()
{
    _disable();
    outp(0x43, 0x34);
    outp(0x40, 0);
    outp(0x40, 0);
    _enable();
    pit_changed = 1;
}

static void pit_restore()
{
    if (!pit_changed)
        return;
    _disable();
    outp(0x43, 0x36);
    outp(0x40, 0);
    outp(0x40, 0);
    _enable();
    pit_changed = 0;
}

/* The clock as raw integers (BIOS ticks + 8254 counts into the tick):
 * cheap enough to take per received packet even with no coprocessor,
 * where every double operation is a software-emulated call.  raw_s()
 * converts afterwards.  now_s() = raw_s(now_raw()). */
void now_raw(ticks_out, el_out)
    unsigned long *ticks_out; unsigned *el_out;
{
    unsigned long ticks;
    unsigned lo, hi, elapsed, irr;

    _disable();
    outp(0x43, 0x00);                         /* latch channel 0 */
    lo = inp(0x40);
    hi = inp(0x40);
    ticks = *(unsigned long far *)MK_FP(0x40, 0x6C);
    outp(0x20, 0x0A);                         /* PIC: read IRR */
    irr = inp(0x20);
    _enable();
    elapsed = (unsigned)(0 - ((hi << 8) | lo));
    /* A tick that fired while interrupts were off is pending in the PIC
     * but not yet counted: the counter has wrapped, the tick count not. */
    if ((irr & 1) && elapsed < 0x8000)
        ticks++;
    *ticks_out = ticks;
    *el_out = elapsed;
}

double raw_s(ticks, elapsed)
    unsigned long ticks; unsigned elapsed;
{
    return ((double)ticks * 65536.0 + (double)elapsed) / PIT_HZ + day_offset;
}

double now_s()
{
    unsigned long ticks;
    unsigned elapsed;
    double t;

    now_raw(&ticks, &elapsed);
    t = raw_s(ticks, elapsed);
    if (t < last_t - 60.0) {                  /* midnight rollover */
        day_offset += (double)TICKS_DAY * 65536.0 / PIT_HZ;
        t += (double)TICKS_DAY * 65536.0 / PIT_HZ;
    }
    last_t = t;
    return t;
}

static void on_break(sig)
    int sig;
{
    (void)sig;
    pit_restore();
    fprintf(stderr, "\nsbdos: interrupted\n");
    exit(1);
}

/* --- shim: what src/cpu.c, bench_mem.c and html.c expect from timer.c,
 * diskio.c and sysinfo.c ------------------------------------------------- */
double sb_now()       { return now_s(); }
double sb_gtod()      { return now_s(); }
double sb_timer_res() { return 1.0 / PIT_HZ; }
double sb_min_batch() { return 0.05; }

/* Only cpu.c's streaming ?axpy pair asks for this, wanting >= 256 KB per
 * vector: more than one 64 KB segment can address.  Refusing makes
 * sb_cpu_run skip those two kernels, as it does on any allocation failure. */
char *sb_alloc_aligned(sz)
    unsigned long sz;
{
    (void)sz;
    return (char *)0;
}

unsigned long sb_physmem_mb() { return 0; }

/* bench_mem.c's SB_SEGMENTED hook: base+off as a normalized far pointer
 * (offset < 16), so a full 64 KB is addressable from it. */
char *sb_seg_ptr(base, off)
    char *base; unsigned long off;
{
    unsigned long lin;
    lin = ((unsigned long)FP_SEG(base) << 4) + FP_OFF(base) + off;
    return (char *)MK_FP((unsigned)(lin >> 4), (unsigned)(lin & 15));
}

void sb_host(b, n)
    char *b; int n;
{
    strncpy(b, o_host, n - 1);
    b[n - 1] = '\0';
}

/* --- output (screen + optional log) ------------------------------------- */
void out(s)
    char *s;
{
    fputs(s, stdout);
    if (logf) fputs(s, logf);
}

/* --- DOS memory ----------------------------------------------------------
 * Every big buffer is taken from DOS only while its test runs and given
 * back after: the disk's 64 KB I/O buffer, cpu.c's ?axpy vectors (120 KB,
 * via sb_seg_alloc), the network receive ring (32 KB, pktnet.c) and the
 * memory sweep's working set.  So each test sees nearly all free memory
 * -- with a packet driver and network setup loaded too, as in a normal
 * boot.  The C heap is grown once, at startup (heap_reserve), so its
 * blocks sit below everything else and never split the free memory. */
#define HEAP_RESERVE 24576u             /* bytes: stdio buffers, chain index */
#define MEM_HEADROOM 0x0100             /* paragraphs (4 KB) left over */

static unsigned buf_seg = 0, buf_dosseg = 0;
static char far *buf;                   /* disk I/O buffer, 64 KB */

static void heap_reserve()
{
    char *p = (char *)malloc(HEAP_RESERVE);
    if (p) free(p);                     /* the heap keeps the segment */
}

/* 64 KB aligned to a 64 KB physical page, so no BIOS/DMA transfer
 * straddles one: 128 KB from DOS, the aligned half used. */
static int disk_buf_get()
{
    unsigned seg;
    if (_dos_allocmem(0x2000, &seg) != 0) {
        fprintf(stderr, "sbdos: cannot allocate 128 KB of DOS memory for the disk buffer\n");
        return -1;
    }
    buf_dosseg = seg;
    buf_seg = (seg + 0x0FFF) & 0xF000;
    buf = (char far *)MK_FP(buf_seg, 0);
    return 0;
}

static void disk_buf_put()
{
    if (buf_dosseg) _dos_freemem(buf_dosseg);
    buf_dosseg = 0;
}

/* sb_seg_alloc / sb_seg_free: cpu.c's SB_SEGMENTED vector hooks.  One
 * DOS block per vector (each under 64 KB), offset 0. */
char *sb_seg_alloc(sz)
    unsigned long sz;
{
    unsigned seg;
    if (sz > 65520L || _dos_allocmem((unsigned)((sz + 15L) >> 4), &seg) != 0)
        return (char *)0;
    return (char *)MK_FP(seg, 0);
}

void sb_seg_free(p)
    char *p;
{
    if (p) _dos_freemem(FP_SEG(p));
}

/* Free conventional memory, in paragraphs, as the largest block. */
static unsigned dos_largest()
{
    unsigned seg;
    if (_dos_allocmem(0xFFFF, &seg) == 0) {     /* impossible below 1 MB */
        _dos_freemem(seg);
        return 0xFFFF;
    }
    return seg;
}

/* --- system description ------------------------------------------------- */
static int dos_major, dos_minor;
static unsigned conv_kb, ext_kb;
static int ext_via_xms = 0;             /* ext_kb is XMS free, not BIOS */
unsigned xms_free_kb(void);             /* dos/xms.asm; 0xFFFF = none */
static int model_b = -1, submodel_b = -1, biosrev_b = -1;

static int cpu_gen, has_fpu;           /* 86/286/386/486; INT 11h bit 1 */

static struct ps2m { int m, s; char *name; int mhz; } ps2tab[] = {
    { 0xFA, 0x00, "IBM PS/2 Model 30",           8 },
    { 0xFA, 0x01, "IBM PS/2 Model 25",           8 },
    { 0xFC, 0x04, "IBM PS/2 Model 50",          10 },
    { 0xFC, 0x05, "IBM PS/2 Model 60",          10 },
    { 0xFC, 0x09, "IBM PS/2 Model 30-286",      10 },
    { 0xF8, 0x00, "IBM PS/2 Model 80 (16 MHz)", 16 },
    { 0xF8, 0x01, "IBM PS/2 Model 80 (20 MHz)", 20 },
    { 0xF8, 0x04, "IBM PS/2 Model 70 (20 MHz)", 20 },
    { 0xF8, 0x09, "IBM PS/2 Model 70 (16 MHz)", 16 },
    { 0xF8, 0x0C, "IBM PS/2 Model 55SX",        16 },
    { 0xF8, 0x0D, "IBM PS/2 Model 70 (25 MHz)", 25 },
    { 0, 0, (char *)0, 0 }
};

static struct ps2m *ps2_find(m, s)
    int m, s;
{
    int i;
    for (i = 0; ps2tab[i].name; i++)
        if (ps2tab[i].m == m && ps2tab[i].s == s)
            return &ps2tab[i];
    return (struct ps2m *)0;
}

static char *ps2_name(m, s)
    int m, s;
{
    struct ps2m *p = ps2_find(m, s);
    return p ? p->name : (char *)0;
}

/* CPU generation from the FLAGS register, the classic way: the 8086
 * forces bits 12-15 to 1, the 286 (real mode) forces them to 0, and only
 * a 486 can toggle EFLAGS.AC (bit 18).  The 32-bit instructions are
 * hand-assembled bytes, so this compiles in 8086 mode and is reached
 * only once a 386 has been established. */
unsigned flags_try(unsigned v);
#pragma aux flags_try = \
    "pushf" "push ax" "popf" "pushf" "pop ax" "popf" \
    __parm [__ax] __value [__ax];
unsigned flags_get(void);
#pragma aux flags_get = "pushf" "pop ax" __value [__ax];
unsigned ac_toggles(void);
#pragma aux ac_toggles = \
    0x66 0x51                       /* push   ecx         */ \
    0x66 0x9C                       /* pushfd             */ \
    0x66 0x58                       /* pop    eax         */ \
    0x66 0x8B 0xC8                  /* mov    ecx, eax    */ \
    0x66 0x35 0x00 0x00 0x04 0x00   /* xor    eax, 40000h */ \
    0x66 0x50                       /* push   eax         */ \
    0x66 0x9D                       /* popfd              */ \
    0x66 0x9C                       /* pushfd             */ \
    0x66 0x58                       /* pop    eax         */ \
    0x66 0x33 0xC1                  /* xor    eax, ecx    */ \
    0x66 0x51                       /* push   ecx         */ \
    0x66 0x9D                       /* popfd: restore     */ \
    0x66 0x59                       /* pop    ecx         */ \
    0x66 0xC1 0xE8 0x10             /* shr    eax, 16     */ \
    __value [__ax] __modify [__ax];

static int detect_cpu()
{
    unsigned f = flags_get();
    if ((flags_try(f & 0x0FFF) & 0xF000) == 0xF000) return 86;
    if ((flags_try(f | 0xF000) & 0xF000) == 0) return 286;
    return (ac_toggles() & 4) ? 486 : 386;
}

static char *cpu_str()
{
    static char b[80];
    sprintf(b, "%s, %s",
            cpu_gen == 86 ? "8086/8088" : cpu_gen == 286 ? "80286" :
            cpu_gen == 386 ? "80386" : "80486 or later",
            !has_fpu ? "no coprocessor (floating point emulated)"
            : getenv("NO87") ? "math coprocessor present, NO87 set: emulated"
            : "math coprocessor present");
    return b;
}

static void probe_system()
{
    union REGS r;
    struct SREGS sr;
    unsigned char far *cfg;

    r.x.ax = 0x3000;
    intdos(&r, &r);
    dos_major = r.h.al;
    dos_minor = r.h.ah;

    int86(0x12, &r, &r);
    conv_kb = r.x.ax;

    int86(0x11, &r, &r);                  /* BIOS equipment word */
    has_fpu = (r.x.ax & 2) != 0;
    cpu_gen = detect_cpu();

    r.h.ah = 0x88;
    int86(0x15, &r, &r);
    ext_kb = r.x.cflag ? 0 : r.x.ax;
    if (ext_kb == 0) {                    /* HIMEM.SYS hides it from INT 15h */
        unsigned x = xms_free_kb();
        if (x != 0xFFFF && x != 0) {
            ext_kb = x;
            ext_via_xms = 1;
        }
    }

    /* INT 15h C0h: system configuration table (PS/2 and later ATs). */
    segread(&sr);
    r.h.ah = 0xC0;
    int86x(0x15, &r, &r, &sr);
    if (!r.x.cflag && r.h.ah == 0) {
        cfg = (unsigned char far *)MK_FP(sr.es, r.x.bx);
        model_b = cfg[2];
        submodel_b = cfg[3];
        biosrev_b = cfg[4];
    }
    if (o_mhz == 0 && model_b >= 0 && ps2_find(model_b, submodel_b))
        o_mhz = (unsigned long)ps2_find(model_b, submodel_b)->mhz;
}

/* --- raw INT 13h -------------------------------------------------------- */
static unsigned g_cyls, g_heads, g_spt;
static unsigned long g_total;            /* sectors */

static int raw_geometry()
{
    union REGS r;
    struct SREGS sr;

    segread(&sr);
    r.h.ah = 0x08;
    r.h.dl = (unsigned char)o_drive;
    int86x(0x13, &r, &r, &sr);
    if (r.x.cflag) {
        fprintf(stderr, "sbdos: BIOS drive %02Xh: no geometry (INT 13h AH=08h failed)\n", o_drive);
        return -1;
    }
    g_spt = r.h.cl & 0x3F;
    g_heads = (unsigned)r.h.dh + 1;
    g_cyls = (((unsigned)(r.h.cl & 0xC0) << 2) | r.h.ch) + 1;
    g_total = (unsigned long)g_cyls * g_heads * g_spt;
    if (g_spt == 0 || g_total == 0) {
        fprintf(stderr, "sbdos: BIOS drive %02Xh reports an empty geometry\n", o_drive);
        return -1;
    }
    return 0;
}

/* One BIOS call: n sectors at lba, to/from buf+off.  Never crosses a track
 * (not every BIOS continues a multi-sector transfer onto the next head).
 * Retries after a drive reset, as DOS does. */
static int raw_call(wr, lba, n, seg, off)
    int wr; unsigned long lba; unsigned n; unsigned seg, off;
{
    union REGS r;
    struct SREGS sr;
    unsigned c, h, s;
    int tries;

    c = (unsigned)(lba / ((unsigned long)g_heads * g_spt));
    h = (unsigned)((lba / g_spt) % g_heads);
    s = (unsigned)(lba % g_spt) + 1;
    for (tries = 0; tries < 3; tries++) {
        segread(&sr);
        sr.es = seg;
        r.h.ah = (unsigned char)(wr ? 0x03 : 0x02);
        r.h.al = (unsigned char)n;
        r.h.ch = (unsigned char)(c & 0xFF);
        r.h.cl = (unsigned char)(((c >> 2) & 0xC0) | s);
        r.h.dh = (unsigned char)h;
        r.h.dl = (unsigned char)o_drive;
        r.x.bx = off;
        int86x(0x13, &r, &r, &sr);
        if (!r.x.cflag)
            return 0;
        r.h.ah = 0x00;                         /* reset, then retry */
        r.h.dl = (unsigned char)o_drive;
        int86(0x13, &r, &r);
    }
    fprintf(stderr, "sbdos: INT 13h %s error at LBA %lu (status %02Xh)\n",
            wr ? "write" : "read", lba, r.h.ah);
    return -1;
}

/* bytes at sector lba, split into track-bounded calls of <= o_maxsec. */
static int raw_xfer(wr, lba, bytes)
    int wr; unsigned long lba; long bytes;
{
    unsigned long left;
    unsigned n, room, off;

    left = (unsigned long)(bytes / 512L);
    off = 0;
    while (left > 0) {
        room = g_spt - (unsigned)(lba % g_spt);
        n = left < (unsigned long)room ? (unsigned)left : room;
        if (n > (unsigned)o_maxsec) n = (unsigned)o_maxsec;
        if (raw_call(wr, lba, n, buf_seg, off) != 0)
            return -1;
        lba += n;
        left -= n;
        off += n * 512;
    }
    return 0;
}

/* One sector from somewhere else on the drive, into a small buffer of its
 * own (placed so it cannot straddle a 64 KB DMA page): the --rewrite
 * random-write test's decoy, see run_test. */
static unsigned char dcy_area[1024];

static int raw_decoy(lba)
    unsigned long lba;
{
    unsigned char far *p = dcy_area;
    unsigned long phys = ((unsigned long)FP_SEG(p) << 4) + FP_OFF(p);
    if ((phys & 0xFFFFL) > 0xFE00L)
        phys += 512L;                         /* the second half is clear */
    return raw_call(0, lba, 1, (unsigned)(phys >> 4), (unsigned)(phys & 15));
}

/* --- DOS file ------------------------------------------------------------ */
static int fd = -1;
static long filesz;

static int smartdrv()                    /* INT 2Fh 4A10h: installed? */
{
    union REGS r;
    r.x.ax = 0x4A10;
    r.x.bx = 0x0000;
    int86(0x2F, &r, &r);
    return r.x.ax == 0xBABE;
}

/* SMARTDRV write-back commit and cache reset: sunbench's cache purge. */
static void smartdrv_flush()
{
    union REGS r;
    r.x.ax = 0x4A10; r.x.bx = 0x0001; int86(0x2F, &r, &r);
    r.x.ax = 0x4A10; r.x.bx = 0x0002; int86(0x2F, &r, &r);
}

static int file_xfer(wr, off, bytes)
    int wr; long off, bytes;
{
    long done;
    unsigned n;

    if (lseek(fd, off, SEEK_SET) != off)
        return -1;
    for (done = 0; done < bytes; done += n) {
        n = (unsigned)(bytes - done > CHUNK ? CHUNK : bytes - done);
        if (wr) {
            if ((unsigned)write(fd, buf + (unsigned)done, n) != n) return -1;
        } else {
            if ((unsigned)read(fd, buf + (unsigned)done, n) != n) return -1;
        }
    }
    return 0;
}

static void file_commit()
{
    union REGS r;
    r.h.ah = 0x68;                    /* commit file (DOS 3.3+) */
    r.x.bx = (unsigned)fd;
    intdos(&r, &r);
}

static int file_prep()
{
    long off;
    struct diskfree_t df;
    unsigned drv;
    double freemb;

    _dos_getdrive(&drv);
    if (o_file[1] == ':')
        drv = (unsigned)((o_file[0] | 0x20) - 'a' + 1);
    if (_dos_getdiskfree(drv, &df) == 0) {
        freemb = (double)df.avail_clusters * df.sectors_per_cluster *
                 df.bytes_per_sector / MB;
        if ((double)o_size_mb > freemb / 2.0) {
            o_size_mb = (long)(freemb / 2.0);
            fprintf(stderr, "sbdos: capping test file to %ld MB (%.0f MB free)\n",
                    o_size_mb, freemb);
        }
    }
    if (o_size_mb < 1) {
        fprintf(stderr, "sbdos: not enough free space for a test file\n");
        return -1;
    }
    filesz = o_size_mb * 1024L * 1024L;
    fd = open(o_file, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (fd < 0) {
        fprintf(stderr, "sbdos: cannot create %s\n", o_file);
        return -1;
    }
    _fmemset(buf, 0x5A, (unsigned)CHUNK);
    fprintf(stderr, "  preparing %ld MB test file %s\n", o_size_mb, o_file);
    for (off = 0; off < filesz; off += CHUNK)
        if ((unsigned)write(fd, buf, (unsigned)CHUNK) != (unsigned)CHUNK) {
            fprintf(stderr, "sbdos: write failed preparing %s\n", o_file);
            return -1;
        }
    file_commit();
    return 0;
}

/* --- one timed test ----------------------------------------------------- */
static unsigned long rng = 1;
static unsigned long rnd()
{
    rng = rng * 1664525L + 1013904223L;
    return rng >> 8;
}

static unsigned long seqpos = 0;          /* in blocks; continues test to
                                           * test so a seq run never re-reads
                                           * what the last one just did */

static int run_test(kind, bs, secs, h, rs, name)
    int kind; long bs; double secs; sb_hist *h; res *rs; char *name;
{
    unsigned long nblocks, blk;
    double t0, ta, tb, busy, bytes, ops, lat;
    long span;
    int wr = (kind & T_WRITE) != 0;

    strcpy(rs->name, name);
    rs->ok = 0;
    if (o_raw)
        nblocks = (g_total * 512L) / (unsigned long)bs;
    else
        nblocks = (unsigned long)(filesz / bs);
    if (nblocks < 2) {
        fprintf(stderr, "sbdos: test area smaller than two %ld-byte blocks\n", bs);
        return -1;
    }
    if (h) sb_hist_init(h);
    if (smartdrv()) smartdrv_flush();     /* it caches INT 13h too */
    fprintf(stderr, "  %-10s ", name);

    busy = 0.0; bytes = 0.0; ops = 0.0;
    t0 = now_s();
    do {
        if (kind & T_RAND)
            blk = rnd() % nblocks;
        else
            blk = seqpos++ % nblocks;
        if (o_raw) {
            unsigned long lba = blk * (unsigned long)(bs / 512L);
            if (wr) {
                /* --rewrite: read the block back first (untimed), then
                 * time writing those same bytes to the same sectors.
                 * For the RANDOM test, one untimed sector read from
                 * another random place goes in between: written straight
                 * after its own read, the block was still in the drive's
                 * or controller's buffer, and small random writes
                 * measured twice as fast as random reads (difnif, PS/2
                 * Model 70).  Now each timed write starts from elsewhere,
                 * as a real random write does.  Sequential writes keep the
                 * read-then-write pairs: their next block follows anyway. */
                if (raw_xfer(0, lba, bs) != 0) return -1;
                if ((kind & T_RAND) &&
                    raw_decoy((rnd() % nblocks) * (unsigned long)(bs / 512L)) != 0)
                    return -1;
                ta = now_s();
                if (raw_xfer(1, lba, bs) != 0) return -1;
            } else {
                ta = now_s();
                if (raw_xfer(0, lba, bs) != 0) return -1;
            }
        } else {
            ta = now_s();
            span = (long)blk * bs;
            if (file_xfer(wr, span, bs) != 0) {
                fprintf(stderr, "sbdos: file I/O failed\n");
                return -1;
            }
        }
        tb = now_s();
        lat = tb - ta;
        busy += lat;
        bytes += (double)bs;
        ops += 1.0;
        if (h) sb_hist_add(h, lat);
    } while (tb - t0 < secs);
    if (wr) {                             /* the drain is real I/O */
        ta = now_s();
        if (!o_raw) file_commit();
        if (smartdrv()) smartdrv_flush();
        busy += now_s() - ta;
    }
    rs->mbps = busy > 0.0 ? bytes / MB / busy : 0.0;
    rs->iops = busy > 0.0 ? ops / busy : 0.0;
    rs->lat_ms = ops > 0.0 ? busy / ops * 1000.0 : 0.0;
    rs->ok = 1;
    fprintf(stderr, "%9.2f MB/s\n", rs->mbps);
    return 0;
}

/* --- reports ------------------------------------------------------------ */
static int disk_ready = 0;               /* raw geometry / test file set up */

/* The machine synopsis, as "  key    : value" lines (sunbench's format):
 * the log preamble and the HTML Machine card both come from here.  The
 * disk lines appear only when this run touched a disk. */
static int syslines(lines, max, disk)
    char (*lines)[SB_SYSLINE]; int max, disk;
{
    int n = 0;
    char *nm;

    sprintf(lines[n++], "  system : MS-DOS %s %d.%02d %s", o_host, dos_major,
            dos_minor, !disk ? "(sbdos)" :
            o_raw ? "(sbdos raw INT 13h)" : "(sbdos DOS file I/O)");
    nm = o_model ? o_model :
         (model_b >= 0 ? ps2_name(model_b, submodel_b) : (char *)0);
    if (model_b >= 0)
        sprintf(lines[n++], "  model  : %s (BIOS model %02X submodel %02X rev %02X)",
                nm ? nm : "unknown", model_b, submodel_b, biosrev_b);
    else if (nm)
        sprintf(lines[n++], "  model  : %s", nm);
    if (o_mhz)
        sprintf(lines[n++], "  cpu    : %s, %lu MHz", cpu_str(), o_mhz);
    else
        sprintf(lines[n++], "  cpu    : %s", cpu_str());
    sprintf(lines[n++], "  memory : %u KB conventional, %u KB extended%s",
            conv_kb, ext_kb, ext_via_xms ? " free (XMS, via HIMEM.SYS)" : "");
    if (disk && o_raw) {
        sprintf(lines[n++], "  disk   : raw BIOS drive %02Xh, %lu MB%s", o_drive,
                g_total / 2048L,
                smartdrv() ? " (SMARTDRV present: reset before each test)" : "");
        sprintf(lines[n++], "  drive  : %u cyl x %u heads x %u sectors (INT 13h CHS)",
                g_cyls, g_heads, g_spt);
    } else if (disk)
        sprintf(lines[n++], "  disk   : DOS file %s%s", o_file,
                smartdrv() ? " (SMARTDRV present: reset before each test)" : "");
    if (sbnet_syn() && n < max)
        strcpy(lines[n++], sbnet_syn());
    (void)max;
    return n;
}

/* html.c's Machine card.  `path` (the Unix scratch file) means nothing here. */
int sb_sysinfo_lines(path, lines, max)
    char *path; char (*lines)[SB_SYSLINE]; int max;
{
    (void)path;
    return syslines(lines, max, disk_ready);
}

void stamp(disk)
    int disk;
{
    time_t t;
    char line[SB_SYSLINE + 8];
    char lines[SB_SYSLINES][SB_SYSLINE];
    int i, n;

    (void)time(&t);
    sprintf(line, "\n========================================================\nsunbench %s   run: %s",
            SB_VERSION, ctime(&t));
    out(line);
    n = syslines(lines, SB_SYSLINES, disk);
    for (i = 0; i < n; i++) {
        sprintf(line, "%s\n", lines[i]);
        out(line);
    }
    if (o_note) {
        sprintf(line, "  note   : %s\n", o_note);
        out(line);
    }
}

void html_flush()
{
    if (!sb_html_on())
        return;
    if (sb_html_write() == 0)
        fprintf(stderr, "sbdos: HTML report written to %s\n", o_html);
    else
        fprintf(stderr, "sbdos: cannot write HTML report %s\n", o_html);
}

/* res -> the sb_result html.c takes. */
static void to_sbr(a, b)
    res *a; sb_result *b;
{
    memset((char *)b, 0, sizeof(*b));
    strncpy(b->name, a->name, sizeof(b->name) - 1);
    b->mb_per_s = a->mbps;
    b->iops = a->iops;
    b->avg_lat_ms = a->lat_ms;
    b->ok = a->ok;
}

static char *fdesc_str()
{
    static char b[96];
    if (o_raw)
        sprintf(b, "BIOS drive %02Xh (%lu MB)", o_drive, g_total / 2048L);
    else
        sprintf(b, "%s (%ld MB)", o_file, o_size_mb);
    return b;
}

static void lat_line(label, h)
    char *label; sb_hist *h;
{
    char line[160];
    if (h->n == 0) return;
    sprintf(line, "  %-10s min %.2f  p50 %.2f  p90 %.2f  p99 %.2f  max %.2f  (ms)\n",
            label, h->min_s * 1000.0, sb_hist_pct(h, 0.50) * 1000.0,
            sb_hist_pct(h, 0.90) * 1000.0, sb_hist_pct(h, 0.99) * 1000.0,
            h->max_s * 1000.0);
    out(line);
}

static void row(r)
    res *r;
{
    char line[120];
    if (!r->ok) return;
    sprintf(line, "  %-10s %10.2f %12.1f %10.3f\n", r->name, r->mbps, r->iops, r->lat_ms);
    out(line);
}

static char *method_str()
{
    return o_raw ? "raw BIOS INT 13h (no filesystem, no DOS)"
                 : "DOS file I/O (INT 21h)";
}

static sb_hist hr, hw;

static int std_run()
{
    res w, r, rr, rw;
    long bs = o_block_kb * 1024L;
    double secs = o_secs;
    char line[160];

    w.ok = r.ok = rr.ok = rw.ok = 0;
    hr.n = hw.n = 0;
    fprintf(stderr, "sbdos: standard run, %ld KB blocks, %.0fs per test\n",
            o_block_kb, secs);
    if ((!o_raw || o_rewrite) &&
        run_test(T_WRITE, bs, secs, (sb_hist *)0, &w, "seq write") != 0) return -1;
    if (run_test(0, bs, secs, (sb_hist *)0, &r, "seq read") != 0) return -1;
    if (run_test(T_RAND, bs, secs, o_hist ? &hr : (sb_hist *)0, &rr, "rand read") != 0) return -1;
    if ((!o_raw || o_rewrite) &&
        run_test(T_RAND | T_WRITE, bs, secs, o_hist ? &hw : (sb_hist *)0, &rw, "rand write") != 0) return -1;

    stamp(1);
    out("sunbench -- disk\n");
    sprintf(line, "  method : %s\n", method_str());
    out(line);
    if (o_raw)
        sprintf(line, "  file   : BIOS drive %02Xh  (%lu MB, %ld KB blocks)\n",
                o_drive, g_total / 2048L, o_block_kb);
    else
        sprintf(line, "  file   : %s  (%ld MB, %ld KB blocks)\n",
                o_file, o_size_mb, o_block_kb);
    out(line);
    sprintf(line, "  %-10s %10s %12s %10s\n", "test", "MB/s", "IOPS", "lat(ms)");
    out(line);
    row(&w); row(&r); row(&rr); row(&rw);
    if (o_hist) {
        out("\n");
        lat_line("rand read", &hr);
        lat_line("rand write", &hw);
        out("\n");
        sb_hist_bars(&hr, "rand read", stdout);
        if (logf) sb_hist_bars(&hr, "rand read", logf);
        if (hw.n) {
            out("\n");
            sb_hist_bars(&hw, "rand write", stdout);
            if (logf) sb_hist_bars(&hw, "rand write", logf);
        }
    }
    if (logf) fflush(logf);
    {
        sb_result sw, sr, srr, srw;
        to_sbr(&w, &sw); to_sbr(&r, &sr); to_sbr(&rr, &srr); to_sbr(&rw, &srw);
        sb_html_disk_std(method_str(), fdesc_str(), &sw, &sr, &srr, &srw);
        if (o_hist && hr.n > 0) {
            sb_html_disk_lat(0, hr.min_s * 1000.0, sb_hist_pct(&hr, 0.50) * 1000.0,
                             sb_hist_pct(&hr, 0.90) * 1000.0,
                             sb_hist_pct(&hr, 0.99) * 1000.0, hr.max_s * 1000.0);
            sb_html_disk_hist(0, &hr);
        }
        if (o_hist && hw.n > 0) {
            sb_html_disk_lat(1, hw.min_s * 1000.0, sb_hist_pct(&hw, 0.50) * 1000.0,
                             sb_hist_pct(&hw, 0.90) * 1000.0,
                             sb_hist_pct(&hw, 0.99) * 1000.0, hw.max_s * 1000.0);
            sb_html_disk_hist(1, &hw);
        }
        html_flush();
    }
    return 0;
}

static int sweep_run()
{
    static long kbs[] = { 1, 2, 4, 8, 16, 32, 64 };
    int i, n = (int)(sizeof(kbs) / sizeof(kbs[0]));
    double secs = o_secs_set ? o_secs : 4.0;
    res w, r, rr, rw;
    char line[160], ws[16], rws[16];
    char *rows[7];
    char rowbuf[7][80];
    unsigned long hkb[7];
    double hmed[7][4], hiops[7];

    fprintf(stderr, "sbdos: block-size sweep, %.0fs per cell\n", secs);
    for (i = 0; i < n; i++) {
        long bs = kbs[i] * 1024L;
        fprintf(stderr, " [%d/%d] block %ld KB\n", i + 1, n, kbs[i]);
        w.ok = rw.ok = 0;
        if ((!o_raw || o_rewrite) &&
            run_test(T_WRITE, bs, secs, (sb_hist *)0, &w, "seq write") != 0) return -1;
        if (run_test(0, bs, secs, (sb_hist *)0, &r, "seq read") != 0) return -1;
        if (run_test(T_RAND, bs, secs, (sb_hist *)0, &rr, "rand read") != 0) return -1;
        if ((!o_raw || o_rewrite) &&
            run_test(T_RAND | T_WRITE, bs, secs, (sb_hist *)0, &rw, "rand write") != 0) return -1;
        if (w.ok) sprintf(ws, "%8.2f", w.mbps); else strcpy(ws, "       -");
        if (rw.ok) sprintf(rws, "%8.2f", rw.mbps); else strcpy(rws, "       -");
        sprintf(rowbuf[i], "  %8ld %s %8.2f %8.2f %s %10.1f\n",
                kbs[i], ws, r.mbps, rr.mbps, rws, rr.iops);
        rows[i] = rowbuf[i];
        hkb[i] = (unsigned long)kbs[i];
        hmed[i][0] = w.ok ? w.mbps : -1.0;
        hmed[i][1] = r.mbps;
        hmed[i][2] = rr.mbps;
        hmed[i][3] = rw.ok ? rw.mbps : -1.0;
        hiops[i] = rr.iops;
    }
    stamp(1);
    out("sunbench -- disk block-size sweep\n");
    sprintf(line, "  method : %s\n", method_str());
    out(line);
    if (o_raw)
        sprintf(line, "  file   : BIOS drive %02Xh  (%lu MB, %.0fs per cell)\n",
                o_drive, g_total / 2048L, secs);
    else
        sprintf(line, "  file   : %s  (%ld MB, %.0fs per cell)\n", o_file, o_size_mb, secs);
    out(line);
    sprintf(line, "  %8s %8s %8s %8s %8s %10s\n",
            "blk(KB)", "seqW", "seqR", "randR", "randW", "rndR-IOPS");
    out(line);
    for (i = 0; i < n; i++)
        out(rows[i]);
    if (logf) fflush(logf);
    sb_html_disk_sweep(method_str(), fdesc_str(), 1, hkb, hmed, hiops, n);
    html_flush();
    return 0;
}

/* --- cpu: sunbench's kernels, unchanged ---------------------------------- */
static void cpu_line(k, name, ips)
    int k; char *name; double ips;
{
    char line[160], *p;
    unsigned long mhz = o_mhz;

    p = line;
    p += sprintf(p, "  %-11s", name);
    if (k == SBK_DHRYSTONE)
        p += sprintf(p, " %12.0f Dhrystones/s  %7.2f VAX-MIPS", ips, ips / 1757.0);
    else if (k == SBK_WHETSTONE)
        p += sprintf(p, " %12.2f MWIPS", ips * WHET_INSTR_PER_LOOP / 1.0e6);
    else if (SBK_IS_XPY(k))
        p += sprintf(p, " %12.2f MFLOPS", ips * sb_cpu_xpy_flops(k) / 1.0e6);
    else
        p += sprintf(p, " %12.3f Miter/s", ips / 1.0e6);
    if (mhz && SBK_IS_XPY(k))
        p += sprintf(p, "   %8.4f flop/clk",
                     ips * sb_cpu_xpy_flops(k) / ((double)mhz * 1.0e6));
    else if (mhz && k != SBK_DHRYSTONE && k != SBK_WHETSTONE)
        p += sprintf(p, "   %8.4f/clk", ips / ((double)mhz * 1.0e6));
    sprintf(p, "\n");
    out(line);
}

static int cpu_run()
{
    double secs = o_secs_set ? o_secs : 4.0;
    double rates[SBK_NKERNELS];
    sb_cpu_result r;
    char line[160];
    int k;

    fprintf(stderr, "sbdos: cpu kernels, %.0fs each%s\n", secs,
            has_fpu && !getenv("NO87") ? ""
            : " (emulated floating point: those kernels are slow)");
    for (k = 0; k < SBK_NKERNELS; k++) {
        rates[k] = -1.0;
        fprintf(stderr, "  %-11s ", sb_cpu_kernel_name(k));
        if (sb_cpu_run(k, secs, &r, (sb_reporter *)0) != 0 || !r.ok) {
            fprintf(stderr, "skipped: the streaming variant needs arrays over 64 KB;\n"
                            "              the plain one above is the measurement\n");
            continue;
        }
        rates[k] = r.iters_per_s;
        fprintf(stderr, "done\n");
    }
    stamp(0);
    out("sunbench -- cpu\n");
    sprintf(line, "  build  : %s\n", sb_cpu_build_id());
    out(line);
    if (o_mhz) {
        sprintf(line, "  clock  : %lu MHz\n", o_mhz);
        out(line);
    }
    for (k = 0; k < SBK_NKERNELS; k++)
        if (rates[k] >= 0.0)
            cpu_line(k, (char *)sb_cpu_kernel_name(k), rates[k]);
    if (logf) fflush(logf);
    sb_cpu_release();                     /* the ?axpy vectors back to DOS */
    sb_html_cpu((char *)sb_cpu_build_id(), o_mhz, 1, rates);
    html_flush();
    return 0;
}

/* --- memory: sunbench's working-set sweep, 1 KB up ------------------------
 * Up to 512 KB, as far as free conventional memory allows (256 KB on a
 * typical 640 KB machine): past the 64 KB external caches of the 386
 * PS/2s, so the cache shows as a step and DRAM as the plateau after it.
 * Sets over 64 KB are walked a segment at a time (bench_mem.c,
 * SB_SEGMENTED).  The working set is all free DOS memory, taken for the
 * sweep and given back (see "DOS memory" above). */
#define MEM_N 10

static void mem_row(kb, v)
    unsigned long kb; double *v;
{
    char line[120], *p;
    int k;
    p = line;
    p += sprintf(p, "  %8lu", kb);
    for (k = 0; k < SBMEM_NKERNELS - 1; k++) {
        if (v[k] < 0.0) p += sprintf(p, " %8s", "-");
        else            p += sprintf(p, " %8.1f", v[k]);
    }
    if (v[SBMEM_LAT] < 0.0) sprintf(p, " %9s\n", "-");
    else                    sprintf(p, " %9.1f\n", v[SBMEM_LAT]);
    out(line);
}

static int mem_run()
{
    static unsigned long kb[MEM_N] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };
    double val[MEM_N][SBMEM_NKERNELS];
    double cell_s = o_secs_set ? o_secs : 0.5;
    sb_mem_result r;
    char line[160];
    int i, k, n;
    unsigned avail, seg;
    unsigned long pool_kb;
    char far *pool;

    avail = dos_largest();
    pool_kb = avail > MEM_HEADROOM + 0x100
              ? (unsigned long)(avail - MEM_HEADROOM - 0x100) / 64L : 0L;
    for (n = 0; n < MEM_N && kb[n] <= pool_kb; n++)
        ;
    if (n == 0 ||
        _dos_allocmem((unsigned)(kb[n - 1] * 64L + 0x100), &seg) != 0) {
        fprintf(stderr, "sbdos: not enough free memory for the memory sweep\n");
        return -1;
    }
    pool = (char far *)MK_FP((seg + 0xFF) & 0xFF00, 0);   /* 4 KB aligned */
    if (n < MEM_N && kb[n - 1] < 256L)
        fprintf(stderr, "sbdos: only %lu KB of free memory: sweep stops at %lu KB "
                "(free conventional memory to go further)\n", pool_kb, kb[n - 1]);

    fprintf(stderr, "sbdos: memory sweep, 1..%lu KB, %.1fs per cell\n",
            kb[n - 1], cell_s);
    for (i = 0; i < n; i++) {
        fprintf(stderr, "  [%d/%d] %lu KB\n", i + 1, n, kb[i]);
        for (k = 0; k < SBMEM_NKERNELS; k++) {
            if (sb_mem_run(k, pool, kb[i] * 1024L, cell_s, &r) != 0 || !r.ok)
                val[i][k] = -1.0;
            else
                val[i][k] = (k == SBMEM_LAT) ? r.ns_per_load : r.mb_per_s;
        }
    }
    stamp(0);
    out("sunbench -- memory (working-set sweep)\n");
    sprintf(line, "  set    : %lu KB .. %lu KB, %.1fs per kernel-cell\n",
            kb[0], kb[n - 1], cell_s);
    out(line);
    sprintf(line, "  %8s %8s %8s %8s %8s %9s\n",
            "set(KB)", "read", "read8", "write", "copy", "lat(ns)");
    out(line);
    sprintf(line, "  %8s %8s %8s %8s %8s %9s\n", "", "MB/s", "MB/s", "MB/s", "MB/s", "");
    out(line);
    for (i = 0; i < n; i++)
        mem_row(kb[i], val[i]);
    if (logf) fflush(logf);
    _dos_freemem(seg);
    sb_html_mem(kb, val, n);
    html_flush();
    return 0;
}

/* --- main --------------------------------------------------------------- */
static char *help[] = {
    "usage: SBDOS [options] disk|sweep|full|cpu|mem|net|all ...",
    "       SBDOS --help | -v   (this help, paged | just the version)",
    "  disk           standard run: seq write/read, rand read/write (64 KB)",
    "  sweep          block-size sweep, 1..64 KB",
    "  full           disk, then sweep",
    "  cpu            sunbench's cpu kernels: Dhrystone, Whetstone, ...",
    "  mem            memory bandwidth/latency sweep, 1 KB up to 256-512 KB",
    "  net            UDP round trip and up/down sweeps against sunbench-netd",
    "  all            full, cpu and mem (and net, with --server)",
    "                 (raw mode is read-only: add --rewrite for write tests)",
    "raw mode (default) -- BIOS INT 13h on a whole drive:",
    "  --drive N      BIOS drive, 80 (hex) = first hard disk  (default 80)",
    "  --rewrite      also time writes, by re-writing data just read back",
    "                 to the same sectors.  Non-destructive by design, but",
    "                 use a scratch disk image on experimental hardware.",
    "  --maxsec N     sectors per INT 13h call (default 64, max 127)",
    "file mode -- DOS file I/O on a scratch file:",
    "  --file PATH    test file (e.g. C:\\SBDOS.DAT); selects file mode",
    "  --size MB      test file size (default 32, capped at half free space)",
    "common:",
    "  --block KB     block size for 'disk' (default 64)",
    "  --time SEC     seconds per test (default 8; sweep 4; cpu 4; mem 0.5)",
    "  --log FILE     append the report to FILE (sunbench log format)",
    "  --html FILE    also write an HTML report (rewritten after each test)",
    "  --mhz N        CPU clock for per-clock figures (PS/2s: from model)",
    "  --note TEXT    note line for the log",
    "  --host NAME    machine name for the log (default dos)",
    "  --model TEXT   override the detected model name",
    "  --nohist       skip latency histograms",
    "net -- through a packet driver; IP settings from mTCP's MTCPCFG file:",
    "  --server IP    the machine running sunbench-netd (required)",
    "  --port N       its port (default 5202)",
    "  --pktint N     packet driver interrupt, hex (default: MTCPCFG, or scan)",
    "  --ip, --mask, --gw A.B.C.D   override MTCPCFG's addresses",
    "  --mtu N        interface MTU (default MTCPCFG, or 1500)",
    (char *)0
};

/* Text rows on screen: the BIOS keeps rows-1 at 0040:0084 on EGA/VGA
 * (every PS/2); older adapters leave it 0, meaning the usual 25. */
static int screen_rows()
{
    unsigned char r = *(unsigned char __far *)MK_FP(0x40, 0x84);
    return (r >= 24 && r < 60) ? r + 1 : 25;
}

/* Help goes to stdout so "SBDOS --help > HELP.TXT" and "| MORE" work
 * (DOS can't redirect stderr).  On the console it pages itself: a
 * screenful, then "-- more --" (any key; Esc or q stops). */
static void usage(int rc)
{
    int i, n = 0, page = isatty(1) ? screen_rows() - 1 : 0, c;
    printf("sbdos %s -- sunbench for MS-DOS\n", SB_VERSION);
    n++;
    for (i = 0; help[i]; i++) {
        if (page && n == page) {
            printf("-- more -- (any key; Esc/q to stop)");
            fflush(stdout);
            c = getch();
            if (c == 0 || c == 0xE0) (void)getch();   /* extended key */
            printf("\r                                   \r");
            if (c == 27 || c == 'q' || c == 'Q') break;
            n = 0;
        }
        printf("%s\n", help[i]);
        n++;
    }
    exit(rc);
}

int main(argc, argv)
    int argc; char **argv;
{
    int i, what = 0, rc = 0;
    char *a;

    for (i = 1; i < argc; i++) {
        a = argv[i];
        if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("%s\n", SB_VERSION);
            return 0;
        }
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 ||
                 strcmp(a, "-?") == 0 || strcmp(a, "/?") == 0 ||
                 strcmp(a, "/h") == 0 || strcmp(a, "/H") == 0) usage(0);
        else if (strcmp(a, "disk") == 0) what |= 1;
        else if (strcmp(a, "sweep") == 0) what |= 2;
        else if (strcmp(a, "full") == 0) what |= 3;
        else if (strcmp(a, "cpu") == 0) what |= 4;
        else if (strcmp(a, "mem") == 0) what |= 8;
        else if (strcmp(a, "all") == 0) what |= 15 | 32;
        else if (strcmp(a, "net") == 0) what |= 16;
        else if (strcmp(a, "--rewrite") == 0) o_rewrite = 1;
        else if (strcmp(a, "--nohist") == 0) o_hist = 0;
        else if (i + 1 < argc && strcmp(a, "--drive") == 0) o_drive = (int)strtol(argv[++i], (char **)0, 16);
        else if (i + 1 < argc && strcmp(a, "--file") == 0) { o_file = argv[++i]; o_raw = 0; }
        else if (i + 1 < argc && strcmp(a, "--size") == 0) o_size_mb = atol(argv[++i]);
        else if (i + 1 < argc && strcmp(a, "--block") == 0) o_block_kb = atol(argv[++i]);
        else if (i + 1 < argc && strcmp(a, "--time") == 0) { o_secs = atof(argv[++i]); o_secs_set = 1; }
        else if (i + 1 < argc && strcmp(a, "--maxsec") == 0) o_maxsec = atoi(argv[++i]);
        else if (i + 1 < argc && strcmp(a, "--log") == 0) o_log = argv[++i];
        else if (i + 1 < argc && strcmp(a, "--html") == 0) o_html = argv[++i];
        else if (i + 1 < argc && strcmp(a, "--mhz") == 0) o_mhz = (unsigned long)atol(argv[++i]);
        else if (i + 1 < argc && strcmp(a, "--note") == 0) o_note = argv[++i];
        else if (i + 1 < argc && strcmp(a, "--host") == 0) o_host = argv[++i];
        else if (i + 1 < argc && strcmp(a, "--model") == 0) o_model = argv[++i];
        else if (i + 1 < argc && sbnet_opt(a, argv[i + 1])) i++;
        else {
            fprintf(stderr, "sbdos: unknown option or command '%s' (SBDOS --help)\n", a);
            return 2;
        }
    }
    if (what == 0) usage(2);
    if ((what & 32) && sbnet_ready()) what |= 16;  /* all: net if --server */
    if ((what & 16) && !sbnet_ready()) {
        fprintf(stderr, "sbdos: net needs --server A.B.C.D (the machine running sunbench-netd)\n");
        return 2;
    }
    if (o_block_kb < 1 || o_block_kb > 64) {
        fprintf(stderr, "sbdos: --block must be 1..64 KB\n");
        return 2;
    }
    if (o_maxsec < 1 || o_maxsec > 127) o_maxsec = 64;
    if (o_drive < 0x80) o_drive += 0x80;    /* "--drive 0" means 80h */

    heap_reserve();                       /* first: see "DOS memory" */
    probe_system();
    if (o_log) {
        logf = fopen(o_log, "a");
        if (!logf) fprintf(stderr, "sbdos: cannot write %s\n", o_log);
    }
    if (what & 3) {
        if (disk_buf_get() != 0) return 1;
        if (o_raw && raw_geometry() != 0) return 1;
        if (!o_raw && file_prep() != 0) return 1;
        disk_ready = 1;
    }
    if (o_html) {
        sb_html_enable(o_html, "");
        if (o_note) sb_html_note(o_note);
    }
    fprintf(stderr, "sbdos %s: %s%s\n", SB_VERSION, cpu_str(),
            o_mhz ? "" : " (clock unknown: --mhz N for per-clock figures)");
    if ((what & 3) && o_raw)
        fprintf(stderr, "sbdos %s: BIOS drive %02Xh, %u/%u/%u CHS, %lu MB%s\n",
                SB_VERSION, o_drive, g_cyls, g_heads, g_spt, g_total / 2048L,
                o_rewrite ? ", rewrite tests ON" : ", read-only");
    if ((what & 3) && o_raw && !o_rewrite)
        fprintf(stderr, "sbdos: NOTE -- no write tests: add --rewrite (raw) or use --file\n");

    rng = (unsigned long)time((time_t *)0) | 1L;
    signal(SIGINT, on_break);
#ifdef SIGBREAK
    signal(SIGBREAK, on_break);       /* Ctrl-Break: restore the 8254 too */
#endif
    atexit(pit_restore);
    pit_mode2();

    if ((what & 1) && std_run() != 0) rc = 1;
    if (rc == 0 && (what & 2) && sweep_run() != 0) rc = 1;
    if (fd >= 0) { close(fd); (void)unlink(o_file); fd = -1; }
    disk_buf_put();                       /* disk done: memory back to DOS */
    if (rc == 0 && (what & 4) && cpu_run() != 0) rc = 1;
    if (rc == 0 && (what & 8) && mem_run() != 0) rc = 1;
    if (rc == 0 && (what & 16) && sbnet_run() != 0) rc = 1;

    pit_restore();
    if (fd >= 0) { close(fd); (void)unlink(o_file); }
    if (logf) {
        fclose(logf);
        fprintf(stderr, "sbdos: report also written to %s\n", o_log);
    }
    return rc;
}

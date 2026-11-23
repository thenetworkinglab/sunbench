# Changelog

Versioning is CalVer: `YYYY.MM.DD-N`, month and day zero-padded, N counting
that day's releases from 1 — `2026.11.22-1` is the first release of
22 November 2026, `2026.11.22-2` the second that day, `2026.12.05-1` the
first of 5 December 2026.

Releases before 22 November 2026's fourth used `YYYY.MDD.PATCH` (`2026.719.0`
= 19 July, first that day; `2026.1122.2` = 22 November, third that day).
That read ambiguously between months 1 and 11/12 (`2026.111.0`: 11 January
or 1 November?), so it was dropped.

## v2026.11.22-4 — 22 November 2026 (first public release)

- Licensed under the GNU GPL v3 or later: LICENSE (full text), NOTICE,
  and a copyright/SPDX header on every sunbench source file.
- src/whet.c now carries Painter Engineering's original Whetstone comment
  block unaltered, as its terms require; it stays under those terms.
- New version scheme, `YYYY.MM.DD-N` (see the top of this file).
- README: MS-DOS (SBDOS) section and License section; example server
  address is now the documentation address 192.0.2.10.

## v2026.1122.2 — 22 November 2026

- SBDOS memory line: "(XMS: HIMEM.SYS owns it)" now reads
  "(XMS, via HIMEM.SYS)".
- HTML report: the CPU table's "unit" heading is left-aligned to match
  its column.

## v2026.1122.1 — 22 November 2026

- SBDOS `-v` / `--version` prints just the version number.
- SBDOS help (`--help`, `-h`, `/?`, or no command) now goes to stdout, so
  `SBDOS --help | MORE` and `> HELP.TXT` work, and on the console it
  pages itself a screenful at a time ("-- more --"; Esc or q stops).
  An unknown option now gets a one-line error instead of the whole help.

## v2026.1122.0 — 22 November 2026

- SBDOS: the `memory :` line said "0 KB extended" whenever HIMEM.SYS
  was loaded.  HIMEM owns all extended memory and makes the BIOS's
  INT 15h AH=88h report 0, so SBDOS now asks it through XMS (function
  08h, dos/xms.asm) when the BIOS answers 0, and says so:
  "N KB extended free (XMS: HIMEM.SYS owns it)".  That is free memory:
  with DOS=HIGH it excludes the 64 KB HMA.  Tested under qemu with MS-DOS
  6.22's HIMEM.SYS.

## v2026.1121.6 — 21 November 2026

- SBDOS --rewrite: each timed RANDOM write now follows an untimed
  one-sector read from another random place.  Written straight after its
  own read-back, the block was still in the drive's or controller's
  buffer, and on a PS/2 Model 70 with a difnif DBA-ESDI emulator 1 KB
  random writes measured 1.04 MB/s against 0.49 for random reads.  The
  sequential write test keeps its read-then-write pairs.  Still
  non-destructive (under qemu, a random-filled disk's checksum was
  unchanged after a run).
- SBDOS memory: every big buffer now comes from DOS only while its own
  test runs: the disk buffer, cpu.c's 120 KB of ?axpy vectors (DOS build
  only: `SB_SEGMENTED` hooks sb_seg_alloc/sb_seg_free and
  sb_cpu_release; Unix keeps its static arrays), the network receive
  ring, and the memory sweep's working set.  SBDOS.EXE is 136 KB (was
  287 KB), and the memory sweep reaches 256 KB with a packet driver
  loaded, where it had stopped at 128 KB.  The C heap is grown once at
  startup, so its blocks never split free memory.
- Fixed before release, found under qemu: the 32 KB receive-ring
  request was computed in a 16-bit int (16 x 2048 overflows) and asked
  DOS for ~1 MB.

## v2026.1121.5 — 21 November 2026

### New: SBDOS net — UDP network tests from MS-DOS, through a packet driver

- `SBDOS net --server A.B.C.D`: sunbench's UDP round trip and UDP
  upload/download sweeps (64 B to the MTU) against sunbench-netd, with
  the same method as the Unix client: upload over the server's arrival
  span, download paced to 1.5x the upload rate and halved while loss is
  over 40%, rate over our own arrival span.  `all` includes it when
  --server is given.  TCP is not measured: there is no TCP stack.
- Its own minimal stack (dos/pktnet.c): the PC/TCP packet driver
  interface, ARP (asking and answering), IPv4 and UDP; the receive
  upcall is 85 bytes of assembly (dos/rxup.asm) filling a 16-slot ring
  that the C code polls.  The log reports frames the ring had to refuse.
  Class 1 (Ethernet) drivers: Crynwr 3C523, SMC_WD/WD8003E, NE2000.
  Token ring (class 3, e.g. IBMTOKEN) is next; the link header is built
  and parsed in one place each to make room for it.
- IP settings come from mTCP's config file (MTCPCFG: PACKETINT, IPADDR,
  NETMASK, GATEWAY, MTU, including what mTCP's DHCP wrote); --pktint,
  --ip, --mask, --gw and --mtu override.  New `net :` synopsis line.
- sunbench-netd: a UDP control channel on the same port (5202), for
  clients with no TCP: PING/PONG, UUP/UEND, UDN/URES, each carrying a
  client id so any lost datagram can simply be repeated (netproto.h).
  Download data goes out from the control port, so NAT and minimal
  stacks see replies from the port they sent to.  TCP clients unchanged.
- html.c: sb_html_net_rtt_label(), so the DOS report says "UDP round
  trip"; sunbench-fleet.py reads either kind of round trip.
- Tested under qemu with an emulated NE2000 and Crynwr NE2000.COM
  against netd on the Mac, end to end.  A first run caught kbit/s
  overflowing DOS's 16-bit int (40,800 kbit/s became the 64 floor);
  fixed with long.  Not yet run on real hardware.

## v2026.1121.4 — 21 November 2026

- SBDOS mem: the read, read8 and write kernels index with a 16-bit
  unsigned in the DOS build (bench_mem.c `sb_idx`; still unsigned long
  everywhere else).  A pass never exceeds one 64 KB segment there, and
  the 32-bit index cost more cycles than the load it addressed: on a
  PS/2 Model 70 read fell only 3.8 -> 3.2 MB/s past the 64 KB cache.
  The inner loop is now plain adds through one pointer register.  DOS
  read/write figures from earlier builds are not comparable.
- SBDOS cpu: honours Watcom's `NO87` variable (`SET NO87=1` forces the
  x87 emulator with a coprocessor fitted), and the `cpu :` line says so.
  Under qemu the floating-point kernels ran ~10x slower with it set, so
  without it they really are on the coprocessor.

## v2026.1121.3 — 21 November 2026

### SBDOS mem goes past 64 KB: to 256 KB on a 640 KB machine (512 if free)

- The 386 PS/2s have 64 KB external caches, so a sweep ending at 64 KB
  never left the cache.  Sets over 64 KB are now walked one 64 KB segment
  at a time, each by the same kernel (bench_mem.c `SB_SEGMENTED`, set only
  by the DOS build; on Unix, MEM_AT is the same pointer arithmetic as
  before).  Under qemu the 128 and 256 KB bandwidths match 64 KB's, so
  walking by segment costs nothing measurable.
- SBDOS takes its DOS memory as one pool at startup (all free memory less
  40 KB for the C heap) and shares it between the disk buffer and the
  memory sweep.  A separate disk buffer, with heap blocks above it,
  fragmented ~300 KB of free memory into pieces too small for 256 KB.
  The sweep stops at the largest size that fits and says so when that is
  under 256 KB.

## v2026.1121.2 — 21 November 2026

- SBDOS: says plainly at startup when a raw-mode run has no write tests
  (add --rewrite, or use --file), and the help notes it under `all`.
- SBDOS cpu: the skip message for the streaming saxpy/daxpy pair now
  says the plain pair is the measurement.

## v2026.1121.1 — 21 November 2026

### New: SBDOS.EXE runs the cpu and memory benchmarks, and writes HTML

- `SBDOS cpu`: sunbench's own CPU kernels — int alu, int muldiv,
  fp double, saxpy/daxpy, Dhrystone 2.1, Whetstone — from src/cpu.c,
  dhry_1.c, dhry_2.c and whet.c compiled unchanged, with the same log
  lines (VAX-MIPS, MWIPS, MFLOPS, per-clock).  The streaming
  saxpy/daxpy pair is skipped: its vectors are bigger than one 64 KB
  segment.  8086 code with int 16 bits wide, the way PC Dhrystones of
  the era were compiled.  Machines without a coprocessor run the
  floating-point kernels on the linked x87 emulator; the log says which.
- `SBDOS mem`: sunbench's working-set sweep (src/bench_mem.c: read,
  read8, write, copy, pointer-chase latency) over 1-64 KB — one segment,
  which covers every cache these machines have.
- `SBDOS all`: full disk, then cpu, then mem.
- `--html FILE`: src/html.c's self-contained report (Machine card,
  disk tables and histograms, sweep, CPU and memory charts), rewritten
  after each test like sunbench's.
- New `cpu :` synopsis line: 8086/286/386/486 found from the FLAGS
  register, and the coprocessor from the BIOS equipment word.  The clock
  comes from the PS/2 model table, or `--mhz N`, for per-clock figures.
- Shared sources: cpu.c and bench_mem.c put their fork()-based MP code
  under `#ifndef SB_NOFORK` (defined only by the DOS build), and dhry.h
  gives Open Watcom the char prototype for Func_1.  ACK needs the
  promoted int there, and Watcom rejects it.  Unix builds are unchanged.
- Tested under qemu (MS-DOS 6.22): every mode, the HTML report, and
  sunbench-fleet.py reading the DOS logs.  After a `--rewrite`
  full run on a random-filled 32 MB disk, the disk's checksum was
  unchanged.  Not yet run on PS/2 hardware.

## v2026.1121.0 — 21 November 2026

### New: SBDOS.EXE — sunbench's disk benchmark for MS-DOS

- For benchmarking PS/2 drives and drive emulators (first target: a
  DBA-ESDI emulator in a Model 55SX and Model 70) under DOS, where the
  previous tool, DiskMeter, gives only averages.  16-bit real mode, 8086
  code, x87 emulation linked — runs on anything from a PS/2 Model 25 up,
  including the 286 Model 50Z.  Cross-compiled on the Mac with Open Watcom
  v2 (`make dos` / dos/build.sh; Watcom's macOS arm64 host binaries come
  from its ow-snapshot release).
- Two layers: raw BIOS INT 13h sector I/O on a whole drive (default;
  read-only unless --rewrite, which times writes of data just read back
  to the same sectors), and DOS file I/O on a scratch file (--file).
  Standard run (seq write/read, rand read/write) and a 1-64 KB block-size
  sweep, with sunbench's latency histograms and percentiles.
- Timing reads the 8254 timer counter directly (channel 0 switched to
  mode 2 at the same divisor, so the BIOS tick is unchanged; restored on
  exit, Ctrl-C and Ctrl-Break) for 0.84 us resolution, against DOS's
  18.2 Hz tick.  I/O uses a 64 KB buffer aligned to a 64 KB physical page
  so no BIOS/DMA transfer straddles a page; INT 13h calls never cross a
  track; SMARTDRV, which caches INT 13h itself, is reset before every test
  in both modes (sunbench's purge) and noted in the log when present.
- Reuses src/histogram.c unchanged (dos/dosport.h stands in for port.h,
  keeping the host's config.h out of the DOS build); logs use sunbench's
  format, so sunbench-fleet.py reads them.  PS/2 model detected from the
  BIOS configuration table (INT 15h C0h).
- Compiles warning-free at -w4.  Raw reads verified on a PS/2 Model
  55SX with a DBA-ESDI emulator; write tests not yet run on hardware.

## v2026.1116.0 — 16 November 2026

### Fix: UDP download overstated throughput when pacing outran the link

- The mirror image of v2026.926.0's upload fix.  The download sweep
  divided every byte the client received by the SERVER's send window,
  but when the pacing (1.5x the upload result) exceeds the link, the
  switch queues the surplus and keeps delivering it after the server
  stops — and the client's drain counted it.  A SPARCstation LX on
  10 Mbit Ethernet reported 9.61 and 9.76 Mbit/s of UDP payload at
  1024 and 1472 bytes, above the ~9.39 and ~9.57 ceilings.  The client
  now timestamps first and last arrival (sb_now: gethrtime on Solaris,
  cheap per datagram) and rates over that span (n arrivals, n-1 gaps),
  falling back to the send window when too few arrive.  Client-only:
  netd and the protocol are unchanged.  Affected runs: any cell where
  the paced rate exceeded the link — mainly larger datagrams on 10 Mbit
  machines, by a few percent; loss figures were always correct.

## v2026.929.0 — 29 September 2026

### Fix: macOS disk tests were served from RAM (fcntl F_NOCACHE)

- macOS has neither directio nor posix_fadvise, and msync(MS_INVALIDATE)
  does not evict there, so the block-size sweep's fixed 256 MB file sat
  in the unified buffer cache: an M4 Mac mini "read" up to 24 GB/s and
  805,000 random 4 KB reads/s from a ~3 GB/s SSD.  (Its standard run,
  with a 40 GB file on 16 GB of RAM, was honest.)  New probe
  HAVE_F_NOCACHE: where directio is absent, fcntl(F_NOCACHE) is probed
  on the scratch filesystem and, if it works, used as the directio
  method ("F_NOCACHE (page-cache bypass)", fixed 512 MB file, no purge
  needed).  Verified on an M2 MacBook Air: sweep reads now 340-1,810
  MB/s, and 64 KB random reads (7,516/s) match the M4's uncached
  large-file figure (7,766/s).  Caveat: macOS fsync does not flush the
  SSD's own cache (F_FULLFSYNC would), so write figures include it —
  like a SCSI disk with its write cache enabled.

### Memory sweep: default ceiling raised from 16 MB to 256 MB

- The 16 MB cap cleared every SPARC E-cache, but modern last-level
  caches are 16-32 MB: the M4 swept entirely inside its 16 MB L2 and
  reported "DRAM" at 95 GB/s and 11 ns.  Default is now min(256 MB,
  RAM/4); machines with 64 MB of RAM or less see no change, larger ones
  gain rows reaching true DRAM (M2: 104-108 ns from 128 MB up, after a
  76 ns transition at 32 MB).  The MP memory test's per-process buffers
  follow the same ceiling (still shrunk to fit RAM/2).  Help text, man
  page, README and CLAUDE.md updated.  Earlier logs' last sweep row is
  still DRAM for every machine whose caches were under 16 MB — i.e. the
  whole SPARC fleet.

## v2026.928.2 — 28 September 2026

### Build: configure keeps the K&R dialect compiling on modern compilers

- sunbench's K&R-style definitions (kept so SunOS 4's bundled cc can
  build it) draw a -Wdeprecated-non-prototype warning per function from
  modern clang (hundreds per build on macOS), and C23 removed the syntax
  outright, so a compiler defaulting to C23 (GCC 15) rejects the tree.
  configure now compiles a one-line K&R test first: if it fails, it
  retries with -std=gnu17 (and uses that for every later probe); if the
  compiler warns about it, it adds -Wno-deprecated-non-prototype.  Old
  compilers get neither flag.  The flags go to src/config.cflags, read
  by the Makefile's compile rule through shell backquotes — portable to
  every make back to SunOS 4, which has no include directive.
- Verified on macOS clang (warnings: hundreds -> 5 unrelated socklen_t
  ones) and against clang forced to -std=c23 as a stand-in for GCC 15
  (K&R test fails, configure adds -std=gnu17, all probes and the build
  succeed).

## v2026.928.1 — 28 September 2026

### macOS (and BSD): system, CPU, clock and filesystem detection

- An M2 MacBook Air run had no model line and no clock, so the fleet
  page showed a blank model (and exposed a fleet-script escaping bug,
  fixed separately).  New configure probe HAVE_SYSCTLBYNAME:
  - model (macOS): system_profiler's marketing name plus the hw.model
    identifier — "MacBook Air (Mac14,2)"; cached, since system_profiler
    takes ~0.5s and the header is printed per section.
  - cpu: machdep.cpu.brand_string with Apple Silicon's performance/
    efficiency split — "Apple M2 (4P + 4E cores)"; "N x <brand>" on
    multi-socket Intel Macs; FreeBSD's brand from hw.model.
  - clock: hw.cpufrequency on Intel Macs, dev.cpu.0.freq on FreeBSD.
    Apple Silicon exposes no clock at all; that stays unknown (--mhz).
- disk: filesystem type and device from statfs's f_fstypename and
  f_mntfromname where statvfs lacks them — "apfs on /dev/disk3s5" on
  macOS, and NetBSD gains the device name too.
- Verified on the M2 MacBook Air itself.

## v2026.928.0 — 28 September 2026

### Linux: system, CPU, clock and filesystem detection

- First Linux run (Raspberry Pi 4) confirmed the posix_fadvise purge
  works — 503 MB file on a 7.7 GB machine, yet every read (standard run
  and the 256 MB sweep) at 30-35 MB/s, under the SD bus's ~45 — but its
  header had no model or cpu line, no filesystem type or device, and
  needed --mhz: all of those came from Solaris-only sources.  New plain
  file-read fallbacks (no configure probes; absent files leave the line
  blank, as before):
  - model: /proc/device-tree/model on ARM boards ("Raspberry Pi 4 Model
    B Rev 1.4"), else DMI vendor + product on PCs.
  - cpu: /proc/cpuinfo — "2 x <brand>" by distinct physical id on x86;
    on ARM the core is named from its part number ("ARM Cortex-A72 (4
    cores)"), since 32-bit Pi kernels call every core "ARMv7 Processor"
    and 64-bit ones give no model name at all.
  - clock: cpufreq's cpuinfo_max_freq (the rated speed, not a throttled
    current one), else /proc/cpuinfo "cpu MHz" — --mhz no longer needed.
  - disk: filesystem type and device from /proc/mounts, which also
    makes the memory-filesystem guard catch tmpfs on Linux.
  Tested against Pi 4 (32- and 64-bit kernel) and dual-socket Xeon
  sample trees.  The drive-model line stays blank on Linux for now.

## v2026.926.2 — 26 September 2026

### Fix: oversized --size wrapped to zero on 32-bit builds

- The test file size in bytes is an unsigned long, and sunbench is not
  built for large files (32-bit off_t), so a 32-bit build's real ceiling
  is 2047 MB.  `--size 8192` on a Sun Fire X4150 (32-bit gcc build)
  computed 8192 * 1048576 = 2^33, which wrapped to 0 and failed with a
  misleading "file too small for 65536-byte blocks".  An explicit --size
  beyond the build's limit is now refused with a clear message pointing
  at a 64-bit rebuild; the automatic RAM-based size (2.5x RAM without
  directio) is capped to the limit instead of wrapping silently — a
  latent bug for any big-memory machine running a 32-bit build.

## v2026.926.1 — 26 September 2026

### Fix: netd's UDP receive loop was dropping small packets itself

- v2026.926.0's arrival timestamps did their job at 1472 bytes (X2200 M2:
  988.7 -> 940.7 Mbit/s, now under gigabit's ~957 payload ceiling), but
  netd then read the clock on EVERY datagram, on top of the per-datagram
  clock read its safety timeout already did and a select() per datagram.
  At gigabit packet rates that made netd the bottleneck: 1024-byte loss
  went 4.1% -> 42.1%.  netd now makes the UDP socket non-blocking and
  drains everything queued per wakeup, with one clock read per batch
  (first-arrival time taken on the first wakeup).  Fewer wakeups than
  even the pre-926.0 loop, so small-packet server-side loss should now
  be LOWER than before either change.  Results from netd 926.0 at
  sub-1472-byte sizes should be discarded.  Client unchanged; rebuild
  and restart sunbench-netd only.

## v2026.926.0 — 26 September 2026

### x86 Solaris: real system and CPU names in the header

- On x86 Solaris, sysinfo(SI_PLATFORM) answers "i86pc" for every
  machine, so the header, CSV and fleet page could not tell an X2200 M2
  from anything else.  sb_model() now falls back to prtdiag's "System
  Configuration:" line there ("Sun Microsystems Sun Fire X2200 M2");
  SPARC platform names are unchanged.
- New `cpu :` header line from `psrinfo -pv` (Solaris 9+): the brand
  string on x86 ("2 x Quad-Core AMD Opteron(tm) Processor 2376"), the
  chip name on SPARC ("UltraSPARC-IIIi"), with an "N x" socket count on
  multi-chip machines and Intel's padded brand strings squeezed.  Absent
  on Solaris 8 and earlier, SunOS 4, NetBSD and Minix (no psrinfo -p).
  Parser tested against Opteron, Xeon and UltraSPARC samples.
- Fleet page shows the cpu line on each card and trims
  "Sun Microsystems " from the summary table's model column.

### Fix: UDP upload overstated throughput on senders faster than the wire

- UDP upload divided the bytes the server received by the CLIENT's send
  window.  A sender faster than the link is still draining its socket
  and NIC queues when its loop ends, and netd's drain counted those
  bytes too — the first gigabit-class machine (Sun Fire X2200 M2)
  reported 999 Mbit/s of UDP payload at 1472 bytes, above gigabit's
  ~957 ceiling.  netd now timestamps first and last arrival and adds the
  span as a third RES field; the client computes the rate over it
  (n arrivals span n-1 gaps).  Backward compatible both ways: old
  clients ignore the field, and new clients fall back to their own
  window against an old netd.  Every earlier fleet machine sent slower
  than its link, so their results were unaffected.  **Rebuild and
  restart sunbench-netd on the server** to get the corrected figure.
- Known and unchanged: at 64-256 bytes a gigabit sender outruns the
  server's receive path, so high UDP upload "loss" there measures the
  server, not the machine under test.

## v2026.925.0 — 25 September 2026

### Fix: disk cache purge on Linux (posix_fadvise)

- The SB_SIZE cold-cache purge used mmap+msync(MS_INVALIDATE), which
  Solaris and SunOS 4 honor but Linux compiles and then ignores for
  file pages — so on Linux, disk READ results would have rested on the
  2.5x-RAM file size alone (the safeguard the SS2 proved insufficient),
  and the block-size sweep's fixed 256 MB file would sit entirely in
  cache on any machine with a few GB of RAM.  New probe HAVE_FADVISE:
  where present, the purge now fsyncs the file and then calls
  posix_fadvise(POSIX_FADV_DONTNEED).  The fsync is essential —
  DONTNEED drops only clean pages, and the prep write leaves the file
  dirty.  Both mechanisms run where both exist; Solaris/SunOS behaviour
  is unchanged where posix_fadvise is absent.  Not yet validated on real
  Linux hardware (compile-checked only); first target: Sun x86 servers
  (X2200 M2, X4600-class).

## v2026.924.0 — 24 September 2026

### Port: Minix 2.0.4 (ACK compiler), first pass

- First build attempt on Minix 2.0.4 (DOSMINIX, IBM PS/2 Model 55SX,
  386SX-16) failed in six places; all addressed behind new configure
  probes, each with a stub per the probe rule:
  - **Timer** (`HAVE_GETTIMEOFDAY`): Minix 2 has no gettimeofday.
    Fallback is POSIX `times()` elapsed ticks (CLK_TCK, 60Hz).  New
    `sb_min_batch()` = max(50ms, 40 ticks) replaces the hard-coded 50ms
    calibration floor in the CPU and memory modules, so quantisation
    stays under ~2.5% on a tick clock; unchanged (50ms) everywhere with
    gettimeofday.  cpu.c now shares timer.c's clock instead of its own
    gettimeofday copy.
  - **Sockets** (`HAVE_SOCKETS`): Minix networking is /dev/tcp + ioctls,
    not BSD sockets.  The network module and the interface synopsis
    compile to stubs; `net` reports the module unavailable instead of
    breaking the build.  (A native /dev/tcp backend is future work.)
  - **valloc** (`HAVE_VALLOC`): falls back to malloc — callers free()
    these buffers, and page alignment only matters for directio.
  - **fsync** (`HAVE_FSYNC`, falls back to sync) and **readlink**
    (`HAVE_READLINK`), both probed for completeness.
  - **Headers** (`HAVE_MINIX`): Minix declares POSIX interfaces only
    under `_MINIX`/`_POSIX_SOURCE`; port.h now defines them there, before
    any system header, fixing implicit popen/fileno/kill declarations.
  - **Dialect** (new CLAUDE.md rule): Dhrystone's `Func_1` prototype
    declared `char` parameters for a K&R definition, whose parameters
    arrive promoted to `int` — a conflicting redeclaration that gcc
    tolerated and ACK rejects.  Prototype now says `int`; no effect on
    the benchmark (call sites always promoted anyway).
- Not a code issue: ACK's assembler ran out of memory on main.c; raise
  the assembler pass's memory allotment with Minix's `chmem`.

## v2026.922.1 — 22 September 2026

### Fix: UDP download collapse on machines that receive slower than they send

- The download sweep paces the server at 1.5x the machine's own upload
  result, assuming rx and tx are symmetric.  On the PS/2 Model 70's
  8-bit WD8003E/A (upload 6.9 Mbit/s, receive ceiling ~5) the server
  offered ~10 Mbit/s at wire rate, the card's 8 KB ring overflowed
  continuously, and the sweep reported 99% loss at 0.07 Mbit/s — a
  collapse artifact, not a throughput.  The client now halves the
  requested pace and reruns the cell while loss exceeds 40% (up to 4
  repacings, floor 64 kbit/s), so the column converges on the highest
  rate the receive path actually sustains.  Headers and the HTML
  caption say so.  Machines with symmetric paths converge on the first
  attempt and are unaffected; expect the sun4c Lance machines
  (norbufs starvation) to benefit alongside 8-bit MCA cards.

## v2026.922.0 — 22 September 2026

### Port: RAM size detection on 4.4BSD systems (NetBSD/i386)

- First non-Sun port (NetBSD 1.6.2 on an IBM PS/2 Model 70, 386DX-25)
  built and ran cleanly, but sb_physmem_mb() only knew Solaris's
  sysconf(_SC_PHYS_PAGES) and returned 0, so the disk test fell back
  to its 96 MB default file, the memory sweep needed an explicit
  --memsize, and CSV rows recorded ram_mb=0.  Added a sysctl
  (CTL_HW, HW_PHYSMEM) implementation behind a new configure probe
  (HAVE_SYSCTL_HW_PHYSMEM); Solaris/SunOS keep their existing paths.
  Results from runs before this fix are unaffected in accuracy (the
  oversized file is, if anything, more cache-proof) — only the
  recorded ram_mb and default sizing differ.

### Port: free-space cap on pre-statvfs systems (NetBSD 1.6, SunOS 4)

- The cap that shrinks the test file to half the free space relied on
  statvfs, which NetBSD lacked until 3.0 and SunOS 4 never had — so on
  both, sb_freespace_mb() returned 0 and the cap silently never fired.
  Harmless on big Sun disks; on a 350 MB PS/2 root the block-size
  sweep's fixed 256 MB file would have filled the filesystem.  Added a
  statfs() fallback behind two probes for the two header homes:
  HAVE_STATFS_MOUNT (4.4BSD, <sys/mount.h>) and HAVE_STATFS_VFS
  (SunOS 4, <sys/vfs.h>); statvfs still wins where present.  The
  4.4BSD flavor also carries f_fstypename, so the fs-type banner and
  the memory-filesystem guard (now also "mfs") work on NetBSD, and
  SunOS 4 gains the free-space cap it never had.

## v2026.919.1 — 20 September 2026

### Fix: network section vanished from --log on Solaris (stale fd close)

- The deadline machinery's cleanup registry kept a STALE socket number
  from sb_net_mtu (which closes its own probe socket and is not a
  wrapped test).  The log file, opened next, received that same fd
  number — and the first wrapped test's cleanup closed it out from
  under stdio.  Every subsequent log write failed silently; the
  "report also written" notice printed anyway.  Every Solaris
  net-with-log run since v2026.918.2 lost its network section (the
  SS1 and the Sep 18 SS2 runs); SunOS 4 was immune only because its
  MTU probe is a socketless stub.  Fixes: the MTU probe no longer
  registers with the cleanup, and every wrapper attempt forgets stale
  registrations before it starts, so cleanup can only touch sockets
  the current attempt created.  CSV and screen output were never
  affected.

## v2026.919.0 — 19 September 2026

### HTML histograms: bars grow from the left

- The latency-distribution bars reused the numeric tables' bar style,
  which anchors at the RIGHT edge (a bar behind a right-aligned value)
  — confusing for a histogram.  New left-anchored variant: bars now
  grow rightward from the bucket label with the count at the right,
  reading exactly like the ASCII version.

## v2026.918.2 — 18 September 2026

### Network: whole-test deadlines, retry-once, pacing cap

- On 10 Mbit half-duplex with broadcast collisions, TCP can back off
  into a stall that blocked the client FOREVER in connect/read/write —
  a Solaris 2.6 SS20 froze mid-upload needing Ctrl-C on both ends, and
  SunOS 4 / Solaris 2.3 / V240 runs showed the milder forms (absurd
  one-off cells, "-" rows).  Every client test now runs under
  alarm(2*secs+30) with longjmp cleanup (close sockets, fail the cell),
  and each failed test retries ONCE before "-" is recorded: a collision
  storm costs at most one marked cell, never a hang.  No protocol
  change — old netd and new client interoperate.
- netd: wall-clock bound inside the UP receive and UDP count loops (a
  trickling client can no longer ride past the alarm), and UDP-down
  pacing is capped at 950 Mbit/s so a gigabit client's 1.5x request can
  never ask the server to exceed the wire (the V240's failed 1472 row).

## v2026.918.1 — 18 September 2026

### HTML report: latency distribution histograms

- The random-read/-write latency histograms — the log's most
  diagnostic table (bimodal SCSI emulators, SD-card stalls, cache
  leaks all showed up there first) — now render in the HTML report's
  disk card as bar tables (bucket ms, CSS bar, count), right under the
  percentile summary, whenever `--hist` is on (`full` always is).
  Same trimmed log2 buckets as the ASCII version; new feeder
  `sb_html_disk_hist()`, new accessor `sb_hist_bucket_low_ms()`.

## v2026.918.0 — 18 September 2026

### size>RAM method: purge the page cache instead of outrunning it

- The 4 KB sweep cells on the Solaris 2.3 SS2 still read 5.4 MB/s
  through a 5.0 MB/s sync-SCSI bus — impossible, therefore cache.  The
  culprit is prep-write residue: the page scanner keeps parts of the
  file resident in patterns no LRU model predicts (~84% of the first
  cell's region), and both the cold-tour discipline and the 2.5x file
  size only reduce the odds, not the mechanism.
- SB_SIZE runs now call sb_cache_purge() before every timed test on a
  pre-existing file: mmap + msync(MS_INVALIDATE) over the file in 8 MB
  chunks — the documented SVR4 way to invalidate a file's cached pages,
  present on SunOS 4.1 and all Solaris.  New configure probe
  HAVE_MSYNC_INV (re-run ./configure); silently a no-op where absent
  (macOS treats it as a hint; directio systems never call it).  The
  cold-tour and 2.5x-size measures stay as belt and braces.

## v2026.916.2 — 16 September 2026

### Non-directio auto file size: RAM x 1.25 -> RAM x 2.5

- The cold-tour discipline (v2026.916.1) stops a test re-reading its own
  cached blocks, but with a file only 1.25x RAM each test still inherited
  ~75% of the file cached by the PREVIOUS test — the standard run's
  random-read histogram on a Solaris 2.3 SS2 stayed 59% cache-fast.  The
  auto size without directio is now RAM x 2.5 (still capped to half the
  free space, still overridable with --size); carryover drops to a small
  fraction that ages out mid-test.  Non-directio standard-run READ
  figures from earlier releases carry the same inflation caveat as
  v2026.916.1.

## v2026.916.1 — 16 September 2026

### Fix: page cache leaked through the size>RAM method on short tests

- Found by a Solaris 2.3 SS2 "measuring" 7.7 MB/s sequential through a
  5 MB/s SCSI-1 bus: without directio, a 4-second sweep cell restarting
  at offset 0 re-reads exactly the region the previous cell just cached,
  so non-directio sequential-read figures were upper bounds, not
  measurements (the bimodal random-read histograms — 6-8 ms cache hits
  next to 65-131 ms real reads — were the same leak).
- For the SB_SIZE method only: the sequential position now persists
  across time-bounded runs (wrapping), so consecutive cells stream
  fresh regions; and random offsets follow a Fisher-Yates permutation
  of the blocks, so no block repeats within a full file tour — with
  file > RAM, every access is cold by construction, at the original
  file size.  SB_DIRECTIO behaviour is unchanged.  **Non-directio
  (SunOS 4 / Solaris <=2.5) disk READ figures from earlier releases
  are inflated and not comparable; writes were always honest.**

## v2026.916.0 — 16 September 2026

### Fix: random()/srandom() absent from early Solaris libc

- Found by a Solaris 2.3 build (gcc, SPARCstation 2): `random`'s only
  home there is the BSD compatibility library `/usr/ucblib` — a linking
  tar pit.  The disk module's random-offset stream now uses a private
  LCG (same family as bench_mem's chase shuffle, still seeded from the
  pid), so no test depends on any platform PRNG.  Offset sequences
  differ from earlier releases; the distribution is equivalent and
  random-test figures remain comparable.

## v2026.909.5 — 9 September 2026

### Fix: strerror() absent from SunOS 4 libc

- `strerror` is an ANSI *library* invention; SunOS 4 has only the BSD
  `sys_errlist[]`/`sys_nerr` table, so the link failed with
  `Undefined symbol _strerror`.  New configure probe `HAVE_STRERROR`;
  when it fails, port.h maps `strerror` to `sb_strerror` (timer.c),
  which indexes sys_errlist.  Re-run ./configure after updating.

### Fix: unary plus and automatic aggregate initializers

- Two more ANSI-isms from the SunOS 4 build: `menu_step(..., +1)` used
  unary plus (K&R has no unary `+`), and the menu's `sb_cfg t = *c;`
  initialized an automatic struct (K&R allows struct ASSIGNMENT but not
  aggregate INITIALIZATION) — now declared then assigned.

### Fix: adjacent string-literal concatenation rejected by K&R cc

- Second carrier of the same disease, caught one file later: the
  network protocol strings pasted `SBN_MAGIC` against literals
  (`SBN_MAGIC " NOP\n"` and four siblings in bench_net.c, plus the
  sscanf format in netd.c) — macro-expanded concatenation the source
  scanner could not see, same as the TUI's ESC idiom.  The magic is now
  passed as a %s argument (netd composes its scanf format at runtime),
  keeping the single definition.  Wire bytes identical; verified by a
  full loopback client/server run.

- `"a" "b"` concatenation is ANSI-only; SunOS 4's cc stops on it.  169
  cross-line concatenations (html.c, main.c, svgchart.c, netd.c ...)
  were mechanically joined into single literals (comment/char-const-safe
  lexer), the TUI's `ESC "[H"` macro idiom was inlined to whole `"\033[H"`
  literals, and the 3.4KB --help literal became a per-line string array
  (old compilers also have line-length limits).  Byte-identical output;
  verified against the help text and a full HTML report.

### Fix: U/UL integer suffixes rejected by K&R cc

- ANSI invented the `U`/`UL` constant suffixes; SunOS 4's bundled K&R
  compiler stops dead on them ("syntax error at or near variable name
  UL", histogram.c being merely the first file it reached).  All 66
  occurrences across the tree are now plain `L` — value- and
  bit-pattern-identical everywhere (the large hex masks included), since
  the surrounding arithmetic is already unsigned long.  CLAUDE.md gains
  both K&R rules so the style stays dead.

### Fix: sprintf return value on SunOS 4 (found by a real 4.1.4 build)

- On SunOS 4 `sprintf()` returns `char *` (old BSD), not the ANSI
  character count, so the `p += sprintf(...)` idiom in the synopsis
  disk-line builder (sysinfo.c) and the CPU TUI rate cell (tui.c) failed
  to compile under the bundled K&R cc — the first true SunOS 4 build,
  on a SPARCstation 2 running 4.1.4, caught it.  Replaced with
  sprintf-then-strlen, identical behaviour everywhere.

## v2026.818.0 — 18 August 2026

### `--diskfull`: the full disk suite in one command

- `sunbench --diskfull [--hist] disk` runs the standard four-test run AND
  the block-size sweep back to back, in plain text — exactly the two disk
  stages `full` runs, without CPU/memory/network.  Both append to the
  same log, CSV and HTML report.  `--sweep` keeps its meaning (sweep
  INSTEAD of the standard run), so existing recipes are unchanged.  The
  natural command for characterizing one disk configuration (SCSI
  emulator settings, bus type, host adapter).

## v2026.802.5 — 2 August 2026

### Fix: HTML CPU MP table printed 0.000 for the MFLOPS/MWIPS rows

- The HTML report's "CPU — multiprocessor scaling" table still divided
  raw iterations/s by 1e6 for every row (header "Miter/s"), so the
  ?axpy rows — where one iteration is a whole vector pass — printed as
  0.001 and 0.000, and whetstone as 0.001.  The text table was fixed in
  v2026.725.0; the HTML writer was not.  Now converts to each kernel's
  native unit (MFLOPS, MWIPS, kDhry/s) exactly as the text table does.
  Ratios and scaling bars were always right.

### Fix: copy kernel measured direct-mapped cache aliasing, not copying

- The copy kernel copied the working set's first half onto its second
  half, putting src and dst exactly size/2 apart — and for every whole-MB
  working set, size/2 is a multiple of every direct-mapped cache size in
  the fleet.  Src and dst lines shared a cache index and evicted each
  other mid-line: a SPARCstation 10 (512 KB direct-mapped HyperSPARC E$)
  measured **3.8 MB/s** at DRAM sizes where a plain copy runs ~20, with a
  surgical 138 → 3.8 collapse between the 512 KB and 1 MB rows (halves
  256 KB apart: different indexes; 512 KB apart: same index).
- The destination is now **skewed 512 bytes** (`memcpy(buf + half + 512,
  buf, half - 512)`), which breaks the alias for every line size in the
  fleet while staying inside the buffer; working sets of 4 KB and below
  keep the old layout.  Machines with associative or write-through
  caches, and the sun4u block-copy path, are unaffected.  **Copy figures
  at >= 1 MB working sets on direct-mapped copy-back machines
  (HyperSPARC, SuperSPARC SuperCache) are not comparable with earlier
  releases** — the old numbers measured the pathology, not the machine.

## v2026.802.3 — 2 August 2026

The MP memory test grows from one row to five: a real characterization of
a multiprocessor's memory system instead of a single aggregate-read figure.

### Memory `--mp`

- **`read 16K`** — in-cache control row: N processes reading private 16 KB
  buffers live entirely in per-CPU L1, so this should scale at N.00×.
  If it doesn't, the deficit in the DRAM rows is scheduling noise, not the
  memory system.
- **`read` / `write` / `copy`** at the full working-set size (was: read
  only).  Write adds cache-coherency ownership traffic and copy drives both
  directions at once — a shared MBus and a UPA crossbar answer these very
  differently, which one aggregate-read number could never show.
- **`lat loaded`** — the pointer chase alone vs while N−1 processes stream
  sequential reads: the loaded-latency figure.  Reported in ns with the
  slowdown factor; the scaling column shows "—" (not applicable).
- Table, `--log`, and HTML report all carry the new rows; bandwidth rows
  keep the scaling bar, the latency row prints ns.
- `sb_mem_run_mp()` now takes the kernel to run; new `sb_mem_lat_mp()`.

### `full` runs the MP stages automatically

- On a multiprocessor, `full` now runs the CPU and memory MP scaling
  stages right after their single-process counterparts — one execution
  produces the whole story, and the HTML report gets the MP tables in
  their proper sections (previously they could never appear there, since
  each execution rewrites the report from its own state).  The suite
  proper stays single-process so machines compare on identical terms;
  single-CPU machines skip the MP stages with a note.  `--cpus N` forces
  a process count.
- `sunbench-fleet.py` parses the MP sections from logs and renders them
  in each machine's card.

### TUI: alternate screen — your scrollback survives

- Every dashboard (disk, sweep, cpu, mem, net) and the menu now runs in
  the xterm **alternate screen** (`ESC[?1049h/l`, the vi/less mechanism),
  so quitting restores the terminal scrollback instead of clobbering it —
  important over telnet.  When a dashboard finishes it switches back and
  reprints its final table plainly, so results land in the normal screen
  and scroll like ordinary output.  Modules launched from the menu run on
  the normal screen for the same reason; quitting the menu restores the
  pre-menu screen.  Ctrl-C anywhere leaves the alternate screen before
  dying.  Terminals that don't know the sequence (real vt100s, the Sun
  console) ignore it and behave exactly as before.
- Fixed a CPU-dashboard buffer overflow: the Dhrystone rate cell could
  exceed 40 characters on very fast (modern build-test) machines.

### Fix: SIOCGIF* never compiled in on Solaris

- Second layer of the same onion: Solaris returns the SIOCGIFMTU answer
  in `ifr_metric` and never defines the BSD `ifr_mtu` alias, so the MTU
  code was STILL compiled out there after the sockio fix (`net    : ce0`
  with no mtu).  A two-line `#define ifr_mtu ifr_metric` shim fixes the
  synopsis line and the net module MTU probe alike.

- On SunOS/Solaris the `SIOCGIF*` ioctls are declared in `<sys/sockio.h>`,
  which `<sys/ioctl.h>` does **not** include (it does on BSD-flavoured
  systems, which is why the macOS build test never noticed).  Both the new
  `net :` synopsis walk and the network module's **MTU detection** were
  silently compiling out on every Solaris build — the "local interface
  MTU 1500" in earlier reports was the fallback, not a measurement.  Both
  now include `<sys/sockio.h>` on Sun systems; MTU-capped UDP sweeps on
  non-Ethernet media (FDDI, token ring) will work as documented from this
  release on.

### `net :` synopsis line

- The system synopsis (log preamble, HTML machine card, TUI header) now
  lists every UP non-loopback interface with its MTU — `net    : le0
  (mtu 1500)` — so the NIC (Lance vs hme vs GigaSwift vs token ring)
  travels with every artifact.  SIOCGIFCONF walk, same portability
  story as the MTU probe: correct on SunOS 4 and Solaris, silently
  absent on sa_len systems (macOS).

## v2026.725.0 — 25 July 2026

SVG line charts in the HTML report, and a tool to retrofit them into
reports collected before charts existed.

### Charts

- Every sweep table in the HTML report now gets a **line chart** below it,
  SpeedSys-style: disk throughput vs block size, memory bandwidth vs working
  set (the cache cliff as a line), load latency vs working set, TCP
  throughput vs chunk size, and UDP throughput vs datagram size (up + down).
- Charts are **inline SVG written by fprintf** — no JavaScript, no external
  anything, all styling in inline attributes, so they print cleanly and
  survive injection into old reports whose stylesheets predate them
  (`src/svgchart.c`, shared by the report writer and the injector).

### MFLOPS kernels (`saxpy sp` / `daxpy dp`)

- Two new CPU kernels report **MFLOPS** the way period reviews quoted it:
  the Linpack inner loop (`y[i] += a*x[i]`) in single and double precision,
  streaming two vectors sized like the classic Linpack-100 working set
  (~40 KB / ~80 KB — deliberately larger than a sun4c cache, which is why
  era figures were memory-honest). 2 FLOPs per element; `flop/clk` shown
  when the clock is known. Comparable in spirit to published figures like
  the SPARCstation 1's "2.53 MFLOPS single / 1.43 double" (full Linpack
  does LU factorization; this is its dominant inner loop).
- A second, **streaming pair** (`saxpy mem` / `daxpy mem`) runs the same
  loop over runtime-sized vectors — `min(4 MB, RAM/8)` each, allocated only
  when the kernels run — big enough to overflow ANY fleet cache (HyperSPARC
  512 KB, SuperCache 1 MB, Ultra E-cache up to 4 MB). On big-cache machines
  the gap between the plain and `mem` rows **is** the cache benefit,
  quantified — mirroring the industry's own Linpack-100 → Linpack-1000
  progression. On sun4c the pairs read nearly identical (80 KB already
  missed).
- Rows appear in the CPU table, TUI, MP mode, HTML, and the CPU CSV
  (schema gains `saxpy_mflops,daxpy_mflops,saxpy_mem_mflops,
  daxpy_mem_mflops` columns — start a fresh CPU CSV).

### sunbench-htmlgraph (`make htmlgraph`)

- Reads an existing sunbench HTML report, finds the sweep tables (any table
  whose rows form a size ladder), parses the numbers back out, and inserts
  the same charts after each table. In place (with a `.bak`) or to a new
  file. Schema-agnostic — it reads the table headers, skips loss/IOPS
  columns, and gives latency its own chart — so it upgrades reports from any
  earlier version. Idempotent: tables already followed by a chart are
  skipped. Runs anywhere (it is meant for the modern machine where reports
  are viewed).

### Fix: FP special values (found by a SPARCstation 1's Weitek 3170)

- The `fp double` CPU kernel's recurrence **diverged to Inf by iteration 15
  and computed NaN forever after**. Invisible on FPUs that handle specials
  in hardware (modern CPUs, microSPARC) — but on a trap-to-kernel-emulation
  FPU like the SS1's Weitek 3170, every NaN op trapped and the kernel
  measured ~1000× slow (0.001 Miter/s). The recurrence is now a contraction,
  provably bounded in ~[0.1, 1.5] for any sanitized seed (verified over 10M
  iterations across the seed range), and the seed check self-heals NaN.
  **`fp double` figures are not comparable with earlier releases** (the op
  mix changed); alu/muldiv/Dhrystone/Whetstone are unaffected.
- The memory `read8` kernel read a buffer whose integer fill pattern,
  reinterpreted as doubles, was **denormal** (~6e-315) — same trap-storm on
  the Weitek, measuring ~0 MB/s. The buffer is now filled with ordinary
  doubles before `read8`. Figures on hardware-handled-FPU machines are
  unchanged.

## v2026.723.0 — 23 July 2026

Fourth and final planned module: **network** — iperf-like client/server over
plain IP.

### Network benchmark (`net` command + `sunbench-netd`)

- **`sunbench-netd`** (`make netd`): a tiny iterative server from the same
  source tree, run on a *modern* machine so the far end is never the
  bottleneck. ASCII control protocol over TCP, one connection per test,
  debuggable with telnet; a wedged client can never wedge the server
  (alarm + longjmp back to accept).
- **Client tests** (`sunbench --server HOST net`): TCP upload (result = the
  server's received-byte count), TCP download, TCP round-trip latency (1-byte
  echo, ms), **TCP write-size and read-size sweeps** (throughput vs
  application `write()`/`read()` size, 64 B – 64 KB, per direction — not wire
  packets, since the kernel segments to the MSS; the curves isolate
  per-syscall/copy cost, and receive is usually the expensive direction on
  old Sun NICs), and a **UDP datagram-size sweep in both directions**
  reporting achieved Mbit/s and **loss %** per size. UDP download is paced
  by the server to 1.5x the client's upload result for that size, so its
  loss column shows the client's receive ceiling instead of the raw speed
  mismatch with a fast server. The TCP up/down rows are labelled with the
  64 KB chunk size they use.
- **MTU-aware sweep**: sizes are capped at the local interface's MTU by
  default (via `SIOCGIFCONF`/`SIOCGIFMTU` on the interface routing to the
  server), so Ethernet is driven to 1472, FDDI to ~4324, token ring / ATM
  higher — each interface tested with the frames it was built for.
  `--netmax BYTES` raises the cap to exercise IP fragmentation deliberately.
- Everything rides plain TCP/UDP over IP — Ethernet, token ring, FDDI, ATM,
  PPP all measured identically, no link-layer code. `ENOBUFS` from a slow
  interface queue (PPP) is treated as backpressure, not loss.
- Full integration: live TUI dashboard, `--log` tee, own `--csv` schema
  (TCP figures + per-size UDP Mbit/s and loss), HTML report section with
  bars, menu entry with **typed hostname prompt**, and inclusion in `full`
  when `--server` is given (CSV derives `-net.csv`).
- New `configure` check reports whether sockets need `-lsocket -lnsl`
  (Solaris 2.x) and prints the `make NETLIBS=...` hint; SunOS 4 and modern
  systems build with no extra libraries.

## v2026.722.2 — 22 July 2026

Output polish: a true full suite, an HTML report, batch mode, and output
controls in the menu.

### Full suite (`full`)

- `full` now runs everything: disk standard run + block-size sweep + CPU +
  memory, in plain text, each with its own timestamped `--log` section.
- With `--csv FILE`, each module writes its own derived file (the schemas
  differ): `fleet.csv` → `fleet-disk.csv`, `fleet-cpu.csv`, `fleet-mem.csv`.
  Single-module runs still write exactly the file named.

### HTML report (`--html FILE`)

- One **self-contained** HTML file — inline CSS, no JavaScript, no external
  references — with a machine-synopsis card and a styled section for every
  benchmark run in the session: disk tables with latency percentiles, the
  block-size sweep, CPU kernels, memory working-set sweep, and the MP scaling
  tables. Sweep tables get **pure-CSS proportional bars**, so the block-size
  curve and the cache cliff are visible at a glance; an `@media print`
  stylesheet keeps it printable.
- The report is rewritten after every completed run, so a menu session or a
  `full` suite grows it incrementally and an abort never loses finished
  results.

### Batch mode (`--quiet` / `-q`)

- Silences all stderr progress and success notices (warnings and errors
  still print) and implies `--plain`. `sunbench -q --log ... --csv ...
  --html ... full` produces artifacts and nothing else.

### Menu

- New output toggles — **Log to text file**, **CSV for spreadsheets**,
  **HTML report** — with sensible default filenames (`sunbench.log` / `.csv`
  / `.html`); names given on the command line are remembered and restored by
  the toggle.

### Fix

- Long `--path` values could overflow a fixed description buffer at the end
  of a sweep (stack smash). Buffers enlarged and paths clamped.

## v2026.722.1 — 22 July 2026

First **memory** benchmark module: bandwidth and latency vs working-set size.

### Memory benchmark (`mem` command)

- **Working-set sweep** — each kernel runs over buffers from 1 KB up to
  `min(16 MB, RAM/4)` (override with `--memsize MB`), doubling each step. On
  classic Sun hardware the table is a photograph of the cache hierarchy:
  bandwidth falls off a cliff and latency steps up at every cache boundary
  (sun4c 64 KB unified; microSPARC 4K/2K; SuperSPARC/HyperSPARC L1 + optional
  module cache; UltraSPARC L1 + E-cache). The sweep starts at 1 KB because
  the microSPARC's D-cache is only 2 KB — a 4 KB floor would already miss at
  every size and show no cliff at all (as a first LX run demonstrated).
- **Kernels:** `read` (4-byte integer loads — what ordinary code gets),
  `read8` (8-byte FPU loads — `ldd` moves twice the bytes per instruction on
  32-bit SPARC, showing peak vs typical), `write` (integer stores), `copy`
  (libc `memcpy`, MB/s counts bytes copied once), and `latency` (random
  pointer-chase at 64-byte stride, ns per dependent load — nothing to
  prefetch or overlap, the honest cache/DRAM latency).
- **MP mode** (`--mp`, `--cpus N`) — N concurrent readers, each with a
  private buffer, vs one alone: how much memory bandwidth the interconnect
  (sun4m MBus, sun4u UPA, sun4c's own memory path) has beyond one CPU's
  demand. Aggregate, speedup, and scaling efficiency, like the CPU MP table.
- Full integration: live TUI table on a terminal, `--log` tee, own `--csv`
  schema (per-size columns for all five kernels), `--passes` median, menu
  entries (**Memory benchmark**, **Memory multiprocessor scaling**). The
  menu's disk, CPU and memory sections are now separated by blank rows.
- Measurement mirrors the CPU module: per-batch `gettimeofday` summation,
  volatile sinks (note: the chase sink must be a *volatile pointer*,
  `char * volatile` — `volatile char *` would be a dead store and let the
  compiler delete the whole dependent-load chain).

## v2026.722.0 — 22 July 2026

CPU module **Phase 3**: multiprocessor scaling, repeat-and-median, a live CPU
dashboard, and a CPU CSV schema for cross-machine comparison.

### Multiprocessor mode (`--mp`, `--cpus N`)

- **`fork()`-based** parallel benchmark — never threads, since SunOS 4 has
  none. The parent measures one process alone, then forks N children that run
  the same kernel concurrently and report their iteration counts back over a
  pipe; the parent sums them.
- Reports the single-process rate, the N-way **aggregate**, the **speedup**,
  and the **scaling efficiency** (aggregate ÷ N × single). Efficiency below
  100% is memory-bus (MBus/XBus) contention — why two 150 MHz CPUs are not
  simply twice one.
- Online CPU count auto-detected via `sysconf(_SC_NPROCESSORS_ONLN)` (new
  `configure` probe `HAVE_SC_NPROCESSORS_ONLN`; falls back to 1); `--cpus N`
  overrides. MP figures are **wall-clock** rates, so the baseline and the
  concurrent children are measured the same way.

### CPU repeat-and-median (`--passes N` for `cpu`)

- `--passes` now applies to the `cpu` command too: each kernel is run N times
  and the **median** rate is reported, with a `+/- X%` spread. This tames the
  ~10% cache-sensitive scatter of Dhrystone and Whetstone that CPU-time
  measurement alone can't remove.

### Live CPU dashboard

- The `cpu` command now uses the same ANSI/vt100 live view as the disk
  benchmark when run on a terminal: a per-kernel progress bar with the running
  rate, each row shown in its native unit (Miter/s, Dhrystones/s, MWIPS). The
  live bar uses a wall-clock estimate; the finished figure is the authoritative
  CPU-time rate. Piped, redirected, `--plain`, `--mp` and `--passes` runs stay
  plain text.
- The interactive menu (bare `sunbench` on a terminal) now offers **CPU
  benchmark** and **CPU multiprocessor scaling** alongside the disk tests.
- Every live dashboard (disk run, sweep, CPU, and the menu) now paints the
  **system synopsis** (uname, model, RAM, and — for disk — the drive) in its
  header, so a screenshot or copy-paste of the screen is self-describing, the
  same way a `--log` file already was.
- The CPU dashboard's rate column now matches the plain report exactly: the
  per-clock figure (`/clk`) for the custom kernels when the clock is known,
  VAX MIPS beside Dhrystones/sec, and MWIPS for Whetstone.

### CPU CSV (`--csv` for `cpu`)

- `--csv` now works with the `cpu` command, writing one wide, spreadsheet-ready
  row per run in its own schema: system metadata, clock, build target, passes,
  note, then per-kernel figures (ALU/mul-div/FP Miter/s, Dhrystones/s + VAX
  MIPS, Whetstone MWIPS). Header written only for a fresh file, so rows from
  many machines accumulate into one cross-machine table.

### CPU timing hardened; Dhrystone layout sensitivity root-caused

- CPU kernel rates are now measured by **summing wall-clock time around each
  kernel batch** (`gettimeofday`), so the live display and reporter are never
  inside the timed region and a slow serial console cannot skew the result.
  (Replaces the `times()` CPU-time divisor; assumes an otherwise idle machine.)
- Investigated a Dhrystone score that swung ~1.5× between sunbench builds on a
  SPARCstation LX while every other kernel held steady. The root cause is
  **not** a harness, reporter, or clock bug (`gettimeofday`, `times()` and
  `gethrtime` all agree on that machine): on the microSPARC's 4 KB
  direct-mapped I-cache, the score depends on **where the linker places**
  Dhrystone's hot functions and libc's `strcpy`/`strcmp`. Proven by linking
  the identical objects in two different orders on the same machine: 40,012
  vs 58,425 Dhrystones/s. Documented in the README; the custom kernels
  (alu / muldiv / fp) fit in cache, are immune, and remain the preferred
  cross-machine ratios.

### Tooling

- New `make dhry-ref` and `make dhry-ref-sb` build standalone **Dhrystone 2.1
  references** from the same kernel objects sunbench uses — the first linked
  minimally, the second with sunbench's full object list in sunbench's exact
  link order. Each times the same loop with three clocks (`gettimeofday`,
  `times()`, `gethrtime`) plus a per-call variant, and prints where each hot
  function landed modulo the 4 KB I-cache size — the pair is how the layout
  sensitivity above was isolated and proven.

## v2026.721.1 — 21 July 2026

First **CPU** benchmark module, plus disk and output refinements.

### CPU benchmark (`cpu` command)

- **Custom kernels:** integer ALU, integer multiply/divide, and
  double-precision floating point. The mul/div kernel is isolated from the ALU
  kernel to expose the sun4c V7 (software mul/div) vs sun4m/sun4u V8/V9
  (hardware) difference — a clean ~2× on V8 hardware.
- **Classic kernels:** Dhrystone 2.1 (Dhrystones/sec + VAX MIPS) and Whetstone
  (MWIPS), for comparison with published figures. Note Dhrystone 2.1 reports
  lower than the older, compiler-inflated Dhrystone 1.1 in vintage marketing.
- **CPU-time measurement** (`times()`), not wall clock, so background load
  can't skew the rate. Tight kernels are rock-steady; the memory-touching
  classic benchmarks retain some inherent (cache-sensitive) run-to-run scatter.
- **Auto clock detection** via `psrinfo` (or `--mhz`), with per-clock
  efficiency for the custom kernels.
- **Build-target stamping** (compiler + `-mcpu`) in every CPU report, since a
  V7 build and a native build measure different things on the same machine.
- Anti-optimization guards (volatile sinks; Dhrystone kept as two separate
  translation units) so kernels aren't folded away. Links `-lm` for Whetstone.

### Disk

- **`--passes N`** repeats each disk test and reports **min / median / max**
  (median on screen, full spread in the CSV) — for SD/flash media whose write
  throughput is erratic while reads stay stable. See the README.

### Output

- **`--log` now tees**: the report *and* its timestamped preamble (system
  synopsis) show on screen and are copied to the file, with a single matching
  timestamp — instead of the report being redirected into the file only.

## v2026.721.0 — 21 July 2026

- **Sweep random span.** The block-size sweep now uses a **256 MB** test file
  by default (was 32 MB), capped to half the free space. A larger file makes
  random I/O scatter over a representative region of the disk rather than
  hammering a small area — closer to real-world random access (databases, OS
  load) and less prone to flash steady-state artefacts. A note is printed when
  the file ends up ≤ 64 MB, since a small random span may not be
  representative. Tune with `--size`.
- **`MCPU=-mcpu=v7` build option** for `sun4c`/V7 fleet binaries: a binary
  built on a faster V8 host (SPARCstation LX, Ultra) with gcc can be forced to
  the V7 instruction set so it still runs on a SPARCstation 1/IPC/IPX (whose
  integer unit has no V8 multiply/divide). Empty by default — SunOS 4's `cc`
  rejects `-mcpu` and its machines are all V7 already; `./configure` prints the
  hint when the compiler accepts the flag.

## v2026.719.0 — 19 July 2026

First release. Disk benchmark complete and validated on real hardware.

### Portability

- Dual **K&R / ANSI C** compilation: K&R function definitions plus a `P_(())`
  prototype macro, so the same source builds with the bundled `cc` on SunOS 4
  and with gcc/SunPro on Solaris 2.x.
- Hand-rolled `./configure` (no autoconf) probing `gethrtime`, `directio`,
  `sysconf(_SC_PHYS_PAGES)`, `O_DSYNC`, `statvfs`, `statvfs.f_basetype`,
  `sysinfo`, `mnttab`, `popen`, `termios`, `<malloc.h>`.
- `__EXTENSIONS__` defined before system headers, without which Solaris hides
  `gethrtime`/`directio` from gcc and silently truncates the 64-bit
  `hrtime_t` result.

### Measurement

- Cache bypass: `directio()` on Solaris 2.6+, otherwise a test file larger
  than RAM. Probed **on the scratch filesystem** at runtime, since `directio`
  fails on tmpfs and some NFS.
- Four access patterns: sequential write/read, random read/write, driven by
  one time-bounded engine.
- Latency histogram in log2 buckets with 4 sub-buckets per octave
  (HdrHistogram-style), giving trustworthy p50/p90/p99 without storing
  per-operation samples.
- Block-size sweep from 4KB to 1MB.
- Display time is measured and excluded from throughput, so the live
  interface cannot skew results on a slow console.

### Safety

- Warns when the scratch path is on a memory-backed filesystem (Solaris
  `/tmp` is tmpfs — benchmarking it measures RAM, not disk).
- Test file capped to half the free space, so a small vintage disk cannot be
  filled.

### Interface

- Live ANSI/vt100 dashboard, output-only, with no ncurses dependency;
  selected automatically via `isatty()` and `$TERM`.
- Compact in-place block-size sweep table.
- Interactive arrow-key menu (`termios` cbreak, restored on exit and on
  SIGINT) with standard run, sweep, full suite and a histogram toggle.
- Plain text with progress on stderr whenever output is piped, redirected or
  `--plain` — scripting is unaffected.

### Output

- `--log FILE` appends a timestamped report with a system synopsis: `uname`,
  hardware model (`SI_PLATFORM`), RAM, filesystem type and capacity, backing
  disk device (via `/etc/mnttab`) and SCSI drive model (via `iostat -En`).
- `--csv FILE` appends one wide, block-major spreadsheet row per sweep;
  header written once so many machines accumulate into one table.
- `--note TEXT` adds a free-text CSV column.
- `--full` runs the standard benchmark and the sweep unattended.
- `--help`, `--version`, and a `sunbench(1)` man page.

### Packaging

- `make pkg` builds an SVR4 datastream package containing a **prebuilt
  binary**, installable on Solaris 2.0–10 with no compiler on the target
  (`pkgadd -d …`, relocatable under `/usr/local` via `BASEDIR`).
- `make srctar` builds a plain (ungzipped) source tarball for SunOS 4 /
  Solaris 1.x, which lacks `pkgadd` but ships a usable K&R `cc`.
- `make install` places the binary and man page under `PREFIX`
  (default `/usr/local`), and is the hook the package build stages.

### Validated on

SPARCstation 1 (`SUNW,Sun_4_60`), Solaris 2.6, gcc, BlueSCSI Pico + SD card
over external SCSI. Sequential ~2 MB/s (SCSI-1 bus limit), random ~10 IOPS
(per-command latency floor), with a clearly bimodal random-latency
distribution.

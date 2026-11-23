# sunbench

**v2026.11.22-4**

A small, easy-to-install disk, CPU, memory and network benchmark for classic Sun hardware —
SunOS 4 (Solaris 1.x) through Solaris 10, tested on SPARC. Builds with the stock K&R
`cc` on SunOS 4 and with gcc/SunPro on Solaris. Only libc (plus libm): no
ncurses, no autoconf, no dependency searching to get it to run.

## Why

Tools like iozone and bonnie are powerful but awkward to build on vintage
Solaris and overwhelming to drive. sunbench aims to be the opposite: one small
binary, sensible defaults, honest numbers, and output you can paste into a
spreadsheet.

## Design constraints

- **SunOS 4.x → Solaris 10**, one 32-bit SPARC V7 binary across `sun4c` /
  `sun4m` / `sun4u` (SPARCstation 1 → Ultra 60, Blade 100/150, Sun Fire V240).
  No `sun4v` / SPARC64.
- **Compiles under the bundled K&R `cc` on SunOS 4** *and* ANSI compilers.
  K&R function definitions plus a `P_(())` prototype macro — see
  [`src/port.h`](src/port.h).
- **No dependency hell.** libc only. Feature detection is a ~90-line shell
  script, [`./configure`](configure) — not autoconf.
- **Honest numbers on big-RAM boxes** without grinding through all of RAM.
- **Frugal.** One reusable I/O buffer, streaming stats, bucketed histograms —
  no per-operation sample arrays. Comfortable on a 28MB SPARCstation 1.

## Cache-bypass strategy

This benchmark uses `directio()` to avoid cache for disk benchmarking, but this
is only available in Solaris 2.6 or later.  In earlier SunOS/Solaris versions, it 
writes a file larger than RAM (size is configurable) to avoid cache.  `directio` 
is probed **on the scratch filesystem** at runtime (it fails on tmpfs and some NFS).

## Build

On Solaris 2.6 with gcc installed in `/usr/bin/local`:

```sh
CC=/usr/local/bin/gcc ./configure
/usr/ccs/bin/make CC=/usr/local/bin/gcc MCPU=-mcpu=v7 NETLIBS='-lsocket -lnsl'
/usr/ccs/bin/make install           # PREFIX=/usr/local by default
```

In Solaris 2.x, the `NETLIBS='-lsocket -lnsl'` is required for the network benchmark
to work. 

On **Solaris 2.x there is no bundled C compiler** — use `CC=gcc ./configure`
and `make CC=gcc`. On **SunOS 4** the bundled `cc` works as-is.

## Running

It is extremely important to run this benchmark in single-user mode and do not
use the `/tmp` filesystem for disk benchmarking. Doing otherwise will cause
incorrect (and poor) benchmark results.  The usual routine is: 

1. Boot the machine as per usual, and FTP the files onto it.
2. Build the `sunbench` binary.
3. Reboot the machine in single-user mode (`boot -s` from the OBP `ok` prompt). Do not start any additional services.
4. Run the test as root, e.g. `./sunbench full --server 192.0.2.10 --log ss1.log --csv ss1.csv --html ss1.html --note "OBP 1.0; Emulex 2400138"`
to run the test in non-interactive mode in Solaris 2.6, producing the text-based log, CSV, and HTML output files, with the server at 192.0.2.10
running `sunbench-netd` for the server component of network testing. 

If you enable additional services in single-user mode, they willl likely use the
disk, network, or CPU while the benchmark is running and you will get spurious
results.

### Building one binary for a mixed fleet (sun4c / V7)

Solaris binaries run *forward* (build on the oldest OS you support; a 2.6 build
runs on 2.6–10, never the reverse). Architecture is separate: `sun4c`
(SPARCstation 1/IPC/IPX) is SPARC **V7** with no hardware integer
multiply/divide, while `sun4m`/`sun4u` are V8/V9. A binary compiled with
V8-only instructions **traps as "illegal instruction" on a sun4c**.

So when you build a fleet binary on a faster V8 box (an LX, an Ultra) with gcc,
force the V7 instruction set so it also runs on your SPARCstation 1:

```sh
make CC=gcc MCPU=-mcpu=v7
```

A V7 binary runs on `sun4c`, `sun4m` and `sun4u` alike. `./configure` prints
this reminder when your compiler accepts the flag. It is **not** the default:
SunOS 4's bundled K&R `cc` doesn't understand `-mcpu`, and its machines are all
`sun4c`/V7 already, so a plain `make` there is correct as-is.

## Packaging

**Solaris 2.0–10** — an SVR4 package carrying a prebuilt binary, so the target
needs no compiler:

```sh
CC=gcc ./configure && make CC=gcc
make pkg                      # -> sunbench-2026.11.22-4-sol-sparc.pkg
pkgadd -d sunbench-2026.11.22-4-sol-sparc.pkg
pkgrm sunbench                # to remove
```

Build the package on the **oldest** Solaris you support: Solaris keeps
backward binary compatibility, so a 2.6 build runs on 2.6/7/8/9/10 — but not
the reverse. Installs under `/usr/local` (relocatable via `BASEDIR`).

**SunOS 4 / Solaris 1.x** — no `pkgadd`, but the bundled `cc` works:

```sh
make srctar                   # -> sunbench-2026.11.22-4-src.tar (plain tar, no gzip needed)
# on the target:
tar -xf sunbench-2026.11.22-4-src.tar
cd sunbench-2026.11.22-4 && ./configure && make && make install
```

## Usage

```sh
sunbench --path /export/home/sb.dat disk     # standard four-test run
sunbench --path /export/home/sb.dat --sweep --hist disk
sunbench --path /export/home/sb.dat --diskfull --hist disk   # full disk suite: standard run + sweep
sunbench --path /export/home/sb.dat --log fleet.log --csv fleet.csv full
sunbench cpu                                 # CPU kernels (live TUI on a terminal)
sunbench --mp cpu                            # multiprocessor scaling test
sunbench mem                                 # memory bandwidth/latency vs working set
sunbench --server modernbox net              # TCP/UDP tests vs sunbench-netd
sunbench                                     # interactive menu on a terminal

# the whole machine, unattended, with every artifact (on a multiprocessor
# the CPU and memory MP scaling stages run automatically after the suite):
sunbench -q --path /export/home/sb.dat \
         --log lx.txt --csv lx.csv --html lx.html full
```

Run `sunbench --help`, or `man sunbench`, for the full option list.

> **Point `--path` at a real disk.** On Solaris `/tmp` is tmpfs (RAM); a
> benchmark there measures memory, not storage. sunbench warns if you do.
> Use `df -k` and pick a `/dev/dsk` filesystem.

### Interface

A live ANSI/vt100 display is used automatically on a terminal (no ncurses);
piped, redirected or `--plain` output stays plain text, so scripting is
unaffected. The time spent painting the display is measured and **subtracted
from the throughput result**, so the interface never skews the numbers.

### Logging and spreadsheets

- `--log FILE` shows the report on screen *and* appends a timestamped copy
  (plus a system synopsis: uname, hardware model, RAM, filesystem, disk device,
  drive model) to the file. Progress stays on stderr and never enters the file.
- `--csv FILE` appends one wide, spreadsheet-ready row per run; each module
  has its own schema, and a `full` run derives per-module names
  (`fleet.csv` → `fleet-disk.csv` / `fleet-cpu.csv` / `fleet-mem.csv`). The
  header is written only once per file, so many machines accumulate into one
  table. `--note` adds a free-text column for what sunbench can't know (OBP
  revision, SCSI chip, drive replacement).
- `--html FILE` writes a **self-contained HTML report** — inline CSS, no
  JavaScript, nothing external — with a machine-synopsis card, styled tables
  for every benchmark run in the session, pure-CSS proportional bars, and
  **SVG line charts** under every sweep table (disk block-size curve, the
  memory cache cliff and latency steps, TCP chunk curves, UDP up/down).
  It prints cleanly, and it's rewritten after each completed run so a long
  session grows it incrementally.
- `make htmlgraph` builds **sunbench-htmlgraph**, which retrofits the same
  charts into reports generated *before* charts existed: it parses the sweep
  tables out of an existing HTML file and injects a chart after each one
  (in place with a `.bak`, or to a new file; idempotent). Run it on the
  modern machine where the reports live.
- `--quiet` (`-q`) silences progress and notices for scripts/cron — a
  `sunbench -q --log ... --csv ... --html ... full` run produces its
  artifacts and nothing else.
- The interactive menu has toggles for all three outputs, with default
  filenames (`sunbench.log` / `.csv` / `.html`).

### Flash / SD-card write variability (why `--passes` exists)

On SD-card and flash-based SCSI emulators (BlueSCSI, ZuluSCSI), **reads are
deterministic but writes are not.** A single test measured across back-to-back
runs shows reads landing within a percent or two every time, while the *same*
write test swings 2–8× — one 256KB sequential write cell might read 6.4 MB/s on
one pass and 1.2 MB/s on the next.

That isn't sunbench being sloppy; it's the flash. Writes trigger the card's
internal **garbage collection, erase-block recycling, and cache flushes**,
whose timing is unpredictable. A short (few-second) measurement window that
happens to overlap one of those pauses reports a badly depressed number; the
next window misses it and reports full speed. Reads never touch that machinery,
so they stay flat.

Because a single write sample is unreliable on this media, use **`--passes N`**
to repeat each test and report the spread:

```sh
sunbench --path /export/home/sb.dat --sweep --passes 3 --csv fleet.csv disk
```

The on-screen table shows the **median** (robust — it drops the one
garbage-collection outlier without cherry-picking a lucky peak), and the CSV
records **min / median / max** for every metric so the variability is visible
rather than hidden. `--passes` runs in plain mode (it's a characterization run,
not the live TUI). Reads barely need it; it's really there to tame writes.

> Adding `--passes` changes the CSV schema (a `passes` column, and
> `_min/_med/_max` columns when N > 1). Start a fresh CSV file for
> multi-pass runs.

### CPU benchmark

```sh
sunbench cpu                        # live TUI on a terminal, plain when piped
sunbench --passes 5 cpu             # median rate + spread per kernel
sunbench --log cpu.log --csv cpu.csv --note "SM61" cpu
sunbench --mp cpu                   # or: --cpus N
```

Nine kernels: integer ALU, integer multiply/divide, double-precision floating
point, **Linpack-style MFLOPS** in single and double precision (the Linpack
inner loop `y[i] += a*x[i]` over a Linpack-100-sized working set — comparable
in spirit to period figures like the SPARCstation 1's "2.53 MFLOPS single /
1.43 double"), the same pair over a **streaming footprint** that overflows
any fleet cache (`saxpy mem` / `daxpy mem` — on a HyperSPARC or Ultra, the
gap between the pairs *is* the E-cache benefit; Linpack-100 fit inside big
caches in the period too, which is why Linpack-1000 was invented),
**Dhrystone 2.1** (Dhrystones/sec + VAX MIPS) and **Whetstone** (MWIPS).
The mul/div kernel is deliberately split from the ALU kernel to expose the
difference between **sun4c (SPARC V7, software mul/div)** and **sun4m/sun4u
(V8/V9, hardware)** — a clean ~2× on V8. Every report is stamped with the build
target (compiler + `-mcpu`), because a V7 build and a native build measure
different things on the same box.

Rates are measured by summing the **wall-clock time spent inside the kernel
batches** (`gettimeofday`), so the live display is never part of the timed
region and even a slow serial console cannot skew the numbers. **Run CPU tests
on an otherwise idle machine** — wall-clock timing means background load
depresses the result. Dhrystone and Whetstone keep a few percent of run-to-run
scatter, which `--passes` smooths by reporting the median.

#### Dhrystone and direct-mapped caches

On machines with small direct-mapped instruction caches (the microSPARC's is
4 KB), the Dhrystone score depends heavily on **where the linker happens to
place** the benchmark's hot functions and libc's `strcpy`/`strcmp`: functions
that collide modulo the cache size evict each other on *every iteration*. On a
50 MHz SPARCstation LX, the **identical** Dhrystone objects measured **40,012**
Dhrystones/s in one link order and **58,425** in another — a 1.46× swing from
placement alone (reproduce it with `make dhry-ref` and `make dhry-ref-sb`,
which also print where each hot function landed modulo 4 KB). This is a
well-known weakness of Dhrystone itself, not a sunbench artifact. Treat
Dhrystone as an indicator, compare machines using the **same sunbench build**,
and prefer the custom kernels (`int alu`, `int muldiv`, `fp double` — small
enough to be immune) for precise cross-machine ratios.

**Multiprocessor scaling** (`--mp`, or `--cpus N`) is done with `fork()` — never
threads, which SunOS 4 doesn't have. sunbench runs one process alone for a
baseline, then N processes concurrently, and reports the single-process rate,
the N-way aggregate, the speedup and the **scaling efficiency**:

```
  test            1-proc     N-proc    speedup    scaling   (Miter/s)
  int alu        150.312    287.994      1.92x      95.8%
  int muldiv      41.203     78.512      1.91x      95.5%
```

Efficiency below 100% is contention on the memory interconnect (MBus on
sun4m, UPA on sun4u, sun4c's own memory path) — the reason two 150 MHz CPUs
are not simply twice one. MP figures are wall-clock rates, so the baseline and
the concurrent children are measured the same way.

### Memory benchmark

```sh
sunbench mem                        # working-set sweep, live TUI on a terminal
sunbench --memsize 32 mem           # sweep 1KB .. 32MB
sunbench --log mem.log --csv mem.csv --note "SS10 2x HyperSPARC" mem
sunbench --mp mem                   # MP scaling: read/write/copy + loaded latency
```

The sweep runs five kernels over working sets from 1 KB up to
`min(256 MB, RAM/4)`, doubling each step — the result is a **photograph of the
cache hierarchy**, with a bandwidth cliff and a latency step at every cache
boundary (sun4c's 64 KB unified cache, the microSPARC's tiny 4K/2K split
caches, SuperSPARC/HyperSPARC L1 + optional MBus-module cache, UltraSPARC L1 +
E-cache):

- `read` — 4-byte integer loads: the bandwidth ordinary code gets
- `read8` — 8-byte FPU loads: on 32-bit SPARC, `ldd` moves twice the bytes
  per instruction, so `read8` vs `read` shows peak vs typical
- `write` — integer stores
- `copy` — libc `memcpy` (bytes copied counted once): the "real application"
  figure, using whatever tricks libc has
- `latency` — random pointer-chase at 64-byte stride, **ns per dependent
  load**: each address is the previous load's data, so nothing can be
  prefetched or overlapped — the honest cache/DRAM latency

**Memory MP mode** (`--mp mem`) runs five experiments, each pitting N
concurrent processes (private buffers) against one process alone:

- `read 16K` — in-cache control: private 16 KB buffers live in per-CPU L1
  and should scale at a flat N.00×; anything less means the run is noisy
- `read`, `write`, `copy` at the full working-set size — write adds
  coherency ownership traffic and copy drives both directions at once,
  which a shared MBus and a UPA crossbar answer very differently
- `lat loaded` — the pointer chase alone vs while N−1 processes stream
  sequential reads: what a busy neighbour does to another CPU's memory
  latency (reported in ns with the slowdown factor)

On a dual-CPU MBus box this answers "can one HyperSPARC saturate the bus,
or is there headroom for the second?" — and on a UPA machine, whether the
crossbar really keeps the second CPU out of the first one's way.

### Network benchmark

```sh
# on a modern machine (Mac, Linux, anything) from this same source tree:
make netd && ./sunbench-netd            # listens on port 5202

# on the Sun:
sunbench --server modernbox net                  # live TUI
sunbench --server modernbox --netmax 8900 net    # explore IP fragmentation
```

iperf-shaped, over plain TCP/UDP — so **any IP-capable interface** (Ethernet,
token ring, FDDI, ATM, PPP) is measured identically, with no link-layer code.
The server runs on a modern machine so the far end is never the bottleneck.
The tests:

- **TCP upload / download** — sustained stream each way with 64 KB
  application chunks (labelled as such), Mbit/s; the upload figure is the
  *server's* received-byte count: what actually got through
- **TCP round trip** — 1-byte echo, average ms
- **TCP write-size and read-size sweeps** — throughput vs the application
  `write()`/`read()` size (64 B → 64 KB) in each direction. These are *not*
  wire packets — the kernel segments to the MSS regardless — so the curves
  isolate **per-syscall/copy cost** per direction, which is what actually
  limits small-write programs (NFS, X11) on a slow CPU. Receive is usually
  the expensive direction on old Sun NICs.
- **UDP size sweep, both directions** — datagrams from 64 bytes up to the
  local interface's **MTU** (detected via `SIOCGIFMTU`: Ethernet 1500, FDDI
  4352, token ring and ATM larger), reporting achieved Mbit/s *and* loss%
  per size. Each interface is driven with the frame sizes it was built for;
  `--netmax` raises the cap to exercise IP fragmentation deliberately.
  `ENOBUFS` from a slow interface queue (PPP) is treated as backpressure,
  not loss. The download direction is **paced by the server to 1.5x the
  upload result** for the same size, so its loss column shows the receive
  ceiling rather than the raw mismatch with a fast server.

## MS-DOS: SBDOS.EXE

`dos/` holds an MS-DOS port of the disk, CPU, memory and network tests, for
PCs and PS/2s back to the 8086 (8086 code; uses an x87 if present). It writes
the same log format as sunbench (so `sunbench-fleet.py` reads it) and the
same HTML report.

- **disk**: raw BIOS INT 13h on a whole drive (read-only unless `--rewrite`,
  which re-writes data just read back), or DOS file I/O with `--file`.
- **cpu / mem**: sunbench's kernels, Dhrystone and Whetstone; memory sweep
  past 64 KB, segment by segment.
- **net**: UDP round trip and up/down sweeps against `sunbench-netd`,
  through a packet driver (Crynwr or similar), reading IP settings from
  mTCP's `MTCPCFG` file.

Cross-compile it with [Open Watcom v2](https://github.com/open-watcom/open-watcom-v2)
(set `WATCOM` to the install directory):

```sh
sh dos/build.sh               # -> SBDOS.EXE
```

On the PC: `SBDOS --help` (paged), `SBDOS -v` for the version, e.g.
`SBDOS --rewrite --log PC.LOG --html PC.HTM all`.

## Measured behaviour (example)

SPARCstation 1 (`SUNW,Sun_4_60`), Solaris 2.6, BlueSCSI Pico + SD:

```
  seq write        1.62 MB/s      seq read        2.13 MB/s
  rand read        0.61 MB/s (9.8 IOPS)
```

The latency histogram shows a **bimodal** distribution — roughly half the
random operations near ~33ms and half near ~131ms, with almost nothing in
between. The ~100ms *average* is a fiction: no operation actually takes that
long. The block-size sweep shows random IOPS pinned near ~14 across 4–16KB
(the SCSI-1 per-command latency floor) before bandwidth begins to dominate.

## Status

- [x] Portability foundation (dual K&R/ANSI compile, feature probes)
- [x] Tiered cache-bypass, tmpfs guardrail, free-space auto-cap
- [x] Disk: seq write / seq read / random read / random write
- [x] Latency histogram: percentiles + `--hist` distribution
- [x] Block-size sweep (4KB…1MB)
- [x] Live ANSI TUI + interactive menu (no ncurses)
- [x] Timestamped logs with system synopsis; CSV export
- [x] Man page, `--help`, versioning
- [x] SVR4 `pkg` (Solaris 2.0–10) + SunOS 4 source tarball
- [x] CPU: integer / FP / Dhrystone / Whetstone kernels, build-target stamped
- [x] CPU: `fork()` multiprocessor scaling, `--passes` median, live TUI, CSV
- [x] Memory: bandwidth + latency working-set sweep, MP aggregate, TUI, CSV
- [x] Full suite, HTML report, `--quiet` batch mode, menu output toggles
- [x] Network: TCP up/down/RTT + UDP MTU-aware size sweep vs `sunbench-netd`

See [CHANGELOG.md](CHANGELOG.md) for release history and
[CLAUDE.md](CLAUDE.md) for the coding conventions this project requires.

## License

sunbench is free software, released under the
[GNU General Public License v3](LICENSE) or (at your option) any later
version. `src/whet.c` (Whetstone, Painter Engineering) keeps its own terms
and Dhrystone 2.1 (Weicker) is public domain; see [NOTICE](NOTICE).

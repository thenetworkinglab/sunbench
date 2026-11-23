# CLAUDE.md — working on sunbench

Read this before changing code. The portability rules are not stylistic; they
are what lets the same source build on a 1990 SPARCstation and on Solaris 10.

## What this is

A disk **and CPU** benchmark for SunOS 4 (Solaris 1.x) through Solaris 10 on
SPARC. One 32-bit SPARC V7 binary runs across `sun4c`/`sun4m`/`sun4u`. No
`sun4v`, no SPARC64. libc + libm only — **never add a third-party dependency**
(no ncurses, no autoconf, no pthreads).

## Status and roadmap (as of v2026.722.0)

- **Disk** — complete: seq/random read/write, latency histogram, block-size
  sweep, live TUI + menu, `--passes` (min/median/max), timestamped log + CSV.
- **CPU** — complete through Phase 3:
  - Kernels: `int alu`, `int muldiv` (isolates V7 soft vs V8 hw mul/div),
    `fp double`, Dhrystone 2.1, Whetstone. Rate = **sum of per-batch wall
    time** (`gettimeofday`), so the reporter is never inside the timed region;
    assumes an idle box. **Dhrystone is code-layout-sensitive on
    direct-mapped-cache machines** (microSPARC 4KB I$): identical objects
    scored 40k vs 58k Dhrystones/s on an LX purely from link order — verified
    with `make dhry-ref` / `make dhry-ref-sb` (which also print hot-function
    addresses mod 4K). Not a harness bug; it is documented, not "fixed" —
    don't chase it again. `psrinfo`/`--mhz` clock; build-target stamped.
  - **MP mode** (`--mp`, `--cpus N`) — `fork()`-based (never threads: SunOS 4
    has none). Parent runs one process for a baseline, then forks N children
    that run the kernel concurrently and report iteration counts back over a
    pipe; parent sums. Reports single rate, N-way aggregate, speedup and
    **scaling efficiency** = aggregate ÷ (single × N) (exposes MBus/XBus
    contention). CPU count via `sysconf(_SC_NPROCESSORS_ONLN)` (probe
    `HAVE_SC_NPROCESSORS_ONLN`, falls back to 1). MP rates are **wall-clock**
    (baseline and children measured the same way; contention shows as reduced
    wall throughput). `run_wall()`/`calibrate()` in `cpu.c`; `SB_MAXCPUS 64`.
  - **CPU `--passes`** — repeat each kernel, report the median rate + a `+/- %`
    spread (tames Dhrystone/Whetstone cache scatter). Reuses `sortd()`.
  - **CPU live TUI** — `sb_cpu_tui_*` in `tui.c`, same ANSI dashboard as disk;
    live bar uses the wall-clock tick estimate, the finished figure is the
    authoritative CPU-time rate (`sb_cpu_tui_set`). `--mp`/`--passes`/`--plain`
    and non-terminals stay plain.
  - **CPU `--csv`** — own schema (`write_cpu_csv` in `main.c`): metadata, clock,
    build target, passes, note, then per-kernel figures. Distinct from the disk
    sweep CSV; header once per fresh file.
- **Memory** — complete (v2026.722.1): working-set sweep 1KB..min(256MB,RAM/4)
  (`--memsize`), five kernels — `read` (long loads), `read8` (double/`ldd`
  loads, 2x bytes/insn on 32-bit SPARC), `write`, `copy` (libc memcpy, bytes
  counted once), `latency` (random 64B-stride pointer chase, ns/dependent
  load). Bandwidth cliffs + latency steps map the cache hierarchy. MP mode
  (`--mp`/`--cpus`): N readers with private buffers vs one — aggregate
  interconnect bandwidth. TUI table, `--log`, own CSV schema, `--passes`
  median, menu entries. Gotcha preserved in `bench_mem.c`: the chase sink
  must be `char * volatile` (a volatile POINTER) — `volatile char *` is a
  dead store and the compiler deletes the whole dependent-load chain.
- **Output** (v2026.722.2) — `full` = disk std + sweep + cpu + mem; `--csv`
  derives per-module names in full mode (schemas differ); `--html FILE`
  self-contained styled report (html.c) with pure-CSS bars, rewritten after
  every run; `--quiet` batch mode (progress/notices off, warnings stay,
  implies --plain; `g_quiet` in main.c + `sb_prog_quiet`); menu has
  log/CSV/HTML toggles with default filenames.
- **Network** (v2026.723.0) — client `sunbench net --server HOST` +
  server `sunbench-netd` (`make netd`, run on a modern box; iterative,
  alarm+longjmp timeout guard). ASCII protocol (netproto.h): UP/DOWN/RTT/UDP,
  "GO\n" handshake, fresh TCP connection per test. Tests: TCP up (server's
  received count is the result), TCP down, RTT (1-byte echo, time-bounded),
  UDP size sweep w/ loss% (ephemeral UDP port per test; ENOBUFS = interface
  backpressure, brief select() backoff, NOT loss). UDP ladder capped at local
  MTU by default (sb_net_mtu: SIOCGIFCONF walk + SIOCGIFMTU, guarded
  `#if defined(SIOCGIFMTU) && defined(ifr_mtu)`, fallback 1500; NB the fixed
  ifreq stride is right for Solaris/SunOS, wrong for BSD sa_len systems —
  falls back harmlessly there); `--netmax` overrides for fragmentation.
  Solaris 2.x sockets need `make NETLIBS='-lsocket -lnsl'` (configure prints
  the hint; SunOS 4 + modern = none). Menu prompts for the hostname in cooked
  mode (raw mode dropped around fgets). `full` includes net when --server set.
- **All four planned modules are complete.**

## Build and test

```sh
./configure                 # writes src/config.h; re-run after adding a probe
make                        # cc by default
CC=gcc ./configure && make CC=gcc

# Local (macOS/Linux) sanity check of the ANSI path:
make CC=cc CFLAGS="-std=c89 -O"
```

Local builds only prove the *ANSI* path compiles. Solaris-only features
(`directio`, `sysinfo`, `mnttab`, `iostat`) are absent off-target and their
code paths compile to stubs — real validation happens on Sun hardware.

The K&R-style function definitions produce `-Wdeprecated-non-prototype`
warnings on modern clang. **That is expected and correct** — do not "fix"
them by converting to ANSI definitions; SunOS 4's `cc` requires K&R.

## C dialect rules (mandatory)

- **K&R function definitions.** Arguments listed, then declared:
  ```c
  int foo(a, b)
      int a;
      char *b;
  { ... }
  ```
- **Prototypes via `P_(())`** in headers: `int foo P_((int a, char *b));`
- **Old-style comments only** — `/* ... */`, never `//`.
- **Declare every local at the top of its block.** No mixed declarations and
  code.
- **No `long long`** except behind `HAVE_GETHRTIME`.
- **No `void *` in public APIs** — use `char *` and cast.
- **No `U`/`UL` integer-constant suffixes** — they are ANSI inventions;
  SunOS 4's K&R `cc` rejects them (found by the first real 4.1.4 build).
  Use plain `L`; unsigned semantics come from the variables involved.
- **Never use `sprintf`'s return value** — on SunOS 4 it returns `char *`
  (old BSD), not the ANSI count.  Append with sprintf-then-`strlen`.
- **No unary `+`** (`+1`) — ANSI-only; write `1`.
- **No automatic aggregate initializers** — `sb_cfg t = *c;` and
  `int a[2] = {0,1};` at function scope are ANSI-only.  Declare, then
  assign (struct assignment itself is fine); arrays with initializers
  must be `static`.
- **No adjacent string-literal concatenation** (`"a" "b"`) — ANSI-only;
  K&R cc needs one literal.  Applies AFTER preprocessing too: a macro
  expanding to a literal next to a literal (the old `ESC "[H"` TUI
  idiom) breaks the same way.  Long text blocks: arrays of per-line
  strings (see `help_lines[]` in main.c), and keep source lines
  under ~300 chars for old compiler buffers.
- **Prototypes must use promoted types for K&R-defined functions** — a
  K&R definition receives `char`/`short` as `int` and `float` as
  `double`, so a prototype declaring the narrow type is a conflicting
  redeclaration.  gcc tolerates it; Minix's ACK rejects it (found by the
  first Minix 2.0.4 build, in Dhrystone's `Func_1`).  Write `int` /
  `double` in the `P_(())` list even when the definition says `char`.
- `const`/`volatile` are `#define`d away on non-`__STDC__` compilers by
  `port.h`; don't rely on them semantically.
- `port.h` must be included **first** in every `.c` — it defines
  `__EXTENSIONS__` before any system header, without which Solaris hides
  `gethrtime`/`directio` from gcc and truncates `hrtime_t` to 32 bits.

## Adding a feature probe

1. Add a `try <<'EOF' ... EOF` block to `./configure` emitting `HAVE_XXX`.
2. Guard the code with `#ifdef HAVE_XXX` and **always provide a stub** in the
   `#else` branch so non-Solaris targets still link.
3. Tell the user to **re-run `./configure`** — a new probe means `config.h`
   must be regenerated.

Watch the `#ifdef` nesting in `sysinfo.c`; it has real/stub pairs for
`HAVE_STATVFS`, `HAVE_MNTTAB` and `HAVE_POPEN`, and it is easy to close a
block in the wrong place.

## Memory discipline

Target floor is a **28MB SPARCstation 1**. One reusable page-aligned I/O
buffer; streaming statistics; bucketed histograms. **Never store per-operation
samples** — a latency run emits millions of them.

## Measurement integrity

- Per-operation latency brackets only the `read`/`write` syscall.
- Throughput subtracts time spent in reporter callbacks (`ui_ovh` in
  `sb_disk_run`), so the TUI cannot skew results on a slow console.
- `fsync` on write is real I/O and **stays counted**.
- Cache bypass must be probed on the *scratch* filesystem, never `/tmp`
  (which is tmpfs on Solaris).

## Architecture

```
port.h        dual K&R/ANSI foundation; include first, everywhere
configure     hand-rolled probes -> src/config.h
timer.c       gethrtime() -> gettimeofday() fallback
sysinfo.c     RAM, filesystem, disk device, drive model, log synopsis
diskio.c      cache-bypass method selection, aligned buffers
histogram.c   log2 + sub-bucket latency distribution
bench_disk.c  ONE time-bounded engine for all four access patterns
bench_mem.c   memory bandwidth/latency kernels + working-set sweep + MP
cpu.c         CPU kernels + harness; dhry_*/whet.c the classic benchmarks
html.c        self-contained HTML report (inline CSS, no JS); results are
              FED as runs complete and the file is rewritten on each flush
svgchart.c    inline-SVG line charts, ALL styling in inline attributes so
              charts survive injection into old reports (used by html.c
              and by sunbench-htmlgraph, the retrofit injector)
progress.c    plain reporter (throttled stderr lines)
tui.c         ANSI/vt100 dashboards (run + sweep) and menu; no ncurses
keys.c        termios cbreak input, restored on exit and SIGINT
main.c        CLI, config, dispatch, log/CSV writers
```

`sb_reporter` (in `bench_disk.h`) is the seam: the engine drives
`begin/tick/end`, and either the plain reporter or a TUI reporter renders it.
Add new output modes there rather than inside the engine.

## Git

**The user runs every git command.** The only git command you may run is
`git status`. Never run `init`, `add`, `commit`, `tag`, `push`, `branch`,
`checkout` or `rebase` — write the commands out for the user to run instead.

## Packaging

- `make pkg` — SVR4 datastream package with a **prebuilt binary**, for
  Solaris 2.0–10. Must be run **on a Solaris host**, after `make`, and
  ideally on the **oldest** Solaris in the fleet: Solaris binaries run
  forward (a 2.6 build works on 2.6/7/8/9/10) but never backward.
- `make srctar` — source tarball for SunOS 4 / Solaris 1.x, which has no
  `pkgadd` but does ship a working K&R `cc`. Plain (uncompressed) tar so
  SunOS 4's stock `tar` can read it without gzip.

Both live in `pkg/` as plain Bourne scripts; `pkgmk`/`pkgtrans` are Solaris
tools, so `make pkg` cannot be tested off-target — `make srctar` can.

### Architecture (sun4c / V7)

`sun4c` (SPARCstation 1/IPC/IPX) is SPARC **V7** — no hardware integer
multiply/divide. `sun4m`/`sun4u` are V8/V9. A binary built with V8-only
instructions **traps on sun4c**. For a fleet binary built on a V8 host with
gcc, force V7: `make CC=gcc MCPU=-mcpu=v7` (a V7 binary runs on all three).
`MCPU` is **empty by default** — do not hardcode `-mcpu` into `CFLAGS`, as
SunOS 4's bundled K&R `cc` rejects it (and its machines are all V7 anyway).
Combine with the OS rule: build on the oldest OS *and* for the oldest arch.

## Releasing

CalVer `YYYY.MM.DD-N` (zero-padded month and day; N = that day's release
count, from 1: `2026.11.22-1`, `2026.11.22-2`). Releases before 22 Nov 2026
used `YYYY.MDD.PATCH`. Bump **both**:

- `SB_VERSION` in `src/version.h`
- `VERSION` in `Makefile`

then add a `CHANGELOG.md` entry and update the `.TH` line in `sunbench.1`.

## Delivering to the user's hardware

The user copies a tarball to real Sun boxes by hand. After changes:

```sh
cd <parent-of-repo> && tar -cf sunbench-src.tar sunbench/configure \
  sunbench/Makefile sunbench/*.md sunbench/sunbench.1 sunbench/src/*.[ch]
cksum sunbench-src.tar
```

Always give the user the **`cksum` value** so they can verify the transfer
before building — a stale tarball has already caused a confusing debug cycle.
Say explicitly whether `./configure` must be re-run (i.e. whether probes
changed).

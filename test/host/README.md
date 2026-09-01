# Host test harness for `src/z3660.c`

Compiles the **real** driver source on host `gcc` against a mock piscsi mailbox
and byte-checks its CD-ROM and disk CDB synthesis — no Amiga, no firmware, no
emulator. Catches CDB-shape regressions (the kind that only surface at a real
`mount`) in a sub-second host build.

```sh
make -C test/host check     # build + run; exit 0 == all gating assertions pass
make -C test/host clean
```

## What it exercises

Current run: **95 gating assertions passed, 0 failed**, plus 6/6 stretch parity
against the firmware's CD oracle.

`z3660queue()` is driven with a hand-built `struct sdcom` and completes
**in-context**: since a5af58a the driver delivers through `z3660_complete()`
before `z3660queue()` returns — there is no `timeout()` deferral any more, and a
driver-owned FIFO flattens `dd.c`'s completion→re-issue recursion (`NOTES.md`
2026-07-12). `kstubs.c` still defines a `timeout()`, but only as an inert stub of
the historical kernel service; nothing calls it.

The driver is compiled **unmodified** apart from one inert seam: its `WRLONG`/
`RDLONG` MMIO macros are wrapped in `#ifndef HOST_TEST`, and the harness
force-includes `mock_regs.h` (`-include`) to route every register access into
the mock instead of a real volatile dereference. Without `-DHOST_TEST` the
driver is byte-for-byte unchanged (the kerntools build compiles it via
`driver.conf` exactly as before).

Gating (mount-critical, gate the exit code):

- CD-ROM (`pdt=0x05`): INQUIRY (36-byte match), READ CAPACITY(10), READ(10)
  single/multi-sector and at a nonzero LBA (block-index scaling), WRITE(6)/
  WRITE(10) rejection → CHECK CONDITION + DATA PROTECT sense (consumed-on-read).
- Disk (`pdt=0x00`): INQUIRY / READ CAPACITY / READ(10) / WRITE(10) byte
  behavior **frozen** as the current driver emits it (the real box boots on
  this path; the oracle's disk strings do *not* apply here).
- TEST UNIT READY GOOD for both.
- Board mapping: `z3660map()` takes the **direct** (section-0 identity) arm and
  calls no `sptalloc()` — see below.
- **Per-CDB round-trip budget** — see below.
- Completion trampoline: a 50-deep re-issue chain through a *persistent* `sdcom`
  (mimicking `dd.c`'s `&dp->com` + `ihandle`) — all 51 completions delivered,
  nesting depth ≤ 2 (iterative, not one frame per I/O), FIFO never overran, no
  mailbox re-entry.
- `spl6` bracket: a simulated clock callout fired from inside an in-flight
  mailbox command is deferred; with the bracket artificially removed
  (`mock_spl_disabled`) the nested transaction is counted and the outer READ is
  visibly corrupted.
- **BLIZZARD F3 cache maintenance** (18 gates, see `../../docs/BLIZZARD-F3.md`):
  that `z3660_cache = 0` — the shipping default — reaches no line op on any path;
  that each maintenance boundary pushes exactly its chunk and only there
  (per chunk, not per command); that the READ **bounce** arm must *not* invalidate,
  because it would discard the `bcopy`; that the WRITE-side staging branch stays
  unreachable (`z3660_bounce_wr_n == 0`); that the S11 page-cross census counts,
  does **not** refuse, and still runs with the gate off; and both arms of the F3-M0
  free-ride assertion — the shipped `DTT0` passes, a cacheable or an uncovered TTR
  fails, and the `sptalloc` arm is refused under `z3660_ci_enforce`.

Page-size neutrality is re-run by hand rather than gated: set `NBPP` in
`stubs/sys/immu.h` and `F3_NBPP` in `z3660_test.c` to 4096 and the same 95
assertions pass, so the derived window geometry *and* the datapath are
page-size-agnostic (re-verified 2026-09-01).

## Round-trip budget: why register accesses are the unit of cost

On real hardware every mailbox register access is a **cross-core round trip**, not a
bus cycle: core1 (the guest's own CPU) publishes the access and then hard-spins,
retiring no 68k instructions, until core0 services it from a cooperative protothread
loop that handles at most one access per iteration (`../Z3660`
`docs/piscsi-service-path.md` §1.1–1.3). A command's cost is therefore set by how
many registers it touches, not by how many bytes it moves — so the mock counts every
`WRLONG`/`RDLONG` (`mock_trips*`, reset by `mock_trips_reset()`), and that count is a
faithful, mock-independent proxy for metal cost.

`test_hotpath_trip_budget()` gates the exact per-CDB totals — **6** for a 2 KB
READ(10), **5** for a WRITE(10) — and, separately, that the three static per-unit
geometry registers (`BLOCKSIZE0+4n`, `BLOCKS0+4n`, `PDT`) are **not touched at all**
on the command path, and that `DRVNUMX` is written exactly once. Before the per-unit
geometry cache those figures were 10 and 9, with a redundant second `DRVNUMX` write.
The named category counters matter: a regression that reinstated one re-fetch fails
by name rather than as an opaque total. `test_direct_mapping()` additionally gates the
probe's breakdown (8 × BLOCKSIZE, 8 × BLOCKS, 8 × PDT) to pin the cache fill to
**attach**, so it cannot silently drift back into the command path.

A cheap-but-wrong cache would pass every count, so the same gate re-checks READ
CAPACITY(10)'s reported block size and block count against the mock's geometry while
asserting it cost no wire reads.

Stretch (reported, never gated): a parity table of the driver against the CD
half of the firmware oracle
(`Z3660_emu/test/host/scsi_cd_test.cpp`), including the MODE SENSE(6)/(10) rows.

## LOCKSTEP contract

`mock_piscsi.c` is a mock of the **firmware** register semantics — the firmware
owns the protocol. Its behavior is transcribed register-by-register from

- `Z3660/.../Z3660/src/scsi/scsi.c` — `handle_piscsi_reg_write()` (~1043),
  `handle_piscsi_reg_read()` (~1600)
- `Z3660/.../Z3660/src/scsi/z3660_scsi_enums.h` — register offsets

and mirrored by `Z3660_emu/src/uae/a3000_scsi.cpp`. The register offsets are a
local copy (the firmware header is not included — same discipline
`a3000_scsi.cpp` follows); if the firmware renumbers a register, **both**
`src/z3660.c` and `mock_piscsi.c` must change together. Notable stateful
semantics reproduced: `PDT`/`DRVTYPE` depend on the last drive select, and a
`BLOCKSIZE0+4n` / `BLOCKS0+4n` read *reassigns* the current drive as a side
effect.

Pointer handling: on real hardware the **caller** hands the driver a physical
address — the stock disk path at `amiga/alien/dd.c:240`, and `amix-cdfs`'s
`amix_kern_media.c`, both `vtop()` before filling `sc.addr`. The driver stores
`cp->addr` straight into `*_ADDR3` and **never translates it**; adding a `vtop()`
inside `z3660.c` would double-translate, and was evaluated and rejected as a
non-fix (`NOTES.md` 2026-07-11 §(a) — do not resurrect it). Under `HOST_TEST`
that same contract makes `vtop()` effectively the identity (the test stores the
raw host buffer pointer in `cp->addr`) and the mock passes it through unmapped —
register cells are `unsigned long` so a 64-bit pointer survives the `*_ADDR3`
slot (LP64 hosts only).

## Board mapping: the harness exercises the arm the metal box runs

`z3660map()` has two arms. Below `VSECT1` (`0x40000000`) the board's physical
address already *is* a valid supervisor VA — AMIX identity-maps the low 1 GB
through a section-0 early-termination descriptor — so the driver dereferences it
and calls nothing; at or above `VSECT1` it falls back to `sptalloc()`. The
shipped config (`autoconfig_rtg NO`, fixed base `0x10000000`) takes the direct
arm, and `mock_piscsi.c`'s `autocon()` reports that same base, so **the harness
tests the shipping path**.

The host has no identity map, so `phystokv()` is routed through the mock
(`z3660_mock_phystokv()` in `mock_piscsi.c`, wired in `stubs/sys/immu.h`) and
returns the same `g_regs`/`g_bounce` buffers the `sptalloc()` stub returns. Both
arms therefore land on the mock's buffers — which is exactly why
`test_direct_mapping()` gates on the driver's `z3660_direct_map` flag: without
it, a regression that silently reverted to `sptalloc()` would leave every other
test passing. Moving `MOCK_BOARD_BASE` to `0x40000000` flips the flag and fails
three gates — the direct-map gate itself, plus the two F3-M0 assertions that key
off it — while the other 92 pass, which is also how the retained high-base arm
stays proven.

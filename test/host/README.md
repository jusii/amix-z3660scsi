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

`z3660queue()` is driven with a hand-built `struct sdcom`; completion arrives
through `cp->intr` (the harness's `timeout()` fires it synchronously). The
driver is compiled **unmodified** apart from one inert seam: its `WRLONG`/
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

Pointer handling: on real hardware the driver hands the firmware a 32-bit
physical address from `vtop()`. Under `HOST_TEST` `vtop()` is identity (the
caller stores the buffer pointer straight into `cp->addr`) and the mock passes
that host pointer through unmapped — register cells are `unsigned long` so a
64-bit pointer survives the `*_ADDR3` slot (LP64 hosts only).

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
that one gate while the other 51 still pass, which is also how the retained
high-base arm stays proven.

## 2026-08-17 — z3660: per-unit static-geometry cache — 10 → 6 round trips per 2KB READ (29b2482)

A piscsi register access is a CROSS-CORE ROUND TRIP, not a bus cycle: core1 (the guest's own
CPU) hard-spins, retiring no 68k instructions, until core0 services it from a cooperative
protothread loop handling at most ONE access per iteration (../Z3660 docs/piscsi-service-path.md
§1.1–1.3). Cost is set by registers touched, not bytes moved — and steady-state granularity is
2KB (physio stages through a 2KB buffer; NBPP=2048), not the protocol's 64KB MAXXFER. Nine of
the ten trips a 2KB READ cost carried no payload; four were pure waste, re-fetched per CDB:
BLOCKSIZE0+unit*4, PDT, BLOCKS0+unit*4, and a redundant second DRVNUMX inside z3660_pdt().
Now cached per unit, filled in one sweep of all 8 units at attach (z3660map()).
Invalidation rationale is a RESET argument: all three facts are devs[unit] fields written only
by piscsi_map_drive(), reachable only via piscsi_init()/the drive refresh, and BOTH run with the
68k held in reset — which reloads this kernel and zeroes the driver's BSS. The cache cannot
outlive the facts it caches; no runtime invalidation hook is needed or even reachable (no wire
signal announces a remap). The one fact a rescan CAN change — whether a unit has a drive mapped
— is deliberately left UNCACHED (valid only when nblocks != 0), so a unit empty at attach is
re-probed on every command and is still REFUSED, not silently no-op'd.
Wire protocol unchanged: no new register, no new command, no firmware change. Ordering preserved
(PDT reflects the last select, and BLOCKSIZE0/BLOCKS0 reads reassign piscsi_cur_drive as a side
effect, so the fill does both array reads before the DRVNUMX select + PDT read); the surviving
per-command DRVNUMX write keeps the firmware's val != piscsi_cur_drive warning silent.
Measured per CDB: READ(10) 2KB 10→6, WRITE(10) 9→5, READ CAPACITY 5→1, attach probe 2→34 (once),
unmapped unit 4→5 (deliberate). 77/77 gating (52 pre-existing + 25 new), 6/6 parity. Metal
throughput NOT proven — the harness proves the trip count fell and the cached facts stay correct,
not core0 loop latency. Reaches the box at the next kernel relink.

## 2026-08-14 — z3660: direct board mapping below VSECT1 (8293cdc)

z3660map() takes the physical base as the kernel VA when the whole span stays under
0x40000000 (section-0 early-termination identity map — AMIX never enables the TT registers);
sptalloc kept only for a base at/above VSECT1. BOTH windows go direct — the bounce window is
the FIRMWARE's own staging area at board+0x80000 (SCSI_NO_DMA_ADDRESS), a mapping not an
allocation. sptmap draw at the metal base: 33 pages → 0 (with z3660net's fix: 98 pages
returned to the 2048-page pool). Falsification-tested (52/52 gating; moving the mock base
flips z3660_direct_map and fails exactly one gate). Reaches the box at the next kernel
relink/golden regeneration.
# Changelog

## 2026-07-12

- **z3660: complete commands synchronously in-context (fixes the T2.P3 cdfs mount panic).**
  The driver used to defer completion to a clock callout — `timeout(z3660done, cp)` — which
  runs in a *different* context from the caller that issued the request. Callers such as the
  cdfs mount path pass a **stack-allocated** `struct sdcom`; by the time the callout fired,
  that caller's frame was long dead, so `cp->intr` was read out of a reclaimed stack and
  jumped through. On the real A4000+Z3660 that surfaced as guest-kernel memory corruption and
  a wild-jump panic the moment `mount -F cdfs` touched the CD.
  Completion is now **synchronous and in-context**: the piscsi mailbox op is itself
  synchronous, so `(*cp->intr)(cp)` is invoked before `z3660queue()` returns, while the
  caller's frame — and therefore its `sdcom` — is still alive. It cannot be a bare inline call,
  because dd.c's disk completion *re-issues* the next I/O (`ihandle` → `startio` → `sdqueue` →
  `z3660queue`), which would recurse one kernel-stack frame per chunk and blow the small SVR4
  kernel stack on the first big multi-chunk burst. A driver-owned completion FIFO plus a
  `z3660_completing` guard **flattens** that recursion iteratively instead. `z3660_cq_overflow`
  is a silent kmem-readable guard on the FIFO (must stay 0).
  Validated on real hardware: `mount -F cdfs 0,2 /cdrom` → rc=0, the full R4 read suite
  byte-identical to the ISO (7/7 files verified host-side), clean umount + remount, and the
  reentrancy/FIFO guards all read 0 after a green suite.

## 2026-07-11

- **z3660: bracket the whole mailbox transaction at spl6 + permanent nest detector.** The
  driver's single shared mailbox + bounce window carried **zero** spl masking, while the
  Amiga clock dispatches callouts above the `sdspl` (spl2) that every other SCSI caller
  guards with — so a completion could nest a second transaction on the mailbox mid-transfer
  and scramble it. `z3660_enter()`/`z3660_leave()` now bracket the entire transaction at spl6.
  The permanent, silent counters `z3660_nest_depth` / `z3660_nest_hits` stay in the driver as
  kmem-readable evidence (both must remain 0). Verified by host disassembly of the stock
  kernel's callout dispatch (callouts run at IPL4; the sole trigger is the CIA-A clock at
  level 2) — the hole was this driver's unguarded transaction, not the sdspl contract.

## 2026-07-10

- **z3660: host CDB test harness — the real driver vs a mock mailbox.** Compiles the actual
  `src/z3660.c` host-side against a lockstep mock of the piscsi mailbox, so CDB synthesis and
  the completion path are gated without a bench cycle. 49/49 gating tests plus a 6/6 CD-oracle
  parity suite; it is what let the metal debugging skip the driver's own logic and go straight
  at firmware/integration.

- **z3660: answer CD-ROM MODE SENSE(10) (CD branch only).** Rounds out the CD command set the
  Amix `sd`/`cdfs` stack probes with; the disk path is untouched.

## 2026-07-07

- **z3660: present a CD-ROM when the firmware flags a unit (pdt 0x05).** The Z3660 firmware
  marks a backend unit as a read-only 2048-byte CD-ROM via the piscsi `PDT` register; the
  driver now reports that peripheral device type up the SVR4 SCSI stack, which is what lets an
  in-kernel filesystem (`cdfs`) mount the unit at all.

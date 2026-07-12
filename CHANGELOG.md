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

# Changelog

## 2026-09-26 — stock-firmware hardening: units with a bad block size are skipped (70f54b6)

z3660_geom_fill() reads BLOCKSIZE first and skips a unit whose block size is not a power of two in
512..4096 or whose block count is 0, never reading BLOCKS for it — stock firmware (without the fork's
c0510a7) divides by zero there. Skipped units answer "absent", get no doorbell, are named once.
Harness 131/131 (was 95), parity 6/6; object 8004 B, same undefined-symbol set. Kernel relink needed.

## 2026-09-26 — docs tidy: STATUS header current, testing path built, boot-fsck open (9fa05a5, 240d863)

z3660.c STATUS header reflects the 2026-06-13 metal boot (comment only; object byte-identical on
three builds). NOTES: the Amiberry Z3660 SCSI emulation exists; the bench rig's stale +cdfs kernel gap
is recorded; boot-fsck marked open as of 2026-09-26. BLIZZARD-F3 cites no tmp/ path.

## 2026-09-01 — docs: NOTES retracts the two cache claims BLIZZARD F3 falsified; journal to HEAD; README status 2026-08; test/host described as it is (daf6c6b, 9a9f280, a374476, cacc989)

Doc/comment-only. NOTES.md retracts the two 030 cache claims the BLIZZARD F3 metal round falsified and
brings the journal up to HEAD with its reader pointers fixed; README states the 2026-08 status and a layout
block that lists what exists; test/host describes the harness the driver actually has (post-a5af58a).

## 2026-08-24 — z3660: BLIZZARD F3 — CPU data-cache coherence for real 68040/68060 silicon (683559d 900f095 0ced4a1 d90f5fd f517811)

This driver has never contained a cache instruction, and that was correct: every deployment so
far has run on an EMULATED CPU (the Z3660 carries the guest 030/040 on core1's interpreter; the
socketed 68LC060 has been a bus-parked passenger), and an interpreter has no data cache. F4 puts
the same object on silicon that has one, where the same program is wrong in both directions — a
WRITE hands the firmware a physical page whose newest bytes are still in the CPU's cache, and a
READ lets the ARM overwrite a page whose stale dirty lines are later evicted over the fresh
bytes. The round is pre-registered in `docs/BLIZZARD-F3.md` (683559d), written before any code
and not rewritten to match the result.

The central finding corrects a reading this file itself invited: **DTT0 is a LOGICAL-address
TTR.** `pstart040.s:326` loads `0x003fc060` — the low 1 GB cache-inhibited for data — which
covers THIS DRIVER'S accesses (`cp->addr` is a physical address by contract, dereferenced
through the low identity alias) and the board window, and covers nothing about the same physical
page's other aliases. User pages are copyback (`hat040.s` `hat_cm_ram = 0x20`, default since
2026-07-30), and that is exactly where a raw-I/O buffer lives. Nor does anything else save us:
/SNOOP sees 68k local-bus cycles only, and the piscsi path has the ARM writing Zynq DDR from its
own memory system. Two load-bearing comment falsehoods went with it (900f095, comment-only, the
cross-compiled object proven BYTE-IDENTICAL): the header's "Amix RAM is < 0x08000000 so the
firmware always bounces" is false because `0x08000000` IS `AMIX_RAM_GUEST_BASE` — nothing in AMIX
RAM is ever staged, so every "the bounce keeps us coherent" argument is void — and the mapping
note's "this kernel has no PG_CI bit at all" is a 68030 fact restated as universal, when the
040/060 leaf PTE carries a CM field the kernel already writes.

**F3-M0 (0ced4a1) asserts the free ride instead of depending on it silently.** DTT0/DTT1/CACR are
read at attach (raw `.word` `movec`, verified by disassembly against gcc 2.7.2.3 / GNU as 2.8.1)
and both endpoints of the 0x90000-byte window are classified for supervisor data; TTR granularity
is 16 MB, so a window that straddles at most one boundary is exhaustively covered by its two ends.
A covering-but-cached TTR fails, and so does an address no enabled TTR covers — it would fall to
page tables whose cache mode this driver cannot read and must not assume. The sptalloc mapping arm
is a separate and worse failure: per-map device cache mode is still `hat040.s`'s deferred TODO, so
that window would take `hat_cm_ram`, i.e. COPYBACK — not a slow mailbox, a broken one. It is
declared UNSUPPORTED on real silicon and counted (`z3660_sptalloc_unsafe`), not silently taken.
The default verdict is to measure loudly and carry on; `z3660_ci_enforce` turns it into ENXIO,
because a first-silicon boot that refuses its own root device tells you nothing.

**F3-M1 (d90f5fd) puts the maintenance at `z3660_rw()`'s chunk loop — not at
`z3660_enter`/`z3660_leave`/`z3660_complete`**, which the plan had named. Those bracket the whole
CDB including the synthesized commands, so a FROM_DEVICE invalidate hung there would fire on
INQUIRY and REQUEST SENSE buffers the CPU itself just wrote. The chunk loop alone knows direction,
physical address and length per chunk, and sits inside the existing spl6 bracket. `cpushl`+`cinvl`
runs before every doorbell — before the WRITE-side staging `bcopy` as well, since that `bcopy`
READS through the CI alias and would stage stale RAM otherwise — and `cinvl` runs after the
completion of a READ **only on the direct-DMA arm**. It must NOT run after the bounce READ or
after the five synthesized responses (REQUEST SENSE 18, INQUIRY 36, READ CAPACITY 8, MODE SENSE(6)
12, MODE SENSE(10) 16 bytes, all sub-cache-line), where a 16-byte-rounded `cinvl` over a
CPU-written range would DISCARD the bytes just written along with their line neighbours; those
ranges get a single push-and-invalidate first instead. Every push is followed by an unconditional
`cinvl` because the 68060 makes CPUSHL's invalidation conditional on CACR.DPI, and one sequence
is then correct on both parts with no CPU-class branch. The line ops select by PHYSICAL line on a
physically-tagged cache, so one op covers every virtual alias — including the copyback user
mapping the buffer actually lives in — and this driver already holds the physical address and is
forbidden to `vtop` it, so it needs no address conversion at all. The S4 doorbell/readback barrier
is placed and gated now, before the store buffer that arms its hazard is turned on; that `nop`
serializes pending writes on the 68060 is recorded as ASSUMED, owed a read of the MC68060UM. The
S11 page-cross census COUNTS and deliberately does not refuse: the raw path's page-boundedness has
never been instrumented, and this driver is the root device of two kernel lines, so the counter
decides in either direction (0 across a boot and an install makes it a permanent refusal;
non-zero means the chunk needs SPLITTING at the page boundary, not refusing).

The whole round is gated on `z3660_cache` (default 0 = off, 40 = 68040 DC, 60 = 68060 DC), poked
through `/dev/kmem` exactly like the port lane's `hg_on` / `i40_on` / `hat_cm_ram`. There is no
compile-time discriminator available — this file is compiled once with `-m68020` into kernels for
an emulated 030, an emulated 040 and a real 060 — and `cputype` may not be referenced, because the
stock kernel has no such symbol and an extern would break the `nm -u` clean gate on the 030 line.
As landed the definition is tentative, so the knob is COMMON in `.bss` and is NOT file-pokeable
from this repo; a build wanting a non-zero default absorbs the COMMON out of tree (f517811
corrects the design doc, which had shown `= 0`). Shipping-path proof, in increasing strength:
`nm -u` unchanged but for `printf`; every `f468`/`f448`/`4e71`/`4e7a` site proven dominated by a
`tstl z3660_cache` / `beq` in the cross-compiled disassembly; host harness **95 gating passed /
0 failed** (was 77/0) and 6/6 oracle parity, the 18 new gates covering the gate-off path, each
maintenance boundary, the READ-bounce no-invalidate rule, the S11 census and BOTH arms of the M0
assertion. New kmem-readable symbols: `z3660_cache`, `z3660_ci_enforce`, `z3660_dtt0/dtt1/cacr`,
`z3660_ci_ok`, `z3660_sptalloc_unsafe`, `z3660_push_n/inv_n`, `z3660_push_bytes/inv_bytes`,
`z3660_bounce_wr_n/rd_n`, `z3660_pagecross_n`, `z3660_range_ovf`.

**Nothing here is evidence about a real cache.** Everything above is proven against the toolchain,
the object and the host harness; the metal A/B pre-registered in §7 — arm A (`z3660_cache = 0`)
expected to FAIL, 15/15 clean cycles and 8/8 byte-identical canaries on arm B, with
`z3660_push_n > 0` as a gate so a green run with a zero counter counts as failed — is still owed,
and the bench structurally cannot run it (Amiberry models no 040/060 copyback data cache). One
adjacent fact does now exist: on **2026-08-26** this driver served the AMIX root disk on a real
**68LC060** for the first time, three boots for three to multiuser at cpufreq 80 with live piscsi
I/O (campaign evidence is in the workspace's dated scratch directory, not published; see NOTES.md §2026-08-26) — but on the free ride alone. `z3660_cache`
was never poked and no F3 counter was read, so that run is a first, not a result.

## 2026-08-18 — z3660: page-size-agnostic board-window geometry (0bd3f10)

The driver stated its window geometry as a page COUNT (BOUNCE_PAGES 32, span = BOUNCE_PAGES *
NBPP), which reads as page-size-aware but is inverted: the fixed quantity is the firmware's 64KB
bounce aperture at board+0x80000, and the page count is what must move when NBPP does. On a
4KB-page kernel the same source silently keeps 32 pages — BOUNCE_SPAN doubles to 0x20000 and
Z3660_WINDOW_TOP moves to 0x000A0000, i.e. 32 sptmap pages claimed where 16 suffice (the same
scarce 2048-page map whose exhaustion broke z3660eth past ~83MB) and a direct-map admission test
64KB wider than the window the firmware has. Inverted: BOUNCE_BYTES (0x10000) is primary,
BOUNCE_PAGES = Z3660_PAGES(BOUNCE_BYTES) rounded up, BOUNCE_SPAN stays 0x10000 at any page size.
The register window's unnamed page count (the literal 1 passed to sptalloc) got the same
treatment — REGS_BYTES = P_WRITE_ADDR3 + 4, REGS_PAGES derived from it. Byte quantities that never
carried an NBPP term are unchanged; Z3660_NUNITS and Z3660_CQ are a register-array depth and a
FIFO depth, not page quantities, and stay counts. Seven #if/#error guards pin the result: each
page count asserted from BOTH sides (count == ceil(bytes/NBPP), so re-hardcoding is a compile
error in either direction), Z3660_WINDOW_TOP asserted 0x00090000 independently of NBPP, both board
offsets asserted page-aligned (sptalloc maps by frame, phystopfn truncates), one MAXXFER chunk
asserted to fit the aperture. Evaluated with the target compiler: NBPP 2048 → 32 / 0x10000, NBPP
4096 → 16 / 0x10000 (pre-change: 32 / 0x20000). No behavioural change on the shipping kernel — the
cross-compiled object is BYTE-IDENTICAL to the previous one, nm -u unchanged (autocon, bcopy,
sptalloc). 77/77 gating + 6/6 parity, and 77/77 again with the harness page size set to 4096.
Reaches the box at the next kernel relink.

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

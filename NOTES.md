# Z3660 SCSI → Amix driver — notes

(Repo `amix-z3660scsi`; builds go through the sibling `amix-kerntools` harness +
golden image — `(cd ../amix-kerntools && ./amix-build z3660scsi)`. Started as
scouting notes; §"Implementation status" is the living state.)

Goal: a native Amix (SVR4/68030) driver for the **Z3660** accelerator's onboard SCSI, so Amix on a real
A4000+Z3660 stops relying on the buggy A3000-WD33C93 emulation. Mirrors the A4091 effort: develop in
Amiberry, validate on real hardware. Source of truth: the open-source `z3660-drivers/scsi/` in
[shanshe/Z3660](https://github.com/shanshe/Z3660) (cloned to `repo/`).

## Headline: route A is *easier* than the A4091

The Z3660's native SCSI is **not** an NCR 53C710 — it is the **PiStorm `piscsi` mailbox protocol**, ported
to the Z3660's Zynq ARM. There is **no SCSI chip, no SCRIPTS, no DSA, no bus-phase management, no
interrupt/poll completion machinery**. It's a tiny synchronous MMIO register mailbox. The `siop_softc` /
`a4091` symbols in `z3660_scsi.h` are leftover device/boot-ROM scaffolding (the AmigaOS device wrapper +
autoboot ROM came from the a4091.device tree), **not** the transport. ✅ (read from source)

## The protocol (Amiga-facing — all we need to port)

- **AutoConfig identity:** manufacturer **`0x144B`**, product **`0x01`** (`FindConfigDev(0x144B,0x1)` →
  `cd->cd_BoardAddr` = `Z3660_REGS`). It's a *real* AutoConfig board, so it appears in Amix's
  `bootinfo.autocon[]` — no synthetic probe needed (unlike the A3000 phantom). ✅
- **Register window:** 32-bit MMIO at `board_base + PISCSI_OFFSET(0x2000) + cmd`. Access is plain
  `*(volatile uint32_t*)`:
  ```c
  #define WRITELONG(cmd,val) *(volatile uint32_t*)(Z3660_REGS + 0x2000 + (cmd)) = (val)
  #define READLONG(cmd,var)  var = *(volatile uint32_t*)(Z3660_REGS + 0x2000 + (cmd))
  ```
- **Command set** (`z3660_scsi_enums.h`, offsets are the `cmd`): geometry/probe `DRVNUM=0x08`,
  `DRVTYPE=0x0C`, `BLOCKS=0x10`, `CYLS=0x14`, `HEADS=0x18`, `SECS=0x1C`, `BLOCKSIZE0..17`/`BLOCKS0..17`
  (per-unit, up to `NUM_UNITS=18`); block I/O `READ=0x04`/`WRITE=0x00`, `READBYTES=0x88`/`WRITEBYTES=0x8C`,
  `READ64=0x64`/`WRITE64=0x60`; transfer params `READ_ADDR1..4 = 0x20/24/28/2C`,
  `WRITE_ADDR1..4 = 0x240/244/248/24C` (= block-offset, length, **buffer address**, io_actual);
  `USED_DMA=0x9C`; `DRVNUMX=0x90`. Plus partition/FS helpers (`GETPART`, `NEXTFS`, `LOADFS`…) we can ignore.
- **A READ/WRITE is:** write `DRVNUMX`(unit) → write the operation's **own** address triple (read and
  write use *separate* registers, never shared): a **READ** uses `READ_ADDR1/2/3 = 0x20/0x24/0x28`, a
  **WRITE** uses `WRITE_ADDR1/2/3 = 0x240/0x244/0x248` — `ADDR1`=block number, `ADDR2`=byte length,
  `ADDR3`=buffer addr → **write the command register** (`READ=0x04`/`WRITE=0x00`, value = unit). That
  single command-register write is the **trigger AND the completion**: the ARM intercepts the Zorro III
  bus cycle and finishes the whole transfer before the write returns. **No poll, no IRQ.** ✅
- **Data movement — bounce buffer at `board_base + 0x80000`:** the ARM accesses the Amiga buffer at the
  address in `*_ADDR3` directly when it can; for **low/chip RAM (`< 0x08000000`)** — and in **EMU mode,
  where direct Zorro-III DMA "is not implemented"** (KNOWN_ISSUES) — it bounces through a buffer at
  `board_base + 0x80000`:
  - WRITE: `if (data < 0x08000000) memcpy(board+0x80000, data, len);` then issue the command.
  - READ: issue the command, `READLONG(USED_DMA)`, and `if (used_dma) memcpy(data, board+0x80000, len);`
  - `PISCSI_MAX_BLOCK_SIZE = 65536` → transfers chunk to ≤64 KB. ✅
- **Cache coherency:** the AmigaOS driver wraps the command write in `CachePreDMA`/`CachePostDMA` (the ARM
  touches the Amiga's RAM buffer). Amix equivalent needed — or, if we **always bounce through
  board+0x80000** (MMIO, not cached system RAM), the coherency problem largely disappears. 🟡 (verify on HW)

## How it maps onto the Amix SCSI framework (the port)

1. **`autocon()` / `support.c`:** nothing exotic — the board is a normal AutoConfig device, so it's already
   in `bootinfo.autocon[]`; the universal kernel just needs it in the registry.
2. **`sd.c` `scsicard[]`:** add `0x144B0001, &z3660queue, "Z3660 SCSI"` (same mechanism as the A4091 row).
3. **Map the board:** `sptalloc()` the `cd_BoardAddr` (+ the 0x2000 register window and the 0x80000 bounce
   window). Same TT-gap-safe approach as the A4091 — works whether the board lands in the 0x40000000 gap or
   the TT1-mapped 0x80000000 range.
4. **`z3660queue(c, cp)`:** translate the Amix `sdcom` CDB → PISCSI ops:
   - `READ_10`/`WRITE_10` → set `DRVNUMX` + `*_ADDR1..3` + bounce + command register. (The natural,
     simplest implementation: **always bounce through board+0x80000** in EMU mode → pure PIO, no
     physical-address or cache concerns.)
   - `INQUIRY` / `READ_CAPACITY` / `TEST_UNIT_READY` / `MODE_SENSE` → either PISCSI raw-CDB passthrough
     (see `piscsi_scsi()` — **not yet read**) or synthesize from the `DRVTYPE`/`BLOCKS`/`BLOCKSIZE`/`CYLS`
     geometry registers. **Open question — decides INQUIRY handling.** 🟡
5. Completion is synchronous → the driver's `intr()`/done path is trivial (no SCRIPTS/ISTAT dance). This
   removes the single hardest A4091 problem (emulation-vs-real completion timing).

## Why this is lower-risk than the A4091 driver we already shipped

- No 53C710 / SCRIPTS / DSA / phase dispatch — ~5 register pokes per I/O.
- Synchronous completion (the bus cycle blocks) — no poll/IRQ/timing fragility.
- Real AutoConfig board — no phantom/synthetic detection.
- The boot-breaker, build env, device-numbering, install-to-boot-partition, and universal-autodetect
  machinery are **already solved** from the A4091 project.

## The Amiberry dev-loop opportunity (high leverage)

Unlike the 53C710, this mailbox protocol is **trivial to emulate in Amiberry**: a device that intercepts
writes to `board+0x2000+cmd`, backs `READBYTES`/`WRITEBYTES` with a hardfile, and exposes the bounce window
at `board+0x80000`. If we build that (~a few hundred lines in Amiberry, possibly cribbed from PiStorm's
reference `piscsi`), we get the **A4091-style full-emulation dev loop back** and use the real A4000+Z3660
only for final validation — exactly the workflow that made the A4091 fast.

## Open questions (before/at design time)

1. `piscsi_scsi()` (the raw-CDB path) — does PISCSI accept arbitrary CDBs, or must we translate the few
   CDBs Amix issues? (Read `z3660_scsi.c` lines ~473+.)
2. Cache coherency in EMU(030) mode on real metal — is always-bounce sufficient, or do we need an Amix
   cache flush around the command write?
3. Direct-vs-bounce: always-bounce (simplest, PIO) vs direct ARM access for fast RAM (faster). Start with
   always-bounce.
4. Protocol stability across Z3660 firmware versions (target a documented baseline; the interface looks
   stable — it's the frozen PiStorm piscsi command set).
5. Board base range on a real A4000+Z3660 (TT-gap vs TT1) — affects nothing functionally (sptalloc), but
   good to know.

## Implementation status (2026-06-07)

**Driver written, integrated, clean-built, and boots.** ✅ (everything except the actual hardware mailbox,
which Amiberry can't exercise — that waits on the emulator below or real HW.)

- **`src/z3660.c`** — the Amix driver. `z3660map()` (autocon `0x144B0001` → sptalloc the 0x2000 register
  window + the 0x80000 / 64 KB bounce window), `z3660_blocksize`/`z3660_nblocks` (per-unit geometry regs),
  `z3660_rw()` (chunked ≤64 KB block I/O via the synchronous command-register write + bounce), and
  `z3660queue()` (interprets TEST_UNIT_READY / INQUIRY / READ_CAPACITY / MODE_SENSE in software and routes
  READ/WRITE 6/10 to `z3660_rw`). Multi-byte SCSI fields written byte-wise big-endian (68030 alignment-safe).
- **`driver.conf`** — declares the mechanical wiring (`0x144B0001 z3660queue "Z3660 SCSI" z3660.c
  probe=z3660present`). kerntools generates the `sd.c` `scsicard[]` row + `z3660queue` extern (+
  `sdcardbase()`) and the `alien` Makefile `z3660.o` `OBJ` entry from it, so this repo ships no copy of
  those files. No `support.c`/`autocon()` change is needed — the Z3660 SCSI is a normal AutoConfig board,
  so the generic table search finds it. (`SDCARDS=2`, so ≤2 controllers register at once — fine for an
  A4000+Z3660.)
- **`src/kernel-patches/dd.c.patch`** — a unified diff against stock `amiga/alien/dd.c` (issue the next
  queued buf before `iodone()` in `ihandle()`; see the real-HW findings below). kerntools applies it to
  the stock tree with `patch`, idempotently.

Verified on the Amix build box (`../amix-a4091/hdf/Amix-dbg.hdf`):
- `z3660.c` compiles clean with the K&R SVR4 `cc` on the first try (`cc -c -O`).
- `ld -r` links it into the `alien` `exp` with `z3660queue` resolving — no unresolved symbols.
- Full kernel clean-gated (boot-breaker): converged in 4 relinks, **`sum -r` = 44396**, `checkunix`
  symtab-clean (7320 syms). Installed to the dbg boot partition and **cold-booted to multiuser (run-level
  2)** — the integrated kernel boots; with no Z3660 present, `autocon(0x144B0001)` returns 0 and
  `z3660queue` is never called (harmless), exactly like the A4091 driver when no A4091.

**What is NOT yet verified:** the actual PISCSI mailbox conversation (register writes, the synchronous
command trigger, the bounce copy, geometry/READ/WRITE against a real backing store). That needs either the
Amiberry emulation below or the real A4000+Z3660.

## Testing path: an Amiberry `piscsi` emulation (the A4091-style fast loop)

The protocol is trivial to emulate — it's exactly the shape Amiberry/WinUAE already implement for the
**a2065** (an AutoConfig board with MMIO registers, which this host even uses for networking). Plan:
- Register an AutoConfig board, **manufacturer 0x144B / product 0x01**, sized to cover the 0x2000 register
  window and the 0x80000 bounce window (model on `a2065_config()` + `autoconfig_bytes`).
- An `addr_bank` whose `lput`/`lget` implement the mailbox: latch `DRVNUMX`/`*_ADDR1..3`; on a write to
  `READ`/`WRITE`/`READBYTES`/`WRITEBYTES` (offset 0x00/0x04/0x88/0x8C) do the backing-store I/O **inside the
  bus cycle** (so the write is synchronous, matching real HW) into the buffer addr or the 0x80000 bounce
  window; answer `DRVTYPE`/`BLOCKS`/`BLOCKSIZE`/`CYLS/HEADS/SECS`/`USED_DMA` from the hardfile geometry.
- Back it with a plain RDB/UFS hardfile. ~a few hundred lines, ≈ `a2065.cpp`'s device logic.
- **Blocker:** needs an **Amiberry source build** (only the 8.1.6 binary + a WinUAE source tree are on this
  machine). Cloning `BlitterStudio/amiberry` @ 8.1.6, adding the device, and building (SDL2 deps) is the
  next sizable chunk. Alternatively, validate straight on the real A4000+Z3660 when access is available.

## Open questions (for HW/emulation validation)
1. Unit-number mapping: Amix target (`cp->unit`, 0–7) → PISCSI drive index. Currently 1:1; confirm against
   how the Z3660 firmware enumerates the SD's RDB drives.
2. Cache coherency on real metal in EMU/030 mode (the AmigaOS driver uses `CachePreDMA`/`CachePostDMA`
   around the command write). Amix RAM is `< 0x08000000` so the firmware always bounces through MMIO
   (board+0x80000), which should sidestep it — verify on HW.
3. Always-bounce vs direct: confirmed Amix uses the bounce path (RAM < 0x08000000); fine.
4. INQUIRY removable bit: set to 0 (fixed disk) for Amix vs the AmigaOS driver's 0x80 (removable) — confirm
   Amix `sd` is happy treating it as a fixed disk.

## Sources
- `repo/z3660-drivers/scsi/z3660_scsi.c`, `z3660_scsi.h`, `z3660_scsi_enums.h`, `bootrom.asm`.
- `repo/KNOWN_ISSUES.md` (DMA-not-in-EMU, SCSI-SD-emulation bug history).
- The A4091-on-Amix predecessor project ([`amix-a4091`](https://github.com/jusii/amix-a4091)) — framework, build env, gotchas, clean-gate.
- `a2065.cpp` (WinUAE/Amiberry) — the AutoConfig-board + MMIO-bank emulation model.

## Real-hardware findings (2026-06-12 overnight session)

First contact with the physical A4000+Z3660. Everything below verified against the
**deployed firmware fork** (`Z3660-amix`, branch `amix-boot`) and live boots.

- **Board identity:** the piscsi window rides the combined RTG+SCSI window. Autoconfig
  products under manuf 0x144B: Z2 RTG+SCSI combo = **product 0x03** (advertises 64KB),
  Z3 fast RAM = 0x02, **Z3 RTG+piscsi = 0x01** (the ID our driver and the upstream AmigaOS
  driver probe). With `autoconfig_rtg NO` (normal config) the combo window never enters the
  autoconfig chain at all — it sits at a **fixed 0x10000000** (EMU decode in
  cpu_emulator.cpp; boot serial prints "[Core1] Autoconfig RTG to 0x1000").
- **Z2 variant is unusable for Amix piscsi:** base 0xE90000 + bounce offset 0x80000 =
  0xF10000, which the EMU decodes as extended-ROM space *before* the SCSI-window branch
  (ext kickstart loads at 0xF00000). All Amix RAM is motherboard fast at 0x07xxxxxx
  (< 0x08000000), so the firmware *always* bounces — the Z2 path can never move data.
- **Detection reality (the silent-hang root cause):** Amix 2.1's bootinfo autocon table
  missed the board both with `autoconfig_rtg NO` (expected — fixed base, not in chain) and
  with `YES` (KS configures it at 0x40000000 but the table still misses it — matches the
  grimoire hydra finding that the 2.1 table is unreliable on real metal). Since the
  generated sd.c only registered cards via autocon(), z3660queue never ran: kernel banner,
  then silence — no panic, no I/O. Fix: multi-method detect (autocon → AGA-gated probe of
  0x10000000, DRVTYPE must read 0/1) + sd.c `probe=` fallback hook (driver.conf field).
- **Free 68k→serial debug channel:** writes to read-only P_BLOCKS (0x10) make the ARM print
  "WARN: Write to read only register …(addr: value)" unconditionally → BREADCRUMB() macro.
  DRVTYPE reads return strictly 0/1 (safe presence probe); **never read P_BLOCKS0+unit×4 of
  an unmapped drive** — the ARM divides by block_size 0 (Zynq div-by-zero).
- **The old "Amix boots then hangs" on the real box was NOT this driver:** that kernel
  (banner "2.1", no AGA gate) booted via the firmware's WD33C93/A3000 emulation at
  0xDD0000 ([WCMD] = WD Select-and-Transfer traces) and wedged deterministically ~2.5 min
  after kernel entry in a completion re-entrancy race (ihandle→iodone→chunk-resubmit double
  startio → self-linked sdcom → buffer-pool starvation). Full analysis: recon workflow
  2026-06-12; instrumented firmware source preserved as commit 87db04b on `amix-boot`.
- **Boot timing (UAE_030_MMU, 667 MHz):** power-on → +1 s SD init → +8 s PISCSI maps →
  +30 s "JIT disabled" → **banner ≈ +120 s**. Old kernel reached root I/O ≈ banner+0–30 s.
  Give up: no banner by 3 min, or no progress 6 min after banner.
- **Deploy loop:** TFTP via ARM console ('C' spam on serial at power-on, then 'P'; path
  toggle SPACE is one-way 0:→1:): 900 MB ≈ 28 min at ~550 KB/s. HDF must be raw (the
  golden VHD is `conectix`/VHD format — convert with qemu-img, RDSK lands at block 2,
  firmware "No RDB found" for Amix images is normal/harmless). Always clean-shutdown the
  Amix guest before grabbing an HDF Amiberry has mounted.

## Overnight session 2026-06-12/13: the banner-hang root cause is the EMU core, not SCSI

Eight build-deploy-observe cycles on the real A4000+Z3660 (full log: tmp/serial-powercycle.log,
crumb decode in the iteration commits). Chronology of findings:

1. **The z3660 piscsi driver WORKS on real hardware.** Multi-method detect engages the fixed
   0x10000000 base, DRVTYPE answers, and the driver carried the whole boot I/O load: ~100+
   reads/writes per boot, every transfer byte-correct (page-in first-longs match /sbin/init and
   libc.so.1 file content exactly), every completion clean. It also faithfully wrote init's core
   dump — the mysterious "final write burst" of every hang.
2. **The hang is init dying.** Proc-table heartbeat dump: pid 1 goes SONPROC → SSLEEP on the
   pageio chunk buf (0x400B1400 — the old kernel's "STUCKBUF") → **SZOMB with p_wcode=CLD_DUMPED,
   p_wdata=SIGILL**. With init dead, boot silently stops after the banner; kernel daemons idle
   normally (heartbeat + scheduler alive). The old WD33C93-path kernel died the same way — its
   "SCSI wedge" was post-mortem noise.
3. **The SIGILL is an EMU-core demand-paging bug.** Core dump analysis (adb + capstone): fault
   PC = libc.so.1 `_rt_boot+0x0` (vaddr 0xC100F348, libc text mapped at 0xC1000000 per the core's
   segment table) — the dynamic-linker bootstrap entry, i.e. the FIRST instruction executed from
   a freshly demand-paged text page. File bytes there are a legal `movea.l a7,a0`. The EMU
   executed something else.
4. **Not stale-ATC-for-lack-of-flushing:** a PFLUSHA executed after every read completion
   (driver-side experiment) does not save init. Together with (3) this converges on
   `UAE_030_MMU_plan.md` Risk #3 / decision #3's predicted failure: **faulted-instruction
   restart** — the ifetch that page-faulted resumes without re-fetching the now-present page.
   The fork's own WIP re-fault detector (87db04b, cpummu030.cpp) was circling the same area.
5. **dd.c latent bug found & fixed on the way** (real, just not the root cause): ihandle ran
   iodone() before its trailing startio; iodone's b_iodone chunk-resubmit re-enters ddstrategy
   synchronously → double-issued &dp->com (async drivers) or unbounded recursion (synchronous
   drivers). Fixed (issue-next-then-iodone) + z3660 completions deferred via timeout() — both
   verified booting in Amiberry.

**Firmware fix domain:** the `Z3660-amix` firmware fork, branch amix-boot, cpummu030.cpp /
m68k_run_mmu030 ifetch-fault restart path. Build chain verified: `make z3660_emu` cross-compiles
clean (arm-none-eabi-gcc present); BOOT.BIN packaging needs zynq-mkbootimage
(`git clone https://github.com/antmicro/zynq-mkbootimage ~/git/zynq-mkbootimage && make` — one
command, was not auto-run overnight). A FAILSAFE.bin (copy of known-good BOOT.BIN) is now ON the
SD's FAT32 partition, so Z3660.bin experiments are recoverable.

**Deployed state (morning of 2026-06-13):** SD carries the full-trace kernel (sum 55077:
multi-method detect, BOUNCE_PAGES=32, nb==0 fail, deferred completions, dd.c reorder, crumb
trace + 2 s heartbeat + proc dump + pflusha experiment). The crumb trace costs ~50 ms/IO — fine
for diagnosis, strip the TRACE blocks for production. Old image is gone (overwritten per
decision); golden VHD + pre-real-HW snapshot intact in amix-kerntools/hdf/.

## 2026-06-13 morning: firmware fix DEPLOYED + on-hardware result (SIGILL fixed, SIGSEGV next)

Built the fixed firmware via the docker `full` image (`z3660-build:latest` — ships mkbootimage
+ Vitis 2023.2 toolchain; `./docker/run.sh make -C .../vitis_ide`; clean-rebuild Z3660_emu in the
container so the ELF is Vitis-built, NOT the host arm-gcc). Packaged BOOT.BIN (12330052 B),
TFTP-deployed as **Z3660.bin** (FAT32 path 0:), readback-verified byte-identical. On-SD BOOT.BIN
+ FAILSAFE.bin untouched.

**On-hardware result (commit c8b9398):** the SIGILL is GONE. The `[RTE-B-IF]` probe fires with
`ps=80003f00` (bit 31 set) and `opcode=ffffffff` on every ifetch fault, and init now executes
through **8 demand-paged text faults at 8 distinct PCs** (c100f348, c10127b4, c1011110, c1018e00,
c1020d20, 80002a4e, c10154a8, c101fee0) — vs dying at the first (c100f348) before. The
faulted-instruction REFETCH-on-resume is correct.

**New blocker:** init now dies **SIGSEGV (p_wcode=CLD_DUMPED, p_wdata=11)**, not SIGILL — a
distinct, later failure. Leading hypothesis = the OTHER half of the frame-$B simplification the
analysis flagged: newcpu_common.cpp case 0xB stacks ZERO for the mmu030_ad[] value longs, the
idx word (0x36), mmu030_state[0..2], and disp_store. A page fault PART-WAY through a non-idempotent
instruction (MOVEM list, (An)+/-(An), RMW, complex EA) therefore loses replay state and restarts
the whole instruction -> wrong effective address -> SIGSEGV. Fix = port the full WinUAE format-$B
frame storage (WinUAE newcpu_common.cpp:1501-1565: mmu030_ad[] with wb3_data pre-step, the idx
word, state words, disp_store, FMOVEM store, real stage-B/C pipe words). The fork's
m68k_do_rte_mmu030 reader already consumes all these fields. Next diagnostic: a DF-side probe
(data-fault resume) + capture the SIGSEGV fault address to confirm the wrong-EA mechanism before
the bigger port.

## 2026-06-13 ~08:00: RESOLVED — Amix boots multiuser on real A4000+Z3660

Two EMU-core MMU bugs (both in the `Z3660-amix` firmware fork, branch amix-boot),
not the driver. Found by kernel-side serial instrumentation + core-dump analysis, fixed against the
WinUAE 4.4.0 reference, each verified on real hardware:

1. **c8b9398 — ifetch-fault resume (was SIGILL).** The format-$B bus-fault frame builder never set
   ps bit 31 ("fault during opcode prefetch") and stacked stale regs.irc in the 0x14 opcode slot,
   so m68k_do_rte_mmu030 restored mmu030_opcode = the previous instruction (the kernel's
   return-to-user RTE 0x4E73) instead of -1; the run loop skipped its insretry refetch and
   re-dispatched the stale RTE in user mode → privilege violation → SVR4 SIGILL at the first
   instruction of every freshly demand-paged text page (init died at libc _rt_boot+0). Fix: set
   ps |= 1<<31 when mmu030_opcode==-1, and stack mmu030_opcode in the 0x14 slot for format 0xb.
   Result on HW: init ran through 8+ demand-paged text faults instead of dying at the first.

2. **e3f9440 — mid-instruction resume (was SIGSEGV).** The same frame builder stacked ZERO for the
   instruction-replay state the reader consumes (mmu030_ad[] value array @0x38-0x58, idx word @0x36,
   mmu030_state[0..2] @0x30-0x34, mmu030_disp_store[] @0x1c/0x20, FMOVEM store), so a fault PART-WAY
   through a non-idempotent instruction (MOVEM, (An)+/-(An), RMW, complex EA) restarted the whole
   instruction with wrong replay state → wrong effective address → SIGSEGV. Fix: port the full
   WinUAE format-$B frame storage for every consumed field at the offsets the reader reads, with the
   write-fault pre-step (mmu030_ad[idx_done]=regs.wb3_data when !RW), using fault-time saved copies
   (mmu030_page_fault:1869-1870). Result on HW: init SURVIVES — full rc tree, fsck runs on
   /dev/rdsk/c6d0s1 (the Z3660 piscsi root disk), stable multiuser process tree.

The amix-z3660scsi SCSI driver itself was correct from the first real-HW boot (carried 100% of boot I/O
byte-perfect); the "hang" was always the EMU demand-paging the driver's pages back in. Driver
production cleanup = a9ad84e (sum 43669), instrumentation stripped, verified multiuser in Amiberry.

**Firmware build/deploy:** docker `full` image (z3660-build:latest) ships mkbootimage + Vitis; the
host clone was never needed. `./docker/run.sh make -C z3660-firmware/Z-TURN/vitis_ide` (clean-rebuild
Z3660_emu IN the container). Deploy BOOT.BIN as **Z3660.bin** via the ARM-console TFTP (path 0:);
never overwrite on-SD BOOT.BIN/FAILSAFE.bin (FSBL fallback only catches load-failures). The HDF goes
to exFAT path 1:/hdf/Amix.hdf (SPACE to switch from 0:).

## 2026-06-16: KNOWN ISSUE — AMIX boot-time fsck is broken (FUTURE refactoring task)

Boot-time fsck does NOT auto-repair a dirty/corrupt root: the box boots all the way to `login:` but
the root UFS free-block bitmap stays broken, so the first **write** to a bad area panics
`PANIC: free: freeing free block, dev=0x480016, fs = /`. Today a power-cycle-heavy firmware test
session left root dirty and we hit this.

**Why it's disabled (the structural bug to fix):** a naive "fsck-on-boot then reboot" `bcheckrc`
LOOPS — fsck repairs → reboot → FS comes back dirty → fsck → reboot → … — because the reboot
re-dirties root (root mounted RW and stamped `FSACTIVE` before being marked clean, and/or the reboot
syncs stale in-core buffers back over the repair). So fsck-on-boot was effectively turned off,
leaving dirty roots un-repaired. The RDB-parse fix (firmware `72661d4`) fixed the *fsck-EVERY-boot*
half (find the RDB → mount root read-only first, not the no-RDB RW fallback); the other half — a
`bcheckrc` that auto-repairs cleanly **once** — lived in the now-deleted `Amix-bcheckrc-fix.hdf`.

**Manual recovery (until fixed):** single-user, **raw** device, re-run to clean, no-sync reboot:
`init 1` → `fsck -y /dev/rdsk/c6d0s1` (NOT `/dev/dsk/` — the block device SIGBUSes a mounted root) →
repeat until CLEAN → `reboot -n`. (SVR4.0 can't `mount -o remount,ro /` — "Invalid argument".)

**Proper fix (future, AMIX-HDF /etc concern — build via amix-kerntools, not the driver/EMU):**
`/etc/bcheckrc` on boot: mount root RO → `fsck -y` (auto-yes, no prompt) → if MODIFIED, `reboot -n`
→ only mount RW + clear FSACTIVE after a clean pass. Fix the FSACTIVE-stamp ordering + no-sync reboot
so it repairs once and proceeds — no loop, no manual fsck.

## 2026-07-11: two load-bearing contracts (T2.P3) — the spl6 bracket + the addr-is-physical rule

Two facts that must not be relitigated. Both came out of the T2.P3 CD-read-completion
corruption investigation (`docs/t2p3-artifacts/` in the Amix workspace: the WILD
exception-frame capture `p0.1-serial-log-findings.md`, the disassembly verdict
`p0.2-callout-ipl-verdict.md`).

### (a) `sdcom.addr` is a PHYSICAL address by contract — do NOT add vtop() in z3660.c

`cp->addr` reaching `z3660queue()` is **already a bus/physical address**: every caller
translates before filling it. The stock disk path does it at `amiga/alien/dd.c:240`
(`dp->com.addr = (caddr_t)vtop(bp->b_un.b_addr, bp->b_proc);`), and the cdfs kernel
transport does it at `amix-cdfs/platform/amix-kernel/amix_kern_media.c:151`
(`req.sc.addr = (caddr_t)vtop(data, (struct proc *)0);`). The driver therefore stores
`cp->addr` straight into `WRITE_ADDR3`/`READ_ADDR3` (and its `< 0x08000000` bounce gate
tests that physical value) — correct as written.

Adding a `vtop()` **inside** z3660.c would translate an already-translated address —
double translation — pointing the firmware DMA at garbage. This "fix" was proposed and
**evaluated on 2026-07-11 as a NON-FIX; do not resurrect it.** (The genuine T2.P3
corruption was the callout re-entrancy in (b), not addressing.)

Host-harness note: under `HOST_TEST` `vtop()` is effectively identity — the test stores
the raw host buffer pointer in `cp->addr` and the mock passes it through unmapped (register
cells are `unsigned long` so a 64-bit pointer survives the `*_ADDR3` slot).

### (b) callout-IPL: why z3660queue() now brackets the whole transaction with spl6/splx

Stock-kernel disassembly (verdict doc) established:

- `timeout()` **callouts are dispatched at SR 0x2400 = IPL 4**, from `timein()`.
- The **sole trigger** of that dispatcher is the **CIA-A clock at 68k level 2** (Paula
  AIEINT2 → p2int → aciaaintr → clock_int → clock() → timepoke() → timein()). So a
  section masked to **spl2 (SR 0x2200) already blocks the trigger** — which is why the
  cdfs transport's `sdspl`=spl2 sections (and `sd.h`'s `sdspl`) are sound.
- **But z3660.c itself previously had ZERO spl masking** (the only spl-ish tokens were the
  two `timeout()` calls). Its synchronous mailbox transaction — the shared `DRVNUMX` /
  `*_ADDR1..3` / bounce register sequence — ran at whatever IPL the caller held, **often
  spl0**. A level-2 clock tick mid-transaction dispatches `z3660done` at IPL 4 →
  `ihandle → startio` → a **full nested mailbox transaction inside the in-flight one** →
  scrambled registers / stale bounce → firmware DMA to the wrong address. That is exactly
  the corrupt-exception-frame state the 0.1 WILD capture recorded (user resumed at
  kernel-text PC 0x3FC5C after CD read #1 completion).

**Fix (shipped here):** bracket the ENTIRE `z3660queue()` transaction — register setup,
the synchronous command trigger, and every status readout — plus the `z3660present()`
probe, with `s = spl6(); … splx(s);` (via `z3660_enter()`/`z3660_leave()`). spl6 raises to
IPL 4, masking the level-2 clock with margin, so a clock-driven completion can never nest a
second transaction inside an in-flight one. The `timeout()` completion calls are unchanged
(they only schedule the callout; the callback runs later from clock context, now serialized
by the bracket). z3660.c now `#include "sys/inline.h"` for spl6/splx, exactly like
`amiga/floppy/flop.c` and `amiga/driver/hd.c`.

**Header quirk (do not be surprised):** in `sys/inline.h` on this platform `spl5`, `spl6`,
and `spl7` are **all** `#define`d to `_spl4`, which emits `move.w #0x2400,%sr` — i.e. "spl6"
in Amix kernel source is really IPL 4, not IPL 6. That is fine here: the clock is CIA-A at
level 2, well below 4. (CIA-B at level 6 would *not* be masked by these macros — irrelevant
while the clock trigger is CIA-A.)

**Permanent reentrancy detector (production, kmem-readable):** two non-static globals ride
the bracket — `z3660_nest_depth` (current transaction nesting depth) and `z3660_nest_hits`
(count of transactions entered while another was already in flight). `z3660_enter()` bumps
depth and, if depth was already nonzero, bumps hits; `z3660_leave()` drops depth. On a
correctly bracketed kernel **`z3660_nest_hits` stays 0 forever**; a nonzero value read via
`/dev/kmem` is direct proof the guard was breached. The accounting is a couple of cheap
instructions and stays in the shipping driver.

Regression coverage: `test/host/` gained mock `spl6()`/`splx()` (a mock IPL + counters, in
`kstubs.c`) and a mid-transaction re-entry test (`z3660_test.c` `test_reentry()`) that fires
a simulated clock callout from inside the in-flight mailbox command. With the bracket the
nested attempt is deferred (`z3660_nest_hits == 0`, outer read data intact); with the bracket
artificially removed (`mock_spl_disabled`) the nested transaction interleaves and
`z3660_nest_hits` counts it. All prior gating assertions plus the CD-oracle parity table
stay green (45/45 gating, 6/6 parity).

## 2026-07-12: T2.P3 mount panic — completion pointer-lifetime bug (the CD-read `cp`)

The spl6 bracket (908f40a) killed the register-scramble but the `mount -F cdfs` still
panicked, one CD read past where progress used to stop. On-box `cmn_err` capture (diag
kernel 0fe0e2e, `docs/t2p3-artifacts/p3-mount-panic-analysis.md` "SMOKING GUN"):

```
z3660done BAD intr: cp=0x40001C58 intr=0x40000730 unit=... addr=0x0 lastcmd=0x28 depth=0 hits=0
```

**Root cause — mechanism (a): the caller passes a transient, PER-PROCESS-STACK `sdcom`,
and the `timeout()`-deferred completion dereferences it out of context.** Not reentrancy
(`depth=0 hits=0`, spl6 sound), not a DMA scribble (the *pointer itself* is bad, not its
target). Evidence chain, all from source:

- The cdfs in-kernel media backend `amix-cdfs/platform/amix-kernel/amix_kern_media.c`
  `amix_kern_submit()` declares the request **on its stack** — `amix_kern_req_t req;`
  (`struct sdcom sc` first member) — fills `&req.sc`, calls `sdqueue(&req.sc)`, then blocks
  in `sleep()` until the driver's completion sets `req.done`. That is the *only* CD-read
  path (cdfs `vop_getpage` is `fs_nosys`; all CD I/O flows through this backend).
- `sd.c:sdqueue()` is a bare pass-through — `(*queue[cp->card].f)(c, cp)` — so `&req.sc`
  reaches `z3660queue()` unchanged, and is the `cp` handed to `timeout(z3660done, cp, 1)`.
- The disk path is immune because its `cp` is **persistent**: `dd.c` passes `&dp->com`
  (a global in `ddtab[][]`, mapped identically in every context) and `scsi.c:gsioctl` a
  `static struct sdcom`. Baseline capture confirms the disk completion `cp=0x80FDxxx`,
  `intr=0x800BF02` (kernel text) — always valid.
- `timeout(...,1)` fires `z3660done` ~1 tick later **from a clock (IPL4) callout, in the
  context of whatever process is current** (capture shows the pid varying: flopd / fsflush /
  pid 3 — "just whoever is current"). The cdfs caller is asleep and usually switched out, so
  its per-process kernel-stack VA (`~0x40001xxx`, the fixed u-area window) now resolves into
  a *different* process's stack. `z3660done` then reads a garbage `cp->intr` (hence the
  run-to-run "variable garbage": `0xFF000000`, `0x7E`, `0x40000730`) and jumps through it →
  KERNEL FAULT. A few CD sectors read cleanly first only because early completions happen to
  land while the caller is still current; once it context-switches out, the next callout
  dies.

**Fix (this driver, `src/z3660.c`): deliver the completion SYNCHRONOUSLY and ITERATIVELY —
no `timeout()`, no deferral window.** `z3660queue()` now calls a new `z3660_complete(cp)`
right after `z3660_leave()` (outside the mailbox bracket) instead of scheduling a callout.
Because the piscsi op is already finished, the completion runs *before z3660queue returns*,
in the caller's own context, so a per-process-stack `cp` is always valid. The *only* reason
`timeout()` existed — breaking `dd.c`'s completion→re-issue recursion (`ihandle → startio →
sdqueue → z3660queue`, one stack frame per chunk, stack-death on big bursts) — is preserved
by a driver-owned completion FIFO + a `z3660_completing` guard: the first completion runs a
drain loop; any completion re-issued from inside an `intr()` merely appends to the FIFO and
returns, and the loop picks it up. So the disk burst is delivered iteratively at a fixed,
small stack depth (host test measures depth 1 over a 51-deep re-issue chain), never one
frame per I/O. `spl6` brackets only the O(1) FIFO bookkeeping (against a clock-callout-driven
re-issue); the `intr()` call runs at the caller's IPL exactly as the callout used to.
`z3660_cq_overflow` is a kmem-readable guard counter (must stay 0). `z3660done` is now the FIFO
drain loop's worker — a bare `(*cp->intr)(cp)` (the temporary BAD-intr trap that caught this bug
on the bench was removed once the fix was proven on metal).

Why both paths stay correct: disk/gsioctl completions were already delivered correctly by a
persistent `cp`; they now run synchronously (a synchronous mailbox device has no async
benefit) and iteratively, so no recursion. The cdfs completion now runs in-context, so its
stack `cp` is live. The fix was proven on metal with the temporary BAD-intr trap still active
(diag kernel 5a46562): the "z3660done BAD intr" line never fired and the mount proceeded past
the CD reads. Those bench diagnostics have since been stripped; the permanent guards
`z3660_cq_overflow` and `z3660_nest_hits` must stay 0 on the shipping driver.

Regression coverage added: `test/host/z3660_test.c` `test_completion_trampoline()` drives a
50-deep re-issue chain through a persistent `sdcom` (mimicking `dd.c`'s `&dp->com` + `ihandle`)
and asserts all 51 completions are delivered, `max_depth <= 2` (iterative, not per-I/O
recursion), FIFO never overran, and no mailbox re-entry. Host harness now 49/49 gating, 6/6
parity. Kernel path cross-compiles clean (`m68k-cbm-sysv4-gcc -O -c`); the object's
undefined-symbol set drops `timeout` and gains nothing.

## 2026-08-14: the board is mapped DIRECTLY below VSECT1 — the sptalloc is gone

`z3660map()` no longer calls `sptalloc()` on the metal box. Both windows it maps —
the 0x2000 register window and the 0x80000 bounce window — are taken at their
physical addresses when the whole span sits below `VSECT1` (`0x40000000`). The
`sptalloc()` arm is kept, unchanged, for a board at or above `VSECT1`.

**Why this is correct.** AMIX's supervisor root table has four level-A entries, one
per 1 GB section (`tc_on` encodes TIA=2; built in `ml/exp` `pstart()`). Entry 0 —
VA `0x00000000`–`0x3FFFFFFF` — is a single **early-termination page descriptor**
with page address 0: a straight identity map of the whole low 1 GB, RAM or not.
That is why `immu.h:348` defines `phystokv(p)` as `(p)`, why stock Amiga drivers
just dereference their `autocon()` base, and why this driver's own AGA gate can
read VPOSR at `0xDFF004` before it has mapped anything. Only section 1 (`VSECT1`)
is page-table-backed, and section 1 is what `sptalloc()` hands out of.

🔴 **Mechanism correction.** The story this file and the driver header used to tell —
"same TT-gap-safe approach as the A4091" (see the 2026-06-07 *How it maps onto the
Amix SCSI framework* §3 and *Open questions* §5, left as written) — was **wrong in
its mechanism**. AMIX never enables transparent translation: `amiga/boot/copyit.s`
`pmove`s **zero** into `%tt0`/`%tt1`, and `ttrap.s`'s `tt0_on` word `0x003F0143`
has E=0 (its own comment reads "Disable"). Same `0x40000000` boundary, different
mechanism. Nothing functional depended on the wrong story, and the cacheability
conclusion is unchanged (section 0 carries CI clear, and the `sptalloc` path could
never have been cache-inhibited either — this kernel has **no `PG_CI` bit at all**;
`immu.h` defines only `PG_ADDR/PG_LOCK/PG_M/PG_REF/PG_W/PG_V`).

**Why it matters.** `sptalloc()` draws from `sptmap`, a hardcoded 2048-page (4 MB)
resource map that `page[]` — sized by presented RAM — is carved out of *first*, so
its free runs shrink as RAM grows. That is what made the sibling `z3660eth`
driver's 65-page mapping fail past ~83 MB of RAM, printing "no Z3660 ethernet
found" for a board that was fully visible and whose SCSI half was serving the root
filesystem at that moment (`amix-z3660net@b89c5d2`). This driver's 33 pages were
sitting in the same budget for no reason.

**Both windows go direct — including the bounce.** The bounce buffer is *not* memory
this driver allocates for DMA staging, despite the name. It is the **firmware's own**
staging area at a fixed board offset (`SCSI_NO_DMA_ADDRESS = RTG_BASE+0x80000`,
`Z3660 src/memorymap.h`), which both sides address by that offset and which the
driver only ever `bcopy()`s to and from. Its address is never `vtop()`'d and never
handed to the firmware — `*_ADDR3` always carries the *caller's* buffer — so moving
it from an `sptalloc`'d VA to its physical address is invisible to the datapath.
Hence 33 pages freed, not 1:

| mapping | pages | before | after (base `0x10000000`) |
|---|---|---|---|
| register window (`+0x2000`) | 1 | `sptalloc` | direct |
| bounce window (`+0x80000`) | `BOUNCE_PAGES` = 32 | `sptalloc` | direct |
| **total sptmap pages** | **33** | **33** | **0** |

(Amix `NBPP` is 2048, so 32 pages = the 64 KB `MAXXFER` bounce.)

The direct arm is gated on the **entire** span — `base + Z3660_WINDOW_TOP`, where
`Z3660_WINDOW_TOP = BOUNCE_OFFSET + BOUNCE_PAGES*NBPP = 0x00090000` — so a board
based just under the boundary takes the mapped path rather than running off the end
of section 0. Highest direct-mapped base is therefore `0x3FF70000` (host-harness
verified). New kmem-readable diagnostic `z3660_direct_map`: 1 = identity, 0 =
`sptalloc`'d.

This also makes explicit something the datapath already relied on: `cp->addr` is a
physical address by contract (2026-07-11 §(a) above), and `z3660_rw()` both
`bcopy()`s through it *and* hands it to the firmware unchanged — only the section-0
identity makes that legal.

**Host harness.** `stubs/sys/immu.h` now carries the real `VSECT1`/`NBPP` values and
routes `phystokv()` into the mock, so the harness exercises the **direct** arm — the
one metal runs — instead of a path the box never takes. New gate
`test_direct_mapping()` asserts `z3660_direct_map == 1`, because both arms reach the
same mock buffers and a silent fallback would otherwise leave every other test green.
52/52 gating, 6/6 parity. Cross-compile clean; the object's undefined-symbol set is
unchanged (`sptalloc` is still referenced by the retained high arm), so the `nm -u`
clean gate is unaffected.

## 2026-08-17: per-unit static-geometry cache — 10 → 6 round trips per 2 KB READ

The Z3660 firmware investigation (`../Z3660` branch `piscsi-crawl`,
`docs/piscsi-service-path.md` §1.1) established the piscsi cost model, and it is not
the one this driver was written against: **a register access is not a bus cycle, it is
a cross-core round trip.** Core1 — the guest's own CPU — publishes each access into the
`SHARED` struct and then *hard-spins, retiring no 68k instructions*, until core0 picks
it up from a cooperative protothread loop that services **at most one access per
iteration**, with the RTG paths, the ethernet thread and an unconditional
`pl_mpeg_arm_decode_progressive()` all on the critical path of every one of them.

So a command's cost is dominated by **how many registers it touches**, not by how many
bytes it moves. And the steady-state request granularity is 2 KB, not the protocol's
64 KB `MAXXFER`: raw/`physio` I/O stages through a 2 KB kernel buffer (`NBPP` = 2048,
and a `vtop()`'d buffer is valid for exactly one page), so the driver pays a full
transaction per 2 KB — about 1/32 of the protocol's capacity.

### What was being re-fetched, and why it never changes

Of the ten round trips a 2 KB READ used to cost, **nine carried no payload**, and four
were pure waste — per-unit facts re-read on every single CDB:

| # | access | why it was removable |
|---:|---|---|
| 2 | `RDLONG(P_BLOCKSIZE0 + unit*4)` | `devs[unit].block_size` — written only by `piscsi_map_drive()` |
| 3 | `WRLONG(P_DRVNUMX, unit)` | a **redundant repeat** of trip 1, inside `z3660_pdt()` |
| 4 | `RDLONG(P_PDT)` | `devs[unit].pdt` — set from `piscsi_map_drive()`'s `is_cd` argument |
| 5 | `RDLONG(P_BLOCKS0 + unit*4)` | derived from `d->fs`, the `f_size()` of the backing `.hdf` at open time |

All three cached facts are fields of the firmware's `devs[unit]` entry, and every one is
written **only** by `piscsi_map_drive()` (`Z3660 src/scsi/scsi.c` :924/:938/:953). The
`.hdf` files are fixed-size images opened once; there is no resize, no re-open, no
re-type and **no media-change or hot-attach path at all** — `config.cd_target[]` is read
only inside `piscsi_init()`.

**The invalidation argument is a reset argument.** `piscsi_map_drive()` has exactly two
entry points, `piscsi_init()` and the drive refresh, and *both run only with the 68k held
in reset*: `main.c reset_thread()` calls `piscsi_refresh_drives()` and then sets
`state68k = M68K_RESET`, and `cpu_emulator.c`'s reset arm calls `piscsi_init()` while the
68k is still held. Every remap is therefore bracketed by a guest reset, which reloads the
AMIX kernel and zeroes this driver's BSS along with it — **the cache cannot outlive the
facts it caches.** No runtime invalidation hook is needed, or even reachable: there is no
wire signal by which the guest could learn of a remap, so a driver-side invalidate could
never be triggered. Re-probe is covered too (`z3660map()` refills, and re-runs whenever
`regs` is 0).

The one fact a rescan *can* change — whether a unit has a drive mapped at all — is
deliberately left **uncached**: an entry is valid only when the unit reports a nonzero
block count, the driver's own long-standing "no drive mapped here" test. A unit empty at
attach is re-read on each command to it, exactly as before; that is off the hot path,
since such a command is refused without touching the medium.

### The one protocol subtlety: ordering

`P_PDT` is not addressed per unit — it returns `devs[piscsi_cur_drive].pdt`, so the unit
must be selected first. And a read of `P_BLOCKSIZE0+4n` / `P_BLOCKS0+4n` **reassigns
`piscsi_cur_drive` to `n` as a side effect** (`scsi.c` :1998, :1944). Hence
`z3660_geom_fill()` does both array reads *before* writing `P_DRVNUMX` and reading
`P_PDT`. Since all four accesses concern the same unit, the firmware is left with
`piscsi_cur_drive == unit` either way — identical to the pre-cache sequence. On the
command path the surviving `P_DRVNUMX` write still leaves `piscsi_cur_drive == unit`
before the doorbell, keeping the firmware's `val != piscsi_cur_drive` warning
(`scsi.c` :1388) silent; the data path indexes `devs[val]` from the doorbell's own value,
never the selected drive. **Wire protocol unchanged: no new register, no new command, no
firmware change.** An out-of-range unit is also byte-identical — the unconditional
`P_DRVNUMX` write still happens, and the geometry answer comes from a dedicated
"absent" entry (bs 512, nblocks 0, pdt 0) matching the old out-of-range returns exactly.

### Measured effect (host harness, not metal)

New `mock_trips*` counters in `test/host/mock_piscsi.c` count every `WRLONG`/`RDLONG`,
which *is* the metal cost unit. New gate `test_hotpath_trip_budget()`, plus a trip
breakdown gated inside `test_direct_mapping()` to pin the fill to attach:

| operation | before | after |
|---|---:|---:|
| READ(10), 2 KB (one chunk) | **10** | **6** |
| WRITE(10), 512 B | **9** | **5** |
| READ CAPACITY(10) | 5 | 1 |
| attach probe (one-time, all 8 units) | 2 | 34 |
| READ(10) to an **unmapped** unit (refused) | 4 | 5 |

The unmapped-unit row is the one regression, and it is deliberate: absence is never
cached, so such a unit is re-probed on every command and pays one extra `P_DRVNUMX`
write. That happens only during sd.c's boot-time bus scan, on a command that is
refused without touching the medium. `test_unmapped_unit_not_cached()` gates both
halves — still refused (the firmware would *silently no-op* the I/O, so reporting GOOD
would hand the caller stale buffer contents) and still re-probed with identical trip
cost on a second pass, proving nothing was latched.

That is the ~1.7× reduction in handshakes per 2 KB predicted by
`piscsi-service-path.md` §7.1. The 32 extra trips at attach are paid once, inside the
same `spl6` bracket the probe already used — far shorter than the bracket the driver
routinely holds across a 64 KB transfer plus its SD-card transaction.

**Throughput on metal is NOT proven by this.** The harness proves the trip count fell and
that the cached facts are still the right facts; it cannot prove core0 loop latency, which
is what those trips actually buy back. Bench/metal validation is separate.

77/77 gating (52 pre-existing + 25 new), 6/6 parity. Cross-compiled clean with the real
target compiler (`m68k-cbm-sysv4-gcc` 2.7.2.3, kerntools' `amiga/alien` recipe); the
`nm -u` clean gate is unaffected — undefined set still exactly `autocon`, `bcopy`,
`sptalloc`. New symbols are `z3660_geom` (COMMON, 128 B = 8 × 16, kmem-readable like the
other diagnostics), local `z3660_geom_absent`, and the two local helpers replacing
`z3660_blocksize`/`z3660_nblocks`/`z3660_pdt`.

## 2026-08-18: window geometry is byte-primary — the page count is now derived

`src/z3660.c` used to state its board-window geometry as a **page count**:

```c
#define BOUNCE_PAGES 32                     /* 64KB bounce; Amix NBPP is 2KB, not 4KB! */
#define BOUNCE_SPAN  (BOUNCE_PAGES * NBPP)  /* 0x10000 == MAXXFER */
#define Z3660_WINDOW_TOP (BOUNCE_OFFSET + BOUNCE_SPAN)  /* 0x00090000 */
```

The span derivation used `NBPP`, which looks page-size-aware but is the wrong way round:
the constant that is *fixed* is the 64 KB firmware aperture at board+0x80000 (`Z3660
src/memorymap.h`, `SCSI_NO_DMA_ADDRESS`), and the page count is what has to move when the
page size does. As written, a 4 KB-page kernel would silently keep 32 pages and **double**
everything derived from them. Measured with the target compiler (see the probe below):

| constant | NBPP 2048, before | NBPP 4096, before | NBPP 2048, after | NBPP 4096, after |
|---|---:|---:|---:|---:|
| `BOUNCE_PAGES` | 32 | **32** | 32 | **16** |
| `BOUNCE_SPAN` | 0x10000 | **0x20000** | 0x10000 | **0x10000** |
| `Z3660_WINDOW_TOP` | 0x00090000 | **0x000A0000** | 0x00090000 | **0x00090000** |

Both "before" regressions are real, not cosmetic. The doubled span claims 32 pages of
`sptmap` on the `>= VSECT1` arm instead of 16 — the same scarce 2048-page resource whose
exhaustion made the sibling `z3660eth` driver fail past ~83 MB (2026-08-14 entry). And the
moved `Z3660_WINDOW_TOP` widens the direct-map admission test by 64 KB, so a board based
just under `VSECT1` could be admitted to the identity-map path on a window bound the
firmware never had.

So the shape is inverted: **bytes are primary, pages are derived, rounded up.**

```c
#define BOUNCE_BYTES  0x00010000            /* the firmware aperture: 64KB, any page size */
#define BOUNCE_SPAN   BOUNCE_BYTES
#define Z3660_PAGES(b) (((b) + NBPP - 1) / NBPP)
#define BOUNCE_PAGES  Z3660_PAGES( BOUNCE_BYTES)
```

### The audit: which constants are byte quantities, and which are genuinely per-page

Every remaining constant in the driver was classified, not just the bounce span:

| constant | kind | treatment |
|---|---|---|
| `BOUNCE_PAGES` | was a page count | **inverted** — derived from `BOUNCE_BYTES` |
| the literal `1` in `sptalloc( 1, ...)` for the register window | was an unnamed page count | **inverted** — `REGS_PAGES` from `REGS_BYTES` |
| `BOUNCE_BYTES`, `BOUNCE_OFFSET`, `PISCSI_OFFSET`, `BOUNCE_THRESH`, `MAXXFER`, `Z3660_FIXED` | firmware/protocol byte addresses and sizes | already byte-primary, no `NBPP` term — unchanged |
| `Z3660_WINDOW_TOP` | byte bound, but was reached *through* a page count | now purely byte-derived |
| `Z3660_NUNITS` (8), `Z3660_CQ` (32) | per-unit register-array depth; completion-FIFO depth | **not** page quantities — left alone (the `32` is a coincidence of value, not of meaning) |
| the arguments to `sptalloc()` and `phystopfn()` | genuinely per-page | stay page-valued *by API* — `sptalloc()` takes a page count and `phystopfn()` a frame number; they are now fed derived values instead of literals |

`REGS_BYTES` is itself derived — `(P_WRITE_ADDR3 + 4)`, the last register this driver
touches plus its long — so adding a higher register cannot leave the mapping short. It is
0x24C, one page at both 2048 and 4096, which is why the mapped total stays 33 pages at
NBPP 2048 and becomes 17 at 4096 rather than 33.

### Compile-time guards (what the 1992-vintage compiler will take)

All the quantities are integer macros, so the guards are plain `#if` / `#error`
conditionals — no generated code, and they work identically in the kernel build and the
host harness. `m68k-cbm-sysv4-gcc` 2.7.2.3 accepts `#error`, `%`, and macro-expanded
arithmetic in `#if` (verified). The negative-array-size idiom
(`typedef char a[cond ? 1 : -1];`) was also tried and *does* work on this compiler —
recorded here in case a future assertion needs `sizeof` — but the preprocessor form is
preferred: it emits nothing at all.

Each page count is pinned from **both** sides, because a hardcoded count fails in both
directions: too few pages leaves the window tail unmapped, too many burns `sptmap`. The
pair asserts `count == ceil(bytes / NBPP)` exactly. Every guard was verified by breaking
one value in a scratch copy and watching it fire:

| deliberate break | guard that fired |
|---|---|
| `BOUNCE_PAGES` re-hardcoded to 32, NBPP 4096 | over-covers `BOUNCE_SPAN` — wasted sptmap pages |
| `BOUNCE_PAGES` re-hardcoded to 16, NBPP 2048 | does not cover `BOUNCE_SPAN` |
| the **whole pre-change spelling** restored, NBPP 4096 | board window top must not vary with `NBPP` |
| `REGS_PAGES` hardcoded 1 with a 256 B page / hardcoded 2 at 2048 | under- / over-covers the register file |
| `MAXXFER` raised to 128 KB | one `MAXXFER` chunk must fit the aperture |
| `BOUNCE_OFFSET` nudged to 0x80004 | offsets must be page-aligned (and the window-top guard) |

The page-alignment guard is not decoration: `sptalloc()` maps by frame and
`phystopfn()` truncates, so a misaligned board offset would silently map a window shifted
down to the frame boundary.

### Evidence

* **No behavioural change on the shipping kernel.** The cross-compiled object is
  **byte-identical** to the pre-change one (`md5 287395f8eabb5d4fbe9d58dd65143ee9`, 5136 B)
  under kerntools' real `amiga/alien` recipe:
  `m68k-cbm-sysv4-gcc -c -O -DSYSV -D_KERNEL -I.. -I../.. -I../inc z3660.c`.
  `nm -u` clean gate unaffected — still exactly `autocon`, `bcopy`, `sptalloc`.
* **77/77 gating, 6/6 parity** on the host harness, and 77/77 again with the harness stub's
  `NBPP` set to 4096, so the datapath is page-size-neutral as well as the geometry.
* Reproducing the constant table: compile the driver inside a probe TU that overrides the
  page size before including it —
  `#include "sys/types.h"` / `#include "sys/immu.h"` (guarded, so it keeps its own `NBPP`
  out of the way) / `#undef NBPP` / `#define NBPP 4096` / `#include "z3660.c"`, then emit
  `long v[] = { NBPP, BOUNCE_PAGES, BOUNCE_SPAN, REGS_BYTES, REGS_PAGES, Z3660_WINDOW_TOP };`
  and read `.data` with `m68k-cbm-sysv4-objdump -s -j .data`. That is the target compiler's
  own arithmetic, not a transcription.

**No 4 KB-page Amix kernel exists.** This is hardening, not enablement: the kernel's
`sys/immu.h` hardcodes `NBPP` 2048 alongside `PNUMSHFT` 11 and `POFFMASK` 0x7FF, and a real
page-size change would have to move all three together. What changes here is that this
driver would then follow correctly instead of silently over-claiming.

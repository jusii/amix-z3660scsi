# Z3660 SCSI → Amix driver — notes

(Repo `amix-z3660`; builds go through the sibling `amix-kerntools` harness +
golden image — `../amix-kerntools/build-kernel.sh ../amix-z3660`. Started as
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
- **A READ/WRITE is:** write `DRVNUMX`(unit) → write `*_ADDR1`(block offset), `*_ADDR2`(len),
  `*_ADDR3`(buffer addr) → **write the command register** (`READBYTES`/`WRITEBYTES` with the unit as the
  value). That single command-register write is the **trigger AND the completion**: the ARM intercepts the
  Zorro III bus cycle and finishes the whole transfer before the write returns. **No poll, no IRQ.** ✅
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
- **`src/kernel-patches/sd.c`** — adds `0x144B0001, &z3660queue, "Z3660 SCSI"` to `scsicard[]` (+ the
  `z3660queue` extern). No `support.c`/`autocon()` change needed — the Z3660 SCSI is a normal AutoConfig
  board, so the generic table search finds it. (`SDCARDS=2`, so ≤2 controllers register at once — fine for
  an A4000+Z3660.)
- **`src/kernel-patches/alien-Makefile`** — adds `z3660.o` to `OBJ`.

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
- The A4091-on-Amix predecessor project (`../amix-a4091/`) — framework, build env, gotchas, clean-gate.
- `a2065.cpp` (WinUAE/Amiberry) — the AutoConfig-board + MMIO-bank emulation model.

## Real-hardware findings (2026-06-12 overnight session)

First contact with the physical A4000+Z3660. Everything below verified against the
**deployed firmware fork** (`~/Devel/Omat/Amiga/Z3660`, branch `amix-boot`) and live boots.

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

# amix-z3660scsi — native Amix SCSI driver for the Z3660 accelerator

A native Amix (Commodore SVR4, 68030/EMU) driver for the **Z3660** accelerator's
onboard SCSI, so Amix on a real A4000+Z3660 stops relying on the buggy
A3000-WD33C93 emulation. Successor to the [amix-a4091](https://github.com/jusii/amix-a4091) project,
which built the framework, build environment, and gotcha catalog this reuses.

The Z3660's "SCSI" is not a SCSI chip at all — it is the PiStorm **piscsi
mailbox protocol** (AutoConfig `0x144B:0x01`, synchronous MMIO register
mailbox, no IRQ/poll). See [NOTES.md](NOTES.md) for the full protocol and
design.

## Scope / responsibility

This repo is **the AMIX SCSI driver for the Z3660, and nothing else.** It is one of
several repos in the AMIX-on-Z3660 effort, each with a single job:

- **Ethernet** driver → [`amix-z3660net`](https://github.com/jusii/amix-z3660net)
- **Firmware / 68k-emulator** (and its bring-up investigations: MMU, fsck, lpsched
  coherency) → [`Z3660-amix`](https://github.com/jusii/Z3660-amix) (`docs/investigations/`)
- **Build harness, golden image, host-ops tooling, build configs** → [`amix-kerntools`](https://github.com/jusii/amix-kerntools)

The full map is in [`amix-kerntools/REPOS.md`](https://github.com/jusii/amix-kerntools/blob/master/REPOS.md).

## Status (2026-08)

**Driver written, integrated, clean-gated, and proven on real hardware** ✅ —
compiled with the native K&R `cc`, linked into the kernel (`checkunix`-clean),
and **boots Amix to multiuser on a real A4000+Z3660**. On the first real-hardware
boot the driver carried 100% of the boot I/O **byte-perfect** — every demand-paged
text page-in and `init`'s core-dump write verified against file content. The two
blockers hit during bring-up turned out to be EMU-core MMU bugs in the **firmware**
(demand-paging instruction restart), not in this driver — see
[NOTES.md](NOTES.md) (2026-06-13 RESOLVED).

Since that milestone — every item below has a dated entry in
[CHANGELOG.md](CHANGELOG.md):

- **CD-ROM units are answered.** A unit the firmware flags `pdt 0x05` gets a CD-ROM
  INQUIRY, READ CAPACITY, MODE SENSE(6)/(10) and a DATA PROTECT rejection for writes
  — which is what the Amix CD filesystem port (`amix-cdfs`) mounts.
- **Completion is delivered synchronously, in-context.** No `timeout()` deferral: a
  driver-owned FIFO flattens the completion→re-issue chain instead of recursing one
  kernel-stack frame per I/O. This fixed a mount panic caused by touching a caller's
  stack `sdcom` after its frame was gone.
- **The board is mapped directly, and the `sptalloc()` is gone.** AMIX identity-maps
  the low 1 GB, so a board below `VSECT1` — the shipped fixed base `0x10000000` — has
  both its windows taken at their physical addresses: 33 pages given back to the
  scarce 2048-page `sptmap`. The `sptalloc()` arm is retained, unchanged, for a board
  at or above `VSECT1`.
- **Per-unit static geometry is cached at attach.** A 2 KB READ costs **6** mailbox
  round trips instead of 10, a WRITE 5 instead of 9. On this hardware a register
  access is a **cross-core round trip**, not a bus cycle, so trips are the unit of cost.
- **Window geometry is byte-primary and page-size-agnostic.** The fixed quantity is
  the firmware's 64 KB aperture; page counts are derived, rounded up, and pinned from
  both sides by compile-time guards.
- **68040/68060 data-cache maintenance is implemented but gated OFF by default**
  (`z3660_cache`, poked through `/dev/kmem`). Every deployment so far has run on an
  emulated CPU, which has no data cache. The round is pre-registered in
  [docs/BLIZZARD-F3.md](docs/BLIZZARD-F3.md) — and **nothing in it is yet evidence
  about a real cache**: the metal A/B is still owed, and the bench emulator cannot run
  it (it models no 040/060 copyback data cache).

On **2026-08-26** this driver served the AMIX root disk on a real **68LC060** for the
first time — three boots for three to multiuser with live piscsi I/O — but on the
`DTT0` cache-inhibit free ride alone: `z3660_cache` was never poked and no F3 counter
was read, so that is a first, not a result.

Earlier milestone: clean cold-boot to multiuser on the Amix build box under Amiberry
with **no Z3660 board present** — `autocon(0x144B0001)` returns 0 and `z3660queue` is
simply never called (harmless, exactly like the A4091 driver with no A4091 present).

## Layout

```
src/z3660.c              the driver (map, geometry, chunked R/W, queue entry)
src/kernel-patches/      dd.c.patch -- unified diff vs stock amiga/alien/dd.c
                         (see src/kernel-patches/NOTICE). The sd.c scsicard[]
                         rows + Makefile OBJ are NOT here -- kerntools generates
                         them from driver.conf.
driver.conf              0x144B0001 z3660queue "Z3660 SCSI" z3660.c
test/host/               host CDB harness -- compiles the REAL driver on host gcc
                         against a mock piscsi mailbox: `make -C test/host check`
docs/                    design documents, written before the code they cover
                         (BLIZZARD-F3.md -- the 040/060 data-cache round)
assets/                  local reference material (gitignored): WinUAE 4.4.0 sources,
                         rollback firmware baselines, deploy scripts -- see assets/README.md
NOTES.md                 the dated engineering journal: protocol scouting, real-HW
                         findings, and the corrections to both
CHANGELOG.md             dated record of everything that landed -- the living state
```

The upstream firmware source (formerly cloned into a gitignored `repo/`) is not kept in
this repo. Re-fetch it when needed (it is read-only reference for the piscsi protocol):

```sh
git clone --filter=blob:none https://github.com/shanshe/Z3660 repo
```

The full firmware fork we actually build/deploy lives in its own repo,
[`jusii/Z3660-amix`](https://github.com/jusii/Z3660-amix).

## Building

This repo is **source only** — it does not build a kernel on its own. The build
goes through the **kerntools harness**, a separate prerequisite repo that holds the
golden Amix build image, the boot-breaker clean-gate, and the FTP/telnet bridge.
The harness takes this repo's two deliverable inputs — [`driver.conf`](driver.conf)
and [`src/z3660.c`](src/z3660.c) — splices them into its golden Amix kernel tree
and relinks. It generates the `sd.c` controller rows and the Makefile `OBJ`
entry from `driver.conf`, and applies our
[`src/kernel-patches/dd.c.patch`](src/kernel-patches/dd.c.patch) to the stock
`amiga/alien/dd.c` (idempotently — re-applying is skipped).

With the kerntools repo checked out alongside this one, bring up your Amix box
(real hardware, or your own WinUAE/Amiberry config with an a2065 NIC on tap0),
then run the single build entry point:

```sh
(cd ../amix-kerntools && ./amix-build z3660scsi)          # Z3660-only kernel
(cd ../amix-kerntools && ./amix-build a4091 z3660scsi)    # universal kernel
```

Never install an ungated kernel — Amix's `ld` intermittently corrupts the
image (the "boot-breaker"; the harness clean-gates against this).

## License

The original work in this repo is released under the **MIT license** (see
[LICENSE](LICENSE)): [`src/z3660.c`](src/z3660.c), [`driver.conf`](driver.conf),
and this repository's documentation.

[`src/kernel-patches/dd.c.patch`](src/kernel-patches/dd.c.patch) is a unified
diff against the stock Amix `amiga/alien/dd.c`: the added lines are MIT, the few
quoted stock context lines remain under the original Commodore SVR4 copyright.
The `sd.c` controller rows and the kernel Makefile `OBJ` entry are generated by
the kerntools harness from `driver.conf`, so they are **not** shipped here. See
[`src/kernel-patches/NOTICE`](src/kernel-patches/NOTICE).

The Z3660 piscsi mailbox protocol that `z3660.c` implements is defined by the
open-source [`shanshe/Z3660`](https://github.com/shanshe/Z3660) firmware
(`z3660-drivers/scsi/`); credit for the protocol is theirs.

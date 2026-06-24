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

## Status (2026-06)

**Driver written, integrated, clean-gated, and proven on real hardware** ✅ —
compiled with the native K&R `cc`, linked into the kernel (`checkunix`-clean),
and **boots Amix to multiuser on a real A4000+Z3660**. On the first real-hardware
boot the driver carried 100% of the boot I/O **byte-perfect** — every demand-paged
text page-in and `init`'s core-dump write verified against file content. The two
blockers hit during bring-up turned out to be EMU-core MMU bugs in the **firmware**
(demand-paging instruction restart), not in this driver — see
[NOTES.md](NOTES.md) (2026-06-13 RESOLVED).

Earlier milestone: clean cold-boot to multiuser on the Amix build box under
Amiberry, which cannot emulate the Z3660 mailbox — so when the board is absent
`autocon(0x144B0001)` returns 0 and `z3660queue` is simply never called (harmless,
exactly like the A4091 driver with no A4091 present).

## Layout

```
src/z3660.c              the driver (map, geometry, chunked R/W, queue entry)
src/kernel-patches/      dd.c.patch -- unified diff vs stock amiga/alien/dd.c
                         (see src/kernel-patches/NOTICE). The sd.c scsicard[]
                         rows + Makefile OBJ are NOT here -- kerntools generates
                         them from driver.conf.
driver.conf              0x144B0001 z3660queue "Z3660 SCSI" z3660.c
assets/                  local reference material (gitignored): WinUAE 4.4.0 sources,
                         rollback firmware baselines, deploy scripts -- see assets/README.md
NOTES.md                 protocol scouting + implementation status + test plan
```

The build-box `.uae` config now lives with the build harness at
`../amix-kerntools/configs/amix-z3660scsi-build.uae`.

The upstream firmware source (formerly cloned into a gitignored `repo/`) is not kept in
this repo. Re-fetch it when needed (it is read-only reference for the piscsi protocol):

```sh
git clone --filter=blob:none https://github.com/shanshe/Z3660 repo
```

The full firmware fork we actually build/deploy lives separately at `~/Devel/Omat/Amiga/Z3660`.

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

With the kerntools repo checked out alongside this one:

```sh
amiberry --config ../amix-kerntools/configs/amix-z3660scsi-build.uae &
../amix-kerntools/build-kernel.sh ../amix-z3660scsi                  # Z3660-only kernel
../amix-kerntools/build-kernel.sh ../amix-a4091 ../amix-z3660scsi    # universal kernel
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

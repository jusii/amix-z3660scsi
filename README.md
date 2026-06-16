# amix-z3660 — native Amix SCSI driver for the Z3660 accelerator

A native Amix (Commodore SVR4, 68030/EMU) driver for the **Z3660** accelerator's
onboard SCSI, so Amix on a real A4000+Z3660 stops relying on the buggy
A3000-WD33C93 emulation. Successor to the [amix-a4091](../amix-a4091/) project,
which built the framework, build environment, and gotcha catalog this reuses.

The Z3660's "SCSI" is not a SCSI chip at all — it is the PiStorm **piscsi
mailbox protocol** (AutoConfig `0x144B:0x01`, synchronous MMIO register
mailbox, no IRQ/poll). See [NOTES.md](NOTES.md) for the full protocol and
design.

## Status (2026-06)

**Driver written, integrated, clean-gated, and boots** ✅ — compiled with the
native K&R `cc`, linked into the kernel (sum 44396, checkunix-clean), and
cold-booted to multiuser on the build box. **Not yet exercised against real
mailbox hardware** — that waits on either an Amiberry piscsi emulation
(scoped in NOTES) or the real A4000+Z3660.

## Layout

```
src/z3660.c              the driver (map, geometry, chunked R/W, queue entry)
src/kernel-patches/      reference combined sd.c + alien Makefile (as proven);
                         the kerntools harness now GENERATES these
driver.conf              0x144B0001 z3660queue "Z3660 SCSI" z3660.c
configs/                 build-box .uae (shared golden image)
assets/                  local reference material (gitignored): WinUAE 4.4.0 sources,
                         known-good firmware, deploy scripts -- see assets/README.md
NOTES.md                 protocol scouting + implementation status + test plan
```

The upstream firmware source (formerly cloned into a gitignored `repo/`) is not kept in
this repo. Re-fetch it when needed (it is read-only reference for the piscsi protocol):

```sh
git clone --filter=blob:none https://github.com/shanshe/Z3660 repo
```

The full firmware fork we actually build/deploy lives separately at `~/Devel/Omat/Amiga/Z3660`.

## Building

Shared tooling lives in the sibling [amix-kerntools](../amix-kerntools/) repo
(golden build image, boot-breaker clean-gate, FTP/telnet bridge):

```sh
sh ../grimoire-amix/tools/host-net/amix-lan-up.sh
amiberry --config configs/amix-z3660-build.uae &
../amix-kerntools/build-kernel.sh ../amix-z3660                  # Z3660 kernel
../amix-kerntools/build-kernel.sh ../amix-a4091 ../amix-z3660    # universal
```

Never install an ungated kernel — Amix's `ld` intermittently corrupts the
image (the "boot-breaker"; see kerntools README).

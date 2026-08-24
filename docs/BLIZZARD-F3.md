# BLIZZARD F3 — z3660scsi against a real 68040/68060 data cache

**Pre-registration, written 2026-08-24, BEFORE any code in this round. Nothing below is to be
rewritten to match a later measurement.** Round 1 covers **F3-M0** (assert the free ride) and
**F3-M1** (real maintenance where the free ride cannot reach) for this driver only; the
ethernet driver is a separate round in `../amix-z3660net`.

Confidence tags follow the campaign convention: **measured** (a line of source read, or a
number off a run, with its file), **derived** (arithmetic over measured facts, shown), and
**assumed** (neither — each one below names the gate that settles it).

---

## 0. Why this driver has never needed a cache instruction

Every deployment of `z3660scsi` that has ever existed has run on an **emulated** CPU. The
Z3660 carries the guest 68030/68040 on core1's UAE interpreter; the socketed 68LC060 is a
bus-parked passenger in every mode shipped so far (`Z3660 docs/carrier-bringup-checklist.md`:
the chip must be fitted, but "never execute a single instruction on the 060"). An interpreter
has no data cache, so a `CPUSHL` the driver never issues costs nothing and a stale line that
never exists corrupts nothing.

BLIZZARD F4 is the first time this driver's instructions reach silicon that has caches. That
is the whole of F3: **the absence of maintenance is not a latent bug that has been getting
away with it — it is a correct program for the machine it has run on, and an incorrect one for
the machine it is about to run on.**

---

## 1. THE DTT0 COVERAGE TRUTH

This is the round's central finding, and it corrects a reading that both the recon and the
plan's own framing invite.

### 1.1 What the register actually says

`pstart040.s:326-327` (**measured**) loads `DTT0 = 0x003fc060`. Decoded against the 040/060 TTR
format — and the decode is confirmed by the file's own A/B comments, which call `ITT0`'s
`0x003fc000` "WT-cacheable", `DTT1`'s `0x807fa060` "cache-inhib", and record that serializing
was tried as `CM 0x60 -> 0x40`:

| field | bits | value | meaning |
|---|---|---|---|
| base | 31-24 | `0x00` | logical address base |
| mask | 23-16 | `0x3f` | address bits 29-24 don't-care ⇒ **`0x00000000`–`0x3FFFFFFF`** |
| E | 15 | 1 | enabled |
| S | 14-13 | `10` | match all function codes (user **and** supervisor) |
| CM | 6-5 | `11` | **cache-inhibited, NONSERIALIZED** |

### 1.2 The correction that matters

> **A TTR matches LOGICAL addresses, not physical ones.**

That sentence is not ours — it is the port lane's own census verdict, reached when the same
mistake was made there and caught: *"the original 'DTT0 masks per-page CM' assumption was
WRONG: a TTR matches LOGICAL addresses only, so high PTE-backed kvseg/segmap/user mappings
already take CM from their leaf PTEs"* (`amix-040-060-port/docs/CACHES-ON-PLAYBOOK.md:173-179`,
**measured**).

So `DTT0` does **not** make AMIX RAM cache-inhibited. It makes *accesses through the low
identity alias* cache-inhibited. The physical page reached at VA `0x08001000` with the data
cache bypassed is the same physical page reached at some user VA `0x808xxxxx` **with the data
cache fully engaged**.

| access path | VA range | cache mode today | evidence |
|---|---|---|---|
| the driver's own `cp->addr` dereferences and `bcopy`s | `< 0x40000000` | **CI**, via DTT0 | `pstart040.s:326`; `cp->addr` is physical by contract (`NOTES.md` 2026-07-11 §a) |
| the board window (registers + bounce aperture) at `0x10000000` | `< 0x40000000` | **CI**, via DTT0 | same; `z3660map()` direct-map arm |
| kernel arena / `sptalloc` / `segkmem` | `0x40000000`+ | leaf PTE `CM` | `CACHES-ON-PLAYBOOK.md:173-179` |
| **user pages** | `0x80000000`+ (user FC) | **copyback** | `hat040.s:1869` `hat_cm_ram = 0x20`, *"default since 2026-07-30"* |

The last row is the one that ends the argument. `hat_cm_ram` has been **copyback since
2026-07-30** (**measured**, `hat040.s:1851-1869`), and DTT1's `S=01` deliberately leaves user
accesses above `0x80000000` to the page tables so they *do* translate and *do* cache.

### 1.3 Consequences, stated plainly

1. **The free ride is real, and it covers exactly one thing: the driver's own accesses.** The
   register window, the bounce aperture, and the driver's own reads/writes of `cp->addr` are
   cache-inhibited for free — because all three are dereferenced through the low identity
   alias, and `cp->addr` is a *physical* address by this driver's contract. That is worth
   asserting (F3-M0) and worth never losing.
2. **The free ride does not cover the buffer's other aliases.** A raw-I/O transfer's `cp->addr`
   is `vtop(bp->b_un.b_addr, bp->b_proc)` (**measured**, `dd.c:240`) — the physical address of
   a *user* page that userland has been reading and writing through a copyback mapping. Dirty
   lines for that page exist in the 040/060 data cache, and neither DTT0 nor `/SNOOP` nor the
   ARM's `Xil_DCacheInvalidateRange` touches them.
3. **Therefore S2 and S3 are real and are not knob-gated.** A WRITE hands the firmware a
   physical page whose newest bytes are still in the CPU's cache (stale data to disk); a READ
   lets the ARM overwrite a physical page whose stale dirty lines can later be evicted over the
   fresh bytes (silent corruption after the fact).
4. **The cure is cheap and exact, and this driver is unusually well placed to apply it.**
   `CPUSHL`/`CINVL` select lines **by physical address** on a physically-tagged cache, so a
   single op covers *every* virtual alias of that line: *"Cache line operations are selected by
   physical line, so they cover high KVA/user CB aliases of that RAM. Retained DTT0 also makes
   the low identity address usable for the helper"*
   (`amix-040-060-port/docs/contracts/A3091-B2-PREPARE-PATCH-SPEC.md:337-343`, **measured**).
   This driver already holds the physical address and is forbidden to `vtop` it — so it needs
   no address conversion at all, unlike `z3660eth`, whose N8 defect is exactly that it passes a
   kernel VA.

### 1.4 What the 060-D cache knobs do and do not change

The dispatch framed this round as designing "for the world AFTER the knobs land (copyback low
memory)". **That world does not exist and is not what the knobs do.** Read
`amix-040-060-port/docs/060-D-CACHE-KNOBS-PLAN.md:26-33` (**measured**):

| knob | where | state | effect on this driver |
|---|---|---|---|
| `DC60_EDC` data cache | CACR bit 31 | **already on, copyback since 2026-07-30** | this is the exposure; it is present today |
| `IC60_EIC` instruction cache | CACR bit 15 | already on | none |
| `ESS` superscalar | PCR bit 0 | already on | none |
| **`DC60_ESB` store buffer** | CACR bit 29 | **off — candidate 1** | **arms S4** (see §3) |
| **`IC60_EBC` branch cache** | CACR bit 23 | off — candidate 2 | none |

Neither knob touches `DTT0`, and neither makes low memory cacheable. `DTT0` narrowing is
booked as *"a separate later low-identity-map/physmap milestone"*
(`CACHES-ON-PLAYBOOK.md:179`, **measured**) and is not part of 060-D.

---

## 2. THE ORDERING DECISION

**Decided: the maintenance lands NOW, default-off, poke-armed — not "when the knobs land".**

The premise that would have justified deferring is false in both directions: copyback is not
coming, it is here (§1.4); and DTT0 does not make the data buffers CI, it makes the driver's
*view* of them CI (§1.2). The maintenance is therefore a **precondition of F4's first boot**,
which is exactly why BLIZZARD §9 gates F4 on F3 — a first real-silicon boot with unfixed
drivers would produce corruption indistinguishable from a kernel bug and burn the one clean
first boot on an ambiguity.

One item genuinely *is* knob-ordered, and only one: **S4**. Its hazard is armed by the store
buffer, so its barrier is written now and its necessity is sequenced after F3-M3, per
BLIZZARD §7 F4-M5 (*"Sequence the knob that arms it after F3-M3, never before"*).

"Default-off" is what makes "now" safe: see §4.

---

## 3. SUSPECT DISPOSITION

Every S-item from `Amix/tmp/2026-08-24-lc060-recon/RECON.md`, re-verified in-tree today against
`src/z3660.c` as it stands at `ab04824`.

| # | claim | re-verified? | disposition |
|---|---|---|---|
| **S2** | WRITE descriptor + doorbell, no `cpush` ⇒ stale data to disk | **yes** — `z3660.c:573-576`, no cache op anywhere in the file | **FIXED (M1)**: per-chunk `cpushl` before the doorbell |
| **S3** | READ completion, no `cinv` ⇒ fresh bytes masked / evicted over | **yes** — `z3660.c:578-584` | **FIXED (M1)**: pre-arm `cpushl` + post-completion per-range `cinvl`, **direct-DMA arm only** (see §5.2) |
| **S4** | completion read non-serialized under `DTT0 CM=11`; in order only while the store buffer is off | **yes** — `CM=11` decoded §1.1; store buffer off per `060-D-CACHE-KNOBS-PLAN.md:31` | **BARRIER WRITTEN, gated (M1)**: a serializing instruction between doorbell and readback; **necessity confirmed at the store-buffer rung, not before** |
| **S5/S6** | bounce `bcopy` aliasing | **yes** — `z3660.c:572` (WRITE), `:584` (READ) | **SPLIT, and the halves are not alike** — see §3.1 |
| **S8** | synthesized INQUIRY/sense/capacity/mode-sense are 36/18/8/12/16 B, **sub-cache-line** | **yes** — `z3660.c:747-853`; lengths confirmed by reading each case | **FIXED (M1), and the naive fix is refuted** — see §3.2 |
| **S10** | `sptalloc` arm: "this kernel has no `PG_CI` bit" is an 030 fact restated as universal | **yes** — `z3660.c:332-335` | **COMMENT FIXED + ARM DECLARED UNSUPPORTED (M0)** — see §3.3 |
| **S11** | chunk loop has no page-cross check; the cdfs law is not applied | **yes** — `z3660.c:566-590` | **CENSUS ONLY this round, deliberately** — see §3.4 |
| — | `BOUNCE_THRESH` header falsehood | **yes** — `z3660.c:27-29,39,76` vs `amix_ram.h:41` | **COMMENT FIXED; the constant is kept** — see §3.5 |

### 3.1 S5/S6 — the two bounce arms are not symmetric

- **WRITE-side bounce is driver-decided and unreachable.** `z3660.c:571` bounces when
  `(ulong)data < BOUNCE_THRESH` = `< 0x08000000`, and `AMIX_RAM_GUEST_BASE` **is**
  `0x08000000` (**measured**, `Z3660 .../src/amix_ram.h:41`). Every AMIX physical address is
  at or above that bound, so this branch cannot fire on any shipping configuration. A counter
  turns "cannot" into "did not".
- **READ-side bounce is firmware-decided and fully live.** It is gated on `USED_DMA`
  (`z3660.c:582-584`), which the firmware sets. This arm is reachable today and its cache
  discipline is the *opposite* of the direct arm's (§5.2) — getting it wrong is a
  data-destroying bug, not a stale-data bug.

### 3.2 S8 — and why the obvious fix would destroy data

The synthesized responses are 8–36 bytes. The buffer is written by the **CPU**, through the
low CI alias, and read by the consumer through whatever alias it owns.

The tempting move — invalidate the response range after writing it, so no stale alias line
survives — is **wrong, and would be a new bug**. `CINVL` discards a dirty line without writing
it back. An 8-byte response rounds outward to one or two 16-byte lines that also cover bytes
the driver never wrote; if any of those neighbours is dirty in cache, the invalidate throws
them away. That is the same partial-line class as `z3660eth`'s N6.

The correct discipline inverts the order:

> **Push-and-invalidate the range BEFORE the CPU writes it, never invalidate after.**

`CPUSHL` writes dirty lines back (so neighbours are preserved) and invalidates them (so no
stale alias survives). After it, the CI writes land in RAM with no cached line covering them,
and any later cached read re-fetches from RAM. One rule, one helper, and it is the same op the
DMA path uses for its prepare.

### 3.3 S10 — the comment, and the arm it protects

`z3660.c:332-335` says the sptalloc path *"could never have been cache-inhibited either — this
kernel has no `PG_CI` bit at all"*. That is true of the **030** kernel's `immu.h` and false of
the 040/060 line, whose leaf PTEs carry a two-bit `CM` field at bits 6-5 that the kernel
already writes (`hat_cm_ram`; u-area leaf `0x60` = noncachable —
`CACHES-ON-PLAYBOOK.md:15`, **measured**).

Worse than merely false: on the 040/060 kernel the device-map `CM` selection is *"the existing
DEFERRED TODO in `hat040.s`"* (`CACHES-ON-PLAYBOOK.md:215`, **measured**) — so an `sptalloc`ed
board window would take `hat_cm_ram`, i.e. **copyback**, making the mailbox cacheable and the
driver non-functional.

**Decision (D8 order): the `sptalloc` arm is UNSUPPORTED on real silicon until a later
milestone.** It is reached only by `autoconfig_rtg YES` (board at `0x40000000`), which is not
the shipped Z3660 configuration. M0 detects it and says so.

### 3.4 S11 — census this round, refusal only on evidence

The cdfs law is real and this project paid for it: *any `vtop`'d DMA buffer must not cross a
page*, because `segkmem_alloc` memory is virtually contiguous and **physically scattered**
(2026-07-13, the wild write that ate `/usr/sbin/umount`'s ELF magic). The reference shape is
`amix-cdfs/platform/amix-kernel/amix_kern_media.c:196-201`, which **refuses** and warns.

The driver's exposure is real in principle: `dd.c:240-258` does one `vtop` and passes
`bp->b_bcount` with no page clamp (**measured**), while `z3660_rw()` chunks at up to
`MAXXFER` = 64 KB with no page-cross check (**measured**, `z3660.c:566-590`).

**But a refusal is the wrong first move here, and the reason is evidence.** The raw path goes
through `amiga_dma_pageio()` (`dd.c:137`, `physdsk.c:35`) and `NOTES.md` records the resulting
granularity as one 2 KB page per transaction (**measured**, 2026-08-17). And empirically this
driver ran a complete 41-minute install and 19 000 verified writes with zero failures on
metal — which a routinely-page-crossing wild DMA could not have done. So either multi-page
requests do not occur, or the buffers behind them are physically contiguous.

Shipping a hard refusal on that evidence risks breaking the two lines this driver is currently
the root device of. **This round therefore counts and does not refuse.** `z3660_pagecross_n`
is a kmem-readable census with zero behavioural effect; the refuse-versus-split decision is
pre-registered as F3-M1b and is taken from the counter, on the box, in either direction.

> **Pre-registered reading of that counter, written before it exists:** if it stays 0 across a
> boot, an install and a `find / -type f | xargs sum`, the caller contract holds and the guard
> becomes a cheap permanent refusal. If it is non-zero, the driver must **split the chunk at
> the page boundary** — not refuse — because a non-zero count means the shipping path depends
> on multi-page transfers.

### 3.5 `BOUNCE_THRESH` — the constant is right for the wrong reason

The header claims (`z3660.c:27-29`, `:39`) that *"Amix RAM is < 0x08000000 so the firmware
always bounces"*. `AMIX_RAM_GUEST_BASE` **is** `0x08000000`, so nothing in AMIX RAM ever
bounces and the firmware DMAs directly into 68k RAM (**measured**, both sides). Every
accidental-coherence argument built on that sentence is void — which is precisely why S2/S3
are live.

The *constant*, however, survives: `data < 0x08000000` means "below the base of AMIX RAM",
i.e. "not an AMIX RAM address at all", which is a defensible guard against handing the
firmware an address it cannot reach. It is kept, re-derived from the firmware's own header with
the citation, and instrumented — because if it ever fires, the `bcopy` stages from something
that is not RAM.

**Open, and owned by the firmware, not by us:** there is no register by which the driver can
*ask* whether a given address needs staging; it can only be *told*, after the fact, by
`USED_DMA` on the READ path. Booked as a cross-repo question for `../Z3660`, not papered over
here.

---

## 4. THE GATE — how "now" is made safe

The driver is compiled **once**, by a 68020-targeting compiler
(`AMIX_KERNEL_CFLAGS ... -m68020`, **measured**), and that one object is linked into kernels
that run on an emulated 030, an emulated 040, and — from F4 — a real 68060. There is no
compile-time discriminator available to it, and `cputype` is **not** a symbol it may reference:
*"The stock kernel has NO cpu-type variable"* (**measured**, `cputype060.s:3`), so an `extern`
to it would break the `nm -u` clean gate on the 030 line.

So the gate is a driver-owned global, defaulting to off, poked through `/dev/kmem` — the
idiom this campaign already uses for `hg_on`, `i40_on` and `hat_cm_ram`:

```c
long z3660_cache = 0;   /* 0 = off (shipping default); 40 = 68040 DC; 60 = 68060 DC */
```

Properties this buys, all of them load-bearing:

1. **Every existing image behaves exactly as today.** No cache instruction is reachable with
   the flag at 0; the added cost is one `tstl`/`beq` per boundary.
2. **F4's proof is single-variable.** Same kernel, same driver object, flag off versus flag on.
   That is the cleanest possible shape for a first-silicon experiment and it makes F3-M3's
   canary protocol an A/B rather than a build comparison.
3. **It cannot be armed by accident**, and arming it is recorded in the session log.

The cache instructions are emitted as raw `.word`s through `__asm__ __volatile__` with the
address pinned in `%a0` by an explicit register variable — verified today against the real
toolchain (gcc 2.7.2.3 / GNU as 2.8.1, `-m68020 -traditional`), which accepts the construct and
emits `cpushl dc,%a0@` with the pointer genuinely in `%a0`.

Encodings, all **measured** by assembling and disassembling them today:

| instruction | word(s) |
|---|---|
| `cpushl dc,%a0@` | `f468` |
| `cinvl dc,%a0@` | `f448` |
| `nop` | `4e71` |
| `movec %dtt0,%d0` | `4e7a 0006` |
| `movec %dtt1,%d0` | `4e7a 0007` |
| `movec %cacr,%d0` | `4e7a 0002` |

**Why `cpushl` is always followed by `cinvl`.** On the 68040 `CPUSHL` pushes *and* invalidates.
On the 68060 that invalidation is conditional on `CACR.DPI`: *"DPI clear pushes and
invalidates; DPI set pushes while leaving the line valid... a shared future helper must either
assert that invariant or issue an explicit `CINVL`"*
(`A3091-B2-PREPARE-PATCH-SPEC.md:331-338`, **measured**). The same source records the explicit
`CINVL` as *"harmless"* on the 040. One unconditional sequence is therefore correct on both
parts with no CPU-class branch, and `CACR` is captured at attach so a set `DPI` is visible
rather than inferred.

---

## 5. THE BOUNDARIES — a correction to the plan

BLIZZARD §6.3 F3-M1 names `z3660_enter` / `z3660_leave` / `z3660_complete` as the boundaries,
inheriting the recon's "ready-made boundaries" line. **Those are the wrong boundaries for data
maintenance, and using them would introduce the §3.2 bug.**

- `z3660_enter`/`z3660_leave` (`z3660.c:254-271`) bracket the **whole CDB**, including the
  synthesized commands. A FROM_DEVICE invalidate hung there would fire on INQUIRY and REQUEST
  SENSE buffers the CPU itself just wrote.
- `z3660_complete` (`z3660.c:909`) drains a FIFO that may hold requests other than the one just
  transferred, at a point where consumers are already running.

They are the right bracket for the *mailbox registers*, which is what they were built for — and
those are CI for free (§1.3.1). The right boundary for **data** is the only place a firmware
DMA is ever armed:

> **`z3660_rw()`'s chunk loop** — it alone knows direction, physical address and length, per
> chunk, and it sits inside the existing spl6 bracket.

### 5.1 WRITE (TO_DEVICE), per chunk

```
1.  cpushl+cinvl  [data, data+len)      prepare: every cached alias written back to RAM,
                                        so the ARM reads current bytes (S2)
2.  if (data < BOUNCE_THRESH)           unreachable on AMIX RAM; counted, not assumed (S5)
        bcopy(data -> bounce)
3.  WRLONG ADDR1/ADDR2/ADDR3
4.  WRLONG P_WRITE                      doorbell (synchronous)
5.  -- no completion cache op --        TO_DEVICE never invalidates
```

Step 1 precedes step 2 deliberately: the `bcopy` *reads* `data` through the CI alias, so it
would stage stale bytes if a dirty alias line held newer ones.

### 5.2 READ (FROM_DEVICE), per chunk — and the arm that must NOT invalidate

```
1.  cpushl+cinvl  [data, data+len)      prepare: no dirty line may survive to be evicted
                                        over fresh device bytes; no stale valid line may
                                        survive to be read instead of them (S3)
2.  WRLONG ADDR1/ADDR2/ADDR3
3.  WRLONG P_READ                       doorbell (synchronous)
4.  serializing barrier                 gated; see S4 in §3
5.  z3660_dma = RDLONG(P_USED_DMA)
6a. if (z3660_dma != 0)                 BOUNCE arm: the CPU writes data, through the CI
        bcopy(bounce -> data)           alias.  NO INVALIDATE -- it would discard the
                                        bytes just written and their line neighbours.
6b. else                                DIRECT arm: the ARM wrote RAM behind the CPU's
        cinvl [data, data+len)          back.  Invalidate before any consumer reads.
```

**Why step 6b is partial-line-safe here, where the a3091's is not.** `dma_cache040.s` accepts
an outward-rounded invalidate on the argument that *"the CPU was forbidden to touch the owned
rounded range"* during an asynchronous transfer. This driver has a stronger argument available:
step 1 already pushed and invalidated the rounded range, the whole transaction runs at spl6,
the mailbox op is synchronous, and the 040/060 data cache does not prefetch — so **no line
covering the range can be dirty when step 6b runs**. The invalidate cannot discard anything.
That argument is recorded here because it is the thing that would stop being true if this
driver ever became asynchronous.

**Why step 6a needs no invalidate of its own.** Step 1 left no cached line covering the range;
the `bcopy`'s CI writes land directly in RAM; a later cached read through any alias re-fetches
from RAM. Coherent, with one op instead of two, and without the §3.2 hazard.

### 5.3 Synthesized responses

A single push-and-invalidate over `[cp->addr, cp->addr + cp->nbyte)` **before** the switch
writes anything, for the five synthesizing opcodes only (`REQUEST SENSE`, `INQUIRY`,
`READ CAPACITY(10)`, `MODE SENSE(6)`, `MODE SENSE(10)`). Rationale in §3.2. Block READ/WRITE do
not take it — their maintenance is per-chunk in `z3660_rw()`.

---

## 6. THE SHIPPING-PATH PROOF

The 030 and emulated-040 lines are the shipping deployments and must not move. Three
independent proofs, in increasing strength:

1. **Comment-only commits are proven byte-identical.** The falsehood fixes (§3.3, §3.5) change
   no code, and the claim is *measured*, not asserted: cross-compile the object before and
   after with the real `AMIX_KERNEL_CFLAGS` and compare bytes. This is already the house
   standard in this repo (`CHANGELOG.md` 2026-08-18: *"the cross-compiled object is
   BYTE-IDENTICAL"*).
2. **Code commits are proven gate-dominated.** With `z3660_cache == 0` no cache opcode is
   reachable. Verified by disassembling the cross-compiled object and confirming that every
   `f468` / `f448` / `4e71` / `4e7a` site is dominated by a test of `z3660_cache`, and that the
   pre-existing instruction sequence between the mailbox accesses is otherwise unchanged.
3. **Behaviour is proven by the host harness.** `make -C test/host check` compiles the real
   driver and asserts the CDB synthesis byte-for-byte against the firmware oracle. The
   pre-change baseline is **77 gating passed / 0 failed, 6 parity matches / 0 deviations**, and
   that must hold after, with the new maintenance gates added on top.

The emulated arms are additionally protected by construction: an interpreted CPU executes the
`.word`s as no-ops on its absent cache, so even a wrongly-armed flag cannot corrupt them. That
is a backstop, not the argument — the argument is the gate.

---

## 7. F3-M3 — THE PRE-REGISTERED METAL PROTOCOL

**Written before the session, per BLIZZARD §6.3. The bench structurally cannot run this**:
Amiberry models no 68040/060 copyback data cache, which is on `ACCEPTANCE.md`'s own list of
things it is not evidence for. Only the real 68060 can score it.

**Adapted from the cdfs canary pattern (2026-07-13), which closed this exact class of bug on
this exact platform.** Pass line is the bar that fix was accepted at, unchanged.

### Preconditions (all four, or the run is void)

1. `z3660_direct_map == 1` and `z3660_ci_ok == 1` read back live from `/dev/kmem` — the free
   ride asserted, not assumed (F3-M0).
2. `z3660_dtt0`, `z3660_dtt1`, `z3660_cacr` captured into the evidence directory. `CACR.DPI`
   (bit 28) recorded; if set, the run is annotated, not silently accepted.
3. The store buffer (`CACR` bit 29) is **OFF**. S4's hazard must not be armed during the run
   that scores the maintenance — one variable at a time.
4. Baseline thermal reading taken, and the F0 thermal policy in force (WARN 60 °C / +30 °C,
   ABORT 70 °C / +35 °C).

### The A/B

Same kernel, same driver object, both arms:

- **Arm A — `z3660_cache = 0`.** The unmaintained driver on real caches. This arm is expected
  to **fail**, and a run in which it passes is a finding in its own right: it would mean the
  exposure is not what §1 says it is, and the maintenance would need re-justifying rather than
  celebrating.
- **Arm B — `z3660_cache = 60`.** The maintained driver.

Running A first is deliberate. A protocol that only ever runs the fixed arm cannot distinguish
"the fix works" from "there was nothing to fix"; the cdfs case was believed precisely because
the bug reproduced first.

### The cycle (one cycle, repeated)

1. Write a known payload set to the piscsi disk: ≥ 8 files, mixed sizes spanning
   sub-cache-line (< 16 B), sub-page (< 2048 B) and multi-page (≥ 64 KB).
2. Concurrent load, running throughout: a `find / -type f | xargs sum` sweep plus sustained
   ethernet traffic — the concurrency is the point, because a single-threaded read-back cannot
   dirty the cache under the transfer.
3. Cold-drop the buffer cache (unmount/remount, or a large enough sweep to evict), then read
   every payload file back and compare **whole-file checksums**, byte-identical.
4. Checksum the **8 canary binaries** — including `/usr/sbin/umount`, whose ELF magic is what
   caught the cdfs wild write — and compare against the pre-run values.
5. Read and record the counters: `z3660_push_n`, `z3660_inv_n`, `z3660_push_bytes`,
   `z3660_inv_bytes`, `z3660_bounce_rd_n`, `z3660_bounce_wr_n`, `z3660_pagecross_n`,
   `z3660_nest_hits`, `z3660_cq_overflow`, `z3660_sptalloc_unsafe`.

### Pass line, pre-registered

> **15/15 cycles clean, 8/8 canaries byte-identical, on arm B.**
>
> Plus, as gates on the counters rather than decoration:
> `z3660_nest_hits == 0`, `z3660_cq_overflow == 0`, `z3660_sptalloc_unsafe == 0`,
> `z3660_bounce_wr_n == 0` (§3.1's unreachability, measured at last), and `z3660_push_n > 0`
> (proof the maintenance was actually on the live path — a green run with a zero counter is a
> **failed** run, because it proves nothing was exercised).

### Kill / fallback, pre-registered

- **Arm B fails with a named mechanism** ⇒ fix it and re-run; that is a bug report, not a
  re-scope.
- **Arm B fails three times with no named mechanism** ⇒ stop. Fall back to the documented,
  asserted free ride (F3-M0) with `z3660_cache = 0`, declare the maintenance arm parked, and
  write the failure up. A documented dependence, never silence (BLIZZARD §6.3).
- **Arm A passes** ⇒ do not ship the maintenance on the strength of a theory the hardware
  declined to confirm. Re-derive §1 against the measurement and report the contradiction.

---

## 8. WHAT THIS ROUND DELIBERATELY DOES NOT DO

- **The `sptalloc` arm is not made safe** — it is declared unsupported and detected (§3.3).
  Making it safe needs a `PG_CI`-equivalent leaf `CM` for device maps, which is `hat040.s`'s
  deferred TODO in another repo.
- **S11 is not enforced** — it is counted (§3.4). Enforcing it on today's evidence risks the
  shipping root path.
- **S4's barrier is not validated** — the instruction is placed and gated, and the claim that
  it serializes pending writes on the 68060 is **assumed** until read out of the MC68060UM.
  It costs nothing today (the hazard is unarmed) and the manual check is owed before the
  store-buffer rung, exactly as F1-M0's PCR check was owed before any PCR code.
- **Nothing is proven about real caches.** Everything in this round is proven against the
  toolchain, the object and the host harness. §7 is the only thing that can score correctness,
  and it needs the metal.

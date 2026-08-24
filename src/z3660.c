/*
 * z3660.c -- Z3660 accelerator onboard SCSI for Amix (SVR4 / 68030).
 *
 * The Z3660's "native SCSI" is NOT an NCR chip -- it is the PiStorm "piscsi"
 * mailbox protocol, ported to the Z3660's Zynq ARM (open source: shanshe/Z3660,
 * z3660-drivers/scsi/z3660_scsi.c).  There is no 53C710, no SCRIPTS, no DSA, no
 * SCSI bus phases and no interrupt/poll completion: it is a tiny synchronous MMIO
 * register mailbox.  The piscsi registers ride the Z3660's combined RTG+SCSI
 * window: with autoconfig_rtg YES it is a Zorro III AutoConfig board (manuf
 * 0x144B, product 0x01) and KS places it high (0x40000000+); with autoconfig_rtg
 * NO (the usual config) the very same window sits at a FIXED 0x10000000 and is
 * never autoconfigured at all.  On Amix 2.1 the bootinfo autocon table is also
 * unreliable on real metal (see grimoire-amix: hydra detection).  So detection
 * is multi-method, like the Hydra driver: try autocon() first, then -- only on
 * an AGA machine (VPOSR >= 0x22; the ECS build box must never touch it) -- probe
 * the fixed base directly and verify the mailbox answers (DRVTYPE reads 0/1).
 *
 * BOTH windows this driver maps are BOARD apertures, not kernel RAM -- including
 * the "bounce buffer", which is the FIRMWARE's own staging area seen through the
 * Zorro window (SCSI_NO_DMA_ADDRESS = RTG_BASE+0x80000, Z3660 src/memorymap.h),
 * never a DMA buffer this driver allocates.  So neither needs a mapping call at
 * all when the board sits below VSECT1; see the mapping note above z3660map().
 *
 * Protocol (board_base-relative, all 32-bit MMIO):
 *   register window  = board_base + 0x2000  (commands written/read as longs at
 *                      regs + cmd_offset)
 *   bounce buffer    = board_base + 0x80000 (<=64KB, the firmware's staging area
 *                      for transfers it cannot DMA directly)
 *
 *   CORRECTION 2026-08-24 (BLIZZARD F3): this comment used to claim "Amix RAM is
 *   < 0x08000000 so the firmware always bounces".  That is FALSE and it is
 *   load-bearing: AMIX_RAM_GUEST_BASE IS 0x08000000 (Z3660 src/amix_ram.h:41), so
 *   every AMIX physical address is at or above the threshold and NOTHING in AMIX
 *   RAM is ever staged by the driver -- the firmware DMAs straight into 68k RAM.
 *   Every "the bounce keeps us coherent" argument built on that sentence is void,
 *   which is why this driver needs real CPU cache maintenance on real silicon.
 *   See docs/BLIZZARD-F3.md.
 *
 *   A block READ/WRITE is:  write DRVNUMX(unit); write the operation's OWN address
 *   triple (read/write use separate registers, never shared) -- READ_ADDR1/2/3 =
 *   0x20/0x24/0x28, WRITE_ADDR1/2/3 = 0x240/0x244/0x248 -- ADDR1=block number,
 *   ADDR2=byte length, ADDR3=buffer phys addr; then write the command
 *   register (READ=0x04 / WRITE=0x00, value = unit).  That single command-register
 *   write is BOTH the trigger and the completion -- the ARM intercepts the Zorro III
 *   bus cycle and finishes the whole transfer before the write returns.  After a
 *   READ, read USED_DMA(0x9C): if nonzero the data is in the bounce buffer, copy it
 *   out.  Before a WRITE, if the buffer is < 0x08000000 copy it into the bounce.
 *
 *   INQUIRY / READ_CAPACITY / MODE_SENSE / TEST_UNIT_READY are interpreted *here*
 *   (the firmware only does block read/write + geometry queries) -- mirroring
 *   piscsi_scsi() in z3660_scsi.c.
 *
 * z3660queue() plugs into Amix sd.c exactly like a4091queue(): sd.c gives us a CDB
 * in cp->cdb, target in cp->unit, buffer in cp->addr, length in cp->nbyte; we run
 * it and call (*cp->intr)(cp).  Add to sd.c scsicard[]:
 *     0x144B0001, &z3660queue, "Z3660 SCSI"
 *
 * STATUS: written from the open-source protocol; compiles + integrates + boots in
 * the Amix build box.  HARDWARE interaction (the actual ARM mailbox) is validated
 * only on a real A4000+Z3660 or against an Amiberry piscsi emulation -- Amiberry
 * does not emulate the Z3660, so when absent autocon() returns 0 and this driver
 * fails the I/O gracefully (harmless), just like the A4091 driver when no A4091.
 *
 * Refs: repo/z3660-drivers/scsi/{z3660_scsi.c,z3660_scsi_enums.h},
 *       repo/KNOWN_ISSUES.md, ../amix-a4091/src/a4091-wr.c (Amix framework).
 */
#include	"sys/types.h"
#include	"sys/immu.h"		/* PG_V, phystopfn, paddr_t */
#include	"sys/errno.h"
#include	"sys/inline.h"		/* spl6()/splx(): the mailbox-transaction bracket
					 * (spl6 => _spl4 => move.w #0x2400,%sr, IPL 4).
					 * Included like flop.c/hd.c; under HOST_TEST it
					 * resolves to test/host/stubs/sys/inline.h. */
#include	"rico.h"
#include	"sd.h"

#define	Z3660_PROD	0x144B0001	/* autocon pc = (manufacturer<<16)|product */
#define	Z3660_FIXED	0x10000000	/* combo window base when not autoconfigured */
#define	VPOSR		0xDFF004	/* Agnus/Alice id: bits 8-14 >= 0x22 -> AGA */
#define	PISCSI_OFFSET	0x00002000	/* register window within the board */
#define	BOUNCE_OFFSET	0x00080000	/* bounce buffer within the board */
#define	BOUNCE_BYTES	0x00010000	/* size of that bounce aperture: 64KB */
#define	MAXXFER		65536		/* max bytes per piscsi op */
/*
 * The WRITE-side staging threshold.  Its VALUE is right; the reason written
 * beside it for a year was not.  0x08000000 is AMIX_RAM_GUEST_BASE (Z3660
 * src/amix_ram.h:41) -- the BASE of AMIX RAM, not a ceiling -- so this test does
 * not mean "the firmware bounces AMIX RAM" (it never does, see the header
 * correction).  It means "this address is below AMIX RAM entirely", i.e. not
 * something the firmware can reach, and staging it is the conservative answer.
 * On every shipping configuration the branch is unreachable; z3660_bounce_wr_n
 * exists so that "unreachable" is measured rather than believed.
 */
#define	BOUNCE_THRESH	0x08000000	/* = AMIX_RAM_GUEST_BASE: below this is not AMIX RAM */
/*
 * Board-window geometry is expressed in BYTES, because every quantity above is a
 * fact about the FIRMWARE's address map (Z3660 src/memorymap.h), not about this
 * kernel's paging: the aperture is 64KB at board+0x80000 whatever a page happens
 * to be.  Page COUNTS are derived from these byte sizes further down, next to the
 * register offsets they are measured against -- never the other way round.  The
 * page count used to be the primary constant here (BOUNCE_PAGES 32, span =
 * BOUNCE_PAGES * NBPP), which silently made the span a function of the page size:
 * the same source that spans 64KB on this NBPP=2048 kernel would have claimed
 * 128KB of window -- and of sptmap -- on a 4KB-page kernel.
 */
#define	BOUNCE_SPAN	BOUNCE_BYTES	/* window span, in bytes, at any page size */
/* highest board-relative byte this driver ever touches, +1 (the bounce top;
 * the register window at PISCSI_OFFSET sits far below it) */
#define	Z3660_WINDOW_TOP	(BOUNCE_OFFSET + BOUNCE_SPAN)	/* 0x00090000 */

/* piscsi command-register offsets (added to the register-window base) */
#define	P_WRITE		0x00		/* trigger block WRITE  (value = unit) */
#define	P_READ		0x04		/* trigger block READ   (value = unit) */
#define	P_DRVTYPE	0x0C
#define	P_BLOCKS	0x10
#define	P_READ_ADDR1	0x20		/* block number  */
#define	P_READ_ADDR2	0x24		/* byte length   */
#define	P_READ_ADDR3	0x28		/* buffer address */
#define	P_DRVNUMX	0x90		/* select unit for subsequent ops */
#define	P_USED_DMA	0x9C		/* read after READ: !=0 => data is in bounce */
/*
 * Peripheral device type of the currently-selected unit: 0x00 = direct-access
 * disk, 0x05 = read-only CD-ROM.  This is the SINGLE source of device type.
 * P_PDT MUST stay in lockstep with the firmware register PISCSI_CMD_PDT (Z3660
 * src/scsi/z3660_scsi_enums.h) and its a3000_scsi mirror -- all three are 0xA0.
 */
#define	P_PDT		0xA0
#define	P_WRITE_ADDR1	0x240
#define	P_WRITE_ADDR2	0x244
#define	P_WRITE_ADDR3	0x248
#define	P_BLOCKSIZE0	0x200		/* + unit*4, units 0..7 */
#define	P_BLOCKS0	0x220		/* + unit*4, units 0..7 */
/*
 * Depth of the piscsi per-unit register arrays, and so of this driver's per-unit
 * geometry cache (see z3660_geom[] below): P_BLOCKSIZE0..+0x1C and
 * P_BLOCKS0..+0x1C, with P_WRITE_ADDR1 starting immediately after them at 0x240.
 */
#define	Z3660_NUNITS	8

/*
 * PAGE GEOMETRY -- derived from the byte sizes above, never primary.
 *
 * The only consumer of a page COUNT in this driver is sptalloc(), whose API takes
 * one (and which is reached only by the >= VSECT1 board; see z3660map()).  Both
 * counts are therefore a rounded-up conversion of a firmware byte size, so the
 * driver claims the same 64KB+register-file of BOARD WINDOW whatever the page
 * size -- 33 sptmap pages on this NBPP=2048 kernel, 17 on a 4KB-page one --
 * instead of silently doubling the span, and the sptmap bill, with the page size.
 *
 * REGS_BYTES is the piscsi register file itself: offsets 0x00 through the last
 * register this driver touches, P_WRITE_ADDR3, inclusive of its long.  It is
 * derived from that offset rather than written out, so adding a higher register
 * above cannot leave the mapping short.
 */
#define	Z3660_PAGES(b)	(((b) + NBPP - 1) / NBPP)	/* bytes -> pages, rounded up */
#define	REGS_BYTES	(P_WRITE_ADDR3 + 4)		/* 0x24C: the whole register file */
#define	REGS_PAGES	Z3660_PAGES( REGS_BYTES)
#define	BOUNCE_PAGES	Z3660_PAGES( BOUNCE_BYTES)

/*
 * Compile-time guards on that geometry.  Everything here is an integer macro, so
 * these are plain preprocessor conditionals -- the only assertion form this
 * K&R-era target compiler (gcc 2.7.2.3) supports with no generated code at all.
 *
 * Each page count is pinned from BOTH sides, because a hardcoded count fails in
 * both directions as the page size moves: too few pages leaves the tail of the
 * window unmapped, too many burns sptmap entries (the scarce resource this
 * driver's direct-map path exists to stop spending).  Together the pair asserts
 * count == ceil(bytes / NBPP) exactly.  Neither can fire while the counts stay
 * derived; that is the point -- they fire the moment someone re-hardcodes one,
 * which is exactly how the span became page-size-dependent in the first place.
 */
#if	BOUNCE_BYTES < MAXXFER
#error	"z3660: one MAXXFER chunk must fit the firmware bounce aperture"
#endif
#if	(BOUNCE_PAGES * NBPP) < BOUNCE_SPAN
#error	"z3660: BOUNCE_PAGES does not cover BOUNCE_SPAN -- derive it, do not hardcode it"
#endif
#if	((BOUNCE_PAGES - 1) * NBPP) >= BOUNCE_SPAN
#error	"z3660: BOUNCE_PAGES over-covers BOUNCE_SPAN -- wasted sptmap pages"
#endif
#if	(REGS_PAGES * NBPP) < REGS_BYTES
#error	"z3660: REGS_PAGES does not cover the piscsi register file"
#endif
#if	((REGS_PAGES - 1) * NBPP) >= REGS_BYTES
#error	"z3660: REGS_PAGES over-covers the piscsi register file"
#endif
/*
 * Z3660_WINDOW_TOP is a FIRMWARE byte bound (the top of the 64KB aperture at
 * board+0x80000), so it must evaluate to the same address on every page size.
 * This is the guard that would have caught the old page-count-primary shape: on a
 * 4KB-page kernel that spelling made the window top 0x000A0000.
 */
#if	Z3660_WINDOW_TOP != 0x00090000
#error	"z3660: the board window top must not vary with NBPP"
#endif
/*
 * sptalloc() maps by FRAME (phystopfn() truncates), so a board offset that is not
 * page-aligned would silently map a window shifted down to the frame boundary.
 * True for every page size up to 0x2000; assert it rather than assume it.
 */
#if	(PISCSI_OFFSET % NBPP) != 0 || (BOUNCE_OFFSET % NBPP) != 0
#error	"z3660: board window offsets must be page-aligned"
#endif

/* SCSI opcodes we interpret */
#define	C_TUR		0x00
#define	C_REQ_SENSE	0x03
#define	C_INQUIRY	0x12
#define	C_MODE_SENSE6	0x1A
#define	C_START_STOP	0x1B
#define	C_MODE_SENSE10	0x5A
#define	C_READ_CAP10	0x25
#define	C_READ_6	0x08
#define	C_WRITE_6	0x0A
#define	C_READ_10	0x28
#define	C_WRITE_10	0x2A

extern int	autocon();
extern caddr_t	sptalloc();
extern void	bcopy();
extern int	printf();

/* per-unit static-geometry cache; defined below, filled from z3660map() above it */
static void	z3660_geom_fill();

static volatile uchar	*regs;		/* board+0x2000 register window  */
static volatile uchar	*bounce;	/* board+0x80000 bounce buffer   */
static long		board_phys;

#ifndef	HOST_TEST
#define	WRLONG(cmd,val)	(*(volatile ulong *)(regs + (cmd)) = (ulong)(val))
#define	RDLONG(cmd)	(*(volatile ulong *)(regs + (cmd)))
#endif	/* !HOST_TEST: the host harness force-includes its own WRLONG/RDLONG
	 * (test/host/mock_regs.h) that drive a mock piscsi mailbox instead of
	 * real MMIO.  Inert for the kernel build -- no HOST_TEST, no change. */

/* last-transaction diagnostics (read via /dev/mem or a probe tool) */
ulong	z3660_lastblock, z3660_lastlen, z3660_blocks0, z3660_dma;
uchar	z3660_rc, z3660_lastcmd, z3660_present;
uchar	z3660_direct_map;	/* 1 = section-0 identity, 0 = sptalloc'd */

/*
 * Pending REQUEST SENSE data for the next C_REQ_SENSE.  A zero key means
 * "nothing pending" -- the disk path never sets it, so its REQUEST SENSE keeps
 * returning all-zeros exactly as before.  A CD-ROM WRITE reject sets DATA
 * PROTECT here (0x07/0x27/0x00), mirroring set_sense() in the a3000_scsi mirror.
 */
static uchar	z3660_sense_key, z3660_sense_asc, z3660_sense_ascq;

/*
 * Permanent mailbox-transaction reentrancy detector (production; both counters
 * are non-static so a userland tool can read them via /dev/kmem).  Every
 * synchronous piscsi transaction is bracketed by z3660_enter()/z3660_leave():
 *
 *   z3660_enter() raises to spl6 -- masking the CIA-A level-2 clock interrupt
 *   that is the SOLE trigger of timeout() callout dispatch (see NOTES.md
 *   "callout-IPL"), so a clock-driven completion can never dispatch z3660done ->
 *   ihandle -> startio -> a NESTED mailbox transaction inside an in-flight one --
 *   then bumps z3660_nest_depth.
 *
 *   z3660_leave() drops the depth and restores the caller's IPL.
 *
 * If a transaction is ever entered while another is still in flight
 * (z3660_nest_depth != 0) z3660_nest_hits is bumped.  With the bracket in place
 * that must stay 0 forever; a nonzero z3660_nest_hits is proof the guard was
 * breached.  The accounting is a couple of cheap, branch-light instructions and
 * stays in the shipping driver.
 */
ulong	z3660_nest_depth, z3660_nest_hits;

/*
 * ---------------------------------------------------------------------------
 * BLIZZARD F3 -- CPU data-cache coherence on real 68040/68060 silicon.
 * Full rationale, suspect table and metal protocol: docs/BLIZZARD-F3.md.
 * ---------------------------------------------------------------------------
 *
 * WHY THERE WAS NEVER ANY CACHE CODE HERE.  Every deployment of this driver so
 * far has run on an EMULATED CPU -- the Z3660 carries the guest 030/040 on
 * core1's interpreter and the socketed 68LC060 is a bus-parked passenger.  An
 * interpreter has no data cache, so the maintenance this file never did cost
 * nothing.  BLIZZARD F4 puts this code on silicon that has caches, and there
 * the same program is wrong in both directions: a WRITE hands the firmware a
 * physical page whose newest bytes are still in the CPU's cache, and a READ
 * lets the ARM overwrite a page whose stale dirty lines are evicted over the
 * fresh bytes afterwards.
 *
 * WHAT DOES *NOT* SAVE US.  Three things are routinely mistaken for coherence
 * here and none of them reaches this class:
 *   - DTT0 = 0x003fc060 (pstart040.s:326) inhibits the low 1 GB for data, but a
 *     TTR matches LOGICAL addresses.  It covers THIS DRIVER'S accesses (cp->addr
 *     is a physical address by contract, dereferenced through the low identity
 *     alias) and the board window.  It does NOT cover the same physical page's
 *     other aliases -- and user pages are copyback (hat_cm_ram = 0x20, default
 *     since 2026-07-30), which is exactly where a raw-I/O buffer lives.
 *   - /SNOOP covers 68k local-bus cycles only; the piscsi path has the ARM write
 *     Zynq DDR from its own memory system, which is not a 68k bus cycle at all,
 *     and on v0.2 boards the net is a no-connect anyway.
 *   - the bounce buffer.  See the BOUNCE_THRESH correction above: nothing in
 *     AMIX RAM is ever staged, so there is no copy step to be coherent behind.
 *
 * THE GATE.  This file is compiled ONCE, with -m68020, and the one object is
 * linked into kernels that run on an emulated 030, an emulated 040 and (from F4)
 * a real 68060.  There is no compile-time discriminator available, and `cputype`
 * may NOT be referenced -- the stock kernel has no such symbol, so an extern to
 * it would break the nm -u clean gate on the 030 line.  So the class is a
 * driver-owned global, default OFF, poked through /dev/kmem exactly like the
 * port lane's hg_on / i40_on / hat_cm_ram:
 *
 *     z3660_cache = 0   no cache instruction is reachable.  THE SHIPPING
 *                       DEFAULT: every existing image behaves as it always has,
 *                       for the cost of one tstl/beq per boundary.
 *                 = 40  real 68040 data cache
 *                 = 60  real 68060 data cache
 *
 * That default is what makes it safe to land this before F4 rather than after,
 * and it makes F4's proof single-variable: same kernel, same object, flag off
 * versus flag on.
 */
long	z3660_cache;		/* 0 = off (default), 40 = 68040 DC, 60 = 68060 DC */
long	z3660_ci_enforce;	/* 0 = warn on a failed free-ride check, else refuse */

/* attach-time capture + verdict (all kmem-readable; see docs/BLIZZARD-F3.md §7) */
ulong	z3660_dtt0, z3660_dtt1, z3660_cacr;
ulong	z3660_ci_ok;		/* 0 = not checked, 1 = free ride verified, 2 = FAILED */
ulong	z3660_sptalloc_unsafe;	/* sptalloc arm taken with maintenance armed */

#ifndef	HOST_TEST
/*
 * 040/060 control-register reads, as raw .word so the -m68020 assembler accepts
 * them, with the destination pinned by an explicit register variable.  Verified
 * against this toolchain (gcc 2.7.2.3 / GNU as 2.8.1) by disassembly:
 *   4e7a 0006 = movec %dtt0,%d0   4e7a 0007 = movec %dtt1,%d0
 *   4e7a 0002 = movec %cacr,%d0
 * Every caller is gated on z3660_cache != 0, so none of this is reachable on the
 * 68030 line, where these words would take a line-F exception.
 */
static ulong
z3660_rd_dtt0()
{
	register ulong	v __asm__("d0");

	__asm__ __volatile__( ".word 0x4e7a,0x0006" : "=d" (v));
	return v;
}

static ulong
z3660_rd_dtt1()
{
	register ulong	v __asm__("d0");

	__asm__ __volatile__( ".word 0x4e7a,0x0007" : "=d" (v));
	return v;
}

static ulong
z3660_rd_cacr()
{
	register ulong	v __asm__("d0");

	__asm__ __volatile__( ".word 0x4e7a,0x0002" : "=d" (v));
	return v;
}
#else	/* HOST_TEST: the harness supplies the register values so BOTH arms of the
	 * free-ride check can be driven -- a check only ever exercised on its
	 * passing arm is not a check. */
ulong	z3660_test_dtt0 = 0x003fc060;	/* the shipped pstart040.s value */
ulong	z3660_test_dtt1 = 0x807fa060;
ulong	z3660_test_cacr = 0x80008000;
static ulong z3660_rd_dtt0() { return z3660_test_dtt0; }
static ulong z3660_rd_dtt1() { return z3660_test_dtt1; }
static ulong z3660_rd_cacr() { return z3660_test_cacr; }
#endif	/* HOST_TEST */

/*
 * Classify one transparent-translation register against one address, for
 * SUPERVISOR DATA accesses:
 *   0 = does not cover this address
 *   1 = covers it, cache-inhibited      (the free ride)
 *   2 = covers it, CACHED               (the free ride is gone)
 *
 * TTR layout (040 and 060 alike; decode confirmed by pstart040.s's own comments,
 * which call ITT0 0x003fc000 "WT-cacheable" and DTT1 0x807fa060 "cache-inhib",
 * and record that serializing DTT0 meant CM 0x60 -> 0x40):
 *   31-24 base   23-16 mask   15 E   14-13 S   6-5 CM   (CM: 0 WT, 1 copyback,
 *   2 CI-serialized, 3 CI-nonserialized)
 */
static int
z3660_ttr_class( t, a)
ulong	t, a;
{
	ulong	base, mask, s, cm;

	if ((t & 0x8000) == 0)			/* E: disabled TTRs cover nothing */
		return 0;
	s = (t >> 13) & 3;			/* 0 = user only, 1 = supervisor only, 2/3 = both */
	if (s == 0)
		return 0;
	base = (t >> 24) & 0xFF;
	mask = (t >> 16) & 0xFF;
	if ((((a >> 24) & 0xFF) & ~mask) != (base & ~mask))
		return 0;
	cm = (t >> 5) & 3;
	return (cm >= 2) ? 1 : 2;
}

/*
 * F3-M0 -- assert the free ride instead of depending on it silently.
 *
 * The whole board window must be reached cache-inhibited, or this driver cannot
 * talk to the mailbox at all on a machine with a live data cache.  Today that is
 * true by accident of a kernel constant no code here names.  Check it once, at
 * attach, and say so out loud when it stops being true -- this is what catches a
 * future kernel that narrows DTT0 (already booked in the port lane as a separate
 * milestone) before the mailbox starts answering out of a stale cache line.
 *
 * The TTR granularity is 16 MB, so a 0x90000-byte window straddles at most one
 * boundary: checking both endpoints is exhaustive.  A covering-but-cached TTR
 * fails regardless of which register it is, and an address covered by NO enabled
 * TTR fails too -- it would fall to the page tables, whose cache mode this
 * driver cannot read and must not assume.
 *
 * Returns 0 to continue, ENXIO to refuse the attach (only when z3660_ci_enforce
 * is set: a first-silicon boot that refuses its own root device tells you
 * nothing, so the default is to measure loudly and carry on).
 *
 * Non-static so the host harness can drive BOTH arms: a check only ever
 * exercised on its passing arm is not a check.
 */
int
z3660_ci_check( lo, hi)
ulong	lo, hi;			/* board window: lo inclusive, hi exclusive */
{
	ulong	a;
	int	i, c0, c1, bad;

	if (z3660_cache == 0) {		/* emulated CPU: nothing to check, nothing to do */
		z3660_ci_ok = 0;
		return 0;
	}
	z3660_dtt0 = z3660_rd_dtt0();
	z3660_dtt1 = z3660_rd_dtt1();
	z3660_cacr = z3660_rd_cacr();

	bad = 0;
	for (i = 0; i < 2; ++i) {
		a  = i ? (hi - 1) : lo;
		c0 = z3660_ttr_class( z3660_dtt0, a);
		c1 = z3660_ttr_class( z3660_dtt1, a);
		if (c0 == 2 || c1 == 2 || (c0 == 0 && c1 == 0)) {
			bad = 1;
			printf( "z3660: board VA 0x%x is NOT cache-inhibited (dtt0 0x%x dtt1 0x%x)\n",
				a, z3660_dtt0, z3660_dtt1);
		}
	}
	/*
	 * The sptalloc arm is a separate failure and a worse one: its window is
	 * page-table-backed, and per-map device cache mode is still hat040.s's
	 * deferred TODO, so the mailbox would be mapped COPYBACK.  See the mapping
	 * note above z3660map().
	 */
	if (z3660_direct_map == 0) {
		z3660_sptalloc_unsafe++;
		bad = 1;
		printf( "z3660: sptalloc mapping arm is unsupported with a live data cache\n");
	}
	if (bad == 0) {
		z3660_ci_ok = 1;
		return 0;
	}
	z3660_ci_ok = 2;
	printf( "z3660: cache-inhibit assertion FAILED (cache=%d cacr 0x%x)\n",
		(int)z3660_cache, z3660_cacr);
	return z3660_ci_enforce ? ENXIO : 0;
}

/*
 * ---------------------------------------------------------------------------
 * F3-M1 -- the maintenance itself.
 * ---------------------------------------------------------------------------
 *
 * Line ops as raw .word, verified against this toolchain by disassembly:
 *   f468 = cpushl dc,%a0@     f448 = cinvl dc,%a0@     4e71 = nop
 *
 * WHY EVERY PUSH IS FOLLOWED BY AN INVALIDATE.  On the 68040 CPUSHL pushes AND
 * invalidates.  On the 68060 that invalidation is conditional on CACR.DPI --
 * clear pushes and invalidates, set pushes and leaves the line VALID.  The port
 * lane's own DMA contract (docs/contracts/A3091-B2-PREPARE-PATCH-SPEC.md) records
 * the explicit CINVL as harmless on the 040 and recommends it wherever invalid
 * state is part of the contract, which it is here.  One unconditional sequence is
 * then correct on both parts with no CPU-class branch, and CACR is captured at
 * attach so a set DPI is visible rather than inferred.
 *
 * WHY PUSH BEFORE, NEVER INVALIDATE AFTER, ON ANYTHING THE CPU WRITES.  CINVL
 * DISCARDS a dirty line without writing it back.  These ranges round outward to
 * 16-byte lines that also cover bytes this driver never touched; invalidating
 * after would throw away whatever else lived in those lines.  CPUSHL writes them
 * back first, so it is safe on a partial line where CINVL is not.
 */
ulong	z3660_push_n, z3660_inv_n;		/* line-op invocations                 */
ulong	z3660_push_bytes, z3660_inv_bytes;	/* bytes covered (pre-rounding)        */
ulong	z3660_bounce_wr_n;			/* WRITE staging fired -- expect 0     */
ulong	z3660_bounce_rd_n;			/* READ came back via the bounce       */
ulong	z3660_pagecross_n;			/* chunks spanning a page boundary     */
ulong	z3660_range_ovf;			/* impossible range refused            */

#ifndef	HOST_TEST
#define	Z3660_CPUSHL(p)	__asm__ __volatile__( ".word 0xf468" : : "a" (p) : "memory")
#define	Z3660_CINVL(p)	__asm__ __volatile__( ".word 0xf448" : : "a" (p) : "memory")
#define	Z3660_NOP()	__asm__ __volatile__( ".word 0x4e71" : : : "memory")
#define	Z3660_LINEPTR	register uchar *p __asm__("a0")
#else	/* the harness has no m68k cache to maintain; the ranging logic is still
	 * walked, and the counters still move, so both are testable on the host. */
#define	Z3660_CPUSHL(p)	((void)(p))
#define	Z3660_CINVL(p)	((void)(p))
#define	Z3660_NOP()	((void)0)
#define	Z3660_LINEPTR	uchar *p
#endif

/*
 * Push-and-invalidate every line covering [pa, pa+len).  `pa` is a PHYSICAL
 * address -- which is what this driver holds anyway (cp->addr is physical by
 * contract, NOTES.md 2026-07-11 (a)) and exactly what the line ops want: they
 * select by physical line on a physically-tagged cache, so one op covers EVERY
 * virtual alias of that RAM, including the copyback user mapping a raw-I/O
 * buffer actually lives in.  No vtop, no bp_map, no alias bookkeeping.
 */
static void
z3660_push( pa, len)
ulong	pa, len;
{
	Z3660_LINEPTR;
	ulong	end;

	if (z3660_cache == 0 || len == 0)
		return;
	if (pa + len < pa) {			/* wrap: refuse rather than loop wild */
		z3660_range_ovf++;
		return;
	}
	p   = (uchar *)(pa & ~15UL);
	end = (pa + len + 15) & ~15UL;
	while ((ulong)p < end) {
		Z3660_CPUSHL( p);
		Z3660_CINVL( p);
		p += 16;
	}
	z3660_push_n++;
	z3660_push_bytes += len;
}

/*
 * Invalidate every line covering [pa, pa+len) -- FROM_DEVICE completion only,
 * and only on the direct-DMA arm.
 *
 * The outward rounding is safe HERE in a way it is not in general: the same
 * range was pushed-and-invalidated before the transfer was armed, the whole
 * transaction runs at spl6, the mailbox op is synchronous, and the 040/060 data
 * cache does not prefetch -- so no line covering the range can be dirty when
 * this runs, and the invalidate cannot discard anything.  That argument is the
 * thing that stops being true if this driver ever becomes asynchronous.
 */
static void
z3660_inv( pa, len)
ulong	pa, len;
{
	Z3660_LINEPTR;
	ulong	end;

	if (z3660_cache == 0 || len == 0)
		return;
	if (pa + len < pa) {
		z3660_range_ovf++;
		return;
	}
	p   = (uchar *)(pa & ~15UL);
	end = (pa + len + 15) & ~15UL;
	while ((ulong)p < end) {
		Z3660_CINVL( p);
		p += 16;
	}
	z3660_inv_n++;
	z3660_inv_bytes += len;
}

/*
 * S4 -- the doorbell/readback ordering barrier.
 *
 * DTT0 is CM=11, cache-inhibited NONSERIALIZED, so the doorbell write and the
 * USED_DMA read that follows it are ordered only because the 060 store buffer is
 * currently OFF (CACR bit 29; 060-D-CACHE-KNOBS-PLAN.md lists turning it on as
 * candidate 1).  The moment that knob lands, the read could overtake a buffered
 * doorbell and return the PREVIOUS transaction's USED_DMA -- which would send the
 * driver down the wrong completion arm, silently.
 *
 * The barrier is placed now, gated, so it is in the code before the hazard is
 * armed.  It costs nothing today.  THAT `nop` SERIALIZES PENDING WRITES ON THE
 * 68060 IS ASSUMED, NOT MEASURED: the claim is owed a read of the MC68060UM
 * before the store-buffer rung, on the same discipline that made F1-M0 settle
 * PCR bit 1 from the manual before any PCR code shipped.
 */
static void
z3660_sync()
{
	if (z3660_cache == 0)
		return;
	Z3660_NOP();
}

static int
z3660_enter()
{
	int	s;

	s = spl6();				/* mask the clock; token = prior IPL */
	z3660_nest_hits += (z3660_nest_depth != 0);
	z3660_nest_depth++;
	return s;
}

static void
z3660_leave( s)
int	s;
{
	z3660_nest_depth--;
	splx( s);				/* restore the caller's IPL */
}

/*
 * Map the register window and the bounce buffer into kernel VA and verify the
 * mailbox answers.  0 on success; ENXIO when no Z3660 is present.
 *
 * Detection is multi-method (hydra-style): bootinfo's autocon table first;
 * when that misses (table unreliable on 2.1, or the board is outside the
 * autoconfig chain at the fixed base) probe Z3660_FIXED directly -- but only
 * on AGA, so the ECS build box (no Z3660, open bus at 0x10000000) never goes
 * there.  The mailbox is then verified by reading DRVTYPE (the firmware returns
 * only 0 or 1; anything else means open bus / not a piscsi window).
 *
 * MAPPING STRATEGY -- why the metal box needs no sptalloc() at all.
 *
 * AMIX's supervisor root table has FOUR level-A entries, one per 1 GB section
 * (tc_on encodes TIA=2; the table is built in ml/exp pstart()).  Entry 0, which
 * covers VA 0x00000000-0x3FFFFFFF, is a single EARLY-TERMINATION page descriptor
 * whose page address is 0 -- a straight identity map of the whole low 1 GB, RAM
 * or not.  That is why immu.h defines phystokv(p) as (p), why stock Amiga
 * drivers simply dereference the address autocon() gave them, and why the AGA
 * gate above can read VPOSR at 0xDFF004 before anything has been mapped.  Only
 * section 1 (VSECT1 = 0x40000000, the kernel-virtual arena) is page-table-backed,
 * and section 1 is what sptalloc() hands addresses out of.
 *
 * This is NOT the 68030 transparent-translation registers: amiga/boot/copyit.s
 * pmoves ZERO into %tt0/%tt1, and ttrap.s's "tt0_on" word 0x003F0143 has E=0 --
 * its own comment says "Disable".  TT is off in AMIX; the low-1 GB reachability
 * is a page-table early termination.  Same 0x40000000 boundary, different
 * mechanism than the "TT-gap safe" story this driver used to tell.  (The driver
 * already depends on that identity in its datapath: cp->addr is a PHYSICAL
 * address by contract -- see NOTES.md 2026-07-11 (a) -- and z3660_rw() both
 * bcopy()s through it and hands it to the firmware as *_ADDR3 unchanged.)
 *
 * So when the board sits below VSECT1 -- the shipped Z3660 config, autoconfig_rtg
 * NO, fixed base 0x10000000 -- its physical address ALREADY IS a valid supervisor
 * VA and no mapping call is needed.  That matters: sptalloc() draws from sptmap,
 * a HARDCODED 2048-page (4 MB) resource map that page[] -- sized by presented RAM
 * -- is carved out of first, so its free runs shrink as RAM grows (this is what
 * made the sibling z3660eth driver's 65-page mapping fail past ~83 MB, printing
 * "no Z3660 ethernet found" for a perfectly visible board).  The pages here
 * (REGS_PAGES + BOUNCE_PAGES -- 33 on this NBPP=2048 kernel) were sitting inside
 * that budget for no reason.
 *
 * BOTH windows can go direct because BOTH are board apertures, not kernel RAM.
 * The register window is obvious.  The bounce buffer is the subtler one: despite
 * the name it is NOT memory this driver allocates for DMA staging -- it is the
 * FIRMWARE's staging area at a fixed board offset (Z3660 src/memorymap.h:
 * SCSI_NO_DMA_ADDRESS = RTG_BASE+0x80000), which both sides address by that
 * offset and which the driver only ever bcopy()s to/from.  Its address is never
 * vtop()'d and never handed to the firmware (*_ADDR3 always carries the caller's
 * buffer), so moving it from an sptalloc'd VA to its physical address is
 * invisible to the datapath.  Hence 33 pages freed, not 1.
 *
 * sptalloc() is kept for a board at or above VSECT1 -- autoconfig_rtg YES puts
 * the combo window at 0x40000000, inside the page-table-backed section, exactly
 * the case a4091-init.c has always handled.  The direct path is chosen only when
 * the ENTIRE span the driver touches (base .. base+Z3660_WINDOW_TOP) stays below
 * VSECT1, so a board based just under the boundary still takes the mapped path
 * rather than running off the end of section 0.
 *
 * CACHE SEMANTICS -- CORRECTED 2026-08-24 (BLIZZARD F3).  This paragraph used to
 * say the two arms were identical because "this kernel has no PG_CI bit at all".
 * That is a 68030 fact (stock immu.h defines only PG_ADDR/PG_LOCK/PG_M/PG_REF/
 * PG_W/PG_V) restated as a universal one, and on the 040/060 kernel line it is
 * false: a leaf PTE there carries a two-bit CM field at bits 6-5 which the kernel
 * already writes (hat_cm_ram = 0x20 copyback for managed RAM; u-area leaf 0x60 =
 * noncachable).  The two arms are NOT alike:
 *
 *   DIRECT arm (below VSECT1, the shipped config): the board window is reached
 *   through the low identity alias, and pstart040.s:326 sets DTT0 = 0x003fc060 --
 *   the whole low 1 GB cache-inhibited for DATA.  The mailbox and the bounce
 *   aperture are CI for free.  This driver depends on that, so as of F3-M0 it
 *   ASSERTS it at attach (z3660_ci_check) instead of assuming it.
 *
 *   SPTALLOC arm (board at/above VSECT1, i.e. autoconfig_rtg YES): the mapping is
 *   page-table-backed, and per-map device CM is still hat040.s's deferred TODO --
 *   so the window would take hat_cm_ram, i.e. COPYBACK, and the mailbox would be
 *   cacheable.  That is not a slow driver, it is a broken one.  This arm is
 *   therefore UNSUPPORTED on real 040/060 silicon until that TODO lands; it is
 *   detected and counted (z3660_sptalloc_unsafe), not silently taken.
 *
 * Neither mapping was ever freed, on either path.
 */
static int
z3660map()
{
	long	base, size;
	ulong	t, bp;
	int	i, e;

	if (regs)
		return 0;
	unless (autocon( Z3660_PROD, 0, &base, &size)) {
		if ((((*(volatile ushort *)VPOSR) >> 8) & 0x7F) < 0x22) {
			z3660_present = 0;
			return ENXIO;	/* ECS/OCS machine -- no Z3660 here */
		}
		base = Z3660_FIXED;	/* AGA: probe the fixed combo window */
	}
	board_phys = base;
	bp = (ulong)base;

	/*
	 * Whole window inside the identity-mapped section 0?  Then the physical
	 * address is the kernel VA.  The subtraction cannot wrap: bp < VSECT1.
	 */
	if (bp < (ulong)VSECT1 &&
	    ((ulong)VSECT1 - bp) >= (ulong)Z3660_WINDOW_TOP) {
		z3660_direct_map = 1;
		regs   = (volatile uchar *)phystokv( (paddr_t)base + PISCSI_OFFSET);
		bounce = (volatile uchar *)phystokv( (paddr_t)base + BOUNCE_OFFSET);
	} else {
		z3660_direct_map = 0;
		regs   = (volatile uchar *)sptalloc( REGS_PAGES, PG_V,
				phystopfn( (paddr_t)base + PISCSI_OFFSET), 0);
		bounce = (volatile uchar *)sptalloc( BOUNCE_PAGES, PG_V,
				phystopfn( (paddr_t)base + BOUNCE_OFFSET), 0);
	}
	if (regs == 0 || bounce == 0) {
		regs = 0;		/* sptalloc failure, or a base of 0 */
		return ENOMEM;
	}
	WRLONG( P_DRVNUMX, 6);
	t = RDLONG( P_DRVTYPE);			/* firmware returns 0 or 1 only */
	if (t > 1) {
		regs = 0;			/* open bus / not a piscsi window */
		z3660_present = 0;
		return ENXIO;
	}
	z3660_present = 1;
	/*
	 * F3-M0: with a live CPU data cache, assert that the whole board window
	 * really is reached cache-inhibited before trusting a single further
	 * mailbox answer.  A no-op (and executes no 040/060 instruction) while
	 * z3660_cache is 0, which is every emulated deployment.
	 */
	if (e = z3660_ci_check( bp, bp + (ulong)Z3660_WINDOW_TOP)) {
		regs = 0;
		return e;
	}
	/*
	 * ATTACH: fill the per-unit static-geometry cache in one sweep of all 8
	 * piscsi units, so that no CDB ever pays for block size / block count /
	 * device type again.  See the cache's own comment below for the full
	 * invalidation rationale -- in short, every firmware remap is bracketed by
	 * a 68k reset that reloads this kernel, so the cache cannot go stale.
	 * Runs inside the caller's spl6 mailbox bracket, like the probe above.
	 */
	for (i = 0; i < Z3660_NUNITS; ++i)
		z3660_geom_fill( i);
	return 0;
}

/*
 * sd.c probe hook (see templates/sd.c.in @DRIVER_PROBES@): register the card
 * even when autocon() knows nothing about it.  Returns 1 and the board base
 * when the mailbox is alive.
 */
int
z3660present( ap)
char	**ap;
{
	int	r, s;

	s = z3660_enter();		/* the probe issues a mailbox register sequence */
	r = z3660map();
	z3660_leave( s);
	if (r)
		return 0;
	*ap = (char *)board_phys;
	return 1;
}

/*
 * ---------------------------------------------------------------------------
 * Per-unit static-geometry cache -- why it exists, and why it never needs
 * invalidating.
 * ---------------------------------------------------------------------------
 *
 * COST.  Every access to this board's window is a CROSS-CORE ROUND TRIP, not a
 * bus cycle: core1 (the guest's own CPU) publishes the access into the shared
 * struct and then HARD-SPINS, retiring no 68k instructions, until core0 picks it
 * up from a cooperative protothread loop that services at most ONE access per
 * iteration -- with the RTG paths, the ethernet thread and an unconditional
 * MPEG-decode call all sitting on the critical path of every one of them (Z3660
 * docs/piscsi-service-path.md 1.1-1.3).  A command's cost is therefore dominated
 * by how many REGISTERS it touches, not by how many bytes it moves.
 *
 * The pre-cache driver spent TEN round trips on a 2 KB READ and nine of them
 * carried no payload.  Four were pure waste, re-fetched on every single CDB:
 *
 *   P_BLOCKSIZE0 + unit*4   block size of the unit
 *   P_PDT                   peripheral device type (0x00 disk / 0x05 CD-ROM)
 *   P_BLOCKS0 + unit*4      block count of the unit
 *   P_DRVNUMX               a second, redundant unit select inside z3660_pdt()
 *
 * Caching those per unit leaves SIX trips for a READ and FIVE for a WRITE --
 * the descriptor triple, the doorbell, and the USED_DMA readback, i.e. only the
 * accesses that actually carry the request.  Nothing on the wire changes: no new
 * register, no new command, no reordering the firmware can observe (see ORDERING
 * below).  The firmware needs no change for this.
 *
 * WHEN COULD A CACHED FACT LEGITIMATELY CHANGE?  All three are fields of the
 * firmware's devs[unit] entry, and every one of them is written ONLY by
 * piscsi_map_drive() (Z3660 src/scsi/scsi.c): block_size at :924/:938/:953, pdt
 * from its is_cd argument, and the block count is derived from d->fs, the
 * f_size() of the backing .hdf captured when that file was opened.  These are
 * fixed-size image files opened once.  The firmware never resizes, re-opens or
 * re-types a mapped unit while the guest runs, and it has no media-change or
 * hot-attach path at all -- config.cd_target[] is consulted only inside
 * piscsi_init().
 *
 * piscsi_map_drive() has exactly two entry points, piscsi_init() and the drive
 * refresh, and BOTH run only with the 68k held in reset: main.c reset_thread()
 * calls piscsi_refresh_drives() and then sets state68k = M68K_RESET, and
 * cpu_emulator.c's reset arm calls piscsi_init() while the 68k is still held.
 * So every remap is bracketed by a guest reset, which reloads the AMIX kernel
 * and zeroes this driver's BSS along with it.  The cache therefore CANNOT
 * outlive the facts it caches, and no runtime invalidation hook is needed -- or
 * even reachable: there is no wire signal by which the guest could learn of a
 * remap, so a driver-side invalidate could never be triggered in the first
 * place.  Rescan/re-probe is covered too: z3660map() refills the whole cache,
 * and it re-runs whenever `regs` is 0.
 *
 * The ONE fact a rescan can genuinely change is whether a unit has a drive
 * mapped at all, so that fact is deliberately left UNCACHED: an entry is marked
 * valid only when the unit reports a nonzero block count -- the driver's own
 * long-standing "the firmware has no drive mapped here" test.  A unit that was
 * empty at attach is thus re-read on every command to it, exactly as before.
 * That is off the hot path, because such a command is refused without touching
 * the medium anyway.
 *
 * ORDERING (the one protocol subtlety).  P_PDT is not addressed per unit: it
 * returns devs[piscsi_cur_drive].pdt, so the unit must be SELECTED first (as
 * backend_drive_present()/fetch_geometry() do in the a3000_scsi Path B mirror).
 * And a read of P_BLOCKSIZE0+4n / P_BLOCKS0+4n REASSIGNS piscsi_cur_drive to n
 * as a side effect (scsi.c :1998, :1944) -- which is why the fill below does
 * both of those reads BEFORE writing P_DRVNUMX and reading P_PDT.  Since all
 * four accesses concern the same unit the firmware is left with
 * piscsi_cur_drive == unit either way, identical to the pre-cache sequence.  On
 * the command path the surviving P_DRVNUMX write still leaves
 * piscsi_cur_drive == unit before the doorbell, which keeps the firmware's
 * `val != piscsi_cur_drive` warning (scsi.c :1388) silent; the data path itself
 * indexes devs[val] from the doorbell's own value, never the selected drive.
 *
 * If a future firmware ever DOES gain a live remap or a media change, it must
 * announce it on the wire (a change counter, or an interrupt).  That is a
 * protocol change, owned by the Z3660 repo -- and this cache is the reason it
 * cannot be introduced there silently.
 */
/*
 * Non-static so a userland tool can read the latched geometry via /dev/kmem --
 * the same diagnostic convention as z3660_lastblock/z3660_blocks0 above.
 */
struct z3660_unitgeom {
	ulong	bs;		/* block size; the firmware's 0 already folded to 512 */
	ulong	nblocks;	/* total blocks; 0 == no drive mapped at this unit     */
	ulong	pdt;		/* 0x00 direct-access disk, 0x05 read-only CD-ROM      */
	ulong	valid;		/* nonzero == bs/nblocks/pdt are cached and usable     */
};
struct z3660_unitgeom	z3660_geom[Z3660_NUNITS];

/*
 * The answer for a unit outside the piscsi per-unit arrays -- byte-for-byte what
 * the pre-cache accessors returned when handed an out-of-range unit: block size
 * 512, no blocks (so every data command to it is refused), device type disk.
 */
static struct z3660_unitgeom	z3660_geom_absent = { 512, 0, 0x00, 0 };

/*
 * Read one unit's static geometry off the mailbox and latch it: four round trips,
 * paid once per unit at attach.  See ORDERING above for why the two per-unit
 * array reads must precede the P_DRVNUMX select and the P_PDT read.
 */
static void
z3660_geom_fill( unit)
int	unit;
{
	ulong	bs, nb;

	bs = RDLONG( P_BLOCKSIZE0 + unit * 4);
	nb = RDLONG( P_BLOCKS0 + unit * 4);
	WRLONG( P_DRVNUMX, unit);
	z3660_geom[unit].pdt     = RDLONG( P_PDT);
	z3660_geom[unit].bs      = (bs == 0) ? 512 : bs;
	z3660_geom[unit].nblocks = nb;
	z3660_geom[unit].valid   = (nb != 0);	/* unmapped units stay re-readable */
}

/*
 * The single geometry entry point on the command path: a cache hit costs no
 * mailbox traffic whatsoever.  A miss can only be a unit that had no drive
 * mapped when the cache was filled, and it then costs exactly what the pre-cache
 * driver always spent -- once per command, not once per fact.
 */
static struct z3660_unitgeom *
z3660_geom_get( unit)
int	unit;
{
	if (unit < 0 || unit >= Z3660_NUNITS)
		return &z3660_geom_absent;
	if (z3660_geom[unit].valid == 0)
		z3660_geom_fill( unit);
	return &z3660_geom[unit];
}

/*
 * Run one block READ or WRITE through the piscsi mailbox, chunked to <=64KB.
 * block = starting LBA, blocks = block count, bs = block size, data = buffer.
 */
static void
z3660_rw( unit, write, block, blocks, bs, data)
int	unit, write;
ulong	block, blocks, bs;
uchar	*data;
{
	ulong	chunk, len, i, perchunk, pa;

	perchunk = MAXXFER / bs;
	if (perchunk == 0)
		perchunk = 1;

	while (blocks > 0) {
		chunk = (blocks < perchunk) ? blocks : perchunk;
		len   = chunk * bs;
		pa    = (ulong)data;

		/*
		 * S11 census (docs/BLIZZARD-F3.md 3.4).  cp->addr is ONE vtop(),
		 * valid for exactly one page, and the firmware transfers len bytes
		 * LINEARLY from it -- while kmem is virtually contiguous and
		 * PHYSICALLY SCATTERED, so a chunk running past the page end writes
		 * an unrelated frame.  That is the 2026-07-13 cdfs wild write, and
		 * amix-cdfs refuses it outright (amix_kern_media.c:196-201).
		 *
		 * This round COUNTS and does NOT refuse, deliberately.  The raw path
		 * is broken up by amiga_dma_pageio() and the belief is that every
		 * request is page-bounded -- but that belief has never been
		 * instrumented, and this driver is the root device of two kernel
		 * lines.  Refusing on an untested belief risks more than it buys.
		 * The counter decides, in either direction: 0 across a boot and an
		 * install makes the guard a permanent refusal; non-zero means the
		 * shipping path needs the chunk SPLIT at the page boundary, not
		 * refused.
		 */
		if ((pa & (ulong)(NBPP - 1)) + len > (ulong)NBPP)
			z3660_pagecross_n++;

		if (write) {
			/*
			 * TO_DEVICE prepare.  Before the staging bcopy as well as
			 * before the doorbell: that bcopy READS `data` through the
			 * cache-inhibited alias, so it would stage stale RAM if a
			 * dirty alias line held newer bytes.
			 */
			z3660_push( pa, len);
			if (pa < BOUNCE_THRESH) {
				z3660_bounce_wr_n++;	/* unreachable on AMIX RAM */
				bcopy( (caddr_t)data, (caddr_t)bounce, (int)len);
			}
			WRLONG( P_WRITE_ADDR1, block);
			WRLONG( P_WRITE_ADDR2, len);
			WRLONG( P_WRITE_ADDR3, pa);
			WRLONG( P_WRITE, unit);			/* trigger (sync) */
			/* no completion op: TO_DEVICE never invalidates */
		} else {
			/*
			 * FROM_DEVICE prepare: no dirty line may survive to be
			 * evicted over the fresh bytes afterwards, and no stale
			 * valid line may survive to be read instead of them.
			 */
			z3660_push( pa, len);
			WRLONG( P_READ_ADDR1, block);
			WRLONG( P_READ_ADDR2, len);
			WRLONG( P_READ_ADDR3, pa);
			WRLONG( P_READ, unit);			/* trigger (sync) */
			z3660_sync();		/* S4: doorbell must precede the readback */
			z3660_dma = RDLONG( P_USED_DMA);
			if (z3660_dma != 0) {
				/*
				 * BOUNCE arm -- the CPU writes `data`, through the
				 * CI alias, and the prepare above left no cached
				 * line covering it, so it is already coherent.
				 * MUST NOT invalidate here: a 16-byte-rounded cinvl
				 * over a CPU-written range would discard the bytes
				 * just written along with their line neighbours.
				 */
				z3660_bounce_rd_n++;
				bcopy( (caddr_t)bounce, (caddr_t)data, (int)len);
			} else {
				/* DIRECT arm -- the ARM wrote RAM behind the CPU's
				 * back; invalidate before any consumer reads it. */
				z3660_inv( pa, len);
			}
		}

		block  += chunk;
		blocks -= chunk;
		data   += len;
	}
}

/*
 * Completion worker: run cp->intr(cp).  Invoked by z3660_complete()'s drain
 * loop (below), NOT inline from the sdqueue/ddstrategy call chain and NOT from a
 * clock callout.  The piscsi op itself is synchronous; the only reason
 * completion is not a bare inline (*cp->intr)(cp) is that dd.c's disk completion
 * re-issues the next I/O (ihandle -> startio -> sdqueue -> z3660queue), which
 * would recurse one kernel-stack frame per chunk (stack death on the first big
 * multi-chunk burst -- observed on real HW ~100 I/Os into boot).  z3660_complete()
 * flattens that recursion; see its comment for the full lifetime rationale (why a
 * timeout()-deferred cp is fatal on the cdfs CD-read path).
 */
static void
z3660done( cp)
struct sdcom	*cp;
{
	(*cp->intr)( cp);
}

/*
 * Synchronous, iterative completion delivery.
 *
 * The piscsi op is synchronous, so the request is finished by the time
 * z3660queue() gets here -- the completion MUST be delivered while the caller's
 * context is still current, because the caller's `cp` (sdcom) may live on the
 * caller's PER-PROCESS kernel stack.  It does on the cdfs CD-read path: the
 * in-kernel media backend (../amix-cdfs platform/amix-kernel/amix_kern_media.c,
 * amix_kern_submit) declares `amix_kern_req_t req;` on the stack, hands sdqueue
 * `&req.sc`, and blocks in sleep() until the completion fires.  sdqueue() is a
 * bare pass-through (scsi.c: (*queue[card].f)(c, cp)), so that stack address is
 * exactly the `cp` reaching us.
 *
 * The old code deferred completion to a clock (IPL4) callout --
 * timeout(z3660done, cp, 1) -- purely to break dd.c's completion->re-issue
 * recursion.  That is safe ONLY for a persistent cp: the disk path passes
 * &dp->com (a global in ddtab[][], mapped identically in every context) and
 * gsioctl a static sdcom.  For the cdfs stack cp it is fatal: the callout fires
 * ~1 tick later from whatever process is current (the caller is asleep, usually
 * switched out), and a per-process kernel-stack VA (~0x40001xxx, the u-area
 * window) then resolves into a DIFFERENT process's stack -- so z3660done reads a
 * garbage cp->intr (run-to-run "variable garbage", varying pid) and jumps
 * through it.  That is the T2.P3 mount panic: the corrupting completion is always
 * a CD READ(10) (lastcmd=0x28) with cp on the stack, while every disk completion
 * (persistent cp=0x80FDxxx) stays healthy.  depth==0/hits==0 -- not reentrancy,
 * not a DMA scribble: a pointer-lifetime bug in the deferred completion.
 *
 * Fix: deliver synchronously here, before z3660queue() returns, so cp is always
 * dereferenced in the context that owns it.  To keep dd.c's disk burst from
 * recursing (the sole reason timeout() ever existed), a small driver-owned FIFO
 * plus a `completing` guard flattens it: the first completion runs the drain
 * loop; a completion re-issued from inside an intr() (ihandle -> startio ->
 * sdqueue -> z3660queue -> here) merely appends to the FIFO and returns, and the
 * loop picks it up.  So the multi-chunk burst is delivered iteratively at a
 * fixed, small stack depth -- never one frame per I/O.  spl6 brackets only the
 * O(1) FIFO bookkeeping (serializing it against a clock-callout-driven re-issue);
 * the intr() call itself runs at the caller's IPL, exactly as before.
 */
#define	Z3660_CQ	32		/* completion FIFO depth (power of 2)          */
static struct sdcom	*z3660_cq[Z3660_CQ];
static uint		z3660_cq_head, z3660_cq_tail;
static int		z3660_completing;
ulong			z3660_cq_overflow;	/* kmem-readable: FIFO overrun count   */

static void
z3660_complete( cp)
struct sdcom	*cp;
{
	struct sdcom	*p;
	uint		nt;
	int		s;

	s = spl6();			/* serialize the FIFO vs a callout-driven re-issue */
	nt = (z3660_cq_tail + 1) & (Z3660_CQ - 1);
	if (nt == z3660_cq_head) {
		z3660_cq_overflow++;	/* full -- must never happen (see Z3660_CQ sizing) */
		splx( s);
		return;
	}
	z3660_cq[z3660_cq_tail] = cp;
	z3660_cq_tail = nt;
	if (z3660_completing) {		/* an outer drain loop already owns delivery */
		splx( s);
		return;
	}
	z3660_completing = 1;
	while (z3660_cq_head != z3660_cq_tail) {
		p = z3660_cq[z3660_cq_head];
		z3660_cq_head = (z3660_cq_head + 1) & (Z3660_CQ - 1);
		splx( s);		/* run the completion at the caller's IPL */
		z3660done( p);		/* (*p->intr)(p); may re-enter here */
		s = spl6();
	}
	z3660_completing = 0;
	splx( s);
}

/*
 * Generic SCSI queue entry -- mirrors a4091queue()/a3091queue().  Interprets the
 * CDB from cp->cdb (the piscsi firmware only does block I/O + geometry, so the
 * non-data commands are synthesised here, as in z3660_scsi.c:piscsi_scsi()).
 */
bool
z3660queue( c, cp)
int		c;
struct sdcom	*cp;
{
	int	e, i, unit, write, s;
	ulong	block, blocks, bs, nb, pdt;
	uchar	*data;
	uchar	op;
	struct z3660_unitgeom	*g;

	/*
	 * Bracket the ENTIRE transaction -- register setup, the synchronous
	 * command trigger, and every status readout -- at spl6.  This is the one
	 * spl in the driver; without it a clock tick mid-transaction dispatches a
	 * nested z3660queue() that scrambles the shared mailbox registers of the
	 * in-flight op (see NOTES.md "callout-IPL").  z3660map() and the geometry/
	 * rw helpers all run inside this bracket.
	 */
	s = z3660_enter();

	if (e = z3660map()) {
		cp->status = 0xff; cp->okay = FALSE;
		z3660_leave( s);
		z3660_complete( cp);		/* deliver OUTSIDE the mailbox bracket */
		return TRUE;
	}

	unit = (int)cp->unit;
	data = (uchar *)cp->addr;
	op   = cp->cdb[0];
	z3660_lastcmd = op;

	/*
	 * Select the unit for this command -- the ONE surviving preamble round
	 * trip.  Block size, block count and device type then come from the
	 * per-unit cache filled at attach (z3660_geom_get(), no mailbox traffic on
	 * a hit); before that cache existed these three facts cost four further
	 * round trips on every single CDB.
	 */
	WRLONG( P_DRVNUMX, unit);
	g   = z3660_geom_get( unit);
	bs  = g->bs;
	pdt = g->pdt;			/* 0x00 disk, 0x05 CD-ROM (sole device-type source) */

	cp->status = 0;			/* default GOOD */
	cp->okay   = TRUE;
	write      = 0;

	/*
	 * S8 -- the synthesised responses below (REQUEST SENSE 18, INQUIRY 36,
	 * READ CAPACITY 8, MODE SENSE(6) 12, MODE SENSE(10) 16 bytes) are written
	 * into the caller's buffer by the CPU, through the cache-inhibited low
	 * alias, and read back by the consumer through whatever alias it owns.
	 * Push-and-invalidate FIRST, so any dirty line covering the range reaches
	 * RAM (preserving the neighbours that share these sub-cache-line ranges)
	 * and no stale line survives to be served instead.  Never invalidate AFTER
	 * a CPU-written range: see docs/BLIZZARD-F3.md 3.2.
	 *
	 * Block READ/WRITE are absent from this list on purpose -- their
	 * maintenance is per chunk, at the DMA boundary inside z3660_rw().
	 */
	switch (op) {
	case C_REQ_SENSE:
	case C_INQUIRY:
	case C_READ_CAP10:
	case C_MODE_SENSE6:
	case C_MODE_SENSE10:
		if (data && cp->nbyte)
			z3660_push( (ulong)data, (ulong)cp->nbyte);
		break;
	default:
		break;
	}

	switch (op) {
	case C_TUR:
	case C_START_STOP:
		break;

	case C_REQ_SENSE:
		if (data) {
			for (i = 0; i < (int)cp->nbyte && i < 18; ++i)
				data[i] = 0;		/* default: NO SENSE */
			/*
			 * Return a pending condition (a CD-ROM WRITE reject) in
			 * fixed format; dd.c reads data[2] (key) and data[12]
			 * (ASC).  Consumed on read.  With nothing pending this
			 * leaves the historical all-zeros disk response intact.
			 */
			if (z3660_sense_key && cp->nbyte >= 14) {
				data[0] = 0x70;		/* current error, fixed fmt */
				data[2] = z3660_sense_key;
				data[7] = 0x0A;		/* additional sense length  */
				data[12] = z3660_sense_asc;
				data[13] = z3660_sense_ascq;
			}
			z3660_sense_key = z3660_sense_asc = z3660_sense_ascq = 0;
		}
		break;

	case C_INQUIRY:
		if (data) {
			for (i = 0; i < (int)cp->nbyte; ++i) data[i] = 0;
			if (pdt == 0x05) {
				/*
				 * CD-ROM -- byte-match the a3000_scsi Path B
				 * mirror: 05 80 02 02 1F 00 00 00, vendor
				 * "Z3660   ", product "AMIX CD-ROM     ", rev
				 * "0.1 " (36-byte SCSI-2 INQUIRY).
				 */
				if (cp->nbyte >= 5) {
					data[0] = 0x05;		/* CD-ROM device type     */
					data[1] = 0x80;		/* RMB = removable medium */
					data[2] = 0x02;		/* SCSI-2                 */
					data[3] = 0x02;		/* response data format 2 */
					data[4] = 0x1F;		/* additional length (36) */
				}
				{ static char cdid[] = "Z3660   AMIX CD-ROM     0.1 ";
				  for (i = 0; i < 28 && (8 + i) < (int)cp->nbyte; ++i)
					data[8 + i] = cdid[i]; }
			} else {
				/* direct-access disk -- unchanged (validated path) */
				if (cp->nbyte >= 5) {
					data[0] = 0x00;		/* direct-access device   */
					data[1] = 0x00;		/* fixed (not removable)  */
					data[2] = 0x02;		/* SCSI-2                 */
					data[3] = 0x02;		/* response data format 2 */
					data[4] = 40 - 4;	/* additional length      */
				}
				{ static char id[] = "Z3660   PiSCSI Disk      0.1 ";
				  for (i = 0; i < 28 && (8 + i) < (int)cp->nbyte; ++i)
					data[8 + i] = id[i]; }
			}
		}
		break;

	case C_READ_CAP10:
		if (data && cp->nbyte >= 8) {
			ulong	lba;
			blocks = g->nblocks;
			lba = blocks - 1;			/* returned LBA = last block */
			data[0] = (lba >> 24); data[1] = (lba >> 16);
			data[2] = (lba >> 8);  data[3] = lba;
			data[4] = (bs >> 24);  data[5] = (bs >> 16);
			data[6] = (bs >> 8);   data[7] = bs;
		}
		break;

	case C_MODE_SENSE6:
		if (data && cp->nbyte >= 12) {
			ulong	nbk;
			for (i = 0; i < (int)cp->nbyte; ++i) data[i] = 0;
			blocks  = g->nblocks;
			nbk     = (blocks - 1) & 0xFFFFFF;	/* 24-bit block count  */
			data[0] = 3 + 8;			/* mode data length        */
			if (pdt == 0x05)
				data[2] = 0x80;			/* CD-ROM: write-protected (WP) */
			data[3] = 8;				/* block descriptor length */
			data[5] = (nbk >> 16); data[6] = (nbk >> 8); data[7] = nbk;
			data[9] = (bs >> 16);  data[10] = (bs >> 8); data[11] = bs;
		}
		break;

	case C_MODE_SENSE10:
		/*
		 * CD-ROM MODE SENSE(10).  The driver never had a 0x5A case, so a
		 * CD MODE SENSE(10) used to fall to the default hard error; cdfs
		 * never issues it, but the a3000_scsi Path B mirror answers it,
		 * so match that oracle: 8-byte header + 8-byte block descriptor,
		 * no rigid-geometry pages, WP set.  A disk (pdt != 0x05) is left
		 * to the SAME hard error as before -- the disk path is untouched.
		 */
		if (pdt == 0x05 && data && cp->nbyte >= 16) {
			ulong	nbk;
			for (i = 0; i < (int)cp->nbyte; ++i) data[i] = 0;
			blocks  = g->nblocks;
			nbk     = (blocks - 1) & 0xFFFFFF;	/* 24-bit block count  */
			data[0] = 0; data[1] = 8 + 8 - 2;	/* mode data length 0x000E */
			data[3] = 0x80;				/* CD-ROM: write-protected (WP) */
			data[7] = 8;				/* block descriptor length */
			data[9]  = (nbk >> 16); data[10] = (nbk >> 8); data[11] = nbk;
			data[13] = (bs >> 16);  data[14] = (bs >> 8);  data[15] = bs;
			break;
		}
		cp->status = 0xff; cp->okay = FALSE;	/* disk / short buf: as before */
		break;

	case C_WRITE_6:
		write = 1;
		/* fall through */
	case C_READ_6:
		block  = ((ulong)(cp->cdb[1] & 0x1f) << 16)
		       | ((ulong)cp->cdb[2] << 8) | cp->cdb[3];
		blocks = cp->cdb[4];
		if (blocks == 0) blocks = 256;
		goto rw;

	case C_WRITE_10:
		write = 1;
		/* fall through */
	case C_READ_10:
		block  = ((ulong)cp->cdb[2] << 24) | ((ulong)cp->cdb[3] << 16)
		       | ((ulong)cp->cdb[4] << 8)  | cp->cdb[5];
		blocks = ((ulong)cp->cdb[7] << 8)  | cp->cdb[8];
	rw:
		z3660_lastblock = block;
		z3660_lastlen   = blocks * bs;
		/*
		 * CD-ROM is read-only: reject WRITE(6)/WRITE(10) with a
		 * CHECK CONDITION / DATA PROTECT sense, exactly as the
		 * a3000_scsi Path B mirror does -- never touch the medium.
		 * okay = TRUE + a non-zero SCSI status is the framework's
		 * "command completed, CHECK CONDITION" signal (see a3091.c);
		 * dd.c then issues REQUEST SENSE and reads DATA PROTECT.
		 */
		if (write && pdt == 0x05) {
			z3660_sense_key  = 0x07;	/* DATA PROTECT    */
			z3660_sense_asc  = 0x27;	/* write protected */
			z3660_sense_ascq = 0x00;
			cp->status = 0x02;		/* CHECK CONDITION */
			cp->okay   = TRUE;
			break;
		}
		nb = g->nblocks;
		z3660_blocks0 = nb;
		/* nb == 0 means the firmware has no drive mapped at this unit --
		 * it would silently no-op the I/O and we must NOT report GOOD. */
		if (blocks == 0 || nb == 0 || (block + blocks) > nb) {
			cp->status = 0xff; cp->okay = FALSE;
			break;
		}
		z3660_rw( unit, write, block, blocks, bs, data);
		break;

	default:
		cp->status = 0xff; cp->okay = FALSE;
		break;
	}

	z3660_rc = cp->okay ? 0 : 0xff;
	z3660_leave( s);
	z3660_complete( cp);			/* deliver OUTSIDE the mailbox bracket */
	return TRUE;
}

void
z3660intr()
{
}

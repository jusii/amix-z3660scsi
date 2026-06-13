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
 * Mapping uses sptalloc() exactly like the A4091 (TT-gap safe).
 *
 * Protocol (board_base-relative, all 32-bit MMIO):
 *   register window  = board_base + 0x2000  (commands written/read as longs at
 *                      regs + cmd_offset)
 *   bounce buffer    = board_base + 0x80000 (<=64KB, for transfers the ARM cannot
 *                      DMA directly -- which in EMU/030 mode is ALWAYS, and Amix RAM
 *                      is < 0x08000000 so the firmware always bounces)
 *
 *   A block READ/WRITE is:  write DRVNUMX(unit); write *_ADDR1(block number),
 *   *_ADDR2(byte length), *_ADDR3(buffer phys addr); then write the command
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
#include	"sys/param.h"		/* USIZE etc. for proc.h */
#include	"sys/immu.h"		/* PG_V, phystopfn, paddr_t */
#include	"sys/errno.h"
#include	"sys/proc.h"		/* TRACE: sleep-channel dump */
#include	"rico.h"
#include	"sd.h"

#define	Z3660_PROD	0x144B0001	/* autocon pc = (manufacturer<<16)|product */
#define	Z3660_FIXED	0x10000000	/* combo window base when not autoconfigured */
#define	VPOSR		0xDFF004	/* Agnus/Alice id: bits 8-14 >= 0x22 -> AGA */
#define	PISCSI_OFFSET	0x00002000	/* register window within the board */
#define	BOUNCE_OFFSET	0x00080000	/* bounce buffer within the board */
#define	BOUNCE_PAGES	32		/* 64KB bounce; Amix NBPP is 2KB, not 4KB! */
#define	MAXXFER		65536		/* max bytes per piscsi op */
#define	BOUNCE_THRESH	0x08000000	/* buffers below this are bounced by the ARM */

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
#define	P_WRITE_ADDR1	0x240
#define	P_WRITE_ADDR2	0x244
#define	P_WRITE_ADDR3	0x248
#define	P_BLOCKSIZE0	0x200		/* + unit*4, units 0..7 */
#define	P_BLOCKS0	0x220		/* + unit*4, units 0..7 */

/* SCSI opcodes we interpret */
#define	C_TUR		0x00
#define	C_REQ_SENSE	0x03
#define	C_INQUIRY	0x12
#define	C_MODE_SENSE6	0x1A
#define	C_START_STOP	0x1B
#define	C_READ_CAP10	0x25
#define	C_READ_6	0x08
#define	C_WRITE_6	0x0A
#define	C_READ_10	0x28
#define	C_WRITE_10	0x2A

extern int	autocon();
extern caddr_t	sptalloc();
extern void	bcopy();
extern int	printf();
extern int	timeout();

static volatile uchar	*regs;		/* board+0x2000 register window  */
static volatile uchar	*bounce;	/* board+0x80000 bounce buffer   */
static long		board_phys;
static void		z3660beat();

#define	WRLONG(cmd,val)	(*(volatile ulong *)(regs + (cmd)) = (ulong)(val))
#define	RDLONG(cmd)	(*(volatile ulong *)(regs + (cmd)))

/* last-transaction diagnostics (read via /dev/mem or a probe tool) */
ulong	z3660_lastblock, z3660_lastlen, z3660_blocks0, z3660_dma;
uchar	z3660_rc, z3660_lastcmd, z3660_present;

/*
 * Map the register window and the bounce buffer into kernel VA and verify the
 * mailbox answers.  0 on success; ENXIO when no Z3660 is present.
 *
 * Detection is multi-method (hydra-style): bootinfo's autocon table first;
 * when that misses (table unreliable on 2.1, or the board is outside the
 * autoconfig chain at the fixed base) probe Z3660_FIXED directly -- but only
 * on AGA, so the ECS build box (no Z3660, open bus at 0x10000000) never goes
 * there.  Breadcrumbs: a write to the read-only P_BLOCKS register makes the
 * ARM print "WARN: Write to read only register" with the value -- a free
 * 68k->serial debug channel on real hardware, a no-op everywhere else.
 */
#define	BREADCRUMB(v)	WRLONG( P_BLOCKS, (ulong)(v))

static int
z3660map()
{
	long	base, size;
	ulong	t;

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
	regs   = (volatile uchar *)sptalloc( 1, PG_V,
			phystopfn( (paddr_t)base + PISCSI_OFFSET), 0);
	bounce = (volatile uchar *)sptalloc( BOUNCE_PAGES, PG_V,
			phystopfn( (paddr_t)base + BOUNCE_OFFSET), 0);
	if (regs == 0 || bounce == 0) {
		regs = 0;
		return ENOMEM;
	}
	BREADCRUMB( 0xB00B0001);		/* mapped; about to probe */
	BREADCRUMB( board_phys);
	WRLONG( P_DRVNUMX, 6);
	t = RDLONG( P_DRVTYPE);			/* firmware: 0 or 1, nothing else */
	BREADCRUMB( 0xB00B0002);
	BREADCRUMB( t);
	if (t > 1) {
		regs = 0;			/* open bus / not a piscsi window */
		z3660_present = 0;
		return ENXIO;
	}
	z3660_present = 1;
	timeout( z3660beat, (caddr_t)0, 100);	/* start liveness heartbeat */
	return 0;
}

/*
 * Cross-file breadcrumb + liveness heartbeat (TRACE build).  z3660_crumb is
 * called from dd.c/physdsk.c instrumentation; a no-op until the board maps
 * (and forever on the build box).  The heartbeat proves clock/timeout
 * machinery is still alive after a freeze.
 */
void
z3660_crumb( v)
ulong	v;
{
	if (regs)
		BREADCRUMB( v);
}

static ulong	z3660_beatn;

extern struct proc	*practive;	/* active process chain (SVR4) */

static void
z3660beat()
{
	z3660_crumb( 0xB00BBEA7);
	z3660_crumb( ++z3660_beatn);
	if ((z3660_beatn % 2) == 0) {		/* every ~4s: all procs, stat+wchan */
		struct proc	*p;
		for (p = practive; p; p = p->p_next) {
			z3660_crumb( 0xB00BC000 | ((ulong)p->p_pid & 0xFFF));
			z3660_crumb( ((ulong)p->p_stat << 28)
				   | ((ulong)p->p_wchan & 0x0FFFFFFF));
			if (p->p_stat == SZOMB) {	/* why did it die? */
				z3660_crumb( 0xB00BDEAD);
				z3660_crumb( ((ulong)p->p_wcode << 24)
					   | ((ulong)p->p_wdata & 0xFFFFFF));
			}
		}
	}
	timeout( z3660beat, (caddr_t)0, 100);
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
	if (z3660map())
		return 0;
	*ap = (char *)board_phys;
	return 1;
}

static ulong
z3660_blocksize( unit)
int	unit;
{
	ulong	bs;

	if (unit < 0 || unit > 7)
		return 512;
	bs = RDLONG( P_BLOCKSIZE0 + unit * 4);
	return (bs == 0) ? 512 : bs;
}

static ulong
z3660_nblocks( unit)
int	unit;
{
	if (unit < 0 || unit > 7)
		return 0;
	return RDLONG( P_BLOCKS0 + unit * 4);
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
	ulong	chunk, len, i, perchunk;

	perchunk = MAXXFER / bs;
	if (perchunk == 0)
		perchunk = 1;

	while (blocks > 0) {
		chunk = (blocks < perchunk) ? blocks : perchunk;
		len   = chunk * bs;

		if (write) {
			if ((ulong)data < BOUNCE_THRESH)
				bcopy( (caddr_t)data, (caddr_t)bounce, (int)len);
			WRLONG( P_WRITE_ADDR1, block);
			WRLONG( P_WRITE_ADDR2, len);
			WRLONG( P_WRITE_ADDR3, (ulong)data);
			WRLONG( P_WRITE, unit);			/* trigger (sync) */
		} else {
			WRLONG( P_READ_ADDR1, block);
			WRLONG( P_READ_ADDR2, len);
			WRLONG( P_READ_ADDR3, (ulong)data);
			WRLONG( P_READ, unit);			/* trigger (sync) */
			z3660_dma = RDLONG( P_USED_DMA);
			if (z3660_dma != 0)
				bcopy( (caddr_t)bounce, (caddr_t)data, (int)len);
		}

		block  += chunk;
		blocks -= chunk;
		data   += len;
	}
}

/*
 * Deferred completion: the piscsi op itself is synchronous, but calling
 * cp->intr inline from inside the sdqueue/ddstrategy call chain recurses
 * dd.c's ihandle->iodone->b_iodone->ddstrategy loop one kernel stack frame
 * per chunk (stack death on the first big multi-chunk burst -- observed on
 * real HW at the cylinder-group write burst ~100 I/Os into boot).  Complete
 * from clock context via timeout() instead, like a real interrupt HBA.
 */
/*
 * PFLUSHA executed from a data array (no inline asm under the K&R cc).
 * EXPERIMENT (real-HW): the EMU core's targeted PFLUSH after PTE updates may
 * be ineffective (stale ATC -> freshly paged-in text executes as garbage ->
 * init dies SIGILL at _rt_boot).  Flushing the whole ATC after every read
 * completion papers over that class of bug; if boot proceeds, the stale-ATC
 * diagnosis is confirmed.  Caches are not emulated in UAE_030_MMU mode, so
 * executing from .data is safe here.
 */
static ushort	z3660_pflusha_ops[] = { 0xF000, 0x2400, 0x4E75 }; /* pflusha; rts */

static void
z3660done( cp)
struct sdcom	*cp;
{
	BREADCRUMB( 0xB00BD0E0 | (cp->okay ? 1 : 0));
	if (cp->reading)
		(* (void (*)()) z3660_pflusha_ops)();
	(*cp->intr)( cp);
	BREADCRUMB( 0xB00BD0EE);
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
	int	e, i, unit, write;
	ulong	block, blocks, bs, nb;
	uchar	*data;
	uchar	op;
	static int	firstq;

	if (e = z3660map()) {
		cp->status = 0xff; cp->okay = FALSE;
		timeout( z3660done, (caddr_t)cp, 1);
		return TRUE;
	}
	if (firstq == 0) {
		firstq = 1;
		BREADCRUMB( 0xB00B0003);	/* first command reached the driver */
		BREADCRUMB( cp->cdb[0]);
	}
	/* TRACE build: every command, op in low byte */
	BREADCRUMB( 0xB00B0F00 | cp->cdb[0]);

	unit = (int)cp->unit;
	data = (uchar *)cp->addr;
	op   = cp->cdb[0];
	z3660_lastcmd = op;

	WRLONG( P_DRVNUMX, unit);
	bs = z3660_blocksize( unit);

	cp->status = 0;			/* default GOOD */
	cp->okay   = TRUE;
	write      = 0;

	switch (op) {
	case C_TUR:
	case C_START_STOP:
		break;

	case C_REQ_SENSE:
		if (data)
			for (i = 0; i < (int)cp->nbyte && i < 18; ++i)
				data[i] = 0;		/* NO SENSE */
		break;

	case C_INQUIRY:
		if (data) {
			for (i = 0; i < (int)cp->nbyte; ++i) data[i] = 0;
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
		break;

	case C_READ_CAP10:
		if (data && cp->nbyte >= 8) {
			ulong	lba;
			blocks = z3660_nblocks( unit);
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
			blocks  = z3660_nblocks( unit);
			nbk     = (blocks - 1) & 0xFFFFFF;	/* 24-bit block count  */
			data[0] = 3 + 8;			/* mode data length        */
			data[3] = 8;				/* block descriptor length */
			data[5] = (nbk >> 16); data[6] = (nbk >> 8); data[7] = nbk;
			data[9] = (bs >> 16);  data[10] = (bs >> 8); data[11] = bs;
		}
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
		nb = z3660_nblocks( unit);
		z3660_blocks0 = nb;
		/* nb == 0 means the firmware has no drive mapped at this unit --
		 * it would silently no-op the I/O and we must NOT report GOOD. */
		if (blocks == 0 || nb == 0 || (block + blocks) > nb) {
			BREADCRUMB( 0xB00B0BAD);	/* TRACE: rejected */
			BREADCRUMB( block);
			BREADCRUMB( blocks);
			BREADCRUMB( nb);
			cp->status = 0xff; cp->okay = FALSE;
			break;
		}
		BREADCRUMB( 0xB00B0010 | (ulong)write);	/* TRACE: rw issue */
		BREADCRUMB( block);
		BREADCRUMB( blocks);
		z3660_rw( unit, write, block, blocks, bs, data);
		if (write == 0 && data) {
			BREADCRUMB( 0xB00B0011);	/* TRACE: read result */
			BREADCRUMB( *(ulong *)data);	/* first long received */
			BREADCRUMB( z3660_dma);		/* USED_DMA seen */
		}
		break;

	default:
		cp->status = 0xff; cp->okay = FALSE;
		break;
	}

	z3660_rc = cp->okay ? 0 : 0xff;
	timeout( z3660done, (caddr_t)cp, 1);
	return TRUE;
}

void
z3660intr()
{
}

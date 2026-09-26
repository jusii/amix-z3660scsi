/*
 * mock_piscsi.c -- host-side mock of the Z3660 "piscsi" mailbox.
 *
 * This models exactly the registers src/z3660.c touches, with semantics kept in
 * LOCKSTEP with the firmware (the sole owner of the protocol):
 *
 *   Z3660/z3660-firmware/Z-TURN/vitis_ide/Z3660/src/scsi/scsi.c
 *   Z3660/.../Z3660/src/scsi/z3660_scsi_enums.h
 *
 * and mirrored by the emulator SCSI target a3000_scsi.cpp.  The offsets below
 * are a local copy of the enum values (the firmware header is NOT included, the
 * same discipline a3000_scsi.cpp follows) -- if the firmware ever renumbers a
 * register, BOTH src/z3660.c AND this mirror must change together.
 *
 * Register contract (firmware file:line in parentheses):
 *
 *   write DRVNUMX (0x90)      cur_drive = val                  (scsi.c:1268)
 *   write DRVNUM  (0x08)      cur_drive = (val>16)?255:val     (scsi.c:1257)
 *   write READ_ADDR1/2/3      piscsi_u32_read[(a-0x20)/4]=val  (scsi.c:1241)
 *   write WRITE_ADDR1/2/3     piscsi_u32_write[(a-0x240)/4]=v  (scsi.c:1249)
 *   write READ  (0x04) = drv  read  block=rd[0] len=rd[1] to rd[2]  (scsi.c:1055)
 *   write WRITE (0x00) = drv  write block=wr[0] len=wr[1] from wr[2] (scsi.c:1146)
 *   read  DRVTYPE (0x0C)      0 if drive unattached else 1     (scsi.c:1626)
 *   read  PDT     (0xA0)      devs[cur_drive].pdt  (STATEFUL)  (scsi.c:1636)
 *   read  BLOCKSIZE0+4n       cur_drive=n; devs[n].block_size  (scsi.c:1723)
 *   read  BLOCKS0+4n          cur_drive=n; devs[n].nblocks     (scsi.c:1669)
 *   read  USED_DMA (0x9C)     used_dma, then cleared (consumed) (scsi.c:1767)
 *
 * DMA path.  On real Amix, main memory sits below the firmware's DMA window
 * (< 0x08000000), so every transfer BOUNCES through board+0x80000: the firmware
 * copies READ data into the bounce buffer and returns used_dma != 0, and the
 * DRIVER copies bounce->caller; for a WRITE the driver first copies
 * caller->bounce (its `data < 0x08000000` gate) and the firmware reads the
 * bounce.  We reproduce that faithfully for the READ path -- do_read() fills the
 * shared bounce buffer and sets used_dma, driver copies it out.
 *
 * For the WRITE path the driver's low-RAM bounce gate is `(ulong)data <
 * 0x08000000`; a host pointer is far above that, so the driver does NOT pre-fill
 * the bounce and instead hands us the buffer pointer directly (READ/WRITE_ADDR3)
 * -- the firmware's "mapped range" direct-DMA branch (scsi.c:1190).  do_write()
 * therefore reads straight from that pointer.  This asymmetry is not a mock
 * choice: it is exactly which of the driver's two branches a host-address-space
 * pointer selects, so the mock stays byte-faithful to the driver as compiled.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock_piscsi.h"

/* ---- piscsi register offsets (LOCKSTEP mirror of z3660_scsi_enums.h) ---- */
#define R_WRITE        0x00
#define R_READ         0x04
#define R_DRVNUM       0x08
#define R_DRVTYPE      0x0C
#define R_READ_ADDR1   0x20
#define R_READ_ADDR4   0x2C
#define R_DRVNUMX      0x90
#define R_USED_DMA     0x9C
#define R_PDT          0xA0
#define R_BLOCKSIZE0   0x200
#define R_BLOCKS0      0x220
#define R_WRITE_ADDR1  0x240
#define R_WRITE_ADDR4  0x24C

#define MOCK_UNITS   8
#define MOCK_BOUNCE  0x10000UL   /* 64 KB == driver MAXXFER (board+0x80000 win) */

/* unsigned long must be wide enough to carry a host pointer through the ADDR3
 * register slots (LP64).  Fails to compile on LLP64 (e.g. 64-bit Windows). */
typedef char mock_ulong_holds_ptr[sizeof(unsigned long) >= sizeof(void *) ? 1 : -1];

typedef struct {
	int            present;
	unsigned long  pdt;
	unsigned long  block_size;
	unsigned long  nblocks;
	unsigned char *backing;
	unsigned long  backing_len;
} mock_dev;

static mock_dev       devs[MOCK_UNITS];
static int            cur_drive;
static unsigned long  u32_read[4];    /* READ_ADDR1..4  = block, len, dst, hi  */
static unsigned long  u32_write[4];   /* WRITE_ADDR1..4 = block, len, src, hi  */
static unsigned long  used_dma;
static unsigned char *g_bounce;       /* board+0x80000 mirror; shared w/ driver */
static unsigned char  g_regs[64];     /* dummy non-NULL register window        */

/*
 * In-flight hook.  Fired from inside do_read()/do_write() -- the synchronous
 * window where the real ARM is servicing the command and the driver is holding
 * the mailbox registers.  The re-entry test registers a hook here to simulate a
 * clock callout landing mid-transaction; NULL (the default) means no test is
 * watching, so every existing test is unaffected.
 */
static void (*g_inflight_hook)();

void mock_set_inflight_hook( fn)
void	(*fn)();
{
	g_inflight_hook = fn;
}

/*
 * Round-trip counters (see mock_piscsi.h).  Every WRLONG/RDLONG below bumps
 * these, because on metal every one of them is a full cross-core handshake with
 * core1 spinning for its duration -- so this is a direct model of the driver's
 * per-CDB cost, not a mock artifact.
 */
unsigned long	mock_trips, mock_trips_rd, mock_trips_wr;
/*
 * Stock-firmware hazard counters (never reset by mock_trips_reset(); only
 * mock_reset() zeroes them).  mock_div0: reads of BLOCKS0+4n while that unit's
 * block_size is 0 -- the read on which STOCK firmware (no Z3660 c0510a7 guard)
 * computes fs / 0.  mock_doorbells: READ/WRITE command triggers per unit.
 */
unsigned long	mock_div0;
unsigned long	mock_doorbells[MOCK_UNITS];
unsigned long	mock_trips_drvnumx, mock_trips_blocksize;
unsigned long	mock_trips_blocks, mock_trips_pdt;

void mock_trips_reset()
{
	mock_trips = mock_trips_rd = mock_trips_wr = 0;
	mock_trips_drvnumx = mock_trips_blocksize = 0;
	mock_trips_blocks = mock_trips_pdt = 0;
}

/* -------- board seams the driver externs (resolved here) ---------------- */

/*
 * The driver maps two BOARD windows -- the 1-page register window at
 * board+0x2000 and the 64 KB firmware bounce staging area at board+0x80000.
 * Both stubs below resolve to g_regs and g_bounce respectively, so the driver's
 * `bounce` IS the buffer do_read()/do_write() move data through (the WRLONG/
 * RDLONG overrides ignore whatever `regs` ends up pointing at).
 *
 * phystokv(): the DIRECT arm, and the one the shipped metal config takes -- the
 * board base 0x10000000 is below VSECT1, inside AMIX's identity-mapped section
 * 0, so z3660map() just dereferences the physical address and calls nothing.
 * The host has no such identity map, so we translate the board-relative offset
 * into the matching mock buffer.  An unrecognised offset returns 0, which makes
 * z3660map() fail with ENOMEM rather than silently hand the driver the wrong
 * buffer -- so a future offset change surfaces as a test failure.
 */
#define MOCK_BOARD_BASE  0x10000000UL	/* what autocon() below reports */
#define MOCK_REGS_OFF    0x00002000UL	/* z3660.c PISCSI_OFFSET */
#define MOCK_BOUNCE_OFF  0x00080000UL	/* z3660.c BOUNCE_OFFSET */

unsigned long z3660_mock_phystokv( paddr)
unsigned long	paddr;
{
	unsigned long	off = paddr - MOCK_BOARD_BASE;

	if (off == MOCK_REGS_OFF)
		return (unsigned long)g_regs;
	if (off == MOCK_BOUNCE_OFF)
		return (unsigned long)g_bounce;
	return 0UL;
}

/*
 * sptalloc(): the arm kept for a board at or above VSECT1 (autoconfig_rtg YES).
 * Unreachable at the base autocon() reports, but resolved so the driver links
 * and so a base change re-exercises it: 1 page = the register window, more = the
 * bounce.
 */
char *sptalloc( npages, flags, pfn, dummy)
int		npages;
unsigned long	flags, pfn;
int		dummy;
{
	if (npages == 1)
		return (char *)g_regs;
	return (char *)g_bounce;
}

/*
 * autocon(): report the board found, at the shipped fixed base.  Returning
 * non-zero makes z3660map() take the happy path and skip the VPOSR ($DFF004)
 * hardware probe, which would segfault on the host.
 */
int autocon( prod, idx, basep, sizep)
long	prod;
int	idx;
long	*basep, *sizep;
{
	*basep = (long)MOCK_BOARD_BASE;
	*sizep = 0x00100000L;
	return 1;
}

/* -------- data movement triggers --------------------------------------- */

static void do_read( drive)
unsigned long	drive;
{
	mock_dev	*d;
	unsigned long	block, len, off, n;

	/* in-flight window: a clock callout may try to nest here (re-entry test).
	 * Fired BEFORE latching the mailbox registers so a nested transaction that
	 * is NOT blocked clobbers them and the interleave is observable. */
	if (g_inflight_hook)
		g_inflight_hook();

	used_dma = 0;
	if (drive >= MOCK_UNITS)
		return;
	d = &devs[drive];
	if (!d->present)
		return;			/* firmware: unmapped drive is a no-op */

	block = u32_read[0];		/* LBA (block index)      */
	len   = u32_read[1];		/* byte count             */
	off   = block * d->block_size;	/* firmware: f_lseek(src * block_size) */

	memset( g_bounce, 0, MOCK_BOUNCE);
	if (off < d->backing_len) {
		n = len;
		if (off + n > d->backing_len)
			n = d->backing_len - off;
		if (n > MOCK_BOUNCE)
			n = MOCK_BOUNCE;
		memcpy( g_bounce, d->backing + off, n);
	}
	used_dma = u32_read[2];		/* non-zero -> driver copies bounce->caller */
}

static void do_write( drive)
unsigned long	drive;
{
	mock_dev	*d;
	unsigned long	block, len, off, n;
	unsigned char	*src;

	if (g_inflight_hook)		/* in-flight window (see do_read) */
		g_inflight_hook();

	used_dma = 0;
	if (drive >= MOCK_UNITS)
		return;
	d = &devs[drive];
	if (!d->present)
		return;

	block = u32_write[0];
	len   = u32_write[1];
	src   = (unsigned char *)(unsigned long)u32_write[2];	/* caller buffer */
	off   = block * d->block_size;

	if (src && off < d->backing_len) {
		n = len;
		if (off + n > d->backing_len)
			n = d->backing_len - off;
		memcpy( d->backing + off, src, n);
	}
}

/* -------- the WRLONG / RDLONG seam (mock_regs.h) ------------------------ */

void z3660_mock_wrlong( cmd, val)
unsigned int	cmd;
unsigned long	val;
{
	mock_trips++;
	mock_trips_wr++;
	if (cmd == R_DRVNUMX)
		mock_trips_drvnumx++;
	if (cmd >= R_READ_ADDR1 && cmd <= R_READ_ADDR4) {
		u32_read[(cmd - R_READ_ADDR1) / 4] = val;
		return;
	}
	if (cmd >= R_WRITE_ADDR1 && cmd <= R_WRITE_ADDR4) {
		u32_write[(cmd - R_WRITE_ADDR1) / 4] = val;
		return;
	}
	switch (cmd) {
	case R_DRVNUMX:	cur_drive = (int)val;			break;
	case R_DRVNUM:	cur_drive = (val > 16) ? 255 : (int)val;break;
	case R_READ:
		if (val < MOCK_UNITS) mock_doorbells[val]++;
		do_read( val);					break;
	case R_WRITE:
		if (val < MOCK_UNITS) mock_doorbells[val]++;
		do_write( val);					break;
	default:						break;
	}
}

unsigned long z3660_mock_rdlong( cmd)
unsigned int	cmd;
{
	mock_trips++;
	mock_trips_rd++;
	if (cmd == R_PDT)
		mock_trips_pdt++;
	if (cmd >= R_BLOCKSIZE0 && cmd < R_BLOCKSIZE0 + MOCK_UNITS * 4) {
		mock_trips_blocksize++;
		cur_drive = (int)((cmd - R_BLOCKSIZE0) / 4);	/* firmware side effect */
		return devs[cur_drive].block_size;
	}
	if (cmd >= R_BLOCKS0 && cmd < R_BLOCKS0 + MOCK_UNITS * 4) {
		mock_trips_blocks++;
		cur_drive = (int)((cmd - R_BLOCKS0) / 4);	/* firmware side effect */
		/*
		 * STOCK firmware computes fs / block_size here unguarded; with
		 * block_size 0 that is the zero divide.  Count it, and return the
		 * test-chosen stand-in for the helper's quotient (nblocks: 0 for a
		 * never-mapped unit, 0xFFFFFFFF for a stale-fs one).
		 */
		if (devs[cur_drive].block_size == 0)
			mock_div0++;
		return devs[cur_drive].nblocks;
	}
	if (cmd >= R_READ_ADDR1 && cmd <= R_READ_ADDR4)
		return u32_read[(cmd - R_READ_ADDR1) / 4];
	if (cmd >= R_WRITE_ADDR1 && cmd <= R_WRITE_ADDR4)
		return u32_write[(cmd - R_WRITE_ADDR1) / 4];

	switch (cmd) {
	case R_DRVTYPE:
		if (cur_drive < 0 || cur_drive >= MOCK_UNITS)
			return 0;
		return devs[cur_drive].present ? 1 : 0;
	case R_PDT:
		if (cur_drive < 0 || cur_drive >= MOCK_UNITS)
			return 0;
		return devs[cur_drive].pdt;
	case R_DRVNUM:
		return (unsigned long)cur_drive;
	case R_USED_DMA: {
		unsigned long t = used_dma;
		used_dma = 0;			/* consumed on read (scsi.c:1771) */
		return t;
	}
	default:
		return 0;
	}
}

/* -------- test-facing control ------------------------------------------ */

void mock_reset()
{
	int	i;

	for (i = 0; i < MOCK_UNITS; i++) {
		if (devs[i].backing)
			free( devs[i].backing);
		memset( &devs[i], 0, sizeof devs[i]);
	}
	cur_drive = 0;
	memset( u32_read,  0, sizeof u32_read);
	memset( u32_write, 0, sizeof u32_write);
	used_dma = 0;
	g_inflight_hook = 0;
	mock_div0 = 0;
	memset( mock_doorbells, 0, sizeof mock_doorbells);
	mock_trips_reset();
	mock_spl_reset();
	if (!g_bounce)
		g_bounce = (unsigned char *)malloc( MOCK_BOUNCE);
	memset( g_bounce, 0, MOCK_BOUNCE);
}

void mock_add_drive( unit, pdt, block_size, nblocks, backing_len)
int		unit;
unsigned long	pdt, block_size, nblocks, backing_len;
{
	mock_dev	*d;
	unsigned long	i, blk;

	if (unit < 0 || unit >= MOCK_UNITS)
		return;
	d = &devs[unit];
	if (d->backing)
		free( d->backing);
	d->present     = 1;
	d->pdt         = pdt;
	d->block_size  = block_size;
	d->nblocks     = nblocks;
	d->backing_len = backing_len;
	d->backing     = (unsigned char *)malloc( backing_len);

	/* Position-dependent fill so a wrong offset yields wrong bytes... */
	for (i = 0; i < backing_len; i++)
		d->backing[i] = (unsigned char)
			((i ^ (i >> 3) ^ (i >> 11) ^ (unit * 0x5B)) & 0xFF);
	/* ...and stamp each whole block's first four bytes with its block index
	 * (big-endian), the marker the read tests check to prove block-index
	 * scaling instead of byte addressing. */
	if (block_size) {
		for (blk = 0; (blk + 1) * block_size <= backing_len; blk++) {
			unsigned char *p = d->backing + blk * block_size;
			p[0] = (unsigned char)(blk >> 24);
			p[1] = (unsigned char)(blk >> 16);
			p[2] = (unsigned char)(blk >> 8);
			p[3] = (unsigned char)(blk);
		}
	}
}

unsigned char *mock_backing( unit)
int	unit;
{
	if (unit < 0 || unit >= MOCK_UNITS)
		return 0;
	return devs[unit].backing;
}

unsigned long mock_backing_len( unit)
int	unit;
{
	if (unit < 0 || unit >= MOCK_UNITS)
		return 0;
	return devs[unit].backing_len;
}

/*
 * Set a unit's raw mailbox answers without a backing store: what BLOCKSIZE0+4n,
 * BLOCKS0+4n and PDT return for it.  Models a firmware unit table entry the
 * driver must judge (e.g. stock's block_size 0 / garbage quotient).  The unit
 * is NOT marked present, so a doorbell to it would find no backing.
 */
void mock_set_geom( unit, pdt, block_size, nblocks)
int		unit;
unsigned long	pdt, block_size, nblocks;
{
	mock_dev	*d;

	if (unit < 0 || unit >= MOCK_UNITS)
		return;
	d = &devs[unit];
	d->pdt        = pdt;
	d->block_size = block_size;
	d->nblocks    = nblocks;
}

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

/* -------- board seams the driver externs (resolved here) ---------------- */

/*
 * sptalloc(): the driver maps two windows -- the 1-page register window and the
 * BOUNCE_PAGES bounce buffer.  We hand back g_regs for the former (the WRLONG/
 * RDLONG overrides ignore the returned pointer) and g_bounce for the latter, so
 * the driver's `bounce` IS the buffer do_read()/do_write() move data through.
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
 * autocon(): report the board found, at a fixed base.  Returning non-zero makes
 * z3660map() take the happy path and skip the VPOSR ($DFF004) hardware probe,
 * which would segfault on the host.
 */
int autocon( prod, idx, basep, sizep)
long	prod;
int	idx;
long	*basep, *sizep;
{
	*basep = 0x10000000L;
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
	case R_READ:	do_read( val);				break;
	case R_WRITE:	do_write( val);				break;
	default:						break;
	}
}

unsigned long z3660_mock_rdlong( cmd)
unsigned int	cmd;
{
	if (cmd >= R_BLOCKSIZE0 && cmd < R_BLOCKSIZE0 + MOCK_UNITS * 4) {
		cur_drive = (int)((cmd - R_BLOCKSIZE0) / 4);	/* firmware side effect */
		return devs[cur_drive].block_size;
	}
	if (cmd >= R_BLOCKS0 && cmd < R_BLOCKS0 + MOCK_UNITS * 4) {
		cur_drive = (int)((cmd - R_BLOCKS0) / 4);	/* firmware side effect */
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

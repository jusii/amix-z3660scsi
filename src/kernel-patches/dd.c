

/*
 * SCSI disk driver.
 */

#include	"sys/types.h"
#include	"sys/param.h"
#include	"sys/buf.h"
#include	"sys/uio.h"
#include	"sys/inline.h"
#include	"sys/errno.h"
#include	"rico.h"
#include	"sd.h"

extern paddr_t vtop();

#define BSIZE 512


struct dd {
	uint		state;
	struct buf	*bhead,
			*btail;
	struct sdcom	com;
	uchar		sense[16];
};
/* dd.state
 */
#define	FIRST	0
#define	SENSE	1
#define	RETRY	2


static struct dd	ddtab[SDCARDS][SDUNITS];

void	ddstrategy( ),
	ddprint( );
static void
	breakup( ),
	startio( ),
	getsense( );


ddopen( devp, mode, type, cr)
dev_t *devp;
int mode;
int type;
struct cred *cr;
{
	int error;

	if (error = sdopen( sdcard( *devp)))
		return error;
	if (error = sdpartition( *devp, ddstrategy))
		return error;

	return 0;
}


ddclose(dev, mode, type, cr)
dev_t dev;
int mode;
int type;
struct cred *cr;
{

	return 0;
}


ddread(dev, uiop, cr)
dev_t dev;
struct uio *uiop;
struct cred *cr;
{

	if (uiop->uio_offset == sddevsize( dev)*BSIZE)
		return 0;
	if (uiop->uio_offset + uiop->uio_resid > sddevsize( dev)*BSIZE)
		uiop->uio_resid = sddevsize( dev)*BSIZE - uiop->uio_offset;
	return uiophysio( breakup, (struct buf *)0, dev, B_READ, uiop);
}


ddwrite(dev, uiop, cr)
dev_t dev;
struct uio *uiop;
struct cred *cr;
{

	if (uiop->uio_offset == sddevsize( dev)*BSIZE)
		return 0;
	if (uiop->uio_offset + uiop->uio_resid > sddevsize( dev)*BSIZE)
		uiop->uio_resid = sddevsize( dev)*BSIZE - uiop->uio_offset;
	return uiophysio( breakup, (struct buf *)0, dev, B_WRITE, uiop);
}


ddioctl(dev, cmd, arg, mode, cr, rvalp)
dev_t dev;
int cmd;
int arg;
int mode;
struct cred *cr;
int *rvalp;
{
	char *s;

	switch (cmd) {
#ifdef DIOC
	case DIOC:
		return 0;
#endif
#ifdef DIOCHARDWARENAME
	case DIOCHARDWARENAME:
		s = sdhardwarename( sdcard( dev));
		if (copyout( s, arg, strlen( s)+1))
			return EFAULT;
		return 0;
#endif
	default:
		/* Unknown ioctl */
		return EINVAL;
	}

	return 0;
}


static void
breakup( bp)
struct buf	*bp;
{

	amiga_dma_pageio( ddstrategy, bp);
}


void
ddstrategy( bp)
struct buf	*bp;
{
	struct dd	*dp;
	int		x;

	if (not sdvalid( bp))
		return;
	{ extern void z3660_crumb();
	  z3660_crumb( 0xB00BDD00);
	  z3660_crumb( bp->b_blkno);
	  z3660_crumb( bp->b_flags); }
	bp->av_forw = 0;
	bp->b_resid = 0;
	dp = &ddtab[sdcard( bp->b_edev)][sdunit( bp->b_edev)];
	x = sdspl( );
	if (dp->bhead) {
		dp->btail->av_forw = bp;
		dp->btail = bp;
	}
	else {
		dp->bhead = bp;
		dp->btail = bp;
		startio( FIRST, dp);
	}
	splx( x);
}


void
ddprint( dev, s)
dev_t	dev;
char	*s;
{

	printf( "%s: disk c%dd%ds%d: %s\n",
		sdhardwarename( sdcard( dev)),
		sdcard( dev)*8+sdunit( dev), 0, sdpart( dev), s);
}


ddsize( dev)
dev_t	dev;
{

	return sddevsize( dev);
}


static void
ihandle( cp)
struct sdcom	*cp;
{
	struct dd	*dp;
	struct buf	*bp;

	{ extern void z3660_crumb();
	  z3660_crumb( 0xB00BDD01);
	  z3660_crumb( cp->status); }
	dp = &ddtab[cp->card][cp->unit];
	bp = dp->bhead;
	if (not cp->okay)
		bp->b_flags |= B_ERROR;
	else
		switch (dp->state) {
		case FIRST:
			if (cp->status != 0) {
				getsense( dp);
				return;
			}
			break;
		case SENSE:
			if (cp->status == 0) {
				printf( "%s: disk c%dd%ds%d: ",
				        sdhardwarename( cp->card),
				        cp->card*8+cp->unit, 0, sdpart( bp->b_edev));
				printf( "blk%d 0x%x 0x%x\n", sdblkno( bp), dp->sense[2], dp->sense[12]);
				startio( RETRY, dp);
				return;
			}
			bp->b_flags |= B_ERROR;
			break;
		case RETRY:
			if (cp->status != 0)
				bp->b_flags |= B_ERROR;
		}
	dp->bhead = bp->av_forw;
	/*
	 * Issue the next queued buf BEFORE iodone(): iodone's b_iodone
	 * callback (pageio chunk resubmit) re-enters ddstrategy synchronously;
	 * with the old order it raced this trailing startio and double-issued
	 * the same &dp->com (self-linked sdcom, lost completions).  With the
	 * new order ddstrategy sees bhead non-empty and only enqueues.
	 */
	startio( FIRST, dp);
	iodone( bp);
	{ extern void z3660_crumb();
	  z3660_crumb( 0xB00BDD02); }
}


static void
startio( s, dp)
struct dd	*dp;
{
	struct buf	*bp;
	uint		bn;

	bp = dp->bhead;
	if (not bp)
		return;
	bn = sdblkno( bp);
	dp->com.card = sdcard( bp->b_edev);
	dp->com.unit = sdunit( bp->b_edev);
	dp->com.addr = (caddr_t)vtop(bp->b_un.b_addr, bp->b_proc);
	if (bp->b_flags & B_READ) {
		dp->com.reading = TRUE;
		dp->com.cdb[0] = 0x28;
	}
	else {
		dp->com.reading = FALSE;
		dp->com.cdb[0] = 0x2A;
	}
	dp->com.cdb[1] = 0;
	dp->com.cdb[2] = byte3( bn);
	dp->com.cdb[3] = byte2( bn);
	dp->com.cdb[4] = byte1( bn);
	dp->com.cdb[5] = byte0( bn);
	dp->com.cdb[6] = 0;
	dp->com.cdb[7] = byte1( bp->b_bcount/BSIZE);
	dp->com.cdb[8] = byte0( bp->b_bcount/BSIZE);
	dp->com.cdb[9] = 0;
	dp->com.nbyte = bp->b_bcount;
	dp->com.intr = ihandle;
	dp->state = s;
	sdqueue( &dp->com);
}


static void
getsense( dp)
struct dd	*dp;
{
	struct buf	*bp;

	bp = dp->bhead;
	dp->com.addr = vtop( dp->sense, (struct proc *)0);
	dp->com.reading = TRUE;
	dp->com.cdb[0] = 0x03;
	dp->com.cdb[1] = 0;
	dp->com.cdb[2] = 0;
	dp->com.cdb[3] = 0;
	dp->com.cdb[4] = sizeof dp->sense;
	dp->com.cdb[5] = 0;
	dp->com.nbyte = sizeof dp->sense;
	dp->state = SENSE;
	sdqueue( &dp->com);
}

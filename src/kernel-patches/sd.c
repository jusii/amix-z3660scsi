

/*
 * scsi interface selector
 *
 * All scsi device drivers call here to queue requests.  The call is routed
 * to the controller of the right type.
 */
#include	"sys/types.h"
#include	"sys/errno.h"
#include	"rico.h"
#include	"sd.h"


extern bool a3091queue(), a2090queue(), a2091queue(), a4091queue(), z3660queue();

static void
	init( ),
	insert( );

/*
 * Register your scsi controller cards here.
 */
static struct {
	uint	pn;				/* product number */
	bool	(*f)( );			/* queuing function */
	char	*hardwarename;
} scsicard[] = {
	0x0202F003, &a3091queue, "A3000 Internal SCSI",
	0x02020001, &a2090queue, "A2090 SCSI",
	0x02020003, &a2091queue, "A2091 SCSI",
	0x02020054, &a4091queue, "A4091 SCSI",
	0x144B0001, &z3660queue, "Z3660 SCSI",
};

struct queue {
	bool	(*f)( );
	uint	c;
	char	*a;
	char	*hardwarename;
};
static struct queue	queue[SDCARDS];


int
sdopen( card)
{

	init( );
	unless (card < nel( queue) && queue[card].f)
		return ENXIO;
	return 0;
}


void
sdqueue( cp)
struct sdcom	*cp;
{

	(*queue[cp->card].f)( queue[cp->card].c, cp);
}


char *
sdhardwarename( card)
uint	card;
{

	return queue[card].hardwarename;
}


static void
init( )
{
	char	*a,
		*o;
	uint	i;

	if (not queue->f)
		for (i=0; i<nel( scsicard); ++i) {
			int c = 0;
			while (autocon( scsicard[i].pn, c, &a, &o))
				insert( scsicard[i].f, c++, a, scsicard[i].hardwarename);
		}
}


static void
insert( f, c, a, n)
bool	(*f)( );
char	*a, *n;
{
	struct queue	*q;

	q = endof( queue) - 1;
	if (not q[0].f) {
		while ((q > queue)
		and (not q[-1].f || q[-1].a > a)) {
			q[0] = q[-1];
			--q;
		}
		q[0].f = f;
		q[0].c = c;
		q[0].a = a;
		q[0].hardwarename = n;
	}
	else
		printf( "sd: too many controllers\n");
}

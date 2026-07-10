#ifndef _HOSTTEST_SD_H
#define _HOSTTEST_SD_H
/*
 * struct sdcom, layout-compatible with usr/sys/amiga/alien/sd.h so z3660queue()
 * sees the exact kernel ABI (the field names/types/order are copied verbatim
 * from the golden build image's header).  rico.h must precede this include so
 * uchar / bool / uint are in scope; caddr_t comes from <sys/types.h>.
 */
#define SDCARDS 2
#define SDUNITS 8
#define sdspl   spl2

struct sdcom {
	volatile struct sdcom	*next;
	bool		reading,
			okay;
	uchar		status,
			cdb[12];
	caddr_t		addr;
	uint		nbyte,
			card,
			unit;
	void		(*intr)( );
};
#endif

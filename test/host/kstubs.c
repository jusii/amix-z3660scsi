/*
 * kstubs.c -- the generic SVR4 kernel services z3660.c externs, stubbed for the
 * host harness.  (The board-specific seams -- autocon()/sptalloc() and the
 * WRLONG/RDLONG mailbox -- live in mock_piscsi.c.)
 *
 * No <string.h>/<strings.h> here on purpose: glibc declares bcopy() with a
 * size_t length, but the driver calls it K&R-style with an int length and no
 * visible prototype, so the definition must take an int too (a size_t param
 * would read 64 bits where the caller passed 32).  bcopy is a plain byte loop
 * to sidestep the clash.
 */

/* SVR4 bcopy: source first, destination second (opposite of memcpy). */
void bcopy( src, dst, n)
char	*src, *dst;
int	n;
{
	int	i;
	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

/*
 * timeout(func, arg, ticks): the driver defers completion to clock context via
 * timeout(z3660done, cp, 1).  Fire it synchronously so (*cp->intr)(cp) runs
 * before z3660queue() returns -- the test reads the result straight afterwards.
 */
int timeout( func, arg, ticks)
void	(*func)();
char	*arg;
int	ticks;
{
	(*func)( arg);
	return 0;
}

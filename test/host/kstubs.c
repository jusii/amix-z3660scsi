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
#include "mock_piscsi.h"

/*
 * Mock spl (interrupt priority level).  The real amiga inline.h emits
 * `move.w #0x2400,%sr` for spl6 (IPL 4) and restores the SR word for splx; here
 * we model just the level.  spl6() raises the mock IPL to 4 and returns the
 * prior level as the restore token; splx() puts it back.  mock_spl_disabled is
 * the re-entry test's knob: with it set, spl6() is a no-op so the driver's
 * bracket is "artificially absent" and the simulated clock callout is no longer
 * masked.  mock_clock_masked() reports whether the CIA-A level-2 clock (the sole
 * trigger of timeout() callout dispatch) would be masked at the current level.
 */
int  mock_ipl;			/* current mock interrupt priority level        */
int  mock_spl_disabled;		/* test knob: when set, spl6() does not raise   */
long mock_spl6_calls;		/* count of spl6() calls (observability)        */
long mock_splx_calls;		/* count of splx() calls                        */

int spl6()
{
	int	old;

	old = mock_ipl;
	mock_spl6_calls++;
	if (!mock_spl_disabled)
		mock_ipl = 4;		/* _spl4: 0x2400 => IPL 4 */
	return old;
}

int splx( s)
int	s;
{
	mock_splx_calls++;
	mock_ipl = s;
	return 0;
}

int mock_clock_masked()
{
	return mock_ipl >= 4;		/* CIA-A level-2 clock masked at IPL >= 4 */
}

void mock_spl_reset()
{
	mock_ipl = 0;
	mock_spl_disabled = 0;
	mock_spl6_calls = 0;
	mock_splx_calls = 0;
}

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
 * timeout(func, arg, ticks): HISTORICAL -- nothing calls this any more.  The
 * driver used to defer completion to clock context via timeout(z3660done, cp, 1);
 * a5af58a removed that deferral, so completion is now delivered in-context by
 * z3660_complete() and the cross-compiled object's undefined-symbol set no longer
 * contains `timeout'.  The definition is kept as an inert stub of the kernel
 * service, still firing synchronously, so the historical symbol resolves if some
 * future test drives that path deliberately.
 */
int timeout( func, arg, ticks)
void	(*func)();
char	*arg;
int	ticks;
{
	(*func)( arg);
	return 0;
}

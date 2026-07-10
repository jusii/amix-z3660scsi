#ifndef _HOSTTEST_SYS_ERRNO_H
#define _HOSTTEST_SYS_ERRNO_H
/* Just the two errno values z3660map() returns (SVR4 values; the harness
 * never inspects them -- autocon() always succeeds so the failure paths that
 * use these are not exercised). */
#define ENXIO  6
#define ENOMEM 12
#endif

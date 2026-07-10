#ifndef _HOSTTEST_RICO_H
#define _HOSTTEST_RICO_H
/*
 * The "alien" short-name macros z3660.c and sd.h rely on.  This is a faithful
 * subset of the real usr/sys/amiga/alien/rico.h (identical definitions), just
 * trimmed to what the driver + the sdcom ABI need.  Included LAST in the
 * harness .c (after all system headers) because these macros are aggressive.
 */
#define uchar   unsigned char
#define ushort  unsigned short
#define uint    unsigned
#define ulong   unsigned long

#define bool    char
#define TRUE    (0 == 0)
#define FALSE   (not TRUE)
#define not     !
#define and     &&
#define or      ||
#define loop    while (TRUE)
#define until(expr)   while (not (expr))
#define unless(expr)  if (not (expr))
#endif

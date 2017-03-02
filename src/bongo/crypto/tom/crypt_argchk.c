/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 *
 * Tom St Denis, tomstdenis@gmail.com, http://libtom.org
 */
#include "tomcrypt.h"
#include <signal.h>

/**
  @file crypt_argchk.c
  Perform argument checking, Tom St Denis
*/

#if (ARGTYPE == 0)
void crypt_argchk(const char *v, const char *s, int d)
{
 fprintf(stderr, "LTC_ARGCHK '%s' failure on line %d of file %s\n",
         v, d, s);
 (void)raise(SIGABRT);
}
#endif

/* $Source: /cvs/libtom/libtomcrypt/src/misc/crypt/crypt_argchk.c,v $ */
/* $Revision: 1.5 $ */
/* $Date: 2006/12/28 01:27:24 $ */

/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */

/* Defines the LTC_ARGCHK macro used within the library */
/* ARGTYPE is defined in tomcrypt_cfg.h */
#if ARGTYPE == 0

#include <signal.h>

/* this is the default LibTomCrypt macro  */
#if defined(__clang__) || defined(__GNUC_MINOR__)
#define NORETURN __attribute__ ((noreturn))
#else
#define NORETURN
#endif

void crypt_argchk(char *v, char *s, int d) NORETURN;
#define LTC_ARGCHK(x) do { if (!(x)) { crypt_argchk(#x, __FILE__, __LINE__); } }while(0)
#define LTC_ARGCHKVD(x) do { if (!(x)) { crypt_argchk(#x, __FILE__, __LINE__); } }while(0)

#elif ARGTYPE == 1

/* fatal type of error */
#define LTC_ARGCHK(x) assert((x))
#define LTC_ARGCHKVD(x) LTC_ARGCHK(x)

#elif ARGTYPE == 2

#define LTC_ARGCHK(x) if (!(x)) { fprintf(stderr, "\nwarning: ARGCHK failed at %s:%d\n", __FILE__, __LINE__); }
#define LTC_ARGCHKVD(x) LTC_ARGCHK(x)

#elif ARGTYPE == 3

#define LTC_ARGCHK(x)
#define LTC_ARGCHKVD(x) LTC_ARGCHK(x)

#elif ARGTYPE == 4

#define LTC_ARGCHK(x)   if (!(x)) return CRYPT_INVALID_ARG;
#define LTC_ARGCHKVD(x) if (!(x)) return;

#endif


/* ref:         HEAD -> release/1.18.0, tag: v1.18.0-rc2 */
/* git commit:  aa0f396c0c8828ce39456129507fc72ef0208bd0 */
/* commit time: 2017-07-13 14:58:01 +0200 */

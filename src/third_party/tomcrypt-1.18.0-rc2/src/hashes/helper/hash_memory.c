/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */
#include "tomcrypt.h"

#ifdef LTC_HASH_HELPERS
/**
  @file hash_memory.c
  Hash memory helper, Tom St Denis
*/

/**
  Hash a block of memory and store the digest.
  @param hash   The index of the hash you wish to use
  @param in     The data you wish to hash
  @param inlen  The length of the data to hash (octets)
  @param out    [out] Where to store the digest
  @param outlen [in/out] Max size and resulting size of the digest
  @return CRYPT_OK if successful
*/
int hash_memory(int hash, const unsigned char *in, unsigned long inlen, unsigned char *out, unsigned long *outlen)
{
    hash_state *md;
    int err;

    LTC_ARGCHK(in     != NULL);
    LTC_ARGCHK(out    != NULL);
    LTC_ARGCHK(outlen != NULL);

    if ((err = hash_is_valid(hash)) != CRYPT_OK) {
        return err;
    }

    if (*outlen < hash_descriptor[hash].hashsize) {
       *outlen = hash_descriptor[hash].hashsize;
       return CRYPT_BUFFER_OVERFLOW;
    }

    md = XMALLOC(sizeof(hash_state));
    if (md == NULL) {
       return CRYPT_MEM;
    }

    if ((err = hash_descriptor[hash].init(md)) != CRYPT_OK) {
       goto LBL_ERR;
    }
    if ((err = hash_descriptor[hash].process(md, in, inlen)) != CRYPT_OK) {
       goto LBL_ERR;
    }
    err = hash_descriptor[hash].done(md, out);
    *outlen = hash_descriptor[hash].hashsize;
LBL_ERR:
#ifdef LTC_CLEAN_STACK
    zeromem(md, sizeof(hash_state));
#endif
    XFREE(md);

    return err;
}
#endif /* #ifdef LTC_HASH_HELPERS */

/* ref:         HEAD -> release/1.18.0, tag: v1.18.0-rc2 */
/* git commit:  aa0f396c0c8828ce39456129507fc72ef0208bd0 */
/* commit time: 2017-07-13 14:58:01 +0200 */

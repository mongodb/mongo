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

/**
  @file hmac_memory.c
  LTC_HMAC support, process a block of memory, Tom St Denis/Dobes Vandermeer
*/

#ifdef LTC_HMAC

/**
   LTC_HMAC a block of memory to produce the authentication tag
   @param hash      The index of the hash to use
   @param key       The secret key
   @param keylen    The length of the secret key (octets)
   @param in        The data to LTC_HMAC
   @param inlen     The length of the data to LTC_HMAC (octets)
   @param out       [out] Destination of the authentication tag
   @param outlen    [in/out] Max size and resulting size of authentication tag
   @return CRYPT_OK if successful
*/
int hmac_memory(int hash,
                const unsigned char *key,  unsigned long keylen,
                const unsigned char *in,   unsigned long inlen,
                      unsigned char *out,  unsigned long *outlen)
{
    hmac_state *hmac;
    int         err;

    LTC_ARGCHK(key    != NULL);
    LTC_ARGCHK(in     != NULL);
    LTC_ARGCHK(out    != NULL);
    LTC_ARGCHK(outlen != NULL);

    /* make sure hash descriptor is valid */
    if ((err = hash_is_valid(hash)) != CRYPT_OK) {
       return err;
    }

    /* is there a descriptor? */
    if (hash_descriptor[hash].hmac_block != NULL) {
        return hash_descriptor[hash].hmac_block(key, keylen, in, inlen, out, outlen);
    }

    /* nope, so call the hmac functions */
    /* allocate ram for hmac state */
    hmac = (hmac_state*)XMALLOC(sizeof(hmac_state));
    if (hmac == NULL) {
       return CRYPT_MEM;
    }

    if ((err = hmac_init(hmac, hash, key, keylen)) != CRYPT_OK) {
       goto LBL_ERR;
    }

    if ((err = hmac_process(hmac, in, inlen)) != CRYPT_OK) {
       goto LBL_ERR;
    }

    if ((err = hmac_done(hmac, out, outlen)) != CRYPT_OK) {
       goto LBL_ERR;
    }

   err = CRYPT_OK;
LBL_ERR:
#ifdef LTC_CLEAN_STACK
   zeromem(hmac, sizeof(hmac_state));
#endif

   XFREE(hmac);
   return err;
}

#endif


/* $Source: /cvs/libtom/libtomcrypt/src/mac/hmac/hmac_memory.c,v $ */
/* $Revision: 1.8 $ */
/* $Date: 2007/05/12 14:37:41 $ */

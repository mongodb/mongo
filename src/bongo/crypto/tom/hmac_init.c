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
  @file hmac_init.c
  LTC_HMAC support, initialize state, Tom St Denis/Dobes Vandermeer
*/

#ifdef LTC_HMAC

#define LTC_HMAC_BLOCKSIZE hash_descriptor[hash].blocksize

/**
   Initialize an LTC_HMAC context.
   @param hmac     The LTC_HMAC state
   @param hash     The index of the hash you want to use
   @param key      The secret key
   @param keylen   The length of the secret key (octets)
   @return CRYPT_OK if successful
*/
int hmac_init(hmac_state *hmac, int hash, const unsigned char *key, unsigned long keylen)
{
    unsigned char *buf;
    unsigned long hashsize;
    unsigned long i, z;
    int err;

    LTC_ARGCHK(hmac != NULL);
    LTC_ARGCHK(key  != NULL);

    /* valid hash? */
    if ((err = hash_is_valid(hash)) != CRYPT_OK) {
        return err;
    }
    hmac->hash = hash;
    hashsize   = hash_descriptor[hash].hashsize;

    /* valid key length? */
    if (keylen == 0) {
        return CRYPT_INVALID_KEYSIZE;
    }

    /* allocate ram for buf */
    buf = (unsigned char*)XMALLOC(LTC_HMAC_BLOCKSIZE);
    if (buf == NULL) {
       return CRYPT_MEM;
    }

    /* allocate memory for key */
    hmac->key = (unsigned char*)XMALLOC(LTC_HMAC_BLOCKSIZE);
    if (hmac->key == NULL) {
       XFREE(buf);
       return CRYPT_MEM;
    }

    /* (1) make sure we have a large enough key */
    if(keylen > LTC_HMAC_BLOCKSIZE) {
        z = LTC_HMAC_BLOCKSIZE;
        if ((err = hash_memory(hash, key, keylen, hmac->key, &z)) != CRYPT_OK) {
           goto LBL_ERR;
        }
        if(hashsize < LTC_HMAC_BLOCKSIZE) {
            zeromem((hmac->key) + hashsize, (size_t)(LTC_HMAC_BLOCKSIZE - hashsize));
        }
        keylen = hashsize;
    } else {
        XMEMCPY(hmac->key, key, (size_t)keylen);
        if(keylen < LTC_HMAC_BLOCKSIZE) {
            zeromem((hmac->key) + keylen, (size_t)(LTC_HMAC_BLOCKSIZE - keylen));
        }
    }

    /* Create the initial vector for step (3) */
    for(i=0; i < LTC_HMAC_BLOCKSIZE;   i++) {
       buf[i] = hmac->key[i] ^ 0x36;
    }

    /* Pre-pend that to the hash data */
    if ((err = hash_descriptor[hash].init(&hmac->md)) != CRYPT_OK) {
       goto LBL_ERR;
    }

    if ((err = hash_descriptor[hash].process(&hmac->md, buf, LTC_HMAC_BLOCKSIZE)) != CRYPT_OK) {
       goto LBL_ERR;
    }
    goto done;
LBL_ERR:
    /* free the key since we failed */
    XFREE(hmac->key);
done:
#ifdef LTC_CLEAN_STACK
   zeromem(buf, LTC_HMAC_BLOCKSIZE);
#endif

   XFREE(buf);
   return err;
}

#endif

/* $Source: /cvs/libtom/libtomcrypt/src/mac/hmac/hmac_init.c,v $ */
/* $Revision: 1.7 $ */
/* $Date: 2007/05/12 14:37:41 $ */

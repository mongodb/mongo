/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */
#include "tomcrypt.h"

/**
  @file crypt_register_hash.c
  Register a HASH, Tom St Denis
*/

/**
   Register a hash with the descriptor table
   @param hash   The hash you wish to register
   @return value >= 0 if successfully added (or already present), -1 if unsuccessful
*/
int register_hash(const struct ltc_hash_descriptor *hash)
{
   int x;

   LTC_ARGCHK(hash != NULL);

   /* is it already registered? */
   LTC_MUTEX_LOCK(&ltc_hash_mutex);
   for (x = 0; x < TAB_SIZE; x++) {
       if (XMEMCMP(&hash_descriptor[x], hash, sizeof(struct ltc_hash_descriptor)) == 0) {
          LTC_MUTEX_UNLOCK(&ltc_hash_mutex);
          return x;
       }
   }

   /* find a blank spot */
   for (x = 0; x < TAB_SIZE; x++) {
       if (hash_descriptor[x].name == NULL) {
          XMEMCPY(&hash_descriptor[x], hash, sizeof(struct ltc_hash_descriptor));
          LTC_MUTEX_UNLOCK(&ltc_hash_mutex);
          return x;
       }
   }

   /* no spot */
   LTC_MUTEX_UNLOCK(&ltc_hash_mutex);
   return -1;
}

/* ref:         HEAD -> release/1.18.0, tag: v1.18.0-rc2 */
/* git commit:  aa0f396c0c8828ce39456129507fc72ef0208bd0 */
/* commit time: 2017-07-13 14:58:01 +0200 */

/*
 * Copyright 2018-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kms_crypto.h"

#ifdef KMS_MESSAGE_ENABLE_CRYPTO_LIBCRYPTO

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
   (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x20700000L)
static EVP_MD_CTX *
EVP_MD_CTX_new (void)
{
   return calloc (sizeof (EVP_MD_CTX), 1);
}

static void
EVP_MD_CTX_free (EVP_MD_CTX *ctx)
{
   EVP_MD_CTX_cleanup (ctx);
   free (ctx);
}
#endif

int
kms_crypto_init ()
{
   return 0;
}

void
kms_crypto_cleanup ()
{
}

bool
kms_sha256 (void *unused_ctx,
            const char *input,
            size_t len,
            unsigned char *hash_out)
{
   EVP_MD_CTX *digest_ctxp = EVP_MD_CTX_new ();
   bool rval = false;

   if (1 != EVP_DigestInit_ex (digest_ctxp, EVP_sha256 (), NULL)) {
      goto cleanup;
   }

   if (1 != EVP_DigestUpdate (digest_ctxp, input, len)) {
      goto cleanup;
   }

   rval = (1 == EVP_DigestFinal_ex (digest_ctxp, hash_out, NULL));

cleanup:
   EVP_MD_CTX_free (digest_ctxp);

   return rval;
}

bool
kms_sha256_hmac (void *unused_ctx,
                 const char *key_input,
                 size_t key_len,
                 const char *input,
                 size_t len,
                 unsigned char *hash_out)
{
   return HMAC (EVP_sha256 (),
                key_input,
                key_len,
                (unsigned char *) input,
                len,
                hash_out,
                NULL) != NULL;
}

#endif /* KMS_MESSAGE_ENABLE_CRYPTO_LIBCRYPTO */

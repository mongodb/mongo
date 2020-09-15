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

#ifdef KMS_MESSAGE_ENABLE_CRYPTO_COMMON_CRYPTO

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>


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
   CC_SHA256_CTX ctx;
   CC_SHA256_Init (&ctx);
   CC_SHA256_Update (&ctx, input, len);
   CC_SHA256_Final (hash_out, &ctx);
   return true;
}

bool
kms_sha256_hmac (void *unused_ctx,
                 const char *key_input,
                 size_t key_len,
                 const char *input,
                 size_t len,
                 unsigned char *hash_out)
{
   CCHmac (kCCHmacAlgSHA256, key_input, key_len, input, len, hash_out);
   return true;
}

#endif /* KMS_MESSAGE_ENABLE_CRYPTO_COMMON_CRYPTO */

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

#ifndef KMS_MESSAGE_ENABLE_CRYPTO

int
kms_crypto_init (void)
{
   return 0;
}

void
kms_crypto_cleanup (void)
{
}

bool
kms_sha256 (void *unused_ctx,
            const char *input,
            size_t len,
            unsigned char *hash_out)
{
   /* only gets called if hooks were mistakenly not set */
   return false;
}

bool
kms_sha256_hmac (void *unused_ctx,
                 const char *key_input,
                 size_t key_len,
                 const char *input,
                 size_t len,
                 unsigned char *hash_out)
{
   /* only gets called if hooks were mistakenly not set */
   return false;
}

bool
kms_sign_rsaes_pkcs1_v1_5 (void *unused_ctx,
                           const char *private_key,
                           size_t private_key_len,
                           const char *input,
                           size_t input_len,
                           unsigned char *signature_out) {
   /* only gets called if hooks were mistakenly not set */
   return false;
}

#endif /* KMS_MESSAGE_ENABLE_CRYPTO */

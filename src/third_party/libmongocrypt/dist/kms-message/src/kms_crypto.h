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

#ifndef KMS_MESSAGE_KMS_CRYPTO_H
#define KMS_MESSAGE_KMS_CRYPTO_H

#include <stdbool.h>
#include <stdlib.h>

typedef struct {
   bool (*sha256) (void *ctx,
                   const char *input,
                   size_t len,
                   unsigned char *hash_out);
   bool (*sha256_hmac) (void *ctx,
                        const char *key_input,
                        size_t key_len,
                        const char *input,
                        size_t len,
                        unsigned char *hash_out);
   bool (*sign_rsaes_pkcs1_v1_5) (void *sign_ctx,
                                  const char *private_key,
                                  size_t private_key_len,
                                  const char *input,
                                  size_t input_len,
                                  unsigned char *signature_out);
   void *ctx;
   void *sign_ctx;
} _kms_crypto_t;

int
kms_crypto_init (void);

void
kms_crypto_cleanup (void);

bool
kms_sha256 (void *ctx, const char *input, size_t len, unsigned char *hash_out);

bool
kms_sha256_hmac (void *ctx,
                 const char *key_input,
                 size_t key_len,
                 const char *input,
                 size_t len,
                 unsigned char *hash_out);

/* signature_out must be a preallocated buffer of 256 bytes (or greater). */
bool
kms_sign_rsaes_pkcs1_v1_5 (void *sign_ctx,
                           const char *private_key,
                           size_t private_key_len,
                           const char *input,
                           size_t input_len,
                           unsigned char *signature_out);

#endif /* KMS_MESSAGE_KMS_CRYPTO_H */

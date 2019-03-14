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

int
kms_crypto_init ();

void
kms_crypto_cleanup ();

bool
kms_sha256 (const char *input, size_t len, unsigned char *hash_out);

bool
kms_sha256_hmac (const char *key_input,
                 size_t key_len,
                 const char *input,
                 size_t len,
                 unsigned char *hash_out);

#endif /* KMS_MESSAGE_KMS_CRYPTO_H */

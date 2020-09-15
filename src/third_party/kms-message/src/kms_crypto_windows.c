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

#ifdef KMS_MESSAGE_ENABLE_CRYPTO_CNG

// tell windows.h not to include a bunch of headers we don't need:
#define WIN32_LEAN_AND_MEAN

// Tell windows.h not to define any NT status codes, so that we can
// get the definitions from ntstatus.h, which has a more complete list.
#define WIN32_NO_STATUS

#include <windows.h>

#undef WIN32_NO_STATUS

// Obtain a definition for the ntstatus type.
#include <winternl.h>

// Add back in the status definitions so that macro expansions for
// things like STILL_ACTIVE and WAIT_OBJECT_O can be resolved (they
// expand to STATUS_ codes).
#include <ntstatus.h>

#include <bcrypt.h>

static BCRYPT_ALG_HANDLE _algoSHA256 = 0;
static BCRYPT_ALG_HANDLE _algoSHA256Hmac = 0;

int
kms_crypto_init ()
{
   if (BCryptOpenAlgorithmProvider (
          &_algoSHA256, BCRYPT_SHA256_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0) !=
       STATUS_SUCCESS) {
      return 1;
   }

   if (BCryptOpenAlgorithmProvider (&_algoSHA256Hmac,
                                    BCRYPT_SHA256_ALGORITHM,
                                    MS_PRIMITIVE_PROVIDER,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) !=
       STATUS_SUCCESS) {
      return 2;
   }

   return 0;
}

void
kms_crypto_cleanup ()
{
   (void) BCryptCloseAlgorithmProvider (_algoSHA256, 0);
   (void) BCryptCloseAlgorithmProvider (_algoSHA256Hmac, 0);
}

bool
kms_sha256 (void *unused_ctx,
            const char *input,
            size_t len,
            unsigned char *hash_out)
{
   BCRYPT_HASH_HANDLE hHash;

   NTSTATUS status =
      BCryptCreateHash (_algoSHA256, &hHash, NULL, 0, NULL, 0, 0);
   if (status != STATUS_SUCCESS) {
      return 0;
   }

   status = BCryptHashData (hHash, (PUCHAR) (input), (ULONG) len, 0);
   if (status != STATUS_SUCCESS) {
      goto cleanup;
   }

   // Hardcode output length
   status = BCryptFinishHash (hHash, hash_out, 256 / 8, 0);
   if (status != STATUS_SUCCESS) {
      goto cleanup;
   }

cleanup:
   (void) BCryptDestroyHash (hHash);

   return status == STATUS_SUCCESS ? 1 : 0;
}

bool
kms_sha256_hmac (void *unused_ctx,
                 const char *key_input,
                 size_t key_len,
                 const char *input,
                 size_t len,
                 unsigned char *hash_out)
{
   BCRYPT_HASH_HANDLE hHash;

   NTSTATUS status = BCryptCreateHash (
      _algoSHA256Hmac, &hHash, NULL, 0, (PUCHAR) key_input, (ULONG) key_len, 0);
   if (status != STATUS_SUCCESS) {
      return 0;
   }

   status = BCryptHashData (hHash, (PUCHAR) input, (ULONG) len, 0);
   if (status != STATUS_SUCCESS) {
      goto cleanup;
   }

   // Hardcode output length
   status = BCryptFinishHash (hHash, hash_out, 256 / 8, 0);
   if (status != STATUS_SUCCESS) {
      goto cleanup;
   }

cleanup:
   (void) BCryptDestroyHash (hHash);

   return status == STATUS_SUCCESS ? 1 : 0;
}

#endif /* KMS_MESSAGE_ENABLE_CRYPTO_CNG */

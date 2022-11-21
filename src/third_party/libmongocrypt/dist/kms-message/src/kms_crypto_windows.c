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
#include <wincrypt.h>

static BCRYPT_ALG_HANDLE _algoSHA256 = 0;
static BCRYPT_ALG_HANDLE _algoSHA256Hmac = 0;
static BCRYPT_ALG_HANDLE _algoRSA = 0;

#define SHA_256_HASH_LEN 32

int
kms_crypto_init (void)
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

   if (BCryptOpenAlgorithmProvider (
          &_algoRSA, BCRYPT_RSA_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0) !=
       STATUS_SUCCESS) {
      return 3;
   }

   return 0;
}

void
kms_crypto_cleanup (void)
{
   (void) BCryptCloseAlgorithmProvider (_algoSHA256, 0);
   (void) BCryptCloseAlgorithmProvider (_algoSHA256Hmac, 0);
   (void) BCryptCloseAlgorithmProvider (_algoRSA, 0);
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

bool
kms_sign_rsaes_pkcs1_v1_5 (void *unused_ctx,
                           const char *private_key,
                           size_t private_key_len,
                           const char *input,
                           size_t input_len,
                           unsigned char *signature_out)
{
   bool success = false;
   bool ret = false;
   LPBYTE blob_private = NULL;
   DWORD blob_private_len = 0;
   LPBYTE raw_private = NULL;
   DWORD raw_private_len = 0;

   NTSTATUS status;
   BCRYPT_KEY_HANDLE hKey = NULL;
   BCRYPT_PKCS1_PADDING_INFO padding_PKCS1;

   unsigned char *hash_value = NULL;
   DWORD hash_length = 256;

   success = CryptDecodeObjectEx (X509_ASN_ENCODING,
                                  PKCS_PRIVATE_KEY_INFO,
                                  private_key,
                                  (DWORD) private_key_len,
                                  0,
                                  NULL,
                                  NULL,
                                  &blob_private_len);
   if (!success) {
      goto cleanup;
   }

   blob_private = (LPBYTE) calloc (1, blob_private_len);

   success = CryptDecodeObjectEx (X509_ASN_ENCODING,
                                  PKCS_PRIVATE_KEY_INFO,
                                  private_key,
                                  (DWORD) private_key_len,
                                  0,
                                  NULL,
                                  blob_private,
                                  &blob_private_len);
   if (!success) {
      goto cleanup;
   }

   CRYPT_PRIVATE_KEY_INFO *privateKeyInfo =
      (CRYPT_PRIVATE_KEY_INFO *) blob_private;

   success = CryptDecodeObjectEx (X509_ASN_ENCODING,
                                  PKCS_RSA_PRIVATE_KEY,
                                  privateKeyInfo->PrivateKey.pbData,
                                  (DWORD) privateKeyInfo->PrivateKey.cbData,
                                  0,
                                  NULL,
                                  NULL,
                                  &raw_private_len);
   if (!success) {
      goto cleanup;
   }

   raw_private = (LPBYTE) calloc (1, raw_private_len);

   success = CryptDecodeObjectEx (X509_ASN_ENCODING,
                                  PKCS_RSA_PRIVATE_KEY,
                                  privateKeyInfo->PrivateKey.pbData,
                                  (DWORD) privateKeyInfo->PrivateKey.cbData,
                                  0,
                                  NULL,
                                  raw_private,
                                  &raw_private_len);
   if (!success) {
      goto cleanup;
   }

   status = BCryptImportKeyPair (
      _algoRSA,                
      NULL,                   
      LEGACY_RSAPRIVATE_BLOB, 
      &hKey,            
      raw_private,            
      raw_private_len,       
      0);                     
   if (!NT_SUCCESS (status)) {
      goto cleanup;
   }

   hash_value = calloc (1, SHA_256_HASH_LEN);

   if(!kms_sha256 (NULL, input, input_len, hash_value)) {
      goto cleanup;
   }

   padding_PKCS1.pszAlgId = BCRYPT_SHA256_ALGORITHM;

   status =
      BCryptSignHash (hKey,
                      &padding_PKCS1,
                      hash_value,        
                      SHA_256_HASH_LEN,            
                      signature_out,
                      hash_length, 
                      &hash_length, 
                      BCRYPT_PAD_PKCS1); 
   if (!NT_SUCCESS (status)) {
      goto cleanup;
   }

   ret = true;

cleanup:
   BCryptDestroyKey(hKey);
   free (blob_private);
   free (raw_private);
   free (hash_value);

   return ret;
}

#endif /* KMS_MESSAGE_ENABLE_CRYPTO_CNG */

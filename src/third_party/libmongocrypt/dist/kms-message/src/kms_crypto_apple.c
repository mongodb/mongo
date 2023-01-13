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
#include "kms_message_private.h" /* KMS_ASSERT */

#ifdef KMS_MESSAGE_ENABLE_CRYPTO_COMMON_CRYPTO

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CoreFoundation/CFArray.h>
#include <Security/SecKey.h>
#include <Security/SecItem.h>
#include <Security/SecImportExport.h>

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
   CC_SHA256_CTX ctx;
   CC_SHA256_Init (&ctx);
   KMS_ASSERT (len <= (size_t) UINT32_MAX);
   CC_SHA256_Update (&ctx, input, (uint32_t) len);
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

static void
safe_CFRelease (CFTypeRef ptr)
{
   if (ptr) {
      CFRelease (ptr);
   }
}

bool
kms_sign_rsaes_pkcs1_v1_5 (void *unused_ctx,
                           const char *private_key,
                           size_t private_key_len,
                           const char *input,
                           size_t input_len,
                           unsigned char *signature_out)
{
   CFDataRef key_data_ref = NULL;
   CFDataRef pass_ref = NULL;
   SecItemImportExportKeyParameters import_params;
   OSStatus status;
   /* TODO: I think the expected format should be kSecFormatWrappedPKCS8, but
    * GCP keys appear to only load for kSecFormatBSAFE. */
   SecExternalFormat format = kSecFormatUnknown;
   SecExternalItemType type = kSecItemTypePrivateKey;
   CFArrayRef out_ref = NULL;
   SecKeyRef key_ref = NULL;
   CFDataRef data_to_sign_ref = NULL;
   CFErrorRef error_ref;
   CFDataRef signature_ref = NULL;
   bool ret = false;

   key_data_ref = CFDataCreate (NULL /* default allocator */,
                                (const uint8_t *) private_key,
                                (CFIndex) private_key_len);
   if (!key_data_ref) {
      goto cleanup;
   }
   memset (&import_params, 0, sizeof (SecItemImportExportKeyParameters));
   import_params.version = SEC_KEY_IMPORT_EXPORT_PARAMS_VERSION;

   /* Give an empty password. SecItemImport returns an error expecting a
    * password. */
   pass_ref = CFDataCreate (NULL, NULL, 0);
   if (!pass_ref) {
      goto cleanup;
   }
   import_params.passphrase = (CFTypeRef) pass_ref;

   status = SecItemImport (key_data_ref,
                           NULL /* extension. */,
                           &format,
                           &type,
                           0,
                           &import_params,
                           NULL /* keychain */,
                           &out_ref);
   if (status != errSecSuccess) {
      goto cleanup;
   }
   if (1 != CFArrayGetCount (out_ref)) {
      goto cleanup;
   }

   key_ref = (SecKeyRef) CFArrayGetValueAtIndex (out_ref, 0);
   KMS_ASSERT (input_len <= (size_t) LONG_MAX);
   data_to_sign_ref =
      CFDataCreate (NULL, (const uint8_t *) input, (long) input_len);
   if (!data_to_sign_ref) {
      goto cleanup;
   }
   error_ref = NULL;
   signature_ref =
      SecKeyCreateSignature (key_ref,
                             kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
                             data_to_sign_ref,
                             &error_ref);
   if (!signature_ref) {
      goto cleanup;
   }
   memcpy (signature_out,
           CFDataGetBytePtr (signature_ref),
           (size_t) CFDataGetLength (signature_ref));

   ret = true;
cleanup:
   safe_CFRelease (key_data_ref);
   safe_CFRelease (pass_ref);
   safe_CFRelease (out_ref);
   safe_CFRelease (data_to_sign_ref);
   safe_CFRelease (signature_ref);
   return ret;
}

#endif /* KMS_MESSAGE_ENABLE_CRYPTO_COMMON_CRYPTO */

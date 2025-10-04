/*
 * Copyright 2020-present MongoDB, Inc.
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

#ifndef KMS_AZURE_REQUEST_H
#define KMS_AZURE_REQUEST_H

#include "kms_message_defines.h"
#include "kms_request.h"
#include "kms_request_opt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Constructs an oauth client credentials grant request for Azure.
 * See
 * https://docs.microsoft.com/en-us/azure/active-directory/develop/v2-oauth2-client-creds-grant-flow#get-a-token.
 *
 * Parameters:
 * All parameters must be NULL terminated strings.
 * - host: The value of the Host header. This should be a custom host or
 * "login.microsoftonline.com".
 * - scope: The oauth scope. This should be a custom scope or
 * "https%3A%2F%2Fvault.azure.net%2F.default". Must be URL encoded.
 * - tenant_id: The Azure tenant ID.
 * - client_id: The client ID to authenticate.
 * - client_secret: The client secret to authenticate.
 * - opt: Additional options. This must have the Azure provider set via
 * kms_request_opt_set_provider.
 *
 * Returns: A new kms_request_t.
 * Always returns a new kms_request_t, even on error.
 * Caller must check if an error occurred by calling kms_request_get_error.
 */
KMS_MSG_EXPORT (kms_request_t *)
kms_azure_request_oauth_new (const char *host,
                             const char *scope,
                             const char *tenant_id,
                             const char *client_id,
                             const char *client_secret,
                             const kms_request_opt_t *opt);

/* Constructs a wrapkey request for Azure.
 * See https://docs.microsoft.com/en-us/rest/api/keyvault/wrapkey/wrapkey
 *
 * Parameters:
 * All parameters must be NULL terminated strings.
 * - host: The value of the Host header, like "mykeyvault.vault.azure.net".
 * - access_token: The access_token obtained from an oauth response as a
 * base64url encoded string.
 * - key_name: The azure key name.
 * - key_version: An optional key version. May be NULL or empty string.
 * - plaintext: The plaintext key to encrypt.
 * - plaintext_len: The number of bytes of plaintext.
 * - opt: Additional options. This must have the Azure provider set via
 * kms_request_opt_set_provider.
 */

KMS_MSG_EXPORT (kms_request_t *)
kms_azure_request_wrapkey_new (const char *host,
                               const char *access_token,
                               const char *key_name,
                               const char *key_version,
                               const uint8_t *plaintext,
                               size_t plaintext_len,
                               const kms_request_opt_t *opt);

/* Constructs an unwrapkey request for Azure.
 * See https://docs.microsoft.com/en-us/rest/api/keyvault/unwrapkey/unwrapkey
 *
 * Parameters:
 * All parameters must be NULL terminated strings.
 * - host: The value of the Host header, like "mykeyvault.vault.azure.net".
 * - access_token: The access_token obtained from an oauth response as a
 * base64url encoded string.
 * - key_name: The azure key name.
 * - key_version: An optional key version. May be NULL or empty string.
 * - ciphertext: The ciphertext key to decrypt.
 * - ciphertext_len: The number of bytes of ciphertext.
 * - opt: Additional options. This must have the Azure provider set via
 * kms_request_opt_set_provider.
 */

KMS_MSG_EXPORT (kms_request_t *)
kms_azure_request_unwrapkey_new (const char *host,
                                 const char *access_token,
                                 const char *key_name,
                                 const char *key_version,
                                 const uint8_t *ciphertext,
                                 size_t ciphertext_len,
                                 const kms_request_opt_t *opt);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KMS_AZURE_REQUEST_H */

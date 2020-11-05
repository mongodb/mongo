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

#ifndef KMS_GCP_REQUEST_H
#define KMS_GCP_REQUEST_H

#include "kms_message_defines.h"
#include "kms_request.h"
#include "kms_request_opt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Constructs an oauth client credentials request for GCP.
 * See https://developers.google.com/identity/protocols/oauth2/service-account
 *
 * Parameters:
 * - host: The host header, like "oauth2.googleapis.com".
 * - email: The email for the service account to authenticate.
 * - audience: The "aud" field in the JSON Web Token (JWT). Should be a URL
 *   like "https://oauth2.googleapis.com/token"
 * - scope: The "scope" field in the JSON Web Token (JWT). Should be a URL
 *   like "https://www.googleapis.com/auth/cloudkms".
 * - private_key_data: Bytes pointing to a PKCS#8 private key.
 * - private_key_len: The length of private_key_data.
 * - opt: Request options. The provider must be set to KMS_REQUEST_PROVIDER_GCP
 *   with kms_request_opt_set_provider. Callers that want to use a custom crypto
 *   callback to sign the request should set the callback on opt with
 *   kms_request_opt_set_crypto_hook_rsaes_pkcs1_v1_5.
 *
 * Returns: A new kms_request_t.
 * Always returns a new kms_request_t, even on error.
 * Caller must check if an error occurred by calling kms_request_get_error.
 */
KMS_MSG_EXPORT (kms_request_t *)
kms_gcp_request_oauth_new (const char *host,
                           const char *email,
                           const char *audience,
                           const char *scope,
                           const char *private_key_data,
                           size_t private_key_len,
                           const kms_request_opt_t *opt);

/* Constructs the encrypt request for GCP.
 * See
 * https://cloud.google.com/kms/docs/encrypt-decrypt#kms-encrypt-symmetric-api
 *
 * Parameters:
 * - host: The value of the Host header, like "cloudkms.googleapis.com".
 * - project_id: The project id.
 * - location: The location id, like "global".
 * - key_ring_name: The key ring name.
 * - key_name: The key name.
 * - key_version: The optional key version. May be NULL.
 * - plaintext: The plaintext key to encrypt.
 * - plaintext_len: The number of bytes of plaintext.
 * - opt: Request options. The provider must be set to KMS_REQUEST_PROVIDER_GCP
 *   with kms_request_opt_set_provider.
 *
 * Returns: A new kms_request_t.
 * Always returns a new kms_request_t, even on error.
 * Caller must check if an error occurred by calling kms_request_get_error.
 */
KMS_MSG_EXPORT (kms_request_t *)
kms_gcp_request_encrypt_new (const char *host,
                             const char *access_token,
                             const char *project_id,
                             const char *location,
                             const char *key_ring_name,
                             const char *key_name,
                             const char *key_version,
                             const uint8_t *plaintext,
                             size_t plaintext_len,
                             const kms_request_opt_t *opt);

/* Constructs the decrypt request for GCP.
 * See
 * https://cloud.google.com/kms/docs/encrypt-decrypt#kms-decrypt-symmetric-api
 *
 * Parameters:
 * - host: The value of the Host header, like "cloudkms.googleapis.com".
 * - project_id: The project id.
 * - location: The location id, like "global".
 * - key_ring_name: The key ring name.
 * - key_name: The key name.
 * - ciphertext: The ciphertext key to encrypt.
 * - ciphertext_len: The number of bytes of ciphertext.
 * - opt: Request options. The provider must be set to KMS_REQUEST_PROVIDER_GCP
 *   with kms_request_opt_set_provider.
 *
 * Returns: A new kms_request_t.
 * Always returns a new kms_request_t, even on error.
 * Caller must check if an error occurred by calling kms_request_get_error.
 */
KMS_MSG_EXPORT (kms_request_t *)
kms_gcp_request_decrypt_new (const char *host,
                             const char *access_token,
                             const char *project_id,
                             const char *location,
                             const char *key_ring_name,
                             const char *key_name,
                             const uint8_t *ciphertext,
                             size_t ciphertext_len,
                             const kms_request_opt_t *opt);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KMS_GCP_REQUEST_H */

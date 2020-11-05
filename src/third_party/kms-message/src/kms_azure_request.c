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

#include "kms_message/kms_azure_request.h"

#include "kms_message/kms_b64.h"
#include "kms_message_private.h"
#include "kms_request_opt_private.h"
#include "kms_request_str.h"

/*
 * Request has the following form:
 * 
 * POST /{tenant ID}/oauth2/v2.0/token HTTP/1.1
 * Host: {host of identify platform URL}
 * Content-Type: application/x-www-form-urlencoded
 * 
 * client_id={client ID}
 * &scope=https%3A%2F%2Fvault.azure.net%2F.default
 * &client_secret={client secret}
 * &grant_type=client_credentials
*/
kms_request_t *
kms_azure_request_oauth_new (const char *host,
                             const char *scope,
                             const char *tenant_id,
                             const char *client_id,
                             const char *client_secret,
                             const kms_request_opt_t *opt)
{
   char *path_and_query = NULL;
   char *payload = NULL;
   kms_request_t *req;
   kms_request_str_t *str;

   str = kms_request_str_new ();
   kms_request_str_appendf (str, "/%s/oauth2/v2.0/token", tenant_id);
   path_and_query = kms_request_str_detach (str);
   str = kms_request_str_new ();
   kms_request_str_appendf (
      str,
      "client_id=%s&scope=%s&client_secret=%s&grant_type=client_credentials",
      client_id,
      scope,
      client_secret);
   payload = kms_request_str_detach (str);

   req = kms_request_new ("POST", path_and_query, opt);

   if (opt->provider != KMS_REQUEST_PROVIDER_AZURE) {
      KMS_ERROR (req, "Expected KMS request with provider type: Azure");
      goto done;
   }

   if (kms_request_get_error (req)) {
      goto done;
   }

   if (!kms_request_add_header_field (
          req, "Content-Type", "application/x-www-form-urlencoded")) {
      goto done;
   }
   if (!kms_request_add_header_field (req, "Host", host)) {
      goto done;
   }
   if (!kms_request_add_header_field (req, "Accept", "application/json")) {
      goto done;
   }

   if (!kms_request_append_payload (req, payload, strlen (payload))) {
      goto done;
   }

done:
   kms_request_free_string (path_and_query);
   kms_request_free_string (payload);
   return req;
}

static kms_request_t *
_wrap_unwrap_common (const char *wrap_unwrap,
                     const char *host,
                     const char *access_token,
                     const char *key_name,
                     const char *key_version,
                     const uint8_t *value,
                     size_t value_len,
                     const kms_request_opt_t *opt)
{
   char *path_and_query = NULL;
   char *payload = NULL;
   char *bearer_token_value = NULL;
   char *value_base64url = NULL;
   kms_request_t *req;
   kms_request_str_t *str;

   str = kms_request_str_new ();
   /* {vaultBaseUrl}/keys/{key-name}/{key-version}/wrapkey?api-version=7.1 */
   kms_request_str_appendf (str,
                            "/keys/%s/%s/%s?api-version=7.1",
                            key_name,
                            key_version ? key_version : "",
                            wrap_unwrap);
   path_and_query = kms_request_str_detach (str);

   req = kms_request_new ("POST", path_and_query, opt);

   if (opt->provider != KMS_REQUEST_PROVIDER_AZURE) {
      KMS_ERROR (req, "Expected KMS request with provider type: Azure");
      goto done;
   }

   if (kms_request_get_error (req)) {
      goto done;
   }

   value_base64url = kms_message_raw_to_b64url (value, value_len);
   if (!value_base64url) {
      KMS_ERROR (req, "Could not bases64url-encode plaintext");
      goto done;
   }

   str = kms_request_str_new ();
   kms_request_str_appendf (
      str, "{\"alg\": \"RSA-OAEP-256\", \"value\": \"%s\"}", value_base64url);
   payload = kms_request_str_detach (str);
   str = kms_request_str_new ();
   kms_request_str_appendf (str, "Bearer %s", access_token);
   bearer_token_value = kms_request_str_detach (str);
   if (!kms_request_add_header_field (
          req, "Authorization", bearer_token_value)) {
      goto done;
   }
   if (!kms_request_add_header_field (
          req, "Content-Type", "application/json")) {
      goto done;
   }
   if (!kms_request_add_header_field (req, "Host", host)) {
      goto done;
   }
   if (!kms_request_add_header_field (req, "Accept", "application/json")) {
      goto done;
   }

   if (!kms_request_append_payload (req, payload, strlen (payload))) {
      goto done;
   }

done:
   kms_request_free_string (path_and_query);
   kms_request_free_string (payload);
   kms_request_free_string (bearer_token_value);
   kms_request_free_string (value_base64url);
   return req;
}

/* 
 * Request has the following form:
 * 
 * POST /keys/{key-name}/{key-version}/wrapkey?api-version=7.1
 * Host: {host of key vault endpoint}
 * Authentication: Bearer {token}
 * Content-Type: application/json
 * 
 * {
 *     "alg": "RSA-OAEP-256"
 *     "value": "base64url encoded data"
 * }
 */
kms_request_t *
kms_azure_request_wrapkey_new (const char *host,
                               const char *access_token,
                               const char *key_name,
                               const char *key_version,
                               const uint8_t *plaintext,
                               size_t plaintext_len,
                               const kms_request_opt_t *opt)
{
   return _wrap_unwrap_common ("wrapkey",
                               host,
                               access_token,
                               key_name,
                               key_version,
                               plaintext,
                               plaintext_len,
                               opt);
}

kms_request_t *
kms_azure_request_unwrapkey_new (const char *host,
                                 const char *access_token,
                                 const char *key_name,
                                 const char *key_version,
                                 const uint8_t *ciphertext,
                                 size_t ciphertext_len,
                                 const kms_request_opt_t *opt)
{
   return _wrap_unwrap_common ("unwrapkey",
                               host,
                               access_token,
                               key_name,
                               key_version,
                               ciphertext,
                               ciphertext_len,
                               opt);
}
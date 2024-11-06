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

#include "kms_message/kms_gcp_request.h"

#include "kms_message/kms_b64.h"
#include "kms_message_private.h"
#include "kms_request_opt_private.h"

/* Set a default expiration of 5 minutes for JSON Web Tokens (GCP allows up to
 * one hour) */
#define JWT_EXPIRATION_SECS 5 * 60
#define SIGNATURE_LEN 256

kms_request_t *
kms_gcp_request_oauth_new (const char *host,
                           const char *email,
                           const char *audience,
                           const char *scope,
                           const char *private_key_data,
                           size_t private_key_len,
                           const kms_request_opt_t *opt)
{
   kms_request_t *req = NULL;
   kms_request_str_t *str = NULL;
   time_t issued_at;
   /* base64 encoding of {"alg":"RS256","typ":"JWT"} */
   const char *jwt_header_b64url = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9";
   char *jwt_claims_b64url = NULL;
   char *jwt_header_and_claims_b64url = NULL;
   uint8_t *jwt_signature = NULL;
   char *jwt_signature_b64url = NULL;
   char *jwt_assertion_b64url = NULL;
   char *payload = NULL;

   req = kms_request_new ("POST", "/token", opt);
   if (opt->provider != KMS_REQUEST_PROVIDER_GCP) {
      KMS_ERROR (req, "Expected KMS request with provider type: GCP");
      goto done;
   }

   if (kms_request_get_error (req)) {
      goto done;
   }

   /* Produce the signed JWT <base64url header>.<base64url claims>.<base64url
    * signature> */
   issued_at = time (NULL);
   str = kms_request_str_new ();
   kms_request_str_appendf (str,
                            "{\"iss\": \"%s\", \"aud\": \"%s\", \"scope\": "
                            "\"%s\", \"iat\": %lu, \"exp\": %lu}",
                            email,
                            audience,
                            scope,
                            (unsigned long) issued_at,
                            (unsigned long) issued_at + JWT_EXPIRATION_SECS);
   jwt_claims_b64url =
      kms_message_raw_to_b64url ((const uint8_t *) str->str, str->len);
   kms_request_str_destroy (str);
   if (!jwt_claims_b64url) {
      KMS_ERROR (req, "Failed to base64url encode JWT claims");
      goto done;
   }

   str = kms_request_str_new ();
   kms_request_str_appendf (str, "%s.%s", jwt_header_b64url, jwt_claims_b64url);
   jwt_header_and_claims_b64url = kms_request_str_detach (str);

   /* Produce the signature of <base64url header>.<base64url claims> */
   req->crypto.sign_rsaes_pkcs1_v1_5 = kms_sign_rsaes_pkcs1_v1_5;
   if (opt->crypto.sign_rsaes_pkcs1_v1_5) {
      req->crypto.sign_rsaes_pkcs1_v1_5 = opt->crypto.sign_rsaes_pkcs1_v1_5;
      req->crypto.sign_ctx = opt->crypto.sign_ctx;
   }

   jwt_signature = calloc (1, SIGNATURE_LEN);
   if (!req->crypto.sign_rsaes_pkcs1_v1_5 (
          req->crypto.sign_ctx,
          private_key_data,
          private_key_len,
          jwt_header_and_claims_b64url,
          strlen (jwt_header_and_claims_b64url),
          jwt_signature)) {
      KMS_ERROR (req, "Failed to create GCP oauth request signature");
      goto done;
   }

   jwt_signature_b64url =
      kms_message_raw_to_b64url (jwt_signature, SIGNATURE_LEN);
   if (!jwt_signature_b64url) {
      KMS_ERROR (req, "Failed to base64url encode JWT signature");
      goto done;
   }
   str = kms_request_str_new ();
   kms_request_str_appendf (str,
                            "%s.%s.%s",
                            jwt_header_b64url,
                            jwt_claims_b64url,
                            jwt_signature_b64url);
   jwt_assertion_b64url = kms_request_str_detach (str);

   str =
      kms_request_str_new_from_chars ("grant_type=urn%3Aietf%3Aparams%3Aoauth%"
                                      "3Agrant-type%3Ajwt-bearer&assertion=",
                                      -1);
   kms_request_str_append_chars (str, jwt_assertion_b64url, -1);
   payload = kms_request_str_detach (str);

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
   free (jwt_signature);
   free (jwt_signature_b64url);
   free (jwt_claims_b64url);
   free (jwt_header_and_claims_b64url);
   free (jwt_assertion_b64url);
   free (payload);
   return req;
}

static kms_request_t *
_encrypt_decrypt_common (const char *encrypt_decrypt,
                         const char *host,
                         const char *access_token,
                         const char *project_id,
                         const char *location,
                         const char *key_ring_name,
                         const char *key_name,
                         const char *key_version,
                         const uint8_t *value,
                         size_t value_len,
                         const kms_request_opt_t *opt)
{
   char *path_and_query = NULL;
   char *payload = NULL;
   char *bearer_token_value = NULL;
   char *value_base64 = NULL;
   kms_request_t *req;
   kms_request_str_t *str;

   str = kms_request_str_new ();
   /* /v1/projects/{project-id}/locations/{location}/keyRings/{key-ring-name}/cryptoKeys/{key-name}
    */
   kms_request_str_appendf (
      str,
      "/v1/projects/%s/locations/%s/keyRings/%s/cryptoKeys/%s",
      project_id,
      location,
      key_ring_name,
      key_name);
   if (key_version && strlen (key_version) > 0) {
      kms_request_str_appendf (str, "/cryptoKeyVersions/%s", key_version);
   }
   kms_request_str_appendf (str, ":%s", encrypt_decrypt);
   path_and_query = kms_request_str_detach (str);

   req = kms_request_new ("POST", path_and_query, opt);

   if (opt->provider != KMS_REQUEST_PROVIDER_GCP) {
      KMS_ERROR (req, "Expected KMS request with provider type: GCP");
      goto done;
   }

   if (kms_request_get_error (req)) {
      goto done;
   }

   value_base64 = kms_message_raw_to_b64 (value, value_len);
   if (!value_base64) {
      KMS_ERROR (req, "Could not bases64-encode plaintext");
      goto done;
   }

   str = kms_request_str_new ();
   if (0 == strcmp ("encrypt", encrypt_decrypt)) {
      kms_request_str_appendf (str, "{\"plaintext\": \"%s\"}", value_base64);
   } else {
      kms_request_str_appendf (str, "{\"ciphertext\": \"%s\"}", value_base64);
   }

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
   kms_request_free_string (value_base64);
   return req;
}

kms_request_t *
kms_gcp_request_encrypt_new (const char *host,
                             const char *access_token,
                             const char *project_id,
                             const char *location,
                             const char *key_ring_name,
                             const char *key_name,
                             const char *key_version,
                             const uint8_t *plaintext,
                             size_t plaintext_len,
                             const kms_request_opt_t *opt)
{
   return _encrypt_decrypt_common ("encrypt",
                                   host,
                                   access_token,
                                   project_id,
                                   location,
                                   key_ring_name,
                                   key_name,
                                   key_version,
                                   plaintext,
                                   plaintext_len,
                                   opt);
}

kms_request_t *
kms_gcp_request_decrypt_new (const char *host,
                             const char *access_token,
                             const char *project_id,
                             const char *location,
                             const char *key_ring_name,
                             const char *key_name,
                             const uint8_t *ciphertext,
                             size_t ciphertext_len,
                             const kms_request_opt_t *opt)
{
   return _encrypt_decrypt_common ("decrypt",
                                   host,
                                   access_token,
                                   project_id,
                                   location,
                                   key_ring_name,
                                   key_name,
                                   NULL /* key_version */,
                                   ciphertext,
                                   ciphertext_len,
                                   opt);
}
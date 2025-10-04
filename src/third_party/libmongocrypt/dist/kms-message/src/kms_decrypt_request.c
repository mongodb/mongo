/*
 * Copyright 2018-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"){}
 *
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

#include "kms_message/kms_message.h"
#include "kms_message_private.h"
#include "kms_message/kms_b64.h"
#include "kms_request_str.h"


kms_request_t *
kms_decrypt_request_new (const uint8_t *ciphertext_blob,
                         size_t len,
                         const kms_request_opt_t *opt)
{
   kms_request_t *request;
   size_t b64_len;
   char *b64 = NULL;
   kms_request_str_t *payload = NULL;

   request = kms_request_new ("POST", "/", opt);
   if (kms_request_get_error (request)) {
      goto done;
   }

   if (!(kms_request_add_header_field (
            request, "Content-Type", "application/x-amz-json-1.1") &&
         kms_request_add_header_field (
            request, "X-Amz-Target", "TrentService.Decrypt"))) {
      goto done;
   }

   b64_len = (len / 3 + 1) * 4 + 1;

   if (!(b64 = malloc (b64_len))) {
      KMS_ERROR (request,
                 "Could not allocate %d bytes for base64-encoding payload",
                 (int) b64_len);
      goto done;
   }

   if (kms_message_b64_ntop (ciphertext_blob, len, b64, b64_len) == -1) {
      KMS_ERROR (request, "Could not base64-encode ciphertext blob");
      goto done;
   }

   payload = kms_request_str_new ();
   kms_request_str_appendf (payload, "{\"CiphertextBlob\": \"%s\"}", b64);
   if (!kms_request_append_payload (request, payload->str, payload->len)) {
      KMS_ERROR (request, "Could not append payload");
      goto done;
   }

done:
   free (b64);
   kms_request_str_destroy (payload);

   return request;
}

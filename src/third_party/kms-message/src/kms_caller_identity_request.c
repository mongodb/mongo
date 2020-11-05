/*
 * Copyright 2019-present MongoDB, Inc.
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
kms_caller_identity_request_new (const kms_request_opt_t *opt)
{
   kms_request_t *request;
   kms_request_str_t *payload = NULL;

   request = kms_request_new ("POST", "/", opt);
   if (kms_request_get_error (request)) {
      goto done;
   }

   if (!(kms_request_add_header_field (
          request, "Content-Type", "application/x-www-form-urlencoded"))) {
      goto done;
   }

   payload = kms_request_str_new ();
   kms_request_str_appendf (payload,
                            "Action=GetCallerIdentity&Version=2011-06-15");
   if (!kms_request_append_payload (request, payload->str, payload->len)) {
      KMS_ERROR (request, "Could not append payload");
      goto done;
   }

done:
   kms_request_str_destroy (payload);

   return request;
}

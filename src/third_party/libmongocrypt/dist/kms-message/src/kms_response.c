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
#include "kms_request_str.h"

void
kms_response_destroy (kms_response_t *response)
{
   if (response == NULL) {
      return;
   }

   free (response->kmip.data);
   kms_kv_list_destroy (response->headers);
   kms_request_str_destroy (response->body);
   free (response);
}

const char *
kms_response_get_body (kms_response_t *response, size_t *len)
{
   if (len) {
      *len = response->body->len;
   }
   return response->body->str;
}

int
kms_response_get_status (kms_response_t *response)
{
   return response->status;
}

const char *
kms_response_get_error (const kms_response_t *response)
{
   return response->failed ? response->error : NULL;
}

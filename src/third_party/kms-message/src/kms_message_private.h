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

#ifndef KMS_MESSAGE_PRIVATE_H
#define KMS_MESSAGE_PRIVATE_H

#include <stdio.h>

#include "kms_message/kms_message.h"
#include "kms_request_str.h"
#include "kms_kv_list.h"
#include "kms_crypto.h"

struct _kms_request_t {
   char error[512];
   bool failed;
   bool finalized;
   kms_request_str_t *region;
   kms_request_str_t *service;
   kms_request_str_t *access_key_id;
   kms_request_str_t *secret_key;
   kms_request_str_t *method;
   kms_request_str_t *path;
   kms_request_str_t *query;
   kms_request_str_t *payload;
   kms_request_str_t *datetime;
   kms_request_str_t *date;
   kms_kv_list_t *query_params;
   kms_kv_list_t *header_fields;
   /* turn off for tests only, not in public kms_request_opt_t API */
   bool auto_content_length;
   _kms_crypto_t crypto;
};

struct _kms_response_t {
   int status;
   kms_kv_list_t *headers;
   kms_request_str_t *body;
};

typedef enum {
   PARSING_STATUS_LINE,
   PARSING_HEADER,
   PARSING_BODY,
   PARSING_DONE
} kms_response_parser_state_t;

struct _kms_response_parser_t {
   char error[512];
   bool failed;
   kms_response_t *response;
   kms_request_str_t *raw_response;
   int content_length;
   int start; /* start of the current thing getting parsed. */
   kms_response_parser_state_t state;
};

#define CHECK_FAILED         \
   do {                      \
      if (request->failed) { \
         return false;       \
      }                      \
   } while (0)

void
set_error (char *error, size_t size, const char *fmt, ...);

#define KMS_ERROR(obj, ...)                                     \
   do {                                                         \
      obj->failed = true;                                       \
      set_error (obj->error, sizeof (obj->error), __VA_ARGS__); \
   } while (0)

#define KMS_ASSERT(stmt) \
if (!(stmt)) { \
    fprintf (stderr, "%s failed\n", #stmt); \
    abort (); \
}

#endif /* KMS_MESSAGE_PRIVATE_H */

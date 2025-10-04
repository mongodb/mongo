/*
 * Copyright 2021-present MongoDB, Inc.
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

#include "kms_message/kms_kmip_response_parser.h"

#include "kms_endian_private.h"
#include "kms_kmip_response_parser_private.h"
#include "kms_message_private.h"
#include "kms_request_str.h"

struct _kms_kmip_response_parser_t {
   uint32_t first_len;
   uint32_t bytes_fed;
   kms_request_str_t *buf;
   bool failed;
   char error[512];
};

/* FIRST_LENGTH_OFFSET is the offset of the first Length in a TTLV sequence. The
 * sequence is: Tag (3 bytes), Type (1 byte), Length (4 bytes), Value (Length
 * bytes). */
static const uint32_t FIRST_LENGTH_OFFSET = 4;

/* _parser_destroy destroys the fields of parser, but not the parser itself. */
static void
_parser_destroy (kms_kmip_response_parser_t *parser)
{
   kms_request_str_destroy (parser->buf);
}

/* _parser_init initializes the members of parser. */
static void
_parser_init (kms_kmip_response_parser_t *parser)
{
   memset (parser, 0, sizeof (*parser));
   parser->buf = kms_request_str_new ();
}

kms_response_parser_t *
kms_kmip_response_parser_new (void *reserved)
{
   kms_response_parser_t *parser = kms_response_parser_new ();

   parser->kmip = malloc (sizeof (kms_kmip_response_parser_t));
   _parser_init (parser->kmip);

   return parser;
}

int32_t
kms_kmip_response_parser_wants_bytes (const kms_kmip_response_parser_t *parser,
                                      int32_t max)
{
   int32_t wants_bytes;
   uint32_t total_len;
   uint32_t want_bytes_pending;
   if (parser->bytes_fed < KMS_KMIP_RESPONSE_PARSER_FIRST_LENGTH) {
      wants_bytes =
         KMS_KMIP_RESPONSE_PARSER_FIRST_LENGTH - (int32_t) parser->bytes_fed;
   } else {
      KMS_ASSERT (parser->first_len <=
                  UINT32_MAX - KMS_KMIP_RESPONSE_PARSER_FIRST_LENGTH);
      total_len = parser->first_len + KMS_KMIP_RESPONSE_PARSER_FIRST_LENGTH;
      KMS_ASSERT (total_len >= parser->bytes_fed);
      want_bytes_pending = total_len - parser->bytes_fed;
      KMS_ASSERT (want_bytes_pending <= (uint32_t) INT32_MAX);
      wants_bytes = (int32_t) want_bytes_pending;
   }
   if (max < wants_bytes) {
      return max;
   }
   return wants_bytes;
}

bool
kms_kmip_response_parser_feed (kms_kmip_response_parser_t *parser,
                               const uint8_t *buf,
                               uint32_t len)
{
   kms_request_str_append_chars (parser->buf, (char *) buf, len);
   parser->bytes_fed += len;

   if (parser->first_len > 0) {
      if (parser->bytes_fed >
          parser->first_len + KMS_KMIP_RESPONSE_PARSER_FIRST_LENGTH) {
         KMS_ERROR (parser, "KMIP parser was fed too much data");
         return false;
      }
   } else if (parser->first_len == 0 &&
              parser->bytes_fed >= KMS_KMIP_RESPONSE_PARSER_FIRST_LENGTH) {
      uint32_t temp;
      memcpy (&temp, parser->buf->str + FIRST_LENGTH_OFFSET, sizeof (uint32_t));
      parser->first_len = KMS_UINT32_FROM_BE (temp);
   }
   return true;
}

kms_response_t *
kms_kmip_response_parser_get_response (kms_kmip_response_parser_t *parser)
{
   kms_response_t *res;

   if (kms_kmip_response_parser_wants_bytes (parser, 1) != 0) {
      KMS_ERROR (parser, "KMIP parser does not have a complete message");
      return NULL;
   }

   res = calloc (1, sizeof (kms_response_t));
   res->provider = KMS_REQUEST_PROVIDER_KMIP;
   res->kmip.len = (uint32_t) parser->buf->len;
   res->kmip.data = (uint8_t *) kms_request_str_detach (parser->buf);
   parser->buf = NULL;

   /* Reinitialize for reuse. */
   _parser_destroy (parser);
   _parser_init (parser);
   return res;
}

const char *
kms_kmip_response_parser_error (const kms_kmip_response_parser_t *parser)
{
   return parser->failed ? parser->error : NULL;
}

void
kms_kmip_response_parser_destroy (kms_kmip_response_parser_t *parser)
{
   if (!parser) {
      return;
   }

   _parser_destroy (parser);
   free (parser);
}

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

#ifndef KMS_KMIP_RESPONSE_PARSER_PRIVATE_H
#define KMS_KMIP_RESPONSE_PARSER_PRIVATE_H

#include "kms_message/kms_response.h"

#include <stdbool.h>
#include <stdint.h>

/* kms_kmip_response_parser_t is a private type used for parsing a KMIP
 * response. */
typedef struct _kms_kmip_response_parser_t kms_kmip_response_parser_t;

/* KMS_KMIP_RESPONSE_PARSER_FIRST_LENGTH is the number of bytes needed by the
 * parser to determine the total length of the message being parsed.
 * It includes the first Length in a TTLV sequence. The sequence is: Tag (3
 * bytes), Type (1 byte), Length (4 bytes), Value (Length bytes). */
#define KMS_KMIP_RESPONSE_PARSER_FIRST_LENGTH 8

int32_t
kms_kmip_response_parser_wants_bytes (const kms_kmip_response_parser_t *parser,
                                      int32_t max);

bool
kms_kmip_response_parser_feed (kms_kmip_response_parser_t *parser,
                               const uint8_t *buf,
                               uint32_t len);

/* kms_kmip_response_parser_get_response returns a kms_response_t and resets the
 * parser. */
kms_response_t *
kms_kmip_response_parser_get_response (kms_kmip_response_parser_t *parser);

void
kms_kmip_response_parser_destroy (kms_kmip_response_parser_t *parser);

const char *
kms_kmip_response_parser_error (const kms_kmip_response_parser_t *parser);

#endif /* KMS_KMIP_RESPONSE_PARSER_PRIVATE_H */

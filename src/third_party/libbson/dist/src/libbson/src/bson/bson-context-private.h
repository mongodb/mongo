/*
 * Copyright 2009-present MongoDB, Inc.
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

#include <bson/bson-prelude.h>


#ifndef BSON_CONTEXT_PRIVATE_H
#define BSON_CONTEXT_PRIVATE_H


#include <bson/bson-context.h>
#include "common-thread-private.h"


BSON_BEGIN_DECLS


enum {
   BSON_OID_RANDOMESS_OFFSET = 4,
   BSON_OID_RANDOMNESS_SIZE = 5,
   BSON_OID_SEQ32_OFFSET = 9,
   BSON_OID_SEQ32_SIZE = 3,
   BSON_OID_SEQ64_OFFSET = 4,
   BSON_OID_SEQ64_SIZE = 8
};

struct _bson_context_t {
   /* flags are defined in bson_context_flags_t */
   int flags;
   uint32_t seq32;
   uint64_t seq64;
   uint8_t randomness[BSON_OID_RANDOMNESS_SIZE];
   uint64_t pid;
};

/**
 * @brief Insert the context's randomness data into the given OID
 *
 * @param context A context for some random data
 * @param oid The OID to update.
 */
void
_bson_context_set_oid_rand (bson_context_t *context, bson_oid_t *oid);

/**
 * @brief Insert the context's sequence counter into the given OID. Increments
 * the context's sequence counter.
 *
 * @param context The context with the counter to get+update
 * @param oid The OID to modify
 */
void
_bson_context_set_oid_seq32 (bson_context_t *context, bson_oid_t *oid);

/**
 * @brief Write a 64-bit counter from the given context into the OID. Increments
 * the context's sequence counter.
 *
 * @param context The context with the counter to get+update
 * @param oid The OID to modify
 *
 * @note Only used by the deprecated @ref bson_oid_init_sequence
 */
void
_bson_context_set_oid_seq64 (bson_context_t *context, bson_oid_t *oid);


BSON_END_DECLS


#endif /* BSON_CONTEXT_PRIVATE_H */

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

#ifndef BSON_BSON_T_H_INCLUDED
#define BSON_BSON_T_H_INCLUDED

#include <bson/macros.h>

#include <stdint.h>

/**
 * bson_t:
 *
 * This structure manages a buffer whose contents are a properly formatted
 * BSON document. You may perform various transforms on the BSON documents.
 * Additionally, it can be iterated over using bson_iter_t.
 *
 * See bson_iter_init() for iterating the contents of a bson_t.
 *
 * When building a bson_t structure using the various append functions,
 * memory allocations may occur. That is performed using power of two
 * allocations and realloc().
 *
 * See http://bsonspec.org for the BSON document spec.
 *
 * This structure is meant to fit in two sequential 64-byte cachelines.
 */
BSON_ALIGNED_BEGIN(BSON_ALIGN_OF_PTR) typedef struct _bson_t {
   uint32_t flags;       /* Internal flags for the bson_t. */
   uint32_t len;         /* Length of BSON data. */
   uint8_t padding[120]; /* Padding for stack allocation. */
} bson_t BSON_ALIGNED_END(BSON_ALIGN_OF_PTR);

/**
 * BSON_INITIALIZER:
 *
 * This macro can be used to initialize a #bson_t structure on the stack
 * without calling bson_init().
 *
 * |[
 * bson_t b = BSON_INITIALIZER;
 * ]|
 */
#define BSON_INITIALIZER {3, 5, {5}}

BSON_STATIC_ASSERT2(bson_t, sizeof(bson_t) == 128);

#endif // BSON_BSON_T_H_INCLUDED

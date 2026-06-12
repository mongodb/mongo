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

#ifndef BSON_VECTOR_PRIVATE_H
#define BSON_VECTOR_PRIVATE_H

#include <bson/bson-vector.h> // IWYU pragma: export

//

#include <bson/compat.h>
#include <bson/macros.h>

BSON_BEGIN_DECLS


typedef enum {
   BSON_VECTOR_ELEMENT_SIGNED_INT = 0,
   BSON_VECTOR_ELEMENT_UNSIGNED_INT = 1,
   BSON_VECTOR_ELEMENT_FLOAT = 2,
} bson_vector_element_type_t;

typedef enum {
   BSON_VECTOR_ELEMENT_1_BIT = 0,
   BSON_VECTOR_ELEMENT_8_BITS = 3,
   BSON_VECTOR_ELEMENT_32_BITS = 7,
} bson_vector_element_size_t;


static BSON_INLINE uint8_t
bson_vector_header_byte_0(bson_vector_element_type_t element_type, bson_vector_element_size_t element_size)
{
   BSON_ASSERT((unsigned)element_type <= 0x0f);
   BSON_ASSERT((unsigned)element_size <= 0x0f);
   return (uint8_t)(((unsigned)element_type << 4) | (unsigned)element_size);
}

// See also `bson_vector_padding_from_header_byte_1` defined in <bson/bson-vector.h> for use by public inline functions.
static BSON_INLINE uint8_t
bson_vector_header_byte_1(size_t padding)
{
   BSON_ASSERT(padding <= 7);
   return (uint8_t)padding;
}


BSON_END_DECLS

#endif /* BSON_VECTOR_PRIVATE_H */

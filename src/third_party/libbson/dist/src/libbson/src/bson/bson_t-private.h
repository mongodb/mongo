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


#ifndef BSON_PRIVATE_H
#define BSON_PRIVATE_H

#include <bson/bson_t.h> // IWYU pragma: export

//

#include <bson/bson-types.h>
#include <bson/macros.h>
#include <bson/memory.h>


BSON_BEGIN_DECLS


typedef enum {
   BSON_FLAG_NONE = 0,
   BSON_FLAG_INLINE_DATA = (1 << 0),    // Set if BSON data is embedded in `bson_t`.
   BSON_FLAG_NO_FREE_OBJECT = (1 << 1), // Set if `bson_destroy` should not free `bson_t` object.
   BSON_FLAG_RDONLY = (1 << 2),
   BSON_FLAG_CHILD = (1 << 3),
   BSON_FLAG_IN_CHILD = (1 << 4),
   BSON_FLAG_NO_FREE_DATA = (1 << 5), // Set if `bson_destroy` should not free BSON data.
} bson_flags_t;


#define BSON_INLINE_DATA_SIZE 120


BSON_ALIGNED_BEGIN(BSON_ALIGN_OF_PTR)
typedef struct {
   bson_flags_t flags;
   uint32_t len;
   uint8_t data[BSON_INLINE_DATA_SIZE];
} bson_impl_inline_t BSON_ALIGNED_END(BSON_ALIGN_OF_PTR);


BSON_STATIC_ASSERT2(impl_inline_t, sizeof(bson_impl_inline_t) == 128);

typedef struct {
   bson_flags_t flags; /* flags describing the bson_t */
   /* len is part of the public bson_t declaration. It is not
    * exposed through an accessor function. Plus, it's redundant since
    * BSON self describes the length in the first four bytes of the
    * buffer. */
   uint32_t len; /* length of bson document in bytes */
   /**
    * @brief Pointer to a parent document object if we are a child of some other
    * document, otherwise a null pointer.
    */
   bson_t *parent;
   uint32_t depth; /* Subdocument depth. */
   /**
    * @brief If non-null, this pointer refers to the pointer to a buffer that is not directly owned
    * by this `bson_t`, but may still manipulated/managed by this `bson_t`.
    *
    * If this pointer is null, then the data buffer is in `own_buffer`
    */
   uint8_t **indirect_buffer;
   size_t *indirect_buflen;
   /**
    * @brief The offset (in bytes) to the beginning of the document within the data buffer.
    */
   size_t offset;
   /**
    * @brief Data buffer that is managed directly by this `bson_t`. This is not used if `indirect_buffer`
    * is non-null.
    */
   uint8_t *own_buffer;
   size_t own_buflen;
   bson_realloc_func realloc; /* our realloc implementation */
   void *realloc_func_ctx;    /* context for our realloc func */
} bson_impl_alloc_t;


BSON_STATIC_ASSERT2(impl_alloc_t, sizeof(bson_impl_alloc_t) <= 128);

// Ensure both `bson_t` implementations have the same alignment requirement:
BSON_STATIC_ASSERT2(impls_match_alignment, BSON_ALIGNOF(bson_impl_inline_t) == BSON_ALIGNOF(bson_impl_alloc_t));
// Ensure `bson_t` has same alignment requirement as implementations:
BSON_STATIC_ASSERT2(impls_match_alignment, BSON_ALIGNOF(bson_t) == BSON_ALIGNOF(bson_impl_alloc_t));


BSON_END_DECLS


#endif /* BSON_PRIVATE_H */

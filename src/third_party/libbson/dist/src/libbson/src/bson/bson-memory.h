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


#ifndef BSON_MEMORY_H
#define BSON_MEMORY_H


#include <bson/bson-macros.h>
#include <bson/bson-types.h>


BSON_BEGIN_DECLS


typedef void *(*bson_realloc_func) (void *mem, size_t num_bytes, void *ctx);


typedef struct _bson_mem_vtable_t {
   void *(*malloc) (size_t num_bytes);
   void *(*calloc) (size_t n_members, size_t num_bytes);
   void *(*realloc) (void *mem, size_t num_bytes);
   void (*free) (void *mem);
   void *(*aligned_alloc) (size_t alignment, size_t num_bytes);
   void *padding[3];
} bson_mem_vtable_t;


BSON_EXPORT (void)
bson_mem_set_vtable (const bson_mem_vtable_t *vtable);
BSON_EXPORT (void)
bson_mem_restore_vtable (void);
BSON_EXPORT (void *)
bson_malloc (size_t num_bytes);
BSON_EXPORT (void *)
bson_malloc0 (size_t num_bytes);
BSON_EXPORT (void *)
bson_aligned_alloc (size_t alignment, size_t num_bytes);
BSON_EXPORT (void *)
bson_aligned_alloc0 (size_t alignment, size_t num_bytes);
BSON_EXPORT (void *)
bson_realloc (void *mem, size_t num_bytes);
BSON_EXPORT (void *)
bson_realloc_ctx (void *mem, size_t num_bytes, void *ctx);
BSON_EXPORT (void)
bson_free (void *mem);
BSON_EXPORT (void)
bson_zero_free (void *mem, size_t size);


#define BSON_ALIGNED_ALLOC(T) ((T *) (bson_aligned_alloc (BSON_ALIGNOF (T), sizeof (T))))
#define BSON_ALIGNED_ALLOC0(T) ((T *) (bson_aligned_alloc0 (BSON_ALIGNOF (T), sizeof (T))))


BSON_END_DECLS


#endif /* BSON_MEMORY_H */

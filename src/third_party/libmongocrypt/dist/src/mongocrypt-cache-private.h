/*
 * Copyright 2019-present MongoDB, Inc.
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
#ifndef MONGOCRYPT_CACHE_PRIVATE
#define MONGOCRYPT_CACHE_PRIVATE

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-mutex-private.h"
#include "mongocrypt-status-private.h"

#define CACHE_EXPIRATION_MS 60000

/* A generic simple cache.
 * To avoid overusing the names "key" or "id", the cache contains
 * "attribute-value" pairs.
 * https://en.wikipedia.org/wiki/Attribute%E2%80%93value_pair
 */
typedef bool (*cache_compare_fn) (void *thing_a, void *thing_b, int *out);
typedef void (*cache_destroy_fn) (void *thing);
typedef void *(*cache_copy_fn) (void *thing);
typedef void (*cache_dump_fn) (void *thing);

typedef struct __mongocrypt_cache_pair_t {
   void *attr;
   void *value;
   struct __mongocrypt_cache_pair_t *next;
   int64_t last_updated;
} _mongocrypt_cache_pair_t;

typedef struct {
   cache_dump_fn dump_attr;
   cache_compare_fn cmp_attr;
   cache_copy_fn copy_attr;
   cache_destroy_fn destroy_attr;
   cache_copy_fn copy_value;
   cache_destroy_fn destroy_value;
   _mongocrypt_cache_pair_t *pair;
   mongocrypt_mutex_t mutex; /* global lock of cache. */
   uint64_t expiration;
} _mongocrypt_cache_t;


/* Attempt to get an entry.
 * Returns boolean indicating success.
 */
bool
_mongocrypt_cache_get (_mongocrypt_cache_t *cache,
                       void *attr,
                       void **value) MONGOCRYPT_WARN_UNUSED_RESULT;

bool
_mongocrypt_cache_add_copy (_mongocrypt_cache_t *cache,
                            void *attr,
                            void *value,
                            mongocrypt_status_t *status)
   MONGOCRYPT_WARN_UNUSED_RESULT;


/* Steals the value instead of copying. Caller relinquishes value when calling.
 */
bool
_mongocrypt_cache_add_stolen (_mongocrypt_cache_t *cache,
                              void *attr,
                              void *value,
                              mongocrypt_status_t *status)
   MONGOCRYPT_WARN_UNUSED_RESULT;


void
_mongocrypt_cache_cleanup (_mongocrypt_cache_t *cache);

/* A helper debug function to dump the state of the cache. */
void
_mongocrypt_cache_dump (_mongocrypt_cache_t *cache);

/* Tests may override the default expiration */
void
_mongocrypt_cache_set_expiration (_mongocrypt_cache_t *cache, uint64_t milli);

uint32_t
_mongocrypt_cache_num_entries (_mongocrypt_cache_t *cache);


#endif /* MONGOCRYPT_CACHE_PRIVATE */
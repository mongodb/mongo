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

#include "mongocrypt-cache-private.h"

/* The collinfo cache.
 *
 * Attribute is a null terminated namespace.
 * Value is a collection info doc (response to listCollections).
 */

static bool _cmp_attr(void *a, void *b, int *out) {
    BSON_ASSERT_PARAM(a);
    BSON_ASSERT_PARAM(b);
    BSON_ASSERT_PARAM(out);

    *out = strcmp((char *)a, (char *)b);
    return true;
}

static void *_copy_attr(void *ns) {
    BSON_ASSERT_PARAM(ns);

    return bson_strdup((const char *)ns);
}

static void _destroy_attr(void *ns) {
    bson_free(ns);
}

static void *_copy_value(void *bson) {
    BSON_ASSERT_PARAM(bson);

    return bson_copy(bson);
}

static void _destroy_value(void *bson) {
    bson_destroy(bson);
}

void _mongocrypt_cache_collinfo_init(_mongocrypt_cache_t *cache) {
    BSON_ASSERT_PARAM(cache);

    cache->cmp_attr = _cmp_attr;
    cache->copy_attr = _copy_attr;
    cache->destroy_attr = _destroy_attr;
    cache->copy_value = _copy_value;
    cache->destroy_value = _destroy_value;
    _mongocrypt_mutex_init(&cache->mutex);
    cache->pair = NULL;
    cache->expiration = CACHE_EXPIRATION_MS;
}

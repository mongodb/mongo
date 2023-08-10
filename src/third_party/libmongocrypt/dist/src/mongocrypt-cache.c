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

#include "mongocrypt-private.h"

/* Did the cache pair expire? Caller must hold lock. */
static bool _pair_expired(_mongocrypt_cache_t *cache, _mongocrypt_cache_pair_t *pair) {
    int64_t current;

    BSON_ASSERT_PARAM(cache);
    BSON_ASSERT_PARAM(pair);

    current = bson_get_monotonic_time() / 1000;
    BSON_ASSERT(current >= INT64_MIN + pair->last_updated);
    BSON_ASSERT(cache->expiration <= INT64_MAX);
    return (current - pair->last_updated) > (int64_t)cache->expiration;
}

/* Return the pair after the one being destroyed. */
static _mongocrypt_cache_pair_t *
_destroy_pair(_mongocrypt_cache_t *cache, _mongocrypt_cache_pair_t *prev, _mongocrypt_cache_pair_t *pair) {
    _mongocrypt_cache_pair_t *tmp;

    BSON_ASSERT_PARAM(cache);
    /* prev is checked before being used, so it can be NULL */
    BSON_ASSERT_PARAM(pair);

    tmp = pair->next;

    /* Unlink */
    if (!prev) {
        cache->pair = cache->pair->next;
    } else {
        prev->next = pair->next;
    }

    /* Destroy pair */
    cache->destroy_attr(pair->attr);
    cache->destroy_value(pair->value);
    bson_free(pair);

    return tmp;
}

/* Caller must hold mutex. */
static void _mongocrypt_cache_evict(_mongocrypt_cache_t *cache) {
    _mongocrypt_cache_pair_t *pair, *prev;

    BSON_ASSERT_PARAM(cache);

    prev = NULL;
    pair = cache->pair;
    while (pair) {
        if (_pair_expired(cache, pair)) {
            pair = _destroy_pair(cache, prev, pair);
            continue;
        }
        prev = pair;
        pair = pair->next;
    }
}

/* Caller must hold mutex. */
static bool _mongocrypt_remove_matches(_mongocrypt_cache_t *cache, void *attr) {
    _mongocrypt_cache_pair_t *pair, *prev;

    BSON_ASSERT_PARAM(cache);
    BSON_ASSERT_PARAM(attr);

    prev = NULL;
    pair = cache->pair;
    while (pair) {
        int res;

        if (!cache->cmp_attr(pair->attr, attr, &res)) {
            return false;
        }

        if (0 == res) {
            pair = _destroy_pair(cache, prev, pair);
            continue;
        }
        prev = pair;
        pair = pair->next;
    }

    return true;
}

void _mongocrypt_cache_set_expiration(_mongocrypt_cache_t *cache, uint64_t milli) {
    BSON_ASSERT_PARAM(cache);

    cache->expiration = milli;
}

/* caller must hold lock. */
static bool _find_pair(_mongocrypt_cache_t *cache, void *attr, _mongocrypt_cache_pair_t **out) {
    _mongocrypt_cache_pair_t *pair;

    BSON_ASSERT_PARAM(cache);
    BSON_ASSERT_PARAM(attr);
    BSON_ASSERT_PARAM(out);

    *out = NULL;

    pair = cache->pair;
    while (pair) {
        int res;
        /* TODO: this is a naive O(n) lookup. Consider optimizing
           with a hash map (possibly vendor one). */
        if (!cache->cmp_attr(pair->attr, attr, &res)) {
            return false;
        }

        if (res == 0) {
            *out = pair;
            return true;
        }
        pair = pair->next;
    }
    return true;
}

/* Create a new pair on linked list. Caller must hold lock. */
static _mongocrypt_cache_pair_t *_pair_new(_mongocrypt_cache_t *cache, void *attr) {
    _mongocrypt_cache_pair_t *pair;

    BSON_ASSERT_PARAM(cache);
    BSON_ASSERT_PARAM(attr);

    pair = bson_malloc0(sizeof(_mongocrypt_cache_pair_t));
    BSON_ASSERT(pair);

    pair->attr = cache->copy_attr(attr);
    /* add rest of values. */
    pair->next = cache->pair;
    pair->last_updated = bson_get_monotonic_time() / 1000;
    cache->pair = pair;
    return pair;
}

/* Caller must hold lock. */
static void _cache_pair_destroy(_mongocrypt_cache_t *cache, _mongocrypt_cache_pair_t *pair) {
    BSON_ASSERT_PARAM(cache);
    BSON_ASSERT_PARAM(pair);

    cache->destroy_attr(pair->attr);
    cache->destroy_value(pair->value);
    bson_free(pair);
}

bool _mongocrypt_cache_get(_mongocrypt_cache_t *cache,
                           void *attr, /* attr of cache item */
                           void **value /* copied to. */) {
    _mongocrypt_cache_pair_t *match;

    BSON_ASSERT_PARAM(cache);
    BSON_ASSERT_PARAM(attr);
    BSON_ASSERT_PARAM(value);

    *value = NULL;

    _mongocrypt_mutex_lock(&cache->mutex);
    /* TODO CDRIVER-3120: optimize the eviction algorithm to avoid unnecessary
     * O(n) traversal */
    _mongocrypt_cache_evict(cache);
    if (!_find_pair(cache, attr, &match)) {
        _mongocrypt_mutex_unlock(&cache->mutex);
        return false;
    }

    if (match) {
        *value = cache->copy_value(match->value);
    }
    _mongocrypt_mutex_unlock(&cache->mutex);
    return true;
}

static bool
_cache_add(_mongocrypt_cache_t *cache, void *attr, void *value, mongocrypt_status_t *status, bool steal_value) {
    _mongocrypt_cache_pair_t *pair;

    BSON_ASSERT_PARAM(cache);
    BSON_ASSERT_PARAM(attr);
    BSON_ASSERT_PARAM(value);

    _mongocrypt_mutex_lock(&cache->mutex);
    _mongocrypt_cache_evict(cache);
    if (!_mongocrypt_remove_matches(cache, attr)) {
        CLIENT_ERR("error removing from cache");
        _mongocrypt_mutex_unlock(&cache->mutex);
        return false;
    }

    pair = _pair_new(cache, attr);

    if (steal_value) {
        pair->value = value;
    } else {
        pair->value = cache->copy_value(value);
    }
    _mongocrypt_mutex_unlock(&cache->mutex);
    return true;
}

bool _mongocrypt_cache_add_copy(_mongocrypt_cache_t *cache, void *attr, void *value, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(cache);
    BSON_ASSERT_PARAM(attr);
    BSON_ASSERT_PARAM(value);

    return _cache_add(cache, attr, value, status, false);
}

bool _mongocrypt_cache_add_stolen(_mongocrypt_cache_t *cache, void *attr, void *value, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(cache);
    BSON_ASSERT_PARAM(attr);
    BSON_ASSERT_PARAM(value);

    return _cache_add(cache, attr, value, status, true);
}

void _mongocrypt_cache_cleanup(_mongocrypt_cache_t *cache) {
    _mongocrypt_cache_pair_t *pair, *tmp;

    if (!cache) {
        return;
    }

    pair = cache->pair;
    while (pair) {
        tmp = pair->next;
        _cache_pair_destroy(cache, pair);
        pair = tmp;
    }
}

/* Print the contents of the cache (for debugging purposes) */
void _mongocrypt_cache_dump(_mongocrypt_cache_t *cache) {
    _mongocrypt_cache_pair_t *pair;
    int count;

    BSON_ASSERT_PARAM(cache);

    _mongocrypt_mutex_lock(&cache->mutex);
    count = 0;
    for (pair = cache->pair; pair != NULL; pair = pair->next) {
        /* don't check that int64_t fits in int, since this is only diagnostic */
        printf("entry:%d last_updated:%d\n", count, (int)pair->last_updated);
        if (cache->dump_attr) {
            printf("- attr:");
            cache->dump_attr(pair->attr);
        }
        count++;
    }

    _mongocrypt_mutex_unlock(&cache->mutex);
}

uint32_t _mongocrypt_cache_num_entries(_mongocrypt_cache_t *cache) {
    _mongocrypt_cache_pair_t *pair;
    uint32_t count;

    BSON_ASSERT_PARAM(cache);

    _mongocrypt_mutex_lock(&cache->mutex);
    count = 0;
    for (pair = cache->pair; pair != NULL; pair = pair->next) {
        count++;
    }

    _mongocrypt_mutex_unlock(&cache->mutex);
    return count;
}

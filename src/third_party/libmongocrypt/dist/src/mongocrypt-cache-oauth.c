/*
 * Copyright 2020-present MongoDB, Inc.
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

#include "mongocrypt-cache-oauth-private.h"

#include "mongocrypt-private.h"

/* How long before the reported "expires_in" time cache entries get evicted.
 * This is intended to prevent use of an oauth token too close to the expiration
 * time.
 */
#define MONGOCRYPT_OAUTH_CACHE_EVICTION_PERIOD_US 5000 * 1000

_mongocrypt_cache_oauth_t *_mongocrypt_cache_oauth_new(void) {
    _mongocrypt_cache_oauth_t *cache;

    cache = bson_malloc0(sizeof(_mongocrypt_cache_oauth_t));
    _mongocrypt_mutex_init(&cache->mutex);
    return cache;
}

void _mongocrypt_cache_oauth_destroy(_mongocrypt_cache_oauth_t *cache) {
    BSON_ASSERT_PARAM(cache);

    _mongocrypt_mutex_cleanup(&cache->mutex);
    bson_destroy(cache->entry);
    bson_free(cache->access_token);
    bson_free(cache);
}

bool _mongocrypt_cache_oauth_add(_mongocrypt_cache_oauth_t *cache,
                                 bson_t *oauth_response,
                                 mongocrypt_status_t *status) {
    bson_iter_t iter;
    int64_t expiration_time_us;
    int64_t cache_time_us;
    int64_t expires_in_s;
    int64_t expires_in_us;
    const char *access_token;

    BSON_ASSERT_PARAM(cache);
    BSON_ASSERT_PARAM(oauth_response);

    /* The OAuth spec strongly implies that the value of expires_in is positive,
     * so the overflow checks in this function don't consider negative values. */
    if (!bson_iter_init_find(&iter, oauth_response, "expires_in") || !BSON_ITER_HOLDS_INT(&iter)) {
        CLIENT_ERR("OAuth response invalid, no 'expires_in' field.");
        return false;
    }
    cache_time_us = bson_get_monotonic_time();
    expires_in_s = bson_iter_as_int64(&iter);
    BSON_ASSERT(expires_in_s <= INT64_MAX / 1000 / 1000);
    expires_in_us = expires_in_s * 1000 * 1000;
    BSON_ASSERT(expires_in_us <= INT64_MAX - cache_time_us
                && expires_in_us + cache_time_us > MONGOCRYPT_OAUTH_CACHE_EVICTION_PERIOD_US);
    expiration_time_us = expires_in_us + cache_time_us - MONGOCRYPT_OAUTH_CACHE_EVICTION_PERIOD_US;

    if (!bson_iter_init_find(&iter, oauth_response, "access_token") || !BSON_ITER_HOLDS_UTF8(&iter)) {
        CLIENT_ERR("OAuth response invalid, no 'access_token' field.");
        return false;
    }
    access_token = bson_iter_utf8(&iter, NULL);

    _mongocrypt_mutex_lock(&cache->mutex);
    if (expiration_time_us > cache->expiration_time_us) {
        bson_destroy(cache->entry);
        cache->entry = bson_copy(oauth_response);
        cache->expiration_time_us = expiration_time_us;
        bson_free(cache->access_token);
        cache->access_token = bson_strdup(access_token);
    }
    _mongocrypt_mutex_unlock(&cache->mutex);
    return true;
}

/* Returns a copy of the base64 encoded oauth token, or NULL if nothing is
 * cached. */
char *_mongocrypt_cache_oauth_get(_mongocrypt_cache_oauth_t *cache) {
    char *access_token;

    BSON_ASSERT_PARAM(cache);

    _mongocrypt_mutex_lock(&cache->mutex);
    if (!cache->entry) {
        _mongocrypt_mutex_unlock(&cache->mutex);
        return NULL;
    }

    if (bson_get_monotonic_time() >= cache->expiration_time_us) {
        bson_destroy(cache->entry);
        cache->entry = NULL;
        cache->expiration_time_us = 0;
        _mongocrypt_mutex_unlock(&cache->mutex);
        return NULL;
    }

    access_token = bson_strdup(cache->access_token);
    _mongocrypt_mutex_unlock(&cache->mutex);

    return access_token;
}

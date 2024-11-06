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

#include "mc-array-private.h"
#include "mongocrypt-private.h"

/* How long before the reported "expires_in" time cache entries get evicted.
 * This is intended to prevent use of an oauth token too close to the expiration
 * time.
 */
#define MONGOCRYPT_OAUTH_CACHE_EVICTION_PERIOD_US 5000 * 1000

typedef struct {
    char *kmsid;
    char *access_token;
    int64_t expiration_time_us;
} mc_mapof_kmsid_to_token_entry_t;

struct _mc_mapof_kmsid_to_token_t {
    mc_array_t entries;
    mongocrypt_mutex_t mutex; // Guards `entries`.
};

mc_mapof_kmsid_to_token_t *mc_mapof_kmsid_to_token_new(void) {
    mc_mapof_kmsid_to_token_t *k2t = bson_malloc0(sizeof(mc_mapof_kmsid_to_token_t));
    _mc_array_init(&k2t->entries, sizeof(mc_mapof_kmsid_to_token_entry_t));
    _mongocrypt_mutex_init(&k2t->mutex);
    return k2t;
}

void mc_mapof_kmsid_to_token_destroy(mc_mapof_kmsid_to_token_t *k2t) {
    if (!k2t) {
        return;
    }
    _mongocrypt_mutex_cleanup(&k2t->mutex);
    for (size_t i = 0; i < k2t->entries.len; i++) {
        mc_mapof_kmsid_to_token_entry_t k2te = _mc_array_index(&k2t->entries, mc_mapof_kmsid_to_token_entry_t, i);
        bson_free(k2te.kmsid);
        bson_free(k2te.access_token);
    }
    _mc_array_destroy(&k2t->entries);
    bson_free(k2t);
}

char *mc_mapof_kmsid_to_token_get_token(mc_mapof_kmsid_to_token_t *k2t, const char *kmsid) {
    BSON_ASSERT_PARAM(k2t);
    BSON_ASSERT_PARAM(kmsid);

    _mongocrypt_mutex_lock(&k2t->mutex);

    for (size_t i = 0; i < k2t->entries.len; i++) {
        mc_mapof_kmsid_to_token_entry_t k2te = _mc_array_index(&k2t->entries, mc_mapof_kmsid_to_token_entry_t, i);
        if (0 == strcmp(k2te.kmsid, kmsid)) {
            if (bson_get_monotonic_time() >= k2te.expiration_time_us) {
                // Expired.
                _mongocrypt_mutex_unlock(&k2t->mutex);
                return NULL;
            }
            char *access_token = bson_strdup(k2te.access_token);
            _mongocrypt_mutex_unlock(&k2t->mutex);
            return access_token;
        }
    }

    _mongocrypt_mutex_unlock(&k2t->mutex);
    return NULL;
}

bool mc_mapof_kmsid_to_token_add_response(mc_mapof_kmsid_to_token_t *k2t,
                                          const char *kmsid,
                                          bson_t *response,
                                          mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(k2t);
    BSON_ASSERT_PARAM(kmsid);
    BSON_ASSERT_PARAM(response);

    // Parse access token before locking.
    const char *access_token;
    int64_t expiration_time_us;
    {
        bson_iter_t iter;
        int64_t cache_time_us;
        int64_t expires_in_s;
        int64_t expires_in_us;

        /* The OAuth spec strongly implies that the value of expires_in is positive,
         * so the overflow checks in this function don't consider negative values. */
        if (!bson_iter_init_find(&iter, response, "expires_in") || !BSON_ITER_HOLDS_INT(&iter)) {
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

        if (!bson_iter_init_find(&iter, response, "access_token") || !BSON_ITER_HOLDS_UTF8(&iter)) {
            CLIENT_ERR("OAuth response invalid, no 'access_token' field.");
            return false;
        }
        access_token = bson_iter_utf8(&iter, NULL);
    }

    _mongocrypt_mutex_lock(&k2t->mutex);

    // Check if there is an existing entry.
    for (size_t i = 0; i < k2t->entries.len; i++) {
        mc_mapof_kmsid_to_token_entry_t *k2te = &_mc_array_index(&k2t->entries, mc_mapof_kmsid_to_token_entry_t, i);
        if (0 == strcmp(k2te->kmsid, kmsid)) {
            // Update entry.
            bson_free(k2te->access_token);
            k2te->access_token = bson_strdup(access_token);
            k2te->expiration_time_us = expiration_time_us;
            _mongocrypt_mutex_unlock(&k2t->mutex);
            return true;
        }
    }
    // Create an entry.
    mc_mapof_kmsid_to_token_entry_t to_put = {.kmsid = bson_strdup(kmsid),
                                              .access_token = bson_strdup(access_token),
                                              .expiration_time_us = expiration_time_us};
    _mc_array_append_val(&k2t->entries, to_put);
    _mongocrypt_mutex_unlock(&k2t->mutex);
    return true;
}

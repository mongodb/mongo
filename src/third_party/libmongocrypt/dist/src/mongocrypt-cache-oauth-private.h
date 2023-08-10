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

#ifndef MONGOCRYPT_CACHE_OAUTH_PRIVATE_H
#define MONGOCRYPT_CACHE_OAUTH_PRIVATE_H

#include "mongocrypt-mutex-private.h"
#include "mongocrypt-status-private.h"

typedef struct {
    bson_t *entry;
    char *access_token;
    int64_t expiration_time_us;
    mongocrypt_mutex_t mutex; /* global lock of cache. */
} _mongocrypt_cache_oauth_t;

_mongocrypt_cache_oauth_t *_mongocrypt_cache_oauth_new(void);

void _mongocrypt_cache_oauth_destroy(_mongocrypt_cache_oauth_t *cache);

bool _mongocrypt_cache_oauth_add(_mongocrypt_cache_oauth_t *cache, bson_t *oauth_response, mongocrypt_status_t *status);

/* Returns a copy of the base64 encoded oauth token, or NULL if nothing is
 * cached. */
char *_mongocrypt_cache_oauth_get(_mongocrypt_cache_oauth_t *cache);

#endif /* MONGOCRYPT_CACHE_OAUTH_PRIVATE_H */
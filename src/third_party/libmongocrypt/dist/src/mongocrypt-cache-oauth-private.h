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

// `mc_mapof_kmsid_to_token_t` maps a KMS ID (e.g. `azure` or `azure:myname`) to an OAuth token.
typedef struct _mc_mapof_kmsid_to_token_t mc_mapof_kmsid_to_token_t;

mc_mapof_kmsid_to_token_t *mc_mapof_kmsid_to_token_new(void);
void mc_mapof_kmsid_to_token_destroy(mc_mapof_kmsid_to_token_t *k2t);
// `mc_mapof_kmsid_to_token_get_token` returns a copy of the base64 encoded oauth token, or NULL.
// Thread-safe.
char *mc_mapof_kmsid_to_token_get_token(mc_mapof_kmsid_to_token_t *k2t, const char *kmsid);
// `mc_mapof_kmsid_to_token_add_response` overwrites an entry if `kms_id` exists.
// Thread-safe.
bool mc_mapof_kmsid_to_token_add_response(mc_mapof_kmsid_to_token_t *k2t,
                                          const char *kmsid,
                                          bson_t *response,
                                          mongocrypt_status_t *status);

#endif /* MONGOCRYPT_CACHE_OAUTH_PRIVATE_H */

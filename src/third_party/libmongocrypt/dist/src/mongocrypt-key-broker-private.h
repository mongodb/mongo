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

#ifndef MONGOCRYPT_KEY_BROKER_PRIVATE_H
#define MONGOCRYPT_KEY_BROKER_PRIVATE_H

#include <bson/bson.h>

#include "kms_message/kms_message.h"
#include "mongocrypt-binary-private.h"
#include "mongocrypt-cache-key-private.h"
#include "mongocrypt-cache-private.h"
#include "mongocrypt-kms-ctx-private.h"
#include "mongocrypt-opts-private.h"
#include "mongocrypt.h"

/* The key broker acts as a middle-man between an encrypt/decrypt request and
 * the key cache.
 * Each encrypt/decrypt request has one key broker. Key brokers are not shared.
 * It is responsible for:
 * - keeping track of requested keys (either by id or keyAltName)
 * - copying keys from the cache to satisfy those requests
 * - generating find cmd filters to fetch keys that aren't cached or are expired
 * - generating KMS decrypt requests on newly fetched keys
 * - adding newly fetched keys back to the cache
 *
 * Notes:
 * - any key request that is satisfied stays satisfied.
 * - keys returned from the driver are validated to not have intersecting key
 * alt names (or duplicate ids).
 * - keys fetched from the cache are not validated, because the cache is shared
 * and locking only occurs on a single fetch (so it is possible to have two keys
 * fetched from the cache that have intersecting keyAltNames but a different
 * _id, and that is not an error)
 */

/* The state of the key broker. */
typedef enum {
    /* Starting state. Accept requests for keys to be added (either by id or
       name) */
    KB_REQUESTING,
    /* Accept key documents fetched from the key vault collection. */
    KB_ADDING_DOCS,
    /* Accept any key document fetched from the key vault collection. */
    KB_ADDING_DOCS_ANY,
    /* Getting oauth token(s) from KMS providers. */
    KB_AUTHENTICATING,
    /* Accept KMS replies to decrypt key material in each key document. */
    KB_DECRYPTING_KEY_MATERIAL,
    KB_DONE,
    KB_ERROR
} key_broker_state_t;

/* Represents a single request for a key, as indicated from a response
 * from mongocryptd. */
typedef struct _key_request_t {
    /* only one of id or alt_name are set. */
    _mongocrypt_buffer_t id;
    _mongocrypt_key_alt_name_t *alt_name;
    bool satisfied; /* true if satisfied by a cache entry or a key returned. */
    struct _key_request_t *next;
} key_request_t;

/* Represents a single key supplied from the driver or cache. */
typedef struct _key_returned_t {
    _mongocrypt_key_doc_t *doc;
    _mongocrypt_buffer_t decrypted_key_material;

    mongocrypt_kms_ctx_t kms;
    bool decrypted;

    bool needs_auth;

    struct _key_returned_t *next;
} key_returned_t;

typedef struct _auth_request_t {
    mongocrypt_kms_ctx_t kms;
    bool returned;
    bool initialized;
} auth_request_t;

typedef struct {
    key_broker_state_t state;
    mongocrypt_status_t *status;
    key_request_t *key_requests;
    /* Keep keys returned from driver separate from keys returned from cache.
     * Keys returned from driver MUST not have conflicts (e.g. intersecting key
     * alt names)
     * But keys from cache MAY have conflicts since the cache is only locked for
     * a single get operation.
     */
    key_returned_t *keys_returned;
    key_returned_t *keys_cached;
    _mongocrypt_buffer_t filter;
    mongocrypt_t *crypt;

    key_returned_t *decryptor_iter;
    auth_request_t auth_request_azure;
    auth_request_t auth_request_gcp;
} _mongocrypt_key_broker_t;

void _mongocrypt_key_broker_init(_mongocrypt_key_broker_t *kb, mongocrypt_t *crypt);

/* Add a request for a key by UUID. */
bool _mongocrypt_key_broker_request_id(_mongocrypt_key_broker_t *kb,
                                       const _mongocrypt_buffer_t *key_id) MONGOCRYPT_WARN_UNUSED_RESULT;

/* Add keyAltName into the key broker.
   Key is added as KEY_EMPTY. */
bool _mongocrypt_key_broker_request_name(_mongocrypt_key_broker_t *kb,
                                         const bson_value_t *key_alt_name) MONGOCRYPT_WARN_UNUSED_RESULT;

/* Switch mode to permit adding documents without prior requests. */
bool _mongocrypt_key_broker_request_any(_mongocrypt_key_broker_t *kb) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_key_broker_requests_done(_mongocrypt_key_broker_t *kb);

/* Get the find command filter. */
bool _mongocrypt_key_broker_filter(_mongocrypt_key_broker_t *kb,
                                   mongocrypt_binary_t *out) MONGOCRYPT_WARN_UNUSED_RESULT;

/* Add a key document. */
bool _mongocrypt_key_broker_add_doc(_mongocrypt_key_broker_t *kb,
                                    _mongocrypt_opts_kms_providers_t *kms_providers,
                                    const _mongocrypt_buffer_t *doc) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_key_broker_docs_done(_mongocrypt_key_broker_t *kb);

/* Iterate the keys needing KMS decryption. */
mongocrypt_kms_ctx_t *_mongocrypt_key_broker_next_kms(_mongocrypt_key_broker_t *kb) MONGOCRYPT_WARN_UNUSED_RESULT;

/* Indicate that all KMS requests are complete. */
bool _mongocrypt_key_broker_kms_done(_mongocrypt_key_broker_t *kb, _mongocrypt_opts_kms_providers_t *kms_providers);

/* Get the final decrypted key material from a key by looking up with a key_id.
 * @out is always initialized, even on error. */
bool _mongocrypt_key_broker_decrypted_key_by_id(_mongocrypt_key_broker_t *kb,
                                                const _mongocrypt_buffer_t *key_id,
                                                _mongocrypt_buffer_t *out) MONGOCRYPT_WARN_UNUSED_RESULT;

/* Get the final decrypted key material from a key, and optionally its key_id.
 * @key_id_out may be NULL. @out and @key_id_out (if not NULL) are always
 * initialized, even on error. */
bool _mongocrypt_key_broker_decrypted_key_by_name(_mongocrypt_key_broker_t *kb,
                                                  const bson_value_t *key_alt_name,
                                                  _mongocrypt_buffer_t *out,
                                                  _mongocrypt_buffer_t *key_id_out) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_key_broker_status(_mongocrypt_key_broker_t *kb, mongocrypt_status_t *out);

void _mongocrypt_key_broker_cleanup(_mongocrypt_key_broker_t *kb);

/* For testing only, add a decrypted key */
void _mongocrypt_key_broker_add_test_key(_mongocrypt_key_broker_t *kb, const _mongocrypt_buffer_t *key_id);

/* _mongocrypt_key_broker_restart is used to request additional keys. It must
 * only be called in the KB_DONE state. */
bool _mongocrypt_key_broker_restart(_mongocrypt_key_broker_t *kb);

#endif /* MONGOCRYPT_KEY_BROKER_PRIVATE_H */

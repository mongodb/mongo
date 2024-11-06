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

#include "mc-array-private.h"
#include "mongocrypt-key-broker-private.h"
#include "mongocrypt-private.h"

typedef struct _auth_request_t {
    mongocrypt_kms_ctx_t kms;
    bool returned;
    char *kmsid;
} auth_request_t;

auth_request_t *auth_request_new() {
    return bson_malloc0(sizeof(auth_request_t));
}

void auth_request_destroy(auth_request_t *ar) {
    if (!ar) {
        return;
    }
    _mongocrypt_kms_ctx_cleanup(&ar->kms);
    bson_free(ar->kmsid);
    bson_free(ar);
}

struct _mc_mapof_kmsid_to_authrequest_t {
    mc_array_t entries;
};

mc_mapof_kmsid_to_authrequest_t *mc_mapof_kmsid_to_authrequest_new(void) {
    mc_mapof_kmsid_to_authrequest_t *k2a = bson_malloc0(sizeof(mc_mapof_kmsid_to_authrequest_t));
    _mc_array_init(&k2a->entries, sizeof(auth_request_t *));
    return k2a;
}

void mc_mapof_kmsid_to_authrequest_destroy(mc_mapof_kmsid_to_authrequest_t *k2a) {
    if (!k2a) {
        return;
    }
    for (size_t i = 0; i < k2a->entries.len; i++) {
        auth_request_t *ar = _mc_array_index(&k2a->entries, auth_request_t *, i);
        auth_request_destroy(ar);
    }
    _mc_array_destroy(&k2a->entries);
    bson_free(k2a);
}

bool mc_mapof_kmsid_to_authrequest_has(const mc_mapof_kmsid_to_authrequest_t *k2a, const char *kmsid) {
    BSON_ASSERT_PARAM(k2a);
    BSON_ASSERT_PARAM(kmsid);
    for (size_t i = 0; i < k2a->entries.len; i++) {
        auth_request_t *ar = _mc_array_index(&k2a->entries, auth_request_t *, i);
        if (0 == strcmp(ar->kmsid, kmsid)) {
            return true;
        }
    }
    return false;
}

size_t mc_mapof_kmsid_to_authrequest_len(const mc_mapof_kmsid_to_authrequest_t *k2a) {
    BSON_ASSERT_PARAM(k2a);
    return k2a->entries.len;
}

bool mc_mapof_kmsid_to_authrequest_empty(const mc_mapof_kmsid_to_authrequest_t *k2a) {
    BSON_ASSERT_PARAM(k2a);
    return k2a->entries.len == 0;
}

// `mc_mapof_kmsid_to_authrequest_put` moves `to_put` into the map and takes ownership of `to_put`.
// No checking is done to prohibit duplicate entries.
void mc_mapof_kmsid_to_authrequest_put(mc_mapof_kmsid_to_authrequest_t *k2a, auth_request_t *to_put) {
    BSON_ASSERT_PARAM(k2a);

    _mc_array_append_val(&k2a->entries, to_put);
}

auth_request_t *mc_mapof_kmsid_to_authrequest_at(mc_mapof_kmsid_to_authrequest_t *k2a, size_t i) {
    BSON_ASSERT_PARAM(k2a);

    return _mc_array_index(&k2a->entries, auth_request_t *, i);
}

void _mongocrypt_key_broker_init(_mongocrypt_key_broker_t *kb, mongocrypt_t *crypt) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(crypt);

    memset(kb, 0, sizeof(*kb));
    kb->crypt = crypt;
    kb->state = KB_REQUESTING;
    kb->status = mongocrypt_status_new();
    kb->auth_requests = mc_mapof_kmsid_to_authrequest_new();
}

/*
 * Creates a new key_returned_t and prepends it to a list.
 *
 * Side effects:
 * - updates *list to point to a new head.
 */
static key_returned_t *
_key_returned_prepend(_mongocrypt_key_broker_t *kb, key_returned_t **list, _mongocrypt_key_doc_t *key_doc) {
    key_returned_t *key_returned;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(list);
    BSON_ASSERT_PARAM(key_doc);

    key_returned = bson_malloc0(sizeof(*key_returned));
    BSON_ASSERT(key_returned);

    key_returned->doc = _mongocrypt_key_new();
    _mongocrypt_key_doc_copy_to(key_doc, key_returned->doc);

    /* Prepend and update the head of the list. */
    key_returned->next = *list;
    *list = key_returned;

    /* Update the head of the decrypting iter. */
    kb->decryptor_iter = kb->keys_returned;
    return key_returned;
}

/* Find the first (if any) key_returned_t matching either a key_id or a list of
 * key_alt_names (both are NULLable) */
static key_returned_t *
_key_returned_find_one(key_returned_t *list, _mongocrypt_buffer_t *key_id, _mongocrypt_key_alt_name_t *key_alt_names) {
    key_returned_t *key_returned;

    /* list can be NULL. */
    /* key_id and key_alt_names are not dereferenced in this function and they
     * are checked just before being passed on as parameters. */

    for (key_returned = list; NULL != key_returned; key_returned = key_returned->next) {
        if (key_id) {
            BSON_ASSERT(key_returned->doc);
            if (0 == _mongocrypt_buffer_cmp(key_id, &key_returned->doc->id)) {
                return key_returned;
            }
        }
        if (key_alt_names) {
            BSON_ASSERT(key_returned->doc);
            if (_mongocrypt_key_alt_name_intersects(key_alt_names, key_returned->doc->key_alt_names)) {
                return key_returned;
            }
        }
    }

    return NULL;
}

/* Find the first (if any) key_request_t in the key broker matching either a
 * key_id or a list of key_alt_names (both are NULLable) */
static key_request_t *_key_request_find_one(_mongocrypt_key_broker_t *kb,
                                            const _mongocrypt_buffer_t *key_id,
                                            _mongocrypt_key_alt_name_t *key_alt_names) {
    key_request_t *key_request;

    BSON_ASSERT_PARAM(kb);
    /* key_id and key_alt_names are not dereferenced in this function and they
     * are checked just before being passed on as parameters. */

    for (key_request = kb->key_requests; NULL != key_request; key_request = key_request->next) {
        if (key_id) {
            if (0 == _mongocrypt_buffer_cmp(key_id, &key_request->id)) {
                return key_request;
            }
        }
        if (key_alt_names) {
            if (_mongocrypt_key_alt_name_intersects(key_alt_names, key_request->alt_name)) {
                return key_request;
            }
        }
    }

    return NULL;
}

static bool _all_key_requests_satisfied(_mongocrypt_key_broker_t *kb) {
    key_request_t *key_request;

    BSON_ASSERT_PARAM(kb);

    for (key_request = kb->key_requests; NULL != key_request; key_request = key_request->next) {
        if (!key_request->satisfied) {
            return false;
        }
    }
    return true;
}

static bool _key_broker_fail_w_msg(_mongocrypt_key_broker_t *kb, const char *msg) {
    mongocrypt_status_t *status;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(msg);

    kb->state = KB_ERROR;
    status = kb->status;
    CLIENT_ERR("%s", msg);
    return false;
}

static bool _key_broker_fail(_mongocrypt_key_broker_t *kb) {
    BSON_ASSERT_PARAM(kb);

    if (mongocrypt_status_ok(kb->status)) {
        return _key_broker_fail_w_msg(kb, "unexpected, failing but no error status set");
    }
    kb->state = KB_ERROR;
    return false;
}

static bool _try_satisfying_from_cache(_mongocrypt_key_broker_t *kb, key_request_t *req) {
    _mongocrypt_cache_key_attr_t *attr = NULL;
    _mongocrypt_cache_key_value_t *value = NULL;
    bool ret = false;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(req);

    if (kb->state != KB_REQUESTING && kb->state != KB_ADDING_DOCS_ANY) {
        _key_broker_fail_w_msg(kb, "trying to retrieve key from cache in invalid state");
        goto cleanup;
    }

    attr = _mongocrypt_cache_key_attr_new(&req->id, req->alt_name);
    if (!_mongocrypt_cache_get(&kb->crypt->cache_key, attr, (void **)&value)) {
        _key_broker_fail_w_msg(kb, "failed to retrieve from cache");
        goto cleanup;
    }

    if (value) {
        key_returned_t *key_returned;

        req->satisfied = true;
        if (_mongocrypt_buffer_empty(&value->decrypted_key_material)) {
            _key_broker_fail_w_msg(kb, "cache entry does not have decrypted key material");
            goto cleanup;
        }

        /* Add the cached key to our locally copied list.
         * Note, we deduplicate requests, but *not* keys from the cache,
         * because the state of the cache may change between each call to
         * _mongocrypt_cache_get.
         */
        key_returned = _key_returned_prepend(kb, &kb->keys_cached, value->key_doc);
        _mongocrypt_buffer_init(&key_returned->decrypted_key_material);
        _mongocrypt_buffer_copy_to(&value->decrypted_key_material, &key_returned->decrypted_key_material);
        key_returned->decrypted = true;
    }

    ret = true;
cleanup:
    _mongocrypt_cache_key_value_destroy(value);
    _mongocrypt_cache_key_attr_destroy(attr);
    return ret;
}

static bool _store_to_cache(_mongocrypt_key_broker_t *kb, key_returned_t *key_returned) {
    _mongocrypt_cache_key_value_t *value;
    _mongocrypt_cache_key_attr_t *attr;
    bool ret;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(key_returned);

    if (!key_returned->decrypted) {
        return _key_broker_fail_w_msg(kb, "cannot cache non-decrypted key");
    }

    attr = _mongocrypt_cache_key_attr_new(&key_returned->doc->id, key_returned->doc->key_alt_names);
    if (!attr) {
        return _key_broker_fail_w_msg(kb, "could not create key cache attribute");
    }
    value = _mongocrypt_cache_key_value_new(key_returned->doc, &key_returned->decrypted_key_material);
    ret = _mongocrypt_cache_add_stolen(&kb->crypt->cache_key, attr, value, kb->status);
    _mongocrypt_cache_key_attr_destroy(attr);
    if (!ret) {
        return _key_broker_fail(kb);
    }
    return true;
}

bool _mongocrypt_key_broker_request_id(_mongocrypt_key_broker_t *kb, const _mongocrypt_buffer_t *key_id) {
    key_request_t *req;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(key_id);

    if (kb->state != KB_REQUESTING) {
        return _key_broker_fail_w_msg(kb, "attempting to request a key id, but in wrong state");
    }

    if (!_mongocrypt_buffer_is_uuid((_mongocrypt_buffer_t *)key_id)) {
        return _key_broker_fail_w_msg(kb, "expected UUID for key id");
    }

    if (_key_request_find_one(kb, key_id, NULL)) {
        return true;
    }

    req = bson_malloc0(sizeof *req);
    BSON_ASSERT(req);

    _mongocrypt_buffer_copy_to(key_id, &req->id);
    req->next = kb->key_requests;
    kb->key_requests = req;
    if (!_try_satisfying_from_cache(kb, req)) {
        return false;
    }
    return true;
}

bool _mongocrypt_key_broker_request_name(_mongocrypt_key_broker_t *kb, const bson_value_t *key_alt_name_value) {
    key_request_t *req;
    _mongocrypt_key_alt_name_t *key_alt_name;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(key_alt_name_value);

    if (kb->state != KB_REQUESTING) {
        return _key_broker_fail_w_msg(kb, "attempting to request a key name, but in wrong state");
    }

    key_alt_name = _mongocrypt_key_alt_name_new(key_alt_name_value);

    /* Check if we already have a request for this key alt name. */
    if (_key_request_find_one(kb, NULL /* key id */, key_alt_name)) {
        _mongocrypt_key_alt_name_destroy_all(key_alt_name);
        return true;
    }

    req = bson_malloc0(sizeof *req);
    BSON_ASSERT(req);

    req->alt_name = key_alt_name /* takes ownership */;
    req->next = kb->key_requests;
    kb->key_requests = req;
    if (!_try_satisfying_from_cache(kb, req)) {
        return false;
    }
    return true;
}

bool _mongocrypt_key_broker_request_any(_mongocrypt_key_broker_t *kb) {
    BSON_ASSERT_PARAM(kb);

    if (kb->state != KB_REQUESTING) {
        return _key_broker_fail_w_msg(kb, "attempting to request any keys, but in wrong state");
    }

    if (kb->key_requests) {
        return _key_broker_fail_w_msg(kb, "attempting to request any keys, but requests already made");
    }

    kb->state = KB_ADDING_DOCS_ANY;

    return true;
}

bool _mongocrypt_key_broker_requests_done(_mongocrypt_key_broker_t *kb) {
    BSON_ASSERT_PARAM(kb);

    if (kb->state != KB_REQUESTING) {
        return _key_broker_fail_w_msg(kb, "attempting to finish adding requests, but in wrong state");
    }

    if (kb->key_requests) {
        if (_all_key_requests_satisfied(kb)) {
            kb->state = KB_DONE;
        } else {
            kb->state = KB_ADDING_DOCS;
        }
    } else {
        kb->state = KB_DONE;
    }
    return true;
}

bool _mongocrypt_key_broker_filter(_mongocrypt_key_broker_t *kb, mongocrypt_binary_t *out) {
    key_request_t *req;
    _mongocrypt_key_alt_name_t *key_alt_name;
    int name_index = 0;
    int id_index = 0;
    bson_t ids, names;
    bson_t *filter;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(out);

    if (kb->state != KB_ADDING_DOCS) {
        return _key_broker_fail_w_msg(kb, "attempting to retrieve filter, but in wrong state");
    }

    if (!_mongocrypt_buffer_empty(&kb->filter)) {
        _mongocrypt_buffer_to_binary(&kb->filter, out);
        return true;
    }

    bson_init(&names);
    bson_init(&ids);

    for (req = kb->key_requests; NULL != req; req = req->next) {
        if (req->satisfied) {
            continue;
        }

        if (!_mongocrypt_buffer_empty(&req->id)) {
            /* Collect key_ids in "ids" */
            char *key_str;

            key_str = bson_strdup_printf("%d", id_index++);
            if (!key_str || !_mongocrypt_buffer_append(&req->id, &ids, key_str, -1)) {
                bson_destroy(&ids);
                bson_destroy(&names);
                bson_free(key_str);
                return _key_broker_fail_w_msg(kb, "could not construct id list");
            }

            bson_free(key_str);
        }

        /* Collect key alt names in "names" */
        for (key_alt_name = req->alt_name; NULL != key_alt_name; key_alt_name = key_alt_name->next) {
            char *key_str;

            key_str = bson_strdup_printf("%d", name_index++);
            BSON_ASSERT(key_str);
            if (!bson_append_value(&names, key_str, (int)strlen(key_str), &key_alt_name->value)) {
                bson_destroy(&ids);
                bson_destroy(&names);
                bson_free(key_str);
                return _key_broker_fail_w_msg(kb, "could not construct keyAltName list");
            }

            bson_free(key_str);
        }
    }

    /*
     * This is our final query:
     * { $or: [ { _id: { $in : [ids] }},
     *          { keyAltName : { $in : [names] }} ] }
     */
    filter = BCON_NEW("$or",
                      "[",
                      "{",
                      "_id",
                      "{",
                      "$in",
                      BCON_ARRAY(&ids),
                      "}",
                      "}",
                      "{",
                      "keyAltNames",
                      "{",
                      "$in",
                      BCON_ARRAY(&names),
                      "}",
                      "}",
                      "]");

    _mongocrypt_buffer_steal_from_bson(&kb->filter, filter);
    _mongocrypt_buffer_to_binary(&kb->filter, out);
    bson_destroy(&ids);
    bson_destroy(&names);

    return true;
}

bool _mongocrypt_key_broker_add_doc(_mongocrypt_key_broker_t *kb,
                                    _mongocrypt_opts_kms_providers_t *kms_providers,
                                    const _mongocrypt_buffer_t *doc) {
    bool ret = false;
    bson_t doc_bson;
    _mongocrypt_key_doc_t *key_doc = NULL;
    key_request_t *key_request;
    key_returned_t *key_returned;
    _mongocrypt_kms_provider_t kek_provider;
    char *access_token = NULL;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(kms_providers);

    if (kb->state != KB_ADDING_DOCS && kb->state != KB_ADDING_DOCS_ANY) {
        _key_broker_fail_w_msg(kb, "attempting to add a key doc, but in wrong state");
        goto done;
    }

    if (!doc) {
        _key_broker_fail_w_msg(kb, "invalid key");
        goto done;
    }

    /* First, parse the key document. */
    key_doc = _mongocrypt_key_new();
    if (!_mongocrypt_buffer_to_bson(doc, &doc_bson)) {
        _key_broker_fail_w_msg(kb, "malformed BSON for key document");
        goto done;
    }

    if (!_mongocrypt_key_parse_owned(&doc_bson, key_doc, kb->status)) {
        goto done;
    }

    if (!_key_request_find_one(kb, &key_doc->id, key_doc->key_alt_names)) {
        /* If in normal mode, ensure that this document matches at least one
         * existing request. */
        if (kb->state == KB_ADDING_DOCS) {
            _key_broker_fail_w_msg(kb, "unexpected key returned, does not match any requests");
            goto done;
        }

        /* If in any mode, add request for provided document now. */
        if (kb->state == KB_ADDING_DOCS_ANY) {
            key_request_t *const req = bson_malloc0(sizeof(key_request_t));

            BSON_ASSERT(req);

            _mongocrypt_buffer_copy_to(&key_doc->id, &req->id);
            req->alt_name = _mongocrypt_key_alt_name_copy_all(key_doc->key_alt_names);
            req->next = kb->key_requests;
            kb->key_requests = req;

            if (!_try_satisfying_from_cache(kb, req)) {
                goto done;
            }

            /* Key is already cached; no work to be done. */
            if (req->satisfied) {
                ret = true;
                goto done;
            }
        }
    }

    /* Check if there are other keys_returned with intersecting altnames or
     * equal id. This is an error. Do *not* check cached keys. */
    if (_key_returned_find_one(kb->keys_returned, &key_doc->id, key_doc->key_alt_names)) {
        _key_broker_fail_w_msg(kb, "keys returned have duplicate keyAltNames or _id");
        goto done;
    }

    key_returned = _key_returned_prepend(kb, &kb->keys_returned, key_doc);

    /* Check that the returned key doc's provider matches. */
    kek_provider = key_doc->kek.kms_provider;

    mc_kms_creds_t kc;
    if (!_mongocrypt_opts_kms_providers_lookup(kms_providers, key_doc->kek.kmsid, &kc)) {
        mongocrypt_status_t *status = kb->status;
        CLIENT_ERR("KMS provider `%s` is not configured", key_doc->kek.kmsid);
        _key_broker_fail(kb);
        goto done;
    }

    /* If the KMS provider is local, decrypt immediately. Otherwise, create the
     * HTTP KMS request. */
    BSON_ASSERT(kb->crypt);
    if (kek_provider == MONGOCRYPT_KMS_PROVIDER_LOCAL) {
        BSON_ASSERT(kc.type == MONGOCRYPT_KMS_PROVIDER_LOCAL);
        if (!_mongocrypt_unwrap_key(kb->crypt->crypto,
                                    &kc.value.local.key,
                                    &key_returned->doc->key_material,
                                    &key_returned->decrypted_key_material,
                                    kb->status)) {
            _key_broker_fail(kb);
            goto done;
        }
        key_returned->decrypted = true;
        if (!_store_to_cache(kb, key_returned)) {
            goto done;
        }
    } else if (kek_provider == MONGOCRYPT_KMS_PROVIDER_AWS) {
        if (!_mongocrypt_kms_ctx_init_aws_decrypt(&key_returned->kms,
                                                  kms_providers,
                                                  key_doc,
                                                  kb->crypt->crypto,
                                                  key_doc->kek.kmsid,
                                                  &kb->crypt->log)) {
            mongocrypt_kms_ctx_status(&key_returned->kms, kb->status);
            _key_broker_fail(kb);
            goto done;
        }
    } else if (kek_provider == MONGOCRYPT_KMS_PROVIDER_AZURE) {
        BSON_ASSERT(kc.type == MONGOCRYPT_KMS_PROVIDER_AZURE);
        if (kc.value.azure.access_token) {
            access_token = bson_strdup(kc.value.azure.access_token);
        } else {
            access_token = mc_mapof_kmsid_to_token_get_token(kb->crypt->cache_oauth, key_doc->kek.kmsid);
        }
        if (!access_token) {
            key_returned->needs_auth = true;
            /* Create an oauth request if one does not exist. */
            if (!mc_mapof_kmsid_to_authrequest_has(kb->auth_requests, key_doc->kek.kmsid)) {
                auth_request_t *ar = auth_request_new();
                if (!_mongocrypt_kms_ctx_init_azure_auth(&ar->kms,
                                                         &kc,
                                                         /* The key vault endpoint is used to determine the scope. */
                                                         key_doc->kek.provider.azure.key_vault_endpoint,
                                                         key_doc->kek.kmsid,
                                                         &kb->crypt->log)) {
                    mongocrypt_kms_ctx_status(&ar->kms, kb->status);
                    _key_broker_fail(kb);
                    auth_request_destroy(ar);
                    goto done;
                }
                ar->kmsid = bson_strdup(key_doc->kek.kmsid);
                mc_mapof_kmsid_to_authrequest_put(kb->auth_requests, ar);
            }
        } else {
            if (!_mongocrypt_kms_ctx_init_azure_unwrapkey(&key_returned->kms,
                                                          kms_providers,
                                                          access_token,
                                                          key_doc,
                                                          key_returned->doc->kek.kmsid,
                                                          &kb->crypt->log)) {
                mongocrypt_kms_ctx_status(&key_returned->kms, kb->status);
                _key_broker_fail(kb);
                goto done;
            }
        }
    } else if (kek_provider == MONGOCRYPT_KMS_PROVIDER_GCP) {
        BSON_ASSERT(kc.type == MONGOCRYPT_KMS_PROVIDER_GCP);
        if (NULL != kc.value.gcp.access_token) {
            access_token = bson_strdup(kc.value.gcp.access_token);
        } else {
            access_token = mc_mapof_kmsid_to_token_get_token(kb->crypt->cache_oauth, key_doc->kek.kmsid);
        }
        if (!access_token) {
            key_returned->needs_auth = true;
            /* Create an oauth request if one does not exist. */
            if (!mc_mapof_kmsid_to_authrequest_has(kb->auth_requests, key_doc->kek.kmsid)) {
                auth_request_t *ar = auth_request_new();
                if (!_mongocrypt_kms_ctx_init_gcp_auth(&ar->kms,
                                                       &kb->crypt->opts,
                                                       &kc,
                                                       key_doc->kek.provider.gcp.endpoint,
                                                       key_doc->kek.kmsid,
                                                       &kb->crypt->log)) {
                    mongocrypt_kms_ctx_status(&ar->kms, kb->status);
                    _key_broker_fail(kb);
                    auth_request_destroy(ar);
                    goto done;
                }
                ar->kmsid = bson_strdup(key_doc->kek.kmsid);
                mc_mapof_kmsid_to_authrequest_put(kb->auth_requests, ar);
            }
        } else {
            if (!_mongocrypt_kms_ctx_init_gcp_decrypt(&key_returned->kms,
                                                      kms_providers,
                                                      access_token,
                                                      key_doc,
                                                      key_returned->doc->kek.kmsid,
                                                      &kb->crypt->log)) {
                mongocrypt_kms_ctx_status(&key_returned->kms, kb->status);
                _key_broker_fail(kb);
                goto done;
            }
        }
    } else if (kek_provider == MONGOCRYPT_KMS_PROVIDER_KMIP) {
        BSON_ASSERT(kc.type == MONGOCRYPT_KMS_PROVIDER_KMIP);
        char *unique_identifier;
        _mongocrypt_endpoint_t *endpoint;

        if (!key_returned->doc->kek.provider.kmip.key_id) {
            _key_broker_fail_w_msg(kb, "KMIP key malformed, no keyId present");
            goto done;
        }

        unique_identifier = key_returned->doc->kek.provider.kmip.key_id;

        if (key_returned->doc->kek.provider.kmip.endpoint) {
            endpoint = key_returned->doc->kek.provider.kmip.endpoint;
        } else if (kc.value.kmip.endpoint) {
            endpoint = kc.value.kmip.endpoint;
        } else {
            _key_broker_fail_w_msg(kb, "endpoint not set for KMIP request");
            goto done;
        }

        if (key_returned->doc->kek.provider.kmip.delegated) {
            if (!_mongocrypt_kms_ctx_init_kmip_decrypt(&key_returned->kms,
                                                       endpoint,
                                                       key_doc->kek.kmsid,
                                                       key_doc,
                                                       &kb->crypt->log)) {
                mongocrypt_kms_ctx_status(&key_returned->kms, kb->status);
                _key_broker_fail(kb);
                goto done;
            }
        } else {
            if (!_mongocrypt_kms_ctx_init_kmip_get(&key_returned->kms,
                                                   endpoint,
                                                   unique_identifier,
                                                   key_doc->kek.kmsid,
                                                   &kb->crypt->log)) {
                mongocrypt_kms_ctx_status(&key_returned->kms, kb->status);
                _key_broker_fail(kb);
                goto done;
            }
        }
    } else {
        _key_broker_fail_w_msg(kb, "unrecognized kms provider");
        goto done;
    }

    /* Mark all matching key requests as satisfied. */
    for (key_request = kb->key_requests; NULL != key_request; key_request = key_request->next) {
        if (0 == _mongocrypt_buffer_cmp(&key_doc->id, &key_request->id)) {
            key_request->satisfied = true;
        }
        if (_mongocrypt_key_alt_name_intersects(key_doc->key_alt_names, key_request->alt_name)) {
            key_request->satisfied = true;
        }
    }

    ret = true;
done:
    bson_free(access_token);
    _mongocrypt_key_destroy(key_doc);
    return ret;
}

bool _mongocrypt_key_broker_docs_done(_mongocrypt_key_broker_t *kb) {
    key_returned_t *key_returned;
    bool needs_decryption;
    bool needs_auth;

    BSON_ASSERT_PARAM(kb);

    if (kb->state != KB_ADDING_DOCS && kb->state != KB_ADDING_DOCS_ANY) {
        return _key_broker_fail_w_msg(kb, "attempting to finish adding docs, but in wrong state");
    }

    /* If there are any requests left unsatisfied, error. */
    if (!_all_key_requests_satisfied(kb)) {
        return _key_broker_fail_w_msg(
            kb,
            "not all keys requested were satisfied. Verify that key vault DB/collection name was correctly specified.");
    }

    /* Transition to the next state.
     *  - If there are any Azure or GCP backed keys, and no oauth token is
     * cached, transition to KB_AUTHENTICATING.
     *  - Otherwise, if there are keys that need to be decrypted, transition to
     * KB_DECRYPTING_KEY_MATERIAL.
     *  - Otherwise, all keys were retrieved from the cache or decrypted locally,
     * skip the decrypting state and go right to KB_DONE.
     */
    needs_decryption = false;
    needs_auth = false;
    for (key_returned = kb->keys_returned; NULL != key_returned; key_returned = key_returned->next) {
        if (key_returned->needs_auth) {
            needs_auth = true;
            break;
        }
        if (!key_returned->decrypted) {
            needs_decryption = true;
        }
    }

    if (needs_auth) {
        kb->state = KB_AUTHENTICATING;
    } else if (needs_decryption) {
        kb->state = KB_DECRYPTING_KEY_MATERIAL;
    } else {
        kb->state = KB_DONE;
    }
    return true;
}

mongocrypt_kms_ctx_t *_mongocrypt_key_broker_next_kms(_mongocrypt_key_broker_t *kb) {
    BSON_ASSERT_PARAM(kb);

    if (kb->state != KB_DECRYPTING_KEY_MATERIAL && kb->state != KB_AUTHENTICATING) {
        _key_broker_fail_w_msg(kb, "attempting to get KMS request, but in wrong state");
        /* TODO (CDRIVER-3327) this breaks other expectations. If the caller only
         * checks the return value they may mistake this NULL as indicating all
         * KMS requests have been iterated. */
        return NULL;
    }

    if (kb->state == KB_AUTHENTICATING) {
        if (mc_mapof_kmsid_to_authrequest_empty(kb->auth_requests)) {
            _key_broker_fail_w_msg(kb,
                                   "unexpected, attempting to authenticate but "
                                   "KMS request not initialized");
            return NULL;
        }

        // Return the first not-yet-returned auth request.
        for (size_t i = 0; i < mc_mapof_kmsid_to_authrequest_len(kb->auth_requests); i++) {
            auth_request_t *ar = mc_mapof_kmsid_to_authrequest_at(kb->auth_requests, i);

            if (ar->kms.should_retry) {
                ar->kms.should_retry = false;
                ar->returned = true;
                return &ar->kms;
            }

            if (ar->returned) {
                continue;
            }
            ar->returned = true;
            return &ar->kms;
        }

        return NULL;
    }

    // Check if any requests need retry
    for (key_returned_t *ptr = kb->keys_returned; ptr != NULL; ptr = ptr->next) {
        if (ptr->kms.should_retry) {
            ptr->kms.should_retry = false;
            return &ptr->kms;
        }
    }
    while (kb->decryptor_iter) {
        if (!kb->decryptor_iter->decrypted) {
            key_returned_t *key_returned;

            key_returned = kb->decryptor_iter;
            /* iterate before returning, so next call starts at next entry */
            kb->decryptor_iter = kb->decryptor_iter->next;
            return &key_returned->kms;
        }
        kb->decryptor_iter = kb->decryptor_iter->next;
    }

    return NULL;
}

bool _mongocrypt_key_broker_kms_done(_mongocrypt_key_broker_t *kb, _mongocrypt_opts_kms_providers_t *kms_providers) {
    key_returned_t *key_returned;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(kms_providers);

    if (kb->state != KB_DECRYPTING_KEY_MATERIAL && kb->state != KB_AUTHENTICATING) {
        return _key_broker_fail_w_msg(kb, "attempting to complete KMS requests, but in wrong state");
    }

    if (kb->state == KB_AUTHENTICATING) {
        bson_t oauth_response;
        _mongocrypt_buffer_t oauth_response_buf;

        // Apply tokens from oauth responses to oauth token cache.
        for (size_t i = 0; i < mc_mapof_kmsid_to_authrequest_len(kb->auth_requests); i++) {
            auth_request_t *ar = mc_mapof_kmsid_to_authrequest_at(kb->auth_requests, i);

            if (!_mongocrypt_kms_ctx_result(&ar->kms, &oauth_response_buf)) {
                mongocrypt_kms_ctx_status(&ar->kms, kb->status);
                return _key_broker_fail(kb);
            }

            /* Cache returned tokens. */
            BSON_ASSERT(_mongocrypt_buffer_to_bson(&oauth_response_buf, &oauth_response));
            if (!mc_mapof_kmsid_to_token_add_response(kb->crypt->cache_oauth, ar->kmsid, &oauth_response, kb->status)) {
                return _key_broker_fail(kb);
            }
        }

        /* Auth should be finished, create any remaining KMS requests. */
        for (key_returned = kb->keys_returned; NULL != key_returned; key_returned = key_returned->next) {
            char *access_token;

            if (!key_returned->needs_auth) {
                continue;
            }

            mc_kms_creds_t kc;
            if (!_mongocrypt_opts_kms_providers_lookup(kms_providers, key_returned->doc->kek.kmsid, &kc)) {
                mongocrypt_status_t *status = kb->status;
                CLIENT_ERR("KMS provider `%s` is not configured", key_returned->doc->kek.kmsid);
                return _key_broker_fail(kb);
            }

            if (key_returned->doc->kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_AZURE) {
                BSON_ASSERT(kc.type == MONGOCRYPT_KMS_PROVIDER_AZURE);
                if (kc.value.azure.access_token) {
                    access_token = bson_strdup(kc.value.azure.access_token);
                } else {
                    access_token =
                        mc_mapof_kmsid_to_token_get_token(kb->crypt->cache_oauth, key_returned->doc->kek.kmsid);
                }

                if (!access_token) {
                    return _key_broker_fail_w_msg(kb, "authentication failed, no oauth token");
                }

                if (!_mongocrypt_kms_ctx_init_azure_unwrapkey(&key_returned->kms,
                                                              kms_providers,
                                                              access_token,
                                                              key_returned->doc,
                                                              key_returned->doc->kek.kmsid,
                                                              &kb->crypt->log)) {
                    mongocrypt_kms_ctx_status(&key_returned->kms, kb->status);
                    bson_free(access_token);
                    return _key_broker_fail(kb);
                }

                key_returned->needs_auth = false;
                bson_free(access_token);
            } else if (key_returned->doc->kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_GCP) {
                BSON_ASSERT(kc.type == MONGOCRYPT_KMS_PROVIDER_GCP);
                if (kc.value.gcp.access_token) {
                    access_token = bson_strdup(kc.value.gcp.access_token);
                } else {
                    access_token =
                        mc_mapof_kmsid_to_token_get_token(kb->crypt->cache_oauth, key_returned->doc->kek.kmsid);
                }

                if (!access_token) {
                    return _key_broker_fail_w_msg(kb, "authentication failed, no oauth token");
                }

                if (!_mongocrypt_kms_ctx_init_gcp_decrypt(&key_returned->kms,
                                                          kms_providers,
                                                          access_token,
                                                          key_returned->doc,
                                                          key_returned->doc->kek.kmsid,
                                                          &kb->crypt->log)) {
                    mongocrypt_kms_ctx_status(&key_returned->kms, kb->status);
                    bson_free(access_token);
                    return _key_broker_fail(kb);
                }

                key_returned->needs_auth = false;
                bson_free(access_token);
            } else {
                return _key_broker_fail_w_msg(kb,
                                              "unexpected, authenticating but "
                                              "no requests require "
                                              "authentication");
            }
        }

        kb->state = KB_DECRYPTING_KEY_MATERIAL;
        return true;
    }

    for (key_returned = kb->keys_returned; NULL != key_returned; key_returned = key_returned->next) {
        /* Local keys were already decrypted. */
        if (key_returned->doc->kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_AWS
            || key_returned->doc->kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_AZURE
            || key_returned->doc->kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_GCP) {
            if (key_returned->decrypted) {
                /* Non-local keys may have been decrypted previously if the key
                 * broker has been restarted. */
                continue;
            }

            if (!key_returned->kms.req) {
                return _key_broker_fail_w_msg(kb, "unexpected, KMS not set on key returned");
            }

            if (!_mongocrypt_kms_ctx_result(&key_returned->kms, &key_returned->decrypted_key_material)) {
                /* Always fatal. Key attempted to decrypt but failed. */
                mongocrypt_kms_ctx_status(&key_returned->kms, kb->status);
                return _key_broker_fail(kb);
            }
        } else if (key_returned->doc->kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_KMIP) {
            _mongocrypt_buffer_t kek;
            if (!_mongocrypt_kms_ctx_result(&key_returned->kms, &kek)) {
                mongocrypt_kms_ctx_status(&key_returned->kms, kb->status);
                return _key_broker_fail(kb);
            }

            if (key_returned->doc->kek.provider.kmip.delegated) {
                if (!_mongocrypt_kms_ctx_result(&key_returned->kms, &key_returned->decrypted_key_material)) {
                    mongocrypt_kms_ctx_status(&key_returned->kms, kb->status);
                    return _key_broker_fail(kb);
                }
            } else if (!_mongocrypt_unwrap_key(kb->crypt->crypto,
                                               &kek,
                                               &key_returned->doc->key_material,
                                               &key_returned->decrypted_key_material,
                                               kb->status)) {
                _key_broker_fail(kb);
                _mongocrypt_buffer_cleanup(&kek);
                return false;
            }
            _mongocrypt_buffer_cleanup(&kek);
        } else if (key_returned->doc->kek.kms_provider != MONGOCRYPT_KMS_PROVIDER_LOCAL) {
            return _key_broker_fail_w_msg(kb, "unrecognized kms provider");
        }

        if (key_returned->decrypted_key_material.len != MONGOCRYPT_KEY_LEN) {
            return _key_broker_fail_w_msg(kb, "decrypted key is incorrect length");
        }

        key_returned->decrypted = true;
        if (!_store_to_cache(kb, key_returned)) {
            return false;
        }
    }

    kb->state = KB_DONE;
    return true;
}

static bool _get_decrypted_key_material(_mongocrypt_key_broker_t *kb,
                                        _mongocrypt_buffer_t *key_id,
                                        _mongocrypt_key_alt_name_t *key_alt_name,
                                        _mongocrypt_buffer_t *out,
                                        _mongocrypt_buffer_t *key_id_out) {
    key_returned_t *key_returned;

    BSON_ASSERT_PARAM(kb);
    /* key_id can be NULL */
    /* key_alt_name can be NULL */
    BSON_ASSERT_PARAM(out);
    /* key_id_out is checked before each use, so it can be NULL */

    _mongocrypt_buffer_init(out);
    if (key_id_out) {
        _mongocrypt_buffer_init(key_id_out);
    }
    /* Search both keys_returned and keys_cached. */

    key_returned = _key_returned_find_one(kb->keys_returned, key_id, key_alt_name);
    if (!key_returned) {
        /* Try the keys retrieved from the cache. */
        key_returned = _key_returned_find_one(kb->keys_cached, key_id, key_alt_name);
    }

    if (!key_returned) {
        return _key_broker_fail_w_msg(kb, "could not find key");
    }

    if (!key_returned->decrypted) {
        return _key_broker_fail_w_msg(kb, "unexpected, key not decrypted");
    }

    _mongocrypt_buffer_copy_to(&key_returned->decrypted_key_material, out);
    if (key_id_out) {
        _mongocrypt_buffer_copy_to(&key_returned->doc->id, key_id_out);
    }
    return true;
}

bool _mongocrypt_key_broker_decrypted_key_by_id(_mongocrypt_key_broker_t *kb,
                                                const _mongocrypt_buffer_t *key_id,
                                                _mongocrypt_buffer_t *out) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(key_id);
    BSON_ASSERT_PARAM(out);

    if (kb->state != KB_DONE && kb->state != KB_REQUESTING) {
        return _key_broker_fail_w_msg(kb, "attempting retrieve decrypted key material, but in wrong state");
    }
    return _get_decrypted_key_material(kb,
                                       (_mongocrypt_buffer_t *)key_id,
                                       NULL /* key alt name */,
                                       out,
                                       NULL /* key id out */);
}

bool _mongocrypt_key_broker_decrypted_key_by_name(_mongocrypt_key_broker_t *kb,
                                                  const bson_value_t *key_alt_name_value,
                                                  _mongocrypt_buffer_t *out,
                                                  _mongocrypt_buffer_t *key_id_out) {
    bool ret;
    _mongocrypt_key_alt_name_t *key_alt_name;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(key_alt_name_value);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(key_id_out);

    if (kb->state != KB_DONE) {
        return _key_broker_fail_w_msg(kb, "attempting retrieve decrypted key material, but in wrong state");
    }

    key_alt_name = _mongocrypt_key_alt_name_new(key_alt_name_value);
    ret = _get_decrypted_key_material(kb, NULL, key_alt_name, out, key_id_out);
    _mongocrypt_key_alt_name_destroy_all(key_alt_name);
    return ret;
}

bool _mongocrypt_key_broker_status(_mongocrypt_key_broker_t *kb, mongocrypt_status_t *out) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(out);

    if (!mongocrypt_status_ok(kb->status)) {
        _mongocrypt_status_copy_to(kb->status, out);
        return false;
    }

    return true;
}

static void _destroy_key_requests(key_request_t *head) {
    key_request_t *tmp;

    while (head) {
        tmp = head->next;

        _mongocrypt_buffer_cleanup(&head->id);
        _mongocrypt_key_alt_name_destroy_all(head->alt_name);

        bson_free(head);
        head = tmp;
    }
}

static void _destroy_keys_returned(key_returned_t *head) {
    key_returned_t *tmp;

    while (head) {
        tmp = head->next;

        _mongocrypt_key_destroy(head->doc);
        _mongocrypt_buffer_cleanup(&head->decrypted_key_material);
        _mongocrypt_kms_ctx_cleanup(&head->kms);

        bson_free(head);
        head = tmp;
    }
}

void _mongocrypt_key_broker_cleanup(_mongocrypt_key_broker_t *kb) {
    if (!kb) {
        return;
    }
    mongocrypt_status_destroy(kb->status);
    _mongocrypt_buffer_cleanup(&kb->filter);
    /* Delete all linked lists */
    _destroy_keys_returned(kb->keys_returned);
    _destroy_keys_returned(kb->keys_cached);
    _destroy_key_requests(kb->key_requests);
    mc_mapof_kmsid_to_authrequest_destroy(kb->auth_requests);
}

void _mongocrypt_key_broker_add_test_key(_mongocrypt_key_broker_t *kb, const _mongocrypt_buffer_t *key_id) {
    key_returned_t *key_returned;
    _mongocrypt_key_doc_t *key_doc;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(key_id);

    key_doc = _mongocrypt_key_new();
    _mongocrypt_buffer_copy_to(key_id, &key_doc->id);

    key_returned = _key_returned_prepend(kb, &kb->keys_returned, key_doc);
    key_returned->decrypted = true;
    _mongocrypt_buffer_init(&key_returned->decrypted_key_material);
    _mongocrypt_buffer_resize(&key_returned->decrypted_key_material, MONGOCRYPT_KEY_LEN);
    // Initialize test key material with all zeros.
    memset(key_returned->decrypted_key_material.data, 0, MONGOCRYPT_KEY_LEN);
    _mongocrypt_key_destroy(key_doc);
    /* Hijack state and move directly to DONE. */
    kb->state = KB_DONE;
}

bool _mongocrypt_key_broker_restart(_mongocrypt_key_broker_t *kb) {
    BSON_ASSERT_PARAM(kb);
    if (kb->state != KB_DONE) {
        return _key_broker_fail_w_msg(kb, "_mongocrypt_key_broker_restart called in wrong state");
    }
    kb->state = KB_REQUESTING;
    _mongocrypt_buffer_cleanup(&kb->filter);
    _mongocrypt_buffer_init(&kb->filter);
    return true;
}

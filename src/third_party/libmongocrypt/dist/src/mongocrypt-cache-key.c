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

#include "mongocrypt-cache-key-private.h"

/* The key cache.
 *
 * Attribute is a UUID in the form of a _mongocrypt_buffer_t.
 * Value contains a key document and decrypted key material.
 */

/* returns true on success. out is set to 0 if equal, non-zero otherwise. */
static bool _cmp_attr(void *a, void *b, int *out) {
    _mongocrypt_cache_key_attr_t *attr_a, *attr_b;

    BSON_ASSERT_PARAM(a);
    BSON_ASSERT_PARAM(b);
    BSON_ASSERT_PARAM(out);

    *out = 1;
    attr_a = (_mongocrypt_cache_key_attr_t *)a;
    attr_b = (_mongocrypt_cache_key_attr_t *)b;

    if (!_mongocrypt_buffer_empty(&attr_a->id) && !_mongocrypt_buffer_empty(&attr_b->id)) {
        if (0 == _mongocrypt_buffer_cmp(&attr_a->id, &attr_b->id)) {
            *out = 0;
        }
    }

    if (_mongocrypt_key_alt_name_intersects(attr_a->alt_names, attr_b->alt_names)) {
        *out = 0;
    }

    /* No error. */
    return true;
}

static void *_copy_attr(void *attr) {
    _mongocrypt_cache_key_attr_t *src;

    BSON_ASSERT_PARAM(attr);

    src = (_mongocrypt_cache_key_attr_t *)attr;

    return _mongocrypt_cache_key_attr_new(&src->id, src->alt_names);
}

static void _destroy_attr(void *attr) {
    _mongocrypt_cache_key_attr_destroy(attr);
}

static void *_copy_contents(void *value) {
    _mongocrypt_cache_key_value_t *key_value;

    BSON_ASSERT_PARAM(value);

    key_value = (_mongocrypt_cache_key_value_t *)value;
    return _mongocrypt_cache_key_value_new(key_value->key_doc, &key_value->decrypted_key_material);
}

static void _dump_attr(void *attr_in) {
    _mongocrypt_cache_key_attr_t *attr;
    _mongocrypt_key_alt_name_t *altname;
    char *hex;

    BSON_ASSERT_PARAM(attr_in);

    attr = (_mongocrypt_cache_key_attr_t *)attr_in;
    hex = _mongocrypt_buffer_to_hex(&attr->id);
    printf("_id=%s,", hex);
    printf("keyAltNames=");
    for (altname = attr->alt_names; NULL != altname; altname = altname->next) {
        printf("%s\n", _mongocrypt_key_alt_name_get_string(altname));
    }
}

_mongocrypt_cache_key_value_t *_mongocrypt_cache_key_value_new(_mongocrypt_key_doc_t *key_doc,
                                                               _mongocrypt_buffer_t *decrypted_key_material) {
    _mongocrypt_cache_key_value_t *key_value;

    BSON_ASSERT_PARAM(key_doc);
    BSON_ASSERT_PARAM(decrypted_key_material);

    key_value = bson_malloc0(sizeof(*key_value));
    BSON_ASSERT(key_value);

    _mongocrypt_buffer_copy_to(decrypted_key_material, &key_value->decrypted_key_material);

    key_value->key_doc = _mongocrypt_key_new();
    _mongocrypt_key_doc_copy_to(key_doc, key_value->key_doc);

    return key_value;
}

void _mongocrypt_cache_key_value_destroy(void *value) {
    _mongocrypt_cache_key_value_t *key_value;

    if (!value) {
        return;
    }
    key_value = (_mongocrypt_cache_key_value_t *)value;
    _mongocrypt_key_destroy(key_value->key_doc);
    _mongocrypt_buffer_cleanup(&key_value->decrypted_key_material);
    bson_free(key_value);
}

void _mongocrypt_cache_key_init(_mongocrypt_cache_t *cache) {
    BSON_ASSERT_PARAM(cache);

    cache->cmp_attr = _cmp_attr;
    cache->copy_attr = _copy_attr;
    cache->destroy_attr = _destroy_attr;
    cache->copy_value = _copy_contents;
    cache->destroy_value = _mongocrypt_cache_key_value_destroy;
    cache->dump_attr = _dump_attr;
    _mongocrypt_mutex_init(&cache->mutex);
    cache->pair = NULL;
    cache->expiration = CACHE_EXPIRATION_MS;
}

/* Since key cache may be looked up by either _id or keyAltName,
 * "id" or "alt_names" may be NULL, but not both. Returns NULL on error. */
_mongocrypt_cache_key_attr_t *_mongocrypt_cache_key_attr_new(_mongocrypt_buffer_t *id,
                                                             _mongocrypt_key_alt_name_t *alt_names) {
    _mongocrypt_cache_key_attr_t *attr;

    if (NULL == id && NULL == alt_names) {
        return NULL;
    }

    attr = bson_malloc0(sizeof(*attr));
    BSON_ASSERT(attr);

    if (id) {
        _mongocrypt_buffer_copy_to(id, &attr->id);
    }
    attr->alt_names = _mongocrypt_key_alt_name_copy_all(alt_names);
    return attr;
}

void _mongocrypt_cache_key_attr_destroy(_mongocrypt_cache_key_attr_t *attr) {
    if (!attr) {
        return;
    }
    _mongocrypt_buffer_cleanup(&attr->id);
    _mongocrypt_key_alt_name_destroy_all(attr->alt_names);
    bson_free(attr);
}

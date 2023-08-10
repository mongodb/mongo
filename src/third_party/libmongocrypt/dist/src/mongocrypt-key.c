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

#include "mongocrypt-key-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-util-private.h" // mc_iter_document_as_bson

/* Check if two single entries are equal (i.e. ignore the 'next' pointer). */
static bool _one_key_alt_name_equal(_mongocrypt_key_alt_name_t *ptr_a, _mongocrypt_key_alt_name_t *ptr_b) {
    BSON_ASSERT_PARAM(ptr_a);
    BSON_ASSERT_PARAM(ptr_b);
    BSON_ASSERT(ptr_a->value.value_type == BSON_TYPE_UTF8);
    BSON_ASSERT(ptr_b->value.value_type == BSON_TYPE_UTF8);
    return 0 == strcmp(_mongocrypt_key_alt_name_get_string(ptr_a), _mongocrypt_key_alt_name_get_string(ptr_b));
}

static bool _find(_mongocrypt_key_alt_name_t *list, _mongocrypt_key_alt_name_t *entry) {
    BSON_ASSERT_PARAM(entry);

    for (; NULL != list; list = list->next) {
        if (_one_key_alt_name_equal(list, entry)) {
            return true;
        }
    }
    return false;
}

static uint32_t _list_len(_mongocrypt_key_alt_name_t *list) {
    uint32_t count = 0;

    while (NULL != list && count < UINT32_MAX) {
        count++;
        list = list->next;
    }
    return count;
}

static bool _check_unique(_mongocrypt_key_alt_name_t *list) {
    for (; NULL != list; list = list->next) {
        /* Check if we can find the current entry in the remaining. */
        if (_find(list->next, list)) {
            return false;
        }
    }
    return true;
}

static bool _parse_masterkey(bson_iter_t *iter, _mongocrypt_key_doc_t *out, mongocrypt_status_t *status) {
    bson_t kek_doc;

    BSON_ASSERT_PARAM(iter);
    BSON_ASSERT_PARAM(out);

    if (!BSON_ITER_HOLDS_DOCUMENT(iter)) {
        CLIENT_ERR("invalid 'masterKey', expected document");
        return false;
    }

    if (!mc_iter_document_as_bson(iter, &kek_doc, status)) {
        return false;
    }

    if (!_mongocrypt_kek_parse_owned(&kek_doc, &out->kek, status)) {
        return false;
    }
    return true;
}

bool _mongocrypt_key_alt_name_from_iter(const bson_iter_t *iter_in,
                                        _mongocrypt_key_alt_name_t **out,
                                        mongocrypt_status_t *status) {
    _mongocrypt_key_alt_name_t *key_alt_names = NULL, *tmp;
    bson_iter_t iter;

    BSON_ASSERT_PARAM(iter_in);
    BSON_ASSERT_PARAM(out);

    memcpy(&iter, iter_in, sizeof(iter));
    *out = NULL;

    /* A key parsed with no keyAltNames will have a zero'ed out bson value. Not
     * an error. */
    if (!BSON_ITER_HOLDS_ARRAY(&iter)) {
        CLIENT_ERR("malformed keyAltNames, expected array");
        return false;
    }

    if (!bson_iter_recurse(&iter, &iter)) {
        CLIENT_ERR("malformed keyAltNames, could not recurse into array");
        return false;
    }

    while (bson_iter_next(&iter)) {
        if (!BSON_ITER_HOLDS_UTF8(&iter)) {
            _mongocrypt_key_alt_name_destroy_all(key_alt_names);
            CLIENT_ERR("unexpected non-UTF8 keyAltName");
            return false;
        }

        tmp = _mongocrypt_key_alt_name_new(bson_iter_value(&iter));
        tmp->next = key_alt_names;
        key_alt_names = tmp;
    }

    if (!_check_unique(key_alt_names)) {
        _mongocrypt_key_alt_name_destroy_all(key_alt_names);
        CLIENT_ERR("unexpected duplicate keyAltNames");
        return false;
    }

    *out = key_alt_names;
    return true;
}

/* Takes ownership of all fields. */
bool _mongocrypt_key_parse_owned(const bson_t *bson, _mongocrypt_key_doc_t *out, mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_id = false, has_key_material = false, has_status = false, has_creation_date = false,
         has_update_date = false, has_master_key = false;

    BSON_ASSERT_PARAM(bson);
    BSON_ASSERT_PARAM(out);

    if (!bson_validate(bson, BSON_VALIDATE_NONE, NULL) || !bson_iter_init(&iter, bson)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }

    bson_destroy(&out->bson);
    bson_copy_to(bson, &out->bson);

    while (bson_iter_next(&iter)) {
        const char *field;

        field = bson_iter_key(&iter);
        if (!field) {
            CLIENT_ERR("invalid BSON, could not retrieve field name");
            return false;
        }
        if (0 == strcmp("_id", field)) {
            has_id = true;
            if (!_mongocrypt_buffer_copy_from_uuid_iter(&out->id, &iter)) {
                CLIENT_ERR("invalid key, '_id' is not a UUID");
                return false;
            }
            continue;
        }

        /* keyAltNames (optional) */
        if (0 == strcmp("keyAltNames", field)) {
            if (!_mongocrypt_key_alt_name_from_iter(&iter, &out->key_alt_names, status)) {
                return false;
            }
            continue;
        }

        if (0 == strcmp("keyMaterial", field)) {
            has_key_material = true;
            if (!_mongocrypt_buffer_copy_from_binary_iter(&out->key_material, &iter)) {
                CLIENT_ERR("invalid 'keyMaterial', expected binary");
                return false;
            }
            if (out->key_material.subtype != BSON_SUBTYPE_BINARY) {
                CLIENT_ERR("invalid 'keyMaterial', expected subtype 0");
                return false;
            }
            continue;
        }

        if (0 == strcmp("masterKey", field)) {
            has_master_key = true;
            if (!_parse_masterkey(&iter, out, status)) {
                return false;
            }
            continue;
        }

        if (0 == strcmp("version", field)) {
            if (!BSON_ITER_HOLDS_INT(&iter)) {
                CLIENT_ERR("invalid 'version', expect int");
                return false;
            }
            if (bson_iter_as_int64(&iter) != 0) {
                CLIENT_ERR("unsupported key document version, only supports version=0");
                return false;
            }
            continue;
        }

        if (0 == strcmp("status", field)) {
            /* Don't need status. Check that it's present and ignore it. */
            has_status = true;
            continue;
        }

        if (0 == strcmp("creationDate", field)) {
            has_creation_date = true;

            if (!BSON_ITER_HOLDS_DATE_TIME(&iter)) {
                CLIENT_ERR("invalid 'creationDate', expect datetime");
                return false;
            }

            out->creation_date = bson_iter_date_time(&iter);
            continue;
        }

        if (0 == strcmp("updateDate", field)) {
            has_update_date = true;

            if (!BSON_ITER_HOLDS_DATE_TIME(&iter)) {
                CLIENT_ERR("invalid 'updateDate', expect datetime");
                return false;
            }

            out->update_date = bson_iter_date_time(&iter);
            continue;
        }

        CLIENT_ERR("unrecognized field '%s'", field);
        return false;
    }

    /* Check that required fields were set. */
    if (!has_id) {
        CLIENT_ERR("invalid key, no '_id'");
        return false;
    }

    if (!has_master_key) {
        CLIENT_ERR("invalid key, no 'masterKey'");
        return false;
    }

    if (!has_key_material) {
        CLIENT_ERR("invalid key, no 'keyMaterial'");
        return false;
    }

    if (!has_status) {
        CLIENT_ERR("invalid key, no 'status'");
        return false;
    }

    if (!has_creation_date) {
        CLIENT_ERR("invalid key, no 'creationDate'");
        return false;
    }

    if (!has_update_date) {
        CLIENT_ERR("invalid key, no 'updateDate'");
        return false;
    }

    return true;
}

_mongocrypt_key_doc_t *_mongocrypt_key_new(void) {
    _mongocrypt_key_doc_t *key_doc;

    key_doc = (_mongocrypt_key_doc_t *)bson_malloc0(sizeof *key_doc);
    bson_init(&key_doc->bson);

    return key_doc;
}

void _mongocrypt_key_destroy(_mongocrypt_key_doc_t *key) {
    if (!key) {
        return;
    }

    _mongocrypt_buffer_cleanup(&key->id);
    _mongocrypt_key_alt_name_destroy_all(key->key_alt_names);
    _mongocrypt_buffer_cleanup(&key->key_material);
    _mongocrypt_kek_cleanup(&key->kek);

    bson_destroy(&key->bson);
    bson_free(key);
}

void _mongocrypt_key_doc_copy_to(_mongocrypt_key_doc_t *src, _mongocrypt_key_doc_t *dst) {
    BSON_ASSERT_PARAM(src);
    BSON_ASSERT_PARAM(dst);

    _mongocrypt_buffer_copy_to(&src->id, &dst->id);
    _mongocrypt_buffer_copy_to(&src->key_material, &dst->key_material);
    dst->key_alt_names = _mongocrypt_key_alt_name_copy_all(src->key_alt_names);
    bson_destroy(&dst->bson);
    bson_copy_to(&src->bson, &dst->bson);
    _mongocrypt_kek_copy_to(&src->kek, &dst->kek);
    dst->creation_date = src->creation_date;
    dst->update_date = src->update_date;
}

_mongocrypt_key_alt_name_t *_mongocrypt_key_alt_name_copy_all(_mongocrypt_key_alt_name_t *ptr) {
    _mongocrypt_key_alt_name_t *ptr_copy = NULL, *head = NULL;

    while (ptr) {
        _mongocrypt_key_alt_name_t *copied;
        copied = bson_malloc0(sizeof(*copied));
        BSON_ASSERT(copied);

        bson_value_copy(&ptr->value, &copied->value);

        if (!ptr_copy) {
            ptr_copy = copied;
            head = ptr_copy;
        } else {
            ptr_copy->next = copied;
            ptr_copy = ptr_copy->next;
        }
        ptr = ptr->next;
    }
    return head;
}

void _mongocrypt_key_alt_name_destroy_all(_mongocrypt_key_alt_name_t *ptr) {
    _mongocrypt_key_alt_name_t *next;
    while (ptr) {
        next = ptr->next;
        bson_value_destroy(&ptr->value);
        bson_free(ptr);
        ptr = next;
    }
}

bool _mongocrypt_key_alt_name_intersects(_mongocrypt_key_alt_name_t *ptr_a, _mongocrypt_key_alt_name_t *ptr_b) {
    _mongocrypt_key_alt_name_t *orig_ptr_b = ptr_b;

    if (!ptr_a || !ptr_b) {
        return false;
    }

    for (; ptr_a; ptr_a = ptr_a->next) {
        for (ptr_b = orig_ptr_b; ptr_b; ptr_b = ptr_b->next) {
            if (_one_key_alt_name_equal(ptr_a, ptr_b)) {
                return true;
            }
        }
    }
    return false;
}

_mongocrypt_key_alt_name_t *_mongocrypt_key_alt_name_create(const char *name, ...) {
    va_list args;
    const char *arg_ptr;
    _mongocrypt_key_alt_name_t *head, *prev;

    head = NULL;
    prev = NULL;
    va_start(args, name);
    arg_ptr = name;
    while (arg_ptr) {
        _mongocrypt_key_alt_name_t *curr;

        curr = bson_malloc0(sizeof(*curr));
        BSON_ASSERT(curr);

        curr->value.value_type = BSON_TYPE_UTF8;
        curr->value.value.v_utf8.str = bson_strdup(arg_ptr);
        curr->value.value.v_utf8.len = (uint32_t)strlen(arg_ptr);
        if (!prev) {
            head = curr;
        } else {
            prev->next = curr;
        }

        arg_ptr = va_arg(args, const char *);
        prev = curr;
    }
    va_end(args);

    return head;
}

_mongocrypt_key_alt_name_t *_mongocrypt_key_alt_name_new(const bson_value_t *value) {
    BSON_ASSERT_PARAM(value);

    _mongocrypt_key_alt_name_t *name = bson_malloc0(sizeof(*name));
    BSON_ASSERT(name);

    bson_value_copy(value, &name->value);
    return name;
}

bool _mongocrypt_key_alt_name_unique_list_equal(_mongocrypt_key_alt_name_t *list_a,
                                                _mongocrypt_key_alt_name_t *list_b) {
    _mongocrypt_key_alt_name_t *ptr;

    BSON_ASSERT(_check_unique(list_a));
    BSON_ASSERT(_check_unique(list_b));
    if (_list_len(list_a) != _list_len(list_b)) {
        return false;
    }
    for (ptr = list_a; NULL != ptr; ptr = ptr->next) {
        if (!_find(list_b, ptr)) {
            return false;
        }
    }
    return true;
}

const char *_mongocrypt_key_alt_name_get_string(_mongocrypt_key_alt_name_t *key_alt_name) {
    BSON_ASSERT_PARAM(key_alt_name);

    return key_alt_name->value.value.v_utf8.str;
}

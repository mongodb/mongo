/*
 * Copyright 2022-present MongoDB, Inc.
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

#include "mc-efc-private.h"

#include "mlib/str.h"
#include "mongocrypt-private.h"
#include "mongocrypt-util-private.h" // mc_iter_document_as_bson

static bool _parse_query_type_string(const char *queryType, supported_query_type_flags *out) {
    BSON_ASSERT_PARAM(queryType);
    BSON_ASSERT_PARAM(out);

    mstr_view qtv = mstrv_view_cstr(queryType);

    if (mstr_eq_ignore_case(mstrv_lit(MONGOCRYPT_QUERY_TYPE_EQUALITY_STR), qtv)) {
        *out = SUPPORTS_EQUALITY_QUERIES;
    } else if (mstr_eq_ignore_case(mstrv_lit(MONGOCRYPT_QUERY_TYPE_RANGE_STR), qtv)) {
        *out = SUPPORTS_RANGE_QUERIES;
    } else if (mstr_eq_ignore_case(mstrv_lit(MONGOCRYPT_QUERY_TYPE_RANGEPREVIEW_DEPRECATED_STR), qtv)) {
        *out = SUPPORTS_RANGE_PREVIEW_DEPRECATED_QUERIES;
    } else {
        return false;
    }

    return true;
}

static bool
_parse_supported_query_types(bson_iter_t *iter, supported_query_type_flags *out, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iter);
    BSON_ASSERT_PARAM(out);
    if (!BSON_ITER_HOLDS_DOCUMENT(iter)) {
        CLIENT_ERR("When parsing supported query types: Expected type document, got: %d", bson_iter_type(iter));
        return false;
    }

    bson_t query_doc;
    if (!mc_iter_document_as_bson(iter, &query_doc, status)) {
        return false;
    }
    bson_iter_t query_type_iter;
    if (!bson_iter_init_find(&query_type_iter, &query_doc, "queryType")) {
        CLIENT_ERR("When parsing supported query types: Unable to find 'queryType' in query document");
        return false;
    }
    if (!BSON_ITER_HOLDS_UTF8(&query_type_iter)) {
        CLIENT_ERR("When parsing supported query types: Expected 'queryType' to be type UTF-8, got: %d",
                   bson_iter_type(&query_type_iter));
        return false;
    }
    const char *queryType = bson_iter_utf8(&query_type_iter, NULL /* length */);
    if (!_parse_query_type_string(queryType, out)) {
        CLIENT_ERR("When parsing supported query types: Did not recognize query type '%s'", queryType);
        return false;
    }
    return true;
}

/* _parse_field parses and prepends one field document to efc->fields. */
static bool
_parse_field(mc_EncryptedFieldConfig_t *efc, bson_t *field, mongocrypt_status_t *status, bool use_range_v2) {
    supported_query_type_flags query_types = SUPPORTS_NO_QUERIES;
    bson_iter_t field_iter;

    BSON_ASSERT_PARAM(efc);
    BSON_ASSERT_PARAM(field);

    if (!bson_iter_init_find(&field_iter, field, "keyId")) {
        CLIENT_ERR("unable to find 'keyId' in 'field' document");
        return false;
    }
    if (!BSON_ITER_HOLDS_BINARY(&field_iter)) {
        CLIENT_ERR("expected 'fields.keyId' to be type binary, got: %d", bson_iter_type(&field_iter));
        return false;
    }
    _mongocrypt_buffer_t field_keyid;
    if (!_mongocrypt_buffer_from_uuid_iter(&field_keyid, &field_iter)) {
        CLIENT_ERR("unable to parse uuid key from 'fields.keyId'");
        return false;
    }

    const char *field_path;
    if (!bson_iter_init_find(&field_iter, field, "path")) {
        CLIENT_ERR("unable to find 'path' in 'field' document");
        return false;
    }
    if (!BSON_ITER_HOLDS_UTF8(&field_iter)) {
        CLIENT_ERR("expected 'fields.path' to be type UTF-8, got: %d", bson_iter_type(&field_iter));
        return false;
    }
    field_path = bson_iter_utf8(&field_iter, NULL /* length */);

    if (bson_iter_init_find(&field_iter, field, "queries")) {
        if (BSON_ITER_HOLDS_ARRAY(&field_iter)) {
            // Multiple queries, iterate through and grab all query types.
            uint32_t queries_buf_len;
            const uint8_t *queries_buf;
            bson_t queries_arr;
            bson_iter_array(&field_iter, &queries_buf_len, &queries_buf);
            if (!bson_init_static(&queries_arr, queries_buf, queries_buf_len)) {
                CLIENT_ERR("Failed to parse 'queries' field");
                return false;
            }

            bson_iter_t queries_iter;
            bson_iter_init(&queries_iter, &queries_arr);
            while (bson_iter_next(&queries_iter)) {
                supported_query_type_flags flag;
                if (!_parse_supported_query_types(&queries_iter, &flag, status)) {
                    return false;
                }
                query_types |= flag;
            }
        } else {
            supported_query_type_flags flag;
            if (!_parse_supported_query_types(&field_iter, &flag, status)) {
                return false;
            }
            query_types |= flag;
        }
    }

    if (query_types & SUPPORTS_RANGE_PREVIEW_DEPRECATED_QUERIES && use_range_v2) {
        // When rangev2 is enabled ("range") error if "rangePreview" is included.
        // This check is intended to give an easier-to-understand earlier error.
        CLIENT_ERR("Cannot use field '%s' with 'rangePreview' queries. 'rangePreview' is unsupported. Use 'range' "
                   "instead. 'range' is not compatible with 'rangePreview' and requires recreating the collection.",
                   field_path);
        return false;
    }

    /* Prepend a new mc_EncryptedField_t */
    mc_EncryptedField_t *ef = bson_malloc0(sizeof(mc_EncryptedField_t));
    _mongocrypt_buffer_copy_to(&field_keyid, &ef->keyId);
    ef->path = bson_strdup(field_path);
    ef->next = efc->fields;
    ef->supported_queries = query_types;
    efc->fields = ef;

    return true;
}

bool mc_EncryptedFieldConfig_parse(mc_EncryptedFieldConfig_t *efc,
                                   const bson_t *efc_bson,
                                   mongocrypt_status_t *status,
                                   bool use_range_v2) {
    bson_iter_t iter;

    BSON_ASSERT_PARAM(efc);
    BSON_ASSERT_PARAM(efc_bson);

    memset(efc, 0, sizeof(*efc));
    if (!bson_iter_init_find(&iter, efc_bson, "fields")) {
        CLIENT_ERR("unable to find 'fields' in encrypted_field_config");
        return false;
    }
    if (!BSON_ITER_HOLDS_ARRAY(&iter)) {
        CLIENT_ERR("expected 'fields' to be type array, got: %d", bson_iter_type(&iter));
        return false;
    }
    if (!bson_iter_recurse(&iter, &iter)) {
        CLIENT_ERR("unable to recurse into encrypted_field_config 'fields'");
        return false;
    }
    while (bson_iter_next(&iter)) {
        bson_t field;
        if (!mc_iter_document_as_bson(&iter, &field, status)) {
            return false;
        }
        if (!_parse_field(efc, &field, status, use_range_v2)) {
            return false;
        }
    }
    return true;
}

void mc_EncryptedFieldConfig_cleanup(mc_EncryptedFieldConfig_t *efc) {
    if (!efc) {
        return;
    }
    mc_EncryptedField_t *ptr = efc->fields;
    while (ptr != NULL) {
        mc_EncryptedField_t *ptr_next = ptr->next;
        _mongocrypt_buffer_cleanup(&ptr->keyId);
        bson_free((char *)ptr->path);
        bson_free(ptr);
        ptr = ptr_next;
    }
}

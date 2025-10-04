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

#include <bson/bson.h>

#include "mc-fle2-insert-update-payload-private-v2.h"
#include "mc-parse-utils-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-util-private.h" // mc_bson_type_to_string
#include "mongocrypt.h"

#define DEF_TEXT_SEARCH_TOKEN_SET_INIT_CLEANUP(Type)                                                                   \
    void mc_Text##Type##TokenSet_init(mc_Text##Type##TokenSet_t *ts) {                                                 \
        BSON_ASSERT_PARAM(ts);                                                                                         \
        memset(ts, 0, sizeof(mc_Text##Type##TokenSet_t));                                                              \
    }                                                                                                                  \
    void mc_Text##Type##TokenSet_cleanup(mc_Text##Type##TokenSet_t *ts) {                                              \
        BSON_ASSERT_PARAM(ts);                                                                                         \
        _mongocrypt_buffer_cleanup(&ts->edcDerivedToken);                                                              \
        _mongocrypt_buffer_cleanup(&ts->escDerivedToken);                                                              \
        _mongocrypt_buffer_cleanup(&ts->serverDerivedFromDataToken);                                                   \
        _mongocrypt_buffer_cleanup(&ts->encryptedTokens);                                                              \
    }                                                                                                                  \
    void mc_Text##Type##TokenSet_shallow_copy(const mc_Text##Type##TokenSet_t *src, mc_Text##Type##TokenSet_t *dst) {  \
        BSON_ASSERT_PARAM(src);                                                                                        \
        BSON_ASSERT_PARAM(dst);                                                                                        \
        _mongocrypt_buffer_set_to(&src->edcDerivedToken, &dst->edcDerivedToken);                                       \
        _mongocrypt_buffer_set_to(&src->escDerivedToken, &dst->escDerivedToken);                                       \
        _mongocrypt_buffer_set_to(&src->serverDerivedFromDataToken, &dst->serverDerivedFromDataToken);                 \
        _mongocrypt_buffer_set_to(&src->encryptedTokens, &dst->encryptedTokens);                                       \
    }

DEF_TEXT_SEARCH_TOKEN_SET_INIT_CLEANUP(Exact)
DEF_TEXT_SEARCH_TOKEN_SET_INIT_CLEANUP(Substring)
DEF_TEXT_SEARCH_TOKEN_SET_INIT_CLEANUP(Suffix)
DEF_TEXT_SEARCH_TOKEN_SET_INIT_CLEANUP(Prefix)
#undef DEF_TEXT_SEARCH_TOKEN_SET_INIT_CLEANUP

void mc_TextSearchTokenSets_init(mc_TextSearchTokenSets_t *tsts) {
    BSON_ASSERT_PARAM(tsts);
    mc_TextExactTokenSet_init(&tsts->exact);
    _mc_array_init(&tsts->substringArray, sizeof(mc_TextSubstringTokenSet_t));
    _mc_array_init(&tsts->suffixArray, sizeof(mc_TextSuffixTokenSet_t));
    _mc_array_init(&tsts->prefixArray, sizeof(mc_TextPrefixTokenSet_t));
}

void mc_TextSearchTokenSets_cleanup(mc_TextSearchTokenSets_t *tsts) {
    BSON_ASSERT_PARAM(tsts);
    mc_TextExactTokenSet_cleanup(&tsts->exact);

    for (size_t i = 0; i < tsts->substringArray.len; i++) {
        mc_TextSubstringTokenSet_t *entry = &_mc_array_index(&tsts->substringArray, mc_TextSubstringTokenSet_t, i);
        mc_TextSubstringTokenSet_cleanup(entry);
    }
    _mc_array_destroy(&tsts->substringArray);

    for (size_t i = 0; i < tsts->suffixArray.len; i++) {
        mc_TextSuffixTokenSet_t *entry = &_mc_array_index(&tsts->suffixArray, mc_TextSuffixTokenSet_t, i);
        mc_TextSuffixTokenSet_cleanup(entry);
    }
    _mc_array_destroy(&tsts->suffixArray);

    // Free all prefix token set entries.
    for (size_t i = 0; i < tsts->prefixArray.len; i++) {
        mc_TextPrefixTokenSet_t *entry = &_mc_array_index(&tsts->prefixArray, mc_TextPrefixTokenSet_t, i);
        mc_TextPrefixTokenSet_cleanup(entry);
    }
    _mc_array_destroy(&tsts->prefixArray);
}

void mc_FLE2InsertUpdatePayloadV2_init(mc_FLE2InsertUpdatePayloadV2_t *payload) {
    BSON_ASSERT_PARAM(payload);

    memset(payload, 0, sizeof(mc_FLE2InsertUpdatePayloadV2_t));
    _mc_array_init(&payload->edgeTokenSetArray, sizeof(mc_EdgeTokenSetV2_t));
    mc_TextSearchTokenSets_init(&payload->textSearchTokenSets.tsts);
}

static void mc_EdgeTokenSetV2_cleanup(mc_EdgeTokenSetV2_t *etc) {
    BSON_ASSERT_PARAM(etc);

    _mongocrypt_buffer_cleanup(&etc->edcDerivedToken);
    _mongocrypt_buffer_cleanup(&etc->escDerivedToken);
    _mongocrypt_buffer_cleanup(&etc->serverDerivedFromDataToken);
    _mongocrypt_buffer_cleanup(&etc->encryptedTokens);
}

void mc_FLE2InsertUpdatePayloadV2_cleanup(mc_FLE2InsertUpdatePayloadV2_t *payload) {
    BSON_ASSERT_PARAM(payload);

    _mongocrypt_buffer_cleanup(&payload->edcDerivedToken);
    _mongocrypt_buffer_cleanup(&payload->escDerivedToken);
    _mongocrypt_buffer_cleanup(&payload->encryptedTokens);
    _mongocrypt_buffer_cleanup(&payload->indexKeyId);
    _mongocrypt_buffer_cleanup(&payload->value);
    _mongocrypt_buffer_cleanup(&payload->serverEncryptionToken);
    _mongocrypt_buffer_cleanup(&payload->serverDerivedFromDataToken);
    _mongocrypt_buffer_cleanup(&payload->plaintext);
    // Free all EdgeTokenSet entries.
    for (size_t i = 0; i < payload->edgeTokenSetArray.len; i++) {
        mc_EdgeTokenSetV2_t entry = _mc_array_index(&payload->edgeTokenSetArray, mc_EdgeTokenSetV2_t, i);
        mc_EdgeTokenSetV2_cleanup(&entry);
    }
    _mc_array_destroy(&payload->edgeTokenSetArray);
    bson_value_destroy(&payload->indexMin);
    bson_value_destroy(&payload->indexMax);
    mc_TextSearchTokenSets_cleanup(&payload->textSearchTokenSets.tsts);
}

#define IF_FIELD(Name)                                                                                                 \
    if (0 == strcmp(field, #Name)) {                                                                                   \
        if (has_##Name) {                                                                                              \
            CLIENT_ERR("Duplicate field '" #Name "' in payload bson");                                                 \
            goto fail;                                                                                                 \
        }                                                                                                              \
        has_##Name = true;

#define END_IF_FIELD                                                                                                   \
    continue;                                                                                                          \
    }

#define PARSE_BINDATA(Name, Type, Dest)                                                                                \
    IF_FIELD(Name) {                                                                                                   \
        if (!parse_bindata(Type, &iter, &out->Dest, status)) {                                                         \
            goto fail;                                                                                                 \
        }                                                                                                              \
    }                                                                                                                  \
    END_IF_FIELD

#define PARSE_BINARY(Name, Dest) PARSE_BINDATA(Name, BSON_SUBTYPE_BINARY, Dest)

#define CHECK_HAS(Name)                                                                                                \
    if (!has_##Name) {                                                                                                 \
        CLIENT_ERR("Missing field '" #Name "' in payload");                                                            \
        goto fail;                                                                                                     \
    } else                                                                                                             \
        ((void)0)

bool mc_FLE2InsertUpdatePayloadV2_parse(mc_FLE2InsertUpdatePayloadV2_t *out,
                                        const _mongocrypt_buffer_t *in,
                                        mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_d = false, has_s = false, has_p = false;
    bool has_u = false, has_t = false, has_v = false;
    bool has_e = false, has_l = false, has_k = false;
    bool has_sp = false, has_pn = false, has_tf = false, has_mn = false, has_mx = false;
    bson_t in_bson;

    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    if (in->len < 1) {
        CLIENT_ERR("FLE2InsertUpdatePayloadV2_parse got too short input");
        return false;
    }

    if (!bson_init_static(&in_bson, in->data + 1, in->len - 1)) {
        CLIENT_ERR("FLE2InsertUpdatePayloadV2_parse got invalid BSON");
        return false;
    }

    if (!bson_validate(&in_bson, BSON_VALIDATE_NONE, NULL) || !bson_iter_init(&iter, &in_bson)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        PARSE_BINARY(d, edcDerivedToken)
        PARSE_BINARY(s, escDerivedToken)
        PARSE_BINARY(p, encryptedTokens)
        PARSE_BINDATA(u, BSON_SUBTYPE_UUID, indexKeyId)
        IF_FIELD(t) {
            int32_t type = bson_iter_int32(&iter);
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("Field 't' expected to hold an int32");
                goto fail;
            }
            if ((type < 0) || (type > 0xFF)) {
                CLIENT_ERR("Field 't' must be a valid BSON type, got: %d", type);
                goto fail;
            }
            out->valueType = (bson_type_t)type;
        }
        END_IF_FIELD

        IF_FIELD(k) {
            int64_t contention = bson_iter_int64(&iter);
            if (!BSON_ITER_HOLDS_INT64(&iter)) {
                CLIENT_ERR("Field 'k' expected to hold an int64");
                goto fail;
            }
            if ((contention < 0)) {
                CLIENT_ERR("Field 'k' must be non-negative, got: %" PRId64, contention);
                goto fail;
            }
            out->contentionFactor = contention;
        }
        END_IF_FIELD

        PARSE_BINARY(v, value)
        PARSE_BINARY(e, serverEncryptionToken)
        PARSE_BINARY(l, serverDerivedFromDataToken)

        IF_FIELD(sp) {
            if (!BSON_ITER_HOLDS_INT64(&iter)) {
                CLIENT_ERR("Field 'sp' expected to hold an int64, got: %s",
                           mc_bson_type_to_string(bson_iter_type(&iter)));
                goto fail;
            }
            int64_t sparsity = bson_iter_int64(&iter);
            out->sparsity = OPT_I64(sparsity);
        }
        END_IF_FIELD

        IF_FIELD(pn) {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("Field 'pn' expected to hold an int32, got: %s",
                           mc_bson_type_to_string(bson_iter_type(&iter)));
                goto fail;
            }
            int32_t precision = bson_iter_int32(&iter);
            if (precision < 0) {
                CLIENT_ERR("Field 'pn' must be non-negative, got: %" PRId32, precision);
                goto fail;
            }
            out->precision = OPT_I32(precision);
        }
        END_IF_FIELD

        IF_FIELD(tf) {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("Field 'tf' expected to hold an int32, got: %s",
                           mc_bson_type_to_string(bson_iter_type(&iter)));
                goto fail;
            }
            int32_t trimFactor = bson_iter_int32(&iter);
            if (trimFactor < 0) {
                CLIENT_ERR("Field 'tf' must be non-negative, got: %" PRId32, trimFactor);
                goto fail;
            }
            out->trimFactor = OPT_I32(trimFactor);
        }
        END_IF_FIELD

        IF_FIELD(mn) {
            bson_value_copy(bson_iter_value(&iter), &out->indexMin);
        }
        END_IF_FIELD

        IF_FIELD(mx) {
            bson_value_copy(bson_iter_value(&iter), &out->indexMax);
        }
        END_IF_FIELD
    }

    CHECK_HAS(d);
    CHECK_HAS(s);
    CHECK_HAS(p);
    CHECK_HAS(u);
    CHECK_HAS(t);
    CHECK_HAS(v);
    CHECK_HAS(e);
    CHECK_HAS(l);
    CHECK_HAS(k);
    // The fields `sp`, `pn`, `tf`, `mn`, and `mx` are only set for "range" payloads.

    if (!_mongocrypt_buffer_from_subrange(&out->userKeyId, &out->value, 0, UUID_LEN)) {
        CLIENT_ERR("failed to create userKeyId buffer");
        goto fail;
    }
    out->userKeyId.subtype = BSON_SUBTYPE_UUID;

    return true;
fail:
    return false;
}

#define IUPS_APPEND_BINDATA(dst, name, subtype, value)                                                                 \
    if (!_mongocrypt_buffer_append(&(value), dst, name, -1)) {                                                         \
        return false;                                                                                                  \
    } else                                                                                                             \
        ((void)0)

bool mc_FLE2InsertUpdatePayloadV2_serialize(const mc_FLE2InsertUpdatePayloadV2_t *payload, bson_t *out) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(payload);

    IUPS_APPEND_BINDATA(out, "d", BSON_SUBTYPE_BINARY, payload->edcDerivedToken);
    IUPS_APPEND_BINDATA(out, "s", BSON_SUBTYPE_BINARY, payload->escDerivedToken);
    IUPS_APPEND_BINDATA(out, "p", BSON_SUBTYPE_BINARY, payload->encryptedTokens);
    IUPS_APPEND_BINDATA(out, "u", BSON_SUBTYPE_UUID, payload->indexKeyId);
    if (!BSON_APPEND_INT32(out, "t", payload->valueType)) {
        return false;
    }
    IUPS_APPEND_BINDATA(out, "v", BSON_SUBTYPE_BINARY, payload->value);
    IUPS_APPEND_BINDATA(out, "e", BSON_SUBTYPE_BINARY, payload->serverEncryptionToken);
    IUPS_APPEND_BINDATA(out, "l", BSON_SUBTYPE_BINARY, payload->serverDerivedFromDataToken);
    if (!BSON_APPEND_INT64(out, "k", payload->contentionFactor)) {
        return false;
    }

    return true;
}

bool mc_FLE2InsertUpdatePayloadV2_serializeForRange(const mc_FLE2InsertUpdatePayloadV2_t *payload, bson_t *out) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(payload);

    if (!mc_FLE2InsertUpdatePayloadV2_serialize(payload, out)) {
        return false;
    }
    // Append "g" array of EdgeTokenSets.
    bson_t g_bson;
    if (!BSON_APPEND_ARRAY_BEGIN(out, "g", &g_bson)) {
        return false;
    }

    uint32_t g_index = 0;
    for (size_t i = 0; i < payload->edgeTokenSetArray.len; i++) {
        mc_EdgeTokenSetV2_t etc = _mc_array_index(&payload->edgeTokenSetArray, mc_EdgeTokenSetV2_t, i);
        bson_t etc_bson;

        const char *g_index_string;
        char storage[16];
        bson_uint32_to_string(g_index, &g_index_string, storage, sizeof(storage));

        if (!BSON_APPEND_DOCUMENT_BEGIN(&g_bson, g_index_string, &etc_bson)) {
            return false;
        }

        IUPS_APPEND_BINDATA(&etc_bson, "d", BSON_SUBTYPE_BINARY, etc.edcDerivedToken);
        IUPS_APPEND_BINDATA(&etc_bson, "s", BSON_SUBTYPE_BINARY, etc.escDerivedToken);
        IUPS_APPEND_BINDATA(&etc_bson, "l", BSON_SUBTYPE_BINARY, etc.serverDerivedFromDataToken);
        IUPS_APPEND_BINDATA(&etc_bson, "p", BSON_SUBTYPE_BINARY, etc.encryptedTokens);

        if (!bson_append_document_end(&g_bson, &etc_bson)) {
            return false;
        }
        if (g_index == UINT32_MAX) {
            break;
        }
        g_index++;
    }

    if (!bson_append_array_end(out, &g_bson)) {
        return false;
    }

    // Encode parameters that were used to generate the payload.
    BSON_ASSERT(payload->sparsity.set);
    if (!BSON_APPEND_INT64(out, "sp", payload->sparsity.value)) {
        return false;
    }

    // Precision may be unset.
    if (payload->precision.set) {
        if (!BSON_APPEND_INT32(out, "pn", payload->precision.value)) {
            return false;
        }
    }

    BSON_ASSERT(payload->trimFactor.set);
    if (!BSON_APPEND_INT32(out, "tf", payload->trimFactor.value)) {
        return false;
    }

    BSON_ASSERT(payload->indexMin.value_type != BSON_TYPE_EOD);
    if (!BSON_APPEND_VALUE(out, "mn", &payload->indexMin)) {
        return false;
    }

    BSON_ASSERT(payload->indexMax.value_type != BSON_TYPE_EOD);
    if (!BSON_APPEND_VALUE(out, "mx", &payload->indexMax)) {
        return false;
    }

    return true;
}

#define SERIALIZE_TEXT_TOKEN_SET_FOR_TYPE_IMPL(Type)                                                                   \
    static bool _fle2_serialize_Text##Type##TokenSet(bson_t *parent,                                                   \
                                                     const char *field_name,                                           \
                                                     const mc_Text##Type##TokenSet_t *ts) {                            \
        BSON_ASSERT_PARAM(parent);                                                                                     \
        BSON_ASSERT_PARAM(field_name);                                                                                 \
        BSON_ASSERT_PARAM(ts);                                                                                         \
        bson_t child;                                                                                                  \
        if (!BSON_APPEND_DOCUMENT_BEGIN(parent, field_name, &child)) {                                                 \
            return false;                                                                                              \
        }                                                                                                              \
        IUPS_APPEND_BINDATA(&child, "d", BSON_SUBTYPE_BINARY, ts->edcDerivedToken);                                    \
        IUPS_APPEND_BINDATA(&child, "s", BSON_SUBTYPE_BINARY, ts->escDerivedToken);                                    \
        IUPS_APPEND_BINDATA(&child, "l", BSON_SUBTYPE_BINARY, ts->serverDerivedFromDataToken);                         \
        IUPS_APPEND_BINDATA(&child, "p", BSON_SUBTYPE_BINARY, ts->encryptedTokens);                                    \
        if (!bson_append_document_end(parent, &child)) {                                                               \
            return false;                                                                                              \
        }                                                                                                              \
        return true;                                                                                                   \
    }
SERIALIZE_TEXT_TOKEN_SET_FOR_TYPE_IMPL(Exact)
SERIALIZE_TEXT_TOKEN_SET_FOR_TYPE_IMPL(Substring)
SERIALIZE_TEXT_TOKEN_SET_FOR_TYPE_IMPL(Suffix)
SERIALIZE_TEXT_TOKEN_SET_FOR_TYPE_IMPL(Prefix)
#undef SERIALIZE_TEXT_TOKEN_SET_FOR_TYPE_IMPL

#undef IUPS_APPEND_BINDATA

#define SERIALIZE_ARRAY_OF_TEXT_TOKEN_SET_FOR_TYPE_IMPL(Type)                                                          \
    static bool _fle2_serialize_array_of_Text##Type##TokenSet(bson_t *parent,                                          \
                                                              const char *array_name,                                  \
                                                              const mc_array_t *array) {                               \
        BSON_ASSERT_PARAM(parent);                                                                                     \
        BSON_ASSERT_PARAM(array_name);                                                                                 \
        BSON_ASSERT_PARAM(array);                                                                                      \
        bson_t arr_bson;                                                                                               \
        if (!BSON_APPEND_ARRAY_BEGIN(parent, array_name, &arr_bson)) {                                                 \
            return false;                                                                                              \
        }                                                                                                              \
                                                                                                                       \
        const char *index_string = NULL;                                                                               \
        char storage[16];                                                                                              \
        uint32_t index = 0;                                                                                            \
        uint32_t limit = (array->len > UINT32_MAX) ? UINT32_MAX : (uint32_t)array->len;                                \
        for (; index < limit; index++) {                                                                               \
            mc_Text##Type##TokenSet_t ts = _mc_array_index(array, mc_Text##Type##TokenSet_t, index);                   \
            bson_uint32_to_string(index, &index_string, storage, sizeof(storage));                                     \
                                                                                                                       \
            if (!_fle2_serialize_Text##Type##TokenSet(&arr_bson, index_string, &ts)) {                                 \
                return false;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
        if (!bson_append_array_end(parent, &arr_bson)) {                                                               \
            return false;                                                                                              \
        }                                                                                                              \
        return true;                                                                                                   \
    }
SERIALIZE_ARRAY_OF_TEXT_TOKEN_SET_FOR_TYPE_IMPL(Substring)
SERIALIZE_ARRAY_OF_TEXT_TOKEN_SET_FOR_TYPE_IMPL(Suffix)
SERIALIZE_ARRAY_OF_TEXT_TOKEN_SET_FOR_TYPE_IMPL(Prefix)
#undef SERIALIZE_ARRAY_OF_TEXT_TOKEN_SET_FOR_TYPE_IMPL

bool mc_FLE2InsertUpdatePayloadV2_serializeForTextSearch(const mc_FLE2InsertUpdatePayloadV2_t *payload, bson_t *out) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(payload);
    BSON_ASSERT(payload->textSearchTokenSets.set);

    if (!mc_FLE2InsertUpdatePayloadV2_serialize(payload, out)) {
        return false;
    }

    const mc_TextSearchTokenSets_t *tsts = &payload->textSearchTokenSets.tsts;

    // Start serializing "b", the TextSearchTokenSets
    bson_t b_bson;
    if (!BSON_APPEND_DOCUMENT_BEGIN(out, "b", &b_bson)) {
        return false;
    }

    // Serialize "b.e", the TextExactTokenSet
    if (!_fle2_serialize_TextExactTokenSet(&b_bson, "e", &tsts->exact)) {
        return false;
    }

    // Serialize "b.s", the array of TextSubstringTokenSet
    if (!_fle2_serialize_array_of_TextSubstringTokenSet(&b_bson, "s", &tsts->substringArray)) {
        return false;
    }

    // Serialize "b.u", the array of TextSuffixTokenSet
    if (!_fle2_serialize_array_of_TextSuffixTokenSet(&b_bson, "u", &tsts->suffixArray)) {
        return false;
    }

    // Serialize "b.p", the array of TextPrefixTokenSet
    if (!_fle2_serialize_array_of_TextPrefixTokenSet(&b_bson, "p", &tsts->prefixArray)) {
        return false;
    }

    // End serializing "b", the TextSearchTokenSets
    if (!bson_append_document_end(out, &b_bson)) {
        return false;
    }
    return true;
}

const _mongocrypt_buffer_t *mc_FLE2InsertUpdatePayloadV2_decrypt(_mongocrypt_crypto_t *crypto,
                                                                 mc_FLE2InsertUpdatePayloadV2_t *iup,
                                                                 const _mongocrypt_buffer_t *user_key,
                                                                 mongocrypt_status_t *status) {
    const _mongocrypt_value_encryption_algorithm_t *fle2v2 = _mcFLE2v2AEADAlgorithm();
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(iup);
    BSON_ASSERT_PARAM(user_key);

    if (iup->value.len == 0) {
        CLIENT_ERR("FLE2InsertUpdatePayloadV2 value not parsed");
        return NULL;
    }

    _mongocrypt_buffer_t ciphertext;
    BSON_ASSERT(iup->value.len >= UUID_LEN);
    if (!_mongocrypt_buffer_from_subrange(&ciphertext, &iup->value, UUID_LEN, iup->value.len - UUID_LEN)) {
        CLIENT_ERR("Failed to create ciphertext buffer");
        return NULL;
    }

    _mongocrypt_buffer_resize(&iup->plaintext, fle2v2->get_plaintext_len(ciphertext.len, status));
    uint32_t bytes_written;

    if (!fle2v2->do_decrypt(crypto, &iup->userKeyId, user_key, &ciphertext, &iup->plaintext, &bytes_written, status)) {
        return NULL;
    }
    iup->plaintext.len = bytes_written;
    return &iup->plaintext;
}

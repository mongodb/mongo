/*
 * Copyright 2025-present MongoDB, Inc.
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
#include <stdlib.h>

#include "mc-fle2-find-text-payload-private.h"
#include "mc-parse-utils-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-util-private.h"
#include "mongocrypt.h"

#define IF_FIELD(Name)                                                                                                 \
    if (0 == strcmp(field, #Name)) {                                                                                   \
        if (has_##Name) {                                                                                              \
            CLIENT_ERR("Error parsing %s: Duplicate field '" #Name "'", class_name);                                   \
            goto fail;                                                                                                 \
        }                                                                                                              \
        has_##Name = true;

#define END_IF_FIELD                                                                                                   \
    continue;                                                                                                          \
    }

#define CHECK_HAS(Name)                                                                                                \
    if (!has_##Name) {                                                                                                 \
        CLIENT_ERR("Error parsing %s: Missing required field '" #Name "'", class_name);                                \
        goto fail;                                                                                                     \
    }

#define PARSE_BINARY(Name, Dest)                                                                                       \
    IF_FIELD(Name) {                                                                                                   \
        if (!parse_bindata(BSON_SUBTYPE_BINARY, &iter, Dest, status)) {                                                \
            goto fail;                                                                                                 \
        }                                                                                                              \
    }                                                                                                                  \
    END_IF_FIELD

typedef struct {
    _mongocrypt_buffer_t *edcDerivedToken;
    _mongocrypt_buffer_t *escDerivedToken;
    _mongocrypt_buffer_t *serverDerivedToken;
} mc_TextFindTokenSetIndirection_t;

typedef struct {
    const _mongocrypt_buffer_t *edcDerivedToken;
    const _mongocrypt_buffer_t *escDerivedToken;
    const _mongocrypt_buffer_t *serverDerivedToken;
} mc_TextFindTokenSetIndirectionConst_t;

/* Cleanup code common to all mc_Text<T>FindTokenSet_t types.
 */
static void mc_TextFindTokenSetIndirection_cleanup(mc_TextFindTokenSetIndirection_t ts) {
    _mongocrypt_buffer_cleanup(ts.edcDerivedToken);
    _mongocrypt_buffer_cleanup(ts.escDerivedToken);
    _mongocrypt_buffer_cleanup(ts.serverDerivedToken);
}

/* Serialization code common to all mc_Text<T>FindTokenSet_t types.
 */
static bool mc_TextFindTokenSetIndirection_serialize(mc_TextFindTokenSetIndirectionConst_t ts,
                                                     bson_t *parent,
                                                     const char *field_name) {
    BSON_ASSERT_PARAM(ts.edcDerivedToken);
    BSON_ASSERT_PARAM(ts.escDerivedToken);
    BSON_ASSERT_PARAM(ts.serverDerivedToken);
    BSON_ASSERT_PARAM(parent);
    BSON_ASSERT_PARAM(field_name);
    bson_t child;
    if (!BSON_APPEND_DOCUMENT_BEGIN(parent, field_name, &child)) {
        return false;
    }
    if (!_mongocrypt_buffer_append(ts.edcDerivedToken, &child, "d", -1)) {
        return false;
    }
    if (!_mongocrypt_buffer_append(ts.escDerivedToken, &child, "s", -1)) {
        return false;
    }
    if (!_mongocrypt_buffer_append(ts.serverDerivedToken, &child, "l", -1)) {
        return false;
    }
    if (!bson_append_document_end(parent, &child)) {
        return false;
    }
    return true;
}

/* Parsing code common to all mc_Text<T>FindTokenSet_t types.
 */
static bool mc_TextFindTokenSetIndirection_parse(mc_TextFindTokenSetIndirection_t out,
                                                 const char *class_name,
                                                 const bson_iter_t *in,
                                                 mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(out.edcDerivedToken);
    BSON_ASSERT_PARAM(out.escDerivedToken);
    BSON_ASSERT_PARAM(out.serverDerivedToken);
    BSON_ASSERT_PARAM(in);
    bson_iter_t iter;
    bool has_d = false, has_s = false, has_l = false;

    iter = *in;
    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR("Error parsing %s: field expected to be a document, but got %s",
                   class_name,
                   mc_bson_type_to_string(bson_iter_type(&iter)));
        return false;
    }
    bson_iter_recurse(&iter, &iter);
    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        PARSE_BINARY(d, out.edcDerivedToken);
        PARSE_BINARY(s, out.escDerivedToken);
        PARSE_BINARY(l, out.serverDerivedToken);

        CLIENT_ERR("Error parsing %s: Unrecognized field '%s'", class_name, field);
        goto fail;
    }

    CHECK_HAS(d);
    CHECK_HAS(s);
    CHECK_HAS(l);

    return true;
fail:
    return false;
}

#define INDIRECT(type, ts)                                                                                             \
    (type) {                                                                                                           \
        .edcDerivedToken = &(ts).edcDerivedToken, .escDerivedToken = &(ts).escDerivedToken,                            \
        .serverDerivedToken = &(ts).serverDerivedFromDataToken                                                         \
    }
#define INDIRECT_TOKENSET(ts) INDIRECT(mc_TextFindTokenSetIndirection_t, ts)
#define INDIRECT_TOKENSET_CONST(ts) INDIRECT(mc_TextFindTokenSetIndirectionConst_t, ts)

#define DEF_TEXT_SEARCH_FIND_TOKEN_SET_CLEANUP(Type)                                                                   \
    static void mc_Text##Type##FindTokenSet_cleanup(mc_Text##Type##FindTokenSet_t *fts) {                              \
        if (fts) {                                                                                                     \
            mc_TextFindTokenSetIndirection_cleanup(INDIRECT_TOKENSET(*fts));                                           \
        }                                                                                                              \
    }
DEF_TEXT_SEARCH_FIND_TOKEN_SET_CLEANUP(Exact);
DEF_TEXT_SEARCH_FIND_TOKEN_SET_CLEANUP(Substring);
DEF_TEXT_SEARCH_FIND_TOKEN_SET_CLEANUP(Suffix);
DEF_TEXT_SEARCH_FIND_TOKEN_SET_CLEANUP(Prefix);

#define DEF_TEXT_SEARCH_FIND_TOKEN_SET_SERIALIZE(Type)                                                                 \
    static bool mc_Text##Type##FindTokenSet_serialize(bson_t *parent,                                                  \
                                                      const char *field_name,                                          \
                                                      const mc_Text##Type##FindTokenSet_t *ts) {                       \
        BSON_ASSERT_PARAM(ts);                                                                                         \
        return mc_TextFindTokenSetIndirection_serialize(INDIRECT_TOKENSET_CONST(*ts), parent, field_name);             \
    }
DEF_TEXT_SEARCH_FIND_TOKEN_SET_SERIALIZE(Exact)
DEF_TEXT_SEARCH_FIND_TOKEN_SET_SERIALIZE(Substring)
DEF_TEXT_SEARCH_FIND_TOKEN_SET_SERIALIZE(Suffix)
DEF_TEXT_SEARCH_FIND_TOKEN_SET_SERIALIZE(Prefix)

#define DEF_TEXT_SEARCH_FIND_TOKEN_SET_PARSE(Type)                                                                     \
    static bool mc_Text##Type##FindTokenSet_parse(mc_Text##Type##FindTokenSet_t *out,                                  \
                                                  bson_iter_t *in,                                                     \
                                                  mongocrypt_status_t *status) {                                       \
        BSON_ASSERT_PARAM(out);                                                                                        \
        return mc_TextFindTokenSetIndirection_parse(INDIRECT_TOKENSET(*out), "Text" #Type "FindTokenSet", in, status); \
    }
DEF_TEXT_SEARCH_FIND_TOKEN_SET_PARSE(Exact)
DEF_TEXT_SEARCH_FIND_TOKEN_SET_PARSE(Substring)
DEF_TEXT_SEARCH_FIND_TOKEN_SET_PARSE(Suffix)
DEF_TEXT_SEARCH_FIND_TOKEN_SET_PARSE(Prefix)

#undef DEF_TEXT_SEARCH_FIND_TOKEN_SET_CLEANUP
#undef DEF_TEXT_SEARCH_FIND_TOKEN_SET_SERIALIZE
#undef DEF_TEXT_SEARCH_FIND_TOKEN_SET_PARSE
#undef INDIRECT_TOKENSET_CONST
#undef INDIRECT_TOKENSET
#undef INDIRECT

void mc_FLE2FindTextPayload_init(mc_FLE2FindTextPayload_t *payload) {
    BSON_ASSERT_PARAM(payload);
    memset(payload, 0, sizeof(*payload));
}

void mc_FLE2FindTextPayload_cleanup(mc_FLE2FindTextPayload_t *payload) {
    if (!payload) {
        return;
    }
    mc_TextExactFindTokenSet_cleanup(&payload->tokenSets.exact.value);
    mc_TextSubstringFindTokenSet_cleanup(&payload->tokenSets.substring.value);
    mc_TextSuffixFindTokenSet_cleanup(&payload->tokenSets.suffix.value);
    mc_TextPrefixFindTokenSet_cleanup(&payload->tokenSets.prefix.value);
}

static bool mc_TextSearchFindTokenSets_parse(mc_TextSearchFindTokenSets_t *out,
                                             const bson_iter_t *in,
                                             mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    bson_iter_t iter;
    bool has_e = false, has_s = false, has_u = false, has_p = false;
    uint8_t field_count = 0;
    const char *class_name = "TextSearchFindTokenSets";

    iter = *in;
    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR("Error parsing %s: field expected to be a document, but got %s",
                   class_name,
                   mc_bson_type_to_string(bson_iter_type(&iter)));
        return false;
    }
    bson_iter_recurse(&iter, &iter);
    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(e) {
            if (!mc_TextExactFindTokenSet_parse(&out->exact.value, &iter, status)) {
                goto fail;
            }
            out->exact.set = true;
            field_count++;
        }
        END_IF_FIELD

        IF_FIELD(s) {
            if (!mc_TextSubstringFindTokenSet_parse(&out->substring.value, &iter, status)) {
                goto fail;
            }
            out->substring.set = true;
            field_count++;
        }
        END_IF_FIELD

        IF_FIELD(u) {
            if (!mc_TextSuffixFindTokenSet_parse(&out->suffix.value, &iter, status)) {
                goto fail;
            }
            out->suffix.set = true;
            field_count++;
        }
        END_IF_FIELD

        IF_FIELD(p) {
            if (!mc_TextPrefixFindTokenSet_parse(&out->prefix.value, &iter, status)) {
                goto fail;
            }
            out->prefix.set = true;
            field_count++;
        }
        END_IF_FIELD

        CLIENT_ERR("Error parsing %s: Unrecognized field '%s'", class_name, field);
        goto fail;
    }

    if (!field_count) {
        CLIENT_ERR("Error parsing %s: exactly one optional field is required", class_name);
        goto fail;
    } else if (field_count > 1) {
        CLIENT_ERR("Error parsing %s: cannot have multiple optional fields present", class_name);
        goto fail;
    }

    return true;
fail:
    return false;
}

bool mc_FLE2FindTextPayload_parse(mc_FLE2FindTextPayload_t *out, const bson_t *in, mongocrypt_status_t *status) {
    bson_iter_t iter;
    const char *class_name = "FLE2FindTextPayload";

    bool has_ts = false, has_cm = false, has_cf = false, has_df = false; // required fields
    bool has_ss = false, has_fs = false, has_ps = false;                 // optional fields

    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    mc_FLE2FindTextPayload_init(out);
    if (!bson_validate(in, BSON_VALIDATE_NONE, NULL) || !bson_iter_init(&iter, in)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        const char *typestr = mc_bson_type_to_string(bson_iter_type(&iter));
        BSON_ASSERT(field);

        IF_FIELD(ts) {
            if (!mc_TextSearchFindTokenSets_parse(&out->tokenSets, &iter, status)) {
                return false;
            }
        }
        END_IF_FIELD

        IF_FIELD(cm) {
            if (!BSON_ITER_HOLDS_INT64(&iter)) {
                CLIENT_ERR("Error parsing %s: Field 'cm' expected to be int64, but got %s", class_name, typestr);
                goto fail;
            }
            out->maxContentionFactor = bson_iter_int64(&iter);
        }
        END_IF_FIELD

        IF_FIELD(cf) {
            if (!BSON_ITER_HOLDS_BOOL(&iter)) {
                CLIENT_ERR("Error parsing %s: Field 'cf' expected to be boolean, but got %s", class_name, typestr);
                goto fail;
            }
            out->caseFold = bson_iter_bool(&iter);
        }
        END_IF_FIELD

        IF_FIELD(df) {
            if (!BSON_ITER_HOLDS_BOOL(&iter)) {
                CLIENT_ERR("Error parsing %s: Field 'df' expected to be boolean, but got %s", class_name, typestr);
                goto fail;
            }
            out->diacriticFold = bson_iter_bool(&iter);
        }
        END_IF_FIELD

        IF_FIELD(ss) {
            if (!mc_FLE2SubstringInsertSpec_parse(&out->substringSpec.value, &iter, status)) {
                goto fail;
            }
            out->substringSpec.set = true;
        }
        END_IF_FIELD

        IF_FIELD(fs) {
            if (!mc_FLE2SuffixInsertSpec_parse(&out->suffixSpec.value, &iter, status)) {
                goto fail;
            }
            out->suffixSpec.set = true;
        }
        END_IF_FIELD

        IF_FIELD(ps) {
            if (!mc_FLE2PrefixInsertSpec_parse(&out->prefixSpec.value, &iter, status)) {
                goto fail;
            }
            out->prefixSpec.set = true;
        }
        END_IF_FIELD

        CLIENT_ERR("Error parsing %s: Unrecognized field '%s'", class_name, field);
        goto fail;
    }

    CHECK_HAS(ts);
    CHECK_HAS(cm);
    CHECK_HAS(cf);
    CHECK_HAS(df);
    return true;
fail:
    return false;
}

bool mc_FLE2FindTextPayload_serialize(const mc_FLE2FindTextPayload_t *payload, bson_t *out) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(payload);

    // Append token sets "ts"
    {
        bson_t child;
        if (!BSON_APPEND_DOCUMENT_BEGIN(out, "ts", &child)) {
            return false;
        }
        // Append "e" if present
        if (payload->tokenSets.exact.set) {
            mc_TextExactFindTokenSet_serialize(&child, "e", &payload->tokenSets.exact.value);
        }
        // Append "s" if present
        if (payload->tokenSets.substring.set) {
            mc_TextSubstringFindTokenSet_serialize(&child, "s", &payload->tokenSets.substring.value);
        }
        // Append "u" if present
        if (payload->tokenSets.suffix.set) {
            mc_TextSuffixFindTokenSet_serialize(&child, "u", &payload->tokenSets.suffix.value);
        }
        // Append "p" if present
        if (payload->tokenSets.prefix.set) {
            mc_TextPrefixFindTokenSet_serialize(&child, "p", &payload->tokenSets.prefix.value);
        }
        if (!bson_append_document_end(out, &child)) {
            return false;
        }
    }

    // Append "cm".
    if (!BSON_APPEND_INT64(out, "cm", payload->maxContentionFactor)) {
        return false;
    }

    // Append "cf".
    if (!BSON_APPEND_BOOL(out, "cf", payload->caseFold)) {
        return false;
    }

    // Append "df".
    if (!BSON_APPEND_BOOL(out, "df", payload->diacriticFold)) {
        return false;
    }

    // Append "ss" if present.
    if (payload->substringSpec.set) {
        bson_t child;
        if (!BSON_APPEND_DOCUMENT_BEGIN(out, "ss", &child)) {
            return false;
        }
        if (!BSON_APPEND_INT32(&child, "mlen", (int32_t)payload->substringSpec.value.mlen)) {
            return false;
        }
        if (!BSON_APPEND_INT32(&child, "ub", (int32_t)payload->substringSpec.value.ub)) {
            return false;
        }
        if (!BSON_APPEND_INT32(&child, "lb", (int32_t)payload->substringSpec.value.lb)) {
            return false;
        }
        if (!bson_append_document_end(out, &child)) {
            return false;
        }
    }

    // Append "fs" if present.
    if (payload->suffixSpec.set) {
        bson_t child;
        if (!BSON_APPEND_DOCUMENT_BEGIN(out, "fs", &child)) {
            return false;
        }
        if (!BSON_APPEND_INT32(&child, "ub", (int32_t)payload->suffixSpec.value.ub)) {
            return false;
        }
        if (!BSON_APPEND_INT32(&child, "lb", (int32_t)payload->suffixSpec.value.lb)) {
            return false;
        }
        if (!bson_append_document_end(out, &child)) {
            return false;
        }
    }

    // Append "ps" if present.
    if (payload->prefixSpec.set) {
        bson_t child;
        if (!BSON_APPEND_DOCUMENT_BEGIN(out, "ps", &child)) {
            return false;
        }
        if (!BSON_APPEND_INT32(&child, "ub", (int32_t)payload->prefixSpec.value.ub)) {
            return false;
        }
        if (!BSON_APPEND_INT32(&child, "lb", (int32_t)payload->prefixSpec.value.lb)) {
            return false;
        }
        if (!bson_append_document_end(out, &child)) {
            return false;
        }
    }

    return true;
}

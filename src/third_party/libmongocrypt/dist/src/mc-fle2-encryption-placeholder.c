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

#include <limits.h> // SIZE_MAX

#include "mc-fle2-encryption-placeholder-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-util-private.h" // mc_bson_type_to_string
#include "mongocrypt.h"

// Common logic for testing field name, tracking duplication, and presence.
#define IF_FIELD(Name)                                                                                                 \
    if (0 == strcmp(field, #Name)) {                                                                                   \
        if (has_##Name) {                                                                                              \
            CLIENT_ERR(ERROR_PREFIX "Duplicate field '" #Name "' in placeholder bson");                                \
            goto fail;                                                                                                 \
        }                                                                                                              \
        has_##Name = true;                                                                                             \
    ((void)0)

#define END_IF_FIELD                                                                                                   \
    continue;                                                                                                          \
    }                                                                                                                  \
    else((void)0)

#define CHECK_HAS(Name)                                                                                                \
    if (!has_##Name) {                                                                                                 \
        CLIENT_ERR(ERROR_PREFIX "Missing field '" #Name "' in placeholder");                                           \
        goto fail;                                                                                                     \
    }

// Common logic for parsing int32 greater than zero
#define IF_FIELD_INT32_GT0_PARSE(Name, Dest, Iter)                                                                     \
    IF_FIELD(Name);                                                                                                    \
    {                                                                                                                  \
        if (!BSON_ITER_HOLDS_INT32(&Iter)) {                                                                           \
            CLIENT_ERR(ERROR_PREFIX "'" #Name "' must be an int32");                                                   \
            goto fail;                                                                                                 \
        }                                                                                                              \
        int32_t val = bson_iter_int32(&Iter);                                                                          \
        if (val <= 0) {                                                                                                \
            CLIENT_ERR(ERROR_PREFIX "'" #Name "' must be greater than zero");                                          \
            goto fail;                                                                                                 \
        }                                                                                                              \
        Dest = (uint32_t)val;                                                                                          \
    }                                                                                                                  \
    END_IF_FIELD

void mc_FLE2EncryptionPlaceholder_init(mc_FLE2EncryptionPlaceholder_t *placeholder) {
    memset(placeholder, 0, sizeof(mc_FLE2EncryptionPlaceholder_t));
}

#define ERROR_PREFIX "Error parsing FLE2EncryptionPlaceholder: "

bool mc_FLE2EncryptionPlaceholder_parse(mc_FLE2EncryptionPlaceholder_t *out,
                                        const bson_t *in,
                                        mongocrypt_status_t *status) {
    bson_iter_t iter = {0};
    bool has_t = false, has_a = false, has_v = false, has_cm = false;
    bool has_ki = false, has_ku = false;
    bool has_s = false;

    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    mc_FLE2EncryptionPlaceholder_init(out);
    if (!bson_validate(in, BSON_VALIDATE_NONE, NULL) || !bson_iter_init(&iter, in)) {
        CLIENT_ERR(ERROR_PREFIX "invalid BSON");
        return false;
    }

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(t);
        {
            int32_t type;
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "invalid marking, 't' must be an int32");
                goto fail;
            }
            type = bson_iter_int32(&iter);
            if ((type != MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT) && (type != MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND)) {
                CLIENT_ERR(ERROR_PREFIX "invalid placeholder type value: %d", type);
                goto fail;
            }
            out->type = (mongocrypt_fle2_placeholder_type_t)type;
        }
        END_IF_FIELD;

        IF_FIELD(a);
        {
            int32_t algorithm;
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "invalid marking, 'a' must be an int32");
                goto fail;
            }
            algorithm = bson_iter_int32(&iter);
            if (algorithm != MONGOCRYPT_FLE2_ALGORITHM_UNINDEXED && algorithm != MONGOCRYPT_FLE2_ALGORITHM_EQUALITY
                && algorithm != MONGOCRYPT_FLE2_ALGORITHM_RANGE && algorithm != MONGOCRYPT_FLE2_ALGORITHM_TEXT_SEARCH) {
                CLIENT_ERR(ERROR_PREFIX "invalid algorithm value: %d", algorithm);
                goto fail;
            }
            out->algorithm = (mongocrypt_fle2_encryption_algorithm_t)algorithm;
        }
        END_IF_FIELD;

        IF_FIELD(ki);
        {
            if (!_mongocrypt_buffer_from_uuid_iter(&out->index_key_id, &iter)) {
                CLIENT_ERR(ERROR_PREFIX "index key id must be a UUID");
                goto fail;
            }
        }
        END_IF_FIELD;

        IF_FIELD(ku);
        {
            if (!_mongocrypt_buffer_from_uuid_iter(&out->user_key_id, &iter)) {
                CLIENT_ERR(ERROR_PREFIX "user key id must be a UUID");
                goto fail;
            }
        }
        END_IF_FIELD;

        IF_FIELD(v);
        memcpy(&out->v_iter, &iter, sizeof(bson_iter_t));
        END_IF_FIELD;

        IF_FIELD(cm);
        {
            if (!BSON_ITER_HOLDS_INT64(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "invalid marking, 'cm' must be an int64");
                goto fail;
            }
            out->maxContentionFactor = bson_iter_int64(&iter);
            if (!mc_validate_contention(out->maxContentionFactor, status)) {
                goto fail;
            }
        }
        END_IF_FIELD;

        IF_FIELD(s);
        {
            if (!BSON_ITER_HOLDS_INT64(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "invalid marking, 's' must be an int64");
                goto fail;
            }
            out->sparsity = bson_iter_int64(&iter);
            if (!mc_validate_sparsity(out->sparsity, status)) {
                goto fail;
            }
        }
        END_IF_FIELD;
    }

    CHECK_HAS(t)
    CHECK_HAS(a)
    CHECK_HAS(ki)
    CHECK_HAS(ku)
    CHECK_HAS(v)
    CHECK_HAS(cm)
    // Do not error if sparsity (s) is not present.
    // 's' was added in version 6.1 of query analysis (mongocryptd or mongo_crypt
    // shared library). 's' is not present in query analysis 6.0.

    return true;

fail:
    return false;
}

void mc_FLE2EncryptionPlaceholder_cleanup(mc_FLE2EncryptionPlaceholder_t *placeholder) {
    BSON_ASSERT_PARAM(placeholder);

    _mongocrypt_buffer_cleanup(&placeholder->index_key_id);
    _mongocrypt_buffer_cleanup(&placeholder->user_key_id);
    mc_FLE2EncryptionPlaceholder_init(placeholder);
}

#undef ERROR_PREFIX
#define ERROR_PREFIX "Error validating contention: "

bool mc_validate_contention(int64_t contention, mongocrypt_status_t *status) {
    if (contention < 0) {
        CLIENT_ERR(ERROR_PREFIX "contention must be non-negative, got: %" PRId64, contention);
        return false;
    }
    if (contention == INT64_MAX) {
        CLIENT_ERR(ERROR_PREFIX "contention must be < INT64_MAX, got: %" PRId64, contention);
        return false;
    }
    return true;
}

#undef ERROR_PREFIX
#define ERROR_PREFIX "Error validating sparsity: "

bool mc_validate_sparsity(int64_t sparsity, mongocrypt_status_t *status) {
    if (sparsity < 0) {
        CLIENT_ERR(ERROR_PREFIX "sparsity must be non-negative, got: %" PRId64, sparsity);
        return false;
    }
    // mc_getEdgesInt expects a size_t sparsity.
    if ((uint64_t)sparsity >= SIZE_MAX) {
        CLIENT_ERR(ERROR_PREFIX "sparsity must be < %zu, got: %" PRId64, SIZE_MAX, sparsity);
        return false;
    }
    return true;
}

#undef ERROR_PREFIX
#define ERROR_PREFIX "Error parsing FLE2RangeFindSpecEdgesInfo: "

static bool mc_FLE2RangeFindSpecEdgesInfo_parse(mc_FLE2RangeFindSpecEdgesInfo_t *out,
                                                const bson_iter_t *in,
                                                mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_lowerBound = false, has_lbIncluded = false, has_upperBound = false, has_ubIncluded = false,
         has_indexMin = false, has_indexMax = false, has_precision = false, has_trimFactor = false;

    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    iter = *in;

    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR(ERROR_PREFIX "must be an iterator to a document");
        return false;
    }
    bson_iter_recurse(&iter, &iter);

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(lowerBound);
        out->lowerBound = iter;
        END_IF_FIELD;

        IF_FIELD(lbIncluded);
        {
            if (!BSON_ITER_HOLDS_BOOL(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'lbIncluded' must be a bool");
                goto fail;
            }
            out->lbIncluded = bson_iter_bool(&iter);
        }
        END_IF_FIELD;

        IF_FIELD(upperBound);
        out->upperBound = iter;
        END_IF_FIELD;

        IF_FIELD(ubIncluded);
        {
            if (!BSON_ITER_HOLDS_BOOL(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'ubIncluded' must be a bool");
                goto fail;
            }
            out->ubIncluded = bson_iter_bool(&iter);
        }
        END_IF_FIELD;

        IF_FIELD(indexMin);
        out->indexMin = iter;
        END_IF_FIELD;

        IF_FIELD(indexMax);
        out->indexMax = iter;
        END_IF_FIELD;

        IF_FIELD(precision);
        {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'precision' must be an int32");
                goto fail;
            }
            int32_t val = bson_iter_int32(&iter);
            if (val < 0) {
                CLIENT_ERR(ERROR_PREFIX "'precision' must be non-negative");
                goto fail;
            }

            out->precision = OPT_I32(val);
        }
        END_IF_FIELD;

        IF_FIELD(trimFactor);
        {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'trimFactor' must be an int32");
                goto fail;
            }
            int32_t val = bson_iter_int32(&iter);
            if (val < 0) {
                CLIENT_ERR(ERROR_PREFIX "'trimFactor' must be non-negative");
                goto fail;
            }

            out->trimFactor = OPT_I32(val);
        }
        END_IF_FIELD;
    }

    CHECK_HAS(lowerBound)
    CHECK_HAS(lbIncluded)
    CHECK_HAS(upperBound)
    CHECK_HAS(ubIncluded)
    CHECK_HAS(indexMin)
    CHECK_HAS(indexMax)
    // Do not error if precision is not present. Precision optional and only
    // applies to double/decimal128.

    return true;

fail:
    return false;
}

#undef ERROR_PREFIX
#define ERROR_PREFIX "Error parsing FLE2RangeFindSpec: "

bool mc_FLE2RangeFindSpec_parse(mc_FLE2RangeFindSpec_t *out, const bson_iter_t *in, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    bson_iter_t iter = *in;
    bool has_edgesInfo = false, has_payloadId = false, has_firstOperator = false, has_secondOperator = false;

    *out = (mc_FLE2RangeFindSpec_t){{{{0}}}};

    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR(ERROR_PREFIX "must be an iterator to a document");
        return false;
    }
    bson_iter_recurse(&iter, &iter);

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(edgesInfo);
        {
            if (!mc_FLE2RangeFindSpecEdgesInfo_parse(&out->edgesInfo.value, &iter, status)) {
                goto fail;
            }
            out->edgesInfo.set = true;
        }
        END_IF_FIELD;

        IF_FIELD(payloadId);
        {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'payloadId' must be an int32");
                goto fail;
            }
            out->payloadId = bson_iter_int32(&iter);
        }
        END_IF_FIELD;

        IF_FIELD(firstOperator);
        {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'firstOperator' must be an int32");
                goto fail;
            }
            const int32_t first_op = bson_iter_int32(&iter);
            if (first_op < FLE2RangeOperator_min_val || first_op > FLE2RangeOperator_max_val) {
                CLIENT_ERR(ERROR_PREFIX "'firstOperator' must be between %d and %d",
                           FLE2RangeOperator_min_val,
                           FLE2RangeOperator_max_val);
                goto fail;
            }
            out->firstOperator = (mc_FLE2RangeOperator_t)first_op;
        }
        END_IF_FIELD;

        IF_FIELD(secondOperator);
        {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'secondOperator' must be an int32");
                goto fail;
            }
            const int32_t second_op = bson_iter_int32(&iter);
            if (second_op < FLE2RangeOperator_min_val || second_op > FLE2RangeOperator_max_val) {
                CLIENT_ERR(ERROR_PREFIX "'secondOperator' must be between %d and %d",
                           FLE2RangeOperator_min_val,
                           FLE2RangeOperator_max_val);
                goto fail;
            }
            out->secondOperator = (mc_FLE2RangeOperator_t)second_op;
        }
        END_IF_FIELD;
    }

    // edgesInfo is optional. Do not require it.
    CHECK_HAS(payloadId)
    CHECK_HAS(firstOperator)
    // secondOperator is optional. Do not require it.
    return true;

fail:
    return false;
}

#undef ERROR_PREFIX
#define ERROR_PREFIX "Error parsing FLE2RangeInsertSpec: "

bool mc_FLE2RangeInsertSpec_parse(mc_FLE2RangeInsertSpec_t *out, const bson_iter_t *in, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    *out = (mc_FLE2RangeInsertSpec_t){{0}};

    bson_iter_t iter = *in;
    bool has_v = false, has_min = false, has_max = false, has_precision = false, has_trimFactor = false;

    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR(ERROR_PREFIX "must be an iterator to a document");
        return false;
    }
    bson_iter_recurse(&iter, &iter);

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(v);
        out->v = iter;
        END_IF_FIELD;

        IF_FIELD(min);
        out->min = iter;
        END_IF_FIELD;

        IF_FIELD(max);
        out->max = iter;
        END_IF_FIELD;

        IF_FIELD(precision);
        {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'precision' must be an int32");
                goto fail;
            }
            int32_t val = bson_iter_int32(&iter);
            if (val < 0) {
                CLIENT_ERR(ERROR_PREFIX "'precision' must be non-negative");
                goto fail;
            }
            out->precision = OPT_I32(val);
        }
        END_IF_FIELD;

        IF_FIELD(trimFactor);
        {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'trimFactor' must be an int32");
                goto fail;
            }
            int32_t val = bson_iter_int32(&iter);
            if (val < 0) {
                CLIENT_ERR(ERROR_PREFIX "'trimFactor' must be non-negative");
                goto fail;
            }
            out->trimFactor = OPT_I32(val);
        }
        END_IF_FIELD;
    }

    CHECK_HAS(v)
    CHECK_HAS(min)
    CHECK_HAS(max)
    // Do not error if precision is not present. Precision optional and only
    // applies to double/decimal128.

    return true;

fail:
    return false;
}

#undef ERROR_PREFIX

#define ERROR_PREFIX "Error parsing FLE2SubstringInsertSpec: "

bool mc_FLE2SubstringInsertSpec_parse(mc_FLE2SubstringInsertSpec_t *out,
                                      const bson_iter_t *in,
                                      mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_mlen = false, has_ub = false, has_lb = false;
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    iter = *in;

    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR(ERROR_PREFIX "must be an iterator to a document");
        return false;
    }
    bson_iter_recurse(&iter, &iter);
    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);
        IF_FIELD_INT32_GT0_PARSE(mlen, out->mlen, iter);
        IF_FIELD_INT32_GT0_PARSE(ub, out->ub, iter);
        IF_FIELD_INT32_GT0_PARSE(lb, out->lb, iter);
    }
    CHECK_HAS(mlen)
    CHECK_HAS(ub)
    CHECK_HAS(lb)
    if (out->ub < out->lb) {
        CLIENT_ERR(ERROR_PREFIX "upper bound cannot be less than the lower bound");
        goto fail;
    }
    if (out->mlen < out->ub) {
        CLIENT_ERR(ERROR_PREFIX "maximum indexed length cannot be less than the upper bound");
        goto fail;
    }
    return true;
fail:
    return false;
}

#undef ERROR_PREFIX

#define ERROR_PREFIX "Error parsing FLE2SuffixInsertSpec: "

bool mc_FLE2SuffixInsertSpec_parse(mc_FLE2SuffixInsertSpec_t *out, const bson_iter_t *in, mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_ub = false, has_lb = false;

    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    iter = *in;

    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR(ERROR_PREFIX "must be an iterator to a document");
        return false;
    }
    bson_iter_recurse(&iter, &iter);
    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);
        IF_FIELD_INT32_GT0_PARSE(ub, out->ub, iter);
        IF_FIELD_INT32_GT0_PARSE(lb, out->lb, iter);
    }
    CHECK_HAS(ub)
    CHECK_HAS(lb)
    if (out->ub < out->lb) {
        CLIENT_ERR(ERROR_PREFIX "upper bound cannot be less than the lower bound");
        goto fail;
    }
    return true;
fail:
    return false;
}

#undef ERROR_PREFIX

#define ERROR_PREFIX "Error parsing FLE2PrefixInsertSpec: "

bool mc_FLE2PrefixInsertSpec_parse(mc_FLE2PrefixInsertSpec_t *out, const bson_iter_t *in, mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_ub = false, has_lb = false;
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    iter = *in;

    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR(ERROR_PREFIX "must be an iterator to a document");
        return false;
    }
    bson_iter_recurse(&iter, &iter);
    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);
        IF_FIELD_INT32_GT0_PARSE(ub, out->ub, iter);
        IF_FIELD_INT32_GT0_PARSE(lb, out->lb, iter);
    }
    CHECK_HAS(ub)
    CHECK_HAS(lb)
    if (out->ub < out->lb) {
        CLIENT_ERR(ERROR_PREFIX "upper bound cannot be less than the lower bound");
        goto fail;
    }
    return true;
fail:
    return false;
}

#undef ERROR_PREFIX

#define ERROR_PREFIX "Error parsing FLE2TextSearchInsertSpec: "

bool mc_FLE2TextSearchInsertSpec_parse(mc_FLE2TextSearchInsertSpec_t *out,
                                       const bson_iter_t *in,
                                       mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    *out = (mc_FLE2TextSearchInsertSpec_t){{0}};

    bson_iter_t iter = *in;
    bool has_v = false, has_casef = false, has_diacf = false;
    bool has_substr = false, has_suffix = false, has_prefix = false;

    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR(ERROR_PREFIX "must be an iterator to a document");
        return false;
    }
    bson_iter_recurse(&iter, &iter);

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(v);
        {
            out->v = bson_iter_utf8(&iter, &out->len);
            if (!out->v) {
                CLIENT_ERR(ERROR_PREFIX "unsupported BSON type: %s for text search",
                           mc_bson_type_to_string(bson_iter_type(&iter)));
                goto fail;
            }
            out->v_iter = iter;
        }
        END_IF_FIELD;

        IF_FIELD(casef);
        {
            if (!BSON_ITER_HOLDS_BOOL(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'casef' must be a bool");
                goto fail;
            }
            out->casef = bson_iter_bool(&iter);
        }
        END_IF_FIELD;

        IF_FIELD(diacf);
        {
            if (!BSON_ITER_HOLDS_BOOL(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "'diacf' must be a bool");
                goto fail;
            }
            out->diacf = bson_iter_bool(&iter);
        }
        END_IF_FIELD;

        IF_FIELD(substr);
        {
            if (!mc_FLE2SubstringInsertSpec_parse(&out->substr.value, &iter, status)) {
                goto fail;
            }
            out->substr.set = true;
        }
        END_IF_FIELD;

        IF_FIELD(suffix);
        {
            if (!mc_FLE2SuffixInsertSpec_parse(&out->suffix.value, &iter, status)) {
                goto fail;
            }
            out->suffix.set = true;
        }
        END_IF_FIELD;

        IF_FIELD(prefix);
        {
            if (!mc_FLE2PrefixInsertSpec_parse(&out->prefix.value, &iter, status)) {
                goto fail;
            }
            out->prefix.set = true;
        }
        END_IF_FIELD;
    }

    CHECK_HAS(v)
    CHECK_HAS(casef)
    CHECK_HAS(diacf)

    return true;

fail:
    return false;
}

#undef ERROR_PREFIX

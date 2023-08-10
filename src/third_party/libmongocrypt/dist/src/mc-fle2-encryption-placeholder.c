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
#include "mongocrypt.h"

// Common logic for testing field name, tracking duplication, and presence.
#define IF_FIELD(Name)                                                                                                 \
    if (0 == strcmp(field, #Name)) {                                                                                   \
        if (has_##Name) {                                                                                              \
            CLIENT_ERR("Duplicate field '" #Name "' in placeholder bson");                                             \
            goto fail;                                                                                                 \
        }                                                                                                              \
        has_##Name = true;

#define END_IF_FIELD                                                                                                   \
    continue;                                                                                                          \
    }

#define CHECK_HAS(Name)                                                                                                \
    if (!has_##Name) {                                                                                                 \
        CLIENT_ERR("Missing field '" #Name "' in placeholder");                                                        \
        goto fail;                                                                                                     \
    }

void mc_FLE2EncryptionPlaceholder_init(mc_FLE2EncryptionPlaceholder_t *placeholder) {
    memset(placeholder, 0, sizeof(mc_FLE2EncryptionPlaceholder_t));
}

bool mc_FLE2EncryptionPlaceholder_parse(mc_FLE2EncryptionPlaceholder_t *out,
                                        const bson_t *in,
                                        mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_t = false, has_a = false, has_v = false, has_cm = false;
    bool has_ki = false, has_ku = false;
    bool has_s = false;

    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    mc_FLE2EncryptionPlaceholder_init(out);
    if (!bson_validate(in, BSON_VALIDATE_NONE, NULL) || !bson_iter_init(&iter, in)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(t) {
            int32_t type;
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("invalid marking, 't' must be an int32");
                goto fail;
            }
            type = bson_iter_int32(&iter);
            if ((type != MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT) && (type != MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND)) {
                CLIENT_ERR("invalid placeholder type value: %d", type);
                goto fail;
            }
            out->type = (mongocrypt_fle2_placeholder_type_t)type;
        }
        END_IF_FIELD

        IF_FIELD(a) {
            int32_t algorithm;
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("invalid marking, 'a' must be an int32");
                goto fail;
            }
            algorithm = bson_iter_int32(&iter);
            if (algorithm != MONGOCRYPT_FLE2_ALGORITHM_UNINDEXED && algorithm != MONGOCRYPT_FLE2_ALGORITHM_EQUALITY
                && algorithm != MONGOCRYPT_FLE2_ALGORITHM_RANGE) {
                CLIENT_ERR("invalid algorithm value: %d", algorithm);
                goto fail;
            }
            out->algorithm = (mongocrypt_fle2_encryption_algorithm_t)algorithm;
        }
        END_IF_FIELD

        IF_FIELD(ki) {
            if (!_mongocrypt_buffer_from_uuid_iter(&out->index_key_id, &iter)) {
                CLIENT_ERR("index key id must be a UUID");
                goto fail;
            }
        }
        END_IF_FIELD

        IF_FIELD(ku) {
            if (!_mongocrypt_buffer_from_uuid_iter(&out->user_key_id, &iter)) {
                CLIENT_ERR("user key id must be a UUID");
                goto fail;
            }
        }
        END_IF_FIELD

        IF_FIELD(v) {
            memcpy(&out->v_iter, &iter, sizeof(bson_iter_t));
        }
        END_IF_FIELD

        IF_FIELD(cm) {
            if (!BSON_ITER_HOLDS_INT64(&iter)) {
                CLIENT_ERR("invalid marking, 'cm' must be an int64");
                goto fail;
            }
            out->maxContentionCounter = bson_iter_int64(&iter);
            if (!mc_validate_contention(out->maxContentionCounter, status)) {
                goto fail;
            }
        }
        END_IF_FIELD

        IF_FIELD(s) {
            if (!BSON_ITER_HOLDS_INT64(&iter)) {
                CLIENT_ERR("invalid marking, 's' must be an int64");
                goto fail;
            }
            out->sparsity = bson_iter_int64(&iter);
            if (!mc_validate_sparsity(out->sparsity, status)) {
                goto fail;
            }
        }
        END_IF_FIELD
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
    mc_FLE2EncryptionPlaceholder_cleanup(out);
    return false;
}

void mc_FLE2EncryptionPlaceholder_cleanup(mc_FLE2EncryptionPlaceholder_t *placeholder) {
    BSON_ASSERT_PARAM(placeholder);

    _mongocrypt_buffer_cleanup(&placeholder->index_key_id);
    _mongocrypt_buffer_cleanup(&placeholder->user_key_id);
    mc_FLE2EncryptionPlaceholder_init(placeholder);
}

bool mc_validate_contention(int64_t contention, mongocrypt_status_t *status) {
    if (contention < 0) {
        CLIENT_ERR("contention must be non-negative, got: %" PRId64, contention);
        return false;
    }
    if (contention == INT64_MAX) {
        CLIENT_ERR("contention must be < INT64_MAX, got: %" PRId64, contention);
        return false;
    }
    return true;
}

bool mc_validate_sparsity(int64_t sparsity, mongocrypt_status_t *status) {
    if (sparsity < 0) {
        CLIENT_ERR("sparsity must be non-negative, got: %" PRId64, sparsity);
        return false;
    }
    // mc_getEdgesInt expects a size_t sparsity.
    if (sparsity >= SIZE_MAX) {
        CLIENT_ERR("sparsity must be < %zu, got: %" PRId64, SIZE_MAX, sparsity);
        return false;
    }
    return true;
}

static bool mc_FLE2RangeFindSpecEdgesInfo_parse(mc_FLE2RangeFindSpecEdgesInfo_t *out,
                                                const bson_iter_t *in,
                                                mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_lowerBound = false, has_lbIncluded = false, has_upperBound = false, has_ubIncluded = false,
         has_indexMin = false, has_indexMax = false, has_precision = false;

    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    iter = *in;

    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR("invalid FLE2RangeFindSpecEdgesInfo: must be an iterator to "
                   "a document");
        return false;
    }
    bson_iter_recurse(&iter, &iter);

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(lowerBound) {
            out->lowerBound = iter;
        }
        END_IF_FIELD

        IF_FIELD(lbIncluded) {
            if (!BSON_ITER_HOLDS_BOOL(&iter)) {
                CLIENT_ERR("invalid FLE2RangeFindSpecEdgesInfo: 'lbIncluded' must "
                           "be a bool");
                goto fail;
            }
            out->lbIncluded = bson_iter_bool(&iter);
        }
        END_IF_FIELD

        IF_FIELD(upperBound) {
            out->upperBound = iter;
        }
        END_IF_FIELD

        IF_FIELD(ubIncluded) {
            if (!BSON_ITER_HOLDS_BOOL(&iter)) {
                CLIENT_ERR("invalid FLE2RangeFindSpecEdgesInfo: 'ubIncluded' must "
                           "be a bool");
                goto fail;
            }
            out->ubIncluded = bson_iter_bool(&iter);
        }
        END_IF_FIELD

        IF_FIELD(indexMin) {
            out->indexMin = iter;
        }
        END_IF_FIELD

        IF_FIELD(indexMax) {
            out->indexMax = iter;
        }
        END_IF_FIELD

        IF_FIELD(precision) {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("invalid FLE2RangeFindSpecEdgesInfo: 'precision' must "
                           "be an int32");
                goto fail;
            }
            int32_t val = bson_iter_int32(&iter);
            if (val < 0) {
                CLIENT_ERR("invalid FLE2RangeFindSpecEdgesInfo: 'precision' must be"
                           "non-negative");
                goto fail;
            }

            out->precision = OPT_U32((uint32_t)val);
        }
        END_IF_FIELD
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

bool mc_FLE2RangeFindSpec_parse(mc_FLE2RangeFindSpec_t *out, const bson_iter_t *in, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    bson_iter_t iter = *in;
    bool has_edgesInfo = false, has_payloadId = false, has_firstOperator = false, has_secondOperator = false;

    *out = (mc_FLE2RangeFindSpec_t){{{{0}}}};

    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR("invalid FLE2RangeFindSpec: must be an iterator to a document");
        return false;
    }
    bson_iter_recurse(&iter, &iter);

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(edgesInfo) {
            if (!mc_FLE2RangeFindSpecEdgesInfo_parse(&out->edgesInfo.value, &iter, status)) {
                goto fail;
            }
            out->edgesInfo.set = true;
        }
        END_IF_FIELD

        IF_FIELD(payloadId) {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("invalid FLE2RangeFindSpec: 'payloadId' must be an int32");
                goto fail;
            }
            out->payloadId = bson_iter_int32(&iter);
        }
        END_IF_FIELD

        IF_FIELD(firstOperator) {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("invalid FLE2RangeFindSpec: 'firstOperator' must be an int32");
                goto fail;
            }
            const int32_t first_op = bson_iter_int32(&iter);
            if (first_op < FLE2RangeOperator_min_val || first_op > FLE2RangeOperator_max_val) {
                CLIENT_ERR("invalid FLE2RangeFindSpec: 'firstOperator' must be "
                           "between %d and %d",
                           FLE2RangeOperator_min_val,
                           FLE2RangeOperator_max_val);
                goto fail;
            }
            out->firstOperator = (mc_FLE2RangeOperator_t)first_op;
        }
        END_IF_FIELD

        IF_FIELD(secondOperator) {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("invalid FLE2RangeFindSpec: 'secondOperator' must be an int32");
                goto fail;
            }
            const int32_t second_op = bson_iter_int32(&iter);
            if (second_op < FLE2RangeOperator_min_val || second_op > FLE2RangeOperator_max_val) {
                CLIENT_ERR("invalid FLE2RangeFindSpec: 'secondOperator' must be "
                           "between %d and %d",
                           FLE2RangeOperator_min_val,
                           FLE2RangeOperator_max_val);
                goto fail;
            }
            out->secondOperator = (mc_FLE2RangeOperator_t)second_op;
        }
        END_IF_FIELD
    }

    // edgesInfo is optional. Do not require it.
    CHECK_HAS(payloadId)
    CHECK_HAS(firstOperator)
    // secondOperator is optional. Do not require it.
    return true;

fail:
    return false;
}

bool mc_FLE2RangeInsertSpec_parse(mc_FLE2RangeInsertSpec_t *out, const bson_iter_t *in, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    *out = (mc_FLE2RangeInsertSpec_t){{0}};

    bson_iter_t iter = *in;
    bool has_v = false, has_min = false, has_max = false, has_precision = false;

    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR("invalid FLE2RangeInsertSpec: must be an iterator to a document");
        return false;
    }
    bson_iter_recurse(&iter, &iter);

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(v) {
            out->v = iter;
        }
        END_IF_FIELD

        IF_FIELD(min) {
            out->min = iter;
        }
        END_IF_FIELD

        IF_FIELD(max) {
            out->max = iter;
        }
        END_IF_FIELD

        IF_FIELD(precision) {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("invalid FLE2RangeFindSpecEdgesInfo: 'precision' must "
                           "be an int32");
                goto fail;
            }
            int32_t val = bson_iter_int32(&iter);
            if (val < 0) {
                CLIENT_ERR("invalid FLE2RangeFindSpecEdgesInfo: 'precision' must be"
                           "non-negative");
                goto fail;
            }
            out->precision = OPT_U32((uint32_t)val);
        }
        END_IF_FIELD
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

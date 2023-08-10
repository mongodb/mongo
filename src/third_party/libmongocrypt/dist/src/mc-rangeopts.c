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

#include "mc-rangeopts-private.h"

#include "mc-check-conversions-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-util-private.h" // mc_bson_type_to_string

#include <float.h> // DBL_MAX

// Common logic for testing field name, tracking duplication, and presence.
#define IF_FIELD(Name, ErrorPrefix)                                                                                    \
    if (0 == strcmp(field, #Name)) {                                                                                   \
        if (has_##Name) {                                                                                              \
            CLIENT_ERR("%sUnexpected duplicate field '" #Name "'", ErrorPrefix);                                       \
            return false;                                                                                              \
        }                                                                                                              \
        has_##Name = true;

#define END_IF_FIELD                                                                                                   \
    continue;                                                                                                          \
    }

#define CHECK_HAS(Name, ErrorPrefix)                                                                                   \
    if (!has_##Name) {                                                                                                 \
        CLIENT_ERR("%sMissing field '" #Name "'", ErrorPrefix);                                                        \
        return false;                                                                                                  \
    }

bool mc_RangeOpts_parse(mc_RangeOpts_t *ro, const bson_t *in, mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_min = false, has_max = false, has_sparsity = false, has_precision = false;
    const char *const error_prefix = "Error parsing RangeOpts: ";

    BSON_ASSERT_PARAM(ro);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT(status || true);

    *ro = (mc_RangeOpts_t){0};
    ro->bson = bson_copy(in);

    if (!bson_iter_init(&iter, ro->bson)) {
        CLIENT_ERR("%sInvalid BSON", error_prefix);
        return false;
    }

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        IF_FIELD(min, error_prefix)
        ro->min.set = true;
        ro->min.value = iter;
        END_IF_FIELD

        IF_FIELD(max, error_prefix)
        ro->max.set = true;
        ro->max.value = iter;
        END_IF_FIELD

        IF_FIELD(sparsity, error_prefix)
        if (!BSON_ITER_HOLDS_INT64(&iter)) {
            CLIENT_ERR("%sExpected int64 for sparsity, got: %s",
                       error_prefix,
                       mc_bson_type_to_string(bson_iter_type(&iter)));
            return false;
        };
        ro->sparsity = bson_iter_int64(&iter);
        END_IF_FIELD

        IF_FIELD(precision, error_prefix) {
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("%s'precision' must be an int32", error_prefix);
                return false;
            }
            int32_t val = bson_iter_int32(&iter);
            if (val < 0) {
                CLIENT_ERR("%s'precision' must be non-negative", error_prefix);
                return false;
            }
            ro->precision = OPT_U32((uint32_t)val);
        }
        END_IF_FIELD

        CLIENT_ERR("%sUnrecognized field: '%s'", error_prefix, field);
        return false;
    }

    // Do not error if min/max are not present. min/max are optional.
    CHECK_HAS(sparsity, error_prefix);
    // Do not error if precision is not present. Precision is optional and only
    // applies to double/decimal128.

    // Expect precision only to be set for double or decimal128.
    if (has_precision) {
        if (!ro->min.set) {
            CLIENT_ERR("setting precision requires min");
            return false;
        }

        bson_type_t minType = bson_iter_type(&ro->min.value);
        if (minType != BSON_TYPE_DOUBLE && minType != BSON_TYPE_DECIMAL128) {
            CLIENT_ERR("expected 'precision' to be set with double or decimal128 "
                       "index, but got: %s min",
                       mc_bson_type_to_string(minType));
            return false;
        }

        if (!ro->max.set) {
            CLIENT_ERR("setting precision requires max");
            return false;
        }

        bson_type_t maxType = bson_iter_type(&ro->max.value);
        if (maxType != BSON_TYPE_DOUBLE && maxType != BSON_TYPE_DECIMAL128) {
            CLIENT_ERR("expected 'precision' to be set with double or decimal128 "
                       "index, but got: %s max",
                       mc_bson_type_to_string(maxType));
            return false;
        }
    }

    // Expect min and max to match types.
    if (ro->min.set && ro->max.set) {
        bson_type_t minType = bson_iter_type(&ro->min.value), maxType = bson_iter_type(&ro->max.value);
        if (minType != maxType) {
            CLIENT_ERR("expected 'min' and 'max' to be same type, but got: %s "
                       "min and %s max",
                       mc_bson_type_to_string(minType),
                       mc_bson_type_to_string(maxType));
            return false;
        }
    }

    if (ro->min.set || ro->max.set) {
        // Setting min/max without precision is error for double and decimal128.
        if (ro->min.set) {
            bson_type_t minType = bson_iter_type(&ro->min.value);
            if (minType == BSON_TYPE_DOUBLE || minType == BSON_TYPE_DECIMAL128) {
                if (!has_precision) {
                    CLIENT_ERR("expected 'precision' to be set with 'min' for %s", mc_bson_type_to_string(minType));
                    return false;
                }
            }
        }

        if (ro->max.set) {
            bson_type_t maxType = bson_iter_type(&ro->max.value);
            if (maxType == BSON_TYPE_DOUBLE || maxType == BSON_TYPE_DECIMAL128) {
                if (!has_precision) {
                    CLIENT_ERR("expected 'precision' to be set with 'max' for %s", mc_bson_type_to_string(maxType));
                    return false;
                }
            }
        }
    }

    return true;
}

bool mc_RangeOpts_to_FLE2RangeInsertSpec(const mc_RangeOpts_t *ro,
                                         const bson_t *v,
                                         bson_t *out,
                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(ro);
    BSON_ASSERT_PARAM(v);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(status || true);

    const char *const error_prefix = "Error making FLE2RangeInsertSpec: ";
    bson_iter_t v_iter;
    if (!bson_iter_init_find(&v_iter, v, "v")) {
        CLIENT_ERR("Unable to find 'v' in input");
        return false;
    }

    bson_t child;
    if (!BSON_APPEND_DOCUMENT_BEGIN(out, "v", &child)) {
        CLIENT_ERR("%sError appending to BSON", error_prefix);
        return false;
    }
    if (!bson_append_iter(&child, "v", 1, &v_iter)) {
        CLIENT_ERR("%sError appending to BSON", error_prefix);
        return false;
    }

    if (!mc_RangeOpts_appendMin(ro, bson_iter_type(&v_iter), "min", &child, status)) {
        return false;
    }

    if (!mc_RangeOpts_appendMax(ro, bson_iter_type(&v_iter), "max", &child, status)) {
        return false;
    }

    if (ro->precision.set) {
        BSON_ASSERT(ro->precision.value <= INT32_MAX);
        if (!BSON_APPEND_INT32(&child, "precision", (int32_t)ro->precision.value)) {
            CLIENT_ERR("%sError appending to BSON", error_prefix);
            return false;
        }
    }
    if (!bson_append_document_end(out, &child)) {
        CLIENT_ERR("%sError appending to BSON", error_prefix);
        return false;
    }
    return true;
}

bool mc_RangeOpts_appendMin(const mc_RangeOpts_t *ro,
                            bson_type_t valueType,
                            const char *fieldName,
                            bson_t *out,
                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(ro);
    BSON_ASSERT_PARAM(fieldName);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(status || true);

    if (ro->min.set) {
        if (bson_iter_type(&ro->min.value) != valueType) {
            CLIENT_ERR("expected matching 'min' and value type. Got range option "
                       "'min' of type %s and value of type %s",
                       mc_bson_type_to_string(bson_iter_type(&ro->min.value)),
                       mc_bson_type_to_string(valueType));
            return false;
        }
        if (!bson_append_iter(out, fieldName, -1, &ro->min.value)) {
            CLIENT_ERR("failed to append BSON");
            return false;
        }
        return true;
    }

    if (valueType == BSON_TYPE_INT32 || valueType == BSON_TYPE_INT64 || valueType == BSON_TYPE_DATE_TIME) {
        CLIENT_ERR("Range option 'min' is required for type: %s", mc_bson_type_to_string(valueType));
        return false;
    } else if (valueType == BSON_TYPE_DOUBLE) {
        if (!BSON_APPEND_DOUBLE(out, fieldName, -DBL_MAX)) {
            CLIENT_ERR("failed to append BSON");
            return false;
        }
    } else if (valueType == BSON_TYPE_DECIMAL128) {
#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
        const bson_decimal128_t min = mc_dec128_to_bson_decimal128(MC_DEC128_LARGEST_NEGATIVE);
        if (!BSON_APPEND_DECIMAL128(out, fieldName, &min)) {
            CLIENT_ERR("failed to append BSON");
            return false;
        }
#else  // ↑↑↑↑↑↑↑↑ With Decimal128 / Without ↓↓↓↓↓↓↓↓↓↓
        CLIENT_ERR("unsupported BSON type (Decimal128) for range: libmongocrypt "
                   "was built without extended Decimal128 support");
        return false;
#endif // MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
    } else {
        CLIENT_ERR("unsupported BSON type: %s for range", mc_bson_type_to_string(valueType));
        return false;
    }
    return true;
}

bool mc_RangeOpts_appendMax(const mc_RangeOpts_t *ro,
                            bson_type_t valueType,
                            const char *fieldName,
                            bson_t *out,
                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(ro);
    BSON_ASSERT_PARAM(fieldName);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(status || true);

    if (ro->max.set) {
        if (bson_iter_type(&ro->max.value) != valueType) {
            CLIENT_ERR("expected matching 'max' and value type. Got range option "
                       "'max' of type %s and value of type %s",
                       mc_bson_type_to_string(bson_iter_type(&ro->max.value)),
                       mc_bson_type_to_string(valueType));
            return false;
        }
        if (!bson_append_iter(out, fieldName, -1, &ro->max.value)) {
            CLIENT_ERR("failed to append BSON");
            return false;
        }
        return true;
    }

    if (valueType == BSON_TYPE_INT32 || valueType == BSON_TYPE_INT64 || valueType == BSON_TYPE_DATE_TIME) {
        CLIENT_ERR("Range option 'max' is required for type: %s", mc_bson_type_to_string(valueType));
        return false;
    } else if (valueType == BSON_TYPE_DOUBLE) {
        if (!BSON_APPEND_DOUBLE(out, fieldName, DBL_MAX)) {
            CLIENT_ERR("failed to append BSON");
            return false;
        }
    } else if (valueType == BSON_TYPE_DECIMAL128) {
#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
        const bson_decimal128_t max = mc_dec128_to_bson_decimal128(MC_DEC128_LARGEST_POSITIVE);
        if (!BSON_APPEND_DECIMAL128(out, fieldName, &max)) {
            CLIENT_ERR("failed to append BSON");
            return false;
        }
#else  // ↑↑↑↑↑↑↑↑ With Decimal128 / Without ↓↓↓↓↓↓↓↓↓↓
        CLIENT_ERR("unsupported BSON type (Decimal128) for range: libmongocrypt "
                   "was built without extended Decimal128 support");
        return false;
#endif // MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
    } else {
        CLIENT_ERR("unsupported BSON type: %s for range", mc_bson_type_to_string(valueType));
        return false;
    }
    return true;
}

void mc_RangeOpts_cleanup(mc_RangeOpts_t *ro) {
    if (!ro) {
        return;
    }

    bson_destroy(ro->bson);
}

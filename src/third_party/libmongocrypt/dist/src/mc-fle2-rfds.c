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

#include "mc-fle2-rfds-private.h"

#include "mc-check-conversions-private.h"
#include "mc-fle-blob-subtype-private.h" // MC_SUBTYPE_FLE2EncryptionPlaceholder
#include "mongocrypt-private.h"          // CLIENT_ERR

#include "mlib/thread.h" // mlib_once_flag
#include <math.h>        // INFINITY

static mc_FLE2RangeOperator_t get_operator_type(const char *key) {
    BSON_ASSERT_PARAM(key);

    if (0 == strcmp(key, "$gt")) {
        return FLE2RangeOperator_kGt;
    } else if (0 == strcmp(key, "$gte")) {
        return FLE2RangeOperator_kGte;
    } else if (0 == strcmp(key, "$lt")) {
        return FLE2RangeOperator_kLt;
    } else if (0 == strcmp(key, "$lte")) {
        return FLE2RangeOperator_kLte;
    } else {
        return FLE2RangeOperator_kNone;
    }
}

static const char *mc_FLE2RangeOperator_to_string(mc_FLE2RangeOperator_t op) {
    switch (op) {
    case FLE2RangeOperator_kGt: return "$gt";
    case FLE2RangeOperator_kGte: return "$gte";
    case FLE2RangeOperator_kLt: return "$lt";
    case FLE2RangeOperator_kLte: return "$lte";
    case FLE2RangeOperator_kNone: return "none";
    default: return "Unknown";
    }
}

static bool is_supported_operator(const char *key) {
    BSON_ASSERT_PARAM(key);
    return get_operator_type(key) != FLE2RangeOperator_kNone;
}

// Suffix an error message with the JSON string form of bson.
#define ERR_WITH_BSON(bson, fmt, ...)                                                                                  \
    if (1) {                                                                                                           \
        char *_str = bson_as_canonical_extended_json((bson), NULL);                                                    \
        CLIENT_ERR(fmt ": %s", __VA_ARGS__, _str);                                                                     \
        bson_free(_str);                                                                                               \
    } else                                                                                                             \
        (void)0

// Parses a document like {$and: []} and outputs an iterator to the array.
static bool parse_and(const bson_t *in, bson_iter_t *out, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(status || true);

    bson_iter_t and;
    if (!bson_iter_init(&and, in) || !bson_iter_next(&and) || 0 != strcmp(bson_iter_key(&and), "$and")) {
        ERR_WITH_BSON(in, "%s", "error unable to find '$and'");
        return false;
    }

    if (!BSON_ITER_HOLDS_ARRAY(&and)) {
        ERR_WITH_BSON(in, "%s", "expected '$and' to be array");
        return false;
    }

    *out = and;

    if (bson_iter_next(&and)) {
        ERR_WITH_BSON(in, "unexpected extra key '%s' after '$and'", bson_iter_key(&and));
        return false;
    }
    return true;
}

typedef struct {
    const char *op_type_str;
    const char *field;
    bson_iter_t value;
    mc_FLE2RangeOperator_t op_type;
} operator_value_t;

// Parses a document like {$gt: ["$age", 5]}.
static bool
parse_aggregate_expression(const bson_t *orig, bson_iter_t *in, operator_value_t *out, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(orig);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(status || true);

    bson_iter_t array, value;
    const char *op_type_str = bson_iter_key(in);
    bool ok = false;
    const char *field;

    if (!BSON_ITER_HOLDS_ARRAY(in)) {
        ERR_WITH_BSON(orig, "%s", "expected argument to be array");
        goto fail;
    }

    if (!bson_iter_recurse(in, &array)) {
        ERR_WITH_BSON(orig, "%s", "failed to recurse into array");
        goto fail;
    }

    // Expect exactly 2 elements, like ["$age", 5]. The first element is the
    // field path. The second element is the value.
    if (!bson_iter_next(&array)) {
        ERR_WITH_BSON(orig, "expected 2 elements in operand %s, got 0", op_type_str);
        goto fail;
    }
    if (!BSON_ITER_HOLDS_UTF8(&array)) {
        ERR_WITH_BSON(orig, "expected UTF-8 as first element in %s", op_type_str);
        goto fail;
    }
    field = bson_iter_utf8(&array, NULL);

    if (!bson_iter_next(&array)) {
        ERR_WITH_BSON(orig, "expected 2 elements in operand %s, got 1", op_type_str);
        goto fail;
    }
    value = array;
    if (bson_iter_next(&array)) {
        ERR_WITH_BSON(orig, "expected 2 elements in operand %s, got > 2", op_type_str);
        goto fail;
    }

    *out = (operator_value_t){
        .field = field,
        .op_type_str = op_type_str,
        .op_type = get_operator_type(op_type_str),
        .value = value,
    };

    ok = true;
fail:
    return ok;
}

// Parses a document like {age: {$gt: 5}}.
static bool
parse_match_expression(const bson_t *orig, bson_iter_t *in, operator_value_t *out, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(orig);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(status || true);

    bson_iter_t document, value;
    const char *op_type_str;
    bool ok = false;
    const char *field = bson_iter_key(in);

    if (!BSON_ITER_HOLDS_DOCUMENT(in)) {
        ERR_WITH_BSON(orig, "%s", "expected argument to be document");
        goto fail;
    }

    if (!bson_iter_recurse(in, &document)) {
        ERR_WITH_BSON(orig, "%s", "failed to recurse into document");
        goto fail;
    }

    // Expect exactly 1 elements, like {$gt: 5}.
    if (!bson_iter_next(&document)) {
        ERR_WITH_BSON(orig, "expected 1 elements in operand %s, got 0", field);
        goto fail;
    }
    op_type_str = bson_iter_key(&document);
    if (!is_supported_operator(op_type_str)) {
        ERR_WITH_BSON(orig, "unsupported operator: %s", op_type_str);
        goto fail;
    }
    value = document;

    if (bson_iter_next(&document)) {
        ERR_WITH_BSON(orig, "expected 1 elements in operand %s, got > 1", field);
        goto fail;
    }

    *out = (operator_value_t){
        .field = field,
        .op_type_str = op_type_str,
        .op_type = get_operator_type(op_type_str),
        .value = value,
    };

    ok = true;
fail:
    return ok;
}

bool mc_FLE2RangeFindDriverSpec_parse(mc_FLE2RangeFindDriverSpec_t *spec,
                                      const bson_t *in,
                                      mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT(status || true);

    *spec = (mc_FLE2RangeFindDriverSpec_t){0};
    // `in` may be an Aggregate Expression with this form:
    // {$and: [{$gt: ["$age", 5]}, {$lt:["$age", 50]}]}
    // Or `in` may be a Match Expression with this form:
    // {$and: [{age: {$gt: 5}}, {age: {$lt: 50}} ]}
    bson_iter_t and, array;
    bool ok = false;

    if (!parse_and(in, &and, status)) {
        goto fail;
    }

    // Iterate over array elements.
    if (!bson_iter_recurse(&and, &array)) {
        ERR_WITH_BSON(in, "%s", "failed to recurse into '$and'");
        goto fail;
    }

    enum { UNKNOWN, MATCH_EXPRESSION, AGGREGATE_EXPRESSION } arg_type = UNKNOWN;

    while (bson_iter_next(&array)) {
        bson_iter_t doc;

        if (!BSON_ITER_HOLDS_DOCUMENT(&array)) {
            ERR_WITH_BSON(in, "%s", "expected document in '$and' array");
            goto fail;
        }

        if (!bson_iter_recurse(&array, &doc)) {
            ERR_WITH_BSON(in, "%s", "failed to recurse into '$and' element");
            goto fail;
        }

        if (!bson_iter_next(&doc)) {
            ERR_WITH_BSON(in, "%s", "unexpected empty '$and' array document");
            goto fail;
        }

        if (arg_type == UNKNOWN) {
            // Attempt to determine argument type by inspecting first key.
            if (is_supported_operator(bson_iter_key(&doc))) {
                // Assume the document is part of an Aggregate Expression, like:
                // {$gt: ["$age", 5]}
                arg_type = AGGREGATE_EXPRESSION;
                spec->isAggregateExpression = true;
            } else {
                // Assume the document is part of a Match Expression, like:
                // {age: {$gt: 5}}
                arg_type = MATCH_EXPRESSION;
            }
        }

        operator_value_t op;
        switch (arg_type) {
        case AGGREGATE_EXPRESSION:
            if (!parse_aggregate_expression(in, &doc, &op, status)) {
                goto fail;
            }
            break;
        case MATCH_EXPRESSION:
            if (!parse_match_expression(in, &doc, &op, status)) {
                goto fail;
            }
            break;
        case UNKNOWN:
        default: ERR_WITH_BSON(in, "%s", "unexpected unknown expression type"); goto fail;
        }

        switch (op.op_type) {
        case FLE2RangeOperator_kGt:
        case FLE2RangeOperator_kGte:
            if (spec->lower.set) {
                ERR_WITH_BSON(in, "unexpected duplicate bound %s", op.op_type_str);
                goto fail;
            }
            spec->lower.set = true;
            spec->lower.value = op.value;
            spec->lower.included = op.op_type == FLE2RangeOperator_kGte;
            break;
        case FLE2RangeOperator_kLt:
        case FLE2RangeOperator_kLte:
            if (spec->upper.set) {
                ERR_WITH_BSON(in, "unexpected duplicate bound %s", op.op_type_str);
                goto fail;
            }
            spec->upper.set = true;
            spec->upper.value = op.value;
            spec->upper.included = op.op_type == FLE2RangeOperator_kLte;
            break;
        case FLE2RangeOperator_kNone:
        default:
            ERR_WITH_BSON(in, "unsupported operator type %s", op.op_type_str);
            goto fail;
            break;
        }

        if (spec->field && 0 != strcmp(spec->field, op.field)) {
            ERR_WITH_BSON(in,
                          "unexpected field mismatch. Expected all fields to be "
                          "%s, but got %s",
                          spec->field,
                          op.field);
            goto fail;
        }

        spec->nOps++;

        switch (spec->nOps) {
        case 1: spec->firstOp = op.op_type; break;
        case 2: spec->secondOp = op.op_type; break;
        default: ERR_WITH_BSON(in, "expected 1 or 2 operators, got > 2: %s", op.op_type_str); goto fail;
        }

        spec->field = op.field;
    }

    ok = true;
fail:
    return ok;
}

// mc_makeRangeFindPlaceholder creates a placeholder to be consumed by
// libmongocrypt for encryption.
bool mc_makeRangeFindPlaceholder(mc_makeRangeFindPlaceholder_args_t *args,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(args);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(status || true);

    bool ok = false;
    bson_t *edgesInfo = bson_new();
    bson_t *v = bson_new();
    bson_t *p = bson_new();
    _mongocrypt_buffer_init(out);

#define TRY(stmt)                                                                                                      \
    if (!(stmt)) {                                                                                                     \
        CLIENT_ERR("error appending BSON for placeholder");                                                            \
        goto fail;                                                                                                     \
    }

    // create edgesInfo.

    if (!args->isStub) {
        TRY(bson_append_iter(edgesInfo, "lowerBound", -1, &args->lowerBound));
        TRY(BSON_APPEND_BOOL(edgesInfo, "lbIncluded", args->lbIncluded));
        TRY(bson_append_iter(edgesInfo, "upperBound", -1, &args->upperBound));
        TRY(BSON_APPEND_BOOL(edgesInfo, "ubIncluded", args->ubIncluded));
        TRY(bson_append_iter(edgesInfo, "indexMin", -1, &args->indexMin));
        TRY(bson_append_iter(edgesInfo, "indexMax", -1, &args->indexMax));
        if (args->precision.set) {
            BSON_ASSERT(args->precision.value <= INT32_MAX);
            TRY(BSON_APPEND_INT32(edgesInfo, "precision", (int32_t)args->precision.value));
        }
        TRY(BSON_APPEND_DOCUMENT(v, "edgesInfo", edgesInfo));
    }

    // create v.
    TRY(BSON_APPEND_INT32(v, "payloadId", args->payloadId));
    TRY(BSON_APPEND_INT32(v, "firstOperator", (int32_t)args->firstOp));
    if (args->secondOp) {
        TRY(BSON_APPEND_INT32(v, "secondOperator", (int32_t)args->secondOp));
    }

    // create placeholder.
    TRY(BSON_APPEND_INT32(p, "t", MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND));
    TRY(BSON_APPEND_INT32(p, "a", MONGOCRYPT_FLE2_ALGORITHM_RANGE));
    TRY(_mongocrypt_buffer_append(args->index_key_id, p, "ki", 2));
    TRY(_mongocrypt_buffer_append(args->user_key_id, p, "ku", 2));
    TRY(BSON_APPEND_DOCUMENT(p, "v", v));
    TRY(BSON_APPEND_INT64(p, "cm", args->maxContentionCounter));
    TRY(BSON_APPEND_INT64(p, "s", args->sparsity));
#undef TRY

    BSON_ASSERT(p->len < UINT32_MAX);
    _mongocrypt_buffer_resize(out, p->len + 1u);
    out->subtype = BSON_SUBTYPE_ENCRYPTED;
    out->data[0] = MC_SUBTYPE_FLE2EncryptionPlaceholder;
    memcpy(out->data + 1, bson_get_data(p), p->len);

    ok = true;
fail:
    bson_destroy(p);
    bson_destroy(v);
    bson_destroy(edgesInfo);
    return ok;
}

bool mc_FLE2RangeFindDriverSpec_to_placeholders(mc_FLE2RangeFindDriverSpec_t *spec,
                                                const mc_RangeOpts_t *range_opts,
                                                int64_t maxContentionCounter,
                                                const _mongocrypt_buffer_t *user_key_id,
                                                const _mongocrypt_buffer_t *index_key_id,
                                                int32_t payloadId,
                                                bson_t *out,
                                                mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(spec);
    BSON_ASSERT_PARAM(range_opts);
    BSON_ASSERT_PARAM(user_key_id);
    BSON_ASSERT_PARAM(index_key_id);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(status || true);

    _mongocrypt_buffer_t p1 = {0}, p2 = {0};
    bson_t infDoc = BSON_INITIALIZER;
    bson_iter_t negInf, posInf;
    bson_t minMaxDoc = BSON_INITIALIZER;
    bson_iter_t indexMin, indexMax;
    bool ok = false;

    BCON_APPEND(&infDoc, "p", BCON_DOUBLE(INFINITY), "n", BCON_DOUBLE(-INFINITY));

#define TRY(stmt)                                                                                                      \
    if (!(stmt)) {                                                                                                     \
        CLIENT_ERR("error transforming BSON for FLE2RangeFindDriverSpec: %s", #stmt);                                  \
        goto fail;                                                                                                     \
    }

    TRY(bson_iter_init_find(&posInf, &infDoc, "p"));
    TRY(bson_iter_init_find(&negInf, &infDoc, "n"));

    // Apply default index min/max values.
    {
        bson_type_t index_type;
        if (spec->lower.set) {
            index_type = bson_iter_type(&spec->lower.value);
        } else if (spec->upper.set) {
            index_type = bson_iter_type(&spec->upper.value);
        } else {
            CLIENT_ERR("expected lower or upper bound to be set");
            goto fail;
        }
        if (!mc_RangeOpts_appendMin(range_opts, index_type, "indexMin", &minMaxDoc, status)) {
            goto fail;
        }
        if (!mc_RangeOpts_appendMax(range_opts, index_type, "indexMax", &minMaxDoc, status)) {
            goto fail;
        }

        TRY(bson_iter_init_find(&indexMin, &minMaxDoc, "indexMin"));
        TRY(bson_iter_init_find(&indexMax, &minMaxDoc, "indexMax"));
    }

    mc_makeRangeFindPlaceholder_args_t args = {.isStub = false,
                                               .user_key_id = user_key_id,
                                               .index_key_id = index_key_id,
                                               .lowerBound = spec->lower.set ? spec->lower.value : negInf,
                                               .lbIncluded = spec->lower.set ? spec->lower.included : true,
                                               .upperBound = spec->upper.set ? spec->upper.value : posInf,
                                               .ubIncluded = spec->upper.set ? spec->upper.included : true,
                                               .payloadId = payloadId,
                                               .firstOp = spec->firstOp,
                                               .secondOp = spec->secondOp,
                                               .indexMin = indexMin,
                                               .indexMax = indexMax,
                                               .precision = range_opts->precision,
                                               .maxContentionCounter = maxContentionCounter,
                                               .sparsity = range_opts->sparsity};

    // First operator is the non-stub.
    if (!mc_makeRangeFindPlaceholder(&args, &p1, status)) {
        goto fail;
    }

    // Second operator (if required) is a stub.
    if (spec->nOps == 2) {
        mc_makeRangeFindPlaceholder_args_t args = {.isStub = true,
                                                   .user_key_id = user_key_id,
                                                   .index_key_id = index_key_id,
                                                   .payloadId = payloadId,
                                                   .firstOp = spec->firstOp,
                                                   .secondOp = spec->secondOp,
                                                   .maxContentionCounter = maxContentionCounter,
                                                   .sparsity = range_opts->sparsity};

        // First operator is the non-stub.
        if (!mc_makeRangeFindPlaceholder(&args, &p2, status)) {
            goto fail;
        }
    }

    if (spec->isAggregateExpression) {
        /*
        Create an Aggregate Expression document like:
        {
        "$and" : [
              {"$gt" : [ "$age", "<placeholder1>" ]},
              {"$lt" : [ "$age", "<placeholder2>" ]}
           ]
        }
        */
        bson_t and;
        TRY(BSON_APPEND_ARRAY_BEGIN(out, "$and", &and));
        bson_t elem;
        TRY(BSON_APPEND_DOCUMENT_BEGIN(&and, "0", &elem));
        bson_t operator;
        TRY(BSON_APPEND_ARRAY_BEGIN(&elem, mc_FLE2RangeOperator_to_string(spec->firstOp), &operator));
        TRY(BSON_APPEND_UTF8(&operator, "0", spec->field));
        TRY(_mongocrypt_buffer_append(&p1, &operator, "1", 1));
        TRY(bson_append_array_end(&elem, &operator));
        TRY(bson_append_document_end(&and, &elem));

        if (spec->nOps == 2) {
            TRY(BSON_APPEND_DOCUMENT_BEGIN(&and, "1", &elem));
            TRY(BSON_APPEND_ARRAY_BEGIN(&elem, mc_FLE2RangeOperator_to_string(spec->secondOp), &operator));
            TRY(BSON_APPEND_UTF8(&operator, "0", spec->field));
            TRY(_mongocrypt_buffer_append(&p2, &operator, "1", 1));
            TRY(bson_append_array_end(&elem, &operator));
            TRY(bson_append_document_end(&and, &elem));
        }

        TRY(bson_append_array_end(out, &and));
    } else {
        /*
        Create a Match Expression document like:
        {
          "$and" : [
              {"age" : { "$gt": "<placeholder1>" }},
              {"age" : { "$lt": "<placeholder2>" }}
           ]
        }
        */
        bson_t and;
        TRY(BSON_APPEND_ARRAY_BEGIN(out, "$and", &and));
        bson_t elem;
        TRY(BSON_APPEND_DOCUMENT_BEGIN(&and, "0", &elem));
        bson_t operator;
        TRY(BSON_APPEND_DOCUMENT_BEGIN(&elem, spec->field, &operator));
        const char *op_str = mc_FLE2RangeOperator_to_string(spec->firstOp);
        TRY(_mongocrypt_buffer_append(&p1, &operator, op_str, -1));
        TRY(bson_append_document_end(&elem, &operator));
        TRY(bson_append_document_end(&and, &elem));

        if (spec->nOps == 2) {
            TRY(BSON_APPEND_DOCUMENT_BEGIN(&and, "1", &elem));
            TRY(BSON_APPEND_DOCUMENT_BEGIN(&elem, spec->field, &operator));
            const char *op_str = mc_FLE2RangeOperator_to_string(spec->secondOp);
            TRY(_mongocrypt_buffer_append(&p2, &operator, op_str, -1));
            TRY(bson_append_document_end(&elem, &operator));
            TRY(bson_append_document_end(&and, &elem));
        }

        TRY(bson_append_array_end(out, &and));
    }

#undef TRY
    ok = true;
fail:
    _mongocrypt_buffer_cleanup(&p2);
    _mongocrypt_buffer_cleanup(&p1);
    bson_destroy(&minMaxDoc);
    bson_destroy(&infDoc);
    return ok;
}

static mlib_once_flag payloadId_init_flag = MLIB_ONCE_INITIALIZER;
static mongocrypt_mutex_t payloadId_mutex;
static int32_t payloadId = 0;

static void payloadId_init_mutex(void) {
    _mongocrypt_mutex_init(&payloadId_mutex);
}

void mc_reset_payloadId_for_testing(void) {
    mlib_call_once(&payloadId_init_flag, payloadId_init_mutex);
    MONGOCRYPT_WITH_MUTEX(payloadId_mutex) {
        payloadId = 0;
    }
}

// mc_getNextPayloadId is thread safe.
int32_t mc_getNextPayloadId(void) {
    mlib_call_once(&payloadId_init_flag, payloadId_init_mutex);
    int32_t ret;
    MONGOCRYPT_WITH_MUTEX(payloadId_mutex) {
        ret = payloadId;
        payloadId = payloadId < INT32_MAX ? payloadId + 1 : 0;
    }
    return ret;
}

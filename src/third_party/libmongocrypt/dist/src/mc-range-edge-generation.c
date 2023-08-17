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

#include "mc-range-edge-generation-private.h"

#include "mc-array-private.h"
#include "mc-check-conversions-private.h"
#include "mc-range-encoding-private.h"
#include "mongocrypt-private.h"

struct _mc_edges_t {
    size_t sparsity;
    /* edges is an array of `char*` edge strings. */
    mc_array_t edges;
};

static mc_edges_t *mc_edges_new(const char *leaf, size_t sparsity, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(leaf);
    if (sparsity < 1) {
        CLIENT_ERR("sparsity must be 1 or larger");
        return NULL;
    }
    mc_edges_t *edges = bson_malloc0(sizeof(mc_edges_t));
    edges->sparsity = sparsity;
    _mc_array_init(&edges->edges, sizeof(char *));

    char *root = bson_strdup("root");
    _mc_array_append_val(&edges->edges, root);

    char *leaf_copy = bson_strdup(leaf);
    _mc_array_append_val(&edges->edges, leaf_copy);

    const size_t leaf_len = strlen(leaf);
    // Start loop at 1. The full leaf is unconditionally appended after loop.
    for (size_t i = 1; i < leaf_len; i++) {
        if (i % sparsity == 0) {
            char *edge = bson_malloc(i + 1);
            bson_strncpy(edge, leaf, i + 1);
            _mc_array_append_val(&edges->edges, edge);
        }
    }

    return edges;
}

const char *mc_edges_get(mc_edges_t *edges, size_t index) {
    BSON_ASSERT_PARAM(edges);
    if (edges->edges.len == 0 || index > edges->edges.len - 1u) {
        return NULL;
    }
    return _mc_array_index(&edges->edges, char *, index);
}

size_t mc_edges_len(mc_edges_t *edges) {
    BSON_ASSERT_PARAM(edges);
    return edges->edges.len;
}

void mc_edges_destroy(mc_edges_t *edges) {
    if (NULL == edges) {
        return;
    }
    for (size_t i = 0; i < edges->edges.len; i++) {
        char *val = _mc_array_index(&edges->edges, char *, i);
        bson_free(val);
    }
    _mc_array_destroy(&edges->edges);
    bson_free(edges);
}

mc_bitstring mc_convert_to_bitstring_u64(uint64_t in) {
    mc_bitstring ret = {{0}};
    char *out = ret.str;
    uint64_t bit = UINT64_C(1) << 63;
    while (bit > 0) {
        if (bit & in) {
            *out++ = '1';
        } else {
            *out++ = '0';
        }
        bit >>= 1;
    }
    return ret;
}

mc_bitstring mc_convert_to_bitstring_u32(uint32_t in) {
    mc_bitstring ret = {{0}};
    char *out = ret.str;
    uint32_t bit = UINT32_C(1) << 31;
    while (bit > 0) {
        if (bit & in) {
            *out++ = '1';
        } else {
            *out++ = '0';
        }
        bit >>= 1;
    }
    return ret;
}

mc_bitstring mc_convert_to_bitstring_u128(mlib_int128 i) {
    const uint64_t lo = mlib_int128_to_u64(i);
    const uint64_t hi = mlib_int128_to_u64(mlib_int128_rshift(i, 64));
    mc_bitstring his = mc_convert_to_bitstring_u64(hi);
    mc_bitstring los = mc_convert_to_bitstring_u64(lo);
    mc_bitstring ret = {{0}};
    const size_t off = mlib_strnmcopy(ret.str, sizeof ret.str, his.str, sizeof his.str);
    mlib_strnmcopy(ret.str + off, (sizeof ret.str) - off, los.str, sizeof los.str);
    return ret;
}

mc_edges_t *mc_getEdgesInt32(mc_getEdgesInt32_args_t args, mongocrypt_status_t *status) {
    mc_OSTType_Int32 got;
    if (!mc_getTypeInfo32((mc_getTypeInfo32_args_t){.value = args.value, .min = args.min, .max = args.max},
                          &got,
                          status)) {
        return NULL;
    }

    // `max` is the domain of values. `max` is used to determine the maximum bit
    // length. `min` is expected to be zero. The `min` and `max` terms are kept
    // for consistency with the server implementation.
    BSON_ASSERT(got.min == 0);

    mc_bitstring valueBin = mc_convert_to_bitstring_u32(got.value);
    size_t offset = mc_count_leading_zeros_u32(got.max);
    const char *leaf = valueBin.str + offset;
    mc_edges_t *ret = mc_edges_new(leaf, args.sparsity, status);
    return ret;
}

mc_edges_t *mc_getEdgesInt64(mc_getEdgesInt64_args_t args, mongocrypt_status_t *status) {
    mc_OSTType_Int64 got;
    if (!mc_getTypeInfo64((mc_getTypeInfo64_args_t){.value = args.value, .min = args.min, .max = args.max},
                          &got,
                          status)) {
        return NULL;
    }

    // `max` is the domain of values. `max` is used to determine the maximum bit
    // length. `min` is expected to be zero. The `min` and `max` terms are kept
    // for consistency with the server implementation.
    BSON_ASSERT(got.min == 0);

    mc_bitstring valueBin = mc_convert_to_bitstring_u64(got.value);
    size_t offset = mc_count_leading_zeros_u64(got.max);
    const char *leaf = valueBin.str + offset;
    mc_edges_t *ret = mc_edges_new(leaf, args.sparsity, status);
    return ret;
}

mc_edges_t *mc_getEdgesDouble(mc_getEdgesDouble_args_t args, mongocrypt_status_t *status) {
    mc_OSTType_Double got;
    if (!mc_getTypeInfoDouble((mc_getTypeInfoDouble_args_t){.value = args.value,
                                                            .min = args.min,
                                                            .max = args.max,
                                                            .precision = args.precision},
                              &got,
                              status)) {
        return NULL;
    }

    // `max` is the domain of values. `max` is used to determine the maximum bit
    // length. `min` is expected to be zero. The `min` and `max` terms are kept
    // for consistency with the server implementation.
    BSON_ASSERT(got.min == 0);

    mc_bitstring valueBin = mc_convert_to_bitstring_u64(got.value);
    size_t offset = mc_count_leading_zeros_u64(got.max);
    const char *leaf = valueBin.str + offset;
    mc_edges_t *ret = mc_edges_new(leaf, args.sparsity, status);
    return ret;
}

#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
mc_edges_t *mc_getEdgesDecimal128(mc_getEdgesDecimal128_args_t args, mongocrypt_status_t *status) {
    mc_OSTType_Decimal128 got;
    if (!mc_getTypeInfoDecimal128(
            (mc_getTypeInfoDecimal128_args_t){
                .value = args.value,
                .min = args.min,
                .max = args.max,
                .precision = args.precision,
            },
            &got,
            status)) {
        return NULL;
    }

    BSON_ASSERT(mlib_int128_eq(got.min, MLIB_INT128(0)));

    mc_bitstring bits = mc_convert_to_bitstring_u128(got.value);
    size_t offset = mc_count_leading_zeros_u128(got.max);
    const char *leaf = bits.str + offset;
    mc_edges_t *ret = mc_edges_new(leaf, args.sparsity, status);
    return ret;
}
#endif // MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
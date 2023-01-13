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

#ifndef MC_RANGE_EDGE_GENERATION_PRIVATE_H
#define MC_RANGE_EDGE_GENERATION_PRIVATE_H

#include <stddef.h> // size_t
#include <stdint.h>
#include "mc-optional-private.h"
#include "mongocrypt-status-private.h"

// mc_edges_t represents a list of edges.
typedef struct _mc_edges_t mc_edges_t;

// mc_edges_get returns edge at an index.
// Returns NULL if `index` is out of range.
const char *
mc_edges_get (mc_edges_t *edges, size_t index);

// mc_edges_len returns the number of represented edges.
size_t
mc_edges_len (mc_edges_t *edges);

// mc_edges_destroys frees `edges`.
void
mc_edges_destroy (mc_edges_t *edges);

typedef struct {
   int32_t value;
   mc_optional_int32_t min;
   mc_optional_int32_t max;
   size_t sparsity;
} mc_getEdgesInt32_args_t;

// mc_getEdgesInt32 implements the Edge Generation algorithm described in
// SERVER-67751 for int32_t.
mc_edges_t *
mc_getEdgesInt32 (mc_getEdgesInt32_args_t args, mongocrypt_status_t *status);

typedef struct {
   int64_t value;
   mc_optional_int64_t min;
   mc_optional_int64_t max;
   size_t sparsity;
} mc_getEdgesInt64_args_t;

// mc_getEdgesInt64 implements the Edge Generation algorithm described in
// SERVER-67751 for int64_t.
mc_edges_t *
mc_getEdgesInt64 (mc_getEdgesInt64_args_t args, mongocrypt_status_t *status);

typedef struct {
   double value;
   size_t sparsity;
   mc_optional_double_t min;
   mc_optional_double_t max;
   mc_optional_uint32_t precision;
} mc_getEdgesDouble_args_t;

// mc_getEdgesDouble implements the Edge Generation algorithm described in
// SERVER-67751 for double.
mc_edges_t *
mc_getEdgesDouble (mc_getEdgesDouble_args_t args, mongocrypt_status_t *status);

// count_leading_zeros_u64 returns the number of leading 0 bits of `in`.
size_t
mc_count_leading_zeros_u64 (uint64_t in);

// count_leading_zeros_u32 returns the number of leading 0 bits of `in`.
size_t
mc_count_leading_zeros_u32 (uint32_t in);

// mc_convert_to_bitstring_u64 returns a 64 character string of 1's and 0's
// representing the bits of `in`. Caller must call `bson_free` on returned
// value.
char *
mc_convert_to_bitstring_u64 (uint64_t in);

// mc_convert_to_bitstring_u32 returns a 32 character string of 1's and 0's
// representing the bits of `in`. Caller must call `bson_free` on returned
// value.
char *
mc_convert_to_bitstring_u32 (uint32_t in);

#endif /* MC_RANGE_EDGE_GENERATION_PRIVATE_H */

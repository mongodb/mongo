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

#include "mc-check-conversions-private.h"

#include "mc-array-private.h"
#include "mc-range-edge-generation-private.h" // mc_count_leading_zeros_u32
#include "mc-range-encoding-private.h"        // mc_getTypeInfo32
#include "mc-range-mincover-private.h"
#include "mongocrypt-private.h"

MC_BEGIN_CONVERSION_ERRORS

struct _mc_mincover_t {
   /* mincover is an array of `char*` edge strings. */
   mc_array_t mincover;
};

static mc_mincover_t *
mc_mincover_new (void)
{
   mc_mincover_t *mincover = bson_malloc0 (sizeof (mc_mincover_t));
   _mc_array_init (&mincover->mincover, sizeof (char *));
   return mincover;
}

const char *
mc_mincover_get (mc_mincover_t *mincover, size_t index)
{
   BSON_ASSERT_PARAM (mincover);
   if (index > mincover->mincover.len - 1) {
      return NULL;
   }
   return _mc_array_index (&mincover->mincover, char *, index);
}

size_t
mc_mincover_len (mc_mincover_t *mincover)
{
   BSON_ASSERT_PARAM (mincover);
   return mincover->mincover.len;
}

void
mc_mincover_destroy (mc_mincover_t *mincover)
{
   if (NULL == mincover) {
      return;
   }
   for (size_t i = 0; i < mincover->mincover.len; i++) {
      char *val = _mc_array_index (&mincover->mincover, char *, i);
      bson_free (val);
   }
   _mc_array_destroy (&mincover->mincover);
   bson_free (mincover);
}

#define BITS 32
#include "mc-range-mincover-generator.template.h"
#undef BITS

mc_mincover_t *
mc_getMincoverInt32 (mc_getMincoverInt32_args_t args,
                     mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (status);
   mc_OSTType_Int32 a, b;
   if (!mc_getTypeInfo32 ((mc_getTypeInfo32_args_t){.min = args.min,
                                                    .max = args.max,
                                                    .value = args.lowerBound},
                          &a,
                          status)) {
      return NULL;
   }
   if (!mc_getTypeInfo32 ((mc_getTypeInfo32_args_t){.min = args.min,
                                                    .max = args.max,
                                                    .value = args.upperBound},
                          &b,
                          status)) {
      return NULL;
   }

   BSON_ASSERT (a.min == b.min);
   BSON_ASSERT (a.max == b.max);

   if (!adjustBounds_u32 (&a.value,
                          args.includeLowerBound,
                          a.min,
                          &b.value,
                          args.includeUpperBound,
                          b.max,
                          status)) {
      return NULL;
   }

   MinCoverGenerator_u32 *mcg = MinCoverGenerator_new_u32 (
      a.value, b.value, a.max, args.sparsity, status);
   if (!mcg) {
      return NULL;
   }
   mc_mincover_t *mc = MinCoverGenerator_minCover_u32 (mcg);
   MinCoverGenerator_destroy_u32 (mcg);
   return mc;
}

#define BITS 64
#include "mc-range-mincover-generator.template.h"
#undef BITS

mc_mincover_t *
mc_getMincoverInt64 (mc_getMincoverInt64_args_t args,
                     mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (status);
   mc_OSTType_Int64 a, b;
   if (!mc_getTypeInfo64 ((mc_getTypeInfo64_args_t){.min = args.min,
                                                    .max = args.max,
                                                    .value = args.lowerBound},
                          &a,
                          status)) {
      return NULL;
   }
   if (!mc_getTypeInfo64 ((mc_getTypeInfo64_args_t){.min = args.min,
                                                    .max = args.max,
                                                    .value = args.upperBound},
                          &b,
                          status)) {
      return NULL;
   }

   BSON_ASSERT (a.min == b.min);
   BSON_ASSERT (a.max == b.max);

   if (!adjustBounds_u64 (&a.value,
                          args.includeLowerBound,
                          a.min,
                          &b.value,
                          args.includeUpperBound,
                          b.max,
                          status)) {
      return NULL;
   }

   MinCoverGenerator_u64 *mcg = MinCoverGenerator_new_u64 (
      a.value, b.value, a.max, args.sparsity, status);
   if (!mcg) {
      return NULL;
   }
   mc_mincover_t *mc = MinCoverGenerator_minCover_u64 (mcg);
   MinCoverGenerator_destroy_u64 (mcg);
   return mc;
}

// mc_getMincoverDouble implements the Mincover Generation algorithm described
// in SERVER-68600 for double.
mc_mincover_t *
mc_getMincoverDouble (mc_getMincoverDouble_args_t args,
                      mongocrypt_status_t *status)
{
   BSON_ASSERT_PARAM (status);
   mc_OSTType_Double a, b;
   if (!mc_getTypeInfoDouble (
          (mc_getTypeInfoDouble_args_t){.value = args.lowerBound},
          &a,
          status)) {
      return NULL;
   }
   if (!mc_getTypeInfoDouble (
          (mc_getTypeInfoDouble_args_t){.value = args.upperBound},
          &b,
          status)) {
      return NULL;
   }

   BSON_ASSERT (a.min == b.min);
   BSON_ASSERT (a.max == b.max);

   if (!adjustBounds_u64 (&a.value,
                          args.includeLowerBound,
                          a.min,
                          &b.value,
                          args.includeUpperBound,
                          b.max,
                          status)) {
      return NULL;
   }

   MinCoverGenerator_u64 *mcg = MinCoverGenerator_new_u64 (
      a.value, b.value, a.max, args.sparsity, status);
   if (!mcg) {
      return NULL;
   }
   mc_mincover_t *mc = MinCoverGenerator_minCover_u64 (mcg);
   MinCoverGenerator_destroy_u64 (mcg);
   return mc;
}

MC_END_CONVERSION_ERRORS

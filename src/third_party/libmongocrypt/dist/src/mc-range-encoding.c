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

#include "mc-check-conversions-private.h"
#include "mc-range-encoding-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-util-private.h" // mc_isinf

#include <math.h> // pow

/* mc-range-encoding.c assumes integers are encoded with two's complement for
 * correctness. */
#if (-1 & 3) != 3
#error Error: Twos complement integer representation is required.
#endif

/**
 * Encode a signed 32-bit integer as an unsigned 32-bit integer by adding 2^31.
 * Some documentation references this as making the value "unbiased".
 */
static uint32_t
encodeInt32 (int32_t v)
{
   // Shift the int32_t range [-2^31, 2^31 - 1] to the uint32_t range [0, 2^32].
   // new_zero is the mapped 0 value.
   uint32_t new_zero = (UINT32_C (1) << 31);

   if (v < 0) {
      // Signed integers have a value that there is no positive equivalent and
      // must be handled specially
      if (v == INT32_MIN) {
         return 0;
      }

      int32_t v_pos = v * -1;
      uint32_t v_u32 = (uint32_t) v_pos;
      return new_zero - v_u32;
   }

   uint32_t v_u32 = (uint32_t) v;
   return new_zero + v_u32;
}

bool
mc_getTypeInfo32 (mc_getTypeInfo32_args_t args,
                  mc_OSTType_Int32 *out,
                  mongocrypt_status_t *status)
{
   if (args.min.set != args.max.set) {
      CLIENT_ERR ("Must specify both a lower and upper bound or no bounds.");
      return false;
   }

   if (!args.min.set) {
      uint32_t v_u32 = encodeInt32 (args.value);
      *out = (mc_OSTType_Int32){v_u32, 0, UINT32_MAX};
      return true;
   }

   if (args.min.value >= args.max.value) {
      CLIENT_ERR ("The minimum value must be less than the maximum value, got "
                  "min: %" PRId32 ", max: %" PRId32,
                  args.min.value,
                  args.max.value);
      return false;
   }

   if (args.value > args.max.value || args.value < args.min.value) {
      CLIENT_ERR (
         "Value must be greater than or equal to the minimum value "
         "and less than or equal to the maximum value, got min: %" PRId32
         ", max: %" PRId32 ", value: %" PRId32,
         args.min.value,
         args.max.value,
         args.value);
      return false;
   }

   // Convert to unbiased uint32. Then subtract the min value.
   uint32_t v_u32 = encodeInt32 (args.value);
   uint32_t min_u32 = encodeInt32 (args.min.value);
   uint32_t max_u32 = encodeInt32 (args.max.value);

   v_u32 -= min_u32;
   max_u32 -= min_u32;

   *out = (mc_OSTType_Int32){v_u32, 0, max_u32};
   return true;
}

/**
 * Encode a signed 64-bit integer as an unsigned 64-bit integer by adding 2^63.
 * Some documentation references this as making the value "unbiased".
 */
static uint64_t
encodeInt64 (int64_t v)
{
   // Shift the int64_t range [-2^63, 2^63 - 1] to the uint64_t range [0, 2^64].
   // new_zero is the mapped 0 value.
   uint64_t new_zero = (UINT64_C (1) << 63);

   if (v < 0) {
      // Signed integers have a value that there is no positive equivalent and
      // must be handled specially
      if (v == INT64_MIN) {
         return 0;
      }

      int64_t v_pos = v * -1;
      uint64_t v_u64 = (uint64_t) v_pos;
      return new_zero - v_u64;
   }

   uint64_t v_u64 = (uint64_t) v;
   return new_zero + v_u64;
}

bool
mc_getTypeInfo64 (mc_getTypeInfo64_args_t args,
                  mc_OSTType_Int64 *out,
                  mongocrypt_status_t *status)
{
   if (args.min.set != args.max.set) {
      CLIENT_ERR ("Must specify both a lower and upper bound or no bounds.");
      return false;
   }

   if (!args.min.set) {
      uint64_t v_u64 = encodeInt64 (args.value);
      *out = (mc_OSTType_Int64){v_u64, 0, UINT64_MAX};
      return true;
   }

   if (args.min.value >= args.max.value) {
      CLIENT_ERR ("The minimum value must be less than the maximum value, got "
                  "min: %" PRId64 ", max: %" PRId64,
                  args.min.value,
                  args.max.value);
      return false;
   }

   if (args.value > args.max.value || args.value < args.min.value) {
      CLIENT_ERR ("Value must be greater than or equal to the minimum value "
                  "and less than or equal to the maximum value, got "
                  "min: %" PRId64 ", max: %" PRId64 ", value: %" PRId64,
                  args.min.value,
                  args.max.value,
                  args.value);
      return false;
   }

   // Convert to unbiased uint64. Then subtract the min value.
   uint64_t v_u64 = encodeInt64 (args.value);
   uint64_t min_u64 = encodeInt64 (args.min.value);
   uint64_t max_u64 = encodeInt64 (args.max.value);

   v_u64 -= min_u64;
   max_u64 -= min_u64;

   *out = (mc_OSTType_Int64){v_u64, 0, max_u64};
   return true;
}

#define exp10Double(x) pow (10, x)

bool
mc_getTypeInfoDouble (mc_getTypeInfoDouble_args_t args,
                      mc_OSTType_Double *out,
                      mongocrypt_status_t *status)
{
   if (args.min.set != args.max.set || args.min.set != args.precision.set) {
      CLIENT_ERR (
         "min, max, and precision must all be set or must all be unset");
      return false;
   }

   if (mc_isinf (args.value) || mc_isnan (args.value)) {
      CLIENT_ERR ("Infinity and NaN double values are not supported.");
      return false;
   }

   if (args.min.set) {
      if (args.min.value >= args.max.value) {
         CLIENT_ERR (
            "The minimum value must be less than the maximum value, got "
            "min: %g, max: %g",
            args.min.value,
            args.max.value);
         return false;
      }

      if (args.value > args.max.value || args.value < args.min.value) {
         CLIENT_ERR ("Value must be greater than or equal to the minimum value "
                     "and less than or equal to the maximum value, got "
                     "min: %g, max: %g, value: %g",
                     args.min.value,
                     args.max.value,
                     args.value);
         return false;
      }
   }

   const bool is_neg = args.value < 0.0;

   // Map negative 0 to zero so sign bit is 0.
   if (args.value == 0.0) {
      args.value = 0.0;
   }

   // When we use precision mode, we try to represent as a double value that
   // fits in [-2^63, 2^63] (i.e. is a valid int64)
   //
   // This check determines if we can represent the precision truncated value as
   // a 64-bit integer I.e. Is ((ub - lb) * 10^precision) < 64 bits.
   //
   bool use_precision_mode = false;
   uint32_t bits_range;
   if (args.precision.set) {
      // Subnormal representations can support up to 5x10^-324 as a number
      if (args.precision.value < 0 || args.precision.value > 324) {
         CLIENT_ERR (
            "Precision must be between 0 and 324 inclusive, got: %" PRIu32,
            args.precision.value);
         return false;
      }

      double range = args.max.value - args.min.value;

      // We can overflow if max = max double and min = min double so make sure
      // we have finite number after we do subtraction
      // Ignore conversion warnings to fix error with glibc.
      if (mc_isfinite (range)) {
         // This creates a range which is wider then we permit by our min/max
         // bounds check with the +1 but it is as the algorithm is written in
         // WRITING-11907.
         double rangeAndPrecision =
            (range + 1) * exp10Double (args.precision.value);

         if (mc_isfinite (rangeAndPrecision)) {
            double bits_range_double = log2 (rangeAndPrecision);
            bits_range = (uint32_t) ceil (bits_range_double);

            if (bits_range < 64) {
               use_precision_mode = true;
            }
         }
      }
   }

   if (use_precision_mode) {
      // Take a number of xxxx.ppppp and truncate it xxxx.ppp if precision = 3.
      // We do not change the digits before the decimal place.
      double v_prime = trunc (args.value * exp10Double (args.precision.value)) /
                       exp10Double (args.precision.value);
      int64_t v_prime2 = (int64_t) ((v_prime - args.min.value) *
                                    exp10Double (args.precision.value));

      BSON_ASSERT (v_prime2 < INT64_MAX && v_prime2 >= 0);

      uint64_t ret = (uint64_t) v_prime2;

      // Adjust maximum value to be the max bit range. This will be used by
      // getEdges/minCover to trim bits.
      uint64_t max_value = (UINT64_C (1) << bits_range) - 1;
      BSON_ASSERT (ret <= max_value);

      *out = (mc_OSTType_Double){ret, 0, max_value};
      return true;
   }

   // Translate double to uint64 by modifying the bit representation and copying
   // into a uint64. Double is assumed to be a IEEE 754 Binary 64.
   // It is bit-encoded as sign, exponent, and fraction:
   // s eeeeeeee ffffffffffffffffffffffffffffffffffffffffffffffffffff

   // When we translate the double into "bits", the sign bit means that the
   // negative numbers get mapped into the higher 63 bits of a 64-bit integer.
   // We want them to  map into the lower 64-bits so we invert the sign bit.
   args.value *= -1.0;

   // On Endianness, we support two sets of architectures
   // 1. Little Endian (ppc64le, x64, aarch64) - in these architectures, int64
   // and double are both 64-bits and both arranged in little endian byte order.
   // 2. Big Endian (s390x) - in these architectures, int64 and double are both
   // 64-bits and both arranged in big endian byte order.
   //
   // Therefore, since the order of bytes on each platform is consistent with
   // itself, the conversion below converts a double into correct 64-bit integer
   // that produces the same behavior across plaforms.
   uint64_t uv;
   memcpy (&uv, &args.value, sizeof (uint64_t));

   if (is_neg) {
      uint64_t new_zero = UINT64_C (1) << 63;
      BSON_ASSERT (uv <= new_zero);
      uv = new_zero - uv;
   }

   *out = (mc_OSTType_Double){.min = 0, .max = UINT64_MAX, .value = uv};

   return true;
}

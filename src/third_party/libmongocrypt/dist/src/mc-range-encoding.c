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
static uint32_t encodeInt32(int32_t v) {
    // Shift the int32_t range [-2^31, 2^31 - 1] to the uint32_t range [0, 2^32].
    // new_zero is the mapped 0 value.
    uint32_t new_zero = (UINT32_C(1) << 31);

    if (v < 0) {
        // Signed integers have a value that there is no positive equivalent and
        // must be handled specially
        if (v == INT32_MIN) {
            return 0;
        }

        int32_t v_pos = v * -1;
        uint32_t v_u32 = (uint32_t)v_pos;
        return new_zero - v_u32;
    }

    uint32_t v_u32 = (uint32_t)v;
    return new_zero + v_u32;
}

bool mc_getTypeInfo32(mc_getTypeInfo32_args_t args, mc_OSTType_Int32 *out, mongocrypt_status_t *status) {
    if (args.min.set != args.max.set) {
        CLIENT_ERR("Must specify both a lower and upper bound or no bounds.");
        return false;
    }

    if (!args.min.set) {
        uint32_t v_u32 = encodeInt32(args.value);
        *out = (mc_OSTType_Int32){v_u32, 0, UINT32_MAX};
        return true;
    }

    if (args.min.value >= args.max.value) {
        CLIENT_ERR("The minimum value must be less than the maximum value, got "
                   "min: %" PRId32 ", max: %" PRId32,
                   args.min.value,
                   args.max.value);
        return false;
    }

    if (args.value > args.max.value || args.value < args.min.value) {
        CLIENT_ERR("Value must be greater than or equal to the minimum value "
                   "and less than or equal to the maximum value, got min: %" PRId32 ", max: %" PRId32
                   ", value: %" PRId32,
                   args.min.value,
                   args.max.value,
                   args.value);
        return false;
    }

    // Convert to unbiased uint32. Then subtract the min value.
    uint32_t v_u32 = encodeInt32(args.value);
    uint32_t min_u32 = encodeInt32(args.min.value);
    uint32_t max_u32 = encodeInt32(args.max.value);

    v_u32 -= min_u32;
    max_u32 -= min_u32;

    *out = (mc_OSTType_Int32){v_u32, 0, max_u32};
    return true;
}

/**
 * Encode a signed 64-bit integer as an unsigned 64-bit integer by adding 2^63.
 * Some documentation references this as making the value "unbiased".
 */
static uint64_t encodeInt64(int64_t v) {
    // Shift the int64_t range [-2^63, 2^63 - 1] to the uint64_t range [0, 2^64].
    // new_zero is the mapped 0 value.
    uint64_t new_zero = (UINT64_C(1) << 63);

    if (v < 0) {
        // Signed integers have a value that there is no positive equivalent and
        // must be handled specially
        if (v == INT64_MIN) {
            return 0;
        }

        int64_t v_pos = v * -1;
        uint64_t v_u64 = (uint64_t)v_pos;
        return new_zero - v_u64;
    }

    uint64_t v_u64 = (uint64_t)v;
    return new_zero + v_u64;
}

bool mc_getTypeInfo64(mc_getTypeInfo64_args_t args, mc_OSTType_Int64 *out, mongocrypt_status_t *status) {
    if (args.min.set != args.max.set) {
        CLIENT_ERR("Must specify both a lower and upper bound or no bounds.");
        return false;
    }

    if (!args.min.set) {
        uint64_t v_u64 = encodeInt64(args.value);
        *out = (mc_OSTType_Int64){v_u64, 0, UINT64_MAX};
        return true;
    }

    if (args.min.value >= args.max.value) {
        CLIENT_ERR("The minimum value must be less than the maximum value, got "
                   "min: %" PRId64 ", max: %" PRId64,
                   args.min.value,
                   args.max.value);
        return false;
    }

    if (args.value > args.max.value || args.value < args.min.value) {
        CLIENT_ERR("Value must be greater than or equal to the minimum value "
                   "and less than or equal to the maximum value, got "
                   "min: %" PRId64 ", max: %" PRId64 ", value: %" PRId64,
                   args.min.value,
                   args.max.value,
                   args.value);
        return false;
    }

    // Convert to unbiased uint64. Then subtract the min value.
    uint64_t v_u64 = encodeInt64(args.value);
    uint64_t min_u64 = encodeInt64(args.min.value);
    uint64_t max_u64 = encodeInt64(args.max.value);

    v_u64 -= min_u64;
    max_u64 -= min_u64;

    *out = (mc_OSTType_Int64){v_u64, 0, max_u64};
    return true;
}

#define exp10Double(x) pow(10, x)

bool mc_getTypeInfoDouble(mc_getTypeInfoDouble_args_t args, mc_OSTType_Double *out, mongocrypt_status_t *status) {
    if (args.min.set != args.max.set || args.min.set != args.precision.set) {
        CLIENT_ERR("min, max, and precision must all be set or must all be unset");
        return false;
    }

    if (mc_isinf(args.value) || mc_isnan(args.value)) {
        CLIENT_ERR("Infinity and NaN double values are not supported.");
        return false;
    }

    if (args.min.set) {
        if (args.min.value >= args.max.value) {
            CLIENT_ERR("The minimum value must be less than the maximum value, got "
                       "min: %g, max: %g",
                       args.min.value,
                       args.max.value);
            return false;
        }

        if (args.value > args.max.value || args.value < args.min.value) {
            CLIENT_ERR("Value must be greater than or equal to the minimum value "
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
        if (args.precision.value > 324) {
            CLIENT_ERR("Precision must be between 0 and 324 inclusive, got: %" PRIu32, args.precision.value);
            return false;
        }

        double range = args.max.value - args.min.value;

        // We can overflow if max = max double and min = min double so make sure
        // we have finite number after we do subtraction
        // Ignore conversion warnings to fix error with glibc.
        if (mc_isfinite(range)) {
            // This creates a range which is wider then we permit by our min/max
            // bounds check with the +1 but it is as the algorithm is written in
            // WRITING-11907.
            double rangeAndPrecision = (range + 1) * exp10Double(args.precision.value);

            if (mc_isfinite(rangeAndPrecision)) {
                double bits_range_double = log2(rangeAndPrecision);
                bits_range = (uint32_t)ceil(bits_range_double);

                if (bits_range < 64) {
                    use_precision_mode = true;
                }
            }
        }
    }

    if (use_precision_mode) {
        // Take a number of xxxx.ppppp and truncate it xxxx.ppp if precision = 3.
        // We do not change the digits before the decimal place.
        double v_prime = trunc(args.value * exp10Double(args.precision.value)) / exp10Double(args.precision.value);
        int64_t v_prime2 = (int64_t)((v_prime - args.min.value) * exp10Double(args.precision.value));

        BSON_ASSERT(v_prime2 < INT64_MAX && v_prime2 >= 0);

        uint64_t ret = (uint64_t)v_prime2;

        // Adjust maximum value to be the max bit range. This will be used by
        // getEdges/minCover to trim bits.
        uint64_t max_value = (UINT64_C(1) << bits_range) - 1;
        BSON_ASSERT(ret <= max_value);

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
    memcpy(&uv, &args.value, sizeof(uint64_t));

    if (is_neg) {
        uint64_t new_zero = UINT64_C(1) << 63;
        BSON_ASSERT(uv <= new_zero);
        uv = new_zero - uv;
    }

    *out = (mc_OSTType_Double){.min = 0, .max = UINT64_MAX, .value = uv};

    return true;
}

#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
/**
 * @brief There is no shipped algorithm for creating a full 128-bit integer from
 * a Decimal128, but it's easy enough to write one of our own.
 *
 * @param dec
 * @return mlib_int128
 */
static mlib_int128 dec128_to_int128(mc_dec128 dec) {
    // Only normal numbers
    BSON_ASSERT(mc_dec128_is_finite(dec));
    BSON_ASSERT(!mc_dec128_is_nan(dec));
    // We don't support negative numbers
    BSON_ASSERT(!mc_dec128_is_negative(dec));
    // There is no fractional part:
    BSON_ASSERT(mc_dec128_is_zero(mc_dec128_modf(dec).frac));

    mlib_int128 ret = mc_dec128_coeff(dec);

    // Scale the resulting number by a power of ten matching the exponent of the
    // Decimal128:
    int32_t exp = ((int32_t)mc_dec128_get_biased_exp(dec)) - MC_DEC128_EXPONENT_BIAS;
    // We will scale up/down based on whether it is negative:
    mlib_int128 e1 = mlib_int128_pow10((uint8_t)abs(exp));
    if (exp < 0) {
        ret = mlib_int128_div(ret, e1);
    } else {
        ret = mlib_int128_mul(ret, e1);
    }

    return ret;
}

bool mc_getTypeInfoDecimal128(mc_getTypeInfoDecimal128_args_t args,
                              mc_OSTType_Decimal128 *out,
                              mongocrypt_status_t *status) {
    /// Basic param checks
    if (args.min.set != args.max.set || args.min.set != args.precision.set) {
        CLIENT_ERR("min, max, and precision must all be set or must all be unset");
        return false;
    }

    // We only accept normal numbers
    if (mc_dec128_is_inf(args.value) || mc_dec128_is_nan(args.value)) {
        CLIENT_ERR("Infinity and Nan Decimal128 values are not supported.");
        return false;
    }

    // Check boundary if a range is set
    if (args.min.set) {
        // [min,max] must be valid
        if (mc_dec128_greater_equal(args.min.value, args.max.value)) {
            CLIENT_ERR("The minimum value must be less than the maximum value, got "
                       "min: %s, max: %s",
                       mc_dec128_to_string(args.min.value).str,
                       mc_dec128_to_string(args.max.value).str);
            return false;
        }

        // Value must be within [min,max)
        if (mc_dec128_greater(args.value, args.max.value) || mc_dec128_less(args.value, args.min.value)) {
            CLIENT_ERR("Value must be greater than or equal to the minimum value "
                       "and less than or equal to the maximum value, got "
                       "min: %s, max: %s, value: %s",
                       mc_dec128_to_string(args.min.value).str,
                       mc_dec128_to_string(args.max.value).str,
                       mc_dec128_to_string(args.value).str);
            return false;
        }
    }

    // Should we use precision mode?
    //
    // When we use precision mode, we try to represent as a decimal128 value that
    // fits in [-2^127, 2^127] (i.e. is a valid int128)
    //
    // This check determines if we can represent any precision-truncated value as
    // a 128-bit integer I.e. Is ((ub - lb) * 10^precision) < 128 bits.
    //
    // It is important that we determine whether a range and its precision would
    // fit, regardless of the value to be encoded, because the encoding for
    // precision-truncated-decimal128 is incompatible with the encoding of the
    // full range.
    bool use_precision_mode = false;
    // The number of bits required to hold the result (used for precision mode)
    uint8_t bits_range = 0;
    if (args.precision.set) {
        // Subnormal representations can support up to 5x10^-6182 as a number
        if (args.precision.value > 6182) {
            CLIENT_ERR("Precision must be between 0 and 6182 inclusive, got: %" PRIu32, args.precision.value);
            return false;
        }

        // max - min
        mc_dec128 bounds_n1 = mc_dec128_sub(args.max.value, args.min.value);
        // The size of [min, max]: (max - min) + 1
        mc_dec128 bounds = mc_dec128_add(bounds_n1, MC_DEC128_ONE);

        // We can overflow if max = max_dec128 and min = min_dec128 so make sure
        // we have finite number after we do subtraction
        if (mc_dec128_is_finite(bounds)) {
            // This creates a range which is wider then we permit by our min/max
            // bounds check with the +1 but it is as the algorithm is written in
            // WRITING-11907.
            mc_dec128 precision_scaled_bounds = mc_dec128_scale(bounds, args.precision.value);
            /// The number of bits required to hold the result for the given
            /// precision (as decimal)
            mc_dec128 bits_range_dec = mc_dec128_log2(precision_scaled_bounds);

            if (mc_dec128_is_finite(bits_range_dec) && mc_dec128_less(bits_range_dec, MC_DEC128(128))) {
                // We need fewer than 128 bits to hold the result. But round up,
                // just to be sure:
                int64_t r =
                    mc_dec128_to_int64(mc_dec128_round_integral_ex(bits_range_dec, MC_DEC128_ROUND_UPWARD, NULL));
                BSON_ASSERT(r >= 0);
                BSON_ASSERT(r <= UINT8_MAX);
                // We've computed the proper 'bits_range'
                bits_range = (uint8_t)r;

                if (bits_range < 128) {
                    use_precision_mode = true;
                }
            }
        }
    }

    // Constant zero
    const mlib_int128 i128_zero = MLIB_INT128(0);
    // Constant 1
    const mlib_int128 i128_one = MLIB_INT128(1);
    // Constant 10
    const mlib_int128 i128_ten = MLIB_INT128(10);
    // Constant: 2^127
    const mlib_int128 i128_2pow127 = mlib_int128_lshift(i128_one, 127);
    // ↑ Coincidentally has the same bit pattern as INT128_SMIN, but we're
    // treating it as an unsigned number here, so don't get confused!

    if (use_precision_mode) {
        BSON_ASSERT(args.precision.set);
        // Example value: 31.4159
        // Example Precision = 2

        // Shift the number up
        // Returns: 3141.9
        mc_dec128 valScaled = mc_dec128_scale(args.value, args.precision.value);

        // Round the number down
        // Returns 3141.0
        mc_dec128 valTrunc = mc_dec128_round_integral_ex(valScaled, MC_DEC128_ROUND_TOWARD_ZERO, NULL);

        // Shift the number down
        // Returns: 31.41
        mc_dec128 v_prime = mc_dec128_scale(valTrunc, -(int32_t)args.precision.value);

        // Adjust the number by the lower bound
        // Make it an integer by scaling the number
        //
        // Returns 3141.0
        mc_dec128 v_prime2 = mc_dec128_scale(mc_dec128_sub(v_prime, args.min.value), args.precision.value);
        // Round the number down again. min may have a fractional value with more
        // decimal places than the precision (e.g. .001). Subtracting min may have
        // resulted in v_prime2 with a non-zero fraction. v_prime2 is expected to
        // have no fractional value when converting to int128.
        v_prime2 = mc_dec128_round_integral_ex(v_prime2, MC_DEC128_ROUND_TOWARD_ZERO, NULL);

        BSON_ASSERT(mc_dec128_less(mc_dec128_log2(v_prime2), MC_DEC128(128)));

        // Resulting OST maximum
        mlib_int128 ost_max = mlib_int128_sub(mlib_int128_pow2(bits_range), i128_one);

        // Now we need to get the Decimal128 out as a 128-bit integer
        // But Decimal128 does not support conversion to Int128.
        //
        // If we think the Decimal128 fits in the range, based on the maximum
        // value, we try to convert to int64 directly.
        if (bits_range < 64) {
            // Try conversion to int64, it may fail but since it is easy we try
            // this first.
            mc_dec128_flagset flags = {0};
            int64_t as64 = mc_dec128_to_int64_ex(v_prime2, &flags);
            if (flags.bits == 0) {
                // No error. It fits
                *out = (mc_OSTType_Decimal128){
                    .value = MLIB_INT128_CAST(as64),
                    .min = i128_zero,
                    .max = ost_max,
                };
                return true;
            } else {
                // Conversion failure to 64-bit. Possible overflow, imprecision,
                // etc. Fallback to slower dec128_to_int128
            }
        }

        mlib_int128 u_ret = dec128_to_int128(v_prime2);

        *out = (mc_OSTType_Decimal128){
            .value = u_ret,
            .min = i128_zero,
            .max = ost_max,
        };

        return true;
    }

    // The coefficient of the number, without exponent/sign
    const mlib_int128 coeff = mc_dec128_coeff(args.value);

    if (mlib_int128_eq(coeff, i128_zero)) {
        // If the coefficient is zero, the result is encoded as the midpoint
        // between zero and 2^128-1
        *out = (mc_OSTType_Decimal128){
            .value = i128_2pow127,
            .min = i128_zero,
            .max = MLIB_INT128_UMAX,
        };
        return true;
    }

    // Coefficient is an unsigned value. We'll later scale our answer based on
    // the sign of the actual Decimal128
    const bool isNegative = mc_dec128_is_negative(args.value);

    // cMax = 10^34 - 1 (The largest integer representable in Decimal128)
    const mlib_int128 cMax = mlib_int128_sub(mlib_int128_pow10(34), MLIB_INT128_CAST(1));
    const mlib_int128 cMax_div_ten = mlib_int128_div(cMax, i128_ten);

    // The biased exponent from the decimal number. The paper refers to the
    // expression (e - e_min), which is the value of the biased exponent.
    const uint32_t exp_biased = mc_dec128_get_biased_exp(args.value);

    // ρ (rho) is the greatest integer such that: coeff×10^ρ <= cMax
    unsigned rho = 0;
    // Keep track of the subexpression coeff×10^ρ rather than recalculating it
    // time.
    // Initially: (ρ = 0) -> (10^ρ = 1) -> (coeff×10^ρ = coeff×1 = coeff):
    mlib_int128 coeff_scaled = coeff;
    // Calculate ρ: This could be done using a log10 with a division, but that
    // is far more work than just a few multiplications.
    // While: coeff×ten^ρ < cMax/10:
    while (mlib_int128_ucmp(coeff_scaled, cMax_div_ten) < 0) {
        // Increase rho until we pass cMax/10
        rho++;
        // Scale our computed subexpression rather than fully recomputing it
        coeff_scaled = mlib_int128_mul(coeff_scaled, i128_ten);
    }

    // No multiplication by 10 should ever send us from N < cMax/10 to N > cMax
    BSON_ASSERT(mlib_int128_ucmp(coeff_scaled, cMax) <= 0);

    mlib_int128 result;
    if (rho <= exp_biased) {
        // ρ is less-than/equal to the exponent with bias.

        // Diff between the biased exponent and ρ.
        // Value in paper is spelled "e - e_min - ρ"
        const uint32_t exp_diff = exp_biased - (uint32_t)rho;
        // cMax * (exp_diff)
        const mlib_int128 cmax_scaled = mlib_int128_mul(cMax, MLIB_INT128_CAST(exp_diff));
        // coeff * 10^rho * cMax * (exp_biased - rho)
        result = mlib_int128_add(coeff_scaled, cmax_scaled);
    } else {
        const mlib_int128 biased_scale = mlib_int128_pow10((uint8_t)exp_biased);
        result = mlib_int128_mul(biased_scale, coeff);
    }

    // Always add 2^127:
    result = mlib_int128_add(result, i128_2pow127);

    if (isNegative) {
        // We calculated the value of the positive coefficient, but the decimal is
        // negative. That's okay: Just flip the sign of the encoded result:
        result = mlib_int128_negate(result);
    }

    *out = (mc_OSTType_Decimal128){
        .value = result,
        .min = i128_zero,
        .max = MLIB_INT128_UMAX,
    };

    return true;
}

#endif // defined MONGOCRYPT_HAVE_DECIMAL128_SUPPORT

/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/crypto/fle_numeric.h"

#include <algorithm>

#include <boost/container/small_vector.hpp>
#include <boost/multiprecision/cpp_int/import_export.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/platform/bits.h"
#include "mongo/platform/overflow_arithmetic.h"

#include <cmath>
#include <iterator>
#include <limits>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
constexpr boost::multiprecision::uint128_t k1(1);
constexpr boost::multiprecision::int128_t k10(10);
constexpr boost::multiprecision::uint128_t k10ui(10);

constexpr double SCALED_DOUBLE_BOUNDS(static_cast<double>(9223372036854775807));  // 2^63 - 1
constexpr double INT_64_MAX_DOUBLE = static_cast<double>(std::numeric_limits<uint64_t>::max());
const Decimal128 scaled_decimal_bounds("170141183460469231731687303715884105727");  // 2^127 - 1

template <typename T>
uint32_t ceil_log2(T t) {
    invariant(t != 0);
    // We count the leading zeros in the value. If there
    // is more than one bit set, then we can just take
    // 64 - value. If there is only one bit set, then
    // we take 64 - value - 1.
    auto clz = getFirstBitSet(t);
    if ((t & (t - 1)) == 0) {
        return clz - 1;
    }
    return clz;
}

}  // namespace

boost::multiprecision::int128_t exp10(int x) {
    return pow(k10, x);
}

boost::multiprecision::uint128_t exp10ui128(int x) {
    return pow(k10ui, x);
}

double exp10Double(int x) {
    return pow(10, x);
}

/**
 * Encode a signed 32-bit integer as an unsigned 32-bit integer
 */
uint32_t encodeInt32(int32_t v) {
    if (v < 0) {

        // Signed integers have a value that there is no positive equivalent and must be handled
        // specially
        if (v == std::numeric_limits<int32_t>::min()) {
            return 0;
        }

        return (v & ~(1U << 31));
    }

    return v + (1U << 31);
}


OSTType_Int32 getTypeInfo32(int32_t value,
                            boost::optional<int32_t> min,
                            boost::optional<int32_t> max) {
    uassert(6775001,
            "Must specify both a lower and upper bound or no bounds.",
            min.has_value() == max.has_value());

    if (!min.has_value()) {
        uint32_t uv = encodeInt32(value);
        return {uv, 0, std::numeric_limits<uint32_t>::max()};
    } else {
        uassert(6775002,
                "The minimum value must be less than the maximum value",
                min.value() < max.value());
        uassert(6775003,
                "Value must be greater than or equal to the minimum value and less than or equal "
                "to the maximum value",
                value >= min.value() && value <= max.value());

        // Handle min int32 as a special case
        if (min.value() == std::numeric_limits<int32_t>::min()) {
            uint32_t uv = encodeInt32(value);
            return {uv, 0, encodeInt32(max.value())};
        }

        // For negative numbers, first convert them to unbiased uint32 and then subtract the min
        // value.
        if (min.value() < 0) {
            uint32_t uv = encodeInt32(value);
            uint32_t min_v = encodeInt32(min.value());
            uint32_t max_v = encodeInt32(max.value());

            uv -= min_v;
            max_v -= min_v;

            return {uv, 0, max_v};
        }

        return {static_cast<uint32_t>(value - min.value()),
                0,
                static_cast<uint32_t>(max.value() - min.value())};
    }
}

/**
 * Encode a signed 64-bit integer as an unsigned 64-bit integer
 */
uint64_t encodeInt64(int64_t v) {
    if (v < 0) {

        // Signed integers have a value that there is no positive equivalent and must be handled
        // specially
        if (v == std::numeric_limits<int64_t>::min()) {
            return 0;
        }

        return (v & ~(1ULL << 63));
    }

    return v + (1ULL << 63);
}

OSTType_Int64 getTypeInfo64(int64_t value,
                            boost::optional<int64_t> min,
                            boost::optional<int64_t> max) {
    uassert(6775004,
            "Must specify both a lower and upper bound or no bounds.",
            min.has_value() == max.has_value());

    if (!min.has_value()) {
        uint64_t uv = encodeInt64(value);
        return {uv, 0, std::numeric_limits<uint64_t>::max()};
    } else {
        uassert(6775005,
                "The minimum value must be less than the maximum value",
                min.value() < max.value());
        uassert(6775006,
                "Value must be greater than or equal to the minimum value and less than or equal "
                "to the maximum value",
                value >= min.value() && value <= max.value());

        // Handle min int64 as a special case
        if (min.value() == std::numeric_limits<int64_t>::min()) {
            uint64_t uv = encodeInt64(value);
            return {uv, 0, encodeInt64(max.value())};
        }

        // For negative numbers, first convert them to unbiased uin64 and then subtract the min
        // value.
        if (min.value() < 0) {
            uint64_t uv = encodeInt64(value);
            uint64_t min_v = encodeInt64(min.value());
            uint64_t max_v = encodeInt64(max.value());

            uv -= min_v;
            max_v -= min_v;

            return {uv, 0, max_v};
        }

        return {static_cast<uint64_t>(value - min.value()),
                0,
                static_cast<uint64_t>(max.value() - min.value())};
    }
}

OSTType_Double getTypeInfoDouble(double value,
                                 boost::optional<double> min,
                                 boost::optional<double> max,
                                 boost::optional<uint32_t> precision) {
    uassert(6775007,
            "Must specify both a lower bound and upper bound or no bounds.",
            min.has_value() == max.has_value());

    uassert(6775008,
            "Infinity and Nan double values are not supported.",
            !std::isinf(value) && !std::isnan(value));

    if (min.has_value()) {
        uassert(6775009,
                "The minimum value must be less than the maximum value",
                min.value() < max.value());
        uassert(6775010,
                "Value must be greater than or equal to the minimum value and less than or equal "
                "to the maximum value",
                value >= min.value() && value <= max.value());
    }

    // Map negative 0 to zero so sign bit is 0.
    if (std::signbit(value) && value == 0) {
        value = 0;
    }

    // When we use precision mode, we try to represent as a double value that fits in [-2^63, 2^63]
    // (i.e. is a valid int64)
    //
    // This check determines if we can represent the precision truncated value as a 64-bit integer
    // I.e. Is ((ub - lb) * 10^precision) < 64 bits.
    //
    // It is important we determine whether a range and its precision fit without looking that value
    // because the encoding for precision truncated doubles is incompatible with the encoding for
    // doubles without precision.
    //
    bool use_precision_mode = false;
    uint32_t bits_range;
    if (precision.has_value()) {
        uassert(6966803,
                "Must specify both a lower bound, upper bound and precision",
                min.has_value() == max.has_value() && max.has_value() == precision.has_value());

        use_precision_mode =
            canUsePrecisionMode(min.get(), max.get(), precision.get(), &bits_range);
    }

    if (use_precision_mode) {

        // Take a number of xxxx.ppppp and truncate it xxxx.ppp if precision = 3. We do not change
        // the digits before the decimal place.
        int64_t scaled_value = (int64_t)trunc(value * exp10Double(precision.get()));
        int64_t scaled_min = (int64_t)(min.get() * exp10Double(precision.get()));
        int64_t v_prime2 = scaled_value - scaled_min;

        invariant(v_prime2 < std::numeric_limits<int64_t>::max() && v_prime2 >= 0);

        uint64_t ret = static_cast<uint64_t>(v_prime2);

        // Adjust maximum value to be the max bit range. This will be used by getEdges/minCover to
        // trim bits.
        uint64_t max_value = (1ULL << bits_range) - 1;
        invariant(ret <= max_value);

        return {ret, 0, max_value};
    }

    // When we translate the double into "bits", the sign bit means that the negative numbers
    // get mapped into the higher 63 bits of a 64-bit integer. We want them to map into the lower
    // 64-bits so we invert the sign bit.
    //
    // On Endianness, we support two sets of architectures
    // 1. Little Endian (ppc64le, x64, aarch64) - in these architectures, int64 and double are both
    // 64-bits and both arranged in little endian byte order.
    // 2. Big Endian (s390x) - in these architectures, int64 and double are both
    // 64-bits and both arranged in big endian byte order.
    //
    // Therefore, since the order of bytes on each platform is consistent with itself, the
    // conversion below converts a double into correct 64-bit integer that produces the same
    // behavior across plaforms.
    bool is_neg = value < 0;

    value *= -1;
    char* buf = reinterpret_cast<char*>(&value);
    uint64_t uv = DataView(buf).read<uint64_t>();

    if (is_neg) {
        dassert(uv < std::numeric_limits<uint64_t>::max());
        uv = (1ULL << 63) - uv;
    }

    return {uv, 0, std::numeric_limits<uint64_t>::max()};
}

boost::multiprecision::uint128_t toUInt128FromDecimal128(Decimal128 dec) {
    // This algorithm only works because it assumes we are dealing with Decimal128 numbers that are
    // valid uint128 numbers. This means the Decimal128 has to be an integer or else the result is
    // undefined.
    uassert(8574710, "Unable to convert non-finite Decimal128 to UInt128", dec.isFinite());
    uassert(8574711, "Unable to convert negative Decimal128 to UInt128", !dec.isNegative());

    // If after rounding, the number has changed, we have a fraction, not an integer.
    uassert(8574712, "Unable to convert non-integral Decimal128 to UInt128", dec.round() == dec);

    boost::multiprecision::uint128_t ret(dec.getCoefficientHigh());

    ret <<= 64;
    ret |= dec.getCoefficientLow();

    auto exponent = static_cast<int32_t>(dec.getBiasedExponent()) - Decimal128::kExponentBias;

    auto e1 = exp10ui128(labs(exponent));
    if (exponent < 0) {
        ret /= e1;
    } else {
        ret *= e1;
    }

    // Round-trip our new Int128 back to Decimal128 and make sure it is equal to the original
    // Decimal128 or else.
    Decimal128 roundTrip(ret.str());
    uassert(8574713,
            "Conversion from Decimal128 to UInt128 did not survive round trip",
            roundTrip.isEqual(dec));

    return ret;
}

boost::multiprecision::int128_t toInt128FromDecimal128(Decimal128 dec) {
    uassert(9178814,
            "Unable to convert Decimal128 to Int128, out of bounds",
            dec.toAbs().isLess(scaled_decimal_bounds));
    bool negative = false;
    if (dec.isLess(Decimal128(0))) {
        negative = true;
        dec = Decimal128(0).subtract(dec);
    }

    auto uint_dec = toUInt128FromDecimal128(dec);
    auto int_dec = static_cast<boost::multiprecision::int128_t>(uint_dec);
    if (negative) {
        return -int_dec;
    }
    return int_dec;
}

// For full algorithm see SERVER-68542
OSTType_Decimal128 getTypeInfoDecimal128(Decimal128 value,
                                         boost::optional<Decimal128> min,
                                         boost::optional<Decimal128> max,
                                         boost::optional<uint32_t> precision) {
    uassert(6854201,
            "Must specify both a lower bound and upper bound or no bounds.",
            min.has_value() == max.has_value());

    uassert(6854202,
            "Infinity and Nan Decimal128 values are not supported.",
            !value.isInfinite() && !value.isNaN());

    if (min.has_value()) {
        uassert(6854203,
                "The minimum value must be less than the maximum value",
                min.value() < max.value());

        uassert(6854204,
                "Value must be greater than or equal to the minimum value and less than or equal "
                "to the maximum value",
                value >= min.value() && value <= max.value());
    }

    // When we use precision mode, we try to represent as a decimal128 value that fits in [-2^127,
    // 2^127] (i.e. is a valid int128)
    //
    // This check determines if we can represent the precision truncated value as a 128-bit integer
    // I.e. Is ((ub - lb) * 10^precision) < 128 bits.
    //
    // It is important we determine whether a range and its precision fit without looking that value
    // because the encoding for precision truncated decimal128 is incompatible with normal
    // decimal128 values.
    bool use_precision_mode = false;
    uint32_t bits_range = 0;
    if (precision.has_value()) {
        uassert(6966804,
                "Must specify both a lower bound, upper bound and precision",
                min.has_value() == max.has_value() && max.has_value() == precision.has_value());

        use_precision_mode =
            canUsePrecisionMode(min.get(), max.get(), precision.get(), &bits_range);
    }

    if (use_precision_mode) {
        // Example value: 31.4159
        // Example Precision = 2

        // Shift the number up
        // Returns: 3141.9
        Decimal128 valueScaled = value.scale(precision.get());

        // Round the number down
        // Returns 3141.0
        Decimal128 valueTruncated = valueScaled.round(Decimal128::kRoundTowardZero);

        // Shift the number down
        // Returns: 31.41
        Decimal128 v_prime = valueTruncated.scale(-static_cast<int32_t>(precision.get()));

        // Adjust the number by the lower bound
        // Make it an integer by scaling the number
        //
        // Returns 3141.0
        Decimal128 v_prime2 = v_prime.subtract(min.get()).scale(precision.get());
        // Round the number down again. min may have a fractional value with more decimal places
        // than the precision (e.g. .001). Subtracting min may have resulted in v_prime2 with
        // a non-zero fraction. v_prime2 is expected to have no fractional value when
        // converting to int128.
        v_prime2 = v_prime2.round(Decimal128::kRoundTowardZero);

        invariant(v_prime2.log2().isLess(Decimal128(128)));

        // Now we need to get the Decimal128 out as a 128-bit integer
        // But Decimal128 does not support conversion to Int128.
        //
        // If we think the Decimal128 fits in the range, based on the maximum value, we try to
        // convert to int64 directly.
        if (bits_range < 64) {

            // Try conversion to int64, it may fail but since it is easy we try this first.
            //
            uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;

            std::int64_t vPrimeInt264 = v_prime2.toLongExact(&signalingFlags);

            if (signalingFlags == Decimal128::SignalingFlag::kNoFlag) {
                std::uint64_t vPrimeUInt264 = static_cast<uint64_t>(vPrimeInt264);
                return {vPrimeUInt264, 0, (1ULL << bits_range) - 1};
            }
        }

        boost::multiprecision::uint128_t u_ret = toUInt128FromDecimal128(v_prime2);

        boost::multiprecision::uint128_t max_dec =
            (boost::multiprecision::uint128_t(1) << bits_range) - 1;

        return {u_ret, 0, max_dec};
    }

    bool isNegative = value.isNegative();
    int32_t scale = value.getBiasedExponent() - Decimal128::kExponentBias;
    int64_t highCoefficent = value.getCoefficientHigh();
    int64_t lowCoefficient = value.getCoefficientLow();

// use int128_t where possible on gcc/clang
#ifdef __SIZEOF_INT128__
    __int128 cMax1 = 0x1ed09bead87c0;
    cMax1 <<= 64;
    cMax1 |= 0x378d8e63ffffffff;
    const boost::multiprecision::uint128_t cMax(cMax1);
    if (kDebugBuild) {
        const boost::multiprecision::uint128_t cMaxStr("9999999999999999999999999999999999");
        dassert(cMaxStr == cMax);
    }
#else
    boost::multiprecision::uint128_t cMax("9999999999999999999999999999999999");
#endif
    const int64_t eMin = -6176;

    boost::multiprecision::int128_t unscaledValue(highCoefficent);
    unscaledValue <<= 64;
    unscaledValue += lowCoefficient;

    int64_t rho = 0;
    auto stepValue = unscaledValue;

    bool flag = true;
    if (unscaledValue == 0) {
        flag = false;
    }

    while (flag != false) {
        if (stepValue > cMax) {
            flag = false;
            rho = rho - 1;
            stepValue /= k10;
        } else {
            rho = rho + 1;
            stepValue *= k10;
        }
    }

    boost::multiprecision::uint128_t mapping = 0;
    auto part2 = k1 << 127;

    if (unscaledValue == 0) {
        mapping = part2;
    } else if (rho <= scale - eMin) {
        auto part1 = stepValue + (cMax * (scale - eMin - rho));
        if (isNegative) {
            part1 = -part1;
        }

        mapping = static_cast<boost::multiprecision::uint128_t>(part1 + part2);

    } else {
        auto part1 = exp10(scale - eMin) * unscaledValue;
        if (isNegative) {
            part1 = -part1;
        }

        mapping = static_cast<boost::multiprecision::uint128_t>(part1 + part2);
    }

    return {mapping, 0, std::numeric_limits<boost::multiprecision::uint128_t>::max()};
}

bool canUsePrecisionMode(Decimal128 min, Decimal128 max, uint32_t precision, uint32_t* maxBitsOut) {
    const Decimal128 int_128_max_decimal("340282366920938463463374607431768211455");

    uassert(9178807,
            "Invalid upper and lower bounds for Decimal128 precision. Min must be strictly less "
            "than max.",
            min < max);

    // Ensure the precision can be converted to signed int without overflow
    // since Decimal128::scale implicitly converts uint32_t to int
    uassert(9125501,
            str::stream() << "Precision cannot be greater than "
                          << std::numeric_limits<int32_t>::max(),
            precision <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
    uassert(9125502,
            "Precision is too large and cannot be used to calculate the scaled range bounds",
            Decimal128(1).scale(precision).isFinite());

    const auto scaled_min = min.scale(precision);
    const auto scaled_max = max.scale(precision);

    const auto scaled_min_trunc = scaled_min.round(Decimal128::RoundingMode::kRoundTowardZero);
    const auto scaled_max_trunc = scaled_max.round(Decimal128::RoundingMode::kRoundTowardZero);

    uassert(9178808,
            "Invalid upper bounds for Decimal128 precision. Digits after the decimal must be less "
            "than specified precision value",
            scaled_max.isEqual(scaled_max_trunc));

    uassert(9178809,
            "Invalid lower bounds for Decimal128 precision. Digits after the decimal must be less "
            "than specified precision value",
            scaled_min.isEqual(scaled_min_trunc));

    uassert(9178810,
            "Invalid upper bounds for Decimal128 precision, must be less than "
            "170141183460469231731687303715884105727",
            scaled_max.toAbs() < scaled_decimal_bounds);

    uassert(9178811,
            "Invalid lower bounds for Decimal128 precision, must be less than "
            "170141183460469231731687303715884105727",
            scaled_min.toAbs() < scaled_decimal_bounds);

    const auto t_1 = scaled_max.subtract(scaled_min);
    const auto t_4 = int_128_max_decimal.subtract(t_1);
    const auto t_5 =
        t_4.log10().round(Decimal128::RoundingMode::kRoundTowardZero).subtract(Decimal128(1));

    uassert(9178812, "Invalid value for precision", t_5.isGreaterEqual(Decimal128(precision)));

    const auto i_1 = toInt128FromDecimal128(scaled_max);
    const auto i_2 = toInt128FromDecimal128(scaled_min);

    // We do not need to check for overflow for int128 in boost because boost represents their
    // integers as "128-bits of precision plus an extra sign bit".
    // https://www.boost.org/doc/libs/1_74_0/libs/multiprecision/doc/html/boost_multiprecision/tut/ints/cpp_int.html
    const auto i_3 = i_1 - i_2 + toInt128FromDecimal128(Decimal128(1).scale(precision));

    uassert(9178813,
            "Invalid upper and lower bounds for Decimal128 precision. Min must be strictly less "
            "than max.",
            i_3 > 0);

    const auto ui_3 = static_cast<boost::multiprecision::uint128_t>(i_3);

    const uint64_t bits = ceil_log2(ui_3);

    if (bits >= 128) {
        return false;
    }

    if (maxBitsOut) {
        *maxBitsOut = bits;
    }

    return true;
}

bool canUsePrecisionMode(double min, double max, uint32_t precision, uint32_t* maxBitsOut) {
    uassert(
        9178800,
        "Invalid upper and lower bounds for double precision. Min must be strictly less than max.",
        min < max);

    // Ensure the precision can be converted to signed int without overflow
    // since exp10Double implicitly converts uint32_t to int
    uassert(9125503,
            str::stream() << "Precision cannot be greater than "
                          << std::numeric_limits<int32_t>::max(),
            precision <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));

    const auto scaled_prc = exp10Double(precision);
    uassert(9125504,
            "Precision is too large and cannot be used to calculate the scaled range bounds",
            !std::isinf(scaled_prc));

    auto scaled_max = max * scaled_prc;
    auto scaled_min = min * scaled_prc;

    uassert(9178801,
            "Invalid upper bounds for double precision. Digits after the decimal must be less than "
            "specified precision value",
            scaled_max == trunc(scaled_max));

    uassert(9178802,
            "Invalid lower bounds for double precision. Digits after the decimal must be less than "
            "specified precision value",
            scaled_min == trunc(scaled_min));

    uassert(9178803,
            "Invalid upper bounds for double precision, must be less than 9223372036854775807",
            std::fabs(scaled_max) < SCALED_DOUBLE_BOUNDS);

    uassert(9178804,
            "Invalid lower bounds for double precision, must be less than 9223372036854775807",
            std::fabs(scaled_min) < SCALED_DOUBLE_BOUNDS);

    const auto t_1 = scaled_max - scaled_min;
    const auto t_4 = INT_64_MAX_DOUBLE - t_1;
    const auto t_5 = floor(log10(t_4)) - 1;

    uassert(9178805, "Invalid value for precision", static_cast<double>(precision) <= t_5);

    const auto i_1 = static_cast<int64_t>(scaled_max);
    const auto i_2 = static_cast<int64_t>(scaled_min);
    int64_t i_range, i_3;

    if (overflow::sub(i_1, i_2, &i_range)) {
        return false;
    }

    if (overflow::add(i_range, static_cast<int64_t>(std::lround(scaled_prc)), &i_3)) {
        return false;
    }

    uassert(
        9178806,
        "Invalid value for upper and lower bounds for double precision. Min must be less than max.",
        i_3 > 0);
    const auto ui_3 = static_cast<uint64_t>(i_3);

    const uint32_t bits = ceil_log2(ui_3);

    if (bits >= 64) {
        return false;
    }

    if (maxBitsOut) {
        *maxBitsOut = bits;
    }
    return true;
}

}  // namespace mongo

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

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
constexpr boost::multiprecision::uint128_t k1(1);
constexpr boost::multiprecision::int128_t k10(10);
constexpr boost::multiprecision::uint128_t k10ui(10);
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

        // Subnormal representations can support up to 5x10^-324 as a number
        uassert(6966801, "Precision must be between 0 and 324 inclusive", precision.get() <= 324);

        uassert(6966803,
                "Must specify both a lower bound, upper bound and precision",
                min.has_value() == max.has_value() && max.has_value() == precision.has_value());

        double range = max.get() - min.get();

        // We can overflow if max = max double and min = min double so make sure we have finite
        // number after we do subtraction
        if (std::isfinite(range)) {

            // This creates a range which is wider then we permit by our min/max bounds check with
            // the +1 but it is as the algorithm is written in the paper.
            double rangeAndPrecision = (range + 1) * exp10Double(precision.get());

            if (std::isfinite(rangeAndPrecision)) {

                double bits_range_double = log2(rangeAndPrecision);
                bits_range = ceil(bits_range_double);

                if (bits_range < 64) {
                    use_precision_mode = true;
                }
            }
        }
    }

    if (use_precision_mode) {

        // Take a number of xxxx.ppppp and truncate it xxxx.ppp if precision = 3. We do not change
        // the digits before the decimal place.
        double v_prime = trunc(value * exp10Double(precision.get())) / exp10Double(precision.get());
        int64_t v_prime2 = (v_prime - min.get()) * exp10Double(precision.get());

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

boost::multiprecision::uint128_t toInt128FromDecimal128(Decimal128 dec) {
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
            roundTrip == dec);

    return ret;
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
    int bits_range = 0;
    if (precision.has_value()) {
        uassert(6966804,
                "Must specify both a lower bound, upper bound and precision",
                min.has_value() == max.has_value() && max.has_value() == precision.has_value());

        uassert(6966802, "Precision must be between 0 and 6182 inclusive", precision.get() <= 6142);


        Decimal128 bounds = max.get().subtract(min.get()).add(Decimal128(1));

        if (bounds.isFinite()) {
            Decimal128 bits_range_dec = bounds.scale(precision.get()).logarithm(Decimal128(2));

            if (bits_range_dec.isFinite() && bits_range_dec < Decimal128(128)) {
                // kRoundTowardPositive is the same as C99 ceil()

                bits_range = bits_range_dec.toIntExact(Decimal128::kRoundTowardPositive);

                // bits_range is always >= 0 but coverity cannot be sure since it does not
                // understand Decimal128 math so we add a check for positive integers.
                if (bits_range >= 0 && bits_range < 128) {
                    use_precision_mode = true;
                }
            }
        }
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

        invariant(v_prime2.logarithm(Decimal128(2)).isLess(Decimal128(128)));

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

        boost::multiprecision::uint128_t u_ret = toInt128FromDecimal128(v_prime2);

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

}  // namespace mongo

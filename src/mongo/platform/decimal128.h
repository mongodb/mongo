/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/data_type.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

namespace MONGO_MOD_PUB mongo {

/**
 * Wrapper class for the MongoDB Decimal128 data type. Sample usage:
 *     Decimal128 d1("+10.0");
 *     Decimal128 d2("+0.1")
 *     Decimal128 sum = d1.add(d2)
 *     std::cout << sum << std::endl;
 */
class Decimal128 {
public:
    /**
     * Static constants to get Decimal128 representations of specific numbers
     * kLargestPositive -> 9999999999999999999999999999999999E6111
     * kSmallestPositive -> 1E-6176
     * kLargestNegative -> -9999999999999999999999999999999999E6111
     * kSmallestNegative -> -1E-6176
     * kLargestNegativeExponentZero -> 0E-6176
     */
    static const Decimal128 kLargestPositive;
    static const Decimal128 kSmallestPositive;
    static const Decimal128 kLargestNegative;
    static const Decimal128 kSmallestNegative;

    static const Decimal128 kNormalizedZero;  // zero with exponent 0
    static const Decimal128 kLargestNegativeExponentZero;

    static const Decimal128 kPositiveInfinity;
    static const Decimal128 kNegativeInfinity;
    static const Decimal128 kPositiveNaN;
    static const Decimal128 kNegativeNaN;

    static const Decimal128 kPi;
    static const Decimal128 kPiOver180;
    static const Decimal128 k180OverPi;

    static constexpr std::uint32_t kMaxBiasedExponent = 6143 + 6144;
    // Biased exponent of a Decimal128 with least significant digit in the units place
    static constexpr std::int32_t kExponentBias = 6143 + 33;
    static constexpr std::uint32_t kInfinityExponent =
        kMaxBiasedExponent + 1;  // internal convention only

    /**
     * This struct holds the raw data for IEEE 754-2008 data types
     */
    struct Value {
        std::uint64_t low64;
        std::uint64_t high64;
    };

    enum RoundingMode {
        kRoundTiesToEven = 0,
        kRoundTowardNegative = 1,
        kRoundTowardPositive = 2,
        kRoundTowardZero = 3,
        kRoundTiesToAway = 4
    };

    /**
     * Indicates if constructing a Decimal128 from a double should round the double to 15 digits
     * (so the conversion will correctly round-trip decimals), or round to the full 34 digits.
     */
    enum RoundingPrecision { kRoundTo15Digits = 0, kRoundTo34Digits = 1 };

    /**
     * The signaling flag enum determines the signaling nature of a decimal operation.
     * The values of these flags are defined in the Intel RDFP math library.
     *
     * The provided hasFlag method checks whether provided signalingFlags contains flag f.
     *
     * Example:
     *     Decimal128 dcml = Decimal128('0.1');
     *     std::uint32_t sigFlag = Decimal128::SignalingFlag::kNoFlag;
     *     double dbl = dcml.toDouble(&sigFlag);
     *     if Decimal128::hasFlag(sigFlag, SignalingFlag::kInexact)
     *         cout << "inexact decimal to double conversion!" << endl;
     */
    enum SignalingFlag {
        kNoFlag = 0x00,
        kInvalid = 0x01,
        kDivideByZero = 0x04,
        kOverflow = 0x08,
        kUnderflow = 0x10,
        kInexact = 0x20,
    };

    constexpr static bool hasFlag(std::uint32_t signalingFlags, SignalingFlag f) {
        return ((signalingFlags & f) != 0u);
    }

    /**
     * Returns true if a valid Decimal can be constructed from the given arguments.
     */
    constexpr static bool isValid(std::uint64_t sign,
                                  std::uint64_t exponent,
                                  std::uint64_t coefficientHigh,
                                  std::uint64_t coefficientLow) {
        if (coefficientHigh >= 0x1ed09bead87c0 &&
            (coefficientHigh != 0x1ed09bead87c0 || coefficientLow != 0x378d8e63ffffffff)) {
            return false;
        }
        auto value =
            Value{coefficientLow,
                  (sign << kSignFieldPos) | (exponent << kExponentFieldPos) | coefficientHigh};

        return Decimal128(value).getBiasedExponent() == exponent;
    }

    /**
     * Construct a 0E0 valued Decimal128.
     */
    constexpr Decimal128()
        : _value{0,
                 static_cast<std::uint64_t>(Decimal128::kExponentBias)
                     << Decimal128::kExponentFieldPos} {}

    /**
     * This constructor takes in a raw decimal128 type, which consists of two
     * uint64_t's. This class performs an endian check on the system to ensure
     * that the Value.high64 represents the higher 64 bits.
     */
    constexpr explicit Decimal128(Decimal128::Value dec128Value) : _value(dec128Value) {}

    /**
     * Constructs a Decimal128 from parts, dealing with proper encoding of the combination field.
     * Assumes that the value will be inside the valid range of finite values. (No NaN/Inf, etc.)
     */
    constexpr Decimal128(std::uint64_t sign,
                         std::uint64_t exponent,
                         std::uint64_t coefficientHigh,
                         std::uint64_t coefficientLow)
        : _value(_valueFromParts(sign, exponent, coefficientHigh, coefficientLow)) {}

    template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
    constexpr explicit Decimal128(T v)
        : Decimal128(v < 0 ? 1 : 0, kExponentBias, 0, _makeCoefficientLow(v)) {}

    /**
     * This constructor takes a double and constructs a Decimal128 object given a roundMode, either
     * to full precision, or with a fixed precision of 15 decimal digits. When a double is used to
     * store a decimal floating point number, it is only correct up to 15 digits after converting
     * back to decimal, so the 15 digit rounding is used for mixed-mode operations.
     * The general idea is to quantize the direct double->dec128 conversion
     * with a quantum of 1E(-15 +/- base10 exponent equivalent of the double).
     * To do this, we find the smallest (abs value) base 10 exponent greater
     * than the double's base 2 exp and shift the quantizer's exp accordingly.
     */
    explicit Decimal128(double doubleValue,
                        RoundingPrecision roundPrecision = kRoundTo15Digits,
                        RoundingMode roundMode = kRoundTiesToEven);

    /**
     * This constructor takes a string and constructs a Decimal128 object from it.
     * Inputs larger than 34 digits of precision are rounded according to the
     * specified rounding mode. The following (and variations) are all accepted:
     * "+2.02E200"
     * "2.02E+200"
     * "-202E-500"
     * "somethingE200" --> NaN
     * "200E9999999999" --> +Inf
     * "-200E9999999999" --> -Inf
     */
    explicit Decimal128(const std::string& stringValue,
                        RoundingMode roundMode = kRoundTiesToEven,
                        size_t* charsConsumed = nullptr);

    Decimal128(const std::string& stringValue,
               std::uint32_t* signalingFlag,
               RoundingMode roundMode = kRoundTiesToEven,
               size_t* charsConsumed = nullptr);

    /**
     * This function gets the inner Value struct storing a Decimal128 value.
     */
    constexpr Value getValue() const {
        return _value;
    }

    /**
     *  Extracts the biased exponent from the combination field.
     */
    constexpr std::uint32_t getBiasedExponent() const {
        const std::uint64_t combo = _getCombinationField();
        if (combo < kCombinationNonCanonical)
            return combo >> 3;

        return combo >= kCombinationInfinity
            ? kMaxBiasedExponent + 1           // NaN or Inf
            : (combo >> 1) & ((1 << 14) - 1);  // non-canonical representation
    }

    /**
     * Returns the high 49 bits of the 113-bit binary encoded coefficient. Returns 0 for
     * non-canonical or non-finite numbers.
     */
    constexpr std::uint64_t getCoefficientHigh() const {
        return _getCombinationField() < kCombinationNonCanonical
            ? _value.high64 & kCanonicalCoefficientHighFieldMask
            : 0;
    }

    /**
     * Returns the low 64 bits of the 113-bit binary encoded coefficient. Returns 0 for
     * non-canonical or non-finite numbers.
     */
    constexpr std::uint64_t getCoefficientLow() const {
        return _getCombinationField() < kCombinationNonCanonical ? _value.low64 : 0;
    }

    /**
     * Returns the absolute value of this.
     */
    Decimal128 toAbs() const;

    /**
     * Returns the atan2(this, other) rather than atan2(other,this), which
     * would produce different results.
     */
    Decimal128 atan2(const Decimal128& other,
                     std::uint32_t* signalingFlags,
                     RoundingMode roundMode) const;

    Decimal128 atan2(const Decimal128& other, RoundingMode roundMode) const {
        return atan2(other, ThrowawayFlag{}, roundMode);
    }

    Decimal128 atan2(const Decimal128& other) {
        return atan2(other, kRoundTiesToEven);
    }

/**
 * Generates overloads for a member function taking no arguments except the optional
 * flags and optional round mode.
 */
#define MONGO_DECIMAL128_DECLARE_UNARY_OP(op)                                   \
    Decimal128 op(std::uint32_t* signalingFlags, RoundingMode roundMode) const; \
    Decimal128 op(RoundingMode roundMode) const {                               \
        return op(ThrowawayFlag{}, roundMode);                                  \
    }                                                                           \
    Decimal128 op() const {                                                     \
        return op(ThrowawayFlag{}, kRoundTiesToEven);                           \
    }

    MONGO_DECIMAL128_DECLARE_UNARY_OP(sqrt)  ///< square root

    MONGO_DECIMAL128_DECLARE_UNARY_OP(exp)    ///< e^x
    MONGO_DECIMAL128_DECLARE_UNARY_OP(exp2)   ///< 2^x
    MONGO_DECIMAL128_DECLARE_UNARY_OP(exp10)  ///< 10^x

    MONGO_DECIMAL128_DECLARE_UNARY_OP(log)    ///< log base e
    MONGO_DECIMAL128_DECLARE_UNARY_OP(log2)   ///< log base 2
    MONGO_DECIMAL128_DECLARE_UNARY_OP(log10)  ///< log base 10

    /** Trigonometric member functions and their inverses. */
    MONGO_DECIMAL128_DECLARE_UNARY_OP(sin)
    MONGO_DECIMAL128_DECLARE_UNARY_OP(cos)
    MONGO_DECIMAL128_DECLARE_UNARY_OP(tan)
    MONGO_DECIMAL128_DECLARE_UNARY_OP(asin)
    MONGO_DECIMAL128_DECLARE_UNARY_OP(acos)
    MONGO_DECIMAL128_DECLARE_UNARY_OP(atan)

    /** Hyperbolic member functions and their inverses. */
    MONGO_DECIMAL128_DECLARE_UNARY_OP(sinh)
    MONGO_DECIMAL128_DECLARE_UNARY_OP(cosh)
    MONGO_DECIMAL128_DECLARE_UNARY_OP(tanh)
    MONGO_DECIMAL128_DECLARE_UNARY_OP(asinh)
    MONGO_DECIMAL128_DECLARE_UNARY_OP(acosh)
    MONGO_DECIMAL128_DECLARE_UNARY_OP(atanh)

#undef MONGO_DECIMAL128_DECLARE_UNARY_OP

    /**
     * Returns `this` with inverted sign bit
     */
    constexpr Decimal128 negate() const {
        return Decimal128(Value{_value.low64, _value.high64 ^ (std::uint64_t{1} << 63)});
    }

    Decimal128 round(RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 round(std::uint32_t* signalingFlags,
                     RoundingMode roundMode = kRoundTiesToEven) const;

    /**
     * This set of functions converts a Decimal128 to a certain integer type with a
     * given rounding mode.
     *
     * Each function is overloaded to provide an optional signalingFlags input-output parameter that
     * will be bitwise ORed with one of the following Decimal128::SignalingFlag enumerators:
     * kNoFlag, kInvalid
     *
     * Note: The signaling flags for these functions only signal
     * an invalid conversion. If inexact conversion flags are necessary, call
     * the toTypeExact version of the function defined below. This set of operations
     * (toInt, toLong) has better performance than the latter.
     */
    std::int32_t toInt(RoundingMode roundMode = kRoundTiesToEven) const;
    std::int32_t toInt(std::uint32_t* signalingFlags,
                       RoundingMode roundMode = kRoundTiesToEven) const;
    std::int64_t toLong(RoundingMode roundMode = kRoundTiesToEven) const;
    std::int64_t toLong(std::uint32_t* signalingFlags,
                        RoundingMode roundMode = kRoundTiesToEven) const;

    /**
     * This set of functions converts a Decimal128 to a certain integer type with a
     * given rounding mode. The signaling flags for these functions will also signal
     * inexact computation.
     *
     * Each function is overloaded to provide an optional signalingFlags input-output parameter that
     * will be bitwise ORed with one of the following Decimal128::SignalingFlag enumerators:
     * kNoFlag, kInexact, kInvalid
     */
    std::int32_t toIntExact(RoundingMode roundMode = kRoundTiesToEven) const;
    std::int32_t toIntExact(std::uint32_t* signalingFlags,
                            RoundingMode roundMode = kRoundTiesToEven) const;
    std::int64_t toLongExact(RoundingMode roundMode = kRoundTiesToEven) const;
    std::int64_t toLongExact(std::uint32_t* signalingFlags,
                             RoundingMode roundMode = kRoundTiesToEven) const;
    std::uint64_t toULongExact(RoundingMode roundMode = kRoundTiesToEven) const;
    std::uint64_t toULongExact(std::uint32_t* signalingFlags,
                               RoundingMode roundMode = kRoundTiesToEven) const;

    /**
     * These functions convert decimals to doubles and have the ability to signal
     * inexact, underflow, overflow, and invalid operation.
     *
     * Each function is overloaded to provide an optional signalingFlags input-output parameter that
     * will be bitwise ORed with one of the following Decimal128::SignalingFlag enumerators:
     * kNoFlag, kInexact, kUnderflow, kOverflow, kInvalid
     */
    double toDouble(RoundingMode roundMode = kRoundTiesToEven) const;
    double toDouble(std::uint32_t* signalingFlags, RoundingMode roundMode = kRoundTiesToEven) const;

    /**
     * This function converts a Decimal128 to a string with the following semantics:
     *
     * Suppose Decimal128 D has P significant digits and exponent Exp.
     * Define SE to be the scientific exponent of D equal to Exp + P - 1.
     *
     * Define format E as normalized scientific notation (ex: 1.0522E+16)
     * Define format F as a regular formatted number with no exponent (ex: 105.22)
     *
     * In order to improve decimal type readability,
     * if SE >= 12 or SE <= -4, use format E to display D.
     * if Exp > 0, use format E to display D because adding trailing zeros implies
     * extra, incorrect precision
     *
     * Otherwise, display using F with no exponent (add leading zeros if necessary).
     *
     * This conversion to string is roughly based on the G C99 printf specifier and
     * existing behavior for the double numeric type in MongoDB.
     */
    std::string toString() const;

    /**
     * This set of functions check whether a Decimal128 is Zero, NaN, or +/- Inf
     */
    bool isZero() const;
    bool isNaN() const;
    bool isInfinite() const;
    bool isNegative() const;

    /**
     * Return true if and only if a Decimal128 is Zero, Normal, or Subnormal (not Inf or NaN)
     */
    bool isFinite() const;

    /**
     * This set of mathematical operation functions implement the corresponding
     * IEEE 754-2008 operations on self and other.
     * The 'add' and 'multiply' methods are commutative, so a.add(b) is equivalent to b.add(a).
     * Rounding of results that require a precision greater than 34 decimal digits
     * is performed using the supplied rounding mode (defaulting to kRoundTiesToEven).
     * NaNs and infinities are handled according to the IEEE 754-2008 specification.
     *
     * Each function is overloaded to provide an optional signalingFlags input-output parameter that
     * will be bitwise ORed with one of the following Decimal128::SignalingFlag enumerators:
     * kNoFlag, kInexact, kUnderflow, kOverflow, kInvalid
     *
     * The divide operation may also set signalingFlags to kDivideByZero
     */
    Decimal128 add(const Decimal128& other, RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 add(const Decimal128& other,
                   std::uint32_t* signalingFlags,
                   RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 subtract(const Decimal128& other, RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 subtract(const Decimal128& other,
                        std::uint32_t* signalingFlags,
                        RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 multiply(const Decimal128& other, RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 multiply(const Decimal128& other,
                        std::uint32_t* signalingFlags,
                        RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 divide(const Decimal128& other, RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 divide(const Decimal128& other,
                      std::uint32_t* signalingFlags,
                      RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 modulo(const Decimal128& other) const;
    Decimal128 modulo(const Decimal128& other, std::uint32_t* signalingFlags) const;

    Decimal128 power(const Decimal128& other, RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 power(const Decimal128& other,
                     std::uint32_t* signalingFlags,
                     RoundingMode roundMode = kRoundTiesToEven) const;

    Decimal128 scale(int n, RoundingMode roundMode = kRoundTiesToEven) const;

    /**
     * This function quantizes the current decimal given a quantum reference
     */
    Decimal128 quantize(const Decimal128& reference,
                        RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 quantize(const Decimal128& reference,
                        std::uint32_t* signalingFlags,
                        RoundingMode roundMode = kRoundTiesToEven) const;

    /**
     * This function normalizes the cohort of a Decimal128 by forcing it to maximum
     * precision (34 decimal digits). This normalization is important when it is desirable
     * to force equal decimals of different representations (i.e. 5.0 and 5.00) to equal
     * decimals with the same representation (5000000000000000000000000000000000E-33).
     * Hashing equal decimals to equal hashes becomes possible with such normalization.
     */
    Decimal128 normalize() const {
        // Normalize by adding 0E-6176 which forces a decimal to maximum precision (34 digits)
        return add(kLargestNegativeExponentZero);
    }

    /**
     * This set of comparison operations takes a single Decimal128 and returns a boolean
     * noting the value of the comparison. These comparisons are not total ordered, but
     * comply with the IEEE 754-2008 spec. The comparison returns true if the caller
     * is <equal, notequal, greater, greaterequal, less, lessequal> the argument (other).
     */
    bool isEqual(const Decimal128& other) const;
    bool isNotEqual(const Decimal128& other) const;
    bool isGreater(const Decimal128& other) const;
    bool isGreaterEqual(const Decimal128& other) const;
    bool isLess(const Decimal128& other) const;
    bool isLessEqual(const Decimal128& other) const;

    /**
     * Returns true iff 'this' and 'other' are bitwise identical. Note that this returns false
     * even for values that may convert to identical strings, such as different NaNs or
     * non-canonical representations that represent bit-patterns never generated by any conforming
     * implementation, but should be treated as 0. Mostly for testing.
     */
    bool isBinaryEqual(const Decimal128& other) const {
        return _value.high64 == other._value.high64 && _value.low64 == other._value.low64;
    }

    constexpr Decimal128 operator-() const {
        return negate();
    }

    constexpr Decimal128 operator+() const {
        return *this;
    }

private:
    /** Used as a temporary to provide a missing `signalingFlags` argument. */
    class ThrowawayFlag {
    public:
        constexpr operator uint32_t*() && {
            return &_i;
        }

    private:
        uint32_t _i = 0;
    };

    constexpr static std::uint8_t kSignFieldPos = 64 - 1;
    constexpr static std::uint8_t kCombinationFieldPos = kSignFieldPos - 17;
    constexpr static std::uint64_t kCombinationFieldMask = (1 << 17) - 1;
    constexpr static std::uint64_t kExponentFieldPos = kCombinationFieldPos + 3;
    constexpr static std::uint64_t kCoefficientContinuationFieldMask =
        (1ull << kCombinationFieldPos) - 1;
    constexpr static std::uint64_t kCombinationNonCanonical = 3 << 15;
    constexpr static std::uint64_t kCombinationInfinity = 0x1e << 12;
    constexpr static std::uint64_t kCombinationNaN = 0x1f << 12;
    constexpr static std::uint64_t kCanonicalCoefficientHighFieldMask = (1ull << 49) - 1;

    std::string _convertToScientificNotation(StringData coefficient, int adjustedExponent) const;
    std::string _convertToStandardDecimalNotation(StringData coefficient, int exponent) const;

    /**
     * This function quantizes the current decimal given a quantum reference without normalizing its
     * arguments.
     */
    Decimal128 nonNormalizingQuantize(const Decimal128& reference,
                                      RoundingMode roundMode = kRoundTiesToEven) const;
    Decimal128 nonNormalizingQuantize(const Decimal128& reference,
                                      std::uint32_t* signalingFlags,
                                      RoundingMode roundMode = kRoundTiesToEven) const;

    constexpr std::uint64_t _getCombinationField() const {
        return (_value.high64 >> kCombinationFieldPos) & kCombinationFieldMask;
    }

    constexpr static Value _valueFromParts(std::uint64_t sign,
                                           std::uint64_t exponent,
                                           std::uint64_t coefficientHigh,
                                           std::uint64_t coefficientLow) {
        // For constexpr's sake the invariant must be compiled only if !isValid().
        if (!isValid(sign, exponent, coefficientHigh, coefficientLow)) {
            invariant(false, "invalid arguments to Decimal128 constructor");
        }

        return Value{coefficientLow,
                     (sign << kSignFieldPos) | (exponent << kExponentFieldPos) | coefficientHigh};
    }

    // The absolute value of constexpr `i` as a uint64_t, avoiding warnings.
    template <typename T>
    static constexpr std::uint64_t _makeCoefficientLow(T i) {
        if constexpr (std::is_signed_v<T>) {
            if (i < 0) {
                std::make_unsigned_t<T> ui = i;
                ui = ~ui + 1;  // Negation, without MSVC C4146.
                return static_cast<std::uint64_t>(ui);
            } else {
                return static_cast<std::uint64_t>(i);
            }
        } else {
            return static_cast<std::uint64_t>(i);
        }
    }

    Value _value;
};

inline namespace literals {

inline Decimal128 operator""_dec128(const char* s) {
    return Decimal128(s);
}

inline Decimal128 operator""_dec128(const char* s, std::size_t len) {
    return Decimal128(std::string(s, len));
}

inline bool operator<(const Decimal128& lhs, const Decimal128& rhs) {
    return lhs.isLess(rhs);
}

inline bool operator<=(const Decimal128& lhs, const Decimal128& rhs) {
    return lhs.isLessEqual(rhs);
}

inline bool operator>(const Decimal128& lhs, const Decimal128& rhs) {
    return lhs.isGreater(rhs);
}

inline bool operator>=(const Decimal128& lhs, const Decimal128& rhs) {
    return lhs.isGreaterEqual(rhs);
}

inline bool operator==(const Decimal128& lhs, const Decimal128& rhs) {
    return lhs.isEqual(rhs);
}

inline bool operator!=(const Decimal128& lhs, const Decimal128& rhs) {
    return lhs.isNotEqual(rhs);
}

}  // namespace literals

template <>
struct DataType::Handler<Decimal128> {
    static void unsafeLoad(Decimal128* valueOut, const char* ptr, size_t* advanced);
    static void unsafeStore(const Decimal128& valueIn, char* ptr, size_t* advanced);

    static Status load(Decimal128* valueOut,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset) {
        if (length < kSizeOfDecimal) {
            return Status(ErrorCodes::Overflow, "Buffer too small to hold Decimal128 value");
        }

        unsafeLoad(valueOut, ptr, advanced);
        return Status::OK();
    }

    static Status store(const Decimal128& valueIn,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset) {
        if (length < kSizeOfDecimal) {
            return Status(ErrorCodes::Overflow, "Buffer too small to write Decimal128 value");
        }

        unsafeStore(valueIn, ptr, advanced);
        return Status::OK();
    }

    static Decimal128 defaultConstruct() {
        return Decimal128();
    }

    static constexpr size_t kSizeOfDecimal = 2 * sizeof(uint64_t);
};

}  // namespace MONGO_MOD_PUB mongo

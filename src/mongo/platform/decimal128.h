/*    Copyright 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

#include "mongo/config.h"

namespace mongo {

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
 * This boolean is used as a master switch to enable and disable decimal support
 * and is set by the build flag --experimental-decimal-support.
 */
#ifdef MONGO_CONFIG_EXPERIMENTAL_DECIMAL_SUPPORT
    static const bool enabled = true;
#else
    static const bool enabled = false;
#endif

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

    static const Decimal128 kLargestNegativeExponentZero;

    static const Decimal128 kPositiveInfinity;
    static const Decimal128 kNegativeInfinity;
    static const Decimal128 kPositiveNaN;
    static const Decimal128 kNegativeNaN;

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

    static bool hasFlag(std::uint32_t signalingFlags, SignalingFlag f) {
        return ((signalingFlags & f) != 0u);
    }

    Decimal128() = default;

    /**
     * This constructor takes in a raw decimal128 type, which consists of two
     * uint64_t's. This class performs an endian check on the system to ensure
     * that the Value.high64 represents the higher 64 bits.
     */
    Decimal128(Decimal128::Value dec128Value) : _value(dec128Value) {}

    Decimal128(std::int32_t int32Value);
    Decimal128(std::int64_t int64Value);

    /**
     * This constructor takes a double and constructs a Decimal128 object
     * given a roundMode with a fixed precision of 15. Doubles can only
     * properly represent a decimal precision of 15-17 digits.
     * The general idea is to quantize the direct double->dec128 conversion
     * with a quantum of 1E(-15 +/- base10 exponent equivalent of the double).
     * To do this, we find the smallest (abs value) base 10 exponent greater
     * than the double's base 2 exp and shift the quantizer's exp accordingly.
     */
    Decimal128(double doubleValue, RoundingMode roundMode = kRoundTiesToEven);

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
    Decimal128(std::string stringValue, RoundingMode roundMode = kRoundTiesToEven);

    /**
     * This function gets the inner Value struct storing a Decimal128 value.
     */
    Value getValue() const;

    /**
     * This function returns the decimal absolute value of the caller.
     */
    Decimal128 toAbs() const;

    /**
     * This set of functions converts a Decimal128 to a certain integer type with a
     * given rounding mode.
     *
     * Each function is overloaded to provide an optional signalingFlags output parameter
     * that can be set to one of the Decimal128::SignalingFlag enumerators:
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
     * Each function is overloaded to provide an optional signalingFlags output parameter
     * that can be set to one of the Decimal128::SignalingFlag enumerators:
     * kNoFlag, kInexact, kInvalid
     */
    std::int32_t toIntExact(RoundingMode roundMode = kRoundTiesToEven) const;
    std::int32_t toIntExact(std::uint32_t* signalingFlags,
                            RoundingMode roundMode = kRoundTiesToEven) const;
    std::int64_t toLongExact(RoundingMode roundMode = kRoundTiesToEven) const;
    std::int64_t toLongExact(std::uint32_t* signalingFlags,
                             RoundingMode roundMode = kRoundTiesToEven) const;

    /**
     * These functions convert decimals to doubles and have the ability to signal
     * inexact, underflow, overflow, and invalid operation.
     *
     * This function is overloaded to provide an optional signalingFlags output parameter
     * that can be set to one of the Decimal128::SignalingFlag enumerators:
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
     * This set of mathematical operation functions implement the corresponding
     * IEEE 754-2008 operations on self and other.
     * The operations are commutative, so a.add(b) is equivalent to b.add(a).
     * Rounding of results that require a precision greater than 34 decimal digits
     * is performed using the supplied rounding mode (defaulting to kRoundTiesToEven).
     * NaNs and infinities are handled according to the IEEE 754-2008 specification.
     *
     * Each function is overloaded to provide an optional signalingFlags output parameter
     * that can be set to one of the Decimal128::SignalingFlag enumerators:
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
    Decimal128 normalize() const;

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

private:
    Value _value;
};

}  // namespace mongo

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

#include "mongo/platform/basic.h"

#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <type_traits>

#include <boost/optional.hpp>

#include "mongo/unittest/unittest.h"

#include "mongo/util/represent_as.h"

namespace mongo {

namespace {

using namespace fmt::literals;

// Char values
const signed char kCharMax = std::numeric_limits<signed char>::max();
const int kCharMaxAsInt = kCharMax;

// Unsigned char values
const unsigned char kUCharMax = std::numeric_limits<unsigned char>::max();
const unsigned char kUCharMin = std::numeric_limits<unsigned char>::lowest();
const int kUCharMaxAsInt = kUCharMax;

// Int values
const int kIntMax = std::numeric_limits<int>::max();
const int kIntMin = std::numeric_limits<int>::lowest();
const long long kIntMaxAsLongLong = kIntMax;
const long long kIntMinAsLongLong = kIntMin;
const unsigned long long kIntMaxAsULongLong = kIntMax;
const unsigned long long kIntMinAsULongLong = kIntMin;

// 32-bit integer values
const int32_t kInt32Zero = 0;
const int32_t kInt32Max = std::numeric_limits<int32_t>::max();
const int32_t kInt32Min = std::numeric_limits<int32_t>::lowest();
const uint32_t kInt32MaxAsUInt32 = kInt32Max;
const uint64_t kInt32MaxAsUInt64 = kInt32Max;
const double kInt32MaxAsDouble = kInt32Max;
const double kInt32MinAsDouble = kInt32Min;

// Unsigned 32-bit integer values
const uint32_t kUInt32Zero = 0;
const uint32_t kUInt32Max = std::numeric_limits<uint32_t>::max();
const int64_t kUInt32MaxAsInt64 = kUInt32Max;
const float kUInt32MaxAsFloat = static_cast<float>(kUInt32Max);
const double kUInt32MaxAsDouble = kUInt32Max;

// 64-bit integer values
const int64_t kInt64Zero = 0;
const int64_t kInt64Max = std::numeric_limits<int64_t>::max();
const int64_t kInt64Min = std::numeric_limits<int64_t>::lowest();
const uint64_t kInt64MaxAsUInt64 = kInt64Max;
const double kInt64MaxAsDouble = static_cast<double>(kInt64Max);
const double kInt64MinAsDouble = kInt64Min;

// Unsigned 64-bit integer values
const uint64_t kUInt64Zero = 0;
const uint64_t kUInt64Max = std::numeric_limits<uint64_t>::max();
const float kUInt64MaxAsFloat = static_cast<float>(kUInt64Max);
const double kUInt64MaxAsDouble = static_cast<double>(kUInt64Max);


// Long long values
const long long kLongLongMax = std::numeric_limits<long long>::max();

// Unsigned long long values
const unsigned long long kULongLongMax = std::numeric_limits<unsigned long long>::max();

// Float values
const float kFloatZero = 0;
const float kFloatMax = std::numeric_limits<float>::max();
const float kFloatMin = std::numeric_limits<float>::lowest();
const double kFloatMaxAsDouble = kFloatMax;
const double kFloatMinAsDouble = kFloatMin;

// Double values
const double kDoubleZero = 0;
const double kDoubleMax = std::numeric_limits<double>::max();
const double kDoubleMin = std::numeric_limits<double>::lowest();

// Precision values
const int kFloatMantissa = std::numeric_limits<float>::digits;
const int kDoubleMantissa = std::numeric_limits<double>::digits;
const int32_t kInt32TooPreciseForFloat =
    static_cast<int32_t>(std::ldexp(1, kFloatMantissa + 1)) + 1;
const uint32_t kUInt32TooPreciseForFloat = kInt32TooPreciseForFloat;
const int64_t kInt64TooPreciseForFloat = kInt32TooPreciseForFloat;
const int64_t kInt64TooPreciseForDouble =
    static_cast<int64_t>(std::ldexp(1, kDoubleMantissa + 1)) + 1;
const uint64_t kUInt64TooPreciseForFloat = kInt32TooPreciseForFloat;
const uint64_t kUInt64TooPreciseForDouble = kInt64TooPreciseForDouble;

}  // namespace

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4756)  // warning C4756: overflow in constant arithmetic
#endif

TEST(RepresentAs, Int32ToDouble) {
    ASSERT(*(representAs<double>(kInt32Zero)) == 0);
    ASSERT(*(representAs<double>(5)) == 5);
}

TEST(RepresentAs, Int64ToDouble) {
    ASSERT(*(representAs<double>(kInt64Zero)) == 0);
    ASSERT(*(representAs<double>(5)) == 5);

    // kInt64Max is too precise for double
    ASSERT(!(representAs<double>(kInt64Max)));
    ASSERT(*(representAs<double>(kInt64Min)) == kInt64MinAsDouble);
}

TEST(RepresentAs, DoubleToInt32) {
    ASSERT(*(representAs<int32_t>(kDoubleZero)) == 0);
    ASSERT(*(representAs<int32_t>(-12345)) == -12345);
    ASSERT(!(representAs<int32_t>(10.3)));

    // Int32 edge cases
    ASSERT(*(representAs<int32_t>(kInt32Max)) == kInt32Max);
    ASSERT(!(representAs<int32_t>(kInt32MaxAsDouble + 1)));
    ASSERT(*(representAs<int32_t>(kInt32Min)) == kInt32Min);
    ASSERT(!(representAs<int32_t>(kInt32MinAsDouble - 1)));

    // Very large and small values
    ASSERT(!(representAs<int32_t>(kDoubleMax)));
    ASSERT(!(representAs<int32_t>(kDoubleMin)));
}

TEST(RepresentAs, DoubleToInt64) {
    ASSERT(*(representAs<int64_t>(kDoubleZero)) == 0);
    ASSERT(*(representAs<int64_t>(-12345)) == -12345);
    ASSERT(!(representAs<int64_t>(10.3)));

    // Int64 edge cases, max can't be represented as doubles, min can
    ASSERT(!(representAs<int64_t>(kInt64MaxAsDouble)));
    ASSERT(*(representAs<int64_t>(kInt64MinAsDouble)) == kInt64Min);

    // Very large and small values
    ASSERT(!(representAs<int64_t>(kDoubleMax)));
    ASSERT(!(representAs<int64_t>(kDoubleMin)));
}

TEST(RepresentAs, DoubleToFloat) {
    ASSERT(*(representAs<float>(kDoubleZero)) == 0);
    ASSERT(*(representAs<float>(-12345)) == -12345);

    // Float edge casees
    ASSERT(*(representAs<float>(kFloatMax)) == (representAs<float>(kFloatMaxAsDouble + 1)));
    ASSERT(*(representAs<float>(kFloatMin)) == (representAs<float>(kFloatMinAsDouble - 1)));

    // Very large and small values
    ASSERT(!(representAs<float>(kDoubleMax)));
    ASSERT(!(representAs<float>(kDoubleMin)));
}

TEST(RepresentAs, DoubleToUnsignedInt) {
    ASSERT(!(representAs<uint32_t>(-1.23)));
    ASSERT(*(representAs<uint64_t>(kDoubleZero)) == kUInt64Zero);
    ASSERT(!(representAs<uint32_t>(kDoubleMax)));
    ASSERT(!(representAs<uint64_t>(kDoubleMax)));
}

TEST(RepresentAs, FloatToDouble) {
    ASSERT(*(representAs<double>(kFloatZero)) == 0);
    ASSERT(*(representAs<double>(-12345)) == -12345);

    ASSERT(*(representAs<double>(kFloatMax)) == kFloatMax);
    ASSERT(*(representAs<double>(kFloatMin)) == kFloatMin);
}

TEST(RepresentAs, FloatToUnsignedInt) {
    ASSERT(!(representAs<uint32_t>(-1.23)));
    ASSERT(!(representAs<uint32_t>(-1)));
    ASSERT(*(representAs<uint64_t>(kUInt64Zero)) == kUInt64Zero);
    ASSERT(*(representAs<uint64_t>(10)) == static_cast<uint64_t>(10));
    ASSERT(!(representAs<uint32_t>(kFloatMax)));
    ASSERT(!(representAs<uint64_t>(kFloatMax)));
}

TEST(RepresentAs, SignedAndUnsigned32BitIntegers) {
    ASSERT(!(representAs<uint32_t>(kInt32Min)));
    ASSERT(*(representAs<uint32_t>(kInt32Max)) == kInt32MaxAsUInt32);

    ASSERT(!(representAs<int32_t>(kUInt32Max)));
    ASSERT(!(representAs<int32_t>(kInt32MaxAsUInt32 + 1)));
}

TEST(RepresentAs, SignedAndUnsigned64BitIntegers) {
    ASSERT(!(representAs<uint64_t>(kInt64Min)));
    ASSERT(*(representAs<uint64_t>(kInt64Max)) == kInt64MaxAsUInt64);

    ASSERT(!(representAs<int64_t>(kUInt64Max)));
    ASSERT(!(representAs<int64_t>(kInt64MaxAsUInt64 + 1)));
}

TEST(RepresentAs, SignedAndUnsignedMixedSizeIntegers) {
    ASSERT(!(representAs<uint32_t>(kInt64Min)));
    ASSERT(!(representAs<uint32_t>(kInt64Max)));
    ASSERT(*(representAs<int64_t>(kUInt32Max)) == kUInt32MaxAsInt64);

    ASSERT(!(representAs<uint64_t>(kInt32Min)));
    ASSERT(*(representAs<uint64_t>(kInt32Max)) == kInt32MaxAsUInt64);
    ASSERT(!(representAs<int32_t>(kUInt64Max)));
}

TEST(RepresentAs, UnsignedIntToFloat) {
    // kUInt32Max and kUInt64Max are too precise for float.
    ASSERT(!(representAs<float>(kUInt32Max)));
    ASSERT(!(representAs<float>(kUInt64Max)));
}

TEST(RepresentAs, UnsignedIntToDouble) {
    // kUInt64Max is too precise for double.
    ASSERT(*(representAs<double>(kUInt32Max)) == kUInt32MaxAsDouble);
    ASSERT(!(representAs<double>(kUInt64Max)));
}

template <typename Number>
void decimal128ToNumber() {
    const auto floor = std::to_string(std::numeric_limits<Number>::lowest());
    const auto ceiling = std::to_string(std::numeric_limits<Number>::max());

    ASSERT_EQ(*representAs<Number>(Decimal128(floor)), std::numeric_limits<Number>::lowest());
    ASSERT_EQ(*representAs<Number>(Decimal128(ceiling)), std::numeric_limits<Number>::max());

    ASSERT_EQ(*representAs<Number>(Decimal128::kNormalizedZero), static_cast<Number>(0));
    ASSERT_EQ(*representAs<Number>(Decimal128("5")), static_cast<Number>(5));

    ASSERT(!representAs<Number>(Decimal128::kLargestPositive));
    ASSERT(!representAs<Number>(Decimal128::kLargestNegative));
}

template <typename Float>
void decimal128ToFloatingPoint() {
    decimal128ToNumber<Float>();
    ASSERT_EQ(*representAs<Float>(Decimal128("-5")), -5);
    ASSERT_EQ(*representAs<Float>(Decimal128("5.5")), 5.5);
    ASSERT_EQ(*representAs<Float>(Decimal128("-5.5")), -5.5);
}

TEST(RepresentAs, Decimal128ToFloat) {
    decimal128ToFloatingPoint<float>();
}

TEST(RepresentAs, Decimal128ToDouble) {
    decimal128ToFloatingPoint<double>();
}

template <typename Integer>
void decimal128ToInteger() {
    decimal128ToNumber<Integer>();
    ASSERT(!representAs<Integer>(Decimal128("5.5")));
    ASSERT(!representAs<Integer>(Decimal128("-5.5")));

    ASSERT(!representAs<Integer>(Decimal128::kLargestPositive));
    ASSERT(!representAs<Integer>(Decimal128::kLargestNegative));
    ASSERT(!representAs<Integer>(Decimal128::kSmallestPositive));
    ASSERT(!representAs<Integer>(Decimal128::kSmallestNegative));

    if constexpr (std::is_signed<Integer>()) {
        ASSERT_EQ(*representAs<Integer>(Decimal128("-5")), -5);
    } else {
        ASSERT(!representAs<Integer>(Decimal128("-5")));
    }
}

TEST(RepresentAs, Decimal128ToInt8) {
    decimal128ToInteger<int8_t>();
}

TEST(RepresentAs, Decimal128ToUInt8) {
    decimal128ToInteger<uint8_t>();
}

TEST(RepresentAs, Decimal128ToInt16) {
    decimal128ToInteger<int16_t>();
}

TEST(RepresentAs, Decimal128ToUInt16) {
    decimal128ToInteger<uint16_t>();
}

TEST(RepresentAs, Decimal128ToInt32) {
    decimal128ToInteger<int32_t>();
}

TEST(RepresentAs, Decimal128ToUInt32) {
    decimal128ToInteger<uint32_t>();
}

TEST(RepresentAs, Decimal128ToInt64) {
    decimal128ToInteger<int64_t>();
}

TEST(RepresentAs, Decimal128ToUInt64) {
    decimal128ToInteger<uint64_t>();
}

TEST(RepresentAs, Decimal128ToDecimal128) {
    Decimal128 decimals[] = {Decimal128::kNormalizedZero,
                             Decimal128::kLargestPositive,
                             Decimal128::kSmallestPositive,
                             Decimal128::kLargestNegative,
                             Decimal128::kSmallestNegative,
                             Decimal128(5),
                             Decimal128(5.5),
                             Decimal128(-5),
                             Decimal128(-5.5)};

    for (const auto& d : decimals) {
        ASSERT(representAs<Decimal128>(d)->isEqual(d));
    }
}

template <typename Integer>
void integerToDecimal128() {
    std::vector<Integer> v{
        Integer{5}, std::numeric_limits<Integer>::lowest(), std::numeric_limits<Integer>::max()};

    if constexpr (std::is_signed_v<Integer>) {
        v.emplace_back(-5);
    }

    for (const Integer n : v) {
        auto d = representAs<Decimal128>(n);
        ASSERT(d);
        if (!d->isEqual(Decimal128(std::to_string(n)))) {
            FAIL(
                "Failed expectation, representAs<Decimal128>({}) == Decimal128({}),"
                " but !Decimal128({}).isEqual(Decimal128(std::to_string({}))"_format(
                    n, d->toString(), d->toString(), n));
        }
    }
}

TEST(RepresentAs, Int8ToDecimal128) {
    integerToDecimal128<int8_t>();
}

TEST(RepresentAs, UInt8ToDecimal128) {
    integerToDecimal128<uint8_t>();
}

TEST(RepresentAs, Int16ToDecimal128) {
    integerToDecimal128<int16_t>();
}

TEST(RepresentAs, UInt16ToDecimal128) {
    integerToDecimal128<uint16_t>();
}

TEST(RepresentAs, Int32ToDecimal128) {
    integerToDecimal128<int32_t>();
}

TEST(RepresentAs, UInt32ToDecimal128) {
    integerToDecimal128<uint32_t>();
}

TEST(RepresentAs, Int64ToDecimal128) {
    integerToDecimal128<int64_t>();
}

TEST(RepresentAs, UInt64ToDecimal128) {
    integerToDecimal128<uint64_t>();
}

template <typename Float>
void floatToDecimal128() {
    Float x = 5.5;
    ASSERT(representAs<Decimal128>(x)->isEqual(Decimal128(std::to_string(x))));
    ASSERT(representAs<Decimal128>(-x)->isEqual(Decimal128(std::to_string(-x))));

    // Assert extreme numbers can be represented as Decimal128, and one basic property of each.
    using limits = std::numeric_limits<Float>;
    ASSERT(representAs<Decimal128>(limits::lowest())->isLess(Decimal128(0)));
    ASSERT(representAs<Decimal128>(limits::min())->isGreater(Decimal128(0)));
    ASSERT(representAs<Decimal128>(limits::denorm_min())->isGreater(Decimal128(0)));
    ASSERT(representAs<Decimal128>(limits::max())->isGreater(Decimal128(0)));
    ASSERT(representAs<Decimal128>(limits::quiet_NaN())->isNaN());
    ASSERT(representAs<Decimal128>(-limits::quiet_NaN())->isNaN());
    ASSERT(representAs<Decimal128>(limits::infinity())->isInfinite());
    ASSERT(representAs<Decimal128>(-limits::infinity())->isInfinite());
    ASSERT(representAs<Decimal128>(Float{0})->isEqual(Decimal128(0)));
    ASSERT(representAs<Decimal128>(Float{-0})->isEqual(Decimal128(0)));
}

TEST(RepresentAs, FloatToDecimal128) {
    floatToDecimal128<float>();
}

TEST(RepresentAs, DoubleToDecimal128) {
    floatToDecimal128<double>();
}

TEST(RepresentAs, PlatformDependent) {
    // signed char
    ASSERT(*(representAs<int>(kCharMax)) == kCharMaxAsInt);
    ASSERT(!(representAs<signed char>(kIntMax)));

    // unsigned char
    ASSERT(*(representAs<int>(kUCharMax)) == kUCharMaxAsInt);
    ASSERT(!(representAs<unsigned char>(kIntMin)));

    // long long
    ASSERT(!(representAs<int>(kLongLongMax)));
    ASSERT(*(representAs<long long>(kIntMin)) == kIntMinAsLongLong);

    // unsigned long long
    ASSERT(!(representAs<int>(kULongLongMax)));
    ASSERT(*(representAs<unsigned long long>(kIntMax)) == kIntMaxAsULongLong);
}

TEST(RepresentAs, NaN) {
    ASSERT(!(representAs<int>(std::nanf("1"))));
    ASSERT(!(representAs<unsigned int>(std::nanf("1"))));

    // NaN Identities
    ASSERT(std::isnan(*representAs<float, float>(std::nanf("1"))));
    ASSERT(std::isnan(*representAs<double>(std::nanf("1"))));
    ASSERT(std::isnan(*representAs<float>(std::nan("1"))));
    ASSERT(std::isnan(*representAs<double>(std::nan("1"))));
}

TEST(RepresentAs, LostPrecision) {
    // A loss of precision should result in a disengaged optional
    ASSERT(!(representAs<float>(kInt32TooPreciseForFloat)));
    ASSERT(!(representAs<float>(kUInt32TooPreciseForFloat)));
    ASSERT(!(representAs<float>(kInt64TooPreciseForFloat)));
    ASSERT(!(representAs<float>(kUInt64TooPreciseForFloat)));

    ASSERT(!(representAs<double>(kInt64TooPreciseForDouble)));
    ASSERT(!(representAs<double>(kUInt64TooPreciseForDouble)));
}

TEST(RepresentAs, Identity) {
    ASSERT(*(representAs<int32_t>(kInt32Max)) == kInt32Max);
    ASSERT(*(representAs<int64_t>(kInt64Max)) == kInt64Max);
    ASSERT(*(representAs<long long>(50)) == 50);
    ASSERT(*(representAs<float>(kFloatMin)) == kFloatMin);
    ASSERT(*(representAs<double>(kDoubleMax)) == kDoubleMax);
    ASSERT(*(representAs<uint32_t>(kUInt32Max)) == kUInt32Max);
    ASSERT(*(representAs<uint64_t>(kUInt64Max)) == kUInt64Max);
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace mongo

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

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

namespace {
using namespace mongo;

TEST(BSONObjToString, EmptyArray) {
    const char text[] = "{ x: [] }";
    mongo::BSONObj o1 = mongo::fromjson(text);
    const std::string o1_str = o1.toString();
    ASSERT_EQUALS(text, o1_str);
}

TEST(BSONObjCompare, Timestamp) {
    ASSERT_BSONOBJ_LT(BSON("" << Timestamp(0, 3)), BSON("" << Timestamp(~0U, 2)));
    ASSERT_BSONOBJ_GT(BSON("" << Timestamp(2, 3)), BSON("" << Timestamp(2, 2)));
    ASSERT_BSONOBJ_EQ(BSON("" << Timestamp(3ULL)), BSON("" << Timestamp(0, 3)));
}

TEST(BSONObjCompare, NumberDouble) {
    ASSERT_BSONOBJ_LT(BSON("" << 0.0), BSON("" << 1.0));
    ASSERT_BSONOBJ_LT(BSON("" << -1.0), BSON("" << 0.0));
    ASSERT_BSONOBJ_LT(BSON("" << -1.0), BSON("" << 1.0));

    ASSERT_BSONOBJ_LT(BSON("" << 0.0), BSON("" << 0.1));
    ASSERT_BSONOBJ_LT(BSON("" << 0.1), BSON("" << 1.0));
    ASSERT_BSONOBJ_LT(BSON("" << -1.0), BSON("" << -0.1));
    ASSERT_BSONOBJ_LT(BSON("" << -0.1), BSON("" << 0.0));
    ASSERT_BSONOBJ_LT(BSON("" << -0.1), BSON("" << 0.1));

    ASSERT_BSONOBJ_LT(BSON("" << 0.0), BSON("" << std::numeric_limits<double>::denorm_min()));
    ASSERT_BSONOBJ_GT(BSON("" << 0.0), BSON("" << -std::numeric_limits<double>::denorm_min()));

    ASSERT_BSONOBJ_LT(BSON("" << 1.0), BSON("" << (1.0 + std::numeric_limits<double>::epsilon())));
    ASSERT_BSONOBJ_GT(BSON("" << -1.0),
                      BSON("" << (-1.0 - std::numeric_limits<double>::epsilon())));

    ASSERT_BSONOBJ_EQ(BSON("" << 0.0), BSON("" << -0.0));

    ASSERT_BSONOBJ_GT(BSON("" << std::numeric_limits<double>::infinity()), BSON("" << 0.0));
    ASSERT_BSONOBJ_GT(BSON("" << std::numeric_limits<double>::infinity()),
                      BSON("" << std::numeric_limits<double>::max()));  // max is finite
    ASSERT_BSONOBJ_GT(BSON("" << std::numeric_limits<double>::infinity()),
                      BSON("" << -std::numeric_limits<double>::infinity()));

    ASSERT_BSONOBJ_LT(BSON("" << -std::numeric_limits<double>::infinity()), BSON("" << 0.0));
    ASSERT_BSONOBJ_LT(BSON("" << -std::numeric_limits<double>::infinity()),
                      BSON("" << -std::numeric_limits<double>::max()));
    ASSERT_BSONOBJ_LT(BSON("" << -std::numeric_limits<double>::infinity()),
                      BSON("" << std::numeric_limits<double>::infinity()));

    ASSERT_BSONOBJ_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()), BSON("" << 0.0));
    ASSERT_BSONOBJ_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
                      BSON("" << -std::numeric_limits<double>::max()));
    ASSERT_BSONOBJ_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
                      BSON("" << std::numeric_limits<double>::infinity()));
    ASSERT_BSONOBJ_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
                      BSON("" << -std::numeric_limits<double>::infinity()));

    // TODO in C++11 use hex floating point to test distinct NaN representations
    ASSERT_BSONOBJ_EQ(BSON("" << std::numeric_limits<double>::quiet_NaN()),
                      BSON("" << std::numeric_limits<double>::signaling_NaN()));
}

TEST(BSONObjCompare, NumberLong_Double) {
    ASSERT_BSONOBJ_EQ(BSON("" << 0ll), BSON("" << 0.0));
    ASSERT_BSONOBJ_EQ(BSON("" << 0ll), BSON("" << -0.0));

    ASSERT_BSONOBJ_EQ(BSON("" << 1ll), BSON("" << 1.0));
    ASSERT_BSONOBJ_EQ(BSON("" << -1ll), BSON("" << -1.0));

    ASSERT_BSONOBJ_LT(BSON("" << 0ll), BSON("" << 1.0));
    ASSERT_BSONOBJ_LT(BSON("" << -1ll), BSON("" << 0.0));
    ASSERT_BSONOBJ_LT(BSON("" << -1ll), BSON("" << 1.0));

    ASSERT_BSONOBJ_LT(BSON("" << 0ll), BSON("" << 0.1));
    ASSERT_BSONOBJ_LT(BSON("" << 0.1), BSON("" << 1ll));
    ASSERT_BSONOBJ_LT(BSON("" << -1ll), BSON("" << -0.1));
    ASSERT_BSONOBJ_LT(BSON("" << -0.1), BSON("" << 0ll));

    ASSERT_BSONOBJ_LT(BSON("" << 0ll), BSON("" << std::numeric_limits<double>::denorm_min()));
    ASSERT_BSONOBJ_GT(BSON("" << 0ll), BSON("" << -std::numeric_limits<double>::denorm_min()));

    ASSERT_BSONOBJ_LT(BSON("" << 1ll), BSON("" << (1.0 + std::numeric_limits<double>::epsilon())));
    ASSERT_BSONOBJ_GT(BSON("" << -1ll),
                      BSON("" << (-1.0 - std::numeric_limits<double>::epsilon())));

    ASSERT_BSONOBJ_GT(BSON("" << std::numeric_limits<double>::infinity()), BSON("" << 0ll));
    ASSERT_BSONOBJ_GT(BSON("" << std::numeric_limits<double>::infinity()),
                      BSON("" << std::numeric_limits<long long>::max()));
    ASSERT_BSONOBJ_GT(BSON("" << std::numeric_limits<double>::infinity()),
                      BSON("" << std::numeric_limits<long long>::min()));

    ASSERT_BSONOBJ_LT(BSON("" << -std::numeric_limits<double>::infinity()), BSON("" << 0ll));
    ASSERT_BSONOBJ_LT(BSON("" << -std::numeric_limits<double>::infinity()),
                      BSON("" << std::numeric_limits<long long>::max()));
    ASSERT_BSONOBJ_LT(BSON("" << -std::numeric_limits<double>::infinity()),
                      BSON("" << std::numeric_limits<long long>::min()));

    ASSERT_BSONOBJ_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()), BSON("" << 0ll));
    ASSERT_BSONOBJ_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
                      BSON("" << std::numeric_limits<long long>::min()));

    for (int powerOfTwo = 0; powerOfTwo < 63; powerOfTwo++) {
        const long long lNum = 1ll << powerOfTwo;
        const double dNum = double(lNum);

        // All powers of two in this range can be represented exactly as doubles.
        invariant(lNum == static_cast<long long>(dNum));

        ASSERT_BSONOBJ_EQ(BSON("" << lNum), BSON("" << dNum));
        ASSERT_BSONOBJ_EQ(BSON("" << -lNum), BSON("" << -dNum));

        ASSERT_BSONOBJ_GT(BSON("" << (lNum + 1)), BSON("" << dNum));
        ASSERT_BSONOBJ_LT(BSON("" << (lNum - 1)), BSON("" << dNum));
        ASSERT_BSONOBJ_GT(BSON("" << (-lNum + 1)), BSON("" << -dNum));
        ASSERT_BSONOBJ_LT(BSON("" << (-lNum - 1)), BSON("" << -dNum));

        if (powerOfTwo <= 52) {  // is dNum - 0.5 representable?
            ASSERT_BSONOBJ_GT(BSON("" << lNum), BSON("" << (dNum - 0.5)));
            ASSERT_BSONOBJ_LT(BSON("" << -lNum), BSON("" << -(dNum - 0.5)));
        }

        if (powerOfTwo <= 51) {  // is dNum + 0.5 representable?
            ASSERT_BSONOBJ_LT(BSON("" << lNum), BSON("" << (dNum + 0.5)));
            ASSERT_BSONOBJ_GT(BSON("" << -lNum), BSON("" << -(dNum + 0.5)));
        }
    }

    {
        // Numbers around +/- numeric_limits<long long>::max() which can't be represented
        // precisely as a double.
        const long long maxLL = std::numeric_limits<long long>::max();
        const double closestAbove = 9223372036854775808.0;  // 2**63
        const double closestBelow = 9223372036854774784.0;  // 2**63 - epsilon

        ASSERT_BSONOBJ_GT(BSON("" << maxLL), BSON("" << (maxLL - 1)));
        ASSERT_BSONOBJ_LT(BSON("" << maxLL), BSON("" << closestAbove));
        ASSERT_BSONOBJ_GT(BSON("" << maxLL), BSON("" << closestBelow));

        ASSERT_BSONOBJ_LT(BSON("" << -maxLL), BSON("" << -(maxLL - 1)));
        ASSERT_BSONOBJ_GT(BSON("" << -maxLL), BSON("" << -closestAbove));
        ASSERT_BSONOBJ_LT(BSON("" << -maxLL), BSON("" << -closestBelow));
    }

    {
        // Numbers around numeric_limits<long long>::min() which can be represented precisely as
        // a double, but not as a positive long long.
        const long long minLL = std::numeric_limits<long long>::min();
        const double closestBelow = -9223372036854777856.0;  // -2**63 - epsilon
        const double equal = -9223372036854775808.0;         // 2**63
        const double closestAbove = -9223372036854774784.0;  // -2**63 + epsilon

// VS2017 Doesn't like the tests below, even though we're using static_cast
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4056)  // warning C4056: overflow in floating-point constant arithmetic
#endif
        invariant(static_cast<double>(minLL) == equal);
        invariant(static_cast<long long>(equal) == minLL);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

        ASSERT_BSONOBJ_LT(BSON("" << minLL), BSON("" << (minLL + 1)));

        ASSERT_BSONOBJ_EQ(BSON("" << minLL), BSON("" << equal));
        ASSERT_BSONOBJ_LT(BSON("" << minLL), BSON("" << closestAbove));
        ASSERT_BSONOBJ_GT(BSON("" << minLL), BSON("" << closestBelow));
    }
}

TEST(BSONObjCompare, NumberDecimalScaleAndZero) {
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(0.0)), BSON("" << Decimal128(1.0)));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(-1.0)), BSON("" << Decimal128(0.0)));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(-1.0)), BSON("" << Decimal128(1.0)));

    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(0.0)), BSON("" << Decimal128(0.1)));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(0.1)), BSON("" << Decimal128(1.0)));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(-1.0)), BSON("" << Decimal128(-0.1)));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(-0.1)), BSON("" << Decimal128(-0.0)));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(-0.1)), BSON("" << Decimal128(0.1)));
}

TEST(BSONObjCompare, NumberDecimalMaxAndMins) {
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(0.0)), BSON("" << Decimal128::kSmallestPositive));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128(0.0)), BSON("" << Decimal128::kLargestNegative));

    // over 34 digits of precision so it should be equal
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(1.0)),
                      BSON("" << Decimal128(1.0).add(Decimal128::kSmallestPositive)));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(0.0)), BSON("" << Decimal128(-0.0)));

    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(0)), BSON("" << Decimal128(0)));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128::kSmallestPositive),
                      BSON("" << Decimal128::kSmallestPositive));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128::kLargestNegative),
                      BSON("" << Decimal128::kLargestNegative));
}

TEST(BSONObjCompare, NumberDecimalInfinity) {
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128::kPositiveInfinity), BSON("" << Decimal128(0.0)));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128::kPositiveInfinity),
                      BSON("" << Decimal128::kLargestPositive));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128::kPositiveInfinity),
                      BSON("" << Decimal128::kNegativeInfinity));

    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128::kPositiveInfinity),
                      BSON("" << Decimal128::kPositiveInfinity));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128::kNegativeInfinity),
                      BSON("" << Decimal128::kNegativeInfinity));

    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kNegativeInfinity), BSON("" << Decimal128(0.0)));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kNegativeInfinity),
                      BSON("" << Decimal128::kSmallestNegative));
}

TEST(BSONObjCompare, NumberDecimalPosNaN) {
    // +/-NaN is well ordered and compares smallest, so +NaN and -NaN should behave the same
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kPositiveNaN), BSON("" << 0.0));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kPositiveNaN),
                      BSON("" << Decimal128::kSmallestNegative));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kPositiveNaN),
                      BSON("" << Decimal128::kPositiveInfinity));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kPositiveNaN),
                      BSON("" << Decimal128::kNegativeInfinity));

    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128::kPositiveNaN), BSON("" << Decimal128::kNegativeNaN));
}

TEST(BSONObjCompare, NumberDecimalNegNan) {
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kNegativeNaN), BSON("" << 0.0));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kNegativeNaN),
                      BSON("" << Decimal128::kSmallestNegative));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kNegativeNaN),
                      BSON("" << Decimal128::kPositiveInfinity));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kNegativeNaN),
                      BSON("" << Decimal128::kNegativeInfinity));

    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128::kNegativeNaN), BSON("" << Decimal128::kPositiveNaN));
}

TEST(BSONObjCompare, NumberDecimalCompareInt) {
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(0.0)), BSON("" << 0));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(502.0)), BSON("" << 502));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(std::numeric_limits<int>::max())),
                      BSON("" << std::numeric_limits<int>::max()));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(-std::numeric_limits<int>::max())),
                      BSON("" << -std::numeric_limits<int>::max()));

    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kNegativeNaN),
                      BSON("" << -std::numeric_limits<int>::max()));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kPositiveNaN),
                      BSON("" << -std::numeric_limits<int>::max()));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kNegativeInfinity),
                      BSON("" << -std::numeric_limits<int>::max()));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128::kPositiveInfinity),
                      BSON("" << std::numeric_limits<int>::max()));

    ASSERT_BSONOBJ_GT(BSON("" << Decimal128(1.0)), BSON("" << 0));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(-1.0)), BSON("" << 0));
}

TEST(BSONObjCompare, NumberDecimalCompareLong) {
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(0.0)), BSON("" << 0ll));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(502.0)), BSON("" << 502ll));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(std::numeric_limits<int64_t>::max())),
                      BSON("" << std::numeric_limits<long long>::max()));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(-std::numeric_limits<int64_t>::max())),
                      BSON("" << -std::numeric_limits<long long>::max()));

    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kNegativeNaN),
                      BSON("" << -std::numeric_limits<long long>::max()));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kPositiveNaN),
                      BSON("" << -std::numeric_limits<long long>::max()));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128::kNegativeInfinity),
                      BSON("" << -std::numeric_limits<long long>::max()));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128::kPositiveInfinity),
                      BSON("" << std::numeric_limits<long long>::max()));

    ASSERT_BSONOBJ_GT(BSON("" << Decimal128(1.0)), BSON("" << 0ll));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(-1.0)), BSON("" << 0ll));
}

TEST(BSONObjCompare, NumberDecimalCompareDoubleExactRepresentations) {
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(0.0)), BSON("" << 0.0));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(1.0)), BSON("" << 1.0));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(-1.0)), BSON("" << -1.0));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128(0.125)), BSON("" << 0.125));

    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(0.0)), BSON("" << 0.125));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(-1.0)), BSON("" << -0.125));

    ASSERT_BSONOBJ_GT(BSON("" << Decimal128(1.0)), BSON("" << 0.125));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128(0.0)), BSON("" << -0.125));
}

TEST(BSONObjCompare, NumberDecimalCompareDoubleNoDoubleRepresentation) {
    // Double 0.1 should not compare the same as decimal 0.1. The standard
    // double constructor for decimal types quantizes at 15 places, but this
    // is not safe for a well ordered comparison because decimal(0.1) would
    // then compare equal to both double(0.10000000000000000555) and
    // double(0.999999999999999876). The following test cases check that
    // proper well ordering is applied to double and decimal comparisons.
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128("0.3")), BSON("" << 0.1));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128("0.1")), BSON("" << 0.3));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128("-0.3")), BSON("" << -0.1));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128("-0.1")), BSON("" << -0.3));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128("0.1")), BSON("" << 0.1));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128("0.3")), BSON("" << 0.3));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128("-0.1")), BSON("" << -0.1));
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128("-0.3")), BSON("" << -0.3));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128("0.5")), BSON("" << 0.5));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128("0.5000000000000000000000000000000001")),
                      BSON("" << 0.5));

    // Double 0.1 should compare well against significantly different decimals
    ASSERT_BSONOBJ_LT(BSON("" << Decimal128(0.0)), BSON("" << 0.1));
    ASSERT_BSONOBJ_GT(BSON("" << Decimal128(1.0)), BSON("" << 0.1));
}

TEST(BSONObjCompare, NumberDecimalCompareDoubleQuantize) {
    // These tests deal with doubles that get adjusted when converted to decimal.
    // The decimal type only will store a double's first 15 decimal digits of
    // precision (the most it can accurately express).
    Decimal128 roundedDoubleLargestPosValue("179769313486232E294");
    Decimal128 roundedDoubleOneAboveLargestPosValue("179769313486233E294");
    Decimal128 roundedDoubleLargestNegValue("-179769313486232E294");
    Decimal128 roundedDoubleOneAboveSmallestNegValue("-179769313486231E294");

    ASSERT_BSONOBJ_EQ(BSON("" << roundedDoubleLargestPosValue),
                      BSON("" << Decimal128(std::numeric_limits<double>::max())));
    ASSERT_BSONOBJ_EQ(BSON("" << roundedDoubleLargestNegValue),
                      BSON("" << Decimal128(-std::numeric_limits<double>::max())));

    ASSERT_BSONOBJ_GT(BSON("" << roundedDoubleOneAboveLargestPosValue),
                      BSON("" << Decimal128(std::numeric_limits<double>::max())));
    ASSERT_BSONOBJ_LT(BSON("" << roundedDoubleOneAboveSmallestNegValue),
                      BSON("" << Decimal128(-std::numeric_limits<double>::min())));
}

TEST(BSONObjCompare, NumberDecimalCompareDoubleInfinity) {
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128::kPositiveInfinity),
                      BSON("" << std::numeric_limits<double>::infinity()));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128::kNegativeInfinity),
                      BSON("" << -std::numeric_limits<double>::infinity()));
}

TEST(BSONObjCompare, NumberDecimalCompareDoubleNaN) {
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128::kPositiveNaN),
                      BSON("" << std::numeric_limits<double>::quiet_NaN()));
    ASSERT_BSONOBJ_EQ(BSON("" << Decimal128::kNegativeNaN),
                      BSON("" << -std::numeric_limits<double>::quiet_NaN()));
}

TEST(BSONObjCompare, StringSymbol) {
    BSONObj l, r;
    {
        BSONObjBuilder b;
        b.append("x", "eliot");
        l = b.obj();
    }
    {
        BSONObjBuilder b;
        b.appendSymbol("x", "eliot");
        r = b.obj();
    }

    ASSERT_EQ(l.woCompare(r), 0);
    ASSERT_EQ(r.woCompare(l), 0);
}

TEST(BSONObjCompare, StringOrder1) {
    BSONObjBuilder b;
    b.appendRegex("x", "foo");
    BSONObj o = b.done();

    BSONObjBuilder c;
    c.appendRegex("x", "goo");
    BSONObj p = c.done();

    ASSERT(!o.binaryEqual(p));
    ASSERT_LT(o.woCompare(p), 0);
}

TEST(BSONObjCompare, StringOrder2) {
    BSONObj x, y, z;
    {
        BSONObjBuilder b;
        b.append("x", (long long)2);
        x = b.obj();
    }
    {
        BSONObjBuilder b;
        b.append("x", (int)3);
        y = b.obj();
    }
    {
        BSONObjBuilder b;
        b.append("x", (long long)4);
        z = b.obj();
    }

    ASSERT_LT(x.woCompare(y), 0);
    ASSERT_LT(x.woCompare(z), 0);
    ASSERT_GT(y.woCompare(x), 0);
    ASSERT_GT(z.woCompare(x), 0);
    ASSERT_LT(y.woCompare(z), 0);
    ASSERT_GT(z.woCompare(y), 0);
}

TEST(BSONObjCompare, StringOrder3) {
    BSONObj ll, d, i, n, u;
    {
        BSONObjBuilder b;
        b.append("x", (long long)2);
        ll = b.obj();
    }
    {
        BSONObjBuilder b;
        b.append("x", (double)2);
        d = b.obj();
    }
    {
        BSONObjBuilder b;
        b.append("x", (int)2);
        i = b.obj();
    }
    {
        BSONObjBuilder b;
        b.appendNull("x");
        n = b.obj();
    }
    {
        BSONObjBuilder b;
        u = b.obj();
    }

    ASSERT_TRUE(ll.woCompare(u) == d.woCompare(u));
    ASSERT_TRUE(ll.woCompare(u) == i.woCompare(u));

    BSONObj k = BSON("x" << 1);
    ASSERT_TRUE(ll.woCompare(u, k) == d.woCompare(u, k));
    ASSERT_TRUE(ll.woCompare(u, k) == i.woCompare(u, k));

    ASSERT_TRUE(u.woCompare(ll) == u.woCompare(d));
    ASSERT_TRUE(u.woCompare(ll) == u.woCompare(i));
    ASSERT_TRUE(u.woCompare(ll, k) == u.woCompare(d, k));
    ASSERT_TRUE(u.woCompare(ll, k) == u.woCompare(d, k));

    ASSERT_TRUE(i.woCompare(n) == d.woCompare(n));

    ASSERT_TRUE(ll.woCompare(n) == d.woCompare(n));
    ASSERT_TRUE(ll.woCompare(n) == i.woCompare(n));
    ASSERT_TRUE(ll.woCompare(n, k) == d.woCompare(n, k));
    ASSERT_TRUE(ll.woCompare(n, k) == i.woCompare(n, k));

    ASSERT_TRUE(n.woCompare(ll) == n.woCompare(d));
    ASSERT_TRUE(n.woCompare(ll) == n.woCompare(i));
    ASSERT_TRUE(n.woCompare(ll, k) == n.woCompare(d, k));
    ASSERT_TRUE(n.woCompare(ll, k) == n.woCompare(d, k));
}

TEST(BSONObjCompare, NumericBounds) {
    BSONObj l, r;
    {
        BSONObjBuilder b;
        b.append("x", std::numeric_limits<long long>::max());
        l = b.obj();
    }
    {
        BSONObjBuilder b;
        b.append("x", std::numeric_limits<double>::max());
        r = b.obj();
    }

    ASSERT_LT(l.woCompare(r), 0);
    ASSERT_GT(r.woCompare(l), 0);

    {
        BSONObjBuilder b;
        b.append("x", std::numeric_limits<int>::max());
        l = b.obj();
    }

    ASSERT_LT(l.woCompare(r), 0);
    ASSERT_GT(r.woCompare(l), 0);
}

TEST(BSONObjCompare, BSONObjHashingIgnoresTopLevelFieldNamesWhenRequested) {
    BSONObj obj1 = fromjson("{a: {b: 1}, c: {d: 1}}");
    BSONObj obj2 = fromjson("{A: {b: 1}, C: {d: 1}}");
    BSONObj obj3 = fromjson("{A: {B: 1}, C: {D: 1}}");

    SimpleBSONObjComparator bsonCmpConsiderFieldNames;
    BSONObjComparator bsonCmpIgnoreFieldNames(
        BSONObj(), BSONObjComparator::FieldNamesMode::kIgnore, nullptr);

    ASSERT_NE(bsonCmpConsiderFieldNames.hash(obj1), bsonCmpConsiderFieldNames.hash(obj2));
    ASSERT_EQ(bsonCmpIgnoreFieldNames.hash(obj1), bsonCmpIgnoreFieldNames.hash(obj2));
    ASSERT_NE(bsonCmpIgnoreFieldNames.hash(obj1), bsonCmpIgnoreFieldNames.hash(obj3));
    ASSERT_NE(bsonCmpIgnoreFieldNames.hash(fromjson("{a: {b: 1, c: 1}, d: 1}")),
              bsonCmpIgnoreFieldNames.hash(fromjson("{a: {b: 1, c: 2}, d: 1}")));
}

TEST(BSONObjCompare, BSONElementHashingIgnoresEltFieldNameWhenRequested) {
    BSONObj obj1 = fromjson("{a: {b: 1}}");
    BSONObj obj2 = fromjson("{A: {b: 1}}");
    BSONObj obj3 = fromjson("{A: {B: 1}}");

    SimpleBSONElementComparator bsonCmpConsiderFieldNames;
    BSONElementComparator bsonCmpIgnoreFieldNames(BSONElementComparator::FieldNamesMode::kIgnore,
                                                  nullptr);

    ASSERT_NE(bsonCmpConsiderFieldNames.hash(obj1.firstElement()),
              bsonCmpConsiderFieldNames.hash(obj2.firstElement()));
    ASSERT_EQ(bsonCmpIgnoreFieldNames.hash(obj1.firstElement()),
              bsonCmpIgnoreFieldNames.hash(obj2.firstElement()));
    ASSERT_NE(bsonCmpIgnoreFieldNames.hash(obj1.firstElement()),
              bsonCmpIgnoreFieldNames.hash(obj3.firstElement()));
}

TEST(BSONObjCompare, WoCompareWithIdxKey) {
    BSONObj obj = fromjson("{a: 1, b: 1, c: 1}");
    BSONObj objEq = fromjson("{a: 1, b: 1, c: 1}");
    BSONObj objGt = fromjson("{a: 2, b: 2, c: 2}");
    BSONObj objLt = fromjson("{a: 0, b: 0, c: 0}");
    BSONObj idxKeyAsc = fromjson("{a: 1, b: 1}");
    BSONObj idxKeyDesc = fromjson("{a: -1, b: 1}");
    BSONObj idxKeyShort = fromjson("{a: 1}");

    ASSERT_EQ(obj.woCompare(objEq, idxKeyAsc), 0);
    ASSERT_EQ(obj.woCompare(objEq, idxKeyDesc), 0);
    ASSERT_EQ(obj.woCompare(objEq, idxKeyShort), 0);
    ASSERT_EQ(obj.woCompare(objGt, idxKeyAsc), -1);
    ASSERT_EQ(obj.woCompare(objGt, idxKeyDesc), 1);
    ASSERT_EQ(obj.woCompare(objGt, idxKeyShort), -1);
    ASSERT_EQ(obj.woCompare(objLt, idxKeyAsc), 1);
    ASSERT_EQ(obj.woCompare(objLt, idxKeyDesc), -1);
    ASSERT_EQ(obj.woCompare(objLt, idxKeyShort), 1);
}

TEST(BSONObjCompare, UnorderedFieldsBSONObjComparison) {
    BSONObj obj = fromjson("{a: {b: 1}, c: 1}");

    UnorderedFieldsBSONObjComparator bsonCmp;

    ASSERT_TRUE(bsonCmp.evaluate(obj == fromjson("{c: 1, a: {b: 1}}")));
    ASSERT_FALSE(bsonCmp.evaluate(obj == fromjson("{a: {b: 1}, c: 1, d: 1}")));
    ASSERT_FALSE(bsonCmp.evaluate(obj == fromjson("{a: {b: 1}}")));
    ASSERT_FALSE(bsonCmp.evaluate(obj == fromjson("{a: {b: 2}, c: 1}")));
}

TEST(BSONObjCompare, UnorderedFieldsBSONObjHashing) {
    BSONObj obj = fromjson("{a: {b: 1, c: 1}, d: 1}");

    UnorderedFieldsBSONObjComparator bsonCmp;

    ASSERT_EQ(bsonCmp.hash(obj), bsonCmp.hash(obj));
    ASSERT_EQ(bsonCmp.hash(obj), bsonCmp.hash(fromjson("{d: 1, a: {b: 1, c: 1}}")));
    ASSERT_EQ(bsonCmp.hash(obj), bsonCmp.hash(fromjson("{a: {c: 1, b: 1}, d: 1}")));
    ASSERT_NE(bsonCmp.hash(obj), bsonCmp.hash(fromjson("{a: {b: 1, c: 1}}")));
    ASSERT_NE(bsonCmp.hash(obj), bsonCmp.hash(fromjson("{a: {b: 1, c: 1}, d: 2}")));
    ASSERT_NE(bsonCmp.hash(obj), bsonCmp.hash(fromjson("{a: {b: 1}, d: 1}")));
}

TEST(Looping, Cpp11Basic) {
    int count = 0;
    for (BSONElement e : BSON("a" << 1 << "a" << 2 << "a" << 3)) {
        ASSERT_EQUALS(e.fieldNameStringData(), "a");
        count += e.Int();
    }

    ASSERT_EQUALS(count, 1 + 2 + 3);
}

TEST(Looping, Cpp11Auto) {
    int count = 0;
    for (auto e : BSON("a" << 1 << "a" << 2 << "a" << 3)) {
        ASSERT_EQUALS(e.fieldNameStringData(), "a");
        count += e.Int();
    }

    ASSERT_EQUALS(count, 1 + 2 + 3);
}

TEST(Looping, Cpp17StructuredBindings) {
    int count = 0;
    for (auto [name, e] : BSON("a" << 1 << "a" << 2 << "a" << 3)) {
        ASSERT_EQUALS(name, "a");
        count += e.Int();
    }

    ASSERT_EQUALS(count, 1 + 2 + 3);
}

TEST(BSONObj, getFields) {
    auto e = BSON("a" << 1 << "b" << 2 << "c" << 3 << "d" << 4 << "e" << 5 << "f" << 6);
    std::array<StringData, 3> fieldNames{"c", "d", "f"};
    std::array<BSONElement, 3> fields;
    e.getFields(fieldNames, &fields);
    ASSERT_EQUALS(fields[0].type(), BSONType::NumberInt);
    ASSERT_EQUALS(fields[0].numberInt(), 3);
    ASSERT_EQUALS(fields[1].type(), BSONType::NumberInt);
    ASSERT_EQUALS(fields[1].numberInt(), 4);
    ASSERT_EQUALS(fields[2].type(), BSONType::NumberInt);
    ASSERT_EQUALS(fields[2].numberInt(), 6);
}

TEST(BSONObj, getFieldsWithDuplicates) {
    auto e = BSON("a" << 2 << "b"
                      << "3"
                      << "a" << 9 << "b" << 10);
    std::array<StringData, 2> fieldNames{"a", "b"};
    std::array<BSONElement, 2> fields;
    e.getFields(fieldNames, &fields);
    ASSERT_EQUALS(fields[0].type(), BSONType::NumberInt);
    ASSERT_EQUALS(fields[0].numberInt(), 2);
    ASSERT_EQUALS(fields[1].type(), BSONType::String);
    ASSERT_EQUALS(fields[1].str(), "3");
}

TEST(BSONObj, ShareOwnershipWith) {
    BSONObj obj;
    {
        BSONObj tmp = BSON("sub" << BSON("a" << 1));
        obj = tmp["sub"].Obj();
        obj.shareOwnershipWith(tmp);
        ASSERT(obj.isOwned());
    }

    // Now that tmp is out of scope, if obj didn't retain ownership, it would be accessing free'd
    // memory which should error on ASAN and debug builds.
    ASSERT(obj.isOwned());
    ASSERT_BSONOBJ_EQ(obj, BSON("a" << 1));
}

TEST(BSONObj, addField) {
    auto obj = BSON("a" << 1 << "b" << 2);

    // Check that replacing a field maintains the same ordering and doesn't add a field.
    auto objA2 = BSON("a" << 2);
    auto elemA2 = objA2.firstElement();
    auto addFieldA2 = obj.addField(elemA2);
    ASSERT_EQ(addFieldA2.nFields(), 2);
    ASSERT_BSONOBJ_EQ(addFieldA2, BSON("a" << 2 << "b" << 2));

    // Check that adding a new field places it at the end.
    auto objC3 = BSON("c" << 3);
    auto elemC3 = objC3.firstElement();
    auto addFieldC3 = obj.addField(elemC3);
    ASSERT_BSONOBJ_EQ(addFieldC3, BSON("a" << 1 << "b" << 2 << "c" << 3));

    // Check that after all this obj is unchanged.
    ASSERT_BSONOBJ_EQ(obj, BSON("a" << 1 << "b" << 2));
}

TEST(BSONObj, addFieldsWithoutSpecifyingFields) {
    // New fields are appended to the end in the order in which they appear in the 'from' object.
    auto obj = BSON("p" << 1 << "q" << 1);
    auto output = obj.addFields(BSON("a" << 2 << "b" << 2), boost::none);
    ASSERT_BSONOBJ_EQ(output, BSON("p" << 1 << "q" << 1 << "a" << 2 << "b" << 2));

    // Duplicate fields names are merged at original poistion.
    obj = BSON("p" << 1 << "q" << 1 << "a" << 1 << "b" << 1);
    output = obj.addFields(BSON("b" << 2 << "a" << BSON("a" << 2)), boost::none);
    ASSERT_BSONOBJ_EQ(output, BSON("p" << 1 << "q" << 1 << "a" << BSON("a" << 2) << "b" << 2));

    // New fields are appended to the end while duplicates are merged in place.
    obj = BSON("p" << 1 << "q" << 1 << "a" << BSON("a" << 1) << "b" << 1);
    output = obj.addFields(BSON("c" << 2 << "a" << 2), boost::none);
    ASSERT_BSONOBJ_EQ(output, BSON("p" << 1 << "q" << 1 << "a" << 2 << "b" << 1 << "c" << 2));

    // No fields added when the set is empty
    obj = BSON("p" << 1);
    output = obj.addFields(BSON("q" << 2), StringDataSet{});
    ASSERT_BSONOBJ_EQ(output, BSON("p" << 1));
}

TEST(BSONObj, addFields) {
    // Fields that are not present in the 'from' object are ignored.
    auto obj = BSON("p" << 1 << "q" << 1);
    auto output = obj.addFields(BSON("a" << 2 << "b" << BSON("b" << 2)), {{"b", "c"}});
    ASSERT_BSONOBJ_EQ(output, BSON("p" << 1 << "q" << 1 << "b" << BSON("b" << 2)));

    // Duplicate fields names are merged at original poistion.
    obj = BSON("p" << 2 << "q" << 2 << "b" << 2);
    output = obj.addFields(BSON("q" << 1 << "p" << BSON("p" << 1)), {{"q", "p", "b", "c"}});
    ASSERT_BSONOBJ_EQ(output, BSON("p" << BSON("p" << 1) << "q" << 1 << "b" << 2));

    // New fields are appended to the end, in the order in which they appear in the 'from'
    // object.
    obj = BSON("p" << 1 << "q" << 1 << "b" << BSON("a" << 1));
    output = obj.addFields(BSON("d" << 2 << "b" << 2 << "c" << 2), {{"b", "c", "d"}});
    ASSERT_BSONOBJ_EQ(output, BSON("p" << 1 << "q" << 1 << "b" << 2 << "d" << 2 << "c" << 2));
}

TEST(BSONObj, sizeChecks) {
    auto generateBuffer = [](std::int32_t size) {
        std::vector<char> buffer(size);
        DataRange bufferRange(&buffer.front(), &buffer.back());
        ASSERT_OK(bufferRange.writeNoThrow(LittleEndian<int32_t>(size)));

        return buffer;
    };

    {
        // Implicitly assert that BSONObj constructor does not throw
        // with standard size buffers.
        auto normalBuffer = generateBuffer(15 * 1024 * 1024);
        BSONObj obj(normalBuffer.data());
    }

    // Large buffers cause an exception to be thrown.
    ASSERT_THROWS_CODE(
        [&] {
            auto largeBuffer = generateBuffer(17 * 1024 * 1024);
            BSONObj obj(largeBuffer.data());
        }(),
        DBException,
        ErrorCodes::BSONObjectTooLarge);


    // Assert that the max size can be increased by passing BSONObj a tag type.
    {
        auto largeBuffer = generateBuffer(17 * 1024 * 1024);
        BSONObj obj(largeBuffer.data(), BSONObj::LargeSizeTrait{});
    }

    // But a size is in fact being enforced.
    ASSERT_THROWS_CODE(
        [&]() {
            auto hugeBuffer = generateBuffer(70 * 1024 * 1024);
            BSONObj obj(hugeBuffer.data(), BSONObj::LargeSizeTrait{});
        }(),
        DBException,
        ErrorCodes::BSONObjectTooLarge);
}

TEST(BSONObj, nullByteInStringBasic) {
    const size_t size = 3;
    StringData str("b\0c", size);

    // { "a": "b\0c" }
    BSONObjBuilder b;
    b.append("a"_sd, str);
    BSONObj obj{b.obj()};

    ASSERT_EQ(str.size(), obj.getStringField("a").size());
    ASSERT_EQ(str, obj.getStringField("a"));
}

TEST(BSONObj, nullByteInStringMulti) {
    const size_t size = 5;
    StringData str("b\0c\0d", size);

    // { "a": "b\0c\0d" }
    BSONObjBuilder b;
    b.append("a"_sd, str);
    BSONObj obj{b.obj()};

    ASSERT_EQ(str.size(), obj.getStringField("a").size());
    ASSERT_EQ(str, obj.getStringField("a"));
}

TEST(BSONObj, nullByteInStringFull) {
    const size_t size = 9;
    StringData str("\0\0\0\0\0\0\0\0\0", size);

    // { "a": "\0\0\0\0\0\0\0\0\0" }
    BSONObjBuilder b;
    b.append("a"_sd, str);
    BSONObj obj{b.obj()};

    ASSERT_EQ(str.size(), obj.getStringField("a").size());
    ASSERT_EQ(str, obj.getStringField("a"));
}

}  // unnamed namespace

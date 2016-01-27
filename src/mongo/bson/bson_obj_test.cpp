/*    Copyright 2013 10gen Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/platform/decimal128.h"

#include "mongo/unittest/unittest.h"

namespace {
using namespace mongo;

TEST(BSONObjToString, EmptyArray) {
    const char text[] = "{ x: [] }";
    mongo::BSONObj o1 = mongo::fromjson(text);
    const std::string o1_str = o1.toString();
    ASSERT_EQUALS(text, o1_str);
}

TEST(BSONObjCompare, Timestamp) {
    ASSERT_LT(BSON("" << Timestamp(0, 3)), BSON("" << Timestamp(~0U, 2)));
    ASSERT_GT(BSON("" << Timestamp(2, 3)), BSON("" << Timestamp(2, 2)));
    ASSERT_EQ(BSON("" << Timestamp(3ULL)), BSON("" << Timestamp(0, 3)));
}

TEST(BSONObjCompare, NumberDouble) {
    ASSERT_LT(BSON("" << 0.0), BSON("" << 1.0));
    ASSERT_LT(BSON("" << -1.0), BSON("" << 0.0));
    ASSERT_LT(BSON("" << -1.0), BSON("" << 1.0));

    ASSERT_LT(BSON("" << 0.0), BSON("" << 0.1));
    ASSERT_LT(BSON("" << 0.1), BSON("" << 1.0));
    ASSERT_LT(BSON("" << -1.0), BSON("" << -0.1));
    ASSERT_LT(BSON("" << -0.1), BSON("" << 0.0));
    ASSERT_LT(BSON("" << -0.1), BSON("" << 0.1));

    ASSERT_LT(BSON("" << 0.0), BSON("" << std::numeric_limits<double>::denorm_min()));
    ASSERT_GT(BSON("" << 0.0), BSON("" << -std::numeric_limits<double>::denorm_min()));

    ASSERT_LT(BSON("" << 1.0), BSON("" << (1.0 + std::numeric_limits<double>::epsilon())));
    ASSERT_GT(BSON("" << -1.0), BSON("" << (-1.0 - std::numeric_limits<double>::epsilon())));

    ASSERT_EQ(BSON("" << 0.0), BSON("" << -0.0));

    ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()), BSON("" << 0.0));
    ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()),
              BSON("" << std::numeric_limits<double>::max()));  // max is finite
    ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()),
              BSON("" << -std::numeric_limits<double>::infinity()));

    ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()), BSON("" << 0.0));
    ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()),
              BSON("" << -std::numeric_limits<double>::max()));
    ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()),
              BSON("" << std::numeric_limits<double>::infinity()));

    ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()), BSON("" << 0.0));
    ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
              BSON("" << -std::numeric_limits<double>::max()));
    ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
              BSON("" << std::numeric_limits<double>::infinity()));
    ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
              BSON("" << -std::numeric_limits<double>::infinity()));

    // TODO in C++11 use hex floating point to test distinct NaN representations
    ASSERT_EQ(BSON("" << std::numeric_limits<double>::quiet_NaN()),
              BSON("" << std::numeric_limits<double>::signaling_NaN()));
}

TEST(BSONObjCompare, NumberLong_Double) {
    ASSERT_EQ(BSON("" << 0ll), BSON("" << 0.0));
    ASSERT_EQ(BSON("" << 0ll), BSON("" << -0.0));

    ASSERT_EQ(BSON("" << 1ll), BSON("" << 1.0));
    ASSERT_EQ(BSON("" << -1ll), BSON("" << -1.0));

    ASSERT_LT(BSON("" << 0ll), BSON("" << 1.0));
    ASSERT_LT(BSON("" << -1ll), BSON("" << 0.0));
    ASSERT_LT(BSON("" << -1ll), BSON("" << 1.0));

    ASSERT_LT(BSON("" << 0ll), BSON("" << 0.1));
    ASSERT_LT(BSON("" << 0.1), BSON("" << 1ll));
    ASSERT_LT(BSON("" << -1ll), BSON("" << -0.1));
    ASSERT_LT(BSON("" << -0.1), BSON("" << 0ll));

    ASSERT_LT(BSON("" << 0ll), BSON("" << std::numeric_limits<double>::denorm_min()));
    ASSERT_GT(BSON("" << 0ll), BSON("" << -std::numeric_limits<double>::denorm_min()));

    ASSERT_LT(BSON("" << 1ll), BSON("" << (1.0 + std::numeric_limits<double>::epsilon())));
    ASSERT_GT(BSON("" << -1ll), BSON("" << (-1.0 - std::numeric_limits<double>::epsilon())));

    ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()), BSON("" << 0ll));
    ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()),
              BSON("" << std::numeric_limits<long long>::max()));
    ASSERT_GT(BSON("" << std::numeric_limits<double>::infinity()),
              BSON("" << std::numeric_limits<long long>::min()));

    ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()), BSON("" << 0ll));
    ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()),
              BSON("" << std::numeric_limits<long long>::max()));
    ASSERT_LT(BSON("" << -std::numeric_limits<double>::infinity()),
              BSON("" << std::numeric_limits<long long>::min()));

    ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()), BSON("" << 0ll));
    ASSERT_LT(BSON("" << std::numeric_limits<double>::quiet_NaN()),
              BSON("" << std::numeric_limits<long long>::min()));

    for (int powerOfTwo = 0; powerOfTwo < 63; powerOfTwo++) {
        const long long lNum = 1ll << powerOfTwo;
        const double dNum = double(lNum);

        // All powers of two in this range can be represented exactly as doubles.
        invariant(lNum == static_cast<long long>(dNum));

        ASSERT_EQ(BSON("" << lNum), BSON("" << dNum));
        ASSERT_EQ(BSON("" << -lNum), BSON("" << -dNum));

        ASSERT_GT(BSON("" << (lNum + 1)), BSON("" << dNum));
        ASSERT_LT(BSON("" << (lNum - 1)), BSON("" << dNum));
        ASSERT_GT(BSON("" << (-lNum + 1)), BSON("" << -dNum));
        ASSERT_LT(BSON("" << (-lNum - 1)), BSON("" << -dNum));

        if (powerOfTwo <= 52) {  // is dNum - 0.5 representable?
            ASSERT_GT(BSON("" << lNum), BSON("" << (dNum - 0.5)));
            ASSERT_LT(BSON("" << -lNum), BSON("" << -(dNum - 0.5)));
        }

        if (powerOfTwo <= 51) {  // is dNum + 0.5 representable?
            ASSERT_LT(BSON("" << lNum), BSON("" << (dNum + 0.5)));
            ASSERT_GT(BSON("" << -lNum), BSON("" << -(dNum + 0.5)));
        }
    }

    {
        // Numbers around +/- numeric_limits<long long>::max() which can't be represented
        // precisely as a double.
        const long long maxLL = std::numeric_limits<long long>::max();
        const double closestAbove = 9223372036854775808.0;  // 2**63
        const double closestBelow = 9223372036854774784.0;  // 2**63 - epsilon

        ASSERT_GT(BSON("" << maxLL), BSON("" << (maxLL - 1)));
        ASSERT_LT(BSON("" << maxLL), BSON("" << closestAbove));
        ASSERT_GT(BSON("" << maxLL), BSON("" << closestBelow));

        ASSERT_LT(BSON("" << -maxLL), BSON("" << -(maxLL - 1)));
        ASSERT_GT(BSON("" << -maxLL), BSON("" << -closestAbove));
        ASSERT_LT(BSON("" << -maxLL), BSON("" << -closestBelow));
    }

    {
        // Numbers around numeric_limits<long long>::min() which can be represented precisely as
        // a double, but not as a positive long long.
        const long long minLL = std::numeric_limits<long long>::min();
        const double closestBelow = -9223372036854777856.0;  // -2**63 - epsilon
        const double equal = -9223372036854775808.0;         // 2**63
        const double closestAbove = -9223372036854774784.0;  // -2**63 + epsilon

        invariant(static_cast<double>(minLL) == equal);
        invariant(static_cast<long long>(equal) == minLL);

        ASSERT_LT(BSON("" << minLL), BSON("" << (minLL + 1)));

        ASSERT_EQ(BSON("" << minLL), BSON("" << equal));
        ASSERT_LT(BSON("" << minLL), BSON("" << closestAbove));
        ASSERT_GT(BSON("" << minLL), BSON("" << closestBelow));
    }
}

TEST(BSONObjCompare, NumberDecimalScaleAndZero) {
    if (Decimal128::enabled) {
        ASSERT_LT(BSON("" << Decimal128(0.0)), BSON("" << Decimal128(1.0)));
        ASSERT_LT(BSON("" << Decimal128(-1.0)), BSON("" << Decimal128(0.0)));
        ASSERT_LT(BSON("" << Decimal128(-1.0)), BSON("" << Decimal128(1.0)));

        ASSERT_LT(BSON("" << Decimal128(0.0)), BSON("" << Decimal128(0.1)));
        ASSERT_LT(BSON("" << Decimal128(0.1)), BSON("" << Decimal128(1.0)));
        ASSERT_LT(BSON("" << Decimal128(-1.0)), BSON("" << Decimal128(-0.1)));
        ASSERT_LT(BSON("" << Decimal128(-0.1)), BSON("" << Decimal128(-0.0)));
        ASSERT_LT(BSON("" << Decimal128(-0.1)), BSON("" << Decimal128(0.1)));
    }
}

TEST(BSONObjCompare, NumberDecimalMaxAndMins) {
    if (Decimal128::enabled) {
        ASSERT_LT(BSON("" << Decimal128(0.0)), BSON("" << Decimal128::kSmallestPositive));
        ASSERT_GT(BSON("" << Decimal128(0.0)), BSON("" << Decimal128::kLargestNegative));

        // over 34 digits of precision so it should be equal
        ASSERT_EQ(BSON("" << Decimal128(1.0)),
                  BSON("" << Decimal128(1.0).add(Decimal128::kSmallestPositive)));
        ASSERT_EQ(BSON("" << Decimal128(0.0)), BSON("" << Decimal128(-0.0)));

        ASSERT_EQ(BSON("" << Decimal128(0)), BSON("" << Decimal128(0)));
        ASSERT_EQ(BSON("" << Decimal128::kSmallestPositive),
                  BSON("" << Decimal128::kSmallestPositive));
        ASSERT_EQ(BSON("" << Decimal128::kLargestNegative),
                  BSON("" << Decimal128::kLargestNegative));
    }
}

TEST(BSONObjCompare, NumberDecimalInfinity) {
    if (Decimal128::enabled) {
        ASSERT_GT(BSON("" << Decimal128::kPositiveInfinity), BSON("" << Decimal128(0.0)));
        ASSERT_GT(BSON("" << Decimal128::kPositiveInfinity),
                  BSON("" << Decimal128::kLargestPositive));
        ASSERT_GT(BSON("" << Decimal128::kPositiveInfinity),
                  BSON("" << Decimal128::kNegativeInfinity));

        ASSERT_EQ(BSON("" << Decimal128::kPositiveInfinity),
                  BSON("" << Decimal128::kPositiveInfinity));
        ASSERT_EQ(BSON("" << Decimal128::kNegativeInfinity),
                  BSON("" << Decimal128::kNegativeInfinity));

        ASSERT_LT(BSON("" << Decimal128::kNegativeInfinity), BSON("" << Decimal128(0.0)));
        ASSERT_LT(BSON("" << Decimal128::kNegativeInfinity),
                  BSON("" << Decimal128::kSmallestNegative));
    }
}

TEST(BSONObjCompare, NumberDecimalPosNaN) {
    if (Decimal128::enabled) {
        // +/-NaN is well ordered and compares smallest, so +NaN and -NaN should behave the same
        ASSERT_LT(BSON("" << Decimal128::kPositiveNaN), BSON("" << 0.0));
        ASSERT_LT(BSON("" << Decimal128::kPositiveNaN), BSON("" << Decimal128::kSmallestNegative));
        ASSERT_LT(BSON("" << Decimal128::kPositiveNaN), BSON("" << Decimal128::kPositiveInfinity));
        ASSERT_LT(BSON("" << Decimal128::kPositiveNaN), BSON("" << Decimal128::kNegativeInfinity));

        ASSERT_EQ(BSON("" << Decimal128::kPositiveNaN), BSON("" << Decimal128::kNegativeNaN));
    }
}

TEST(BSONObjCompare, NumberDecimalNegNan) {
    if (Decimal128::enabled) {
        ASSERT_LT(BSON("" << Decimal128::kNegativeNaN), BSON("" << 0.0));
        ASSERT_LT(BSON("" << Decimal128::kNegativeNaN), BSON("" << Decimal128::kSmallestNegative));
        ASSERT_LT(BSON("" << Decimal128::kNegativeNaN), BSON("" << Decimal128::kPositiveInfinity));
        ASSERT_LT(BSON("" << Decimal128::kNegativeNaN), BSON("" << Decimal128::kNegativeInfinity));

        ASSERT_EQ(BSON("" << Decimal128::kNegativeNaN), BSON("" << Decimal128::kPositiveNaN));
    }
}

TEST(BSONObjCompare, NumberDecimalCompareInt) {
    if (Decimal128::enabled) {
        ASSERT_EQ(BSON("" << Decimal128(0.0)), BSON("" << 0));
        ASSERT_EQ(BSON("" << Decimal128(502.0)), BSON("" << 502));
        ASSERT_EQ(BSON("" << Decimal128(std::numeric_limits<int>::max())),
                  BSON("" << std::numeric_limits<int>::max()));
        ASSERT_EQ(BSON("" << Decimal128(-std::numeric_limits<int>::max())),
                  BSON("" << -std::numeric_limits<int>::max()));

        ASSERT_LT(BSON("" << Decimal128::kNegativeNaN),
                  BSON("" << -std::numeric_limits<int>::max()));
        ASSERT_LT(BSON("" << Decimal128::kPositiveNaN),
                  BSON("" << -std::numeric_limits<int>::max()));
        ASSERT_LT(BSON("" << Decimal128::kNegativeInfinity),
                  BSON("" << -std::numeric_limits<int>::max()));
        ASSERT_GT(BSON("" << Decimal128::kPositiveInfinity),
                  BSON("" << std::numeric_limits<int>::max()));

        ASSERT_GT(BSON("" << Decimal128(1.0)), BSON("" << 0));
        ASSERT_LT(BSON("" << Decimal128(-1.0)), BSON("" << 0));
    }
}

TEST(BSONObjCompare, NumberDecimalCompareLong) {
    if (Decimal128::enabled) {
        ASSERT_EQ(BSON("" << Decimal128(0.0)), BSON("" << 0ll));
        ASSERT_EQ(BSON("" << Decimal128(502.0)), BSON("" << 502ll));
        ASSERT_EQ(BSON("" << Decimal128(std::numeric_limits<int64_t>::max())),
                  BSON("" << std::numeric_limits<long long>::max()));
        ASSERT_EQ(BSON("" << Decimal128(-std::numeric_limits<int64_t>::max())),
                  BSON("" << -std::numeric_limits<long long>::max()));

        ASSERT_LT(BSON("" << Decimal128::kNegativeNaN),
                  BSON("" << -std::numeric_limits<long long>::max()));
        ASSERT_LT(BSON("" << Decimal128::kPositiveNaN),
                  BSON("" << -std::numeric_limits<long long>::max()));
        ASSERT_LT(BSON("" << Decimal128::kNegativeInfinity),
                  BSON("" << -std::numeric_limits<long long>::max()));
        ASSERT_GT(BSON("" << Decimal128::kPositiveInfinity),
                  BSON("" << std::numeric_limits<long long>::max()));

        ASSERT_GT(BSON("" << Decimal128(1.0)), BSON("" << 0ll));
        ASSERT_LT(BSON("" << Decimal128(-1.0)), BSON("" << 0ll));
    }
}

TEST(BSONObjCompare, NumberDecimalCompareDoubleExactRepresentations) {
    if (Decimal128::enabled) {
        ASSERT_EQ(BSON("" << Decimal128(0.0)), BSON("" << 0.0));
        ASSERT_EQ(BSON("" << Decimal128(1.0)), BSON("" << 1.0));
        ASSERT_EQ(BSON("" << Decimal128(-1.0)), BSON("" << -1.0));
        ASSERT_EQ(BSON("" << Decimal128(0.125)), BSON("" << 0.125));

        ASSERT_LT(BSON("" << Decimal128(0.0)), BSON("" << 0.125));
        ASSERT_LT(BSON("" << Decimal128(-1.0)), BSON("" << -0.125));

        ASSERT_GT(BSON("" << Decimal128(1.0)), BSON("" << 0.125));
        ASSERT_GT(BSON("" << Decimal128(0.0)), BSON("" << -0.125));
    }
}

TEST(BSONObjCompare, NumberDecimalCompareDoubleNoDoubleRepresentation) {
    if (Decimal128::enabled) {
        // Double 0.1 should not compare the same as decimal 0.1. The standard
        // double constructor for decimal types quantizes at 15 places, but this
        // is not safe for a well ordered comparison because decimal(0.1) would
        // then compare equal to both double(0.10000000000000000555) and
        // double(0.999999999999999876). The following test cases check that
        // proper well ordering is applied to double and decimal comparisons.
        ASSERT_GT(BSON("" << Decimal128("0.3")), BSON("" << 0.1));
        ASSERT_LT(BSON("" << Decimal128("0.1")), BSON("" << 0.3));
        ASSERT_LT(BSON("" << Decimal128("-0.3")), BSON("" << -0.1));
        ASSERT_GT(BSON("" << Decimal128("-0.1")), BSON("" << -0.3));
        ASSERT_LT(BSON("" << Decimal128("0.1")), BSON("" << 0.1));
        ASSERT_GT(BSON("" << Decimal128("0.3")), BSON("" << 0.3));
        ASSERT_GT(BSON("" << Decimal128("-0.1")), BSON("" << -0.1));
        ASSERT_LT(BSON("" << Decimal128("-0.3")), BSON("" << -0.3));
        ASSERT_EQ(BSON("" << Decimal128("0.5")), BSON("" << 0.5));
        ASSERT_GT(BSON("" << Decimal128("0.5000000000000000000000000000000001")), BSON("" << 0.5));

        // Double 0.1 should compare well against significantly different decimals
        ASSERT_LT(BSON("" << Decimal128(0.0)), BSON("" << 0.1));
        ASSERT_GT(BSON("" << Decimal128(1.0)), BSON("" << 0.1));
    }
}

TEST(BSONObjCompare, NumberDecimalCompareDoubleQuantize) {
    if (Decimal128::enabled) {
        // These tests deal with doubles that get adjusted when converted to decimal.
        // The decimal type only will store a double's first 15 decimal digits of
        // precision (the most it can accurately express).
        Decimal128 roundedDoubleLargestPosValue("179769313486232E294");
        Decimal128 roundedDoubleOneAboveLargestPosValue("179769313486233E294");
        Decimal128 roundedDoubleLargestNegValue("-179769313486232E294");
        Decimal128 roundedDoubleOneAboveSmallestNegValue("-179769313486231E294");

        ASSERT_EQ(BSON("" << roundedDoubleLargestPosValue),
                  BSON("" << Decimal128(std::numeric_limits<double>::max())));
        ASSERT_EQ(BSON("" << roundedDoubleLargestNegValue),
                  BSON("" << Decimal128(-std::numeric_limits<double>::max())));

        ASSERT_GT(BSON("" << roundedDoubleOneAboveLargestPosValue),
                  BSON("" << Decimal128(std::numeric_limits<double>::max())));
        ASSERT_LT(BSON("" << roundedDoubleOneAboveSmallestNegValue),
                  BSON("" << Decimal128(-std::numeric_limits<double>::min())));
    }
}

TEST(BSONObjCompare, NumberDecimalCompareDoubleInfinity) {
    if (Decimal128::enabled) {
        ASSERT_EQ(BSON("" << Decimal128::kPositiveInfinity),
                  BSON("" << std::numeric_limits<double>::infinity()));
        ASSERT_EQ(BSON("" << Decimal128::kNegativeInfinity),
                  BSON("" << -std::numeric_limits<double>::infinity()));
    }
}

TEST(BSONObjCompare, NumberDecimalCompareDoubleNaN) {
    if (Decimal128::enabled) {
        ASSERT_EQ(BSON("" << Decimal128::kPositiveNaN),
                  BSON("" << std::numeric_limits<double>::quiet_NaN()));
        ASSERT_EQ(BSON("" << Decimal128::kNegativeNaN),
                  BSON("" << -std::numeric_limits<double>::quiet_NaN()));
    }
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


}  // unnamed namespace

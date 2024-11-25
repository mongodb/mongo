/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Verifies that the server treats legacy index key specs the way that we expect them. Namely,
// anything that is not a number is treated as ascending, and all negative numbers are descending.
// The exception to this is any form of a negative 0 (-0, -0.0, etc) — these are treated as
// ascending.
TEST(IndexKeyOrderingTest, VerifyOlderIndexKeySpecBehavior) {
    auto generateOrdering = [](const auto& input) {
        return Ordering::make(BSON("a" << input)).get(0);
    };

    // Non-negative numbers. Should all be ascending.
    ASSERT_EQ(generateOrdering(0), 1);
    ASSERT_EQ(generateOrdering(0.0), 1);
    ASSERT_EQ(generateOrdering(-0.0), 1);  // Special case - a negative 0 is treated as ascending.
    ASSERT_EQ(generateOrdering(std::numeric_limits<double>::quiet_NaN()), 1);
    ASSERT_EQ(generateOrdering(1E-10f), 1);
    ASSERT_EQ(generateOrdering(FLT_MIN), 1);
    ASSERT_EQ(generateOrdering(FLT_TRUE_MIN), 1);
    ASSERT_EQ(generateOrdering(DBL_MIN), 1);
    ASSERT_EQ(generateOrdering(DBL_TRUE_MIN), 1);
    ASSERT_EQ(generateOrdering(Decimal128("1E-10")), 1);

    // Negative numbers. Should all be descending.
    ASSERT_EQ(generateOrdering(-1E-10f), -1);
    ASSERT_EQ(generateOrdering(-FLT_MIN), -1);
    ASSERT_EQ(generateOrdering(-FLT_TRUE_MIN), -1);
    ASSERT_EQ(generateOrdering(-DBL_MIN), -1);
    ASSERT_EQ(generateOrdering(-DBL_TRUE_MIN), -1);
    ASSERT_EQ(generateOrdering(-Decimal128("1E-10")), -1);

    // Miscellaneous non-numeric types.
    ASSERT_EQ(generateOrdering(""), 1);
    ASSERT_EQ(generateOrdering("xyz"), 1);
    ASSERT_EQ(generateOrdering(BSON("y" << 1)), 1);
    ASSERT_EQ(generateOrdering(BSON_ARRAY(1)), 1);
    ASSERT_EQ(generateOrdering(BSONBinData("", 0, BinDataGeneral)), 1);
    ASSERT_EQ(generateOrdering(BSONUndefined), 1);
    ASSERT_EQ(generateOrdering(OID("deadbeefdeadbeefdeadbeef")), 1);
    ASSERT_EQ(generateOrdering(false), 1);
    ASSERT_EQ(generateOrdering(true), 1);
    ASSERT_EQ(generateOrdering(DATENOW), 1);
    ASSERT_EQ(generateOrdering(BSONNULL), 1);
    ASSERT_EQ(generateOrdering(BSONRegEx("reg.ex")), 1);
    ASSERT_EQ(generateOrdering(BSONDBRef("db", OID("dbdbdbdbdbdbdbdbdbdbdbdb"))), 1);
    ASSERT_EQ(generateOrdering(BSONCode("(function(){})();")), 1);
    ASSERT_EQ(generateOrdering(BSONSymbol("symbol")), 1);
    ASSERT_EQ(generateOrdering(BSONCodeWScope("(function(){})();", BSON("a" << 1))), 1);
    ASSERT_EQ(generateOrdering(Timestamp(1, 2)), 1);
    ASSERT_EQ(generateOrdering(MINKEY), 1);
    ASSERT_EQ(generateOrdering(MAXKEY), 1);
}
}  // namespace
}  // namespace mongo

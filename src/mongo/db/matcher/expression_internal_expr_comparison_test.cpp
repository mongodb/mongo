/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <limits>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

TEST(InternalExprComparisonMatchExpression, DoesNotPerformTypeBracketing) {
    BSONObj operand = BSON("x" << 2);
    {
        InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << MINKEY)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("y" << 0)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << BSONNULL)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << BSONUndefined)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << 1)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << 2)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << 3.5)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt,
                                               BSON("x"
                                                    << "string")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON("a" << 1))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << OID())));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << DATENOW)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << Timestamp(0, 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt,
                                               BSON("x"
                                                    << "/^m/")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << MAXKEY)));
    }
    {
        InternalExprGTEMatchExpression gte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << MINKEY)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("y" << 0)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << BSONNULL)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << BSONUndefined)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << 1)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << 2)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << 3.5)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte,
                                               BSON("x"
                                                    << "string")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON("a" << 1))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << OID())));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << DATENOW)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << Timestamp(0, 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte,
                                               BSON("x"
                                                    << "/^m/")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << MAXKEY)));
    }
    {
        InternalExprLTMatchExpression lt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << MINKEY)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("y" << 0)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSONNULL)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSONUndefined)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << 1)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << 2)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << 3.5)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt,
                                                BSON("x"
                                                     << "string")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON("a" << 1))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &lt,
            BSON("x" << BSON_ARRAY(1 << 2
                                     << 3))));  // Always returns true if path contains an array.
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << OID())));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << DATENOW)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << Timestamp(0, 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt,
                                                BSON("x"
                                                     << "/^m/")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << MAXKEY)));
    }
    {
        InternalExprLTEMatchExpression lte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << MINKEY)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("y" << 0)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSONNULL)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSONUndefined)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << 1)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << 2)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << 3.5)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte,
                                                BSON("x"
                                                     << "string")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON("a" << 1))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &lte,
            BSON("x" << BSON_ARRAY(1 << 2
                                     << 3))));  // Always returns true if path contains an array.
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << OID())));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << DATENOW)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << Timestamp(0, 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte,
                                                BSON("x"
                                                     << "/^m/")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << MAXKEY)));
    }
}

TEST(InternalExprComparisonMatchExpression, CorrectlyComparesNaN) {
    BSONObj operand = BSON("x" << kNaN);
    // This behavior differs from how regular comparison MatchExpressions treat NaN, and places NaN
    // within the total order of values as less than all numbers.
    {
        InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << MINKEY)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << BSONNULL)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << kNaN)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << Decimal128::kNegativeInfinity)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << 2)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << 3.5)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << MAXKEY)));
    }
    {
        InternalExprGTEMatchExpression gte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << MINKEY)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << BSONNULL)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << kNaN)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << Decimal128::kNegativeInfinity)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << 2)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << 3.5)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << MAXKEY)));
    }
    {
        InternalExprLTMatchExpression lt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << MINKEY)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSONNULL)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << kNaN)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << Decimal128::kNegativeInfinity)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << 2)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << 3.5)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << MAXKEY)));
    }
    {
        InternalExprLTEMatchExpression lte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << MINKEY)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSONNULL)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << kNaN)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << Decimal128::kNegativeInfinity)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << 2)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << 3.5)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << MAXKEY)));
    }
}

// Note we depend on this for our ability to rewrite predicates on timeseries collections where the
// buckets have mixed types.
TEST(InternalExprComparisonMatchExpression, AlwaysReturnsTrueWithLeafArrays) {
    BSONObj operand = BSON("x" << 2);
    {
        InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(0))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << 1)));
    }
    {
        InternalExprGTEMatchExpression gte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(0))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << 1)));
    }
    {
        InternalExprLTMatchExpression lt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(0))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << 3)));
    }
    {
        InternalExprLTEMatchExpression lte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(0))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << 3)));
    }
}

TEST(InternalExprComparisonMatchExpression, AlwaysReturnsTrueWithNonLeafArrays) {
    BSONObj operand = BSON("x.y" << 2);
    {
        InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(BSON("y" << 1)))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &gt, BSON("x" << BSON_ARRAY(BSON("y" << 2) << BSON("y" << 3)))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON("y" << 1))));
    }
    {
        InternalExprGTEMatchExpression gte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(BSON("y" << 1)))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &gte, BSON("x" << BSON_ARRAY(BSON("y" << 2) << BSON("y" << 3)))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON("y" << 1))));
    }
    {
        InternalExprLTMatchExpression lt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(BSON("y" << 3)))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &lt, BSON("x" << BSON_ARRAY(BSON("y" << 1) << BSON("y" << 2)))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON("y" << 3))));
    }
    {
        InternalExprLTEMatchExpression lte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(BSON("y" << 3)))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &lte, BSON("x" << BSON_ARRAY(BSON("y" << 1) << BSON("y" << 2)))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON("y" << 3))));
    }
}

DEATH_TEST_REGEX(InternalExprComparisonMatchExpression,
                 CannotCompareToArray,
                 R"#(Invariant failure.*_rhs.type\(\) != BSONType::Array)#") {
    BSONObj operand = BSON("x" << BSON_ARRAY(1 << 2));
    InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
}

DEATH_TEST_REGEX(InternalExprComparisonMatchExpression,
                 CannotCompareToUndefined,
                 R"#(Invariant failure.*_rhs.type\(\) != BSONType::Undefined)#") {
    BSONObj operand = BSON("x" << BSONUndefined);
    InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
}


}  // namespace
}  // namespace mongo

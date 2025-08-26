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

#include "mongo/db/matcher/expression_algo.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/parsers/matcher/parsed_match_expression_for_test.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <tuple>
#include <variant>

#include <absl/container/flat_hash_map.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

namespace mongo {

using std::unique_ptr;
using namespace std::string_literals;


void assertMatchesEqual(const ParsedMatchExpressionForTest& expected,
                        const std::unique_ptr<MatchExpression>& actual) {
    if (expected.get() == nullptr) {
        ASSERT(actual == nullptr);
        return;
    }
    ASSERT(actual != nullptr);
    ASSERT_EQ(expected.get()->toString(), actual.get()->toString());
}

TEST(ExpressionAlgoIsSubsetOf, NullAndOmittedField) {
    // Verify that the ComparisonMatchExpression constructor prohibits creating a match expression
    // with an Undefined type.
    BSONObj undefined = fromjson("{a: undefined}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  MatchExpressionParser::parse(undefined, std::move(expCtx)).getStatus());

    ParsedMatchExpressionForTest empty("{}");
    ParsedMatchExpressionForTest null("{a: null}");

    ASSERT_TRUE(expression::isSubsetOf(null.get(), empty.get()));
    ASSERT_FALSE(expression::isSubsetOf(empty.get(), null.get()));

    ParsedMatchExpressionForTest b1("{b: 1}");
    ParsedMatchExpressionForTest aNullB1("{a: null, b: 1}");

    ASSERT_TRUE(expression::isSubsetOf(aNullB1.get(), b1.get()));
    ASSERT_FALSE(expression::isSubsetOf(b1.get(), aNullB1.get()));

    ParsedMatchExpressionForTest a1C3("{a: 1, c: 3}");
    ParsedMatchExpressionForTest a1BNullC3("{a: 1, b: null, c: 3}");

    ASSERT_TRUE(expression::isSubsetOf(a1BNullC3.get(), a1C3.get()));
    ASSERT_FALSE(expression::isSubsetOf(a1C3.get(), a1BNullC3.get()));
}

TEST(ExpressionAlgoIsSubsetOf, NullAndIn) {
    ParsedMatchExpressionForTest eqNull("{x: null}");
    ParsedMatchExpressionForTest inNull("{x: {$in: [null]}}");
    ParsedMatchExpressionForTest inNullOr2("{x: {$in: [null, 2]}}");

    ASSERT_TRUE(expression::isSubsetOf(inNull.get(), eqNull.get()));
    ASSERT_FALSE(expression::isSubsetOf(inNullOr2.get(), eqNull.get()));

    ASSERT_TRUE(expression::isSubsetOf(eqNull.get(), inNull.get()));
    ASSERT_TRUE(expression::isSubsetOf(eqNull.get(), inNullOr2.get()));
}

TEST(ExpressionAlgoIsSubsetOf, NullAndExists) {
    ParsedMatchExpressionForTest null("{x: null}");
    ParsedMatchExpressionForTest exists("{x: {$exists: true}}");
    ASSERT_FALSE(expression::isSubsetOf(null.get(), exists.get()));
    ASSERT_FALSE(expression::isSubsetOf(exists.get(), null.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_NaN) {
    ParsedMatchExpressionForTest nan("{x: NaN}");
    ParsedMatchExpressionForTest lt("{x: {$lt: 5}}");
    ParsedMatchExpressionForTest lte("{x: {$lte: 5}}");
    ParsedMatchExpressionForTest gte("{x: {$gte: 5}}");
    ParsedMatchExpressionForTest gt("{x: {$gt: 5}}");
    ParsedMatchExpressionForTest in("{x: {$in: [5]}}");

    ASSERT_TRUE(expression::isSubsetOf(nan.get(), nan.get()));
    ASSERT_FALSE(expression::isSubsetOf(nan.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(lt.get(), nan.get()));
    ASSERT_FALSE(expression::isSubsetOf(nan.get(), lte.get()));
    ASSERT_FALSE(expression::isSubsetOf(lte.get(), nan.get()));
    ASSERT_FALSE(expression::isSubsetOf(nan.get(), gte.get()));
    ASSERT_FALSE(expression::isSubsetOf(gte.get(), nan.get()));
    ASSERT_FALSE(expression::isSubsetOf(nan.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(gt.get(), nan.get()));
    ASSERT_FALSE(expression::isSubsetOf(nan.get(), in.get()));
    ASSERT_FALSE(expression::isSubsetOf(in.get(), nan.get()));

    ParsedMatchExpressionForTest decNan("{x : NumberDecimal(\"NaN\") }");
    ASSERT_TRUE(expression::isSubsetOf(decNan.get(), decNan.get()));
    ASSERT_TRUE(expression::isSubsetOf(nan.get(), decNan.get()));
    ASSERT_TRUE(expression::isSubsetOf(decNan.get(), nan.get()));
    ASSERT_FALSE(expression::isSubsetOf(decNan.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(lt.get(), decNan.get()));
    ASSERT_FALSE(expression::isSubsetOf(decNan.get(), lte.get()));
    ASSERT_FALSE(expression::isSubsetOf(lte.get(), decNan.get()));
    ASSERT_FALSE(expression::isSubsetOf(decNan.get(), gte.get()));
    ASSERT_FALSE(expression::isSubsetOf(gte.get(), decNan.get()));
    ASSERT_FALSE(expression::isSubsetOf(decNan.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(gt.get(), decNan.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_EQ) {
    ParsedMatchExpressionForTest a5("{a: 5}");
    ParsedMatchExpressionForTest a6("{a: 6}");
    ParsedMatchExpressionForTest b5("{b: 5}");

    ASSERT_TRUE(expression::isSubsetOf(a5.get(), a5.get()));
    ASSERT_FALSE(expression::isSubsetOf(a5.get(), a6.get()));
    ASSERT_FALSE(expression::isSubsetOf(a5.get(), b5.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CompareAnd_EQ) {
    ParsedMatchExpressionForTest a1B2("{a: 1, b: 2}");
    ParsedMatchExpressionForTest a1B7("{a: 1, b: 7}");
    ParsedMatchExpressionForTest a1("{a: 1}");
    ParsedMatchExpressionForTest b2("{b: 2}");

    ASSERT_TRUE(expression::isSubsetOf(a1B2.get(), a1B2.get()));
    ASSERT_FALSE(expression::isSubsetOf(a1B2.get(), a1B7.get()));

    ASSERT_TRUE(expression::isSubsetOf(a1B2.get(), a1.get()));
    ASSERT_TRUE(expression::isSubsetOf(a1B2.get(), b2.get()));
    ASSERT_FALSE(expression::isSubsetOf(a1B7.get(), b2.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CompareAnd_GT) {
    ParsedMatchExpressionForTest filter("{a: {$gt: 5}, b: {$gt: 6}}");
    ParsedMatchExpressionForTest query("{a: {$gt: 5}, b: {$gt: 6}, c: {$gt: 7}}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CompareAnd_SingleField) {
    ParsedMatchExpressionForTest filter("{a: {$gt: 5, $lt: 7}}");
    ParsedMatchExpressionForTest query("{a: {$gt: 5, $lt: 6}}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CompareOr_LT) {
    ParsedMatchExpressionForTest lt5("{a: {$lt: 5}}");
    ParsedMatchExpressionForTest eq2OrEq3("{$or: [{a: 2}, {a: 3}]}");
    ParsedMatchExpressionForTest eq4OrEq5("{$or: [{a: 4}, {a: 5}]}");
    ParsedMatchExpressionForTest eq4OrEq6("{$or: [{a: 4}, {a: 6}]}");

    ASSERT_TRUE(expression::isSubsetOf(eq2OrEq3.get(), lt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(eq4OrEq5.get(), lt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(eq4OrEq6.get(), lt5.get()));

    ParsedMatchExpressionForTest lt4OrLt5("{$or: [{a: {$lt: 4}}, {a: {$lt: 5}}]}");

    ASSERT_TRUE(expression::isSubsetOf(lt4OrLt5.get(), lt5.get()));
    ASSERT_TRUE(expression::isSubsetOf(lt5.get(), lt4OrLt5.get()));

    ParsedMatchExpressionForTest lt7OrLt8("{$or: [{a: {$lt: 7}}, {a: {$lt: 8}}]}");

    ASSERT_FALSE(expression::isSubsetOf(lt7OrLt8.get(), lt5.get()));
    ASSERT_TRUE(expression::isSubsetOf(lt5.get(), lt7OrLt8.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CompareOr_GTE) {
    ParsedMatchExpressionForTest gte5("{a: {$gte: 5}}");
    ParsedMatchExpressionForTest eq4OrEq6("{$or: [{a: 4}, {a: 6}]}");
    ParsedMatchExpressionForTest eq5OrEq6("{$or: [{a: 5}, {a: 6}]}");
    ParsedMatchExpressionForTest eq7OrEq8("{$or: [{a: 7}, {a: 8}]}");

    ASSERT_FALSE(expression::isSubsetOf(eq4OrEq6.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(eq5OrEq6.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(eq7OrEq8.get(), gte5.get()));

    ParsedMatchExpressionForTest gte5OrGte6("{$or: [{a: {$gte: 5}}, {a: {$gte: 6}}]}");

    ASSERT_TRUE(expression::isSubsetOf(gte5OrGte6.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(gte5.get(), gte5OrGte6.get()));

    ParsedMatchExpressionForTest gte3OrGte4("{$or: [{a: {$gte: 3}}, {a: {$gte: 4}}]}");

    ASSERT_FALSE(expression::isSubsetOf(gte3OrGte4.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(gte5.get(), gte3OrGte4.get()));
}

TEST(ExpressionAlgoIsSubsetOf, DifferentCanonicalTypes) {
    ParsedMatchExpressionForTest number("{x: {$gt: 1}}");
    ParsedMatchExpressionForTest string("{x: {$gt: 'a'}}");
    ASSERT_FALSE(expression::isSubsetOf(number.get(), string.get()));
    ASSERT_FALSE(expression::isSubsetOf(string.get(), number.get()));
}

TEST(ExpressionAlgoIsSubsetOf, DifferentNumberTypes) {
    ParsedMatchExpressionForTest numberDouble("{x: 5.0}");
    ParsedMatchExpressionForTest numberInt("{x: NumberInt(5)}");
    ParsedMatchExpressionForTest numberLong("{x: NumberLong(5)}");

    ASSERT_TRUE(expression::isSubsetOf(numberDouble.get(), numberInt.get()));
    ASSERT_TRUE(expression::isSubsetOf(numberDouble.get(), numberLong.get()));
    ASSERT_TRUE(expression::isSubsetOf(numberInt.get(), numberDouble.get()));
    ASSERT_TRUE(expression::isSubsetOf(numberInt.get(), numberLong.get()));
    ASSERT_TRUE(expression::isSubsetOf(numberLong.get(), numberDouble.get()));
    ASSERT_TRUE(expression::isSubsetOf(numberLong.get(), numberInt.get()));
}

TEST(ExpressionAlgoIsSubsetOf, PointInUnboundedRange) {
    ParsedMatchExpressionForTest a4("{a: 4}");
    ParsedMatchExpressionForTest a5("{a: 5}");
    ParsedMatchExpressionForTest a6("{a: 6}");
    ParsedMatchExpressionForTest b5("{b: 5}");

    ParsedMatchExpressionForTest lt5("{a: {$lt: 5}}");
    ParsedMatchExpressionForTest lte5("{a: {$lte: 5}}");
    ParsedMatchExpressionForTest gte5("{a: {$gte: 5}}");
    ParsedMatchExpressionForTest gt5("{a: {$gt: 5}}");

    ASSERT_TRUE(expression::isSubsetOf(a4.get(), lte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(a5.get(), lte5.get()));
    ASSERT_FALSE(expression::isSubsetOf(a6.get(), lte5.get()));

    ASSERT_TRUE(expression::isSubsetOf(a4.get(), lt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(a5.get(), lt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(a6.get(), lt5.get()));

    ASSERT_FALSE(expression::isSubsetOf(a4.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(a5.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(a6.get(), gte5.get()));

    ASSERT_FALSE(expression::isSubsetOf(a4.get(), gt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(a5.get(), gt5.get()));
    ASSERT_TRUE(expression::isSubsetOf(a6.get(), gt5.get()));

    // An unbounded range query does not match a subset of documents of a point query.
    ASSERT_FALSE(expression::isSubsetOf(lt5.get(), a5.get()));
    ASSERT_FALSE(expression::isSubsetOf(lte5.get(), a5.get()));
    ASSERT_FALSE(expression::isSubsetOf(gte5.get(), a5.get()));
    ASSERT_FALSE(expression::isSubsetOf(gt5.get(), a5.get()));

    // Cannot be a subset if comparing different field names.
    ASSERT_FALSE(expression::isSubsetOf(b5.get(), lt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(b5.get(), lte5.get()));
    ASSERT_FALSE(expression::isSubsetOf(b5.get(), gte5.get()));
    ASSERT_FALSE(expression::isSubsetOf(b5.get(), gt5.get()));
}

TEST(ExpressionAlgoIsSubsetOf, PointInBoundedRange) {
    ParsedMatchExpressionForTest filter("{a: {$gt: 5, $lt: 10}}");
    ParsedMatchExpressionForTest query("{a: 6}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, PointInBoundedRange_FakeAnd) {
    ParsedMatchExpressionForTest filter("{a: {$gt: 5, $lt: 10}}");
    ParsedMatchExpressionForTest query("{$and: [{a: 6}, {a: 6}]}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, MultiplePointsInBoundedRange) {
    ParsedMatchExpressionForTest filter("{a: {$gt: 5, $lt: 10}}");
    ParsedMatchExpressionForTest queryAllInside("{a: {$in: [6, 7, 8]}}");
    ParsedMatchExpressionForTest queryStraddleLower("{a: {$in: [4.9, 5.1]}}");
    ParsedMatchExpressionForTest queryStraddleUpper("{a: {$in: [9.9, 10.1]}}");

    ASSERT_TRUE(expression::isSubsetOf(queryAllInside.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(queryStraddleLower.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(queryStraddleUpper.get(), filter.get()));
}

TEST(ExpressionAlgoIsSubsetOf, PointInCompoundRange) {
    ParsedMatchExpressionForTest filter("{a: {$gt: 5}, b: {$gt: 6}, c: {$gt: 7}}");
    ParsedMatchExpressionForTest query("{a: 10, b: 10, c: 10}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_LT_LTE) {
    ParsedMatchExpressionForTest lte4("{x: {$lte: 4}}");
    ParsedMatchExpressionForTest lt5("{x: {$lt: 5}}");
    ParsedMatchExpressionForTest lte5("{x: {$lte: 5}}");
    ParsedMatchExpressionForTest lt6("{x: {$lt: 6}}");

    ASSERT_TRUE(expression::isSubsetOf(lte4.get(), lte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(lt5.get(), lte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(lte5.get(), lte5.get()));
    ASSERT_FALSE(expression::isSubsetOf(lt6.get(), lte5.get()));

    ASSERT_TRUE(expression::isSubsetOf(lte4.get(), lt5.get()));
    ASSERT_TRUE(expression::isSubsetOf(lt5.get(), lt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(lte5.get(), lt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(lt6.get(), lt5.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_GT_GTE) {
    ParsedMatchExpressionForTest gte6("{x: {$gte: 6}}");
    ParsedMatchExpressionForTest gt5("{x: {$gt: 5}}");
    ParsedMatchExpressionForTest gte5("{x: {$gte: 5}}");
    ParsedMatchExpressionForTest gt4("{x: {$gt: 4}}");

    ASSERT_TRUE(expression::isSubsetOf(gte6.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(gt5.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(gte5.get(), gte5.get()));
    ASSERT_FALSE(expression::isSubsetOf(gt4.get(), gte5.get()));

    ASSERT_TRUE(expression::isSubsetOf(gte6.get(), gt5.get()));
    ASSERT_TRUE(expression::isSubsetOf(gt5.get(), gt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(gte5.get(), gt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(gt4.get(), gt5.get()));
}

TEST(ExpressionAlgoIsSubsetOf, BoundedRangeInUnboundedRange) {
    ParsedMatchExpressionForTest filter("{a: {$gt: 1}}");
    ParsedMatchExpressionForTest query("{a: {$gt: 5, $lt: 10}}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, MultipleRangesInUnboundedRange) {
    ParsedMatchExpressionForTest filter("{a: {$gt: 1}}");
    ParsedMatchExpressionForTest negative("{$or: [{a: {$gt: 5, $lt: 10}}, {a: {$lt: 0}}]}");
    ParsedMatchExpressionForTest unbounded("{$or: [{a: {$gt: 5, $lt: 10}}, {a: {$gt: 15}}]}");
    ParsedMatchExpressionForTest bounded(
        "{$or: [{a: {$gt: 5, $lt: 10}}, {a: {$gt: 20, $lt: 30}}]}");

    ASSERT_FALSE(expression::isSubsetOf(negative.get(), filter.get()));
    ASSERT_TRUE(expression::isSubsetOf(unbounded.get(), filter.get()));
    ASSERT_TRUE(expression::isSubsetOf(bounded.get(), filter.get()));
}

TEST(ExpressionAlgoIsSubsetOf, MultipleFields) {
    ParsedMatchExpressionForTest filter("{a: {$gt: 5}, b: {$lt: 10}}");
    ParsedMatchExpressionForTest onlyA("{$or: [{a: 6, b: {$lt: 4}}, {a: {$gt: 11}}]}");
    ParsedMatchExpressionForTest onlyB("{$or: [{b: {$lt: 4}}, {a: {$gt: 11}, b: 9}]}");
    ParsedMatchExpressionForTest both("{$or: [{a: 6, b: {$lt: 4}}, {a: {$gt: 11}, b: 9}]}");

    ASSERT_FALSE(expression::isSubsetOf(onlyA.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(onlyB.get(), filter.get()));
    ASSERT_TRUE(expression::isSubsetOf(both.get(), filter.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_LT_In) {
    ParsedMatchExpressionForTest lt("{a: {$lt: 5}}");

    ParsedMatchExpressionForTest inLt("{a: {$in: [4.9]}}");
    ParsedMatchExpressionForTest inEq("{a: {$in: [5]}}");
    ParsedMatchExpressionForTest inGt("{a: {$in: [5.1]}}");
    ParsedMatchExpressionForTest inNull("{a: {$in: [null]}}");

    ParsedMatchExpressionForTest inAllEq("{a: {$in: [5, 5.0]}}");
    ParsedMatchExpressionForTest inAllLte("{a: {$in: [4.9, 5]}}");
    ParsedMatchExpressionForTest inAllLt("{a: {$in: [2, 3, 4]}}");
    ParsedMatchExpressionForTest inStraddle("{a: {$in: [4, 6]}}");
    ParsedMatchExpressionForTest inLtAndNull("{a: {$in: [1, null]}}");

    ASSERT_TRUE(expression::isSubsetOf(inLt.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inEq.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inGt.get(), lt.get()));

    ASSERT_FALSE(expression::isSubsetOf(inAllEq.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inAllLte.get(), lt.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllLt.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inStraddle.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inLtAndNull.get(), lt.get()));

    ASSERT_FALSE(expression::isSubsetOf(lt.get(), inLt.get()));
    ASSERT_FALSE(expression::isSubsetOf(lt.get(), inEq.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_LTE_In) {
    ParsedMatchExpressionForTest lte("{a: {$lte: 5}}");

    ParsedMatchExpressionForTest inLt("{a: {$in: [4.9]}}");
    ParsedMatchExpressionForTest inEq("{a: {$in: [5]}}");
    ParsedMatchExpressionForTest inGt("{a: {$in: [5.1]}}");
    ParsedMatchExpressionForTest inNull("{a: {$in: [null]}}");

    ParsedMatchExpressionForTest inAllEq("{a: {$in: [5, 5.0]}}");
    ParsedMatchExpressionForTest inAllLte("{a: {$in: [4.9, 5]}}");
    ParsedMatchExpressionForTest inAllLt("{a: {$in: [2, 3, 4]}}");
    ParsedMatchExpressionForTest inStraddle("{a: {$in: [4, 6]}}");
    ParsedMatchExpressionForTest inLtAndNull("{a: {$in: [1, null]}}");

    ASSERT_TRUE(expression::isSubsetOf(inLt.get(), lte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inEq.get(), lte.get()));
    ASSERT_FALSE(expression::isSubsetOf(inGt.get(), lte.get()));

    ASSERT_TRUE(expression::isSubsetOf(inAllEq.get(), lte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllLte.get(), lte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllLt.get(), lte.get()));
    ASSERT_FALSE(expression::isSubsetOf(inStraddle.get(), lte.get()));
    ASSERT_FALSE(expression::isSubsetOf(inLtAndNull.get(), lte.get()));

    ASSERT_FALSE(expression::isSubsetOf(lte.get(), inLt.get()));
    ASSERT_FALSE(expression::isSubsetOf(lte.get(), inEq.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_EQ_In) {
    ParsedMatchExpressionForTest eq("{a: 5}");

    ParsedMatchExpressionForTest inLt("{a: {$in: [4.9]}}");
    ParsedMatchExpressionForTest inEq("{a: {$in: [5]}}");
    ParsedMatchExpressionForTest inGt("{a: {$in: [5.1]}}");
    ParsedMatchExpressionForTest inNull("{a: {$in: [null]}}");

    ParsedMatchExpressionForTest inAllEq("{a: {$in: [5, 5.0]}}");
    ParsedMatchExpressionForTest inStraddle("{a: {$in: [4, 6]}}");
    ParsedMatchExpressionForTest inEqAndNull("{a: {$in: [5, null]}}");

    ASSERT_FALSE(expression::isSubsetOf(inLt.get(), eq.get()));
    ASSERT_TRUE(expression::isSubsetOf(inEq.get(), eq.get()));
    ASSERT_FALSE(expression::isSubsetOf(inGt.get(), eq.get()));

    ASSERT_TRUE(expression::isSubsetOf(inAllEq.get(), eq.get()));
    ASSERT_FALSE(expression::isSubsetOf(inStraddle.get(), eq.get()));
    ASSERT_FALSE(expression::isSubsetOf(inEqAndNull.get(), eq.get()));

    ASSERT_TRUE(expression::isSubsetOf(eq.get(), inEq.get()));
    ASSERT_FALSE(expression::isSubsetOf(eq.get(), inLt.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_GT_In) {
    ParsedMatchExpressionForTest gt("{a: {$gt: 5}}");

    ParsedMatchExpressionForTest inLt("{a: {$in: [4.9]}}");
    ParsedMatchExpressionForTest inEq("{a: {$in: [5]}}");
    ParsedMatchExpressionForTest inGt("{a: {$in: [5.1]}}");
    ParsedMatchExpressionForTest inNull("{a: {$in: [null]}}");

    ParsedMatchExpressionForTest inAllEq("{a: {$in: [5, 5.0]}}");
    ParsedMatchExpressionForTest inAllGte("{a: {$in: [5, 5.1]}}");
    ParsedMatchExpressionForTest inAllGt("{a: {$in: [6, 7, 8]}}");
    ParsedMatchExpressionForTest inStraddle("{a: {$in: [4, 6]}}");
    ParsedMatchExpressionForTest inGtAndNull("{a: {$in: [9, null]}}");

    ASSERT_FALSE(expression::isSubsetOf(inLt.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inEq.get(), gt.get()));
    ASSERT_TRUE(expression::isSubsetOf(inGt.get(), gt.get()));

    ASSERT_FALSE(expression::isSubsetOf(inAllEq.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inAllGte.get(), gt.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllGt.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inStraddle.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inGtAndNull.get(), gt.get()));

    ASSERT_FALSE(expression::isSubsetOf(gt.get(), inGt.get()));
    ASSERT_FALSE(expression::isSubsetOf(gt.get(), inEq.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_GTE_In) {
    ParsedMatchExpressionForTest gte("{a: {$gte: 5}}");

    ParsedMatchExpressionForTest inLt("{a: {$in: [4.9]}}");
    ParsedMatchExpressionForTest inEq("{a: {$in: [5]}}");
    ParsedMatchExpressionForTest inGt("{a: {$in: [5.1]}}");
    ParsedMatchExpressionForTest inNull("{a: {$in: [null]}}");

    ParsedMatchExpressionForTest inAllEq("{a: {$in: [5, 5.0]}}");
    ParsedMatchExpressionForTest inAllGte("{a: {$in: [5, 5.1]}}");
    ParsedMatchExpressionForTest inAllGt("{a: {$in: [6, 7, 8]}}");
    ParsedMatchExpressionForTest inStraddle("{a: {$in: [4, 6]}}");
    ParsedMatchExpressionForTest inGtAndNull("{a: {$in: [9, null]}}");

    ASSERT_FALSE(expression::isSubsetOf(inLt.get(), gte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inEq.get(), gte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inGt.get(), gte.get()));

    ASSERT_TRUE(expression::isSubsetOf(inAllEq.get(), gte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllGte.get(), gte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllGt.get(), gte.get()));
    ASSERT_FALSE(expression::isSubsetOf(inStraddle.get(), gte.get()));
    ASSERT_FALSE(expression::isSubsetOf(inGtAndNull.get(), gte.get()));

    ASSERT_FALSE(expression::isSubsetOf(gte.get(), inGt.get()));
    ASSERT_FALSE(expression::isSubsetOf(gte.get(), inEq.get()));
}

TEST(ExpressionAlgoIsSubsetOf, RegexAndIn) {
    ParsedMatchExpressionForTest eq1("{x: 1}");
    ParsedMatchExpressionForTest eqA("{x: 'a'}");
    ParsedMatchExpressionForTest inRegexA("{x: {$in: [/a/]}}");
    ParsedMatchExpressionForTest inRegexAbc("{x: {$in: [/abc/]}}");
    ParsedMatchExpressionForTest inRegexAOrEq1("{x: {$in: [/a/, 1]}}");
    ParsedMatchExpressionForTest inRegexAOrNull("{x: {$in: [/a/, null]}}");

    ASSERT_FALSE(expression::isSubsetOf(inRegexAOrEq1.get(), eq1.get()));
    ASSERT_FALSE(expression::isSubsetOf(inRegexA.get(), eqA.get()));
    ASSERT_FALSE(expression::isSubsetOf(inRegexAOrNull.get(), eqA.get()));

    ASSERT_FALSE(expression::isSubsetOf(eq1.get(), inRegexAOrEq1.get()));
    ASSERT_FALSE(expression::isSubsetOf(eqA.get(), inRegexA.get()));
    ASSERT_FALSE(expression::isSubsetOf(eqA.get(), inRegexAOrNull.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Exists) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest bExists("{b: {$exists: true}}");
    ParsedMatchExpressionForTest aExistsBExists("{a: {$exists: true}, b: {$exists: true}}");
    ParsedMatchExpressionForTest aExistsBExistsC5("{a: {$exists: true}, b: {$exists: true}, c: 5}");

    ASSERT_TRUE(expression::isSubsetOf(aExists.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), bExists.get()));

    ASSERT_TRUE(expression::isSubsetOf(aExistsBExists.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aExistsBExists.get(), bExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExistsBExists.get(), aExistsBExistsC5.get()));

    ASSERT_TRUE(expression::isSubsetOf(aExistsBExistsC5.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aExistsBExistsC5.get(), bExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aExistsBExistsC5.get(), aExistsBExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_Exists) {
    ParsedMatchExpressionForTest exists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest eq("{a: 1}");
    ParsedMatchExpressionForTest gt("{a: {$gt: 4}}");
    ParsedMatchExpressionForTest lte("{a: {$lte: 7}}");

    ASSERT_TRUE(expression::isSubsetOf(eq.get(), exists.get()));
    ASSERT_TRUE(expression::isSubsetOf(gt.get(), exists.get()));
    ASSERT_TRUE(expression::isSubsetOf(lte.get(), exists.get()));

    ASSERT_FALSE(expression::isSubsetOf(exists.get(), eq.get()));
    ASSERT_FALSE(expression::isSubsetOf(exists.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(exists.get(), lte.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Type) {
    ParsedMatchExpressionForTest aType1("{a: {$type: 1}}");
    ParsedMatchExpressionForTest aType2("{a: {$type: 2}}");
    ParsedMatchExpressionForTest bType2("{b: {$type: 2}}");

    ASSERT_FALSE(expression::isSubsetOf(aType1.get(), aType2.get()));
    ASSERT_FALSE(expression::isSubsetOf(aType2.get(), aType1.get()));

    ASSERT_TRUE(expression::isSubsetOf(aType2.get(), aType2.get()));
    ASSERT_FALSE(expression::isSubsetOf(aType2.get(), bType2.get()));
}

TEST(ExpressionAlgoIsSubsetOf, TypeAndExists) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest aType2("{a: {$type: 2}}");
    ParsedMatchExpressionForTest bType2("{b: {$type: 2}}");

    ASSERT_TRUE(expression::isSubsetOf(aType2.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aType2.get()));
    ASSERT_FALSE(expression::isSubsetOf(bType2.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, AllAndExists) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest aAll("{a: {$all: ['x', 'y', 'z']}}");
    ParsedMatchExpressionForTest bAll("{b: {$all: ['x', 'y', 'z']}}");
    ParsedMatchExpressionForTest aAllWithNull("{a: {$all: ['x', null, 'z']}}");

    ASSERT_TRUE(expression::isSubsetOf(aAll.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bAll.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aAllWithNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, ElemMatchAndExists_Value) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest aElemMatch("{a: {$elemMatch: {$gt: 5, $lte: 10}}}");
    ParsedMatchExpressionForTest bElemMatch("{b: {$elemMatch: {$gt: 5, $lte: 10}}}");
    ParsedMatchExpressionForTest aElemMatchNull("{a: {$elemMatch: {$eq: null}}}");

    ASSERT_TRUE(expression::isSubsetOf(aElemMatch.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aElemMatch.get()));
    ASSERT_FALSE(expression::isSubsetOf(bElemMatch.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aElemMatchNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, ElemMatchAndExists_Object) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest aElemMatch("{a: {$elemMatch: {x: {$gt: 5}, y: {$lte: 10}}}}");
    ParsedMatchExpressionForTest bElemMatch("{b: {$elemMatch: {x: {$gt: 5}, y: {$lte: 10}}}}");
    ParsedMatchExpressionForTest aElemMatchNull("{a: {$elemMatch: {x: null, y: null}}}");

    ASSERT_TRUE(expression::isSubsetOf(aElemMatch.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aElemMatch.get()));
    ASSERT_FALSE(expression::isSubsetOf(bElemMatch.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aElemMatchNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, SizeAndExists) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest aSize0("{a: {$size: 0}}");
    ParsedMatchExpressionForTest aSize1("{a: {$size: 1}}");
    ParsedMatchExpressionForTest aSize3("{a: {$size: 3}}");
    ParsedMatchExpressionForTest bSize3("{b: {$size: 3}}");

    ASSERT_TRUE(expression::isSubsetOf(aSize0.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aSize1.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aSize3.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aSize3.get()));
    ASSERT_FALSE(expression::isSubsetOf(bSize3.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, ModAndExists) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest aMod5("{a: {$mod: [5, 0]}}");
    ParsedMatchExpressionForTest bMod5("{b: {$mod: [5, 0]}}");

    ASSERT_TRUE(expression::isSubsetOf(aMod5.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bMod5.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, RegexAndExists) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest aRegex("{a: {$regex: 'pattern'}}");
    ParsedMatchExpressionForTest bRegex("{b: {$regex: 'pattern'}}");

    ASSERT_TRUE(expression::isSubsetOf(aRegex.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bRegex.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, InAndExists) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest aIn("{a: {$in: [1, 2, 3]}}");
    ParsedMatchExpressionForTest bIn("{b: {$in: [1, 2, 3]}}");
    ParsedMatchExpressionForTest aInWithNull("{a: {$in: [1, null, 3]}}");

    ASSERT_TRUE(expression::isSubsetOf(aIn.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bIn.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aInWithNull.get(), aExists.get()));

    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aIn.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), bIn.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aInWithNull.get()));
}

TEST(ExpressionAlgoIsSubsetOf, NinAndExists) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest aNin("{a: {$nin: [1, 2, 3]}}");
    ParsedMatchExpressionForTest bNin("{b: {$nin: [1, 2, 3]}}");
    ParsedMatchExpressionForTest aNinWithNull("{a: {$nin: [1, null, 3]}}");

    ASSERT_FALSE(expression::isSubsetOf(aNin.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bNin.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aNinWithNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_Exists_NE) {
    ParsedMatchExpressionForTest aExists("{a: {$exists: true}}");
    ParsedMatchExpressionForTest aNotEqual1("{a: {$ne: 1}}");
    ParsedMatchExpressionForTest bNotEqual1("{b: {$ne: 1}}");
    ParsedMatchExpressionForTest aNotEqualNull("{a: {$ne: null}}");

    ASSERT_FALSE(expression::isSubsetOf(aNotEqual1.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bNotEqual1.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aNotEqualNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CollationAwareStringComparison) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ParsedMatchExpressionForTest lhs("{a: {$gt: 'abc'}}", &collator);
    ParsedMatchExpressionForTest rhs("{a: {$gt: 'cba'}}", &collator);

    ASSERT_TRUE(expression::isSubsetOf(lhs.get(), rhs.get()));

    ParsedMatchExpressionForTest lhsLT("{a: {$lt: 'abc'}}", &collator);
    ParsedMatchExpressionForTest rhsLT("{a: {$lt: 'cba'}}", &collator);

    ASSERT_FALSE(expression::isSubsetOf(lhsLT.get(), rhsLT.get()));
}

TEST(ExpressionAlgoIsSubsetOf, NonMatchingCollationsStringComparison) {
    CollatorInterfaceMock collatorAlwaysEqual(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock collatorReverseString(CollatorInterfaceMock::MockType::kReverseString);
    ParsedMatchExpressionForTest lhs("{a: {$gt: 'abc'}}", &collatorAlwaysEqual);
    ParsedMatchExpressionForTest rhs("{a: {$gt: 'cba'}}", &collatorReverseString);

    ASSERT_FALSE(expression::isSubsetOf(lhs.get(), rhs.get()));

    ParsedMatchExpressionForTest lhsLT("{a: {$lt: 'abc'}}", &collatorAlwaysEqual);
    ParsedMatchExpressionForTest rhsLT("{a: {$lt: 'cba'}}", &collatorReverseString);

    ASSERT_FALSE(expression::isSubsetOf(lhsLT.get(), rhsLT.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CollationAwareStringComparisonIn) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ParsedMatchExpressionForTest lhsAllGTcba("{a: {$in: ['abc', 'cbc']}}", &collator);
    ParsedMatchExpressionForTest lhsSomeGTcba("{a: {$in: ['abc', 'aba']}}", &collator);
    ParsedMatchExpressionForTest rhs("{a: {$gt: 'cba'}}", &collator);

    ASSERT_TRUE(expression::isSubsetOf(lhsAllGTcba.get(), rhs.get()));
    ASSERT_FALSE(expression::isSubsetOf(lhsSomeGTcba.get(), rhs.get()));

    ParsedMatchExpressionForTest rhsLT("{a: {$lt: 'cba'}}", &collator);

    ASSERT_FALSE(expression::isSubsetOf(lhsAllGTcba.get(), rhsLT.get()));
    ASSERT_FALSE(expression::isSubsetOf(lhsSomeGTcba.get(), rhsLT.get()));
}

// TODO SERVER-24674: isSubsetOf should return true after exploring nested objects.
TEST(ExpressionAlgoIsSubsetOf, NonMatchingCollationsNoStringComparisonLHS) {
    CollatorInterfaceMock collatorAlwaysEqual(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock collatorReverseString(CollatorInterfaceMock::MockType::kReverseString);
    ParsedMatchExpressionForTest lhs("{a: {b: 1}}", &collatorAlwaysEqual);
    ParsedMatchExpressionForTest rhs("{a: {$lt: {b: 'abc'}}}", &collatorReverseString);

    ASSERT_FALSE(expression::isSubsetOf(lhs.get(), rhs.get()));
}

TEST(ExpressionAlgoIsSubsetOf, NonMatchingCollationsNoStringComparison) {
    CollatorInterfaceMock collatorAlwaysEqual(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock collatorReverseString(CollatorInterfaceMock::MockType::kReverseString);
    ParsedMatchExpressionForTest lhs("{a: 1}", &collatorAlwaysEqual);
    ParsedMatchExpressionForTest rhs("{a: {$gt: 0}}", &collatorReverseString);

    ASSERT_TRUE(expression::isSubsetOf(lhs.get(), rhs.get()));
}

TEST(ExpressionAlgoIsSubsetOf, InternalExprEqIsSubsetOfNothing) {
    ParsedMatchExpressionForTest exprEq("{a: {$_internalExprEq: 0}}");
    ParsedMatchExpressionForTest regularEq("{a: {$eq: 0}}");
    {
        ParsedMatchExpressionForTest rhs("{a: {$gte: 0}}");
        ASSERT_FALSE(expression::isSubsetOf(exprEq.get(), rhs.get()));
        ASSERT_TRUE(expression::isSubsetOf(regularEq.get(), rhs.get()));
    }

    {
        ParsedMatchExpressionForTest rhs("{a: {$lte: 0}}");
        ASSERT_FALSE(expression::isSubsetOf(exprEq.get(), rhs.get()));
        ASSERT_TRUE(expression::isSubsetOf(regularEq.get(), rhs.get()));
    }
}

TEST(ExpressionAlgoIsSubsetOf, IsSubsetOfRHSAndWithinOr) {
    ParsedMatchExpressionForTest rhs("{$or: [{a: 3}, {$and: [{a: 5}, {b: 5}]}]}");
    {
        ParsedMatchExpressionForTest lhs("{a:5, b:5}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), rhs.get()));
    }
}

TEST(ExpressionAlgoIsSubsetOf, IsSubsetOfComplexRHSExpression) {
    ParsedMatchExpressionForTest complex(
        "{$or: [{z: 1}, {$and: [{x: 1}, {$or: [{y: 1}, {y: 2}]}]}]}");
    {
        ParsedMatchExpressionForTest lhs("{z: 1}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), complex.get()));
    }

    {
        ParsedMatchExpressionForTest lhs("{z: 1, x: 1, y:2}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), complex.get()));
    }

    {
        ParsedMatchExpressionForTest lhs(
            "{$or: [{z: 1}, {$and: [{x: 1}, {$or: [{y: 1}, {y: 2}]}]}]}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), complex.get()));
    }


    {
        ParsedMatchExpressionForTest lhs(
            "{$or: [{z: 2}, {$and: [{x: 2}, {$or: [{y: 3}, {y: 4}]}]}]}");
        ASSERT_FALSE(expression::isSubsetOf(lhs.get(), complex.get()));
    }


    {
        ParsedMatchExpressionForTest lhs("{z: 1, y:2}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), complex.get()));
    }

    {
        ParsedMatchExpressionForTest lhs("{z: 2, y: 1}");
        ASSERT_FALSE(expression::isSubsetOf(lhs.get(), complex.get()));
    }

    {
        ParsedMatchExpressionForTest lhs("{x: 1, y: 3}");
        ASSERT_FALSE(expression::isSubsetOf(lhs.get(), complex.get()));
    }
}

TEST(ExpressionAlgoIsSubsetOf, IsSubsetOfRHSDisjunction) {
    ParsedMatchExpressionForTest rhs("{$or: [{a: 1}, {a: {$gt: 20}}]}");
    {
        ParsedMatchExpressionForTest lhs("{$or: [{a: 1}, {a: {$gt: 20}}], b: 5}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), rhs.get()));
    }
    {
        ParsedMatchExpressionForTest lhs("{$or: [{a: 1}, {a: {$gt: 30}}]}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), rhs.get()));
    }
    {
        // {a: 11, b: 5} is a document which satisfies this predicate but not the rhs.
        ParsedMatchExpressionForTest lhs("{$or: [{a: 1}, {a: {$gt: 10}}], b: 5}");
        ASSERT_FALSE(expression::isSubsetOf(lhs.get(), rhs.get()));
    }
}

TEST(IsIndependent, AndIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{$and: [{a: 1}, {b: 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOfConst(*expr.get(), {"b"}));
    ASSERT_TRUE(expression::isIndependentOfConst(*expr.get(), {"c"}));
}

TEST(IsIndependent, EqNullIsIndependentOnlyIfToplevelFieldDiffers) {
    // This predicate is not independent from 'a.x', because {$set: {'a.x': _}}
    // can transform some 'a' values from scalar to object. But it is independent
    // from 'x' or 'x.y', because the top-level field is different.
    //
    // See also 'jstests/aggregation/sources/addFields/independence.js'.
    BSONObj matchPredicate = fromjson("{'a.b': {$eq: null}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOfConst(*expr.get(), {"a.x"}));
    ASSERT_TRUE(expression::isIndependentOfConst(*expr.get(), {"x"}));
    ASSERT_TRUE(expression::isIndependentOfConst(*expr.get(), {"x.y"}));
}

TEST(IsIndependent, ElemMatchIsIndependent) {
    BSONObj matchPredicate = fromjson("{x: {$elemMatch: {y: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOfConst(*expr.get(), {"x"}));
    ASSERT_FALSE(expression::isIndependentOfConst(*expr.get(), {"x.y"}));
    ASSERT_TRUE(expression::isIndependentOfConst(*expr.get(), {"y"}));
}

TEST(IsIndependent, NorIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{$nor: [{a: 1}, {b: 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOfConst(*expr.get(), {"b"}));
    ASSERT_TRUE(expression::isIndependentOfConst(*expr.get(), {"c"}));
}

TEST(IsIndependent, NotIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{a: {$not: {$eq: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_TRUE(expression::isIndependentOfConst(*expr.get(), {"b"}));
    ASSERT_FALSE(expression::isIndependentOfConst(*expr.get(), {"a"}));
}

TEST(IsIndependent, OrIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{$or: [{a: 1}, {b: 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOfConst(*expr.get(), {"a"}));
    ASSERT_TRUE(expression::isIndependentOfConst(*expr.get(), {"c"}));
}

TEST(IsIndependent, AndWithDottedFieldPathsIsNotIndependent) {
    BSONObj matchPredicate = fromjson("{$and: [{'a': 1}, {'a.b': 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOfConst(*expr.get(), {"a.b.c"}));
    ASSERT_FALSE(expression::isIndependentOfConst(*expr.get(), {"a.b"}));
}

TEST(IsIndependent, BallIsIndependentOfBalloon) {
    BSONObj matchPredicate = fromjson("{'a.ball': 4}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_TRUE(expression::isIndependentOfConst(*expr.get(), {"a.balloon"}));
    ASSERT_TRUE(expression::isIndependentOfConst(*expr.get(), {"a.b"}));
    ASSERT_FALSE(expression::isIndependentOfConst(*expr.get(), {"a.ball.c"}));
}

// This is a descriptive test to ensure that until renames are implemented for these expressions,
// matches on these expressions cannot be swapped with other stages.
TEST(IsIndependent, NonRenameableExpressionIsNotIndependent) {
    std::vector<std::string> stringExpressions = {
        // Category: kOther.
        "{$or: [{a: {$_internalSchemaObjectMatch: {b: 1}}},"
        "       {a: {$_internalSchemaObjectMatch: {b: 2}}}]}",
    };

    for (const auto& str : stringExpressions) {
        BSONObj matchPredicate = fromjson(str);
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
        ASSERT_OK(swMatchExpression.getStatus());
        auto matchExpression = std::move(swMatchExpression.getValue());

        // Both of these should be true once renames are implemented.
        ASSERT_FALSE(expression::isIndependentOfConst(*matchExpression.get(), {"c"}));
        ASSERT_FALSE(expression::isOnlyDependentOnConst(*matchExpression.get(), {"a", "b"}));
    }
}

TEST(IsIndependent, EmptyDependencySetsPassIsOnlyDependentOn) {
    BSONObj matchPredicate = fromjson("{}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    auto matchExpression = std::move(swMatchExpression.getValue());
    ASSERT_TRUE(expression::isOnlyDependentOnConst(*matchExpression.get(), {}));
}

TEST(ContainsOverlappingPaths, Basics) {
    // No overlap cases.
    ASSERT_FALSE(expression::containsOverlappingPaths({}));
    ASSERT_FALSE(expression::containsOverlappingPaths({"a"}));
    ASSERT_FALSE(expression::containsOverlappingPaths({"a.b"}));
    ASSERT_FALSE(expression::containsOverlappingPaths({"a", "b"}));
    ASSERT_FALSE(expression::containsOverlappingPaths({"a", "ab", "a-b"}));
    ASSERT_FALSE(expression::containsOverlappingPaths({"a.b", "ab", "a-b"}));
    ASSERT_FALSE(expression::containsOverlappingPaths({"a.b", "a.c", "a.d"}));
    ASSERT_FALSE(
        expression::containsOverlappingPaths({"users.address.zip", "users.address.state"}));

    // Overlap cases
    ASSERT_TRUE(expression::containsOverlappingPaths({"a.b", "ab", "a-b", "a"}));
    ASSERT_TRUE(expression::containsOverlappingPaths({"a.b", "ab", "a-b", "a.b.c"}));
    ASSERT_TRUE(expression::containsOverlappingPaths({"a", "ab", "a-b", "a.b.c"}));
    ASSERT_TRUE(expression::containsOverlappingPaths({"users.address", "users.address.state"}));
}

TEST(ContainsEmptyPaths, Basics) {
    // No empty paths.
    ASSERT_FALSE(expression::containsEmptyPaths({}));
    ASSERT_FALSE(expression::containsEmptyPaths({"a", "a.b"}));

    // Empty paths.
    ASSERT_TRUE(expression::containsEmptyPaths({""}));
    ASSERT_TRUE(expression::containsEmptyPaths({"."}));
    ASSERT_TRUE(expression::containsEmptyPaths({"a."}));
    ASSERT_TRUE(expression::containsEmptyPaths({".a"}));
    ASSERT_TRUE(expression::containsEmptyPaths({"a..b"}));
    ASSERT_TRUE(expression::containsEmptyPaths({".."}));

    // Mixed.
    ASSERT_TRUE(expression::containsEmptyPaths({"a", ""}));
    ASSERT_TRUE(expression::containsEmptyPaths({"", "a"}));
}

TEST(SplitMatchExpression, AndWithSplittableChildrenIsSplittable) {
    BSONObj matchPredicate = fromjson("{$and: [{a: 1}, {b: 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"b"}, {});

    ASSERT_TRUE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());

    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(), fromjson("{a: {$eq: 1}}"));
    ASSERT_BSONOBJ_EQ(splitExpr.second->serialize(), fromjson("{b: {$eq: 1}}"));
}

TEST(SplitMatchExpression, NorWithIndependentChildrenIsSplittable) {
    BSONObj matchPredicate = fromjson("{$nor: [{a: 1}, {b: 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"b"}, {});

    ASSERT_TRUE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());

    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(), fromjson("{$nor: [{a: {$eq: 1}}]}"));
    ASSERT_BSONOBJ_EQ(splitExpr.second->serialize(), fromjson("{$nor: [{b: {$eq: 1}}]}"));
}

TEST(SplitMatchExpression, NotWithIndependentChildIsSplittable) {
    BSONObj matchPredicate = fromjson("{x: {$not: {$gt: 4}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"y"}, {});

    ASSERT_TRUE(splitExpr.first.get());

    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(), fromjson("{x: {$not: {$gt: 4}}}"));
    ASSERT_FALSE(splitExpr.second);
}

TEST(SplitMatchExpression, OrWithOnlyIndependentChildrenIsNotSplittable) {
    BSONObj matchPredicate = fromjson("{$or: [{a: 1}, {b: 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"b"}, {});

    ASSERT_TRUE(splitExpr.second.get());

    ASSERT_FALSE(splitExpr.first);
    ASSERT_BSONOBJ_EQ(splitExpr.second->serialize(),
                      fromjson("{$or: [{a: {$eq: 1}}, {b: {$eq: 1}}]}"));
}

TEST(SplitMatchExpression, ComplexMatchExpressionSplitsCorrectly) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{x: {$not: {$size: 2}}},"
        "{$or: [{'a.b' : 3}, {'a.b.c': 4}]},"
        "{$nor: [{x: {$gt: 4}}, {$and: [{x: {$not: {$eq: 1}}}, {y: 3}]}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"x"}, {});

    ASSERT_TRUE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());

    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(),
                      fromjson("{$or: [{'a.b': {$eq: 3}}, {'a.b.c': {$eq: 4}}]}"));
    ASSERT_BSONOBJ_EQ(splitExpr.second->serialize(),
                      fromjson("{$and: [{x: {$not: {$size: 2}}}, {$nor: [{x: {$gt: 4}}, {$and: "
                               "[{x: {$not: {$eq: 1}}}, {y: {$eq: 3}}]}]}]}"));
}


TEST(SplitMatchExpression, ShouldNotExtractPrefixOfDottedPathAsIndependent) {
    BSONObj matchPredicate = fromjson("{$and: [{a: 1}, {'a.b': 1}, {'a.c': 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"a.b"}, {});

    ASSERT_TRUE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());

    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(), fromjson("{'a.c': {$eq: 1}}"));
    ASSERT_BSONOBJ_EQ(splitExpr.second->serialize(),
                      fromjson("{$and: [{a: {$eq: 1}}, {'a.b': {$eq: 1}}]}"));
}

TEST(SplitMatchExpression, ShouldMoveIndependentLeafPredicateAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: 1}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "b"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitExpr.first.get());
    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(), fromjson("{b: {$eq: 1}}"));

    ASSERT_FALSE(splitExpr.second.get());
}

TEST(SplitMatchExpression, ShouldMoveIndependentAndPredicateAcrossRename) {
    BSONObj matchPredicate = fromjson("{$and: [{a: 1}, {b: 2}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitExpr.first.get());
    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(),
                      fromjson("{$and: [{c: {$eq: 1}}, {b: {$eq: 2}}]}"));

    ASSERT_FALSE(splitExpr.second.get());
}

TEST(SplitMatchExpression, ShouldSplitPartiallyDependentAndPredicateAcrossRename) {
    BSONObj matchPredicate = fromjson("{$and: [{a: 1}, {b: 2}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {"b"}, renames);

    ASSERT_TRUE(splitExpr.first.get());
    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(), fromjson("{c: {$eq: 1}}"));

    ASSERT_TRUE(splitExpr.second.get());
    ASSERT_BSONOBJ_EQ(splitExpr.second->serialize(), fromjson("{b: {$eq: 2}}"));
}

TEST(SplitMatchExpression, ShouldSplitPartiallyDependentComplexPredicateMultipleRenames) {
    BSONObj matchPredicate = fromjson("{$and: [{a: 1}, {$or: [{b: 2}, {c: 3}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"b", "d"}, {"c", "e"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {"a"}, renames);

    ASSERT_TRUE(splitExpr.first.get());
    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(),
                      fromjson("{$or: [{d: {$eq: 2}}, {e: {$eq: 3}}]}"));

    ASSERT_TRUE(splitExpr.second.get());
    ASSERT_BSONOBJ_EQ(splitExpr.second->serialize(), fromjson("{a: {$eq: 1}}"));
}

TEST(SplitMatchExpression,
     ShouldSplitPartiallyDependentComplexPredicateMultipleRenamesDottedPaths) {
    BSONObj matchPredicate = fromjson("{$and: [{a: 1}, {$or: [{'d.e.f': 2}, {'e.f.g': 3}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"d.e.f", "x"}, {"e.f.g", "y"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {"a"}, renames);

    ASSERT_TRUE(splitExpr.first.get());
    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(),
                      fromjson("{$or: [{x: {$eq: 2}}, {y: {$eq: 3}}]}"));

    ASSERT_TRUE(splitExpr.second.get());
    ASSERT_BSONOBJ_EQ(splitExpr.second->serialize(), fromjson("{a: {$eq: 1}}"));
}

TEST(SplitMatchExpression, ShouldMoveElemMatchObjectAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$elemMatch: {b: 3}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    auto [splitOutExpr, residualExpr] =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson("{c: {$elemMatch: {b: {$eq: 3}}}}"));

    ASSERT_FALSE(residualExpr.get());
}

TEST(SplitMatchExpression, ShouldNotMoveDependentElemMatchObjectAcrossRename) {
    auto matchPredicateDependentFieldAndExpected = {
        // Assumes that this match expression follows {$project: {x: "$a.b"}} and the path 'x' is a
        // modified one.
        std::make_tuple(fromjson(R"({"x": {$elemMatch: {c: 3}}})"),
                        "x"s,
                        fromjson(R"({"x": {$elemMatch: {c: {$eq: 3}}}})")),
        // Assumes that this match expression follows {$project: {x: {y: {"$a.b"}}}} and the path
        // 'x.y' is a modified one.
        std::make_tuple(fromjson(R"({"x.y": {$elemMatch: {c: 3}}})"),
                        "x.y"s,
                        fromjson(R"({"x.y": {$elemMatch: {c: {$eq: 3}}}})")),
        // Assumes that this match expression follows {$project: {x: {y: {z: {"$a.b"}}}}} and the
        // path 'x.y.z' is a modified one.
        std::make_tuple(fromjson(R"({"x.y.z": {$elemMatch: {c: 3}}})"),
                        "x.y.z"s,
                        fromjson(R"({"x.y.z": {$elemMatch: {c: {$eq: 3}}}})")),
    };

    for (const auto& [matchPredicate, field, expected] : matchPredicateDependentFieldAndExpected) {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
        ASSERT_OK(matcher.getStatus());

        // The 'x' is a modified path by $project and the match expression is dependent on 'x' and
        // not splittable.
        auto [splitOutExpr, residualExpr] =
            expression::splitMatchExpressionBy(std::move(matcher.getValue()), {field}, {});

        ASSERT_FALSE(splitOutExpr.get());

        ASSERT_TRUE(residualExpr.get());
        ASSERT_BSONOBJ_EQ(residualExpr->serialize(), expected);
    }
}

TEST(SplitMatchExpression, ShouldNotMoveElemMatchObjectOnModifiedDependencyAcrossRename) {
    // Assumes that this match expression follows {$project: {x: {$map: {input: "$a", as:
    // "iter", in: {y: "$$iter.b"}}}}}
    auto matchPredicate = fromjson(R"({"x": {$elemMatch: {y: 3}}})");
    auto expected = fromjson(R"({"x": {$elemMatch: {y: {$eq: 3}}}})");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    // The 'x' is a modified dependency by $project and the match expression is dependent on 'x' and
    // not splittable.
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        std::move(matcher.getValue()), {"x"s}, {{"x.y"s, "a.b"s}});

    ASSERT_FALSE(splitOutExpr.get());

    ASSERT_TRUE(residualExpr.get());
    ASSERT_BSONOBJ_EQ(residualExpr->serialize(), expected);
}

TEST(SplitMatchExpression, ShouldNotMoveEqAcrossAttemptedButFailedRename) {
    // Assumes that this match expression follows {$project: {x: {$map: {input: "$a", as: "i", in:
    // {y: "$$i.b"}}}}}
    auto matchPredicate = fromjson(R"({x: {y: 3}})");
    auto expected = fromjson(R"({x: {$eq: {y: 3}}})");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    // Here empty 'fields' means that the previous stage is a simple rename. Due to the limitation
    // of the current renaming algorithm, 'x' and 'y' are not renamed into 'a' and 'b' respectively
    // which is classified as "attempted but failed to" rename. In such a case,
    // splitMatchExpressionBy() returns the original match expression as 'residualExpr'.
    //
    // TODO SERVER-74298 Implement renaming by each path component for sub-classes of
    // PathMatchExpression so that such match expressions can be split out.
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        std::move(matcher.getValue()), /*fields*/ {}, /*renames*/ {{"x.y"s, "a.b"s}});

    ASSERT_FALSE(splitOutExpr.get())
        << splitOutExpr->serialize()
        << ", residual: " << (residualExpr ? residualExpr->serialize().toString() : "nullptr"s);

    ASSERT_TRUE(residualExpr.get());
    ASSERT_BSONOBJ_EQ(residualExpr->serialize(), expected);
}

TEST(SplitMatchExpression, ShouldNotMoveElemMatchObjectAcrossAttemptedButFailedRename) {
    // Assumes that this match expression follows {$project: {x: {$map: {input: "$a", as: "i", in:
    // {y: "$$i.b"}}}}} which is a simple rename.
    auto matchPredicate = fromjson(R"({"x": {$elemMatch: {y: 3}}})");
    auto expected = fromjson(R"({"x": {$elemMatch: {y: {$eq: 3}}}})");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    // Here empty 'fields' means that the previous stage is a simple rename. Due to the limitation
    // of the current renaming algorithm, 'x' and 'y' are not renamed into 'a' and 'b' respectively
    // which is classified as "attempted but failed to" rename. In such a case,
    // splitMatchExpressionBy() returns the original match expression as 'residualExpr'.
    //
    // TODO SERVER-74298 Implement renaming by each path component for sub-classes of
    // PathMatchExpression so that such match expressions can be split out.
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        std::move(matcher.getValue()), /*fields*/ {}, /*renames*/ {{"x.y"s, "a.b"s}});

    ASSERT_FALSE(splitOutExpr.get())
        << splitOutExpr->serialize()
        << ", residual: " << (residualExpr ? residualExpr->serialize().toString() : "nullptr"s);

    ASSERT_TRUE(residualExpr.get());
    ASSERT_BSONOBJ_EQ(residualExpr->serialize(), expected);
}

TEST(SplitMatchExpression, ShouldMoveElemMatchObjectOnRenamedPathForDottedPathAcrossRename) {
    auto matchPredicateRenameAndExpected = {
        // Assumes that this match expression follows {$project: {a: "$x"}}} which is a simple
        // rename.
        std::make_tuple(fromjson(R"({"a.b": {$elemMatch: {c: 3}}})"),
                        std::make_pair("a"s, "x"s),
                        fromjson(R"({"x.b": {$elemMatch: {c: {$eq: 3}}}})")),
        // Assumes that this match expression follows {$project: {a: {b: {"$x"}}}} which is a simple
        // rename.
        std::make_tuple(fromjson(R"({"a.b": {$elemMatch: {c: 3}}})"),
                        std::make_pair("a.b"s, "x"s),
                        fromjson(R"({"x": {$elemMatch: {c: {$eq: 3}}}})")),
        // Assumes that this match expression follows {$project: {a: {b: {c: {"$x"}}}}} which is a
        // simple rename.
        std::make_tuple(fromjson(R"({"a.b.c": {$elemMatch: {d: 3}}})"),
                        std::make_pair("a.b.c"s, "x"s),
                        fromjson(R"({"x": {$elemMatch: {d: {$eq: 3}}}})")),
    };

    for (const auto& [matchPredicate, rename, expected] : matchPredicateRenameAndExpected) {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
        ASSERT_OK(matcher.getStatus());

        StringMap<std::string> renames{rename};
        auto [splitOutExpr, residualExpr] =
            expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

        ASSERT_TRUE(splitOutExpr.get());
        ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), expected);

        ASSERT_FALSE(residualExpr.get());
    }
}

TEST(SplitMatchExpression, ShouldMoveElemMatchValueAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$elemMatch: {$eq: 3}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    auto [splitOutExpr, residualExpr] =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson("{c: {$elemMatch: {$eq: 3}}}"));

    ASSERT_FALSE(residualExpr.get());
}

TEST(SplitMatchExpression, ShouldMoveElemMatchValueForDottedPathAcrossRename) {
    BSONObj matchPredicate = fromjson(R"({"a.b": {$elemMatch: {$eq: 3}}})");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    auto [splitOutExpr, residualExpr] =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson(R"({"c.b": {$elemMatch: {$eq: 3}}})"));

    ASSERT_FALSE(residualExpr.get());
}

TEST(SplitMatchExpression, ShouldMoveTypeAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$type: 16}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(), fromjson("{c: {$type: [16]}}"));

    ASSERT_FALSE(splitExpr.second.get());
}

TEST(SplitMatchExpression, ShouldMoveSizeAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$size: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    auto [splitOutExpr, residualExpr] =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson("{c: {$size: 3}}"));

    ASSERT_FALSE(residualExpr.get());
}

TEST(SplitMatchExpression, ShouldNotMoveDependentSizeAcrossRename) {
    // Assumes that this match expression follows {$project: {x: "$a.b"}}.
    BSONObj matchPredicate = fromjson("{x: {$size: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"x", "a.b"}};
    // The 'x' is a modified path by $project and the match expression is dependent on 'x' and not
    // splittable.
    auto [splitOutExpr, residualExpr] =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {"x"}, renames);

    ASSERT_FALSE(splitOutExpr.get());

    ASSERT_TRUE(residualExpr.get());
    ASSERT_BSONOBJ_EQ(residualExpr->serialize(), fromjson("{x: {$size: 3}}"));
}

TEST(SplitMatchExpression, ShouldMoveMinItemsAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$_internalSchemaMinItems: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    auto [splitOutExpr, residualExpr] =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson("{c: {$_internalSchemaMinItems: 3}}"));

    ASSERT_FALSE(residualExpr.get());
}

TEST(SplitMatchExpression, ShouldMoveMaxItemsAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$_internalSchemaMaxItems: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    auto [splitOutExpr, residualExpr] =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson("{c: {$_internalSchemaMaxItems: 3}}"));

    ASSERT_FALSE(residualExpr.get());
}

TEST(SplitMatchExpression, ShouldMoveMaxItemsInLogicalExpressionAcrossRename) {
    BSONObj matchPredicate = fromjson(
        "{$or: [{a: {$_internalSchemaMaxItems: 3}},"
        "       {a: {$_internalSchemaMaxItems: 4}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    auto [splitOutExpr, residualExpr] =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(),
                      fromjson("{$or: [{c: {$_internalSchemaMaxItems: 3}},"
                               "       {c: {$_internalSchemaMaxItems: 4}}]}"));

    ASSERT_FALSE(residualExpr.get());
}

TEST(SplitMatchExpression, ShouldNotMoveInternalSchemaObjectMatchInLogicalExpressionAcrossRename) {
    BSONObj matchPredicate = fromjson(
        "{$or: [{a: {$_internalSchemaObjectMatch: {b: 1}}},"
        "       {a: {$_internalSchemaObjectMatch: {b: 1}}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_FALSE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());
    ASSERT_BSONOBJ_EQ(splitExpr.second->serialize(),
                      fromjson("{$or: [{a: {$_internalSchemaObjectMatch: {b: {$eq: 1}}}},"
                               "       {a: {$_internalSchemaObjectMatch: {b: {$eq: 1}}}}]}"));
}

TEST(SplitMatchExpression, ShouldMoveMinLengthAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$_internalSchemaMinLength: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitExpr.first.get());
    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(),
                      fromjson("{c: {$_internalSchemaMinLength: 3}}"));

    ASSERT_FALSE(splitExpr.second.get());
}

TEST(SplitMatchExpression, ShouldMoveMaxLengthAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$_internalSchemaMaxLength: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitExpr.first.get());
    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(),
                      fromjson("{c: {$_internalSchemaMaxLength: 3}}"));

    ASSERT_FALSE(splitExpr.second.get());
}

TEST(SplitMatchExpression, ShouldMoveIndependentPredicateWhenThereAreMultipleRenames) {
    // Designed to reproduce SERVER-32690.
    BSONObj matchPredicate = fromjson("{y: 3}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"y", "x"}, {"x", "x"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitExpr.first.get());
    ASSERT_BSONOBJ_EQ(splitExpr.first->serialize(), fromjson("{x: {$eq: 3}}"));

    ASSERT_FALSE(splitExpr.second.get());
}

TEST(SplitMatchExpression, ShouldNotSplitWhenRand) {
    const auto randExpr = "{$expr: {$lt: [{$rand: {}}, {$const: 0.25}]}}";
    const auto assertMatchDoesNotSplit = [&](const std::string& exprString) {
        BSONObj matchPredicate = fromjson(exprString);
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
        ASSERT_OK(matcher.getStatus());

        auto&& [split, residual] =
            expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, {});
        ASSERT_FALSE(split.get());
        ASSERT_TRUE(residual.get());

        ASSERT_BSONOBJ_EQ(residual->serialize(), fromjson(randExpr));
    };

    // We should not push down a $match with a $rand expression.
    assertMatchDoesNotSplit(randExpr);

    // This is equivalent to 'randExpr'.
    assertMatchDoesNotSplit("{$sampleRate: 0.25}");
}

TEST(SplitMatchExpression, ShouldSplitJsonSchemaRequiredByMetaField) {
    ParsedMatchExpressionForTest matcher(R"({$and: [{b: 1}, {$jsonSchema: {required: ["a"]}}]})");
    auto [splitOutExpr, residualExpr] =
        expression::splitMatchExpressionBy(matcher.release(), {"a"}, {});

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson("{b: {$eq: 1}}"));

    ASSERT_TRUE(residualExpr.get());
    ASSERT_BSONOBJ_EQ(residualExpr->serialize(), fromjson("{a: {$exists: true}}"));
}

TEST(SplitMatchExpression,
     ShouldSplitOutAndRenameJsonSchemaRequiredAndTheRestIsNullByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher(R"({$jsonSchema: {required: ["a"]}})");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta".
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(),
                      fromjson("{$and: [{$and: [{meta: {$exists: true}}]}]}"));

    ASSERT_FALSE(residualExpr.get());
}

TEST(SplitMatchExpression,
     ShouldSplitOutAndRenameJsonSchemaRequiredAndTheRestIs_NOT_NullByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher(R"({$jsonSchema: {required: ["a", "b"]}})");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta". The expression for the non-meta field "b" remains.
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson("{meta: {$exists: true}}"));

    ASSERT_TRUE(residualExpr.get());
    ASSERT_BSONOBJ_EQ(residualExpr->serialize(), fromjson("{b: {$exists: true}}"));
}

TEST(SplitMatchExpression, ShouldSplitOutAndRenameJsonSchemaRequiredByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher(R"({$and: [{b: 1}, {$jsonSchema: {required: ["a"]}}]})");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta". We have a residual expression too in this test case.
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(),
                      fromjson("{$and: [{$and: [{meta: {$exists: true}}]}]}"));

    ASSERT_TRUE(residualExpr.get());
    ASSERT_BSONOBJ_EQ(residualExpr->serialize(), fromjson("{b: {$eq: 1}}"));
}

// Verifies that $jsonSchema 'type' is supported by splitMatchExpressionBy().
TEST(SplitMatchExpression, ShouldSplitOutAndRenameJsonSchemaTypeByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher(R"({$jsonSchema: {properties: {a: {type: "number"}}}})");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta".
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson(R"(
{
    $and: [{
        $and: [{
            $or: [
                {meta: {$not: {$exists: true}}},
                {$and: [{meta: {$_internalSchemaType: ["number"]}}]}
            ]
        }]
    }]
}
        )"));

    ASSERT_FALSE(residualExpr.get());
}

// Verifies that $jsonSchema 'bsonType' is supported by splitMatchExpressionBy().
TEST(SplitMatchExpression, ShouldSplitOutAndRenameJsonSchemaBsonTypeByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher(R"({$jsonSchema: {properties: {a: {bsonType: "long"}}}})");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta".
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson(R"(
{
    $and: [{
        $and: [
            {$or: [{meta: {$not: {$exists: true}}}, {$and: [{meta: {$_internalSchemaType: [18]}}]}]}
        ]
    }]
}
        )"));

    ASSERT_FALSE(residualExpr.get());
}

// Verifies that $jsonSchema 'enum' is supported by splitMatchExpressionBy().
TEST(SplitMatchExpression, ShouldSplitOutAndRenameJsonSchemaEnumByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher(R"({$jsonSchema: {properties: {a: {enum: [1,2]}}}})");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta".
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson(R"(
{
    $and: [{
        $and: [{
            $or: [
                {meta: {$not: {$exists: true}}},
                {$and: [{$or: [{meta: {$_internalSchemaEq: 1}}, {meta: {$_internalSchemaEq: 2}}]}]}
            ]
        }]
    }]
}
        )"));

    ASSERT_FALSE(residualExpr.get());
}

// Verifies that $jsonSchema 'minimum' and 'maximum' are supported by splitMatchExpressionBy().
TEST(SplitMatchExpression, ShouldSplitOutAndRenameJsonSchemaMinMaxByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher(
        "{$jsonSchema: {properties: {a: {minimum: 1, maximum: 10}}}}");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta".
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson(R"(
{
    $and: [{
        $and: [{
            $or: [
                {meta: {$not: {$exists: true}}},
                {
                    $and: [
                        {
                            $or: [
                                {meta: {$not: {$_internalSchemaType: ["number"]}}},
                                {meta: {$lte: 10}}
                            ]
                        },
                        {
                            $or: [
                                {meta: {$not: {$_internalSchemaType: ["number"]}}},
                                {meta: {$gte: 1}}
                            ]
                        }
                    ]
                }
            ]
        }]
    }]
}
        )"));

    ASSERT_FALSE(residualExpr.get());
}

// Verifies that $jsonSchema 'minLength' and 'maxLength' are supported by splitMatchExpressionBy().
TEST(SplitMatchExpression, ShouldSplitOutAndRenameJsonSchemaMinMaxLengthByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher(
        "{$jsonSchema: {properties: {a: {minLength: 1, maxLength: 10}}}}");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta".
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson(R"(
{
    $and: [{
        $and: [{
            $or: [
                {meta: {$not: {$exists: true}}},
                {
                    $and: [
                        {
                            $or: [
                                {meta: {$not: {$_internalSchemaType: [2]}}},
                                {meta: {$_internalSchemaMaxLength: 10}}
                            ]
                        },
                        {
                            $or: [
                                {meta: {$not: {$_internalSchemaType: [2]}}},
                                {meta: {$_internalSchemaMinLength: 1}}
                            ]
                        }
                    ]
                }
            ]
        }]
    }]
}
        )"));

    ASSERT_FALSE(residualExpr.get());
}

// Verifies that $jsonSchema 'multipleOf' is supported by splitMatchExpressionBy().
TEST(SplitMatchExpression, ShouldSplitOutAndRenameJsonSchemaMultipleOfByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher("{$jsonSchema: {properties: {a: {multipleOf: 10}}}}");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta".
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson(R"(
{
    $and: [{
        $and: [{
            $or: [
                {meta: {$not: {$exists: true}}},
                {
                    $and: [{
                        $or: [
                            {meta: {$not: {$_internalSchemaType: ["number"]}}},
                            {meta: {$_internalSchemaFmod: [10, 0]}}
                        ]
                    }]
                }
            ]
        }]
    }]
}
        )"));

    ASSERT_FALSE(residualExpr.get());
}

// Verifies that $jsonSchema 'pattern' is supported by splitMatchExpressionBy().
TEST(SplitMatchExpression, ShouldSplitOutAndRenameJsonSchemaPatternByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher(
        R"({$jsonSchema: {properties: {a: {pattern: "[0-9]*"}}}})");
    auto originalExpr = matcher.release();
    auto originalExprCopy = originalExpr->clone();

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta".
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        std::move(originalExpr), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    // 'splitOutExpr' must be same as the expression after renaming 'a' to 'meta'.
    expression::Renameables renameables;
    ASSERT_TRUE(
        expression::isOnlyDependentOn(*originalExprCopy, {"a"}, {{"a", "meta"}}, renameables));
    expression::applyRenamesToExpression({{"a", "meta"}}, &renameables);
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), originalExprCopy->serialize());

    ASSERT_FALSE(residualExpr.get());
}

// Verifies that $jsonSchema 'uniqueItems' is supported by splitMatchExpressionBy().
TEST(SplitMatchExpression, ShouldSplitOutAndRenameJsonSchemaUniqueItemsByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher("{$jsonSchema: {properties: {a: {uniqueItems: true}}}}");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta".
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson(R"(
{
    $and: [{
        $and: [{
            $or: [
                {meta: {$not: {$exists: true}}},
                {
                    $and: [{
                        $or: [
                            {meta: {$not: {$_internalSchemaType: [4]}}},
                            {meta: {$_internalSchemaUniqueItems: true}}
                        ]
                    }]
                }
            ]
        }]
    }]
}
        )"));

    ASSERT_FALSE(residualExpr.get());
}

// Verifies that $jsonSchema 'minItems' and 'maxItems' is supported by splitMatchExpressionBy().
TEST(SplitMatchExpression, ShouldSplitOutAndRenameJsonSchemaMinMaxItemsByIsOnlyDependentOn) {
    ParsedMatchExpressionForTest matcher(
        "{$jsonSchema: {properties: {a: {minItems: 1, maxItems: 10}}}}");

    // $jsonSchema expression will be split out by the meta field "a" and the meta field "a" will be
    // renamed to "meta".
    auto [splitOutExpr, residualExpr] = expression::splitMatchExpressionBy(
        matcher.release(), {"a"}, {{"a", "meta"}}, expression::isOnlyDependentOn);

    ASSERT_TRUE(splitOutExpr.get());
    ASSERT_BSONOBJ_EQ(splitOutExpr->serialize(), fromjson(R"(
{
    $and: [{
        $and: [{
            $or: [
                {meta: {$not: {$exists: true}}},
                {
                    $and: [
                        {
                            $or: [
                                {meta: {$not: {$_internalSchemaType: [4]}}},
                                {meta: {$_internalSchemaMinItems: 1}}
                            ]
                        },
                        {
                            $or: [
                                {meta: {$not: {$_internalSchemaType: [4]}}},
                                {meta: {$_internalSchemaMaxItems: 10}}
                            ]
                        }
                    ]
                }
            ]
        }]
    }]
}
        )"));

    ASSERT_FALSE(residualExpr.get());
}

TEST(ApplyRenamesToExpression, ShouldApplyBasicRenamesForAMatchWithExpr) {
    BSONObj matchPredicate = fromjson("{$expr: {$eq: ['$a.b', '$c']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "d"}, {"c", "e"}, {"x", "y"}};
    auto renamedExpr = expression::copyExpressionAndApplyRenames(matcher.getValue().get(), renames);
    ASSERT_TRUE(renamedExpr);

    ASSERT_BSONOBJ_EQ(renamedExpr->serialize(), fromjson("{$expr: {$eq: ['$d.b', '$e']}}"));
}

TEST(ApplyRenamesToExpression, ShouldApplyDottedRenamesForAMatch) {
    BSONObj matchPredicate = fromjson("{x: 2}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"x", "subDocument.x"}};
    auto renamedExpr = expression::copyExpressionAndApplyRenames(matcher.getValue().get(), renames);
    ASSERT_TRUE(renamedExpr);

    ASSERT_BSONOBJ_EQ(renamedExpr->serialize(), fromjson("{'subDocument.x': {$eq: 2}}"));
}

TEST(ApplyRenamesToExpression, ShouldApplyDottedRenamesForAMatchWithExpr) {
    BSONObj matchPredicate = fromjson("{$expr: {$lt: ['$a.b.c', '$d.e.f']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a.b.c", "x"}, {"d.e", "y"}};
    auto renamedExpr = expression::copyExpressionAndApplyRenames(matcher.getValue().get(), renames);
    ASSERT_TRUE(renamedExpr);

    ASSERT_BSONOBJ_EQ(renamedExpr->serialize(), fromjson("{$expr: {$lt: ['$x', '$y.f']}}"));
}

TEST(ApplyRenamesToExpression, ShouldApplyDottedRenamesForAMatchWithNestedExpr) {
    BSONObj matchPredicate =
        fromjson("{$and: [{$expr: {$eq: ['$a.b.c', '$c']}}, {$expr: {$lt: ['$d.e.f', '$a']}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "x.y"}, {"d.e", "y"}, {"c", "q.r"}};
    auto renamedExpr = expression::copyExpressionAndApplyRenames(matcher.getValue().get(), renames);
    ASSERT_TRUE(renamedExpr);

    ASSERT_BSONOBJ_EQ(
        renamedExpr->serialize(),
        fromjson(
            "{$and: [{$expr: {$eq: ['$x.y.b.c', '$q.r']}}, {$expr: {$lt: ['$y.f', '$x.y']}}]}"));
}

TEST(ApplyRenamesToExpression, ShouldNotApplyRenamesForAMatchWithExprWithNoFieldPaths) {
    BSONObj matchPredicate = fromjson("{$expr: {$concat: ['a', 'b', 'c']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "x.y"}, {"d.e", "y"}, {"c", "q.r"}};
    auto renamedExpr = expression::copyExpressionAndApplyRenames(matcher.getValue().get(), renames);
    ASSERT_TRUE(renamedExpr);

    ASSERT_BSONOBJ_EQ(
        renamedExpr->serialize(),
        fromjson("{$expr: {$concat: [{$const: 'a'}, {$const: 'b'}, {$const: 'c'}]}}"));
}

TEST(MapOverMatchExpression, DoesMapOverLogicalNodes) {
    BSONObj matchPredicate = fromjson("{a: {$not: {$eq: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());

    bool hasLogicalNode = false;
    expression::mapOver(swMatchExpression.getValue().get(),
                        [&hasLogicalNode](MatchExpression* expression, std::string path) -> void {
                            if (expression->getCategory() ==
                                MatchExpression::MatchCategory::kLogical) {
                                hasLogicalNode = true;
                            }
                        });

    ASSERT_TRUE(hasLogicalNode);
}

TEST(MapOverMatchExpression, DoesMapOverLeafNodes) {
    BSONObj matchPredicate = fromjson("{a: {$not: {$eq: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());

    bool hasLeafNode = false;
    expression::mapOver(swMatchExpression.getValue().get(),
                        [&hasLeafNode](MatchExpression* expression, std::string path) -> void {
                            if (expression->getCategory() !=
                                MatchExpression::MatchCategory::kLogical) {
                                hasLeafNode = true;
                            }
                        });

    ASSERT_TRUE(hasLeafNode);
}

TEST(MapOverMatchExpression, DoesPassPath) {
    BSONObj matchPredicate = fromjson("{a: {$elemMatch: {b: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());

    std::vector<std::string> paths;
    expression::mapOver(swMatchExpression.getValue().get(),
                        [&paths](MatchExpression* expression, std::string path) -> void {
                            if (!expression->numChildren()) {
                                paths.push_back(path);
                            }
                        });

    ASSERT_EQ(paths.size(), 1U);
    ASSERT_EQ(paths[0], "a.b");
}

TEST(MapOverMatchExpression, DoesMapOverNodesWithMultipleChildren) {
    BSONObj matchPredicate = fromjson("{$and: [{a: {$gt: 1}}, {b: {$lte: 2}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());

    size_t nodeCount = 0;
    expression::mapOver(
        swMatchExpression.getValue().get(),
        [&nodeCount](MatchExpression* expression, std::string path) -> void { ++nodeCount; });

    ASSERT_EQ(nodeCount, 3U);
}

TEST(IsPathPrefixOf, ComputesPrefixesCorrectly) {
    ASSERT_TRUE(expression::isPathPrefixOf("a.b", "a.b.c"));
    ASSERT_TRUE(expression::isPathPrefixOf("a", "a.b"));
    ASSERT_FALSE(expression::isPathPrefixOf("a.b", "a.balloon"));
    ASSERT_FALSE(expression::isPathPrefixOf("a", "a"));
    ASSERT_FALSE(expression::isPathPrefixOf("a.b", "a"));
}

TEST(hasPredicateOnPaths, IdentifiesLeavesCorrectly) {
    BSONObj matchPredicate = fromjson("{$and: [{a: {$exists: true}}, {b: {$lte: 2}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::EXISTS, {"a"}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::EXISTS, {"b"}));
}

TEST(hasPredicateOnPaths, HandlesMultiplePredicatesWithSamePath) {
    BSONObj matchPredicate = fromjson("{$and: [{a: {$gt: 5000}}, {a: {$exists: false}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::EXISTS, {"a"}));
}

TEST(hasPredicateOnPaths, DeeperTreeTest) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{q: {$gt: 5000}}, {$and: [{z: {$lte: 50}},"
        "{$or: [{f : {$gte: 4}}, {a : {$exists : true}}]}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::EXISTS, {"a"}));
}

TEST(hasPredicateOnPaths, HandlesDottedPathsInDeepTree) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{q: {$gt: 5000}}, {$and: [{z: {$lte: 50}},"
        "{$or: [{f : {$gte: 4}}, {'a.b.c.d' : {$exists : true}}]}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::EXISTS, {"a.b.c.d"}));
}

TEST(hasPredicateOnPaths, ReturnsFalseWhenExistsOnlyOnPrefix) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{q: {$gt: 5000}}, {$and: [{z: {$lte: 50}},"
        "{$or: [{f : {$gte: 4}}, {'a' : {$exists : true}}]}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::EXISTS, {"a.b"}));
}

TEST(hasPredicateOnPaths, ReturnsTrueWhenExistsOnSubpath) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{q: {$gt: 5000}}, {$and: [{z: {$lte: 50}},"
        "{$or: [{f : {$gte: 4}}, {'a.b' : {$exists : true}}]}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::EXISTS, {"a"}));
}

TEST(hasPredicateOnPaths, ReturnsTrueWhenTypeOnSubpath) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{q: {$gt: 5000}}, {$and: [{z: {$lte: 50}},"
        "{$or: [{f : {$gte: 4}}, {'a.b' : {$type : 'long'}}]}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a"}));
}

TEST(hasPredicateOnPaths, ReturnsTrueWhenTraversingThroughElemMatchObject) {
    BSONObj matchPredicate = fromjson("{a: {$elemMatch: {b: 5, c: {$type : 'long'}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());

    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a"}));
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a.c"}));

    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"b"}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"c"}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a.b"}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a.d"}));
}

TEST(hasPredicateOnPaths, ReturnsTrueWhenTraversingThroughElemMatchValue) {
    BSONObj matchPredicate = fromjson("{a: {$elemMatch: {$type : 'long'}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());

    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a"}));

    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a.b"}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a."}));
}

TEST(hasPredicateOnPaths, EmptyFieldTests) {
    BSONObj matchPredicate = fromjson("{\"\": {$elemMatch: {a: {$type : 'long'}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());

    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {""}));
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {".a"}));

    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a"}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"."}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {".b"}));
}

TEST(hasPredicateOnPaths, NestedEmptyFieldTests) {
    BSONObj matchPredicate = fromjson("{a: {$elemMatch: {\"\": {$type : 'long'}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());

    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a"}));
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a."}));

    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {""}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {".a"}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"."}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a.a"}));
}

TEST(hasPredicateOnPaths, FindsTypeUsage) {
    BSONObj matchPredicate = fromjson("{$and: [{a: {$type: 'string'}}, {b: {$lte: 2}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a"}));
    ASSERT_FALSE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"b"}));
}

TEST(hasPredicateOnPaths, IdentifiesBothExistsAndType) {
    BSONObj matchPredicate = fromjson("{$and: [{a: {$type: 'string'}}, {b: {$exists : true}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a"}));
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::EXISTS, {"b"}));
}

TEST(hasPredicateOnPaths, FindsTypeInSecondPredicate) {
    BSONObj matchPredicate = fromjson("{$and: [{a: {$gt: 0}}, {a: {$type: 'long'}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(expression::hasPredicateOnPaths(
        *swMatchExpression.getValue().get(), MatchExpression::MatchType::TYPE_OPERATOR, {"a"}));
}

struct RemoveImprecisePredicateTestCase {
    std::string initialPredicateStr;

    struct AssertTriviallyTrue {};
    struct AssertTriviallyFalse {};
    struct AssertPredicateRewritten {
        std::string predicateStr;
    };
    std::variant<AssertTriviallyTrue, AssertTriviallyFalse, AssertPredicateRewritten> expected;
};

void testRemoveImprecisePredicates(const RemoveImprecisePredicateTestCase& testCase) {
    ParsedMatchExpressionForTest predicate(testCase.initialPredicateStr);
    auto newExpr = expression::assumeImpreciseInternalExprNodesReturnTrue(predicate.release());


    visit(OverloadedVisitor{
              [&](RemoveImprecisePredicateTestCase::AssertTriviallyTrue) {
                  ASSERT(newExpr->isTriviallyTrue());
              },
              [&](RemoveImprecisePredicateTestCase::AssertTriviallyFalse) {
                  ASSERT(newExpr->isTriviallyFalse());
              },
              [&](RemoveImprecisePredicateTestCase::AssertPredicateRewritten a) {
                  ParsedMatchExpressionForTest parsedPred(a.predicateStr);
                  std::unique_ptr<MatchExpression> expected = parsedPred.release();
                  sortMatchExpressionTree(expected.get());
                  ASSERT(expected->equivalent(newExpr.get()));
              },
          },
          testCase.expected);
}

TEST(RemoveImprecisePredicates, ImprecisePredicateInTopLevelOrBecomesAlwaysTrue) {
    RemoveImprecisePredicateTestCase t{
        "{"
        " $or: [{a: {$_internalExprEq: 123}}, {b: {gt: 5}}]"
        "}",
        RemoveImprecisePredicateTestCase::AssertTriviallyTrue{}};
    testRemoveImprecisePredicates(t);
}

TEST(RemoveImprecisePredicates, RemovesImprecisePredicatesFromTopLevelAnd) {
    RemoveImprecisePredicateTestCase t{
        "{"
        " $and: [{a: {$_internalExprEq: 123}},"
        "        {c: {$eq: 123}}]"
        "}",
        RemoveImprecisePredicateTestCase::AssertPredicateRewritten{"{"
                                                                   " $and: [{c: {$eq: 123}}]"
                                                                   "}"}};
    testRemoveImprecisePredicates(t);
}

TEST(RemoveImprecisePredicates, ImprecisePredicateInTopLevelNorBecomesAlwaysFalse) {
    RemoveImprecisePredicateTestCase t{
        "{"
        " $nor: [{a: {$_internalExprEq: 123}},"
        "        {c: {$eq: 123}}]"
        "}",
        RemoveImprecisePredicateTestCase::AssertTriviallyFalse{}};
    testRemoveImprecisePredicates(t);
}

TEST(RemoveImprecisePredicates, RemovesImprecisePredicateFromNestedAnds) {
    RemoveImprecisePredicateTestCase t{
        "{"
        " $or: [{$and: [{$expr: {$eq: ['$a', 123]}}, {a: {$_internalExprEq: 123}}]}, "
        "       {$and: [{$expr: {$eq: ['$b', 456]}}, {b: {$_internalExprEq: 456}}]}] "
        "}",
        RemoveImprecisePredicateTestCase::AssertPredicateRewritten{
            "{"
            " $or: [{$and: [{$expr: {$eq: ['$a', {$const: 123}]}}]}, {$and: [{$expr: {$eq: ['$b', "
            "{$const: 456}]}}]}] "
            "}"}};
    testRemoveImprecisePredicates(t);
}

TEST(RemoveImprecisePredicates, RemovesImprecisePredicatesNestedBelowAnds) {
    RemoveImprecisePredicateTestCase t{
        "{"
        " $and: [{$or: [{a: {$_internalExprEq: 123}}, {b: 456}]},"
        "        {c: {$eq: 123}}]"
        "}",
        RemoveImprecisePredicateTestCase::AssertPredicateRewritten{"{"
                                                                   " $and: [{c: {$eq: 123}}]"
                                                                   "}"}};
    testRemoveImprecisePredicates(t);
}

TEST(RemoveImprecisePredicates, RemovesImprecisePredicatesNestedBelowOrs) {
    RemoveImprecisePredicateTestCase t{
        "{"
        " $or: [{$nor: [{a: {$_internalExprEq: 123}}, {b: 456}]},"
        "       {c: {$eq: 123}}]"
        "}",
        RemoveImprecisePredicateTestCase::AssertPredicateRewritten{"{"
                                                                   " $or: [{c: {$eq: 123}}]"
                                                                   "}"}};
    testRemoveImprecisePredicates(t);
}

TEST(RemoveImprecisePredicates, RemovesImprecisePredicatesBelowNors) {
    RemoveImprecisePredicateTestCase t{
        "{"
        " $nor: [{$and: [{a: {$_internalExprEq: 123}}, {b: 456}]},"
        "       {c: {$eq: 123}}]"
        "}",
        RemoveImprecisePredicateTestCase::AssertPredicateRewritten{
            "{"
            " $nor: [{$and: [{b: 456}]}, {c: {$eq: 123}}]"
            "}"}};
    testRemoveImprecisePredicates(t);
}

TEST(RemoveImprecisePredicates, AndBecomesTriviallyFalseWhenChildIsTriviallyFalse) {
    RemoveImprecisePredicateTestCase t{
        "{"
        " $and: [{$nor: [{a: {$_internalExprEq: 123}}, {b: 456}]},"
        "       {c: {$eq: 123}}]"
        "}",
        RemoveImprecisePredicateTestCase::AssertTriviallyFalse{}};
    testRemoveImprecisePredicates(t);
}

TEST(RemoveImprecisePredicates, TriviallyFalseExpressionInOrIsRemoved) {
    RemoveImprecisePredicateTestCase t{
        "{"
        " $or: [{$nor: [{a: {$_internalExprEq: 123}}, {b: 456}]},"
        "        {c: {$eq: 123}},"
        "        {d: {$eq: 123}}]"
        "}",
        RemoveImprecisePredicateTestCase::AssertPredicateRewritten{"{"
                                                                   " $or: [{c: {$eq: 123}},"
                                                                   "       {d: {$eq: 123}}]"
                                                                   "}"}};
    testRemoveImprecisePredicates(t);
}

TEST(RemoveImprecisePredicates, RemovesTriviallyFalseChildOfNor) {
    RemoveImprecisePredicateTestCase t{
        "{"
        " $nor: [{$nor: [{a: {$_internalExprEq: 123}}, {b: 456}]},"
        "       {c: {$eq: 123}},"
        "       {d: {$eq: 456}}]"
        "}",
        RemoveImprecisePredicateTestCase::AssertPredicateRewritten{
            "{"
            " $nor: [{c: {$eq: 123}}, {d: {$eq: 456}}]"
            "}"}};
    testRemoveImprecisePredicates(t);
}

TEST(Independence, MakeIndependent) {
    // Test that makeIndependent correctly results in a set which contains members of
    // the first set, which do _not_ have a match in the second set for either:
    // * The exact path
    // * A prefix of the path
    // * A sub path
    // That is, if a "$addFields" overwrote all the fields of the second set,
    // all the paths of the first set would be unaffected.
    // Verifies with areIndependent.
    for (const auto& [a, b] : std::vector<std::pair<OrderedPathSet, OrderedPathSet>>{
             // Set: A  -   B
             {{"a"}, {"a"}},        // Exactly equal.
             {{"a"}, {"a.b"}},      // Value removed by subpath match.
             {{"a.b"}, {"a"}},      // Value removed by prefix match.
             {{"a.b.c.d"}, {"a"}},  // Deeply nested value removed by prefix match.
             {{"a"}, {"a.b.c.d"}},  // Value removed by deeply nested subpath match.
             {{"a", "b"}, {"a"}},   // Above + unaffected member of a.
             {{"a", "b"}, {"a.b"}},
             {{"a.b", "b"}, {"a"}},
             {{"a.b.c.d", "b"}, {"a"}},
             {{"a", "b"}, {"a.b.c.d"}},
             {{"a"}, {"a", "b"}},  // Above + member in b with no match in a.
             {{"a"}, {"a.b", "b"}},
             {{"a.b"}, {"a", "b"}},
             {{"a.b.c.d"}, {"a", "b"}},
             {{"a"}, {"a.b.c.d", "b"}},
             {{"a", "a.b"}, {"a"}},         // Multiple paths removed by one member of b.
             {{"a", "a.b"}, {"a", "a.b"}},  // Multiple paths removed by multiple members of b.
             {{"a"}, {"a", "a.b"}},         // One path would be removed by multiple members of b.
         }) {
        // Start off not independent.
        ASSERT_FALSE(expression::areIndependent(a, b))
            << fmt::format("[{}], [{}] ", fmt::join(a, ", "), fmt::join(b, ", "));
        auto aPrime = expression::makeIndependent(a, b);
        // Are now independent.
        ASSERT_TRUE(expression::areIndependent(aPrime, b)) << fmt::format(
            "[{}], [{}], [{}] ", fmt::join(a, ", "), fmt::join(aPrime, ", "), fmt::join(b, ", "));
        // Only members of a are retained.
        ASSERT_TRUE(std::includes(a.begin(), a.end(), aPrime.begin(), aPrime.end(), a.key_comp()));
    }
}

}  // namespace mongo

// expression_algo_test.cpp

/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/unittest/unittest.h"

#include <memory>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/platform/decimal128.h"

namespace mongo {

using std::unique_ptr;

/**
 * A MatchExpression does not hold the memory for BSONElements, so use ParsedMatchExpression to
 * ensure that the BSONObj outlives the MatchExpression.
 */
class ParsedMatchExpression {
public:
    ParsedMatchExpression(const std::string& str, const CollatorInterface* collator = nullptr)
        : _obj(fromjson(str)) {
        StatusWithMatchExpression result =
            MatchExpressionParser::parse(_obj, ExtensionsCallbackDisallowExtensions(), collator);
        ASSERT_OK(result.getStatus());
        _expr = std::move(result.getValue());
    }

    const MatchExpression* get() const {
        return _expr.get();
    }

private:
    const BSONObj _obj;
    std::unique_ptr<MatchExpression> _expr;
};

TEST(ExpressionAlgoIsSubsetOf, NullAndOmittedField) {
    // Verify that ComparisonMatchExpression::init() prohibits creating a match expression with
    // an Undefined type.
    BSONObj undefined = fromjson("{a: undefined}");
    const CollatorInterface* collator = nullptr;
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        MatchExpressionParser::parse(undefined, ExtensionsCallbackDisallowExtensions(), collator)
            .getStatus());

    ParsedMatchExpression empty("{}");
    ParsedMatchExpression null("{a: null}");

    ASSERT_TRUE(expression::isSubsetOf(null.get(), empty.get()));
    ASSERT_FALSE(expression::isSubsetOf(empty.get(), null.get()));

    ParsedMatchExpression b1("{b: 1}");
    ParsedMatchExpression aNullB1("{a: null, b: 1}");

    ASSERT_TRUE(expression::isSubsetOf(aNullB1.get(), b1.get()));
    ASSERT_FALSE(expression::isSubsetOf(b1.get(), aNullB1.get()));

    ParsedMatchExpression a1C3("{a: 1, c: 3}");
    ParsedMatchExpression a1BNullC3("{a: 1, b: null, c: 3}");

    ASSERT_TRUE(expression::isSubsetOf(a1BNullC3.get(), a1C3.get()));
    ASSERT_FALSE(expression::isSubsetOf(a1C3.get(), a1BNullC3.get()));
}

TEST(ExpressionAlgoIsSubsetOf, NullAndIn) {
    ParsedMatchExpression eqNull("{x: null}");
    ParsedMatchExpression inNull("{x: {$in: [null]}}");
    ParsedMatchExpression inNullOr2("{x: {$in: [null, 2]}}");

    ASSERT_TRUE(expression::isSubsetOf(inNull.get(), eqNull.get()));
    ASSERT_FALSE(expression::isSubsetOf(inNullOr2.get(), eqNull.get()));
}

TEST(ExpressionAlgoIsSubsetOf, NullAndExists) {
    ParsedMatchExpression null("{x: null}");
    ParsedMatchExpression exists("{x: {$exists: true}}");
    ASSERT_FALSE(expression::isSubsetOf(null.get(), exists.get()));
    ASSERT_FALSE(expression::isSubsetOf(exists.get(), null.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_NaN) {
    ParsedMatchExpression nan("{x: NaN}");
    ParsedMatchExpression lt("{x: {$lt: 5}}");
    ParsedMatchExpression lte("{x: {$lte: 5}}");
    ParsedMatchExpression gte("{x: {$gte: 5}}");
    ParsedMatchExpression gt("{x: {$gt: 5}}");
    ParsedMatchExpression in("{x: {$in: [5]}}");

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

    ParsedMatchExpression decNan("{x : NumberDecimal(\"NaN\") }");
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
    ParsedMatchExpression a5("{a: 5}");
    ParsedMatchExpression a6("{a: 6}");
    ParsedMatchExpression b5("{b: 5}");

    ASSERT_TRUE(expression::isSubsetOf(a5.get(), a5.get()));
    ASSERT_FALSE(expression::isSubsetOf(a5.get(), a6.get()));
    ASSERT_FALSE(expression::isSubsetOf(a5.get(), b5.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CompareAnd_EQ) {
    ParsedMatchExpression a1B2("{a: 1, b: 2}");
    ParsedMatchExpression a1B7("{a: 1, b: 7}");
    ParsedMatchExpression a1("{a: 1}");
    ParsedMatchExpression b2("{b: 2}");

    ASSERT_TRUE(expression::isSubsetOf(a1B2.get(), a1B2.get()));
    ASSERT_FALSE(expression::isSubsetOf(a1B2.get(), a1B7.get()));

    ASSERT_TRUE(expression::isSubsetOf(a1B2.get(), a1.get()));
    ASSERT_TRUE(expression::isSubsetOf(a1B2.get(), b2.get()));
    ASSERT_FALSE(expression::isSubsetOf(a1B7.get(), b2.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CompareAnd_GT) {
    ParsedMatchExpression filter("{a: {$gt: 5}, b: {$gt: 6}}");
    ParsedMatchExpression query("{a: {$gt: 5}, b: {$gt: 6}, c: {$gt: 7}}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CompareOr_LT) {
    ParsedMatchExpression lt5("{a: {$lt: 5}}");
    ParsedMatchExpression eq2OrEq3("{$or: [{a: 2}, {a: 3}]}");
    ParsedMatchExpression eq4OrEq5("{$or: [{a: 4}, {a: 5}]}");
    ParsedMatchExpression eq4OrEq6("{$or: [{a: 4}, {a: 6}]}");

    ASSERT_TRUE(expression::isSubsetOf(eq2OrEq3.get(), lt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(eq4OrEq5.get(), lt5.get()));
    ASSERT_FALSE(expression::isSubsetOf(eq4OrEq6.get(), lt5.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CompareOr_GTE) {
    ParsedMatchExpression gte5("{a: {$gte: 5}}");
    ParsedMatchExpression eq4OrEq6("{$or: [{a: 4}, {a: 6}]}");
    ParsedMatchExpression eq5OrEq6("{$or: [{a: 5}, {a: 6}]}");
    ParsedMatchExpression eq7OrEq8("{$or: [{a: 7}, {a: 8}]}");

    ASSERT_FALSE(expression::isSubsetOf(eq4OrEq6.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(eq5OrEq6.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(eq7OrEq8.get(), gte5.get()));
}

TEST(ExpressionAlgoIsSubsetOf, DifferentCanonicalTypes) {
    ParsedMatchExpression number("{x: {$gt: 1}}");
    ParsedMatchExpression string("{x: {$gt: 'a'}}");
    ASSERT_FALSE(expression::isSubsetOf(number.get(), string.get()));
    ASSERT_FALSE(expression::isSubsetOf(string.get(), number.get()));
}

TEST(ExpressionAlgoIsSubsetOf, DifferentNumberTypes) {
    ParsedMatchExpression numberDouble("{x: 5.0}");
    ParsedMatchExpression numberInt("{x: NumberInt(5)}");
    ParsedMatchExpression numberLong("{x: NumberLong(5)}");

    ASSERT_TRUE(expression::isSubsetOf(numberDouble.get(), numberInt.get()));
    ASSERT_TRUE(expression::isSubsetOf(numberDouble.get(), numberLong.get()));
    ASSERT_TRUE(expression::isSubsetOf(numberInt.get(), numberDouble.get()));
    ASSERT_TRUE(expression::isSubsetOf(numberInt.get(), numberLong.get()));
    ASSERT_TRUE(expression::isSubsetOf(numberLong.get(), numberDouble.get()));
    ASSERT_TRUE(expression::isSubsetOf(numberLong.get(), numberInt.get()));
}

TEST(ExpressionAlgoIsSubsetOf, PointInUnboundedRange) {
    ParsedMatchExpression a4("{a: 4}");
    ParsedMatchExpression a5("{a: 5}");
    ParsedMatchExpression a6("{a: 6}");
    ParsedMatchExpression b5("{b: 5}");

    ParsedMatchExpression lt5("{a: {$lt: 5}}");
    ParsedMatchExpression lte5("{a: {$lte: 5}}");
    ParsedMatchExpression gte5("{a: {$gte: 5}}");
    ParsedMatchExpression gt5("{a: {$gt: 5}}");

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
    ParsedMatchExpression filter("{a: {$gt: 5, $lt: 10}}");
    ParsedMatchExpression query("{a: 6}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, PointInBoundedRange_FakeAnd) {
    ParsedMatchExpression filter("{a: {$gt: 5, $lt: 10}}");
    ParsedMatchExpression query("{$and: [{a: 6}, {a: 6}]}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, MultiplePointsInBoundedRange) {
    ParsedMatchExpression filter("{a: {$gt: 5, $lt: 10}}");
    ParsedMatchExpression queryAllInside("{a: {$in: [6, 7, 8]}}");
    ParsedMatchExpression queryStraddleLower("{a: {$in: [4.9, 5.1]}}");
    ParsedMatchExpression queryStraddleUpper("{a: {$in: [9.9, 10.1]}}");

    ASSERT_TRUE(expression::isSubsetOf(queryAllInside.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(queryStraddleLower.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(queryStraddleUpper.get(), filter.get()));
}

TEST(ExpressionAlgoIsSubsetOf, PointInCompoundRange) {
    ParsedMatchExpression filter("{a: {$gt: 5}, b: {$gt: 6}, c: {$gt: 7}}");
    ParsedMatchExpression query("{a: 10, b: 10, c: 10}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_LT_LTE) {
    ParsedMatchExpression lte4("{x: {$lte: 4}}");
    ParsedMatchExpression lt5("{x: {$lt: 5}}");
    ParsedMatchExpression lte5("{x: {$lte: 5}}");
    ParsedMatchExpression lt6("{x: {$lt: 6}}");

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
    ParsedMatchExpression gte6("{x: {$gte: 6}}");
    ParsedMatchExpression gt5("{x: {$gt: 5}}");
    ParsedMatchExpression gte5("{x: {$gte: 5}}");
    ParsedMatchExpression gt4("{x: {$gt: 4}}");

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
    ParsedMatchExpression filter("{a: {$gt: 1}}");
    ParsedMatchExpression query("{a: {$gt: 5, $lt: 10}}");

    ASSERT_TRUE(expression::isSubsetOf(query.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(filter.get(), query.get()));
}

TEST(ExpressionAlgoIsSubsetOf, MultipleRangesInUnboundedRange) {
    ParsedMatchExpression filter("{a: {$gt: 1}}");
    ParsedMatchExpression negative("{$or: [{a: {$gt: 5, $lt: 10}}, {a: {$lt: 0}}]}");
    ParsedMatchExpression unbounded("{$or: [{a: {$gt: 5, $lt: 10}}, {a: {$gt: 15}}]}");
    ParsedMatchExpression bounded("{$or: [{a: {$gt: 5, $lt: 10}}, {a: {$gt: 20, $lt: 30}}]}");

    ASSERT_FALSE(expression::isSubsetOf(negative.get(), filter.get()));
    ASSERT_TRUE(expression::isSubsetOf(unbounded.get(), filter.get()));
    ASSERT_TRUE(expression::isSubsetOf(bounded.get(), filter.get()));
}

TEST(ExpressionAlgoIsSubsetOf, MultipleFields) {
    ParsedMatchExpression filter("{a: {$gt: 5}, b: {$lt: 10}}");
    ParsedMatchExpression onlyA("{$or: [{a: 6, b: {$lt: 4}}, {a: {$gt: 11}}]}");
    ParsedMatchExpression onlyB("{$or: [{b: {$lt: 4}}, {a: {$gt: 11}, b: 9}]}");
    ParsedMatchExpression both("{$or: [{a: 6, b: {$lt: 4}}, {a: {$gt: 11}, b: 9}]}");

    ASSERT_FALSE(expression::isSubsetOf(onlyA.get(), filter.get()));
    ASSERT_FALSE(expression::isSubsetOf(onlyB.get(), filter.get()));
    ASSERT_TRUE(expression::isSubsetOf(both.get(), filter.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_LT_In) {
    ParsedMatchExpression lt("{a: {$lt: 5}}");

    ParsedMatchExpression inLt("{a: {$in: [4.9]}}");
    ParsedMatchExpression inEq("{a: {$in: [5]}}");
    ParsedMatchExpression inGt("{a: {$in: [5.1]}}");
    ParsedMatchExpression inNull("{a: {$in: [null]}}");

    ParsedMatchExpression inAllEq("{a: {$in: [5, 5.0]}}");
    ParsedMatchExpression inAllLte("{a: {$in: [4.9, 5]}}");
    ParsedMatchExpression inAllLt("{a: {$in: [2, 3, 4]}}");
    ParsedMatchExpression inStraddle("{a: {$in: [4, 6]}}");
    ParsedMatchExpression inLtAndNull("{a: {$in: [1, null]}}");

    ASSERT_TRUE(expression::isSubsetOf(inLt.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inEq.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inGt.get(), lt.get()));

    ASSERT_FALSE(expression::isSubsetOf(inAllEq.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inAllLte.get(), lt.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllLt.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inStraddle.get(), lt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inLtAndNull.get(), lt.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_LTE_In) {
    ParsedMatchExpression lte("{a: {$lte: 5}}");

    ParsedMatchExpression inLt("{a: {$in: [4.9]}}");
    ParsedMatchExpression inEq("{a: {$in: [5]}}");
    ParsedMatchExpression inGt("{a: {$in: [5.1]}}");
    ParsedMatchExpression inNull("{a: {$in: [null]}}");

    ParsedMatchExpression inAllEq("{a: {$in: [5, 5.0]}}");
    ParsedMatchExpression inAllLte("{a: {$in: [4.9, 5]}}");
    ParsedMatchExpression inAllLt("{a: {$in: [2, 3, 4]}}");
    ParsedMatchExpression inStraddle("{a: {$in: [4, 6]}}");
    ParsedMatchExpression inLtAndNull("{a: {$in: [1, null]}}");

    ASSERT_TRUE(expression::isSubsetOf(inLt.get(), lte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inEq.get(), lte.get()));
    ASSERT_FALSE(expression::isSubsetOf(inGt.get(), lte.get()));

    ASSERT_TRUE(expression::isSubsetOf(inAllEq.get(), lte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllLte.get(), lte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllLt.get(), lte.get()));
    ASSERT_FALSE(expression::isSubsetOf(inStraddle.get(), lte.get()));
    ASSERT_FALSE(expression::isSubsetOf(inLtAndNull.get(), lte.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_EQ_In) {
    ParsedMatchExpression eq("{a: 5}");

    ParsedMatchExpression inLt("{a: {$in: [4.9]}}");
    ParsedMatchExpression inEq("{a: {$in: [5]}}");
    ParsedMatchExpression inGt("{a: {$in: [5.1]}}");
    ParsedMatchExpression inNull("{a: {$in: [null]}}");

    ParsedMatchExpression inAllEq("{a: {$in: [5, 5.0]}}");
    ParsedMatchExpression inStraddle("{a: {$in: [4, 6]}}");
    ParsedMatchExpression inEqAndNull("{a: {$in: [5, null]}}");

    ASSERT_FALSE(expression::isSubsetOf(inLt.get(), eq.get()));
    ASSERT_TRUE(expression::isSubsetOf(inEq.get(), eq.get()));
    ASSERT_FALSE(expression::isSubsetOf(inGt.get(), eq.get()));

    ASSERT_TRUE(expression::isSubsetOf(inAllEq.get(), eq.get()));
    ASSERT_FALSE(expression::isSubsetOf(inStraddle.get(), eq.get()));
    ASSERT_FALSE(expression::isSubsetOf(inEqAndNull.get(), eq.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_GT_In) {
    ParsedMatchExpression gt("{a: {$gt: 5}}");

    ParsedMatchExpression inLt("{a: {$in: [4.9]}}");
    ParsedMatchExpression inEq("{a: {$in: [5]}}");
    ParsedMatchExpression inGt("{a: {$in: [5.1]}}");
    ParsedMatchExpression inNull("{a: {$in: [null]}}");

    ParsedMatchExpression inAllEq("{a: {$in: [5, 5.0]}}");
    ParsedMatchExpression inAllGte("{a: {$in: [5, 5.1]}}");
    ParsedMatchExpression inAllGt("{a: {$in: [6, 7, 8]}}");
    ParsedMatchExpression inStraddle("{a: {$in: [4, 6]}}");
    ParsedMatchExpression inGtAndNull("{a: {$in: [9, null]}}");

    ASSERT_FALSE(expression::isSubsetOf(inLt.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inEq.get(), gt.get()));
    ASSERT_TRUE(expression::isSubsetOf(inGt.get(), gt.get()));

    ASSERT_FALSE(expression::isSubsetOf(inAllEq.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inAllGte.get(), gt.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllGt.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inStraddle.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(inGtAndNull.get(), gt.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_GTE_In) {
    ParsedMatchExpression gte("{a: {$gte: 5}}");

    ParsedMatchExpression inLt("{a: {$in: [4.9]}}");
    ParsedMatchExpression inEq("{a: {$in: [5]}}");
    ParsedMatchExpression inGt("{a: {$in: [5.1]}}");
    ParsedMatchExpression inNull("{a: {$in: [null]}}");

    ParsedMatchExpression inAllEq("{a: {$in: [5, 5.0]}}");
    ParsedMatchExpression inAllGte("{a: {$in: [5, 5.1]}}");
    ParsedMatchExpression inAllGt("{a: {$in: [6, 7, 8]}}");
    ParsedMatchExpression inStraddle("{a: {$in: [4, 6]}}");
    ParsedMatchExpression inGtAndNull("{a: {$in: [9, null]}}");

    ASSERT_FALSE(expression::isSubsetOf(inLt.get(), gte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inEq.get(), gte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inGt.get(), gte.get()));

    ASSERT_TRUE(expression::isSubsetOf(inAllEq.get(), gte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllGte.get(), gte.get()));
    ASSERT_TRUE(expression::isSubsetOf(inAllGt.get(), gte.get()));
    ASSERT_FALSE(expression::isSubsetOf(inStraddle.get(), gte.get()));
    ASSERT_FALSE(expression::isSubsetOf(inGtAndNull.get(), gte.get()));
}

TEST(ExpressionAlgoIsSubsetOf, RegexAndIn) {
    ParsedMatchExpression eq1("{x: 1}");
    ParsedMatchExpression eqA("{x: 'a'}");
    ParsedMatchExpression inRegexA("{x: {$in: [/a/]}}");
    ParsedMatchExpression inRegexAbc("{x: {$in: [/abc/]}}");
    ParsedMatchExpression inRegexAOrEq1("{x: {$in: [/a/, 1]}}");
    ParsedMatchExpression inRegexAOrNull("{x: {$in: [/a/, null]}}");

    ASSERT_FALSE(expression::isSubsetOf(inRegexAOrEq1.get(), eq1.get()));
    ASSERT_FALSE(expression::isSubsetOf(inRegexA.get(), eqA.get()));
    ASSERT_FALSE(expression::isSubsetOf(inRegexAOrNull.get(), eqA.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Exists) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression bExists("{b: {$exists: true}}");
    ParsedMatchExpression aExistsBExists("{a: {$exists: true}, b: {$exists: true}}");
    ParsedMatchExpression aExistsBExistsC5("{a: {$exists: true}, b: {$exists: true}, c: 5}");

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
    ParsedMatchExpression exists("{a: {$exists: true}}");
    ParsedMatchExpression eq("{a: 1}");
    ParsedMatchExpression gt("{a: {$gt: 4}}");
    ParsedMatchExpression lte("{a: {$lte: 7}}");

    ASSERT_TRUE(expression::isSubsetOf(eq.get(), exists.get()));
    ASSERT_TRUE(expression::isSubsetOf(gt.get(), exists.get()));
    ASSERT_TRUE(expression::isSubsetOf(lte.get(), exists.get()));

    ASSERT_FALSE(expression::isSubsetOf(exists.get(), eq.get()));
    ASSERT_FALSE(expression::isSubsetOf(exists.get(), gt.get()));
    ASSERT_FALSE(expression::isSubsetOf(exists.get(), lte.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Type) {
    ParsedMatchExpression aType1("{a: {$type: 1}}");
    ParsedMatchExpression aType2("{a: {$type: 2}}");
    ParsedMatchExpression bType2("{b: {$type: 2}}");

    ASSERT_FALSE(expression::isSubsetOf(aType1.get(), aType2.get()));
    ASSERT_FALSE(expression::isSubsetOf(aType2.get(), aType1.get()));

    ASSERT_TRUE(expression::isSubsetOf(aType2.get(), aType2.get()));
    ASSERT_FALSE(expression::isSubsetOf(aType2.get(), bType2.get()));
}

TEST(ExpressionAlgoIsSubsetOf, TypeAndExists) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression aType2("{a: {$type: 2}}");
    ParsedMatchExpression bType2("{b: {$type: 2}}");

    ASSERT_TRUE(expression::isSubsetOf(aType2.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aType2.get()));
    ASSERT_FALSE(expression::isSubsetOf(bType2.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, AllAndExists) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression aAll("{a: {$all: ['x', 'y', 'z']}}");
    ParsedMatchExpression bAll("{b: {$all: ['x', 'y', 'z']}}");
    ParsedMatchExpression aAllWithNull("{a: {$all: ['x', null, 'z']}}");

    ASSERT_TRUE(expression::isSubsetOf(aAll.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bAll.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aAllWithNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, ElemMatchAndExists_Value) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression aElemMatch("{a: {$elemMatch: {$gt: 5, $lte: 10}}}");
    ParsedMatchExpression bElemMatch("{b: {$elemMatch: {$gt: 5, $lte: 10}}}");
    ParsedMatchExpression aElemMatchNull("{a: {$elemMatch: {$eq: null}}}");

    ASSERT_TRUE(expression::isSubsetOf(aElemMatch.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aElemMatch.get()));
    ASSERT_FALSE(expression::isSubsetOf(bElemMatch.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aElemMatchNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, ElemMatchAndExists_Object) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression aElemMatch("{a: {$elemMatch: {x: {$gt: 5}, y: {$lte: 10}}}}");
    ParsedMatchExpression bElemMatch("{b: {$elemMatch: {x: {$gt: 5}, y: {$lte: 10}}}}");
    ParsedMatchExpression aElemMatchNull("{a: {$elemMatch: {x: null, y: null}}}");

    ASSERT_TRUE(expression::isSubsetOf(aElemMatch.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aElemMatch.get()));
    ASSERT_FALSE(expression::isSubsetOf(bElemMatch.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aElemMatchNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, SizeAndExists) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression aSize0("{a: {$size: 0}}");
    ParsedMatchExpression aSize1("{a: {$size: 1}}");
    ParsedMatchExpression aSize3("{a: {$size: 3}}");
    ParsedMatchExpression bSize3("{b: {$size: 3}}");

    ASSERT_TRUE(expression::isSubsetOf(aSize0.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aSize1.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aSize3.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aSize3.get()));
    ASSERT_FALSE(expression::isSubsetOf(bSize3.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, ModAndExists) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression aMod5("{a: {$mod: [5, 0]}}");
    ParsedMatchExpression bMod5("{b: {$mod: [5, 0]}}");

    ASSERT_TRUE(expression::isSubsetOf(aMod5.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bMod5.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, RegexAndExists) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression aRegex("{a: {$regex: 'pattern'}}");
    ParsedMatchExpression bRegex("{b: {$regex: 'pattern'}}");

    ASSERT_TRUE(expression::isSubsetOf(aRegex.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bRegex.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, InAndExists) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression aIn("{a: {$in: [1, 2, 3]}}");
    ParsedMatchExpression bIn("{b: {$in: [1, 2, 3]}}");
    ParsedMatchExpression aInWithNull("{a: {$in: [1, null, 3]}}");

    ASSERT_TRUE(expression::isSubsetOf(aIn.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bIn.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(aInWithNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, NinAndExists) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression aNin("{a: {$nin: [1, 2, 3]}}");
    ParsedMatchExpression bNin("{b: {$nin: [1, 2, 3]}}");
    ParsedMatchExpression aNinWithNull("{a: {$nin: [1, null, 3]}}");

    ASSERT_FALSE(expression::isSubsetOf(aNin.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bNin.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aNinWithNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, Compare_Exists_NE) {
    ParsedMatchExpression aExists("{a: {$exists: true}}");
    ParsedMatchExpression aNotEqual1("{a: {$ne: 1}}");
    ParsedMatchExpression bNotEqual1("{b: {$ne: 1}}");
    ParsedMatchExpression aNotEqualNull("{a: {$ne: null}}");

    ASSERT_FALSE(expression::isSubsetOf(aNotEqual1.get(), aExists.get()));
    ASSERT_FALSE(expression::isSubsetOf(bNotEqual1.get(), aExists.get()));
    ASSERT_TRUE(expression::isSubsetOf(aNotEqualNull.get(), aExists.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CollationAwareStringComparison) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ParsedMatchExpression lhs("{a: {$gt: 'abc'}}", &collator);
    ParsedMatchExpression rhs("{a: {$gt: 'cba'}}", &collator);

    ASSERT_TRUE(expression::isSubsetOf(lhs.get(), rhs.get()));

    ParsedMatchExpression lhsLT("{a: {$lt: 'abc'}}", &collator);
    ParsedMatchExpression rhsLT("{a: {$lt: 'cba'}}", &collator);

    ASSERT_FALSE(expression::isSubsetOf(lhsLT.get(), rhsLT.get()));
}

TEST(ExpressionAlgoIsSubsetOf, NonMatchingCollationsStringComparison) {
    CollatorInterfaceMock collatorAlwaysEqual(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock collatorReverseString(CollatorInterfaceMock::MockType::kReverseString);
    ParsedMatchExpression lhs("{a: {$gt: 'abc'}}", &collatorAlwaysEqual);
    ParsedMatchExpression rhs("{a: {$gt: 'cba'}}", &collatorReverseString);

    ASSERT_FALSE(expression::isSubsetOf(lhs.get(), rhs.get()));

    ParsedMatchExpression lhsLT("{a: {$lt: 'abc'}}", &collatorAlwaysEqual);
    ParsedMatchExpression rhsLT("{a: {$lt: 'cba'}}", &collatorReverseString);

    ASSERT_FALSE(expression::isSubsetOf(lhsLT.get(), rhsLT.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CollationAwareStringComparisonIn) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ParsedMatchExpression lhsAllGTcba("{a: {$in: ['abc', 'cbc']}}", &collator);
    ParsedMatchExpression lhsSomeGTcba("{a: {$in: ['abc', 'aba']}}", &collator);
    ParsedMatchExpression rhs("{a: {$gt: 'cba'}}", &collator);

    ASSERT_TRUE(expression::isSubsetOf(lhsAllGTcba.get(), rhs.get()));
    ASSERT_FALSE(expression::isSubsetOf(lhsSomeGTcba.get(), rhs.get()));

    ParsedMatchExpression rhsLT("{a: {$lt: 'cba'}}", &collator);

    ASSERT_FALSE(expression::isSubsetOf(lhsAllGTcba.get(), rhsLT.get()));
    ASSERT_FALSE(expression::isSubsetOf(lhsSomeGTcba.get(), rhsLT.get()));
}

// TODO SERVER-24674: isSubsetOf should return true after exploring nested objects.
TEST(ExpressionAlgoIsSubsetOf, NonMatchingCollationsNoStringComparisonLHS) {
    CollatorInterfaceMock collatorAlwaysEqual(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock collatorReverseString(CollatorInterfaceMock::MockType::kReverseString);
    ParsedMatchExpression lhs("{a: {b: 1}}", &collatorAlwaysEqual);
    ParsedMatchExpression rhs("{a: {$lt: {b: 'abc'}}}", &collatorReverseString);

    ASSERT_FALSE(expression::isSubsetOf(lhs.get(), rhs.get()));
}

TEST(ExpressionAlgoIsSubsetOf, NonMatchingCollationsNoStringComparison) {
    CollatorInterfaceMock collatorAlwaysEqual(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock collatorReverseString(CollatorInterfaceMock::MockType::kReverseString);
    ParsedMatchExpression lhs("{a: 1}", &collatorAlwaysEqual);
    ParsedMatchExpression rhs("{a: {$gt: 0}}", &collatorReverseString);

    ASSERT_TRUE(expression::isSubsetOf(lhs.get(), rhs.get()));
}

TEST(IsIndependent, AndIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{$and: [{a: 1}, {b: 1}]}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"b"}));
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"c"}));
}

TEST(IsIndependent, ElemMatchIsNotIndependent) {
    BSONObj matchPredicate = fromjson("{x: {$elemMatch: {y: 1}}}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"x"}));
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"x.y"}));
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"y"}));
}

TEST(IsIndependent, NorIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{$nor: [{a: 1}, {b: 1}]}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"b"}));
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"c"}));
}

TEST(IsIndependent, NotIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{a: {$not: {$eq: 1}}}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"b"}));
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"a"}));
}

TEST(IsIndependent, OrIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{$or: [{a: 1}, {b: 1}]}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"a"}));
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"c"}));
}

TEST(IsIndependent, AndWithDottedFieldPathsIsNotIndependent) {
    BSONObj matchPredicate = fromjson("{$and: [{'a': 1}, {'a.b': 1}]}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"a.b.c"}));
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"a.b"}));
}

TEST(IsIndependent, BallIsIndependentOfBalloon) {
    BSONObj matchPredicate = fromjson("{'a.ball': 4}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"a.balloon"}));
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"a.b"}));
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"a.ball.c"}));
}

TEST(SplitMatchExpression, AndWithSplittableChildrenIsSplittable) {
    BSONObj matchPredicate = fromjson("{$and: [{a: 1}, {b: 1}]}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"b"});

    ASSERT_TRUE(splitExpr.first.get());
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob);

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob);

    ASSERT_EQUALS(firstBob.obj(), fromjson("{a: {$eq: 1}}"));
    ASSERT_EQUALS(secondBob.obj(), fromjson("{b: {$eq: 1}}"));
}

TEST(SplitMatchExpression, NorWithIndependentChildrenIsSplittable) {
    BSONObj matchPredicate = fromjson("{$nor: [{a: 1}, {b: 1}]}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"b"});

    ASSERT_TRUE(splitExpr.first.get());
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob);

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob);

    ASSERT_EQUALS(firstBob.obj(), fromjson("{$nor: [{a: {$eq: 1}}]}"));
    ASSERT_EQUALS(secondBob.obj(), fromjson("{$nor: [{b: {$eq: 1}}]}"));
}

TEST(SplitMatchExpression, NotWithIndependentChildIsSplittable) {
    BSONObj matchPredicate = fromjson("{x: {$not: {$gt: 4}}}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"y"});

    ASSERT_TRUE(splitExpr.first.get());
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob);

    ASSERT_EQUALS(firstBob.obj(), fromjson("{$nor: [{$and: [{x: {$gt: 4}}]}]}"));
    ASSERT_FALSE(splitExpr.second);
}

TEST(SplitMatchExpression, OrWithOnlyIndependentChildrenIsNotSplittable) {
    BSONObj matchPredicate = fromjson("{$or: [{a: 1}, {b: 1}]}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"b"});

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder bob;
    splitExpr.second->serialize(&bob);

    ASSERT_FALSE(splitExpr.first);
    ASSERT_EQUALS(bob.obj(), fromjson("{$or: [{a: {$eq: 1}}, {b: {$eq: 1}}]}"));
}

TEST(SplitMatchExpression, ComplexMatchExpressionSplitsCorrectly) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{x: {$not: {$size: 2}}},"
        "{$or: [{'a.b' : 3}, {'a.b.c': 4}]},"
        "{$nor: [{x: {$gt: 4}}, {$and: [{x: {$not: {$eq: 1}}}, {y: 3}]}]}]}");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(status.getStatus());

    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(status.getValue()), {"x"});

    ASSERT_TRUE(splitExpr.first.get());
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob);

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob);

    ASSERT_EQUALS(firstBob.obj(), fromjson("{$or: [{'a.b': {$eq: 3}}, {'a.b.c': {$eq: 4}}]}"));
    ASSERT_EQUALS(
        secondBob.obj(),
        fromjson("{$and: [{$nor: [{$and: [{x: {$size: 2}}]}]}, {$nor: [{x: {$gt: 4}}, {$and: "
                 "[{$nor: [{$and: [{x: "
                 "{$eq: 1}}]}]}, {y: {$eq: 3}}]}]}]}"));
}

TEST(MapOverMatchExpression, DoesMapOverLogicalNodes) {
    BSONObj matchPredicate = fromjson("{a: {$not: {$eq: 1}}}");
    const CollatorInterface* collator = nullptr;
    auto swMatchExpression =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(swMatchExpression.getStatus());

    bool hasLogicalNode = false;
    expression::mapOver(swMatchExpression.getValue().get(),
                        [&hasLogicalNode](MatchExpression* expression, std::string path) -> void {
                            if (expression->isLogical()) {
                                hasLogicalNode = true;
                            }
                        });

    ASSERT_TRUE(hasLogicalNode);
}

TEST(MapOverMatchExpression, DoesMapOverLeafNodes) {
    BSONObj matchPredicate = fromjson("{a: {$not: {$eq: 1}}}");
    const CollatorInterface* collator = nullptr;
    auto swMatchExpression =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
    ASSERT_OK(swMatchExpression.getStatus());

    bool hasLeafNode = false;
    expression::mapOver(swMatchExpression.getValue().get(),
                        [&hasLeafNode](MatchExpression* expression, std::string path) -> void {
                            if (!expression->isLogical()) {
                                hasLeafNode = true;
                            }
                        });

    ASSERT_TRUE(hasLeafNode);
}

TEST(MapOverMatchExpression, DoesPassPath) {
    BSONObj matchPredicate = fromjson("{a: {$elemMatch: {b: 1}}}");
    const CollatorInterface* collator = nullptr;
    auto swMatchExpression =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
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
    const CollatorInterface* collator = nullptr;
    auto swMatchExpression =
        MatchExpressionParser::parse(matchPredicate, ExtensionsCallbackNoop(), collator);
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


}  // namespace mongo

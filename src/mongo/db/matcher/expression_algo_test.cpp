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

#include "mongo/unittest/unittest.h"

#include <memory>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
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
        _expCtx = make_intrusive<ExpressionContextForTest>();
        _expCtx->setCollator(CollatorInterface::cloneCollator(collator));
        StatusWithMatchExpression result = MatchExpressionParser::parse(_obj, _expCtx);
        ASSERT_OK(result.getStatus());
        _expr = std::move(result.getValue());
    }

    const MatchExpression* get() const {
        return _expr.get();
    }

private:
    const BSONObj _obj;
    std::unique_ptr<MatchExpression> _expr;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

void assertMatchesEqual(const ParsedMatchExpression& expected,
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

    ASSERT_TRUE(expression::isSubsetOf(eqNull.get(), inNull.get()));
    ASSERT_TRUE(expression::isSubsetOf(eqNull.get(), inNullOr2.get()));
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

    ParsedMatchExpression lt4OrLt5("{$or: [{a: {$lt: 4}}, {a: {$lt: 5}}]}");

    ASSERT_TRUE(expression::isSubsetOf(lt4OrLt5.get(), lt5.get()));
    ASSERT_TRUE(expression::isSubsetOf(lt5.get(), lt4OrLt5.get()));

    ParsedMatchExpression lt7OrLt8("{$or: [{a: {$lt: 7}}, {a: {$lt: 8}}]}");

    ASSERT_FALSE(expression::isSubsetOf(lt7OrLt8.get(), lt5.get()));
    ASSERT_TRUE(expression::isSubsetOf(lt5.get(), lt7OrLt8.get()));
}

TEST(ExpressionAlgoIsSubsetOf, CompareOr_GTE) {
    ParsedMatchExpression gte5("{a: {$gte: 5}}");
    ParsedMatchExpression eq4OrEq6("{$or: [{a: 4}, {a: 6}]}");
    ParsedMatchExpression eq5OrEq6("{$or: [{a: 5}, {a: 6}]}");
    ParsedMatchExpression eq7OrEq8("{$or: [{a: 7}, {a: 8}]}");

    ASSERT_FALSE(expression::isSubsetOf(eq4OrEq6.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(eq5OrEq6.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(eq7OrEq8.get(), gte5.get()));

    ParsedMatchExpression gte5OrGte6("{$or: [{a: {$gte: 5}}, {a: {$gte: 6}}]}");

    ASSERT_TRUE(expression::isSubsetOf(gte5OrGte6.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(gte5.get(), gte5OrGte6.get()));

    ParsedMatchExpression gte3OrGte4("{$or: [{a: {$gte: 3}}, {a: {$gte: 4}}]}");

    ASSERT_FALSE(expression::isSubsetOf(gte3OrGte4.get(), gte5.get()));
    ASSERT_TRUE(expression::isSubsetOf(gte5.get(), gte3OrGte4.get()));
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

    ASSERT_FALSE(expression::isSubsetOf(lt.get(), inLt.get()));
    ASSERT_FALSE(expression::isSubsetOf(lt.get(), inEq.get()));
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

    ASSERT_FALSE(expression::isSubsetOf(lte.get(), inLt.get()));
    ASSERT_FALSE(expression::isSubsetOf(lte.get(), inEq.get()));
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

    ASSERT_TRUE(expression::isSubsetOf(eq.get(), inEq.get()));
    ASSERT_FALSE(expression::isSubsetOf(eq.get(), inLt.get()));
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

    ASSERT_FALSE(expression::isSubsetOf(gt.get(), inGt.get()));
    ASSERT_FALSE(expression::isSubsetOf(gt.get(), inEq.get()));
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

    ASSERT_FALSE(expression::isSubsetOf(gte.get(), inGt.get()));
    ASSERT_FALSE(expression::isSubsetOf(gte.get(), inEq.get()));
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

    ASSERT_FALSE(expression::isSubsetOf(eq1.get(), inRegexAOrEq1.get()));
    ASSERT_FALSE(expression::isSubsetOf(eqA.get(), inRegexA.get()));
    ASSERT_FALSE(expression::isSubsetOf(eqA.get(), inRegexAOrNull.get()));
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

    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aIn.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), bIn.get()));
    ASSERT_FALSE(expression::isSubsetOf(aExists.get(), aInWithNull.get()));
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

TEST(ExpressionAlgoIsSubsetOf, InternalExprEqIsSubsetOfNothing) {
    ParsedMatchExpression exprEq("{a: {$_internalExprEq: 0}}");
    ParsedMatchExpression regularEq("{a: {$eq: 0}}");
    {
        ParsedMatchExpression rhs("{a: {$gte: 0}}");
        ASSERT_FALSE(expression::isSubsetOf(exprEq.get(), rhs.get()));
        ASSERT_TRUE(expression::isSubsetOf(regularEq.get(), rhs.get()));
    }

    {
        ParsedMatchExpression rhs("{a: {$lte: 0}}");
        ASSERT_FALSE(expression::isSubsetOf(exprEq.get(), rhs.get()));
        ASSERT_TRUE(expression::isSubsetOf(regularEq.get(), rhs.get()));
    }
}

TEST(ExpressionAlgoIsSubsetOf, IsSubsetOfRHSAndWithinOr) {
    ParsedMatchExpression rhs("{$or: [{a: 3}, {$and: [{a: 5}, {b: 5}]}]}");
    {
        ParsedMatchExpression lhs("{a:5, b:5}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), rhs.get()));
    }
}

TEST(ExpressionAlgoIsSubsetOf, IsSubsetOfComplexRHSExpression) {
    ParsedMatchExpression complex("{$or: [{z: 1}, {$and: [{x: 1}, {$or: [{y: 1}, {y: 2}]}]}]}");
    {
        ParsedMatchExpression lhs("{z: 1}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), complex.get()));
    }

    {
        ParsedMatchExpression lhs("{z: 1, x: 1, y:2}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), complex.get()));
    }

    {
        ParsedMatchExpression lhs("{$or: [{z: 1}, {$and: [{x: 1}, {$or: [{y: 1}, {y: 2}]}]}]}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), complex.get()));
    }


    {
        ParsedMatchExpression lhs("{$or: [{z: 2}, {$and: [{x: 2}, {$or: [{y: 3}, {y: 4}]}]}]}");
        ASSERT_FALSE(expression::isSubsetOf(lhs.get(), complex.get()));
    }


    {
        ParsedMatchExpression lhs("{z: 1, y:2}");
        ASSERT_TRUE(expression::isSubsetOf(lhs.get(), complex.get()));
    }

    {
        ParsedMatchExpression lhs("{z: 2, y: 1}");
        ASSERT_FALSE(expression::isSubsetOf(lhs.get(), complex.get()));
    }

    {
        ParsedMatchExpression lhs("{x: 1, y: 3}");
        ASSERT_FALSE(expression::isSubsetOf(lhs.get(), complex.get()));
    }
}

TEST(IsIndependent, AndIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{$and: [{a: 1}, {b: 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"b"}));
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"c"}));
}

TEST(IsIndependent, ElemMatchIsNotIndependent) {
    BSONObj matchPredicate = fromjson("{x: {$elemMatch: {y: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"x"}));
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"x.y"}));
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"y"}));
}

TEST(IsIndependent, NorIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{$nor: [{a: 1}, {b: 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"b"}));
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"c"}));
}

TEST(IsIndependent, NotIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{a: {$not: {$eq: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"b"}));
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"a"}));
}

TEST(IsIndependent, OrIsIndependentOnlyIfChildrenAre) {
    BSONObj matchPredicate = fromjson("{$or: [{a: 1}, {b: 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"a"}));
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"c"}));
}

TEST(IsIndependent, AndWithDottedFieldPathsIsNotIndependent) {
    BSONObj matchPredicate = fromjson("{$and: [{'a': 1}, {'a.b': 1}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"a.b.c"}));
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"a.b"}));
}

TEST(IsIndependent, BallIsIndependentOfBalloon) {
    BSONObj matchPredicate = fromjson("{'a.ball': 4}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(status.getStatus());

    unique_ptr<MatchExpression> expr = std::move(status.getValue());
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"a.balloon"}));
    ASSERT_TRUE(expression::isIndependentOf(*expr.get(), {"a.b"}));
    ASSERT_FALSE(expression::isIndependentOf(*expr.get(), {"a.ball.c"}));
}

// This is a descriptive test to ensure that until renames are implemented for these expressions,
// matches on these expressions cannot be swapped with other stages.
TEST(IsIndependent, NonRenameableExpressionIsNotIndependent) {
    std::vector<std::string> stringExpressions = {
        // Category: kOther.
        "{$or: [{a: {$size: 3}}, {b: {$size: 4}}]}",
        // Category: kArrayMatching.
        "{$or: [{a: {$_internalSchemaMaxItems: 3}}, {b: {$_internalSchemaMaxItems: 4}}]}",
        "{$or: [{a: {$_internalSchemaMinItems: 3}}, {b: {$_internalSchemaMinItems: 4}}]}",
        "{$or: [{a: {$_internalSchemaObjectMatch: {b: 1}}},"
        "       {a: {$_internalSchemaObjectMatch: {b: 2}}}]}",
        "{$or: [{a: {$elemMatch: {b: 3}}}, {a: {$elemMatch: {b: 4}}}]}"};

    for (auto str : stringExpressions) {
        BSONObj matchPredicate = fromjson(str);
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
        ASSERT_OK(swMatchExpression.getStatus());
        auto matchExpression = std::move(swMatchExpression.getValue());

        // Both of these should be true once renames are implemented.
        ASSERT_FALSE(expression::isIndependentOf(*matchExpression.get(), {"c"}));
        ASSERT_FALSE(expression::isOnlyDependentOn(*matchExpression.get(), {"a", "b"}));
    }
}

TEST(IsIndependent, EmptyDependencySetsPassIsOnlyDependentOn) {
    BSONObj matchPredicate = fromjson("{}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    auto matchExpression = std::move(swMatchExpression.getValue());
    ASSERT_TRUE(expression::isOnlyDependentOn(*matchExpression.get(), {}));
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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);

    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{a: {$eq: 1}}"));
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{b: {$eq: 1}}"));
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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);

    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{$nor: [{a: {$eq: 1}}]}"));
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{$nor: [{b: {$eq: 1}}]}"));
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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);

    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{x: {$not: {$gt: 4}}}"));
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
    BSONObjBuilder bob;
    splitExpr.second->serialize(&bob, true);

    ASSERT_FALSE(splitExpr.first);
    ASSERT_BSONOBJ_EQ(bob.obj(), fromjson("{$or: [{a: {$eq: 1}}, {b: {$eq: 1}}]}"));
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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);

    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{$or: [{'a.b': {$eq: 3}}, {'a.b.c': {$eq: 4}}]}"));
    ASSERT_BSONOBJ_EQ(secondBob.obj(),
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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);

    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{'a.c': {$eq: 1}}"));
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{$and: [{a: {$eq: 1}}, {'a.b': {$eq: 1}}]}"));
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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);
    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{b: {$eq: 1}}"));

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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);
    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{$and: [{c: {$eq: 1}}, {b: {$eq: 2}}]}"));

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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);
    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{c: {$eq: 1}}"));

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{b: {$eq: 2}}"));
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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);
    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{$or: [{d: {$eq: 2}}, {e: {$eq: 3}}]}"));

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{a: {$eq: 1}}"));
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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);
    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{$or: [{x: {$eq: 2}}, {y: {$eq: 3}}]}"));

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{a: {$eq: 1}}"));
}

TEST(SplitMatchExpression, ShouldNotMoveElemMatchObjectAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$elemMatch: {b: 3}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_FALSE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{a: {$elemMatch: {b: {$eq: 3}}}}"));
}

TEST(SplitMatchExpression, ShouldNotMoveElemMatchValueAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$elemMatch: {$eq: 3}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_FALSE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{a: {$elemMatch: {$eq: 3}}}"));
}

TEST(SplitMatchExpression, ShouldMoveTypeAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$type: 16}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_TRUE(splitExpr.first.get());
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);
    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{c: {$type: [16]}}"));

    ASSERT_FALSE(splitExpr.second.get());
}

TEST(SplitMatchExpression, ShouldNotMoveSizeAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$size: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_FALSE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{a: {$size: 3}}"));
}

TEST(SplitMatchExpression, ShouldNotMoveMinItemsAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$_internalSchemaMinItems: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_FALSE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{a: {$_internalSchemaMinItems: 3}}"));
}

TEST(SplitMatchExpression, ShouldNotMoveMaxItemsAcrossRename) {
    BSONObj matchPredicate = fromjson("{a: {$_internalSchemaMaxItems: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_FALSE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);
    ASSERT_BSONOBJ_EQ(secondBob.obj(), fromjson("{a: {$_internalSchemaMaxItems: 3}}"));
}

TEST(SplitMatchExpression, ShouldNotMoveMaxItemsInLogicalExpressionAcrossRename) {
    BSONObj matchPredicate = fromjson(
        "{$or: [{a: {$_internalSchemaMaxItems: 3}},"
        "       {a: {$_internalSchemaMaxItems: 4}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "c"}};
    std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitExpr =
        expression::splitMatchExpressionBy(std::move(matcher.getValue()), {}, renames);

    ASSERT_FALSE(splitExpr.first.get());

    ASSERT_TRUE(splitExpr.second.get());
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);
    ASSERT_BSONOBJ_EQ(secondBob.obj(),
                      fromjson("{$or: [{a: {$_internalSchemaMaxItems: 3}},"
                               "       {a: {$_internalSchemaMaxItems: 4}}]}"));
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
    BSONObjBuilder secondBob;
    splitExpr.second->serialize(&secondBob, true);
    ASSERT_BSONOBJ_EQ(secondBob.obj(),
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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);
    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{c: {$_internalSchemaMinLength: 3}}"));

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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);
    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{c: {$_internalSchemaMaxLength: 3}}"));

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
    BSONObjBuilder firstBob;
    splitExpr.first->serialize(&firstBob, true);
    ASSERT_BSONOBJ_EQ(firstBob.obj(), fromjson("{x: {$eq: 3}}"));

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

        BSONObjBuilder oldBob;
        residual->serialize(&oldBob, true);
        ASSERT_BSONOBJ_EQ(oldBob.obj(), fromjson(randExpr));
    };

    // We should not push down a $match with a $rand expression.
    assertMatchDoesNotSplit(randExpr);

    // This is equivalent to 'randExpr'.
    assertMatchDoesNotSplit("{$sampleRate: 0.25}");
}

TEST(ApplyRenamesToExpression, ShouldApplyBasicRenamesForAMatchWithExpr) {
    BSONObj matchPredicate = fromjson("{$expr: {$eq: ['$a.b', '$c']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "d"}, {"c", "e"}, {"x", "y"}};
    expression::applyRenamesToExpression(matcher.getValue().get(), renames);

    ASSERT_BSONOBJ_EQ(matcher.getValue()->serialize(), fromjson("{$expr: {$eq: ['$d.b', '$e']}}"));
}

TEST(ApplyRenamesToExpression, ShouldApplyDottedRenamesForAMatchWithExpr) {
    BSONObj matchPredicate = fromjson("{$expr: {$lt: ['$a.b.c', '$d.e.f']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a.b.c", "x"}, {"d.e", "y"}};
    expression::applyRenamesToExpression(matcher.getValue().get(), renames);

    ASSERT_BSONOBJ_EQ(matcher.getValue()->serialize(), fromjson("{$expr: {$lt: ['$x', '$y.f']}}"));
}

TEST(ApplyRenamesToExpression, ShouldApplyDottedRenamesForAMatchWithNestedExpr) {
    BSONObj matchPredicate =
        fromjson("{$and: [{$expr: {$eq: ['$a.b.c', '$c']}}, {$expr: {$lt: ['$d.e.f', '$a']}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "x.y"}, {"d.e", "y"}, {"c", "q.r"}};
    expression::applyRenamesToExpression(matcher.getValue().get(), renames);

    ASSERT_BSONOBJ_EQ(
        matcher.getValue()->serialize(),
        fromjson(
            "{$and: [{$expr: {$eq: ['$x.y.b.c', '$q.r']}}, {$expr: {$lt: ['$y.f', '$x.y']}}]}"));
}

TEST(ApplyRenamesToExpression, ShouldNotApplyRenamesForAMatchWithExprWithNoFieldPaths) {
    BSONObj matchPredicate = fromjson("{$expr: {$concat: ['a', 'b', 'c']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matcher = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(matcher.getStatus());

    StringMap<std::string> renames{{"a", "x.y"}, {"d.e", "y"}, {"c", "q.r"}};
    expression::applyRenamesToExpression(matcher.getValue().get(), renames);

    ASSERT_BSONOBJ_EQ(
        matcher.getValue()->serialize(),
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

TEST(HasExistencePredicateOnPath, IdentifiesLeavesCorrectly) {
    BSONObj matchPredicate = fromjson("{$and: [{a: {$exists: true}}, {b: {$lte: 2}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(
        expression::hasExistencePredicateOnPath(*swMatchExpression.getValue().get(), "a"_sd));
    ASSERT_FALSE(
        expression::hasExistencePredicateOnPath(*swMatchExpression.getValue().get(), "b"_sd));
}

TEST(HasExistencePredicateOnPath, HandlesMultiplePredicatesWithSamePath) {
    BSONObj matchPredicate = fromjson("{$and: [{a: {$gt: 5000}}, {a: {$exists: false}}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(
        expression::hasExistencePredicateOnPath(*swMatchExpression.getValue().get(), "a"_sd));
}

TEST(HasExistencePredicateOnPath, DeeperTreeTest) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{q: {$gt: 5000}}, {$and: [{z: {$lte: 50}},"
        "{$or: [{f : {$gte: 4}}, {a : {$exists : true}}]}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(
        expression::hasExistencePredicateOnPath(*swMatchExpression.getValue().get(), "a"_sd));
}

TEST(HasExistencePredicateOnPath, HandlesDottedPathsInDeepTree) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{q: {$gt: 5000}}, {$and: [{z: {$lte: 50}},"
        "{$or: [{f : {$gte: 4}}, {'a.b.c.d' : {$exists : true}}]}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_TRUE(
        expression::hasExistencePredicateOnPath(*swMatchExpression.getValue().get(), "a.b.c.d"_sd));
}

TEST(HasExistencePredicateOnPath, ReturnsFalseWhenExistsOnlyOnPrefix) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{q: {$gt: 5000}}, {$and: [{z: {$lte: 50}},"
        "{$or: [{f : {$gte: 4}}, {'a' : {$exists : true}}]}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_FALSE(
        expression::hasExistencePredicateOnPath(*swMatchExpression.getValue().get(), "a.b"_sd));
}

TEST(HasExistencePredicateOnPath, ReturnsFalseWhenExistsOnSubpath) {
    BSONObj matchPredicate = fromjson(
        "{$and: [{q: {$gt: 5000}}, {$and: [{z: {$lte: 50}},"
        "{$or: [{f : {$gte: 4}}, {'a.b' : {$exists : true}}]}]}]}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto swMatchExpression = MatchExpressionParser::parse(matchPredicate, std::move(expCtx));
    ASSERT_OK(swMatchExpression.getStatus());
    ASSERT_FALSE(
        expression::hasExistencePredicateOnPath(*swMatchExpression.getValue().get(), "a"_sd));
}

TEST(SplitMatchExpressionForColumns, PreservesEmptyPredicates) {
    ParsedMatchExpression empty("{}");
    auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(empty.get());
    ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
    ASSERT(residual == nullptr);
}

TEST(SplitMatchExpressionForColumns, RejectsUnsupportedPredicates) {
    {
        // Future work.
        ParsedMatchExpression orClause("{$or: [{a: 1}, {b: 2}]}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(orClause.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(orClause, residual);
    }

    {
        // Would match missing values, not safe for a columnar index.
        ParsedMatchExpression alwaysTrue("{$alwaysTrue: 1}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(alwaysTrue.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(alwaysTrue, residual);
    }

    {
        // Future work.
        ParsedMatchExpression exprClause("{$expr: {$eq: ['$x', 0]}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(exprClause.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(exprClause, residual);
    }
}

// Test equality predicates that are safe to split (in contrast to next test).
TEST(SplitMatchExpressionForColumns, SplitsSafeEqualities) {

    {
        ParsedMatchExpression singleEqualsNumber("{albatross: 1}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(singleEqualsNumber.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::EQ)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }

    {
        ParsedMatchExpression singleEqualsString("{albatross: 'flying'}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(singleEqualsString.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::EQ)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }

    {
        ParsedMatchExpression doubleEqualsNumber("{albatross: 1, blackbird: 2}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(doubleEqualsNumber.get());
        ASSERT_EQ(splitUp.size(), 2) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::EQ)
            << splitUp.at("albatross")->toString();
        ASSERT(splitUp.contains("blackbird"));
        ASSERT(splitUp.at("blackbird")->matchType() == MatchExpression::EQ)
            << splitUp.at("blackbird")->toString();
        ASSERT(residual == nullptr);
    }

    {
        ParsedMatchExpression mixedEquals(
            "{albatross: 1,"
            " blackbird: 'flying',"
            " cowbird: {$eq: /oreo/},"
            " duck: NumberInt(2),"
            " eagle: NumberLong(50),"
            " grackle: ObjectId('000000000000000000000000'),"
            " heron: true,"
            " ibis: false,"
            " jay: Timestamp(1, 0),"
            " kiwi: NumberDecimal('22'),"
            " 'loggerhead shrike': {$minKey: 1},"
            " mallard: {$maxKey: 1}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(mixedEquals.get());
        ASSERT_EQ(splitUp.size(), 12) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::EQ)
            << splitUp.at("albatross")->toString();
        ASSERT(splitUp.contains("blackbird"));
        ASSERT(splitUp.at("blackbird")->matchType() == MatchExpression::EQ)
            << splitUp.at("blackbird")->toString();
        ASSERT(residual == nullptr);
    }
}


TEST(SplitMatchExpressionForColumns, SupportsEqualityToEmptyObjects) {
    {
        ParsedMatchExpression equalsEmptyObj("{albatross: {}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(equalsEmptyObj.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::EQ)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
}

TEST(SplitMatchExpressionForColumns, SupportsEqualityToEmptyArray) {
    {
        ParsedMatchExpression equalsEmptyArray("{albatross: []}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(equalsEmptyArray.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::EQ)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
}

TEST(SplitMatchExpressionForColumns, DoesNotSupportEqualsNull) {
    {
        ParsedMatchExpression equalsNull("{a: null}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(equalsNull.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(equalsNull, residual);
    }
}

TEST(SplitMatchExpressionForColumns, DoesSupportNotEqualsNull) {
    {
        ParsedMatchExpression neNull("{a: {$ne: null}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(neNull.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("a"));
        ASSERT(splitUp.at("a")->matchType() == MatchExpression::NOT) << splitUp.at("a")->toString();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression notEqualsNull("{a: {$not: {$eq: null}}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(notEqualsNull.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("a"));
        ASSERT(splitUp.at("a")->matchType() == MatchExpression::NOT) << splitUp.at("a")->toString();
        ASSERT(residual == nullptr);
    }
}

TEST(SplitMatchExpressionForColumns, DoesNotSupportCompoundEquals) {
    {
        ParsedMatchExpression implicitEqualsArray("{a: [1, 2]}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(implicitEqualsArray.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(implicitEqualsArray, residual);
    }
    {
        ParsedMatchExpression explicitEqualsArray("{a: {$eq: [1, 2]}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(explicitEqualsArray.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(explicitEqualsArray, residual);
    }
    {
        ParsedMatchExpression implicitEqualsObject("{a: {boats: 1, planes: 2}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(implicitEqualsObject.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(implicitEqualsObject, residual);
    }
    {
        ParsedMatchExpression explicitEqualsObject("{a: {$eq: {boats: 1, planes: 2}}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(explicitEqualsObject.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(explicitEqualsObject, residual);
    }
    // We should be able to do dotted path version though, as a potential workaround.
    {
        ParsedMatchExpression equalsDotted("{'a.boats': 1, 'a.planes': 2}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(equalsDotted.get());
        ASSERT_GT(splitUp.size(), 0);
        ASSERT(splitUp.size() == 2);
        ASSERT(splitUp.contains("a.boats"));
        ASSERT(splitUp.at("a.boats")->matchType() == MatchExpression::EQ)
            << splitUp.at("a.boats")->toString();
        ASSERT(splitUp.contains("a.planes"));
        ASSERT(splitUp.at("a.planes")->matchType() == MatchExpression::EQ)
            << splitUp.at("a.planes")->toString();
        ASSERT(residual == nullptr);
    }
}

// Tests that comparisons (like $lt and $gte) have the same splitting rules as equality.
TEST(SplitMatchExpressionForColumns, SupportsComparisonsLikeEqualities) {

    {
        ParsedMatchExpression singleLtNumber("{albatross: {$lt: 1}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(singleLtNumber.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::LT)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression singleLteNumber("{albatross: {$lte: 1}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(singleLteNumber.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::LTE)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression singleGtNumber("{albatross: {$gt: 1}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(singleGtNumber.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::GT)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression singleGteNumber("{albatross: {$gte: 1}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(singleGteNumber.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::GTE)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression combinationPredicate(
            "{"
            " albatross: {$lt: 100},"
            " blackbird: {$gt: 0},"
            " cowbird: {$gte: 0, $lte: 100}"
            "}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(combinationPredicate.get());
        ASSERT_EQ(splitUp.size(), 3) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::LT)
            << splitUp.at("albatross")->toString();
        ASSERT(splitUp.contains("blackbird"));
        ASSERT(splitUp.at("blackbird")->matchType() == MatchExpression::GT)
            << splitUp.at("blackbird")->toString();
        ASSERT(splitUp.contains("cowbird"));
        ASSERT(splitUp.at("cowbird")->matchType() == MatchExpression::AND)
            << splitUp.at("cowbird")->toString();
        ASSERT(residual == nullptr);
    }
}

// While equality to [] or {} is OK, inequality is not so obvious. Left as future work.
TEST(SplitMatchExpressionForColumns, DoesNotSupportInequalitiesToObjectsOrArrays) {
    {
        ParsedMatchExpression ltArray("{albatross: {$lt: []}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(ltArray.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(ltArray, residual);
    }
    {
        ParsedMatchExpression ltObject("{albatross: {$lt: {}}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(ltObject.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(ltObject, residual);
    }
    {
        ParsedMatchExpression lteArray("{albatross: {$lte: []}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(lteArray.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(lteArray, residual);
    }
    {
        ParsedMatchExpression lteObject("{albatross: {$lte: {}}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(lteObject.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(lteObject, residual);
    }
    {
        ParsedMatchExpression gtArray("{albatross: {$gt: []}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(gtArray.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(gtArray, residual);
    }
    {
        ParsedMatchExpression gtObject("{albatross: {$gt: {}}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(gtObject.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(gtObject, residual);
    }
    {
        ParsedMatchExpression gteArray("{albatross: {$gte: []}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(gteArray.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(gteArray, residual);
    }
    {
        ParsedMatchExpression gteObject("{albatross: {$gte: {}}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(gteObject.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(gteObject, residual);
    }
}

// Tests that comparisons which only match values of a certain type are allowed.
TEST(SplitMatchExpressionForColumns, SupportsTypeSpecificPredicates) {
    ParsedMatchExpression combinationPredicate(
        "{"
        " albatross: /oreo/,"
        " blackbird: {$mod: [2, 0]},"
        " cowbird: {$bitsAllSet: 7},"
        " duck: {$bitsAllClear: 24},"
        " eagle: {$bitsAnySet: 7},"
        " falcon: {$bitsAnyClear: 24}"
        "}");
    auto&& [splitUp, residual] =
        expression::splitMatchExpressionForColumns(combinationPredicate.get());
    ASSERT_EQ(splitUp.size(), 6) << splitUp.size();
    ASSERT(splitUp.contains("albatross"));
    ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::REGEX)
        << splitUp.at("albatross")->toString();
    ASSERT(splitUp.contains("blackbird"));
    ASSERT(splitUp.at("blackbird")->matchType() == MatchExpression::MOD)
        << splitUp.at("blackbird")->toString();
    ASSERT(splitUp.contains("cowbird"));
    ASSERT(splitUp.at("cowbird")->matchType() == MatchExpression::BITS_ALL_SET)
        << splitUp.at("cowbird")->toString();
    ASSERT(splitUp.contains("duck"));
    ASSERT(splitUp.at("duck")->matchType() == MatchExpression::BITS_ALL_CLEAR)
        << splitUp.at("duck")->toString();
    ASSERT(splitUp.contains("eagle"));
    ASSERT(splitUp.at("eagle")->matchType() == MatchExpression::BITS_ANY_SET)
        << splitUp.at("eagle")->toString();
    ASSERT(splitUp.contains("falcon"));
    ASSERT(splitUp.at("falcon")->matchType() == MatchExpression::BITS_ANY_CLEAR)
        << splitUp.at("falcon")->toString();
    ASSERT(residual == nullptr);
}

TEST(SplitMatchExpressionForColumns, SupportsInWithRegexes) {
    {
        // First confirm a $in clause is supported without regexes.
        ParsedMatchExpression stringInClause("{albatross: {$in: ['big', 'ol', 'bird']}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(stringInClause.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::MATCH_IN)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        // Test that $in with regexes is supported also work.
        ParsedMatchExpression regexInClause("{albatross: {$in: [/big/, /bird/]}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(regexInClause.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::MATCH_IN)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        // Test that a mix of both is supported
        ParsedMatchExpression regexInClause("{albatross: {$in: [/big/, 'bird']}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(regexInClause.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::MATCH_IN)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        // Test that it is still disallowed if there's a disqualifying equality such as a null.
        ParsedMatchExpression regexInClause("{albatross: {$in: [/big/, null, 'bird']}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(regexInClause.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(regexInClause, residual);
    }
}

TEST(SplitMatchExpressionForColumns, SupportsExistsTrue) {
    ParsedMatchExpression existsPredicate("{albatross: {$exists: true}}");
    auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(existsPredicate.get());
    ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
    ASSERT(splitUp.contains("albatross"));
    ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::EXISTS)
        << splitUp.at("albatross")->toString();
    ASSERT(residual == nullptr);
}

TEST(SplitMatchExpressionForColumns, DoesNotSupportExistsFalse) {
    ParsedMatchExpression existsPredicate("{albatross: {$exists: false}}");
    auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(existsPredicate.get());
    ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
    assertMatchesEqual(existsPredicate, residual);
}

// $in constraints are similar to equality. Most of them should work, exceptions broken out in the
// next test.
TEST(SplitMatchExpressionForColumns, SupportsInPredicates) {
    {
        ParsedMatchExpression emptyIn("{albatross: {$in: []}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(emptyIn.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::MATCH_IN)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression singleElementIn("{albatross: {$in: [4]}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(singleElementIn.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::MATCH_IN)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression inWithEmptyArray("{albatross: {$in: [[]]}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(inWithEmptyArray.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::MATCH_IN)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression inWithEmptyObject("{albatross: {$in: [{}]}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(inWithEmptyObject.get());
        ASSERT_GT(splitUp.size(), 0);
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::MATCH_IN)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression mixedTypeIn("{albatross: {$in: [4, {}, [], 'string', /regex/]}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(mixedTypeIn.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::MATCH_IN)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
}

// We can't support compound types, just like for equality.
TEST(SplitMatchExpressionForColumns, DoesNotSupportCertainInEdgeCases) {
    {
        ParsedMatchExpression inWithArray("{albatross: {$in: [[2,3]]}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(inWithArray.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(inWithArray, residual);
    }
    {
        ParsedMatchExpression inWithObject("{albatross: {$in: [{wings: 2}]}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(inWithObject.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(inWithObject, residual);
    }
    {
        ParsedMatchExpression inWithNull("{albatross: {$in: [null]}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(inWithNull.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(inWithNull, residual);
    }
    {
        ParsedMatchExpression unsupportedMixedInWithSupported(
            "{albatross: {$in: ['strings', 1, null, {x: 4}, [0, 0], 4]}}");
        auto&& [splitUp, residual] =
            expression::splitMatchExpressionForColumns(unsupportedMixedInWithSupported.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(unsupportedMixedInWithSupported, residual);
    }
}

TEST(SplitMatchExpressionForColumns, SupportsTypePredicates) {
    {
        ParsedMatchExpression intFilter("{albatross: {$type: 'int'}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(intFilter.get());
        ASSERT_GT(splitUp.size(), 0);
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::TYPE_OPERATOR)
            << splitUp.at("albatross")->toString();
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression numberFilter("{albatross: {$type: 'number'}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(numberFilter.get());
        ASSERT_GT(splitUp.size(), 0);
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::TYPE_OPERATOR)
            << splitUp.at("albatross")->toString();
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression stringFilter("{albatross: {$type: 'string'}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(stringFilter.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::TYPE_OPERATOR)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
    {
        ParsedMatchExpression nullFilter("{albatross: {$type: 'null'}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(nullFilter.get());
        ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
        ASSERT(splitUp.contains("albatross"));
        ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::TYPE_OPERATOR)
            << splitUp.at("albatross")->toString();
        ASSERT(residual == nullptr);
    }
}

TEST(SplitMatchExpressionForColumns, DoesNotSupportQueriesForTypeObject) {
    ParsedMatchExpression objectFilter("{albatross: {$type: 'object'}}");
    auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(objectFilter.get());
    ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
    assertMatchesEqual(objectFilter, residual);
}

// This may be workable. But until we can prove it we'll disallow {$type: "array"}.
TEST(SplitMatchExpressionForColumns, DoesNotSupportQueriesForTypeArray) {
    ParsedMatchExpression arrayFilter("{albatross: {$type: 'array'}}");
    auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(arrayFilter.get());
    ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
    assertMatchesEqual(arrayFilter, residual);
}

TEST(SplitMatchExpressionForColumns, DoesNotSupportNotQueries) {
    {
        ParsedMatchExpression notEqFilter("{albatross: {$not: {$eq: 2}}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(notEqFilter.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(notEqFilter, residual);
    }
    {
        ParsedMatchExpression notAndFilter("{albatross: {$not: {$gt: 2, $lt: 10}}}");
        auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(notAndFilter.get());
        ASSERT_EQ(splitUp.size(), 0) << splitUp.size();
        assertMatchesEqual(notAndFilter, residual);
    }
}

TEST(SplitMatchExpressionForColumns, CanCombinePredicates) {
    ParsedMatchExpression compoundFilter(
        "{"
        " albatross: {$gte: 100},"
        " albatross: {$mod: [2, 0]}"
        "}");
    auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(compoundFilter.get());
    ASSERT_EQ(splitUp.size(), 1) << splitUp.size();
    ASSERT(splitUp.contains("albatross"));
    ASSERT(splitUp["albatross"]->matchType() == MatchExpression::AND)
        << splitUp.at("albatross")->toString();
    ASSERT_EQ(splitUp.at("albatross")->numChildren(), 2) << splitUp.at("albatross")->toString();
    // Don't care about the order.
    auto andExpr = splitUp.at("albatross").get();
    auto firstChild = andExpr->getChild(0);
    if (firstChild->matchType() == MatchExpression::GTE) {
        ASSERT(firstChild->matchType() == MatchExpression::GTE) << firstChild->toString();
        ASSERT(andExpr->getChild(1)->matchType() == MatchExpression::MOD) << firstChild->toString();
    } else {
        ASSERT(firstChild->matchType() == MatchExpression::MOD) << firstChild->toString();
        ASSERT(andExpr->getChild(1)->matchType() == MatchExpression::GTE) << firstChild->toString();
    }
    ASSERT(residual == nullptr);
}

TEST(SplitMatchExpressionForColumns, CanSplitPredicate) {
    ParsedMatchExpression complexPredicate(R"({
        a: {$gte: 0, $lt: 10},
        "addresses.zip": {$in: ["12345", "01234"]},
        unsubscribed: false,
        specialAddress: {$exists: false}
        })");
    auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(complexPredicate.get());
    ASSERT_EQ(splitUp.size(), 3) << splitUp.size();
    ASSERT(splitUp.contains("a"));
    ASSERT(splitUp.at("a")->matchType() == MatchExpression::AND) << splitUp.at("a")->toString();
    ASSERT_EQ(splitUp.at("a")->numChildren(), 2) << splitUp.at("a")->toString();
    ASSERT(splitUp.contains("addresses.zip"));
    ASSERT(splitUp.contains("unsubscribed"));
    ASSERT(!splitUp.contains("specialAddress"));
    ParsedMatchExpression expectedResidual("{specialAddress: {$exists: false}}");
    assertMatchesEqual(expectedResidual, residual);
}

TEST(SplitMatchExpressionForColumns, SupportsDottedPaths) {
    ParsedMatchExpression compoundFilter(
        "{"
        " albatross: /oreo/,"
        " \"blackbird.feet\": {$mod: [2, 0]},"
        " \"blackbird.softwareUpdates\": {$bitsAllSet: 7},"
        // Stress the path combination logic with some prefixes and suffixes to be sure.
        " blackbird: {$ne: null},"
        " bla: {$ne: null},"
        " blackbirds: {$exists: true},"
        " \"blackbird.feetsies\": {$ne: null},"
        " \"cowbird.beakLength\": {$gte: 24, $lt: 40},"
        " \"cowbird.eggSet\": {$bitsAnySet: 7}"
        "}");
    auto&& [splitUp, residual] = expression::splitMatchExpressionForColumns(compoundFilter.get());
    ASSERT(residual == nullptr);
    ASSERT_EQ(splitUp.size(), 9) << splitUp.size();
    ASSERT(splitUp.contains("albatross"));
    ASSERT(splitUp.at("albatross")->matchType() == MatchExpression::REGEX)
        << splitUp.at("albatross")->toString();
    ASSERT(splitUp.contains("blackbird.feet"));
    ASSERT(splitUp.at("blackbird.feet")->matchType() == MatchExpression::MOD)
        << splitUp.at("blackbird.feet")->toString();
    ASSERT(splitUp.contains("blackbird.softwareUpdates"));
    ASSERT(splitUp.at("blackbird.softwareUpdates")->matchType() == MatchExpression::BITS_ALL_SET)
        << splitUp.at("blackbird.softwareUpdates")->toString();
    ASSERT(splitUp.contains("blackbird"));
    ASSERT(splitUp.at("blackbird")->matchType() == MatchExpression::NOT)
        << splitUp.at("blackbird")->toString();
    ASSERT(splitUp.contains("bla"));
    ASSERT(splitUp.contains("blackbirds"));
    ASSERT(splitUp.at("blackbirds")->matchType() == MatchExpression::EXISTS)
        << splitUp.at("blackbirds")->toString();
    ASSERT(splitUp.contains("blackbird.feetsies"));
    ASSERT(splitUp.at("cowbird.beakLength")->matchType() == MatchExpression::AND)
        << splitUp.at("cowbird.beakLength")->toString();
    ASSERT_EQ(splitUp.at("cowbird.beakLength")->numChildren(), 2)
        << splitUp.at("cowbird.beakLength")->toString();
    ASSERT(splitUp.at("cowbird.eggSet")->matchType() == MatchExpression::BITS_ANY_SET)
        << splitUp.at("cowbird.eggSet")->toString();
    ASSERT(!splitUp.contains("cowbird"));
    ASSERT(residual == nullptr);
}

TEST(SplitMatchExpressionForColumns, LeavesOriginalMatchExpressionFunctional) {
    ParsedMatchExpression combinationPredicate(
        "{"
        " albatross: {$lt: 100},"
        " blackbird: {$gt: 0},"
        " cowbird: {$gte: 0, $lte: 100}"
        "}");
    auto&& [splitUp, residual] =
        expression::splitMatchExpressionForColumns(combinationPredicate.get());
    ASSERT_GT(splitUp.size(), 0);
    ASSERT(residual == nullptr);
    // Won't bother asserting on the detaiils here - done above.
    ASSERT(combinationPredicate.get()->matchesBSON(
        BSON("albatross" << 45 << "blackbird" << 1 << "cowbird" << 2)));
}

}  // namespace mongo

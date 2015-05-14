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

namespace mongo {

    /**
     * A MatchExpression does not hold the memory for BSONElements, so use ParsedMatchExpression to
     * ensure that the BSONObj outlives the MatchExpression.
     */
    class ParsedMatchExpression {
    public:
        ParsedMatchExpression(const std::string& str)
           : _obj(fromjson(str)) {
            StatusWithMatchExpression result = MatchExpressionParser::parse(_obj);
            ASSERT_OK(result.getStatus());
            _expr.reset(result.getValue());
        }

        const MatchExpression* get() const { return _expr.get(); }

    private:
        const BSONObj _obj;
        std::unique_ptr<MatchExpression> _expr;
    };

    TEST(ExpressionAlgoIsSubsetOf, NullAndOmittedField) {
        // Verify that ComparisonMatchExpression::init() prohibits creating a match expression with
        // an Undefined type.
        BSONObj undefined = fromjson("{a: undefined}");
        ASSERT_EQUALS(ErrorCodes::BadValue, MatchExpressionParser::parse(undefined).getStatus());

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

        ASSERT_TRUE(expression::isSubsetOf(nan.get(), nan.get()));
        ASSERT_FALSE(expression::isSubsetOf(nan.get(), lt.get()));
        ASSERT_FALSE(expression::isSubsetOf(lt.get(), nan.get()));
        ASSERT_FALSE(expression::isSubsetOf(nan.get(), lte.get()));
        ASSERT_FALSE(expression::isSubsetOf(lte.get(), nan.get()));
        ASSERT_FALSE(expression::isSubsetOf(nan.get(), gte.get()));
        ASSERT_FALSE(expression::isSubsetOf(gte.get(), nan.get()));
        ASSERT_FALSE(expression::isSubsetOf(nan.get(), gt.get()));
        ASSERT_FALSE(expression::isSubsetOf(gt.get(), nan.get()));
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

}  // namespace mongo

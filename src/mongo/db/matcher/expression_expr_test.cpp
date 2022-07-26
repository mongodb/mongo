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

#include "mongo/base/checked_cast.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using unittest::assertGet;

const double kNaN = std::numeric_limits<double>::quiet_NaN();

class ExprMatchTest : public mongo::unittest::Test {
public:
    ExprMatchTest() : _expCtx(new ExpressionContextForTest()) {}

    void createMatcher(const BSONObj& matchExpr) {
        _matchExpression = uassertStatusOK(
            MatchExpressionParser::parse(matchExpr,
                                         _expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures));
        _matchExpression = MatchExpression::optimize(std::move(_matchExpression));
    }

    void setCollator(std::unique_ptr<CollatorInterface> collator) {
        _expCtx->setCollator(std::move(collator));
        if (_matchExpression) {
            _matchExpression->setCollator(_expCtx->getCollator());
        }
    }

    void setVariable(StringData name, Value val) {
        auto varId = _expCtx->variablesParseState.defineVariable(name);
        _expCtx->variables.setValue(varId, val);
    }

    bool matches(const BSONObj& doc) {
        invariant(_matchExpression);
        return _matchExpression->matchesBSON(doc);
    }

    ExprMatchExpression* getExprMatchExpression() {
        return checked_cast<ExprMatchExpression*>(_matchExpression.get());
    }

private:
    const boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
    std::unique_ptr<MatchExpression> _matchExpression;
};

TEST_F(ExprMatchTest, ComparisonToConstantMatchesCorrectly) {
    createMatcher(BSON("$expr" << BSON("$eq" << BSON_ARRAY("$a" << 5))));

    ASSERT_TRUE(matches(BSON("a" << 5)));

    ASSERT_FALSE(matches(BSON("a" << 4)));
    ASSERT_FALSE(matches(BSON("a" << 6)));
}

TEST_F(ExprMatchTest, ComparisonToConstantVariableMatchesCorrectly) {
    setVariable("var", Value(5));
    createMatcher(BSON("$expr" << BSON("$eq" << BSON_ARRAY("$a"
                                                           << "$$var"))));

    ASSERT_TRUE(matches(BSON("a" << 5)));

    ASSERT_FALSE(matches(BSON("a" << 4)));
    ASSERT_FALSE(matches(BSON("a" << 6)));
}

TEST_F(ExprMatchTest, ComparisonBetweenTwoFieldPathsMatchesCorrectly) {
    createMatcher(BSON("$expr" << BSON("$gt" << BSON_ARRAY("$a"
                                                           << "$b"))));

    ASSERT_TRUE(matches(BSON("a" << 10 << "b" << 2)));

    ASSERT_FALSE(matches(BSON("a" << 2 << "b" << 2)));
    ASSERT_FALSE(matches(BSON("a" << 2 << "b" << 10)));
}

TEST_F(ExprMatchTest, ComparisonThrowsWithUnboundVariable) {
    ASSERT_THROWS(createMatcher(BSON("$expr" << BSON("$eq" << BSON_ARRAY("$a"
                                                                         << "$$var")))),
                  DBException);
}

TEST_F(ExprMatchTest, EqWithLHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$eq: ['$x', 3]}}"));

    ASSERT_TRUE(matches(BSON("x" << 3)));

    ASSERT_FALSE(matches(BSON("x" << 1)));
    ASSERT_FALSE(matches(BSON("x" << 10)));
}

TEST_F(ExprMatchTest, EqWithRHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$eq: [3, '$x']}}"));

    ASSERT_TRUE(matches(BSON("x" << 3)));

    ASSERT_FALSE(matches(BSON("x" << 1)));
    ASSERT_FALSE(matches(BSON("x" << 10)));
}

TEST_F(ExprMatchTest, NeWithLHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$ne: ['$x', 3]}}"));

    ASSERT_TRUE(matches(BSON("x" << 1)));
    ASSERT_TRUE(matches(BSON("x" << 10)));

    ASSERT_FALSE(matches(BSON("x" << 3)));
}

TEST_F(ExprMatchTest, NeWithFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$ne: [3, '$x']}}"));

    ASSERT_TRUE(matches(BSON("x" << 1)));
    ASSERT_TRUE(matches(BSON("x" << 10)));

    ASSERT_FALSE(matches(BSON("x" << 3)));
}

TEST_F(ExprMatchTest, GtWithLHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$gt: ['$x', 3]}}"));

    ASSERT_TRUE(matches(BSON("x" << 10)));

    ASSERT_FALSE(matches(BSON("x" << 1)));
    ASSERT_FALSE(matches(BSON("x" << 3)));
}

TEST_F(ExprMatchTest, GtWithRHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$gt: [3, '$x']}}"));

    ASSERT_TRUE(matches(BSON("x" << 1)));

    ASSERT_FALSE(matches(BSON("x" << 3)));
    ASSERT_FALSE(matches(BSON("x" << 10)));
}

TEST_F(ExprMatchTest, GteWithLHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$gte: ['$x', 3]}}"));

    ASSERT_TRUE(matches(BSON("x" << 3)));
    ASSERT_TRUE(matches(BSON("x" << 10)));

    ASSERT_FALSE(matches(BSON("x" << 1)));
}

TEST_F(ExprMatchTest, GteWithRHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$gte: [3, '$x']}}"));

    ASSERT_TRUE(matches(BSON("x" << 3)));
    ASSERT_TRUE(matches(BSON("x" << 1)));

    ASSERT_FALSE(matches(BSON("x" << 10)));
}

TEST_F(ExprMatchTest, LtWithLHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$lt: ['$x', 3]}}"));

    ASSERT_TRUE(matches(BSON("x" << 1)));

    ASSERT_FALSE(matches(BSON("x" << 3)));
    ASSERT_FALSE(matches(BSON("x" << 10)));
}

TEST_F(ExprMatchTest, LtWithRHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$lt: [3, '$x']}}"));

    ASSERT_TRUE(matches(BSON("x" << 10)));

    ASSERT_FALSE(matches(BSON("x" << 3)));
    ASSERT_FALSE(matches(BSON("x" << 1)));
}

TEST_F(ExprMatchTest, LteWithLHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$lte: ['$x', 3]}}"));

    ASSERT_TRUE(matches(BSON("x" << 3)));
    ASSERT_FALSE(matches(BSON("x" << 10)));
}

TEST_F(ExprMatchTest, LteWithRHSFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$lte: [3, '$x']}}"));

    ASSERT_TRUE(matches(BSON("x" << 3)));
    ASSERT_TRUE(matches(BSON("x" << 10)));

    ASSERT_FALSE(matches(BSON("x" << 1)));
}

TEST_F(ExprMatchTest, AndMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$and: [{$eq: ['$x', 3]}, {$ne: ['$y', 4]}]}}"));

    ASSERT_TRUE(matches(BSON("x" << 3)));
    ASSERT_TRUE(matches(BSON("x" << 3 << "y" << 5)));

    ASSERT_FALSE(matches(BSON("x" << 10 << "y" << 5)));
    ASSERT_FALSE(matches(BSON("x" << 3 << "y" << 4)));
    ASSERT_FALSE(matches(BSON("x" << 10 << "y" << 5)));
}

TEST_F(ExprMatchTest, OrMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$or: [{$lte: ['$x', 3]}, {$gte: ['$y', 4]}]}}"));

    ASSERT_TRUE(matches(BSON("x" << 3)));
    ASSERT_TRUE(matches(BSON("y" << 5)));

    ASSERT_FALSE(matches(BSON("x" << 10)));
}

TEST_F(ExprMatchTest, AndNestedWithinOrMatchesCorrectly) {
    createMatcher(fromjson(
        "{$expr: {$or: [{$and: [{$eq: ['$x', 3]}, {$gt: ['$z', 5]}]}, {$lt: ['$y', 4]}]}}"));

    ASSERT_TRUE(matches(BSON("x" << 3 << "z" << 7)));
    ASSERT_TRUE(matches(BSON("y" << 1)));

    ASSERT_FALSE(matches(BSON("y" << 5)));
}

TEST_F(ExprMatchTest, OrNestedWithinAndMatchesCorrectly) {
    createMatcher(fromjson(
        "{$expr: {$and: [{$or: [{$eq: ['$x', 3]}, {$eq: ['$z', 5]}]}, {$eq: ['$y', 4]}]}}"));

    ASSERT_TRUE(matches(BSON("x" << 3 << "y" << 4)));
    ASSERT_TRUE(matches(BSON("z" << 5 << "y" << 4)));
    ASSERT_TRUE(matches(BSON("x" << 3 << "z" << 5 << "y" << 4)));

    ASSERT_FALSE(matches(BSON("x" << 3 << "z" << 5)));
    ASSERT_FALSE(matches(BSON("y" << 4)));
    ASSERT_FALSE(matches(BSON("x" << 3 << "y" << 10)));
}

TEST_F(ExprMatchTest, InWithLhsFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$in: ['$x', [1, 2, 3]]}}"));

    ASSERT_TRUE(matches(BSON("x" << 1)));
    ASSERT_TRUE(matches(BSON("x" << 3)));

    ASSERT_FALSE(matches(BSON("x" << 5)));
    ASSERT_FALSE(matches(BSON("y" << 2)));
    ASSERT_FALSE(matches(BSON("x" << BSON("y" << 2))));
}

TEST_F(ExprMatchTest, InWithLhsFieldPathAndArrayAsConstMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$in: ['$x', {$const: [1, 2, 3]}]}}"));

    ASSERT_TRUE(matches(BSON("x" << 1)));
    ASSERT_TRUE(matches(BSON("x" << 3)));

    ASSERT_FALSE(matches(BSON("x" << 5)));
    ASSERT_FALSE(matches(BSON("y" << 2)));
    ASSERT_FALSE(matches(BSON("x" << BSON("y" << 2))));
}

TEST_F(ExprMatchTest, CmpMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$cmp: ['$x', 3]}}"));

    ASSERT_TRUE(matches(BSON("x" << 2)));
    ASSERT_TRUE(matches(BSON("x" << 4)));
    ASSERT_TRUE(matches(BSON("y" << 3)));

    ASSERT_FALSE(matches(BSON("x" << 3)));
}

TEST_F(ExprMatchTest, ConstantLiteralExpressionMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$literal: {$eq: ['$x', 10]}}}"));

    ASSERT_TRUE(matches(BSON("x" << 2)));
}

TEST_F(ExprMatchTest, ConstantPositiveNumberExpressionMatchesCorrectly) {
    createMatcher(fromjson("{$expr: 1}"));

    ASSERT_TRUE(matches(BSON("x" << 2)));
}

TEST_F(ExprMatchTest, ConstantNegativeNumberExpressionMatchesCorrectly) {
    createMatcher(fromjson("{$expr: -1}"));

    ASSERT_TRUE(matches(BSON("x" << 2)));
}

TEST_F(ExprMatchTest, ConstantNumberZeroExpressionMatchesCorrectly) {
    createMatcher(fromjson("{$expr: 0}"));

    ASSERT_FALSE(matches(BSON("x" << 2)));
}

TEST_F(ExprMatchTest, ConstantTrueValueExpressionMatchesCorrectly) {
    createMatcher(fromjson("{$expr: true}"));

    ASSERT_TRUE(matches(BSON("x" << 2)));
}

TEST_F(ExprMatchTest, ConstantFalseValueExpressionMatchesCorrectly) {
    createMatcher(fromjson("{$expr: false}"));

    ASSERT_FALSE(matches(BSON("x" << 2)));
}

TEST_F(ExprMatchTest, EqWithTwoFieldPathsMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$eq: ['$x', '$y']}}"));

    ASSERT_TRUE(matches(BSON("x" << 2 << "y" << 2)));

    ASSERT_FALSE(matches(BSON("x" << 2 << "y" << 3)));
    ASSERT_FALSE(matches(BSON("x" << 2)));
}

TEST_F(ExprMatchTest, EqWithTwoConstantsMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$eq: [3, 4]}}"));

    ASSERT_FALSE(matches(BSON("x" << 3)));
}

TEST_F(ExprMatchTest, EqWithDottedFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$eq: ['$x.y', 3]}}"));

    ASSERT_TRUE(matches(BSON("x" << BSON("y" << 3))));

    ASSERT_FALSE(matches(BSON("x" << BSON("y" << BSON_ARRAY(3)))));
    ASSERT_FALSE(matches(BSON("x" << BSON_ARRAY(BSON("y" << 3)))));
    ASSERT_FALSE(matches(BSON("x" << BSON_ARRAY(BSON("y" << BSON_ARRAY(3))))));
}

TEST_F(ExprMatchTest, InWithDottedFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$in: ['$x.y', [1, 2, 3]]}}"));

    ASSERT_TRUE(matches(BSON("x" << BSON("y" << 3))));

    ASSERT_FALSE(matches(BSON("x" << BSON("y" << BSON_ARRAY(3)))));
}

TEST_F(ExprMatchTest, AndWithNoMatchRewritableChildrenMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$and: [{$eq: ['$w', '$x']}, {$eq: ['$y', '$z']}]}}"));

    ASSERT_TRUE(matches(BSON("w" << 2 << "x" << 2 << "y" << 5 << "z" << 5)));

    ASSERT_FALSE(matches(BSON("w" << 1 << "x" << 2 << "y" << 5 << "z" << 5)));
    ASSERT_FALSE(matches(BSON("w" << 2 << "x" << 2 << "y" << 5 << "z" << 6)));
    ASSERT_FALSE(matches(BSON("w" << 2 << "y" << 5)));
}

TEST_F(ExprMatchTest, OrWithDistinctMatchRewritableAndNonMatchRewritableChildrenMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$or: [{$eq: ['$x', 1]}, {$eq: ['$y', '$z']}]}}"));

    ASSERT_TRUE(matches(BSON("x" << 1)));
    ASSERT_TRUE(matches(BSON("y" << 1 << "z" << 1)));

    ASSERT_FALSE(matches(BSON("x" << 2 << "y" << 3)));
    ASSERT_FALSE(matches(BSON("y" << 1)));
    ASSERT_FALSE(matches(BSON("y" << 1 << "z" << 2)));
}

TEST_F(ExprMatchTest, InWithoutLhsFieldPathMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$in: [2, [1, 2, 3]]}}"));
    ASSERT_TRUE(matches(BSON("x" << 2)));

    createMatcher(fromjson("{$expr: {$in: [2, [5, 6, 7]]}}"));
    ASSERT_FALSE(matches(BSON("x" << 2)));
}

TEST_F(ExprMatchTest, NestedAndWithTwoFieldPathsWithinOrMatchesCorrectly) {
    createMatcher(fromjson(
        "{$expr: {$or: [{$and: [{$eq: ['$x', '$w']}, {$eq: ['$z', 5]}]}, {$eq: ['$y', 4]}]}}"));

    ASSERT_TRUE(matches(BSON("x" << 2 << "w" << 2 << "z" << 5)));
    ASSERT_TRUE(matches(BSON("y" << 4)));

    ASSERT_FALSE(matches(BSON("x" << 2 << "w" << 4)));
    ASSERT_FALSE(matches(BSON("y" << 5)));
}

TEST_F(ExprMatchTest, AndWithDistinctMatchAndNonMatchSubTreeMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$and: [{$eq: ['$x', 1]}, {$eq: ['$y', '$z']}]}}"));

    ASSERT_TRUE(matches(BSON("x" << 1 << "y" << 2 << "z" << 2)));

    ASSERT_FALSE(matches(BSON("x" << 2 << "y" << 2 << "z" << 2)));
    ASSERT_FALSE(matches(BSON("x" << 1 << "y" << 2 << "z" << 10)));
    ASSERT_FALSE(matches(BSON("x" << 1 << "y" << 2)));
}

TEST_F(ExprMatchTest, ExprLtDoesNotUseTypeBracketing) {
    createMatcher(fromjson("{$expr: {$lt: ['$x', true]}}"));

    ASSERT_TRUE(matches(BSON("x" << false)));
    ASSERT_TRUE(matches(BSON("x" << BSON("y" << 1))));
    ASSERT_TRUE(matches(BSONObj()));

    ASSERT_FALSE(matches(BSON("x" << Timestamp(0, 1))));
}

TEST_F(ExprMatchTest, NullMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$eq: ['$x', null]}}"));

    ASSERT_TRUE(matches(BSON("x" << BSONNULL)));

    ASSERT_FALSE(matches(BSON("x" << BSONUndefined)));
    ASSERT_FALSE(matches(BSONObj()));
}

TEST_F(ExprMatchTest, UndefinedMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$eq: ['$x', undefined]}}"));

    ASSERT_TRUE(matches(BSON("x" << BSONUndefined)));
    ASSERT_TRUE(matches(BSONObj()));

    ASSERT_FALSE(matches(BSON("x" << BSONNULL)));
}


TEST_F(ExprMatchTest, NaNMatchesCorrectly) {
    createMatcher(fromjson("{$expr: {$eq: ['$x', NaN]}}"));

    ASSERT_TRUE(matches(BSON("x" << kNaN)));

    ASSERT_FALSE(matches(BSONObj()));
    ASSERT_FALSE(matches(BSON("x" << 0)));
    ASSERT_FALSE(matches(BSONObj()));

    createMatcher(fromjson("{$expr: {$lt: ['$x', NaN]}}"));

    ASSERT_TRUE(matches(BSONObj()));

    ASSERT_FALSE(matches(BSON("x" << kNaN)));
    ASSERT_FALSE(matches(BSON("x" << 0)));

    createMatcher(fromjson("{$expr: {$lte: ['$x', NaN]}}"));

    ASSERT_TRUE(matches(BSONObj()));
    ASSERT_TRUE(matches(BSON("x" << kNaN)));

    ASSERT_FALSE(matches(BSON("x" << 0)));

    createMatcher(fromjson("{$expr: {$gt: ['$x', NaN]}}"));

    ASSERT_TRUE(matches(BSON("x" << 0)));

    ASSERT_FALSE(matches(BSON("x" << kNaN)));
    ASSERT_FALSE(matches(BSONObj()));

    createMatcher(fromjson("{$expr: {$gte: ['$x', NaN]}}"));

    ASSERT_TRUE(matches(BSON("x" << 0)));
    ASSERT_TRUE(matches(BSON("x" << kNaN)));

    ASSERT_FALSE(matches(BSONObj()));
}

TEST_F(ExprMatchTest, MatchAgainstArrayIsCorrect) {
    createMatcher(fromjson("{$expr: {$gt: ['$x', 4]}}"));

    // Matches because BSONType Array is greater than BSONType double.
    ASSERT_TRUE(matches(BSON("x" << BSON_ARRAY(1.0 << 2.0 << 3.0))));

    createMatcher(fromjson("{$expr: {$eq: ['$x', [4]]}}"));

    ASSERT_TRUE(matches(BSON("x" << BSON_ARRAY(4))));

    ASSERT_FALSE(matches(BSON("x" << 4)));
}

TEST_F(ExprMatchTest, ComplexExprMatchesCorrectly) {
    createMatcher(
        fromjson("{"
                 "  $expr: {"
                 "      $and: ["
                 "          {$eq: ['$a', 1]},"
                 "          {$eq: ['$b', '$c']},"
                 "          {"
                 "            $or: ["
                 "                {$eq: ['$d', 1]},"
                 "                {$eq: ['$e', 3]},"
                 "                {"
                 "                  $and: ["
                 "                      {$eq: ['$f', 1]},"
                 "                      {$eq: ['$g', '$h']},"
                 "                      {$or: [{$eq: ['$i', 3]}, {$eq: ['$j', '$k']}]}"
                 "                  ]"
                 "                }"
                 "            ]"
                 "          }"
                 "      ]"
                 "  }"
                 "}"));

    ASSERT_TRUE(matches(BSON("a" << 1 << "b" << 3 << "c" << 3 << "d" << 1)));
    ASSERT_TRUE(matches(BSON("a" << 1 << "b" << 3 << "c" << 3 << "e" << 3)));
    ASSERT_TRUE(matches(BSON("a" << 1 << "b" << 3 << "c" << 3 << "f" << 1 << "i" << 3)));
    ASSERT_TRUE(
        matches(BSON("a" << 1 << "b" << 3 << "c" << 3 << "f" << 1 << "j" << 5 << "k" << 5)));

    ASSERT_FALSE(matches(BSON("a" << 1)));
    ASSERT_FALSE(matches(BSON("a" << 1 << "b" << 3 << "c" << 3)));
    ASSERT_FALSE(matches(BSON("a" << 1 << "b" << 3 << "c" << 3 << "d" << 5)));
    ASSERT_FALSE(matches(BSON("a" << 1 << "b" << 3 << "c" << 3 << "j" << 5 << "k" << 10)));
}

TEST_F(ExprMatchTest,
       OrWithAndContainingMatchRewritableAndNonMatchRewritableChildMatchesCorrectly) {
    createMatcher(fromjson(
        "{$expr: {$or: [{$eq: ['$x', 3]}, {$and: [{$eq: ['$y', 4]}, {$eq: ['$y', '$z']}]}]}}"));

    ASSERT_TRUE(matches(BSON("x" << 3)));
    ASSERT_TRUE(matches(BSON("y" << 4 << "z" << 4)));

    ASSERT_FALSE(matches(BSON("x" << 4)));
    ASSERT_FALSE(matches(BSON("y" << 4 << "z" << 5)));
}

TEST_F(ExprMatchTest, InitialCollationUsedForComparisons) {
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    setCollator(std::move(collator));
    createMatcher(fromjson("{$expr: {$eq: ['$x', 'abc']}}"));

    ASSERT_TRUE(matches(BSON("x"
                             << "AbC")));

    ASSERT_FALSE(matches(BSON("x"
                              << "cba")));
}

TEST_F(ExprMatchTest, SetCollatorChangesCollationUsedForComparisons) {
    createMatcher(fromjson("{$expr: {$eq: ['$x', 'abc']}}"));

    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    setCollator(std::move(collator));

    ASSERT_TRUE(matches(BSON("x"
                             << "AbC")));

    ASSERT_FALSE(matches(BSON("x"
                              << "cba")));
}

TEST_F(ExprMatchTest, FailGracefullyOnInvalidExpression) {
    ASSERT_THROWS_CODE(createMatcher(fromjson("{$expr: {$anyElementTrue: undefined}}")),
                       AssertionException,
                       17041);
    ASSERT_THROWS_CODE(
        createMatcher(fromjson("{$and: [{x: 1},{$expr: {$anyElementTrue: undefined}}]}")),
        AssertionException,
        17041);
    ASSERT_THROWS_CODE(
        createMatcher(fromjson("{$or: [{x: 1},{$expr: {$anyElementTrue: undefined}}]}")),
        AssertionException,
        17041);
    ASSERT_THROWS_CODE(
        createMatcher(fromjson("{$nor: [{x: 1},{$expr: {$anyElementTrue: undefined}}]}")),
        AssertionException,
        17041);
}

TEST_F(ExprMatchTest, ReturnsFalseInsteadOfErrorWithFailpointSet) {
    createMatcher(fromjson("{$expr: {$divide: [10, '$divisor']}}"));
    ASSERT_THROWS_CODE(matches(BSON("divisor" << 0)), AssertionException, ErrorCodes::BadValue);

    FailPointEnableBlock scopedFailpoint("ExprMatchExpressionMatchesReturnsFalseOnException");
    createMatcher(fromjson("{$expr: {$divide: [10, '$divisor']}}"));
    ASSERT_FALSE(matches(BSON("divisor" << 0)));
}

TEST(ExprMatchTest, IdenticalPostOptimizedExpressionsAreEquivalent) {
    BSONObj expression = BSON("$expr" << BSON("$multiply" << BSON_ARRAY(2 << 2)));
    BSONObj expressionEquiv = BSON("$expr" << BSON("$const" << 4));
    BSONObj expressionNotEquiv = BSON("$expr" << BSON("$const" << 10));

    // Create and optimize an ExprMatchExpression.
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::unique_ptr<MatchExpression> matchExpr =
        std::make_unique<ExprMatchExpression>(expression.firstElement(), expCtx);
    matchExpr = MatchExpression::optimize(std::move(matchExpr));

    // We expect that the optimized 'matchExpr' is still an ExprMatchExpression.
    std::unique_ptr<ExprMatchExpression> pipelineExpr(
        dynamic_cast<ExprMatchExpression*>(matchExpr.release()));
    ASSERT_TRUE(pipelineExpr);

    ASSERT_TRUE(pipelineExpr->equivalent(pipelineExpr.get()));

    ExprMatchExpression pipelineExprEquiv(expressionEquiv.firstElement(), expCtx);
    ASSERT_TRUE(pipelineExpr->equivalent(&pipelineExprEquiv));

    ExprMatchExpression pipelineExprNotEquiv(expressionNotEquiv.firstElement(), expCtx);
    ASSERT_FALSE(pipelineExpr->equivalent(&pipelineExprNotEquiv));
}

TEST(ExprMatchTest, ExpressionOptimizeRewritesVariableDereferenceAsConstant) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto varId = expCtx->variablesParseState.defineVariable("var");
    expCtx->variables.setConstantValue(varId, Value(4));

    BSONObj expression = BSON("$expr"
                              << "$$var");
    BSONObj expressionEquiv = BSON("$expr" << BSON("$const" << 4));
    BSONObj expressionNotEquiv = BSON("$expr" << BSON("$const" << 10));

    // Create and optimize an ExprMatchExpression.
    std::unique_ptr<MatchExpression> matchExpr =
        std::make_unique<ExprMatchExpression>(expression.firstElement(), expCtx);
    matchExpr = MatchExpression::optimize(std::move(matchExpr));

    // We expect that the optimized 'matchExpr' is still an ExprMatchExpression.
    auto& pipelineExpr = dynamic_cast<ExprMatchExpression&>(*matchExpr);
    ASSERT_TRUE(pipelineExpr.equivalent(&pipelineExpr));

    ExprMatchExpression pipelineExprEquiv(expressionEquiv.firstElement(), expCtx);
    ASSERT_TRUE(pipelineExpr.equivalent(&pipelineExprEquiv));

    ExprMatchExpression pipelineExprNotEquiv(expressionNotEquiv.firstElement(), expCtx);
    ASSERT_FALSE(pipelineExpr.equivalent(&pipelineExprNotEquiv));
}

TEST(ExprMatchTest, OptimizingIsANoopWhenAlreadyOptimized) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expression = fromjson("{$expr: {$eq: ['$a', 4]}}");

    // Create and optimize an ExprMatchExpression.
    std::unique_ptr<MatchExpression> singlyOptimized =
        std::make_unique<ExprMatchExpression>(expression.firstElement(), expCtx);
    singlyOptimized = MatchExpression::optimize(std::move(singlyOptimized));

    // We expect that the optimized 'matchExpr' is now an $and.
    ASSERT(dynamic_cast<const AndMatchExpression*>(singlyOptimized.get()));

    // We expect the twice-optimized match expression to be equivalent to the once-optimized one.
    std::unique_ptr<MatchExpression> doublyOptimized =
        std::make_unique<ExprMatchExpression>(expression.firstElement(), expCtx);
    for (size_t i = 0; i < 2u; ++i) {
        doublyOptimized = MatchExpression::optimize(std::move(doublyOptimized));
    }
    ASSERT_TRUE(doublyOptimized->equivalent(singlyOptimized.get()));
}

TEST(ExprMatchTest, OptimizingAnAlreadyOptimizedCloneIsANoop) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expression = fromjson("{$expr: {$eq: ['$a', 4]}}");

    // Create and optimize an ExprMatchExpression.
    std::unique_ptr<MatchExpression> singlyOptimized =
        std::make_unique<ExprMatchExpression>(expression.firstElement(), expCtx);
    singlyOptimized = MatchExpression::optimize(std::move(singlyOptimized));

    // We expect that the optimized 'matchExpr' is now an $and.
    ASSERT(dynamic_cast<const AndMatchExpression*>(singlyOptimized.get()));

    // Clone the match expression and optimize it again. We expect the twice-optimized match
    // expression to be equivalent to the once-optimized one.
    std::unique_ptr<MatchExpression> doublyOptimized = singlyOptimized->shallowClone();
    doublyOptimized = MatchExpression::optimize(std::move(doublyOptimized));
    ASSERT_TRUE(doublyOptimized->equivalent(singlyOptimized.get()));
}

TEST(ExprMatchTest, ShallowClonedExpressionIsEquivalentToOriginal) {
    BSONObj expression = BSON("$expr" << BSON("$eq" << BSON_ARRAY("$a" << 5)));

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ExprMatchExpression pipelineExpr(expression.firstElement(), std::move(expCtx));
    auto shallowClone = pipelineExpr.shallowClone();
    ASSERT_TRUE(pipelineExpr.equivalent(shallowClone.get()));
}

TEST(ExprMatchTest, OptimizingExprAbsorbsAndOfAnd) {
    BSONObj exprBson = fromjson("{$expr: {$and: [{$eq: ['$a', 1]}, {$eq: ['$b', 2]}]}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matchExpr =
        std::make_unique<ExprMatchExpression>(exprBson.firstElement(), std::move(expCtx));
    auto optimized = MatchExpression::optimize(std::move(matchExpr));

    // The optimized match expression should not have and AND children of AND nodes. This should be
    // collapsed during optimization.
    BSONObj serialized;
    {
        BSONObjBuilder builder;
        optimized->serialize(&builder, true);
        serialized = builder.obj();
    }

    BSONObj expectedSerialization = fromjson(
        "{$and: [{$expr: {$and: [{$eq: ['$a', {$const: 1}]}, {$eq: ['$b', {$const: 2}]}]}},"
        "{a: {$_internalExprEq: 1}}, {b: {$_internalExprEq: 2}}]}");
    ASSERT_BSONOBJ_EQ(serialized, expectedSerialization);
}

TEST_F(ExprMatchTest, ExpressionEvaluationReturnsResultsCorrectly) {
    createMatcher(fromjson("{$expr: -2}"));
    BSONMatchableDocument document{BSONObj{}};
    auto expressionResult = getExprMatchExpression()->evaluateExpression(&document);
    ASSERT_TRUE(expressionResult.integral());
    ASSERT_EQUALS(-2, expressionResult.coerceToInt());
}

DEATH_TEST_REGEX(ExprMatchTest, GetChildFailsIndexGreaterThanZero, "Tripwire assertion.*6400207") {
    BSONObj exprBson = fromjson("{$expr: {$and: [{$eq: ['$a', 1]}, {$eq: ['$b', 2]}]}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matchExpr =
        std::make_unique<ExprMatchExpression>(exprBson.firstElement(), std::move(expCtx));

    ASSERT_EQ(matchExpr->numChildren(), 0);
    ASSERT_THROWS_CODE(matchExpr->getChild(0), AssertionException, 6400207);
}

}  // namespace
}  // namespace mongo

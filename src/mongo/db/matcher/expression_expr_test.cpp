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

#include "mongo/db/matcher/expression_expr.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <functional>
#include <limits>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {


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
        _matchExpression = optimizeMatchExpression(std::move(_matchExpression));
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

    MatchExpression* getMatchExpression() {
        return _matchExpression.get();
    }

    ExprMatchExpression* getExprMatchExpression() {
        return checked_cast<ExprMatchExpression*>(_matchExpression.get());
    }

    BSONObj serialize(const SerializationOptions& opts) {
        return _matchExpression->serialize(opts);
    }

private:
    const boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
    std::unique_ptr<MatchExpression> _matchExpression;
};

TEST_F(ExprMatchTest, ComparisonThrowsWithUnboundVariable) {
    ASSERT_THROWS(createMatcher(BSON("$expr" << BSON("$eq" << BSON_ARRAY("$a" << "$$var")))),
                  DBException);
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

TEST_F(ExprMatchTest, IdenticalPostOptimizedExpressionsAreEquivalent) {
    BSONObj expression =
        BSON("$expr" << BSON("$ifNull" << BSON_ARRAY("$NO_SUCH_FIELD"
                                                     << BSON("$multiply" << BSON_ARRAY(2 << 2)))));
    BSONObj expressionEquiv =
        BSON("$expr" << BSON("$ifNull" << BSON_ARRAY("$NO_SUCH_FIELD" << BSON("$const" << 4))));
    BSONObj expressionNotEquiv =
        BSON("$expr" << BSON("$ifNull" << BSON_ARRAY("$NO_SUCH_FIELD" << BSON("$const" << 10))));

    // Create and optimize an ExprMatchExpression.
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::unique_ptr<MatchExpression> matchExpr =
        std::make_unique<ExprMatchExpression>(expression.firstElement(), expCtx);
    matchExpr = optimizeMatchExpression(std::move(matchExpr));

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

TEST(SimpleExprMatchTest, ExpressionOptimizeRewritesVariableDereferenceAsConstant) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto varId = expCtx->variablesParseState.defineVariable("var");
    expCtx->variables.setConstantValue(varId, Value(4));
    BSONObj expression =
        BSON("$expr" << BSON("$ifNull" << BSON_ARRAY("$NO_SUCH_FIELD" << "$$var")));
    BSONObj expressionEquiv =
        BSON("$expr" << BSON("$ifNull" << BSON_ARRAY("$NO_SUCH_FIELD" << BSON("$const" << 4))));
    BSONObj expressionNotEquiv =
        BSON("$expr" << BSON("$ifNull" << BSON_ARRAY("$NO_SUCH_FIELD" << BSON("$const" << 10))));

    // Create and optimize an ExprMatchExpression.
    std::unique_ptr<MatchExpression> matchExpr =
        std::make_unique<ExprMatchExpression>(expression.firstElement(), expCtx);
    matchExpr = optimizeMatchExpression(std::move(matchExpr));

    // We expect that the optimized 'matchExpr' is still an ExprMatchExpression.
    auto& pipelineExpr = dynamic_cast<ExprMatchExpression&>(*matchExpr);
    ASSERT_TRUE(pipelineExpr.equivalent(&pipelineExpr));

    ExprMatchExpression pipelineExprEquiv(expressionEquiv.firstElement(), expCtx);
    ASSERT_TRUE(pipelineExpr.equivalent(&pipelineExprEquiv));

    ExprMatchExpression pipelineExprNotEquiv(expressionNotEquiv.firstElement(), expCtx);
    ASSERT_FALSE(pipelineExpr.equivalent(&pipelineExprNotEquiv));
}

TEST(SimpleExprMatchTest, OptimizingIsANoopWhenAlreadyOptimized) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expression = fromjson("{$expr: {$eq: ['$a', 4]}}");

    // Create and optimize an ExprMatchExpression.
    std::unique_ptr<MatchExpression> singlyOptimized =
        std::make_unique<ExprMatchExpression>(expression.firstElement(), expCtx);
    singlyOptimized = optimizeMatchExpression(std::move(singlyOptimized));

    // We expect that the optimized 'matchExpr' is now an $and.
    ASSERT(dynamic_cast<const AndMatchExpression*>(singlyOptimized.get()));

    // We expect the twice-optimized match expression to be equivalent to the once-optimized one.
    std::unique_ptr<MatchExpression> doublyOptimized =
        std::make_unique<ExprMatchExpression>(expression.firstElement(), expCtx);
    for (size_t i = 0; i < 2u; ++i) {
        doublyOptimized = optimizeMatchExpression(std::move(doublyOptimized));
    }
    ASSERT_TRUE(doublyOptimized->equivalent(singlyOptimized.get()));
}

TEST(SimpleExprMatchTest, OptimizingAnAlreadyOptimizedCloneIsANoop) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expression = fromjson("{$expr: {$eq: ['$a', 4]}}");

    // Create and optimize an ExprMatchExpression.
    std::unique_ptr<MatchExpression> singlyOptimized =
        std::make_unique<ExprMatchExpression>(expression.firstElement(), expCtx);
    singlyOptimized = optimizeMatchExpression(std::move(singlyOptimized));

    // We expect that the optimized 'matchExpr' is now an $and.
    ASSERT(dynamic_cast<const AndMatchExpression*>(singlyOptimized.get()));

    // Clone the match expression and optimize it again. We expect the twice-optimized match
    // expression to be equivalent to the once-optimized one.
    std::unique_ptr<MatchExpression> doublyOptimized = singlyOptimized->clone();
    doublyOptimized = optimizeMatchExpression(std::move(doublyOptimized));
    ASSERT_TRUE(doublyOptimized->equivalent(singlyOptimized.get()));
}

TEST(SimpleExprMatchTest, ShallowClonedExpressionIsEquivalentToOriginal) {
    BSONObj expression = BSON("$expr" << BSON("$eq" << BSON_ARRAY("$a" << 5)));

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ExprMatchExpression pipelineExpr(expression.firstElement(), std::move(expCtx));
    auto clone = pipelineExpr.clone();
    ASSERT_TRUE(pipelineExpr.equivalent(clone.get()));
}

TEST(SimpleExprMatchTest, OptimizingExprAbsorbsAndOfAnd) {
    BSONObj exprBson = fromjson("{$expr: {$and: [{$eq: ['$a', 1]}, {$eq: ['$b', 2]}]}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matchExpr =
        std::make_unique<ExprMatchExpression>(exprBson.firstElement(), std::move(expCtx));
    auto optimized = optimizeMatchExpression(std::move(matchExpr));

    // The optimized match expression should not have and AND children of AND nodes. This should be
    // collapsed during optimization.
    BSONObj expectedSerialization = fromjson(
        "{$and: [{$expr: {$and: [{$eq: ['$a', {$const: 1}]}, {$eq: ['$b', {$const: 2}]}]}},"
        "{a: {$_internalExprEq: 1}}, {b: {$_internalExprEq: 2}}]}");
    ASSERT_BSONOBJ_EQ(optimized->serialize(), expectedSerialization);
}

TEST(SimpleExprMatchTest, OptimizingExprRemovesTrueConstantExpression) {
    auto exprBson = fromjson("{$expr: true}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto matchExpr =
        std::make_unique<ExprMatchExpression>(exprBson.firstElement(), std::move(expCtx));
    auto optimized = optimizeMatchExpression(std::move(matchExpr));

    auto serialization = optimized->serialize();
    auto expectedSerialization = fromjson("{}");
    ASSERT_BSONOBJ_EQ(serialization, expectedSerialization);
}

TEST(SimpleExprMatchTest, OptimizingExprRemovesTruthyConstantExpression) {
    auto exprBson = fromjson("{$expr: {$concat: ['a', 'b', 'c']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto matchExpr =
        std::make_unique<ExprMatchExpression>(exprBson.firstElement(), std::move(expCtx));
    auto optimized = optimizeMatchExpression(std::move(matchExpr));

    auto serialization = optimized->serialize();
    auto expectedSerialization = fromjson("{}");
    ASSERT_BSONOBJ_EQ(serialization, expectedSerialization);
}

TEST_F(ExprMatchTest, ExprWithTrueConstantExpressionIsTriviallyTrue) {
    createMatcher(fromjson("{$expr: true}"));
    ASSERT_TRUE(getMatchExpression()->isTriviallyTrue());
}

TEST_F(ExprMatchTest, ExprWithTruthyConstantExpressionIsTriviallyTrue) {
    createMatcher(fromjson("{$expr: {$concat: ['a', 'b', 'c']}}"));
    ASSERT_TRUE(getMatchExpression()->isTriviallyTrue());
}

TEST_F(ExprMatchTest, ExprWithNonConstantExpressionIsNotTriviallyTrue) {
    createMatcher(fromjson("{$expr: {$concat: ['$a', '$b', '$c']}}"));
    ASSERT_FALSE(getMatchExpression()->isTriviallyTrue());
}

TEST_F(ExprMatchTest, ExprWithFalsyConstantExpressionIsNotTriviallyTrue) {
    createMatcher(fromjson("{$expr: {$sum: [1, -1]}}"));
    ASSERT_FALSE(getMatchExpression()->isTriviallyTrue());
}

TEST_F(ExprMatchTest, ExprWithFalseConstantExpressionIsTriviallyFalse) {
    createMatcher(fromjson("{$expr: false}"));
    ASSERT_TRUE(getMatchExpression()->isTriviallyFalse());
}

TEST_F(ExprMatchTest, ExprThatOptimizesToFalseIsTriviallyFalse) {
    createMatcher(fromjson("{$expr:{$eq:['a','b']}}"));
    ASSERT_TRUE(getMatchExpression()->isTriviallyFalse());
}

TEST_F(ExprMatchTest, AndWithFalseConstantExpressionIsTriviallyFalse) {
    createMatcher(fromjson("{$and:[ {$expr:false}, {_id:15} ]}"));
    ASSERT_TRUE(getMatchExpression()->isTriviallyFalse());
}

TEST_F(ExprMatchTest, OrWithFalseConstantExpressionIsNotTriviallyFalse) {
    createMatcher(fromjson("{$or:[ {$expr:false}, {_id:20} ]}"));
    ASSERT_FALSE(getMatchExpression()->isTriviallyFalse());
}

TEST_F(ExprMatchTest, ExprWithNonConstantExpressionIsNotTriviallyFalse) {
    createMatcher(fromjson("{$expr: {$sum: [\"$a\", \"$b\"]}}"));
    ASSERT_FALSE(getMatchExpression()->isTriviallyFalse());
}

TEST_F(ExprMatchTest, ExprWithTruthyConstantExpressionsIsNotTriviallyFalse) {
    createMatcher(fromjson("{$expr: {$sum: [1, 1, 1]}}"));
    ASSERT_FALSE(getMatchExpression()->isTriviallyFalse());
}

DEATH_TEST_REGEX(ExprMatchDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400207") {
    BSONObj exprBson = fromjson("{$expr: {$and: [{$eq: ['$a', 1]}, {$eq: ['$b', 2]}]}}");

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matchExpr =
        std::make_unique<ExprMatchExpression>(exprBson.firstElement(), std::move(expCtx));

    ASSERT_EQ(matchExpr->numChildren(), 0);
    ASSERT_THROWS_CODE(matchExpr->getChild(0), AssertionException, 6400207);
}

TEST_F(ExprMatchTest, ExprRedactsCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    createMatcher(fromjson("{$expr: {$sum: [\"$a\", \"$b\"]}}"));

    SerializationOptions opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$expr":{"$sum":["$HASH<a>","$HASH<b>"]}})",
        serialize(opts));

    createMatcher(fromjson("{$expr: {$sum: [\"$a\", \"b\"]}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$expr":{"$sum":["$HASH<a>","?string"]}})",
        serialize(opts));

    createMatcher(fromjson("{$expr: {$sum: [\"$a.b\", \"$b\"]}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$expr":{"$sum":["$HASH<a>.HASH<b>","$HASH<b>"]}})",
        serialize(opts));

    createMatcher(fromjson("{$expr: {$eq: [\"$a\", \"$$NOW\"]}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "HASH<a>": {
                        "$_internalExprEq": "?date"
                    }
                },
                {
                    "$expr": {
                        "$eq": [
                            "$HASH<a>",
                            "?date"
                        ]
                    }
                }
            ]
        })",
        serialize(opts));

    createMatcher(fromjson("{$expr: {$eq: [\"$a\", \"$$NOW\"]}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "HASH<a>": {
                        "$_internalExprEq": "?date"
                    }
                },
                {
                    "$expr": {
                        "$eq": [
                            "$HASH<a>",
                            "?date"
                        ]
                    }
                }
            ]
        })",
        serialize(opts));

    createMatcher(fromjson("{$expr: {$getField: {field: \"b\", input: {a: 1, b: 2}}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$expr":{"$getField":{"field":"HASH<b>","input":"?object"}}})",
        serialize(opts));

    createMatcher(fromjson("{$expr: {$getField: {field: \"$b\", input: {a: 1, b: 2}}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$expr":{"$getField":{"field":"$HASH<b>","input":"?object"}}})",
        serialize(opts));

    createMatcher(fromjson("{$expr: {$getField: {field: {$const: \"$b\"}, input: {a: 1, b: 2}}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$expr":{"$getField":{"field":{$const:"HASH<$b>"},"input":"?object"}}})",
        serialize(opts));

    createMatcher(fromjson("{$expr: {$getField: {field: \"b\", input: \"$a\"}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$expr":{"$getField":{"field":"HASH<b>","input":"$HASH<a>"}}})",
        serialize(opts));

    createMatcher(fromjson("{$expr: {$getField: {field: \"b\", input: {a: 1, b: \"$c\"}}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$expr": {
                "$getField": {
                    "field": "HASH<b>",
                    "input": {
                        "HASH<a>": "?number",
                        "HASH<b>": "$HASH<c>"
                    }
                }
            }
        })",
        serialize(opts));

    createMatcher(fromjson("{$expr: {$getField: {field: \"b.c\", input: {a: 1, b: \"$c\"}}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$expr": {
                "$getField": {
                    "field": "HASH<b>.HASH<c>",
                    "input": {
                        "HASH<a>": "?number",
                        "HASH<b>": "$HASH<c>"
                    }
                }
            }
        })",
        serialize(opts));

    createMatcher(
        fromjson("{$expr: {$setField: {field: \"b\", input: {a: 1, b: \"$c\"}, value: 5}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$expr": {
                "$setField": {
                    "field": "HASH<b>",
                    "input": {
                        "HASH<a>": "?number",
                        "HASH<b>": "$HASH<c>"
                    },
                    "value": "?number"
                }
            }
        })",
        serialize(opts));

    createMatcher(fromjson(
        "{$expr: {$setField: {field: \"b.c\", input: {a: 1, b: \"$c\"}, value: \"$d\"}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$expr": {
                "$setField": {
                    "field": "HASH<b>.HASH<c>",
                    "input": {
                        "HASH<a>": "?number",
                        "HASH<b>": "$HASH<c>"
                    },
                    "value": "$HASH<d>"
                }
            }
        })",
        serialize(opts));

    createMatcher(fromjson(
        "{$expr: {$setField: {field: \"b.c\", input: {a: 1, b: \"$c\"}, value: \"$d.e\"}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$expr": {
                "$setField": {
                    "field": "HASH<b>.HASH<c>",
                    "input": {
                        "HASH<a>": "?number",
                        "HASH<b>": "$HASH<c>"
                    },
                    "value": "$HASH<d>.HASH<e>"
                }
            }
        })",
        serialize(opts));

    createMatcher(
        fromjson("{$expr: {$setField: {field: \"b\", input: {a: 1, b: \"$c\"}, value: {a: 1, b: 2, "
                 "c: 3}}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$expr": {
                "$setField": {
                    "field": "HASH<b>",
                    "input": {
                        "HASH<a>": "?number",
                        "HASH<b>": "$HASH<c>"
                    },
                    "value": "?object"
                }
            }
        })",
        serialize(opts));

    createMatcher(
        fromjson("{$expr: {$setField: {field: \"b\", input: {a: 1, b: \"$c\"}, value: {a: 1, b: 2, "
                 "c: \"$d\"}}}}"));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$expr": {
                "$setField": {
                    "field": "HASH<b>",
                    "input": {
                        "HASH<a>": "?number",
                        "HASH<b>": "$HASH<c>"
                    },
                    "value": {
                        "HASH<a>": "?number",
                        "HASH<b>": "?number",
                        "HASH<c>": "$HASH<d>"
                    }
                }
            }
        })",
        serialize(opts));
}
}  // namespace
}  // namespace mongo

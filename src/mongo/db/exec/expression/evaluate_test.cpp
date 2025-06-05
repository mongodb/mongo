/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/name_expression.h"
#include "mongo/unittest/unittest.h"

#include <climits>
#include <cmath>
#include <limits>

namespace mongo {
namespace expression_evaluation_test {

using boost::intrusive_ptr;

/* ------------------------ Constant -------------------- */

TEST(ExpressionConstantTest, Create) {
    /** Create an ExpressionConstant from a Value. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value(5));
    ASSERT_BSONOBJ_BINARY_EQ(BSON("" << 5), toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionConstantTest, CreateFromBsonElement) {
    /** Create an ExpressionConstant from a BsonElement. */
    BSONObj spec = BSON("IGNORED_FIELD_NAME" << "foo");
    auto expCtx = ExpressionContextForTest{};
    BSONElement specElement = spec.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    intrusive_ptr<Expression> expression = ExpressionConstant::parse(&expCtx, specElement, vps);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("" << "foo"),
                             toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionConstantTest, ConstantOfValueMissingRemovesField) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value());
    ASSERT_BSONOBJ_BINARY_EQ(
        BSONObj(),
        toBson(expression->evaluate(Document{{"foo", Value("bar"_sd)}}, &expCtx.variables)));
}

namespace BuiltinRemoveVariable {

TEST(BuiltinRemoveVariableTest, TypeOfRemoveIsMissing) {
    assertExpectedResults("$type", {{{Value("$$REMOVE"_sd)}, Value("missing"_sd)}});
}

TEST(BuiltinRemoveVariableTest, LiteralEscapesRemoveVar) {
    assertExpectedResults(
        "$literal", {{{Value("$$REMOVE"_sd)}, Value(std::vector<Value>{Value("$$REMOVE"_sd)})}});
}

}  // namespace BuiltinRemoveVariable

namespace NowAndClusterTime {
TEST(NowAndClusterTime, BasicTest) {
    auto expCtx = ExpressionContextForTest{};

    // $$NOW is the Date type.
    {
        auto expression = ExpressionFieldPath::parse(&expCtx, "$$NOW", expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables);
        ASSERT_EQ(result.getType(), BSONType::date);
    }
    // $$CLUSTER_TIME is the timestamp type.
    {
        auto expression =
            ExpressionFieldPath::parse(&expCtx, "$$CLUSTER_TIME", expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables);
        ASSERT_EQ(result.getType(), BSONType::timestamp);
    }

    // Multiple references to $$NOW must return the same value.
    {
        auto expression = Expression::parseExpression(
            &expCtx, fromjson("{$eq: [\"$$NOW\", \"$$NOW\"]}"), expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables);

        ASSERT_VALUE_EQ(result, Value{true});
    }
    // Same is true for the $$CLUSTER_TIME.
    {
        auto expression =
            Expression::parseExpression(&expCtx,
                                        fromjson("{$eq: [\"$$CLUSTER_TIME\", \"$$CLUSTER_TIME\"]}"),
                                        expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables);

        ASSERT_VALUE_EQ(result, Value{true});
    }
}
}  // namespace NowAndClusterTime

TEST(NameExpression, Literal) {
    auto expCtx = ExpressionContextForTest{};
    auto nameExprObj = fromjson(R"({db: "abc"})");
    auto nameExpr = NameExpression::parseFromBSON(nameExprObj["db"]);
    ASSERT_TRUE(nameExpr.isLiteral());
    ASSERT_EQ("abc", nameExpr.getLiteral());

    auto serializedStr = nameExpr.toString();
    ASSERT_EQ(nameExprObj.toString(), serializedStr);
}

TEST(NameExpression, SimplePath) {
    auto expCtx = ExpressionContextForTest{};
    auto nameExprObj = fromjson(R"({coll: "$apath"})");
    auto nameExpr = NameExpression::parseFromBSON(nameExprObj["coll"]);
    ASSERT_FALSE(nameExpr.isLiteral());
    ASSERT_EQ("ljk", nameExpr.evaluate(&expCtx, fromJson(R"({apath: "ljk"})")));

    auto serializedStr = nameExpr.toString();
    ASSERT_EQ(nameExprObj.toString(), serializedStr);
}

TEST(NameExpression, Expression) {
    auto expCtx = ExpressionContextForTest{};
    auto nameExprObj =
        fromjson(R"({fullName: {$concat: ["$customer.firstname", " ", "$customer.surname"]}})");
    auto nameExpr = NameExpression::parseFromBSON(nameExprObj["fullName"]);
    ASSERT_FALSE(nameExpr.isLiteral());
    ASSERT_EQ("Firstname Lastname", nameExpr.evaluate(&expCtx, fromJson(R"(
                                    {
                                        customer: {
                                            firstname: "Firstname",
                                            surname: "Lastname"
                                        }
                                    }
                                )")));

    auto serializedStr = nameExpr.toString();
    ASSERT_EQ(nameExprObj.toString(), serializedStr);
}

TEST(NameExpression, NonStringValue) {
    auto expCtx = ExpressionContextForTest{};
    auto nameExprObj = fromjson(R"({fullName: {$add: ["$customer.id", 10]}})");
    auto nameExpr = NameExpression::parseFromBSON(nameExprObj["fullName"]);
    ASSERT_FALSE(nameExpr.isLiteral());
    ASSERT_THROWS_CODE(
        nameExpr.evaluate(&expCtx, fromJson(R"({customer: {id: 10}})")), DBException, 8117101);
}

TEST(NameExpression, InvalidInput) {
    auto expCtx = ExpressionContextForTest{};
    auto nameExprObj =
        fromjson(R"({fullName: {$concat: ["$customer.firstname", " ", "$customer.surname"]}})");
    auto nameExpr = NameExpression::parseFromBSON(nameExprObj["fullName"]);
    ASSERT_FALSE(nameExpr.isLiteral());
    ASSERT_THROWS_CODE(
        nameExpr.evaluate(&expCtx, fromJson(R"({customer: {id: 10}})")), DBException, 8117101);
}

}  // namespace expression_evaluation_test
}  // namespace mongo

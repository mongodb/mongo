// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <functional>
#include <string>


namespace mongo {
namespace ExpressionTests {
namespace {

TEST(RedactionTest, ExpressionLet) {
    query_shape::SerializationOptions options =
        query_shape::SerializationOptions::kMarkIdentifiers_FOR_TEST;

    auto expCtx = ExpressionContextForTest{};

    auto expression = Expression::parseExpression(&expCtx,
                                                  fromjson(R"(
        {$let: {vars: {foo: 35}, in: {$gt: ["$$foo", 23]}}}
    )"),
                                                  expCtx.variablesParseState);
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({
            "$let": {
                "vars": {
                    "HASH<foo>": {
                        "$const": 35
                    }
                },
                "in": {
                    "$gt": [
                        "$$HASH<foo>",
                        {
                            "$const": 23
                        }
                    ]
                }
            }
        })",
        expression->serialize(options).getDocument());

    expression = Expression::parseExpression(&expCtx,
                                             fromjson(R"(
        {$let: {vars: {foo: 35, myNow: "$$NOW", obj: {hello: 23}}, in: {$gt: ["$$myNow", "$$obj.hello"]}}}
    )"),
                                             expCtx.variablesParseState);

    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({
            "$let": {
                "vars": {
                    "HASH<foo>": {
                        "$const": 35
                    },
                    "HASH<myNow>": "$$NOW",
                    "HASH<obj>": {
                        "HASH<hello>": {
                            "$const": 23
                        }
                    }
                },
                "in": {
                    "$gt": [
                        "$$HASH<myNow>",
                        "$$HASH<obj>.HASH<hello>"
                    ]
                }
            }
        })",
        expression->serialize(options).getDocument());
}

TEST(ExpressionLetOptimizeTest, InlineConstants) {
    auto expCtx = ExpressionContextForTest{};

    auto expression = Expression::parseExpression(&expCtx,
                                                  fromjson(R"(
        {$let: {
            vars: {
                variable: {$split: ["$data", "\n"]},
                constant: 3,
                constantObject: {four: 4}
            },
            in: {$and: [
                {$gte: ["$a", "$$constant"]},
                {$gte: ["$b", "$$constantObject.four"]},
                {$eq: ["$firstLine", {$first: "$$variable"}]},
                {$eq: ["$secondLine", {$last: "$$variable"}]}
            ]}
        }}
    )"),

                                                  expCtx.variablesParseState);
    expression = expression->optimize();
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"(
            {$let: {
                vars: {
                    variable: {$split: ["$data", {$const: "\n"}]}
                },
                in: {$and: [
                    {$gte: ["$a", {$const: 3}]},
                    {$gte: ["$b", {$const: 4}]},
                    {$eq: ["$firstLine", {$first: ["$$variable"]}]},
                    {$eq: ["$secondLine", {$last: ["$$variable"]}]}
                ]}
            }}
        )",
        expression->serialize().getDocument());
}

TEST(ExpressionLetOptimizeTest, RemoveUnusedVariables) {
    auto expCtx = ExpressionContextForTest{};

    auto expression = Expression::parseExpression(&expCtx,
                                                  fromjson(R"(
        {$let: {
            vars: {
                unused: "unused",
                variable: {$split: ["$data", "\n"]}
            },
            in: {$and: [
                {$eq: ["$firstLine", {$first: "$$variable"}]},
                {$eq: ["$secondLine", {$last: "$$variable"}]}
            ]}
        }}
    )"),

                                                  expCtx.variablesParseState);
    expression = expression->optimize();
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"(
        {$let: {
            vars: {
                variable: {$split: ["$data", {$const: "\n"}]}
            },
            in: {$and: [
                {$eq: ["$firstLine", {$first: ["$$variable"]}]},
                {$eq: ["$secondLine", {$last: ["$$variable"]}]}
            ]}
        }}

        )",
        expression->serialize().getDocument());
}

TEST(ExpressionLetOptimizeTest, RemoveLetIfAllVariablesAreRemoved) {
    auto expCtx = ExpressionContextForTest{};

    auto expression = Expression::parseExpression(&expCtx,
                                                  fromjson(R"(
        {$let: {
            vars: {
                minConstant: 3,
                maxConstant: 5,
                unused: "unused"
            },
            in: {$and: [
                {$gte: ["$a", "$$minConstant"]},
                {$lte: ["$a", "$$maxConstant"]}
            ]}
        }}
    )"),

                                                  expCtx.variablesParseState);
    expression = expression->optimize();
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"(
        {$and: [
                {$gte: ["$a", {$const: 3}]},
                {$lte: ["$a", {$const: 5}]}
            ]}
        )",
        expression->serialize().getDocument());
}

void buildRecursiveLet(BSONObjBuilder& builder, size_t iterationsLeft) {
    BSONObjBuilder subObjBuilder = builder.subobjStart("$let");

    {
        BSONObjBuilder vars = subObjBuilder.subobjStart("vars");
        vars.append(str::stream() << "unused" << iterationsLeft,
                    str::stream() << "ununsed string " << iterationsLeft);
        vars.appendNumber("iteration", static_cast<long long>(iterationsLeft));
    }

    BSONObjBuilder in = subObjBuilder.subobjStart("in");
    BSONArrayBuilder dollarAnd = in.subarrayStart("$and");
    dollarAnd.append(
        BSON("$eq" << BSON_ARRAY(((str::stream() << "$field" << iterationsLeft).ss.str())
                                 << "$$iteration")));

    if (iterationsLeft > 0) {
        BSONObjBuilder sub(dollarAnd.subobjStart());
        buildRecursiveLet(sub, iterationsLeft - 1);
    }
}

BSONObj buildRecursiveLet(size_t depth, bool optimized) {
    BSONObjBuilder builder;
    buildRecursiveLet(builder, depth);
    return builder.obj();
}

BSONObj buildOptimizedExpression(size_t depth) {
    BSONObjBuilder b;
    BSONArrayBuilder andArray{b.subarrayStart("$and")};
    for (size_t i = 0; i <= depth; ++i) {
        int64_t idx = depth - i;
        std::string fieldName = str::stream() << "$field" << (depth - i);
        andArray.append(BSON("$eq" << BSON_ARRAY(fieldName << BSON("$const" << idx))));
    }
    andArray.done();
    return b.obj();
}

TEST(ExpressionLetOptimizeTest, DoesNotCauseExponentialTraversals) {
    auto expCtx = ExpressionContextForTest{};
    auto expression = Expression::parseExpression(
        &expCtx, buildRecursiveLet(50, false /*optimized*/), expCtx.variablesParseState);
    expression = expression->optimize();
    ASSERT_BSONOBJ_EQ(buildOptimizedExpression(50), expression->serialize().getDocument().toBson());
}

TEST(ExpressionLetOptimizerTest, SupportExpressionsWithNullChildren) {
    auto expCtx = ExpressionContextForTest{};

    auto expression = Expression::parseExpression(&expCtx,
                                                  fromjson(R"(
        {$let: {
            vars: {
                format: "%Y-%m-%d"
            },
            in: {$dateToString: {
                date: "$date",
                format: "$$format"
            }}
        }}
    )"),

                                                  expCtx.variablesParseState);
    expression = expression->optimize();
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"(
        {$dateToString: {
                date: "$date",
                format: {$const: "%Y-%m-%d"}
        }}
        )",
        expression->serialize().getDocument());
}

}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo

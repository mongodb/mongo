/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/dbtests/dbtests.h"

namespace mongo {
namespace ExpressionTests {
namespace {
using boost::intrusive_ptr;

/** Convert Value to a wrapped BSONObj with an empty string field name. */
static BSONObj toBson(const Value& value) {
    BSONObjBuilder bob;
    value.addToBsonObj(&bob, "");
    return bob.obj();
}

/** Convert Document to BSON. */
static BSONObj toBson(const Document& document) {
    return document.toBson();
}

/** Create a Document from a BSONObj. */
Document fromBson(BSONObj obj) {
    return Document(obj);
}

std::string redactFieldNameForTest(StringData s) {
    return str::stream() << "HASH<" << s << ">";
}

namespace FieldPath {

/** The provided field path does not pass validation. */
class Invalid {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        ASSERT_THROWS(ExpressionFieldPath::deprecatedCreate(&expCtx, ""), AssertionException);
    }
};

TEST(FieldPath, NoOptimizationForRootFieldPathWithDottedPath) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<ExpressionFieldPath> expression =
        ExpressionFieldPath::parse(&expCtx, "$$ROOT.x.y", expCtx.variablesParseState);

    // An attempt to optimize returns the Expression itself.
    ASSERT_EQUALS(expression, expression->optimize());
}

TEST(FieldPath, NoOptimizationForCurrentFieldPathWithDottedPath) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<ExpressionFieldPath> expression =
        ExpressionFieldPath::parse(&expCtx, "$$CURRENT.x.y", expCtx.variablesParseState);

    // An attempt to optimize returns the Expression itself.
    ASSERT_EQUALS(expression, expression->optimize());
}

TEST(FieldPath, RemoveOptimizesToMissingValue) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<ExpressionFieldPath> expression =
        ExpressionFieldPath::parse(&expCtx, "$$REMOVE", expCtx.variablesParseState);

    auto optimizedExpr = expression->optimize();

    ASSERT_VALUE_EQ(
        Value(),
        optimizedExpr->evaluate(Document(BSON("x" << BSON("y" << 123))), &expCtx.variables));
}

TEST(FieldPath, NoOptimizationOnNormalPath) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionFieldPath::deprecatedCreate(&expCtx, "a");
    // An attempt to optimize returns the Expression itself.
    ASSERT_EQUALS(expression, expression->optimize());
}

TEST(FieldPath, OptimizeOnVariableWithConstantScalarValue) {
    auto expCtx = ExpressionContextForTest{};
    auto varId = expCtx.variablesParseState.defineVariable("userVar");
    expCtx.variables.setConstantValue(varId, Value(123));

    auto expr = ExpressionFieldPath::parse(&expCtx, "$$userVar", expCtx.variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    ASSERT_TRUE(dynamic_cast<ExpressionConstant*>(optimizedExpr.get()));
}

TEST(FieldPath, OptimizeOnVariableWithConstantArrayValue) {
    auto expCtx = ExpressionContextForTest{};
    auto varId = expCtx.variablesParseState.defineVariable("userVar");
    expCtx.variables.setConstantValue(varId, Value(BSON_ARRAY(1 << 2 << 3)));

    auto expr = ExpressionFieldPath::parse(&expCtx, "$$userVar", expCtx.variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    auto constantExpr = dynamic_cast<ExpressionConstant*>(optimizedExpr.get());
    ASSERT_TRUE(constantExpr);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2 << 3)), constantExpr->getValue());
}

TEST(FieldPath, OptimizeToEmptyArrayOnNumericalPathComponentAndConstantArrayValue) {
    auto expCtx = ExpressionContextForTest{};
    auto varId = expCtx.variablesParseState.defineVariable("userVar");
    expCtx.variables.setConstantValue(varId, Value(BSON_ARRAY(1 << 2 << 3)));

    auto expr = ExpressionFieldPath::parse(&expCtx, "$$userVar.1", expCtx.variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    auto constantExpr = dynamic_cast<ExpressionConstant*>(optimizedExpr.get());
    ASSERT_TRUE(constantExpr);
    ASSERT_VALUE_EQ(Value(BSONArray()), constantExpr->getValue());
}

TEST(FieldPath, OptimizeOnVariableWithConstantValueAndDottedPath) {
    auto expCtx = ExpressionContextForTest{};
    auto varId = expCtx.variablesParseState.defineVariable("userVar");
    expCtx.variables.setConstantValue(varId, Value(Document{{"x", Document{{"y", 123}}}}));

    auto expr = ExpressionFieldPath::parse(&expCtx, "$$userVar.x.y", expCtx.variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    auto constantExpr = dynamic_cast<ExpressionConstant*>(optimizedExpr.get());
    ASSERT_TRUE(constantExpr);
    ASSERT_VALUE_EQ(Value(123), constantExpr->getValue());
}

TEST(FieldPath, NoOptimizationOnVariableWithNoValue) {
    auto expCtx = ExpressionContextForTest{};
    expCtx.variablesParseState.defineVariable("userVar");

    auto expr = ExpressionFieldPath::parse(&expCtx, "$$userVar", expCtx.variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(optimizedExpr.get()));
}

TEST(FieldPath, NoOptimizationOnVariableWithMissingValue) {
    auto expCtx = ExpressionContextForTest{};
    auto varId = expCtx.variablesParseState.defineVariable("userVar");
    expCtx.variables.setValue(varId, Value());

    auto expr = ExpressionFieldPath::parse(&expCtx, "$$userVar", expCtx.variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(optimizedExpr.get()));
}

TEST(FieldPath, NoOptimizationOnCertainVariables) {
    auto expCtx = ExpressionContextForTest{};

    {
        auto expr = ExpressionFieldPath::parse(&expCtx, "$$NOW", expCtx.variablesParseState);
        ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

        auto optimizedExpr = expr->optimize();
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(optimizedExpr.get()));
    }
    {
        auto expr =
            ExpressionFieldPath::parse(&expCtx, "$$CLUSTER_TIME", expCtx.variablesParseState);
        ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

        auto optimizedExpr = expr->optimize();
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(optimizedExpr.get()));
    }
    {
        auto expr = ExpressionFieldPath::parse(&expCtx, "$$USER_ROLES", expCtx.variablesParseState);
        ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

        auto optimizedExpr = expr->optimize();
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(optimizedExpr.get()));
    }
}

TEST(FieldPath, ScalarVariableWithDottedFieldPathOptimizesToConstantMissingValue) {
    auto expCtx = ExpressionContextForTest{};
    auto varId = expCtx.variablesParseState.defineVariable("userVar");
    expCtx.variables.setConstantValue(varId, Value(123));

    auto expr = ExpressionFieldPath::parse(&expCtx, "$$userVar.x.y", expCtx.variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    auto constantExpr = dynamic_cast<ExpressionConstant*>(optimizedExpr.get());
    ASSERT_TRUE(constantExpr);
    ASSERT_VALUE_EQ(Value(), constantExpr->getValue());
}

TEST(FieldPath, SerializeWithRedaction) {
    SerializationOptions options;
    options.identifierRedactionPolicy = redactFieldNameForTest;
    options.redactIdentifiers = true;

    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "bar", expCtx.variablesParseState);
    ASSERT_VALUE_EQ_AUTO(  // NOLINT
        "\"$HASH<bar>\"",
        expression->serialize(options));

    // Repeat with a dotted path.
    expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b.c", expCtx.variablesParseState);
    ASSERT_VALUE_EQ_AUTO(  // NOLINT
        "\"$HASH<a>.HASH<b>.HASH<c>\"",
        expression->serialize(options));

    auto expr = [&](const std::string& json) {
        return Expression::parseExpression(&expCtx, fromjson(json), expCtx.variablesParseState);
    };

    // Expression with multiple field paths.
    expression = expr(R"({$and: [{$gt: ["$foo", 5]}, {$lt: ["$foo", 10]}]})");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({
            "$and": [
                {
                    "$gt": [
                        "$HASH<foo>",
                        {
                            "$const": 5
                        }
                    ]
                },
                {
                    "$lt": [
                        "$HASH<foo>",
                        {
                            "$const": 10
                        }
                    ]
                }
            ]
        })",
        expression->serialize(options).getDocument());

    // Test that a variable followed by user fields is properly hashed.
    std::string replacementChar = "?";
    options.replacementForLiteralArgs = replacementChar;

    expression = expr(R"({$gt: ["$$ROOT.a.b", 5]})");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"$gt":["$$ROOT.HASH<a>.HASH<b>",{"$const":"?"}]})",
        expression->serialize(options).getDocument());

    expression = expr(R"({$gt: ["$foo", "$$NOW"]})");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"$gt":["$HASH<foo>","$$NOW"]})",
        expression->serialize(options).getDocument());

    // Repeat the above test with a dotted path.
    expression = expr(R"({$gt: ["$foo.a.b", "$$NOW"]})");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"$gt":["$HASH<foo>.HASH<a>.HASH<b>","$$NOW"]})",
        expression->serialize(options).getDocument());
}

/** The field path itself is a dependency. */
class Dependencies {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        DepsTracker dependencies;
        expression::addDependencies(expression.get(), &dependencies);
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(1U, dependencies.fields.count("a.b"));
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedsAnyMetadata());
    }
};

/** Field path target field is missing. */
class Missing {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression = ExpressionFieldPath::deprecatedCreate(&expCtx, "a");
        ASSERT_BSONOBJ_BINARY_EQ(fromjson("{}"),
                                 toBson(expression->evaluate({}, &expCtx.variables)));
    }
};

/** Simple case where the target field is present. */
class Present {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression = ExpressionFieldPath::deprecatedCreate(&expCtx, "a");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{'':123}"),
            toBson(expression->evaluate(fromBson(BSON("a" << 123)), &expCtx.variables)));
    }
};

/** Target field parent is null. */
class NestedBelowNull {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{}"),
            toBson(expression->evaluate(fromBson(fromjson("{a:null}")), &expCtx.variables)));
    }
};

/** Target field parent is undefined. */
class NestedBelowUndefined {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{}"),
            toBson(expression->evaluate(fromBson(fromjson("{a:undefined}")), &expCtx.variables)));
    }
};

/** Target field parent is missing. */
class NestedBelowMissing {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{}"),
            toBson(expression->evaluate(fromBson(fromjson("{z:1}")), &expCtx.variables)));
    }
};

/** Target field parent is an integer. */
class NestedBelowInt {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{}"),
            toBson(expression->evaluate(fromBson(BSON("a" << 2)), &expCtx.variables)));
    }
};

/** A value in a nested object. */
class NestedValue {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(BSON("" << 55),
                                 toBson(expression->evaluate(fromBson(BSON("a" << BSON("b" << 55))),
                                                             &expCtx.variables)));
    }
};

/** Target field within an empty object. */
class NestedBelowEmptyObject {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{}"),
            toBson(expression->evaluate(fromBson(BSON("a" << BSONObj())), &expCtx.variables)));
    }
};

/** Target field within an empty array. */
class NestedBelowEmptyArray {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            BSON("" << BSONArray()),
            toBson(expression->evaluate(fromBson(BSON("a" << BSONArray())), &expCtx.variables)));
    }
};

/** Target field within an array containing null. */
class NestedBelowArrayWithNull {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{'':[]}"),
            toBson(expression->evaluate(fromBson(fromjson("{a:[null]}")), &expCtx.variables)));
    }
};

/** Target field within an array containing undefined. */
class NestedBelowArrayWithUndefined {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{'':[]}"),
            toBson(expression->evaluate(fromBson(fromjson("{a:[undefined]}")), &expCtx.variables)));
    }
};

/** Target field within an array containing an integer. */
class NestedBelowArrayWithInt {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{'':[]}"),
            toBson(expression->evaluate(fromBson(fromjson("{a:[1]}")), &expCtx.variables)));
    }
};

/** Target field within an array. */
class NestedWithinArray {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{'':[9]}"),
            toBson(expression->evaluate(fromBson(fromjson("{a:[{b:9}]}")), &expCtx.variables)));
    }
};

/** Multiple value types within an array. */
class MultipleArrayValues {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{'':[9,20]}"),
            toBson(expression->evaluate(
                fromBson(fromjson("{a:[{b:9},null,undefined,{g:4},{b:20},{}]}")),
                &expCtx.variables)));
    }
};

/** Expanding values within nested arrays. */
class ExpandNestedArrays {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b.c");
        ASSERT_BSONOBJ_BINARY_EQ(
            fromjson("{'':[[1,2],3,[4],[[5]],[6,7]]}"),
            toBson(expression->evaluate(fromBson(fromjson("{a:[{b:[{c:1},{c:2}]},"
                                                          "{b:{c:3}},"
                                                          "{b:[{c:4}]},"
                                                          "{b:[{c:[5]}]},"
                                                          "{b:{c:[6,7]}}]}")),
                                        &expCtx.variables)));
    }
};

/** Add to a BSONObj. */
class AddToBsonObj {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b.c");
        ASSERT_BSONOBJ_BINARY_EQ(BSON("foo"
                                      << "$a.b.c"),
                                 BSON("foo" << expression->serialize(false)));
    }
};

/** Add to a BSONArray. */
class AddToBsonArray {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b.c");
        BSONArrayBuilder bab;
        bab << expression->serialize(false);
        ASSERT_BSONOBJ_BINARY_EQ(BSON_ARRAY("$a.b.c"), bab.arr());
    }
};

}  // namespace FieldPath

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("expression") {}

    void setupTests() {
        add<FieldPath::Invalid>();
        add<FieldPath::Dependencies>();
        add<FieldPath::Missing>();
        add<FieldPath::Present>();
        add<FieldPath::NestedBelowNull>();
        add<FieldPath::NestedBelowUndefined>();
        add<FieldPath::NestedBelowMissing>();
        add<FieldPath::NestedBelowInt>();
        add<FieldPath::NestedValue>();
        add<FieldPath::NestedBelowEmptyObject>();
        add<FieldPath::NestedBelowEmptyArray>();
        add<FieldPath::NestedBelowEmptyArray>();
        add<FieldPath::NestedBelowArrayWithNull>();
        add<FieldPath::NestedBelowArrayWithUndefined>();
        add<FieldPath::NestedBelowArrayWithInt>();
        add<FieldPath::NestedWithinArray>();
        add<FieldPath::MultipleArrayValues>();
        add<FieldPath::ExpandNestedArrays>();
        add<FieldPath::AddToBsonObj>();
        add<FieldPath::AddToBsonArray>();
    }
};

OldStyleSuiteInitializer<All> fieldPathAll;

}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo

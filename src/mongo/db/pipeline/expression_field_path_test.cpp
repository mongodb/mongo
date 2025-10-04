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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <functional>
#include <string>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

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

    auto constantExpr = dynamic_cast<ExpressionConstant*>(optimizedExpr.get());
    ASSERT_TRUE(constantExpr);
    ASSERT_VALUE_EQ(Value(), constantExpr->getValue());
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

TEST(FieldPath, NoOptimizationOnCertainVariablesUnderSbeFull) {
    RAIIServerParameterControllerForTest sbe("featureFlagSbeFull", true);

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
    SerializationOptions options = SerializationOptions::kMarkIdentifiers_FOR_TEST;

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
    options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;

    expression = expr(R"({$gt: ["$$ROOT.a.b", 5]})");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"$gt":["$$ROOT.HASH<a>.HASH<b>","?number"]})",
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

/** Add to a BSONObj. */
class AddToBsonObj {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression =
            ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b.c");
        ASSERT_BSONOBJ_BINARY_EQ(BSON("foo" << "$a.b.c"), BSON("foo" << expression->serialize()));
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
        bab << expression->serialize();
        ASSERT_BSONOBJ_BINARY_EQ(BSON_ARRAY("$a.b.c"), bab.arr());
    }
};

}  // namespace FieldPath

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("expression") {}

    void setupTests() override {
        add<FieldPath::Invalid>();
        add<FieldPath::Dependencies>();
        add<FieldPath::AddToBsonObj>();
        add<FieldPath::AddToBsonArray>();
    }
};

unittest::OldStyleSuiteInitializer<All> fieldPathAll;

}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo

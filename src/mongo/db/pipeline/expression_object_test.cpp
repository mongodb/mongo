// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


namespace mongo {
namespace ExpressionTests {
namespace {
using std::vector;

namespace Object {
using namespace std::literals::string_view_literals;
using mongo::ExpressionObject;

template <typename T>
Document literal(T&& value) {
    return Document{{"$const", Value(std::forward<T>(value))}};
}

//
// Parsing.
//

TEST(ExpressionObjectParse, ShouldAcceptEmptyObject) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object = ExpressionObject::parse(&expCtx, BSONObj(), vps);
    ASSERT_VALUE_EQ(Value(Document{}), object->serialize());
}

TEST(ExpressionObjectParse, ShouldAcceptLiteralsAsValues) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object = ExpressionObject::parse(&expCtx,
                                          BSON("a" << 5 << "b"
                                                   << "string"
                                                   << "c" << BSONNULL),
                                          vps);
    auto expectedResult =
        Value(Document{{"a", literal(5)}, {"b", literal("string"sv)}, {"c", literal(BSONNULL)}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize());
}

TEST(ExpressionObjectParse, ShouldAccept_idAsFieldName) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object = ExpressionObject::parse(&expCtx, BSON("_id" << 5), vps);
    auto expectedResult = Value(Document{{"_id", literal(5)}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize());
}

TEST(ExpressionObjectParse, ShouldAcceptFieldNameContainingDollar) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object = ExpressionObject::parse(&expCtx, BSON("a$b" << 5), vps);
    auto expectedResult = Value(Document{{"a$b", literal(5)}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize());
}

TEST(ExpressionObjectParse, ShouldAcceptNestedObjects) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object =
        ExpressionObject::parse(&expCtx, fromjson("{a: {b: 1}, c: {d: {e: 1, f: 1}}}"), vps);
    auto expectedResult =
        Value(Document{{"a", Document{{"b", literal(1)}}},
                       {"c", Document{{"d", Document{{"e", literal(1)}, {"f", literal(1)}}}}}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize());
}

TEST(ExpressionObjectParse, ShouldAcceptArrays) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object = ExpressionObject::parse(&expCtx, fromjson("{a: [1, 2]}"), vps);
    auto expectedResult =
        Value(Document{{"a", vector<Value>{Value(literal(1)), Value(literal(2))}}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize());
}

TEST(ObjectParsing, ShouldAcceptExpressionAsValue) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object = ExpressionObject::parse(&expCtx, BSON("a" << BSON("$and" << BSONArray())), vps);
    ASSERT_VALUE_EQ(object->serialize(), Value(Document{{"a", Document{{"$and", BSONArray()}}}}));
}

//
// Error cases.
//

TEST(ExpressionObjectParse, ShouldRejectDottedFieldNames) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    ASSERT_THROWS(ExpressionObject::parse(&expCtx, BSON("a.b" << 1), vps), AssertionException);
    ASSERT_THROWS(ExpressionObject::parse(&expCtx, BSON("c" << 3 << "a.b" << 1), vps),
                  AssertionException);
    ASSERT_THROWS(ExpressionObject::parse(&expCtx, BSON("a.b" << 1 << "c" << 3), vps),
                  AssertionException);
}

TEST(ExpressionObjectParse, ShouldRejectDuplicateFieldNames) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    ASSERT_THROWS(ExpressionObject::parse(&expCtx, BSON("a" << 1 << "a" << 1), vps),
                  AssertionException);
    ASSERT_THROWS(ExpressionObject::parse(&expCtx, BSON("a" << 1 << "b" << 2 << "a" << 1), vps),
                  AssertionException);
    ASSERT_THROWS(
        ExpressionObject::parse(&expCtx, BSON("a" << BSON("c" << 1) << "b" << 2 << "a" << 1), vps),
        AssertionException);
    ASSERT_THROWS(
        ExpressionObject::parse(&expCtx, BSON("a" << 1 << "b" << 2 << "a" << BSON("c" << 1)), vps),
        AssertionException);
}

TEST(ExpressionObjectParse, ShouldRejectInvalidFieldName) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    ASSERT_THROWS(ExpressionObject::parse(&expCtx, BSON("$a" << 1), vps), AssertionException);
    ASSERT_THROWS(ExpressionObject::parse(&expCtx, BSON("" << 1), vps), AssertionException);
    ASSERT_THROWS(ExpressionObject::parse(&expCtx, BSON(std::string("a\0b", 3) << 1), vps),
                  AssertionException);
}

TEST(ExpressionObjectParse, ShouldRejectInvalidFieldPathAsValue) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    ASSERT_THROWS(ExpressionObject::parse(&expCtx, BSON("a" << "$field."), vps),
                  AssertionException);
}

TEST(ParseObject, ShouldRejectExpressionAsTheSecondField) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    ASSERT_THROWS(
        ExpressionObject::parse(
            &expCtx, BSON("a" << BSON("$and" << BSONArray()) << "$or" << BSONArray()), vps),
        AssertionException);
}

//
// Dependencies.
//

TEST(ExpressionObjectDependencies, ConstantValuesShouldNotBeAddedToDependencies) {
    auto expCtx = ExpressionContextForTest{};
    auto object =
        ExpressionObject::create(&expCtx, {{"a", ExpressionConstant::create(&expCtx, Value{5})}});
    DepsTracker deps;
    expression::addDependencies(object.get(), &deps);
    ASSERT_EQ(deps.fields.size(), 0UL);
}

TEST(ExpressionObjectDependencies, FieldPathsShouldBeAddedToDependencies) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx,
        {{"x",
          ExpressionFieldPath::createPathFromString(&expCtx, "c.d", expCtx.variablesParseState)}});
    DepsTracker deps;
    expression::addDependencies(object.get(), &deps);
    ASSERT_EQ(deps.fields.size(), 1UL);
    ASSERT_EQ(deps.fields.count("c.d"), 1UL);
};

TEST(ExpressionObjectDependencies, VariablesShouldBeAddedToReferences) {
    auto expCtx = ExpressionContextForTest{};
    auto varID = expCtx.variablesParseState.defineVariable("var1");
    auto fieldPath = ExpressionFieldPath::parse(&expCtx, "$$var1", expCtx.variablesParseState);
    std::set<Variables::Id> refs;
    expression::addVariableRefs(fieldPath.get(), &refs);
    ASSERT_EQ(refs.size(), 1UL);
    ASSERT_EQ(refs.count(varID), 1UL);
}

TEST(ExpressionObjectDependencies, LocalLetVariablesShouldBeFilteredOutOfDependencies) {
    auto expCtx = ExpressionContextForTest{};
    expCtx.variablesParseState.defineVariable("var1");
    auto letSpec =
        BSON("$let" << BSON("vars" << BSON("var2" << "abc") << "in"
                                   << BSON("$multiply" << BSON_ARRAY("$$var1" << "$$var2"))));
    auto expressionLet =
        ExpressionLet::parse(&expCtx, letSpec.firstElement(), expCtx.variablesParseState);
    std::set<Variables::Id> refs;
    expression::addVariableRefs(expressionLet.get(), &refs);
    ASSERT_EQ(refs.size(), 1UL);
    ASSERT_EQ(expCtx.variablesParseState.getVariable("var1"), *refs.begin());
}

TEST(ExpressionObjectDependencies, LocalMapVariablesShouldBeFilteredOutOfDependencies) {
    auto expCtx = ExpressionContextForTest{};
    expCtx.variablesParseState.defineVariable("var1");
    auto mapSpec = BSON(
        "$map" << BSON("input" << "$field1"
                               << "as"
                               << "var2"
                               << "in" << BSON("$multiply" << BSON_ARRAY("$$var1" << "$$var2"))));

    auto expressionMap =
        ExpressionMap::parse(&expCtx, mapSpec.firstElement(), expCtx.variablesParseState);
    std::set<Variables::Id> refs;
    expression::addVariableRefs(expressionMap.get(), &refs);
    ASSERT_EQ(refs.size(), 1UL);
    ASSERT_EQ(expCtx.variablesParseState.getVariable("var1"), *refs.begin());
}

TEST(ExpressionObjectDependencies, LocalFilterVariablesShouldBeFilteredOutOfDependencies) {
    auto expCtx = ExpressionContextForTest{};
    expCtx.variablesParseState.defineVariable("var1");
    auto filterSpec = BSON(
        "$filter" << BSON("input" << BSON_ARRAY(1 << 2 << 3) << "as"
                                  << "var2"
                                  << "cond" << BSON("$gt" << BSON_ARRAY("$$var1" << "$$var2"))));

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, filterSpec.firstElement(), expCtx.variablesParseState);
    std::set<Variables::Id> refs;
    expression::addVariableRefs(expressionFilter.get(), &refs);
    ASSERT_EQ(refs.size(), 1UL);
    ASSERT_EQ(expCtx.variablesParseState.getVariable("var1"), *refs.begin());
}

//
// Optimizations.
//

TEST(ExpressionObjectOptimizations, OptimizingAnObjectShouldOptimizeSubExpressions) {
    // Build up the object {a: {$add: [1, 2]}}.
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto addExpression =
        ExpressionAdd::parse(&expCtx, BSON("$add" << BSON_ARRAY(1 << 2)).firstElement(), vps);
    auto object = ExpressionObject::create(&expCtx, {{"a", addExpression}});
    ASSERT_EQ(object->getChildExpressions().size(), 1UL);

    auto optimized = object->optimize();
    auto optimizedObject = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(optimizedObject);
    ASSERT_VALUE_EQ(Value(BSON("a" << 3)), optimizedObject->getValue());
};

TEST(ExpressionObjectOptimizations,
     OptimizingAnObjectWithAllConstantsShouldOptimizeToExpressionConstant) {

    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    // All constants should optimize to ExpressionConstant.
    auto objectWithAllConstants = ExpressionObject::parse(&expCtx, BSON("b" << 1 << "c" << 1), vps);
    auto optimizedToAllConstants = objectWithAllConstants->optimize();
    auto constants = dynamic_cast<ExpressionConstant*>(optimizedToAllConstants.get());
    ASSERT_TRUE(constants);

    // Not all constants should not optimize to ExpressionConstant.
    auto objectNotAllConstants = ExpressionObject::parse(&expCtx,
                                                         BSON("b" << 1 << "input"
                                                                  << "$inputField"),
                                                         vps);
    auto optimizedNotAllConstants = objectNotAllConstants->optimize();
    auto shouldNotBeConstant = dynamic_cast<ExpressionConstant*>(optimizedNotAllConstants.get());
    ASSERT_FALSE(shouldNotBeConstant);

    // Sub expression should optimize to constant expression.
    auto expressionWithConstantObject = ExpressionObject::parse(
        &expCtx,
        BSON("willBeConstant" << BSON("$add" << BSON_ARRAY(1 << 2)) << "alreadyConstant"
                              << "string"),
        vps);
    auto optimizedWithConstant = expressionWithConstantObject->optimize();
    auto optimizedObject = dynamic_cast<ExpressionConstant*>(optimizedWithConstant.get());
    ASSERT_TRUE(optimizedObject);
    ASSERT_VALUE_EQ(Value(BSON("willBeConstant" << 3 << "alreadyConstant"
                                                << "string")),
                    optimizedObject->getValue());
};

}  // namespace Object

}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo

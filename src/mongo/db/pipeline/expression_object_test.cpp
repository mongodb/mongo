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
#include "mongo/config.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace ExpressionTests {
namespace {
using boost::intrusive_ptr;
using std::vector;

namespace Object {
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
    ASSERT_VALUE_EQ(Value(Document{}), object->serialize(false));
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
        Value(Document{{"a", literal(5)}, {"b", literal("string"_sd)}, {"c", literal(BSONNULL)}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize(false));
}

TEST(ExpressionObjectParse, ShouldAccept_idAsFieldName) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object = ExpressionObject::parse(&expCtx, BSON("_id" << 5), vps);
    auto expectedResult = Value(Document{{"_id", literal(5)}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize(false));
}

TEST(ExpressionObjectParse, ShouldAcceptFieldNameContainingDollar) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object = ExpressionObject::parse(&expCtx, BSON("a$b" << 5), vps);
    auto expectedResult = Value(Document{{"a$b", literal(5)}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize(false));
}

TEST(ExpressionObjectParse, ShouldAcceptNestedObjects) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object =
        ExpressionObject::parse(&expCtx, fromjson("{a: {b: 1}, c: {d: {e: 1, f: 1}}}"), vps);
    auto expectedResult =
        Value(Document{{"a", Document{{"b", literal(1)}}},
                       {"c", Document{{"d", Document{{"e", literal(1)}, {"f", literal(1)}}}}}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize(false));
}

TEST(ExpressionObjectParse, ShouldAcceptArrays) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object = ExpressionObject::parse(&expCtx, fromjson("{a: [1, 2]}"), vps);
    auto expectedResult =
        Value(Document{{"a", vector<Value>{Value(literal(1)), Value(literal(2))}}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize(false));
}

TEST(ObjectParsing, ShouldAcceptExpressionAsValue) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto object = ExpressionObject::parse(&expCtx, BSON("a" << BSON("$and" << BSONArray())), vps);
    ASSERT_VALUE_EQ(object->serialize(false),
                    Value(Document{{"a", Document{{"$and", BSONArray()}}}}));
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
    ASSERT_THROWS(ExpressionObject::parse(&expCtx,
                                          BSON("a"
                                               << "$field."),
                                          vps),
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
// Evaluation.
//

TEST(ExpressionObjectEvaluate, EmptyObjectShouldEvaluateToEmptyDocument) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(&expCtx, {});
    ASSERT_VALUE_EQ(Value(Document()), object->evaluate(Document(), &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document()), object->evaluate(Document{{"a", 1}}, &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document()),
                    object->evaluate(Document{{"_id", "ID"_sd}}, &(expCtx.variables)));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateEachField) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(&expCtx,
                                           {{"a", ExpressionConstant::create(&expCtx, Value{1})},
                                            {"b", ExpressionConstant::create(&expCtx, Value{5})}});


    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}),
                    object->evaluate(Document(), &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}),
                    object->evaluate(Document{{"a", 1}}, &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}),
                    object->evaluate(Document{{"_id", "ID"_sd}}, &(expCtx.variables)));
}

TEST(ExpressionObjectEvaluate, OrderOfFieldsInOutputShouldMatchOrderInSpecification) {
    auto expCtx = ExpressionContextForTest{};
    auto object =
        ExpressionObject::create(&expCtx,
                                 {{"a", ExpressionFieldPath::deprecatedCreate(&expCtx, "a")},
                                  {"b", ExpressionFieldPath::deprecatedCreate(&expCtx, "b")},
                                  {"c", ExpressionFieldPath::deprecatedCreate(&expCtx, "c")}});
    ASSERT_VALUE_EQ(
        Value(Document{{"a", "A"_sd}, {"b", "B"_sd}, {"c", "C"_sd}}),
        object->evaluate(Document{{"c", "C"_sd}, {"a", "A"_sd}, {"b", "B"_sd}, {"_id", "ID"_sd}},
                         &(expCtx.variables)));
}

TEST(ExpressionObjectEvaluate, ShouldRemoveFieldsThatHaveMissingValues) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx,
        {{"a", ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b")},
         {"b", ExpressionFieldPath::deprecatedCreate(&expCtx, "missing")}});
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document(), &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document{{"a", 1}}, &(expCtx.variables)));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateFieldsWithinNestedObject) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx,
        {{"a",
          ExpressionObject::create(
              &expCtx,
              {{"b", ExpressionConstant::create(&expCtx, Value{1})},
               {"c", ExpressionFieldPath::deprecatedCreate(&expCtx, "_id")}})}});
    ASSERT_VALUE_EQ(Value(Document{{"a", Document{{"b", 1}}}}),
                    object->evaluate(Document(), &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document{{"a", Document{{"b", 1}, {"c", "ID"_sd}}}}),
                    object->evaluate(Document{{"_id", "ID"_sd}}, &(expCtx.variables)));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateToEmptyDocumentIfAllFieldsAreMissing) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx, {{"a", ExpressionFieldPath::deprecatedCreate(&expCtx, "missing")}});
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document(), &(expCtx.variables)));

    auto objectWithNestedObject = ExpressionObject::create(&expCtx, {{"nested", object}});
    ASSERT_VALUE_EQ(Value(Document{{"nested", Document{}}}),
                    objectWithNestedObject->evaluate(Document(), &(expCtx.variables)));
}

//
// Dependencies.
//

TEST(ExpressionObjectDependencies, ConstantValuesShouldNotBeAddedToDependencies) {
    auto expCtx = ExpressionContextForTest{};
    auto object =
        ExpressionObject::create(&expCtx, {{"a", ExpressionConstant::create(&expCtx, Value{5})}});
    DepsTracker deps;
    object->addDependencies(&deps);
    ASSERT_EQ(deps.fields.size(), 0UL);
}

TEST(ExpressionObjectDependencies, FieldPathsShouldBeAddedToDependencies) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx, {{"x", ExpressionFieldPath::deprecatedCreate(&expCtx, "c.d")}});
    DepsTracker deps;
    object->addDependencies(&deps);
    ASSERT_EQ(deps.fields.size(), 1UL);
    ASSERT_EQ(deps.fields.count("c.d"), 1UL);
};

TEST(ExpressionObjectDependencies, VariablesShouldBeAddedToDependencies) {
    auto expCtx = ExpressionContextForTest{};
    auto varID = expCtx.variablesParseState.defineVariable("var1");
    auto fieldPath = ExpressionFieldPath::parse(&expCtx, "$$var1", expCtx.variablesParseState);
    DepsTracker deps;
    fieldPath->addDependencies(&deps);
    ASSERT_EQ(deps.vars.size(), 1UL);
    ASSERT_EQ(deps.vars.count(varID), 1UL);
}

TEST(ExpressionObjectDependencies, LocalLetVariablesShouldBeFilteredOutOfDependencies) {
    auto expCtx = ExpressionContextForTest{};
    expCtx.variablesParseState.defineVariable("var1");
    auto letSpec = BSON("$let" << BSON("vars" << BSON("var2"
                                                      << "abc")
                                              << "in"
                                              << BSON("$multiply" << BSON_ARRAY("$$var1"
                                                                                << "$$var2"))));
    auto expressionLet =
        ExpressionLet::parse(&expCtx, letSpec.firstElement(), expCtx.variablesParseState);
    DepsTracker deps;
    expressionLet->addDependencies(&deps);
    ASSERT_EQ(deps.vars.size(), 1UL);
    ASSERT_EQ(expCtx.variablesParseState.getVariable("var1"), *deps.vars.begin());
}

TEST(ExpressionObjectDependencies, LocalMapVariablesShouldBeFilteredOutOfDependencies) {
    auto expCtx = ExpressionContextForTest{};
    expCtx.variablesParseState.defineVariable("var1");
    auto mapSpec = BSON("$map" << BSON("input"
                                       << "$field1"
                                       << "as"
                                       << "var2"
                                       << "in"
                                       << BSON("$multiply" << BSON_ARRAY("$$var1"
                                                                         << "$$var2"))));

    auto expressionMap =
        ExpressionMap::parse(&expCtx, mapSpec.firstElement(), expCtx.variablesParseState);
    DepsTracker deps;
    expressionMap->addDependencies(&deps);
    ASSERT_EQ(deps.vars.size(), 1UL);
    ASSERT_EQ(expCtx.variablesParseState.getVariable("var1"), *deps.vars.begin());
}

TEST(ExpressionObjectDependencies, LocalFilterVariablesShouldBeFilteredOutOfDependencies) {
    auto expCtx = ExpressionContextForTest{};
    expCtx.variablesParseState.defineVariable("var1");
    auto filterSpec = BSON("$filter" << BSON("input" << BSON_ARRAY(1 << 2 << 3) << "as"
                                                     << "var2"
                                                     << "cond"
                                                     << BSON("$gt" << BSON_ARRAY("$$var1"
                                                                                 << "$$var2"))));

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, filterSpec.firstElement(), expCtx.variablesParseState);
    DepsTracker deps;
    expressionFilter->addDependencies(&deps);
    ASSERT_EQ(deps.vars.size(), 1UL);
    ASSERT_EQ(expCtx.variablesParseState.getVariable("var1"), *deps.vars.begin());
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
    ASSERT_VALUE_EQ(optimizedObject->evaluate(Document(), &(expCtx.variables)),
                    Value(BSON("a" << 3)));
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
    ASSERT_VALUE_EQ(optimizedObject->evaluate(Document(), &expCtx.variables),
                    Value(BSON("willBeConstant" << 3 << "alreadyConstant"
                                                << "string")));
};

}  // namespace Object

}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo

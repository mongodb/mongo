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

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::projection_executor {
namespace {
using std::vector;

// These AddFieldsProjectionExecutor spec tests are a subset of the ProjectionExecutor creation
// tests. AddFieldsProjectionExecutor should behave the same way, but does not use the same
// creation, so we include an abbreviation of the same tests here.

// Verify that AddFieldsProjectionExecutor rejects specifications with conflicting field paths.
TEST(AddFieldsProjectionExecutorSpec, ThrowsOnCreationWithConflictingFieldPaths) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // These specs contain the same exact path.
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("a" << 1 << "a" << 2)),
                  AssertionException);
    ASSERT_THROWS(
        AddFieldsProjectionExecutor::create(expCtx, BSON("a" << BSON("b" << 1 << "b" << 2))),
        AssertionException);
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("_id" << 3 << "_id" << true)),
                  AssertionException);

    // These specs contain overlapping paths.
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("a" << 1 << "a.b" << 2)),
                  AssertionException);
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("a.b.c" << 1 << "a" << 2)),
                  AssertionException);
    ASSERT_THROWS(
        AddFieldsProjectionExecutor::create(expCtx, BSON("_id" << true << "_id.x" << true)),
        AssertionException);
}

// Verify that AddFieldsProjectionExecutor rejects specifications that contain invalid field paths.
TEST(AddFieldsProjectionExecutorSpec, ThrowsOnCreationWithInvalidFieldPath) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Dotted subfields are not allowed.
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("a" << BSON("b.c" << true))),
                  AssertionException);

    // The user cannot start a field with $.
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("$dollar" << 0)),
                  AssertionException);
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("c.$d" << true)),
                  AssertionException);

    // Empty field names should throw an error.
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("" << 2)), AssertionException);
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("a" << BSON("" << true))),
                  AssertionException);
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("" << BSON("a" << true))),
                  AssertionException);
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON("a." << true)),
                  AssertionException);
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(expCtx, BSON(".a" << true)),
                  AssertionException);
}

// Verify that AddFieldsProjectionExecutor rejects specifications that contain empty objects or
// invalid expressions.
TEST(AddFieldsProjectionExecutorSpec, ThrowsOnCreationWithInvalidObjectsOrExpressions) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Invalid expressions should be rejected.
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(
                      expCtx, BSON("a" << BSON("$add" << BSON_ARRAY(4 << 2) << "b" << 1))),
                  AssertionException);
    ASSERT_THROWS(
        AddFieldsProjectionExecutor::create(expCtx,
                                            BSON("a" << BSON("$gt" << BSON("bad"
                                                                           << "arguments")))),
        AssertionException);
    ASSERT_THROWS(AddFieldsProjectionExecutor::create(
                      expCtx, BSON("a" << false << "b" << BSON("$unknown" << BSON_ARRAY(4 << 2)))),
                  AssertionException);
}

TEST(AddFieldsProjectionExecutor, DoesNotErrorOnEmptySpec) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor::create(expCtx, BSONObj());
}

TEST(AddFieldsProjectionExecutor, DoesNotErrorOnTwoNestedFields) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor::create(expCtx, BSON("a.b" << true << "a.c" << true));
    AddFieldsProjectionExecutor::create(expCtx, BSON("a.b" << true << "a" << BSON("c" << true)));
}

// Verify that replaced fields are not included as dependencies.
TEST(AddFieldsProjectionExecutorDeps, RemovesReplaceFieldsFromDependencies) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("a" << true));

    DepsTracker deps;
    addition.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 0UL);
    ASSERT_EQ(deps.fields.count("_id"), 0UL);  // Not explicitly included.
    ASSERT_EQ(deps.fields.count("a"), 0UL);    // Set to true.
}

TEST(AddFieldsProjectionExecutor, ShouldNotIncludeDependenciesForEmptySpec) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("a" << true));

    DepsTracker deps;
    addition.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 0UL);
}

// Verify that adding nested fields keeps the top-level field as a dependency.
TEST(AddFieldsProjectionExecutorDeps, IncludesTopLevelFieldInDependenciesWhenAddingNestedFields) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("x.y" << true));

    DepsTracker deps;
    addition.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 1UL);
    ASSERT_EQ(deps.fields.count("_id"), 0UL);  // Not explicitly included.
    ASSERT_EQ(deps.fields.count("x.y"), 0UL);  // Set to true.
    ASSERT_EQ(deps.fields.count("x"), 1UL);    // Top-level of nested field included.
}

// Verify that fields that an expression depends on are added to the dependencies.
TEST(AddFieldsProjectionExecutorDeps, AddsDependenciesForComputedFields) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("x.y"
                        << "$z"
                        << "a"
                        << "$b"));

    DepsTracker deps;
    addition.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 3UL);
    ASSERT_EQ(deps.fields.count("_id"), 0UL);  // Not explicitly included.
    ASSERT_EQ(deps.fields.count("z"), 1UL);    // Needed by the ExpressionFieldPath for x.y.
    ASSERT_EQ(deps.fields.count("x"), 1UL);    // Preserves top-level field, for structure.
    ASSERT_EQ(deps.fields.count("a"), 0UL);    // Replaced, so omitted.
    ASSERT_EQ(deps.fields.count("b"), 1UL);    // Needed by the ExpressionFieldPath for a.
}

// Verify that the serialization produces the correct output: converting numbers and literals to
// their corresponding $const form.
TEST(AddFieldsProjectionExecutorSerialize, SerializesToCorrectForm) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(fromjson("{a: {$add: ['$a', 2]}, b: {d: 3}, 'x.y': {$literal: 4}}"));

    auto expectedSerialization = Document(
        fromjson("{a: {$add: [\"$a\", {$const: 2}]}, b: {d: {$const: 3}}, x: {y: {$const: 4}}}"));

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, addition.serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST(AddFieldsProjectionExecutorSerialize, EmptySpecSerializesToCorrectForm) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(fromjson("{}"));

    auto expectedSerialization = Document();

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, addition.serializeTransformation(boost::none));
}

// Verify that serialize treats the _id field as any other field: including when explicity included.
TEST(AddFieldsProjectionExecutorSerialize, AddsIdToSerializeWhenExplicitlyIncluded) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("_id" << false));

    // Adds explicit "_id" setting field, serializes expressions.
    auto expectedSerialization = Document(fromjson("{_id: {$const: false}}"));

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, addition.serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

// Verify that serialize treats the _id field as any other field: excluded when not explicitly
// listed in the specification. We add this check because it is different behavior from $project,
// yet they derive from the same parent class. If the parent class were to change, this test would
// fail.
TEST(AddFieldsProjectionExecutorSerialize, OmitsIdFromSerializeWhenNotIncluded) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("a" << true));

    // Does not implicitly include "_id" field.
    auto expectedSerialization = Document(fromjson("{a: {$const: true}}"));

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, addition.serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

// Verify that the $addFields stage optimizes expressions into simpler forms when possible.
TEST(AddFieldsProjectionExecutorOptimize, OptimizesTopLevelExpressions) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("a" << BSON("$add" << BSON_ARRAY(1 << 2))));
    addition.optimize();
    auto expectedSerialization = Document{{"a", Document{{"$const", 3}}}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, addition.serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

// Verify that the $addFields stage optimizes expressions even when they are nested.
TEST(AddFieldsProjectionExecutorOptimize, ShouldOptimizeNestedExpressions) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("a.b" << BSON("$add" << BSON_ARRAY(1 << 2))));
    addition.optimize();
    auto expectedSerialization = Document{{"a", Document{{"b", Document{{"$const", 3}}}}}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, addition.serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       addition.serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

//
// Top-level only.
//

// Verify that a new field is added to the end of the document.
TEST(AddFieldsProjectionExecutorExecutionTest, AddsNewFieldToEndOfDocument) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("c" << 3));

    // There are no fields in the document.
    auto result = addition.applyProjection(Document{});
    auto expectedResult = Document{{"c", 3}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are fields in the document but none of them are the added field.
    result = addition.applyProjection(Document{{"a", 1}, {"b", 2}});
    expectedResult = Document{{"a", 1}, {"b", 2}, {"c", 3}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(AddFieldsProjectionExecutorExecutionTest, AddingEmptySpecResultsInNoOp) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);

    // There are no fields in the document.
    auto result = addition.applyProjection(Document{});
    auto expectedResult = Document{{}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are fields in the document but none of them are the added field.
    result = addition.applyProjection(Document{{"a", 1}, {"b", 2}});
    expectedResult = Document{{"a", 1}, {"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}


// Verify that an existing field is replaced and stays in the same order in the document.
TEST(AddFieldsProjectionExecutorExecutionTest, ReplacesFieldThatAlreadyExistsInDocument) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("c" << 3));

    // Specified field is the only field in the document, and is replaced.
    auto result = addition.applyProjection(Document{{"c", 1}});
    auto expectedResult = Document{{"c", 3}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is one of the fields in the document, and is replaced in its existing order.
    result = addition.applyProjection(Document{{"c", 1}, {"b", 2}});
    expectedResult = Document{{"c", 3}, {"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that replacing multiple fields preserves the original field order in the document.
TEST(AddFieldsProjectionExecutorExecutionTest,
     ReplacesMultipleFieldsWhilePreservingInputFieldOrder) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("second"
                        << "SECOND"
                        << "first"
                        << "FIRST"));
    auto result = addition.applyProjection(Document{{"first", 0}, {"second", 1}, {"third", 2}});
    auto expectedResult = Document{{"first", "FIRST"_sd}, {"second", "SECOND"_sd}, {"third", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that adding multiple fields adds the fields in the order specified.
TEST(AddFieldsProjectionExecutorExecutionTest, AddsNewFieldsAfterExistingFieldsInOrderSpecified) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("firstComputed"
                        << "FIRST"
                        << "secondComputed"
                        << "SECOND"));
    auto result = addition.applyProjection(Document{{"first", 0}, {"second", 1}, {"third", 2}});
    auto expectedResult = Document{{"first", 0},
                                   {"second", 1},
                                   {"third", 2},
                                   {"firstComputed", "FIRST"_sd},
                                   {"secondComputed", "SECOND"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that both adding and replacing fields at the same time follows the same rules as doing
// each independently.
TEST(AddFieldsProjectionExecutorExecutionTest,
     ReplacesAndAddsNewFieldsWithSameOrderingRulesAsSeparately) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("firstComputed"
                        << "FIRST"
                        << "second"
                        << "SECOND"));
    auto result = addition.applyProjection(Document{{"first", 0}, {"second", 1}, {"third", 2}});
    auto expectedResult = Document{
        {"first", 0}, {"second", "SECOND"_sd}, {"third", 2}, {"firstComputed", "FIRST"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that _id is included just like a regular field, in whatever order it appears in the
// input document, when adding new fields.
TEST(AddFieldsProjectionExecutorExecutionTest, IdFieldIsKeptInOrderItAppearsInInputDocument) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("newField"
                        << "computedVal"));
    auto result = addition.applyProjection(Document{{"_id", "ID"_sd}, {"a", 1}});
    auto expectedResult = Document{{"_id", "ID"_sd}, {"a", 1}, {"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    result = addition.applyProjection(Document{{"a", 1}, {"_id", "ID"_sd}});
    expectedResult = Document{{"a", 1}, {"_id", "ID"_sd}, {"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that replacing or adding _id works just like any other field.
TEST(AddFieldsProjectionExecutorExecutionTest, ShouldReplaceIdWithComputedId) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("_id"
                        << "newId"));
    auto result = addition.applyProjection(Document{{"_id", "ID"_sd}, {"a", 1}});
    auto expectedResult = Document{{"_id", "newId"_sd}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    result = addition.applyProjection(Document{{"a", 1}, {"_id", "ID"_sd}});
    expectedResult = Document{{"a", 1}, {"_id", "newId"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    result = addition.applyProjection(Document{{"a", 1}});
    expectedResult = Document{{"a", 1}, {"_id", "newId"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Adding nested fields.
//

// Verify that adding a dotted field keeps the other fields in the subdocument.
TEST(AddFieldsProjectionExecutorExecutionTest,
     KeepsExistingSubFieldsWhenAddingSimpleDottedFieldToSubDoc) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("a.b" << true));

    // More than one field in sub document.
    auto result = addition.applyProjection(Document{{"a", Document{{"b", 1}, {"c", 2}}}});
    auto expectedResult = Document{{"a", Document{{"b", true}, {"c", 2}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the sub document.
    result = addition.applyProjection(Document{{"a", Document{{"b", 1}}}});
    expectedResult = Document{{"a", Document{{"b", true}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the sub document.
    result = addition.applyProjection(Document{{"a", Document{{"c", 1}}}});
    expectedResult = Document{{"a", Document{{"c", 1}, {"b", true}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in sub document.
    result = addition.applyProjection(Document{{"a", Document{}}});
    expectedResult = Document{{"a", Document{{"b", true}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that creating a dotted field creates the subdocument structure necessary.
TEST(AddFieldsProjectionExecutorExecutionTest, CreatesSubDocIfDottedAddedFieldDoesNotExist) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("sub.target" << true));

    // Should add the path if it doesn't exist.
    auto result = addition.applyProjection(Document{});
    auto expectedResult = Document{{"sub", Document{{"target", true}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should replace the second part of the path if that part already exists.
    result = addition.applyProjection(Document{{"sub", "notADocument"_sd}});
    expectedResult = Document{{"sub", Document{{"target", true}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that adding a dotted value to an array field sets the field in every element of the array.
// SERVER-25200: make this agree with $set.
TEST(AddFieldsProjectionExecutorExecutionTest, AppliesDottedAdditionToEachElementInArray) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("a.b" << true));


    auto result = addition.applyProjection(Document{{"a",
                                                     {1,
                                                      Document{},
                                                      Document{{"b", 1}},
                                                      Document{{"b", 1}, {"c", 2}},
                                                      vector<Value>{},
                                                      {1, Document{{"c", 1}}}}}});
    // Adds the field "b" to every object in the array. Recurses on non-empty nested arrays.
    auto expectedResult = Document{{"a",
                                    {Document{{"b", true}},
                                     Document{{"b", true}},
                                     Document{{"b", true}},
                                     Document{{"b", true}, {"c", 2}},
                                     vector<Value>{},
                                     {Document{{"b", true}}, Document{{"c", 1}, {"b", true}}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that creation of the subdocument structure works for many layers of nesting.
TEST(AddFieldsProjectionExecutorExecutionTest, CreatesNestedSubDocumentsAllTheWayToAddedField) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("a.b.c.d"
                        << "computedVal"));

    // Should add the path if it doesn't exist.
    auto result = addition.applyProjection(Document{});
    auto expectedResult =
        Document{{"a", Document{{"b", Document{{"c", Document{{"d", "computedVal"_sd}}}}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should replace non-documents with documents.
    result = addition.applyProjection(Document{{"a", Document{{"b", "other"_sd}}}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that _id is not special: we can add subfields to it as well.
TEST(AddFieldsProjectionExecutorExecutionTest, AddsSubFieldsOfId) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("_id.X" << true << "_id.Z"
                                << "NEW"));
    auto result = addition.applyProjection(Document{{"_id", Document{{"X", 1}, {"Y", 2}}}});
    auto expectedResult = Document{{"_id", Document{{"X", true}, {"Y", 2}, {"Z", "NEW"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that both ways of specifying nested fields -- both dotted notation and nesting --
// can be used together in the same specification.
TEST(AddFieldsProjectionExecutorExecutionTest, ShouldAllowMixedNestedAndDottedFields) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    // Include all of "a.b", "a.c", "a.d", and "a.e".
    // Add new computed fields "a.W", "a.X", "a.Y", and "a.Z".
    addition.parse(BSON("a.b" << true << "a.c" << true << "a.W"
                              << "W"
                              << "a.X"
                              << "X"
                              << "a"
                              << BSON("d" << true << "e" << true << "Y"
                                          << "Y"
                                          << "Z"
                                          << "Z")));
    auto result = addition.applyProjection(Document{
        {"a",
         Document{{"b", "b"_sd}, {"c", "c"_sd}, {"d", "d"_sd}, {"e", "e"_sd}, {"f", "f"_sd}}}});
    auto expectedResult = Document{{"a",
                                    Document{{"b", true},
                                             {"c", true},
                                             {"d", true},
                                             {"e", true},
                                             {"f", "f"_sd},
                                             {"W", "W"_sd},
                                             {"X", "X"_sd},
                                             {"Y", "Y"_sd},
                                             {"Z", "Z"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

// Verify that adding nested fields preserves the addition order in the spec.
TEST(AddFieldsProjectionExecutorExecutionTest, AddsNestedAddedFieldsInOrderSpecified) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("b.d"
                        << "FIRST"
                        << "b.c"
                        << "SECOND"));
    auto result = addition.applyProjection(Document{});
    auto expectedResult = Document{{"b", Document{{"d", "FIRST"_sd}, {"c", "SECOND"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Misc/Metadata.
//

// Verify that the metadata is kept from the original input document.
TEST(AddFieldsProjectionExecutorExecutionTest, AlwaysKeepsMetadataFromOriginalDoc) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addition(expCtx);
    addition.parse(BSON("a" << true));

    MutableDocument inputDocBuilder(Document{{"a", 1}});
    inputDocBuilder.metadata().setRandVal(1.0);
    inputDocBuilder.metadata().setTextScore(10.0);
    Document inputDoc = inputDocBuilder.freeze();

    auto result = addition.applyProjection(inputDoc);

    MutableDocument expectedDoc(Document{{"a", true}});
    expectedDoc.copyMetaDataFrom(inputDoc);
    ASSERT_DOCUMENT_EQ(result, expectedDoc.freeze());
}

// Extract computed projections depending on a given field.
TEST(AddFieldsProjectionExecutorExecutionTest, ExtractComputedProjections) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addFields(expCtx);
    addFields.parse(BSON("meta1" << BSON("$toUpper"
                                         << "$myMeta.x")));

    const std::set<StringData> reservedNames{};
    auto [extractedAddFields, deleteFlag] =
        addFields.extractComputedProjections("myMeta", "meta", reservedNames);

    ASSERT_EQ(extractedAddFields.nFields(), 1);
    ASSERT_EQ(deleteFlag, true);
    auto expectedAddFields = BSON("meta1" << BSON("$toUpper" << BSON_ARRAY("$meta.x")));
    ASSERT_BSONOBJ_EQ(expectedAddFields, extractedAddFields);

    ASSERT_DOCUMENT_EQ(Document(fromjson("{}")), addFields.serializeTransformation(boost::none));
}

TEST(AddFieldsProjectionExecutorExecutionTest, ExtractComputedProjectionsPrefix) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addFields(expCtx);
    addFields.parse(BSON("meta1" << BSON("$toUpper"
                                         << "$myMeta.x")
                                 << "computed2" << BSON("$add" << BSON_ARRAY("$c" << 1))));

    const std::set<StringData> reservedNames{};
    auto [extractedAddFields, deleteFlag] =
        addFields.extractComputedProjections("myMeta", "meta", reservedNames);

    ASSERT_EQ(extractedAddFields.nFields(), 1);
    ASSERT_EQ(deleteFlag, false);
    auto expectedAddFields = BSON("meta1" << BSON("$toUpper" << BSON_ARRAY("$meta.x")));
    ASSERT_BSONOBJ_EQ(expectedAddFields, extractedAddFields);

    ASSERT_DOCUMENT_EQ(Document(fromjson("{computed2: {$add: [\"$c\", {$const: 1}]}}")),
                       addFields.serializeTransformation(boost::none));
}

TEST(AddFieldsProjectionExecutorExecutionTest, DoNotExtractComputedProjectionsSuffix) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addFields(expCtx);
    addFields.parse(BSON("computed1" << BSON("$add" << BSON_ARRAY("$c" << 1)) << "meta2"
                                     << BSON("$toUpper"
                                             << "$myMeta.x")));

    const std::set<StringData> reservedNames{};
    auto [extractedAddFields, deleteFlag] =
        addFields.extractComputedProjections("myMeta", "meta", reservedNames);

    ASSERT_EQ(extractedAddFields.nFields(), 0);
    ASSERT_EQ(deleteFlag, false);

    auto expectedResult = Document(fromjson(
        "{computed1: {$add: [\"$c\", {$const: 1}]}, meta2 : {$toUpper : [\"$myMeta.x\"]}}"));
    ASSERT_DOCUMENT_EQ(expectedResult, addFields.serializeTransformation(boost::none));
}

TEST(AddFieldsProjectionExecutorExecutionTest, DoNotExtractComputedProjectionWithReservedName) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddFieldsProjectionExecutor addFields(expCtx);
    addFields.parse(BSON("meta1" << BSON("$toUpper"
                                         << "$myMeta.x")
                                 << "data"
                                 << BSON("$toUpper"
                                         << "$myMeta.y")));

    const std::set<StringData> reservedNames{"data"};
    auto [extractedAddFields, deleteFlag] =
        addFields.extractComputedProjections("myMeta", "meta", reservedNames);

    ASSERT_EQ(extractedAddFields.nFields(), 1);
    ASSERT_EQ(deleteFlag, false);
    auto expectedAddFields = BSON("meta1" << BSON("$toUpper" << BSON_ARRAY("$meta.x")));
    ASSERT_BSONOBJ_EQ(expectedAddFields, extractedAddFields);

    ASSERT_DOCUMENT_EQ(Document(fromjson("{data: {$toUpper: [\"$myMeta.y\"]}}")),
                       addFields.serializeTransformation(boost::none));
}

}  // namespace
}  // namespace mongo::projection_executor

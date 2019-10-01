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

#include "mongo/db/pipeline/parsed_inclusion_projection.h"

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace parsed_aggregation_projection {
namespace {

using std::vector;

template <typename T>
BSONObj wrapInLiteral(const T& arg) {
    return BSON("$literal" << arg);
}

// Helper to simplify the creation of a ParsedInclusionProjection with default policies.
ParsedInclusionProjection makeInclusionProjectionWithDefaultPolicies() {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ProjectionPolicies defaultPolicies;
    return {expCtx, defaultPolicies};
}

// Helper to simplify the creation of a ParsedInclusionProjection which excludes _id by default.
ParsedInclusionProjection makeInclusionProjectionWithDefaultIdExclusion() {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ProjectionPolicies defaultExcludeId{ProjectionPolicies::DefaultIdPolicy::kExcludeId,
                                        ProjectionPolicies::kArrayRecursionPolicyDefault,
                                        ProjectionPolicies::kComputedFieldsPolicyDefault};
    return {expCtx, defaultExcludeId};
}

// Helper to simplify the creation of a ParsedInclusionProjection which does not recurse arrays.
ParsedInclusionProjection makeInclusionProjectionWithNoArrayRecursion() {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ProjectionPolicies noArrayRecursion{
        ProjectionPolicies::kDefaultIdPolicyDefault,
        ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays,
        ProjectionPolicies::kComputedFieldsPolicyDefault};
    return {expCtx, noArrayRecursion};
}

DEATH_TEST(InclusionProjectionExecutionTest,
           ShouldFailWhenGivenExcludedNonIdField,
           "Invariant failure elem.trueValue()") {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << false));
}

DEATH_TEST(InclusionProjectionExecutionTest,
           ShouldFailWhenGivenIncludedIdSubfield,
           "Invariant failure elem.trueValue()") {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("_id.id1" << false));
}

TEST(InclusionProjectionExecutionTest, ShouldThrowWhenParsingInvalidExpression) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    ASSERT_THROWS(inclusion.parse(BSON("a" << BSON("$gt" << BSON("bad"
                                                                 << "arguments")))),
                  AssertionException);
}

TEST(InclusionProjectionExecutionTest, ShouldRejectProjectionWithNoOutputFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    ASSERT_THROWS(inclusion.parse(BSON("_id" << false)), AssertionException);
}

TEST(InclusionProjectionExecutionTest, ShouldAddIncludedFieldsToDependencies) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("_id" << false << "a" << true << "x.y" << true));

    DepsTracker deps;
    inclusion.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 2UL);
    ASSERT_EQ(deps.fields.count("_id"), 0UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
    ASSERT_EQ(deps.fields.count("x.y"), 1UL);
}

TEST(InclusionProjectionExecutionTest, ShouldAddIdToDependenciesIfNotSpecified) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true));

    DepsTracker deps;
    inclusion.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 2UL);
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
}

TEST(InclusionProjectionExecutionTest, ShouldAddDependenciesOfComputedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a"
                         << "$a"
                         << "x"
                         << "$z"));

    DepsTracker deps;
    inclusion.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 3UL);
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
    ASSERT_EQ(deps.fields.count("z"), 1UL);
}

TEST(InclusionProjectionExecutionTest, ShouldAddPathToDependenciesForNestedComputedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("x.y"
                         << "$z"));

    DepsTracker deps;
    inclusion.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 3UL);
    // Implicit "_id".
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
    // Needed by the ExpressionFieldPath.
    ASSERT_EQ(deps.fields.count("z"), 1UL);
    // Needed to ensure we preserve the structure of the input document.
    ASSERT_EQ(deps.fields.count("x"), 1UL);
}

TEST(InclusionProjectionExecutionTest, ShouldSerializeToEquivalentProjection) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(fromjson("{a: {$add: ['$a', 2]}, b: {d: 3}, 'x.y': {$literal: 4}}"));

    // Adds implicit "_id" inclusion, converts numbers to bools, serializes expressions.
    auto expectedSerialization = Document(fromjson(
        "{_id: true, a: {$add: [\"$a\", {$const: 2}]}, b: {d: true}, x: {y: {$const: 4}}}"));

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion.serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST(InclusionProjectionExecutionTest, ShouldSerializeExplicitExclusionOfId) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("_id" << false << "a" << true));

    // Adds implicit "_id" inclusion, converts numbers to bools, serializes expressions.
    auto expectedSerialization = Document{{"_id", false}, {"a", true}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion.serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}


TEST(InclusionProjectionExecutionTest, ShouldOptimizeTopLevelExpressions) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << BSON("$add" << BSON_ARRAY(1 << 2))));

    inclusion.optimize();

    auto expectedSerialization = Document{{"_id", true}, {"a", Document{{"$const", 3}}}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion.serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST(InclusionProjectionExecutionTest, ShouldOptimizeNestedExpressions) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a.b" << BSON("$add" << BSON_ARRAY(1 << 2))));

    inclusion.optimize();

    auto expectedSerialization =
        Document{{"_id", true}, {"a", Document{{"b", Document{{"$const", 3}}}}}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion.serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST(InclusionProjectionExecutionTest, ShouldReportThatAllExceptIncludedFieldsAreModified) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << wrapInLiteral("computedVal") << "b.c"
                             << wrapInLiteral("computedVal") << "d" << true << "e.f" << true));

    auto modifiedPaths = inclusion.getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    // Included paths are not modified.
    ASSERT_EQ(modifiedPaths.paths.count("_id"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("d"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("e.f"), 1UL);
    // Computed paths are modified.
    ASSERT_EQ(modifiedPaths.paths.count("a"), 0UL);
    ASSERT_EQ(modifiedPaths.paths.count("b.c"), 0UL);
    ASSERT_EQ(modifiedPaths.paths.size(), 3UL);
}

TEST(InclusionProjectionExecutionTest,
     ShouldReportThatAllExceptIncludedFieldsAreModifiedWithIdExclusion) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("_id" << false << "a" << wrapInLiteral("computedVal") << "b.c"
                               << wrapInLiteral("computedVal") << "d" << true << "e.f" << true));

    auto modifiedPaths = inclusion.getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    // Included paths are not modified.
    ASSERT_EQ(modifiedPaths.paths.count("d"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("e.f"), 1UL);
    // Computed paths are modified.
    ASSERT_EQ(modifiedPaths.paths.count("a"), 0UL);
    ASSERT_EQ(modifiedPaths.paths.count("b.c"), 0UL);
    // _id is explicitly excluded.
    ASSERT_EQ(modifiedPaths.paths.count("_id"), 0UL);

    ASSERT_EQ(modifiedPaths.paths.size(), 2UL);
}

//
// Top-level only.
//

TEST(InclusionProjectionExecutionTest, ShouldIncludeTopLevelField) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true));

    // More than one field in document.
    auto result = inclusion.applyProjection(Document{{"a", 1}, {"b", 2}});
    auto expectedResult = Document{{"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the document.
    result = inclusion.applyProjection(Document{{"a", 1}});
    expectedResult = Document{{"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the document.
    result = inclusion.applyProjection(Document{{"c", 1}});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in the document.
    result = inclusion.applyProjection(Document{});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAddComputedTopLevelField) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("newField" << wrapInLiteral("computedVal")));
    auto result = inclusion.applyProjection(Document{});
    auto expectedResult = Document{{"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Computed field should replace existing field.
    result = inclusion.applyProjection(Document{{"newField", "preExisting"_sd}});
    expectedResult = Document{{"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyBothInclusionsAndComputedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true << "newField" << wrapInLiteral("computedVal")));
    auto result = inclusion.applyProjection(Document{{"a", 1}});
    auto expectedResult = Document{{"a", 1}, {"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldIncludeFieldsInOrderOfInputDoc) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("first" << true << "second" << true << "third" << true));
    auto inputDoc = Document{{"second", 1}, {"first", 0}, {"third", 2}};
    auto result = inclusion.applyProjection(inputDoc);
    ASSERT_DOCUMENT_EQ(result, inputDoc);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyComputedFieldsInOrderSpecified) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("firstComputed" << wrapInLiteral("FIRST") << "secondComputed"
                                         << wrapInLiteral("SECOND")));
    auto result = inclusion.applyProjection(Document{{"first", 0}, {"second", 1}, {"third", 2}});
    auto expectedResult = Document{{"firstComputed", "FIRST"_sd}, {"secondComputed", "SECOND"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldImplicitlyIncludeId) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true));
    auto result = inclusion.applyProjection(Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}});
    auto expectedResult = Document{{"_id", "ID"_sd}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should leave the "_id" in the same place as in the original document.
    result = inclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    expectedResult = Document{{"a", 1}, {"_id", "ID"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldImplicitlyIncludeIdWithComputedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("newField" << wrapInLiteral("computedVal")));
    auto result = inclusion.applyProjection(Document{{"_id", "ID"_sd}, {"a", 1}});
    auto expectedResult = Document{{"_id", "ID"_sd}, {"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldIncludeIdIfExplicitlyIncluded) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true << "_id" << true << "b" << true));
    auto result =
        inclusion.applyProjection(Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}, {"c", 3}});
    auto expectedResult = Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldExcludeIdIfExplicitlyExcluded) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true << "_id" << false));
    auto result = inclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldReplaceIdWithComputedId) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("_id" << wrapInLiteral("newId")));
    auto result = inclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"_id", "newId"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Projections with nested fields.
//

TEST(InclusionProjectionExecutionTest, ShouldIncludeSimpleDottedFieldFromSubDoc) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a.b" << true));

    // More than one field in sub document.
    auto result = inclusion.applyProjection(Document{{"a", Document{{"b", 1}, {"c", 2}}}});
    auto expectedResult = Document{{"a", Document{{"b", 1}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the sub document.
    result = inclusion.applyProjection(Document{{"a", Document{{"b", 1}}}});
    expectedResult = Document{{"a", Document{{"b", 1}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the sub document.
    result = inclusion.applyProjection(Document{{"a", Document{{"c", 1}}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in sub document.
    result = inclusion.applyProjection(Document{{"a", Document{}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldNotCreateSubDocIfDottedIncludedFieldDoesNotExist) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("sub.target" << true));

    // Should not add the path if it doesn't exist.
    auto result = inclusion.applyProjection(Document{});
    auto expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should not replace the first part of the path if that part exists.
    result = inclusion.applyProjection(Document{{"sub", "notADocument"_sd}});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyDottedInclusionToEachElementInArray) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a.b" << true));

    vector<Value> nestedValues = {Value(1),
                                  Value(Document{}),
                                  Value(Document{{"b", 1}}),
                                  Value(Document{{"b", 1}, {"c", 2}}),
                                  Value(vector<Value>{}),
                                  Value(vector<Value>{Value(1), Value(Document{{"c", 1}})})};

    // Drops non-documents and non-arrays. Applies projection to documents, recurses on nested
    // arrays.
    vector<Value> expectedNestedValues = {Value(),
                                          Value(Document{}),
                                          Value(Document{{"b", 1}}),
                                          Value(Document{{"b", 1}}),
                                          Value(vector<Value>{}),
                                          Value(vector<Value>{Value(), Value(Document{})})};
    auto result = inclusion.applyProjection(Document{{"a", nestedValues}});
    auto expectedResult = Document{{"a", expectedNestedValues}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAddComputedDottedFieldToSubDocument) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("sub.target" << wrapInLiteral("computedVal")));

    // Other fields exist in sub document, one of which is the specified field.
    auto result = inclusion.applyProjection(Document{{"sub", Document{{"target", 1}, {"c", 2}}}});
    auto expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the sub document.
    result = inclusion.applyProjection(Document{{"sub", Document{{"c", 1}}}});
    expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in sub document.
    result = inclusion.applyProjection(Document{{"sub", Document{}}});
    expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldCreateSubDocIfDottedComputedFieldDoesntExist) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("sub.target" << wrapInLiteral("computedVal")));

    // Should add the path if it doesn't exist.
    auto result = inclusion.applyProjection(Document{});
    auto expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should replace non-documents with documents.
    result = inclusion.applyProjection(Document{{"sub", "notADocument"_sd}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldCreateNestedSubDocumentsAllTheWayToComputedField) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a.b.c.d" << wrapInLiteral("computedVal")));

    // Should add the path if it doesn't exist.
    auto result = inclusion.applyProjection(Document{});
    auto expectedResult =
        Document{{"a", Document{{"b", Document{{"c", Document{{"d", "computedVal"_sd}}}}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should replace non-documents with documents.
    result = inclusion.applyProjection(Document{{"a", Document{{"b", "other"_sd}}}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAddComputedDottedFieldToEachElementInArray) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a.b" << wrapInLiteral("COMPUTED")));

    vector<Value> nestedValues = {Value(1),
                                  Value(Document{}),
                                  Value(Document{{"b", 1}}),
                                  Value(Document{{"b", 1}, {"c", 2}}),
                                  Value(vector<Value>{}),
                                  Value(vector<Value>{Value(1), Value(Document{{"c", 1}})})};
    vector<Value> expectedNestedValues = {
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(vector<Value>{}),
        Value(vector<Value>{Value(Document{{"b", "COMPUTED"_sd}}),
                            Value(Document{{"b", "COMPUTED"_sd}})})};
    auto result = inclusion.applyProjection(Document{{"a", nestedValues}});
    auto expectedResult = Document{{"a", expectedNestedValues}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyInclusionsAndAdditionsToEachElementInArray) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a.inc" << true << "a.comp" << wrapInLiteral("COMPUTED")));

    vector<Value> nestedValues = {Value(1),
                                  Value(Document{}),
                                  Value(Document{{"inc", 1}}),
                                  Value(Document{{"inc", 1}, {"c", 2}}),
                                  Value(Document{{"c", 2}, {"inc", 1}}),
                                  Value(Document{{"inc", 1}, {"c", 2}, {"comp", "original"_sd}}),
                                  Value(vector<Value>{}),
                                  Value(vector<Value>{Value(1), Value(Document{{"inc", 1}})})};
    vector<Value> expectedNestedValues = {
        Value(Document{{"comp", "COMPUTED"_sd}}),
        Value(Document{{"comp", "COMPUTED"_sd}}),
        Value(Document{{"inc", 1}, {"comp", "COMPUTED"_sd}}),
        Value(Document{{"inc", 1}, {"comp", "COMPUTED"_sd}}),
        Value(Document{{"inc", 1}, {"comp", "COMPUTED"_sd}}),
        Value(Document{{"inc", 1}, {"comp", "COMPUTED"_sd}}),
        Value(vector<Value>{}),
        Value(vector<Value>{Value(Document{{"comp", "COMPUTED"_sd}}),
                            Value(Document{{"inc", 1}, {"comp", "COMPUTED"_sd}})})};
    auto result = inclusion.applyProjection(Document{{"a", nestedValues}});
    auto expectedResult = Document{{"a", expectedNestedValues}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAddOrIncludeSubFieldsOfId) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("_id.X" << true << "_id.Z" << wrapInLiteral("NEW")));
    auto result = inclusion.applyProjection(Document{{"_id", Document{{"X", 1}, {"Y", 2}}}});
    auto expectedResult = Document{{"_id", Document{{"X", 1}, {"Z", "NEW"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAllowMixedNestedAndDottedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    // Include all of "a.b", "a.c", "a.d", and "a.e".
    // Add new computed fields "a.W", "a.X", "a.Y", and "a.Z".
    inclusion.parse(BSON("a.b" << true << "a.c" << true << "a.W" << wrapInLiteral("W") << "a.X"
                               << wrapInLiteral("X") << "a"
                               << BSON("d" << true << "e" << true << "Y" << wrapInLiteral("Y")
                                           << "Z" << wrapInLiteral("Z"))));
    auto result = inclusion.applyProjection(Document{
        {"a",
         Document{{"b", "b"_sd}, {"c", "c"_sd}, {"d", "d"_sd}, {"e", "e"_sd}, {"f", "f"_sd}}}});
    auto expectedResult = Document{{"a",
                                    Document{{"b", "b"_sd},
                                             {"c", "c"_sd},
                                             {"d", "d"_sd},
                                             {"e", "e"_sd},
                                             {"W", "W"_sd},
                                             {"X", "X"_sd},
                                             {"Y", "Y"_sd},
                                             {"Z", "Z"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyNestedComputedFieldsInOrderSpecified) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << wrapInLiteral("FIRST") << "b.c" << wrapInLiteral("SECOND")));
    auto result = inclusion.applyProjection(Document{});
    auto expectedResult = Document{{"a", "FIRST"_sd}, {"b", Document{{"c", "SECOND"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyComputedFieldsAfterAllInclusions) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("b.c" << wrapInLiteral("NEW") << "a" << true));
    auto result = inclusion.applyProjection(Document{{"a", 1}});
    auto expectedResult = Document{{"a", 1}, {"b", Document{{"c", "NEW"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    result = inclusion.applyProjection(Document{{"a", 1}, {"b", 4}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // In this case, the field 'b' shows up first and has a nested inclusion or computed field. Even
    // though it is a computed field, it will appear first in the output document. This is
    // inconsistent, but the expected behavior, and a consequence of applying the projection
    // recursively to each sub-document.
    result = inclusion.applyProjection(Document{{"b", 4}, {"a", 1}});
    expectedResult = Document{{"b", Document{{"c", "NEW"_sd}}}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ComputedFieldReplacingExistingShouldAppearAfterInclusions) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("b" << wrapInLiteral("NEW") << "a" << true));
    auto result = inclusion.applyProjection(Document{{"b", 1}, {"a", 1}});
    auto expectedResult = Document{{"a", 1}, {"b", "NEW"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    result = inclusion.applyProjection(Document{{"a", 1}, {"b", 4}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Metadata inclusion.
//

TEST(InclusionProjectionExecutionTest, ShouldAlwaysKeepMetadataFromOriginalDoc) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true));

    MutableDocument inputDocBuilder(Document{{"a", 1}});
    inputDocBuilder.metadata().setRandVal(1.0);
    inputDocBuilder.metadata().setTextScore(10.0);
    Document inputDoc = inputDocBuilder.freeze();

    auto result = inclusion.applyProjection(inputDoc);

    MutableDocument expectedDoc(inputDoc);
    expectedDoc.copyMetaDataFrom(inputDoc);
    ASSERT_DOCUMENT_EQ(result, expectedDoc.freeze());
}

//
// _id inclusion policy.
//

TEST(InclusionProjectionExecutionTest, ShouldIncludeIdByDefault) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true));

    auto result = inclusion.applyProjection(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"_id", 2}, {"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldIncludeIdWithIncludePolicy) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true));

    auto result = inclusion.applyProjection(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"_id", 2}, {"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldExcludeIdWithExcludePolicy) {
    auto inclusion = makeInclusionProjectionWithDefaultIdExclusion();
    inclusion.parse(BSON("a" << true));

    auto result = inclusion.applyProjection(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldOverrideIncludePolicyWithExplicitExcludeIdSpec) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("_id" << false << "a" << true));

    auto result = inclusion.applyProjection(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldOverrideExcludePolicyWithExplicitIncludeIdSpec) {
    auto inclusion = makeInclusionProjectionWithDefaultIdExclusion();
    inclusion.parse(BSON("_id" << true << "a" << true));

    auto result = inclusion.applyProjection(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"_id", 2}, {"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAllowInclusionOfIdSubfieldWithDefaultIncludePolicy) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("_id.id1" << true << "a" << true));

    auto result = inclusion.applyProjection(
        Document{{"_id", Document{{"id1", 1}, {"id2", 2}}}, {"a", 3}, {"b", 4}});
    auto expectedResult = Document{{"_id", Document{{"id1", 1}}}, {"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAllowInclusionOfIdSubfieldWithDefaultExcludePolicy) {
    auto inclusion = makeInclusionProjectionWithDefaultIdExclusion();
    inclusion.parse(BSON("_id.id1" << true << "a" << true));

    auto result = inclusion.applyProjection(
        Document{{"_id", Document{{"id1", 1}, {"id2", 2}}}, {"a", 3}, {"b", 4}});
    auto expectedResult = Document{{"_id", Document{{"id1", 1}}}, {"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Nested array recursion.
//

TEST(InclusionProjectionExecutionTest, ShouldRecurseNestedArraysByDefault) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a.b" << true));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => {a: [{b: 2}, [{b: 4}], {}]}
    auto result = inclusion.applyProjection(
        Document{{"a",
                  vector<Value>{Value(1),
                                Value(Document{{"b", 2}, {"c", 3}}),
                                Value(vector<Value>{Value(Document{{"b", 4}, {"c", 5}})}),
                                Value(Document{{"d", 6}})}}});

    auto expectedResult = Document{{"a",
                                    vector<Value>{Value(),
                                                  Value(Document{{"b", 2}}),
                                                  Value(vector<Value>{Value(Document{{"b", 4}})}),
                                                  Value(Document{})}}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldNotRecurseNestedArraysForNoRecursePolicy) {
    auto inclusion = makeInclusionProjectionWithNoArrayRecursion();
    inclusion.parse(BSON("a.b" << true));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => {a: [{b: 2}, {}]}
    auto result = inclusion.applyProjection(
        Document{{"a",
                  vector<Value>{Value(1),
                                Value(Document{{"b", 2}, {"c", 3}}),
                                Value(vector<Value>{Value(Document{{"b", 4}, {"c", 5}})}),
                                Value(Document{{"d", 6}})}}});

    auto expectedResult = Document{
        {"a", vector<Value>{Value(), Value(Document{{"b", 2}}), Value(), Value(Document{})}}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldRetainNestedArraysIfNoRecursionNeeded) {
    auto inclusion = makeInclusionProjectionWithNoArrayRecursion();
    inclusion.parse(BSON("a" << true));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => [output doc identical to input]
    const auto inputDoc =
        Document{{"a",
                  vector<Value>{Value(1),
                                Value(Document{{"b", 2}, {"c", 3}}),
                                Value(vector<Value>{Value(Document{{"b", 4}, {"c", 5}})}),
                                Value(Document{{"d", 6}})}}};

    auto result = inclusion.applyProjection(inputDoc);
    const auto& expectedResult = inputDoc;

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ComputedFieldIsAddedToNestedArrayElementsForRecursePolicy) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a.b" << wrapInLiteral("COMPUTED")));

    vector<Value> nestedValues = {Value(1),
                                  Value(Document{}),
                                  Value(Document{{"b", 1}}),
                                  Value(Document{{"b", 1}, {"c", 2}}),
                                  Value(vector<Value>{}),
                                  Value(vector<Value>{Value(1), Value(Document{{"c", 1}})})};
    vector<Value> expectedNestedValues = {
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(vector<Value>{}),
        Value(vector<Value>{Value(Document{{"b", "COMPUTED"_sd}}),
                            Value(Document{{"b", "COMPUTED"_sd}})})};
    auto result = inclusion.applyProjection(Document{{"a", nestedValues}});
    auto expectedResult = Document{{"a", expectedNestedValues}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ComputedFieldShouldReplaceNestedArrayForNoRecursePolicy) {
    auto inclusion = makeInclusionProjectionWithNoArrayRecursion();
    inclusion.parse(BSON("a.b" << wrapInLiteral("COMPUTED")));

    // For kRecurseNestedArrays, the computed field (1) replaces any scalar values in the array with
    // a subdocument containing the new field, and (2) is added to each element of the array and all
    // nested arrays individually. With kDoNotRecurseNestedArrays, the nested arrays are replaced
    // rather than being traversed, in exactly the same way as scalar values.
    vector<Value> nestedValues = {Value(1),
                                  Value(Document{}),
                                  Value(Document{{"b", 1}}),
                                  Value(Document{{"b", 1}, {"c", 2}}),
                                  Value(vector<Value>{}),
                                  Value(vector<Value>{Value(1), Value(Document{{"c", 1}})})};

    vector<Value> expectedNestedValues = {Value(Document{{"b", "COMPUTED"_sd}}),
                                          Value(Document{{"b", "COMPUTED"_sd}}),
                                          Value(Document{{"b", "COMPUTED"_sd}}),
                                          Value(Document{{"b", "COMPUTED"_sd}}),
                                          Value(Document{{"b", "COMPUTED"_sd}}),
                                          Value(Document{{"b", "COMPUTED"_sd}})};

    auto result = inclusion.applyProjection(Document{{"a", nestedValues}});
    auto expectedResult = Document{{"a", expectedNestedValues}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Detection of subset projection.
//

TEST(InclusionProjectionExecutionTest, ShouldDetectSubsetForIdenticalProjection) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true << "b" << true));

    auto proj = BSON("_id" << false << "a" << true << "b" << true);

    ASSERT_TRUE(inclusion.isSubsetOfProjection(proj));
}

TEST(InclusionProjectionExecutionTest, ShouldDetectSubsetForSupersetProjection) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true << "b" << true));

    auto proj = BSON("_id" << false << "a" << true << "b" << true << "c" << true);

    ASSERT_TRUE(inclusion.isSubsetOfProjection(proj));
}

TEST(InclusionProjectionExecutionTest, ShouldDetectSubsetForIdenticalNestedProjection) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a.b" << true));

    auto proj = BSON("_id" << false << "a.b" << true);

    ASSERT_TRUE(inclusion.isSubsetOfProjection(proj));
}

TEST(InclusionProjectionExecutionTest, ShouldDetectSubsetForSupersetProjectionWithNestedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true << "c" << BSON("d" << true)));

    auto proj = BSON("_id" << false << "a" << true << "b" << true << "c.d" << true);

    ASSERT_TRUE(inclusion.isSubsetOfProjection(proj));
}

TEST(InclusionProjectionExecutionTest, ShouldDetectNonSubsetForProjectionWithMissingFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true << "b" << true));

    auto proj = BSON("_id" << false << "a" << true);
    ASSERT_FALSE(inclusion.isSubsetOfProjection(proj));

    proj = BSON("_id" << false << "a" << true << "c" << true);
    ASSERT_FALSE(inclusion.isSubsetOfProjection(proj));
}

TEST(InclusionProjectionExecutionTest,
     ShouldDetectNonSubsetForSupersetProjectionWithoutComputedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true << "b" << true << "c" << BSON("$literal" << 1)));

    auto proj = BSON("_id" << false << "a" << true << "b" << true);

    ASSERT_FALSE(inclusion.isSubsetOfProjection(proj));
}

TEST(InclusionProjectionExecutionTest, ShouldDetectNonSubsetForProjectionWithMissingNestedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a.b" << true << "a.c" << true));

    auto proj = BSON("_id" << false << "a.b" << true);

    ASSERT_FALSE(inclusion.isSubsetOfProjection(proj));
}

TEST(InclusionProjectionExecutionTest, ShouldDetectNonSubsetForProjectionWithRenamedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a"
                         << "$b"));

    auto proj = BSON("_id" << false << "b" << true);

    ASSERT_FALSE(inclusion.isSubsetOfProjection(proj));
}

TEST(InclusionProjectionExecutionTest, ShouldDetectNonSubsetForProjectionWithMissingIdField) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies();
    inclusion.parse(BSON("a" << true));

    auto proj = BSON("a" << true);

    ASSERT_FALSE(inclusion.isSubsetOfProjection(proj));
}

}  // namespace
}  // namespace parsed_aggregation_projection
}  // namespace mongo

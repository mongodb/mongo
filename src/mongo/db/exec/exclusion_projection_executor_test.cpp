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

#include "mongo/db/exec/exclusion_projection_executor.h"

#include <iostream>
#include <iterator>
#include <string>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::projection_executor {
namespace {
using std::vector;

auto createProjectionExecutor(const BSONObj& spec, const ProjectionPolicies& policies) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto projection = projection_ast::parse(expCtx, spec, policies);
    auto builderParams = BuilderParamsBitSet{kDefaultBuilderParams};
    builderParams.reset(kAllowFastPath);
    auto executor = buildProjectionExecutor(expCtx, &projection, policies, builderParams);
    invariant(executor->getType() == TransformerInterface::TransformerType::kExclusionProjection);
    return executor;
}

// Helper to simplify the creation of a ExclusionProjectionExecutor with default policies.
auto makeExclusionProjectionWithDefaultPolicies(const BSONObj& spec) {
    return createProjectionExecutor(spec, {});
}

// Helper to simplify the creation of a ExclusionProjectionExecutor which excludes _id by default.
auto makeExclusionProjectionWithDefaultIdExclusion(const BSONObj& spec) {
    ProjectionPolicies defaultExcludeId{ProjectionPolicies::DefaultIdPolicy::kExcludeId,
                                        ProjectionPolicies::kArrayRecursionPolicyDefault,
                                        ProjectionPolicies::kComputedFieldsPolicyDefault};
    return createProjectionExecutor(spec, defaultExcludeId);
}

// Helper to simplify the creation of a ExclusionProjectionExecutor which does not recurse arrays.
auto makeExclusionProjectionWithNoArrayRecursion(const BSONObj& spec) {
    ProjectionPolicies noArrayRecursion{
        ProjectionPolicies::kDefaultIdPolicyDefault,
        ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays,
        ProjectionPolicies::kComputedFieldsPolicyDefault};
    return createProjectionExecutor(spec, noArrayRecursion);
}

TEST(ExclusionProjectionExecutionTest, ShouldSerializeToEquivalentProjection) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(
        fromjson("{a: 0, b: {c: NumberLong(0), d: 0.0}, 'x.y': false, _id: NumberInt(0)}"));

    // Converts numbers to bools, converts dotted paths to nested documents. Note order of excluded
    // fields is subject to change.
    auto serialization = exclusion->serializeTransformation(boost::none);
    ASSERT_EQ(serialization.computeSize(), 4ULL);
    ASSERT_VALUE_EQ(serialization["a"], Value(false));
    ASSERT_VALUE_EQ(serialization["_id"], Value(false));

    ASSERT_EQ(serialization["b"].getType(), BSONType::Object);
    ASSERT_EQ(serialization["b"].getDocument().computeSize(), 2ULL);
    ASSERT_VALUE_EQ(serialization["b"].getDocument()["c"], Value(false));
    ASSERT_VALUE_EQ(serialization["b"].getDocument()["d"], Value(false));

    ASSERT_EQ(serialization["x"].getType(), BSONType::Object);
    ASSERT_EQ(serialization["x"].getDocument().computeSize(), 1ULL);
    ASSERT_VALUE_EQ(serialization["x"].getDocument()["y"], Value(false));
}

TEST(ExclusionProjectionExecutionTest, ShouldSerializeWithTopLevelID) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("a" << 0 << "b" << 0));
    auto serialization = exclusion->serializeTransformation(boost::none);
    ASSERT_VALUE_EQ(serialization["a"], Value(false));
    ASSERT_VALUE_EQ(serialization["b"], Value(false));
    ASSERT_VALUE_EQ(serialization["_id"], Value(true));

    exclusion = makeExclusionProjectionWithDefaultPolicies(
        BSON("a" << 0 << "b" << BSON("c" << 0 << "d" << 0)));
    serialization = exclusion->serializeTransformation(boost::none);
    ASSERT_VALUE_EQ(serialization["a"], Value(false));
    ASSERT_VALUE_EQ(serialization["b"]["c"], Value(false));
    ASSERT_VALUE_EQ(serialization["b"]["d"], Value(false));
    ASSERT_VALUE_EQ(serialization["_id"], Value(true));
    ASSERT_VALUE_EQ(serialization["b"]["_id"], Value());

    exclusion = makeExclusionProjectionWithDefaultIdExclusion(BSON("a" << false << "b" << false));
    serialization = exclusion->serializeTransformation(boost::none);
    ASSERT_VALUE_EQ(serialization["a"], Value(false));
    ASSERT_VALUE_EQ(serialization["b"], Value(false));
    ASSERT_VALUE_EQ(serialization["_id"], Value(false));

    exclusion = makeExclusionProjectionWithDefaultIdExclusion(
        BSON("a" << false << "b" << false << "_id" << false));
    serialization = exclusion->serializeTransformation(boost::none);
    ASSERT_VALUE_EQ(serialization["a"], Value(false));
    ASSERT_VALUE_EQ(serialization["b"], Value(false));
    ASSERT_VALUE_EQ(serialization["_id"], Value(false));

    exclusion = makeExclusionProjectionWithDefaultIdExclusion(
        BSON("a" << false << "b" << false << "_id" << true));
    serialization = exclusion->serializeTransformation(boost::none);
    ASSERT_VALUE_EQ(serialization["a"], Value(false));
    ASSERT_VALUE_EQ(serialization["b"], Value(false));
    ASSERT_VALUE_EQ(serialization["_id"], Value(true));
}

TEST(ExclusionProjectionExecutionTest, ShouldNotAddAnyDependencies) {
    // An exclusion projection will cause the stage to return DepsTracker::State::SEE_NEXT, meaning
    // it doesn't strictly require any fields.
    //
    // For example, if our projection was {a: 0}, and a later stage requires the field "a", then "a"
    // will be added to the dependencies correctly. If a later stage doesn't need "a", then we don't
    // need to include the "a" in the dependencies of this projection, since it will just be ignored
    // later. If there are no later stages, then we will finish the dependency computation
    // cycle without full knowledge of which fields are needed, and thus include all the fields.
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(
        BSON("_id" << false << "a" << false << "b.c" << false << "x.y.z" << false));

    DepsTracker deps;
    exclusion->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 0UL);
    ASSERT_FALSE(deps.needWholeDocument);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST(ExclusionProjectionExecutionTest, ShouldReportExcludedFieldsAsModified) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(
        BSON("_id" << false << "a" << false << "b.c" << false));

    auto modifiedPaths = exclusion->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(modifiedPaths.paths.count("_id"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("a"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("b.c"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.size(), 3UL);
}

TEST(ExclusionProjectionExecutionTest,
     ShouldReportExcludedFieldsAsModifiedWhenSpecifiedAsNestedObj) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(
        BSON("a" << BSON("b" << false << "c" << BSON("d" << false))));

    auto modifiedPaths = exclusion->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(modifiedPaths.paths.count("a.b"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("a.c.d"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.size(), 2UL);
}

//
// Tests of execution of exclusions at the top level.
//

TEST(ExclusionProjectionExecutionTest, ShouldExcludeTopLevelField) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("a" << false));

    // More than one field in document.
    auto result = exclusion->applyTransformation(Document{{"a", 1}, {"b", 2}});
    auto expectedResult = Document{{"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the document.
    result = exclusion->applyTransformation(Document{{"a", 1}});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the document.
    result = exclusion->applyTransformation(Document{{"c", 1}});
    expectedResult = Document{{"c", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in the document.
    result = exclusion->applyTransformation(Document{});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldCoerceNumericsToBools) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON(
        "a" << Value(0) << "b" << Value(0LL) << "c" << Value(0.0) << "d" << Value(Decimal128(0))));

    auto result =
        exclusion->applyTransformation(Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}, {"c", 3}});
    auto expectedResult = Document{{"_id", "ID"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldPreserveOrderOfExistingFields) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("second" << false));
    auto result =
        exclusion->applyTransformation(Document{{"first", 0}, {"second", 1}, {"third", 2}});
    auto expectedResult = Document{{"first", 0}, {"third", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldImplicitlyIncludeId) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("a" << false));
    auto result = exclusion->applyTransformation(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"b", 2}, {"_id", "ID"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldExcludeIdIfExplicitlyExcluded) {
    auto exclusion =
        makeExclusionProjectionWithDefaultPolicies(BSON("a" << false << "_id" << false));
    auto result = exclusion->applyTransformation(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldExcludeIdAndKeepAllOtherFields) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("_id" << false));
    auto result = exclusion->applyTransformation(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"a", 1}, {"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Tests of execution of nested exclusions.
//

TEST(ExclusionProjectionExecutionTest, ShouldExcludeSubFieldsOfId) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(
        BSON("_id.x" << false << "_id" << BSON("y" << false)));
    auto result = exclusion->applyTransformation(
        Document{{"_id", Document{{"x", 1}, {"y", 2}, {"z", 3}}}, {"a", 1}});
    auto expectedResult = Document{{"_id", Document{{"z", 3}}}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldExcludeSimpleDottedFieldFromSubDoc) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("a.b" << false));

    // More than one field in sub document.
    auto result = exclusion->applyTransformation(Document{{"a", Document{{"b", 1}, {"c", 2}}}});
    auto expectedResult = Document{{"a", Document{{"c", 2}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the sub document.
    result = exclusion->applyTransformation(Document{{"a", Document{{"b", 1}}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the sub document.
    result = exclusion->applyTransformation(Document{{"a", Document{{"c", 1}}}});
    expectedResult = Document{{"a", Document{{"c", 1}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in sub document.
    result = exclusion->applyTransformation(Document{{"a", Document{}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldNotCreateSubDocIfDottedExcludedFieldDoesNotExist) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("sub.target" << false));

    // Should not add the path if it doesn't exist.
    auto result = exclusion->applyTransformation(Document{});
    auto expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should not replace non-documents with documents.
    result = exclusion->applyTransformation(Document{{"sub", "notADocument"_sd}});
    expectedResult = Document{{"sub", "notADocument"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldApplyDottedExclusionToEachElementInArray) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("a.b" << false));

    auto result = exclusion->applyTransformation(Document{{"a",
                                                           {1,
                                                            Document{},
                                                            Document{{"b", 1}},
                                                            Document{{"b", 1}, {"c", 2}},
                                                            vector<Value>{},
                                                            {1, Document{{"c", 1}, {"b", 1}}}}}});
    auto expectedResult = Document{{"a",
                                    {1,
                                     Document{},
                                     Document{},
                                     Document{{"c", 2}},
                                     vector<Value>{},
                                     {1, Document{{"c", 1}}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAllowMixedNestedAndDottedFields) {
    // Exclude all of "a.b", "a.c", "a.d", and "a.e".
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(
        BSON("a.b" << false << "a.c" << false << "a" << BSON("d" << false << "e" << false)));
    auto result = exclusion->applyTransformation(
        Document{{"a", Document{{"b", 1}, {"c", 2}, {"d", 3}, {"e", 4}, {"f", 5}}}});
    auto expectedResult = Document{{"a", Document{{"f", 5}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAlwaysKeepMetadataFromOriginalDoc) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("a" << false));

    MutableDocument inputDocBuilder(Document{{"_id", "ID"_sd}, {"a", 1}});
    inputDocBuilder.metadata().setRandVal(1.0);
    inputDocBuilder.metadata().setTextScore(10.0);
    Document inputDoc = inputDocBuilder.freeze();

    auto result = exclusion->applyTransformation(inputDoc);

    MutableDocument expectedDoc(Document{{"_id", "ID"_sd}});
    expectedDoc.copyMetaDataFrom(inputDoc);
    ASSERT_DOCUMENT_EQ(result, expectedDoc.freeze());
}

TEST(ExclusionProjectionExecutionTest, ShouldEvaluateMetaExpressions) {
    auto exclusion =
        makeExclusionProjectionWithDefaultPolicies(fromjson("{a: 0, c: {$meta: 'textScore'}, "
                                                            "d: {$meta: 'randVal'}, "
                                                            "e: {$meta: 'searchScore'}, "
                                                            "f: {$meta: 'searchHighlights'}, "
                                                            "g: {$meta: 'geoNearDistance'}, "
                                                            "h: {$meta: 'geoNearPoint'}, "
                                                            "i: {$meta: 'recordId'}, "
                                                            "j: {$meta: 'indexKey'}, "
                                                            "k: {$meta: 'sortKey'}, "
                                                            "l: {$meta: 'searchScoreDetails'}}"));

    MutableDocument inputDocBuilder(Document{{"a", 1}, {"b", 2}});
    inputDocBuilder.metadata().setTextScore(0.0);
    inputDocBuilder.metadata().setRandVal(1.0);
    inputDocBuilder.metadata().setSearchScore(2.0);
    inputDocBuilder.metadata().setSearchHighlights(Value{"foo"_sd});
    inputDocBuilder.metadata().setGeoNearDistance(3.0);
    inputDocBuilder.metadata().setGeoNearPoint(Value{BSON_ARRAY(4 << 5)});
    inputDocBuilder.metadata().setRecordId(RecordId{6});
    inputDocBuilder.metadata().setIndexKey(BSON("foo" << 7));
    inputDocBuilder.metadata().setSortKey(Value{Document{{"bar", 8}}}, true);
    inputDocBuilder.metadata().setSearchScoreDetails(BSON("scoreDetails"
                                                          << "foo"));
    Document inputDoc = inputDocBuilder.freeze();

    auto result = exclusion->applyTransformation(inputDoc);

    ASSERT_DOCUMENT_EQ(result,
                       Document{fromjson("{b: 2, c: 0.0, d: 1.0, e: 2.0, f: 'foo', g: 3.0, "
                                         "h: [4, 5], i: 6, j: {foo: 7}, k: [{bar: 8}],"
                                         "l: {scoreDetails: 'foo'}}")});
}

TEST(ExclusionProjectionExecutionTest, ShouldAddMetaExpressionsToDependencies) {
    auto exclusion =
        makeExclusionProjectionWithDefaultPolicies(fromjson("{a: 0, c: {$meta: 'textScore'}, "
                                                            "d: {$meta: 'randVal'}, "
                                                            "e: {$meta: 'searchScore'}, "
                                                            "f: {$meta: 'searchHighlights'}, "
                                                            "g: {$meta: 'geoNearDistance'}, "
                                                            "h: {$meta: 'geoNearPoint'}, "
                                                            "i: {$meta: 'recordId'}, "
                                                            "j: {$meta: 'indexKey'}, "
                                                            "k: {$meta: 'sortKey'}, "
                                                            "l: {$meta: 'searchScoreDetails'}}"));

    DepsTracker deps;
    exclusion->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 0UL);

    // We do not add the dependencies for searchScore, searchHighlights, or searchScoreDetails
    // because those values are not stored in the collection (or in mongod at all).
    ASSERT_FALSE(deps.metadataDeps()[DocumentMetadataFields::kSearchScore]);
    ASSERT_FALSE(deps.metadataDeps()[DocumentMetadataFields::kSearchHighlights]);
    ASSERT_FALSE(deps.metadataDeps()[DocumentMetadataFields::kSearchScoreDetails]);

    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kRandVal]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kGeoNearDist]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kGeoNearPoint]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kRecordId]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kIndexKey]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kSortKey]);
}

//
// _id exclusion policy.
//

TEST(ExclusionProjectionExecutionTest, ShouldIncludeIdByDefault) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("a" << false));

    auto result = exclusion->applyTransformation(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"_id", 2}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldExcludeIdWithExplicitPolicy) {
    auto exclusion = makeExclusionProjectionWithDefaultIdExclusion(BSON("a" << false));

    auto result = exclusion->applyTransformation(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldOverrideIncludePolicyWithExplicitExcludeIdSpec) {
    auto exclusion =
        makeExclusionProjectionWithDefaultPolicies(BSON("_id" << false << "a" << false));

    auto result = exclusion->applyTransformation(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldOverrideExcludePolicyWithExplicitIncludeIdSpec) {
    auto exclusion =
        makeExclusionProjectionWithDefaultIdExclusion(BSON("_id" << true << "a" << false));

    auto result = exclusion->applyTransformation(Document{{"_id", 2}, {"a", 3}, {"b", 4}});
    auto expectedResult = Document{{"_id", 2}, {"b", 4}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAllowExclusionOfIdSubfieldWithDefaultIncludePolicy) {
    auto exclusion =
        makeExclusionProjectionWithDefaultPolicies(BSON("_id.id1" << false << "a" << false));

    auto result = exclusion->applyTransformation(
        Document{{"_id", Document{{"id1", 1}, {"id2", 2}}}, {"a", 3}, {"b", 4}});
    auto expectedResult = Document{{"_id", Document{{"id2", 2}}}, {"b", 4}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAllowExclusionOfIdSubfieldWithDefaultExcludePolicy) {
    auto exclusion =
        makeExclusionProjectionWithDefaultIdExclusion(BSON("_id.id1" << false << "a" << false));

    auto result = exclusion->applyTransformation(
        Document{{"_id", Document{{"id1", 1}, {"id2", 2}}}, {"a", 3}, {"b", 4}});
    auto expectedResult = Document{{"_id", Document{{"id2", 2}}}, {"b", 4}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAllowLimitedDollarPrefixedFields) {
    auto exclusion = makeExclusionProjectionWithDefaultIdExclusion(
        BSON("$id" << false << "$db" << false << "$ref" << false << "$sortKey" << false));

    auto result = exclusion->applyTransformation(
        Document{{"$id", 5}, {"$db", 3}, {"$ref", 4}, {"$sortKey", 5}, {"someField", 6}});
    auto expectedResult = Document{{"someField", 6}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Nested array recursion.
//

TEST(ExclusionProjectionExecutionTest, ShouldRecurseNestedArraysByDefault) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies(BSON("a.b" << false));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => {a: [1, {c: 3}, [{c: 5}], {d: 6}]}
    auto result = exclusion->applyTransformation(Document{{"a",
                                                           {1,
                                                            Document{{"b", 2}, {"c", 3}},
                                                            vector{Document{{"b", 4}, {"c", 5}}},
                                                            Document{{"d", 6}}}}});

    auto expectedResult =
        Document{{"a", {1, Document{{"c", 3}}, vector{Document{{"c", 5}}}, Document{{"d", 6}}}}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldNotRecurseNestedArraysForNoRecursePolicy) {
    auto exclusion = makeExclusionProjectionWithNoArrayRecursion(BSON("a.b" << false));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => {a: [1, {c: 3}, [{b: 4, c: 5}], {d:
    // 6}]}
    auto result = exclusion->applyTransformation(Document{{"a",
                                                           {1,
                                                            Document{{"b", 2}, {"c", 3}},
                                                            vector{Document{{"b", 4}, {"c", 5}}},
                                                            Document{{"d", 6}}}}});

    auto expectedResult = Document{
        {"a", {1, Document{{"c", 3}}, vector{Document{{"b", 4}, {"c", 5}}}, Document{{"d", 6}}}}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldNotRetainNestedArraysIfNoRecursionNeeded) {
    auto exclusion = makeExclusionProjectionWithNoArrayRecursion(BSON("a" << false));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => {}
    const auto inputDoc = Document{{"a",
                                    {1,
                                     Document{{"b", 2}, {"c", 3}},
                                     vector{Document{{"b", 4}, {"c", 5}}},
                                     Document{{"d", 6}}}}};

    auto result = exclusion->applyTransformation(inputDoc);
    const auto expectedResult = Document{};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}
}  // namespace
}  // namespace mongo::projection_executor

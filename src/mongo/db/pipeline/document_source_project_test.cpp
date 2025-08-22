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

#include "mongo/db/pipeline/document_source_project.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using std::vector;

//
// DocumentSourceProject delegates much of its responsibilities to the ProjectionExecutor. Most of
// the functional tests are testing ProjectionExecutor directly. These are meant as simpler
// integration tests.
//

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using ProjectStageTest = AggregationContextFixture;
using UnsetTest = AggregationContextFixture;

TEST_F(ProjectStageTest, InclusionProjectionShouldRemoveUnspecifiedFields) {
    auto project = DocumentSourceProject::create(
        BSON("a" << true << "c" << BSON("d" << true)), getExpCtx(), "$project"_sd);
    auto projectStage = exec::agg::buildStage(project);
    auto mockStage =
        exec::agg::MockStage::createForTest("{_id: 0, a: 1, b: 1, c: {d: 1}}", getExpCtx());
    projectStage->setSource(mockStage.get());
    // The first result exists and is as expected.
    auto next = projectStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getInt());
    ASSERT(next.getDocument().getField("b").missing());
    // The _id field is included by default in the root document.
    ASSERT_EQUALS(0, next.getDocument().getField("_id").getInt());
    // The nested c.d inclusion.
    ASSERT_EQUALS(1, next.getDocument()["c"]["d"].getInt());
}

TEST_F(ProjectStageTest, ShouldOptimizeInnerExpressions) {
    auto project = DocumentSourceProject::create(
        BSON("a" << BSON("$and" << BSON_ARRAY(BSON("$const" << true)))),
        getExpCtx(),
        "$project"_sd);
    project->optimize();
    // The $and should have been replaced with its only argument.
    vector<Value> serializedArray;
    project->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(serializedArray[0].getDocument().toBson(),
                      fromjson("{$project: {_id: true, a: {$const: true}}}"));
}

TEST_F(ProjectStageTest, ShouldErrorOnNonObjectSpec) {
    BSONObj spec = BSON("$project" << "foo");
    BSONElement specElement = spec.firstElement();
    ASSERT_THROWS(DocumentSourceProject::createFromBson(specElement, getExpCtx()),
                  AssertionException);
}

/**
 * Basic sanity check that two documents can be projected correctly with a simple inclusion
 * projection.
 */
TEST_F(ProjectStageTest, InclusionShouldBeAbleToProcessMultipleDocuments) {
    auto project = DocumentSourceProject::create(BSON("a" << true), getExpCtx(), "$project"_sd);
    auto projectStage = exec::agg::buildStage(project);
    auto mockStage =
        exec::agg::MockStage::createForTest({"{a: 1, b: 2}", "{a: 3, b: 4}"}, getExpCtx());
    projectStage->setSource(mockStage.get());
    auto next = projectStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getInt());
    ASSERT(next.getDocument().getField("b").missing());

    next = projectStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(3, next.getDocument().getField("a").getInt());
    ASSERT(next.getDocument().getField("b").missing());

    ASSERT(projectStage->getNext().isEOF());
    ASSERT(projectStage->getNext().isEOF());
    ASSERT(projectStage->getNext().isEOF());
}

/**
 * Basic sanity check that two documents can be projected correctly with a simple inclusion
 * projection.
 */
TEST_F(ProjectStageTest, ExclusionShouldBeAbleToProcessMultipleDocuments) {
    auto project = DocumentSourceProject::create(BSON("a" << false), getExpCtx(), "$project"_sd);
    auto projectStage = exec::agg::buildStage(project);
    auto source =
        exec::agg::MockStage::createForTest({"{a: 1, b: 2}", "{a: 3, b: 4}"}, getExpCtx());
    projectStage->setSource(source.get());
    auto next = projectStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT(next.getDocument().getField("a").missing());
    ASSERT_EQUALS(2, next.getDocument().getField("b").getInt());

    next = projectStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT(next.getDocument().getField("a").missing());
    ASSERT_EQUALS(4, next.getDocument().getField("b").getInt());

    ASSERT(projectStage->getNext().isEOF());
    ASSERT(projectStage->getNext().isEOF());
    ASSERT(projectStage->getNext().isEOF());
}

TEST_F(ProjectStageTest, ShouldPropagatePauses) {
    auto project = DocumentSourceProject::create(BSON("a" << false), getExpCtx(), "$project"_sd);
    auto projectStage = exec::agg::buildStage(project);
    auto mockStage =
        exec::agg::MockStage::createForTest({Document(),
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document(),
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document(),
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            getExpCtx());
    projectStage->setSource(mockStage.get());

    ASSERT_TRUE(projectStage->getNext().isAdvanced());
    ASSERT_TRUE(projectStage->getNext().isPaused());
    ASSERT_TRUE(projectStage->getNext().isAdvanced());
    ASSERT_TRUE(projectStage->getNext().isPaused());
    ASSERT_TRUE(projectStage->getNext().isAdvanced());
    ASSERT_TRUE(projectStage->getNext().isPaused());

    ASSERT(projectStage->getNext().isEOF());
    ASSERT(projectStage->getNext().isEOF());
    ASSERT(projectStage->getNext().isEOF());
}

TEST_F(ProjectStageTest, InclusionShouldAddDependenciesOfIncludedAndComputedFields) {
    auto project = DocumentSourceProject::create(
        fromjson("{a: true, x: '$b', y: {$and: ['$c','$d']}, z: {$meta: 'textScore'}}"),
        getExpCtx(),
        "$project"_sd);
    DepsTracker dependencies(DepsTracker::kOnlyTextScore);
    ASSERT_EQUALS(DepsTracker::State::EXHAUSTIVE_FIELDS, project->getDependencies(&dependencies));
    ASSERT_EQUALS(5U, dependencies.fields.size());

    // Implicit _id dependency.
    ASSERT_EQUALS(1U, dependencies.fields.count("_id"));

    // Inclusion dependency.
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));

    // Field path expression dependency.
    ASSERT_EQUALS(1U, dependencies.fields.count("b"));

    // Nested expression dependencies.
    ASSERT_EQUALS(1U, dependencies.fields.count("c"));
    ASSERT_EQUALS(1U, dependencies.fields.count("d"));
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(true, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(ProjectStageTest, ExclusionShouldNotAddDependencies) {
    auto project = DocumentSourceProject::create(
        fromjson("{a: false, 'b.c': false}"), getExpCtx(), "$project"_sd);

    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, project->getDependencies(&dependencies));

    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(ProjectStageTest, InclusionProjectionReportsIncludedPathsFromGetModifiedPaths) {
    auto project = DocumentSourceProject::create(
        fromjson("{a: true, 'b.c': {d: true}, e: {f: {g: true}}, h: {i: {$literal: true}}}"),
        getExpCtx(),
        "$project"_sd);

    auto modifiedPaths = project->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    ASSERT_EQUALS(4U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("_id"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("a"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("b.c.d"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("e.f.g"));
}

TEST_F(ProjectStageTest, InclusionProjectionReportsIncludedPathsButExcludesId) {
    auto project = DocumentSourceProject::create(
        fromjson("{_id: false, 'b.c': {d: true}, e: {f: {g: true}}, h: {i: {$literal: true}}}"),
        getExpCtx(),
        "$project"_sd);

    auto modifiedPaths = project->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    ASSERT_EQUALS(2U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("b.c.d"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("e.f.g"));
}

TEST_F(ProjectStageTest, ExclusionProjectionReportsExcludedPathsAsModifiedPaths) {
    auto project = DocumentSourceProject::create(
        fromjson("{a: false, 'b.c': {d: false}, e: {f: {g: false}}}"), getExpCtx(), "$project"_sd);

    auto modifiedPaths = project->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQUALS(3U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("a"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("b.c.d"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("e.f.g"));
}

TEST_F(ProjectStageTest, ExclusionProjectionReportsExcludedPathsWithIdExclusion) {
    auto project = DocumentSourceProject::create(
        fromjson("{_id: false, 'b.c': {d: false}, e: {f: {g: false}}}"),
        getExpCtx(),
        "$project"_sd);

    auto modifiedPaths = project->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQUALS(3U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("_id"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("b.c.d"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("e.f.g"));
}

TEST_F(ProjectStageTest, CanUseRemoveSystemVariableToConditionallyExcludeProjectedField) {
    auto project = DocumentSourceProject::create(
        fromjson("{a: 1, b: {$cond: [{$eq: ['$b', 4]}, '$$REMOVE', '$b']}}"),
        getExpCtx(),
        "$project"_sd);
    auto projectStage = exec::agg::buildStage(project);
    auto source =
        exec::agg::MockStage::createForTest({"{a: 2, b: 2}", "{a: 3, b: 4}"}, getExpCtx());
    projectStage->setSource(source.get());
    auto next = projectStage->getNext();
    ASSERT(next.isAdvanced());
    Document expected{{"a", 2}, {"b", 2}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    next = projectStage->getNext();
    ASSERT(next.isAdvanced());
    expected = Document{{"a", 3}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    ASSERT(projectStage->getNext().isEOF());
}

TEST_F(ProjectStageTest, ProjectionCorrectlyReportsRenamesForwards) {
    auto project =
        DocumentSourceProject::create(fromjson("{'renamedB' : '$b'}"), getExpCtx(), "$project"_sd);
    auto renames =
        semantic_analysis::renamedPaths({"b"}, *project, semantic_analysis::Direction::kForward);
    // renamedPaths should return a mapping of old name->new name for each path in interestingPaths
    // if the paths are all unmodified (but possibly renamed). Because path b is preserved, but
    // renamed (to renamedB) we expect a renamedPaths to return a mapping from b->renamedB.
    auto single_rename = renames->extract("b");
    ASSERT_FALSE(single_rename.empty());
    ASSERT_EQUALS(single_rename.mapped(), "renamedB");
    ASSERT_TRUE(renames->empty());
}

TEST_F(ProjectStageTest, ProjectionRenameModifiesDestination) {
    auto project = DocumentSourceProject::create(
        fromjson("{'somePath' : '$otherField'}"), getExpCtx(), "$project"_sd);

    // Forwards: "somePath" is _not_ preserved by this projection - any existing value has been
    // overwritten.
    auto renames = semantic_analysis::renamedPaths(
        {"somePath"}, *project, semantic_analysis::Direction::kForward);
    ASSERT_FALSE(renames.has_value());

    // Forwards: "otherField" _is_ preserved by this projection, and is renamed to "somePath".
    renames = semantic_analysis::renamedPaths(
        {"otherField"}, *project, semantic_analysis::Direction::kForward);
    ASSERT_TRUE(renames.has_value());
    ASSERT_EQUALS(renames->at("otherField"), "somePath");

    // Backwards: "somePath" is the result of a rename, so traversing backwards should map to the
    // previous name.
    renames = semantic_analysis::renamedPaths(
        {"somePath"}, *project, semantic_analysis::Direction::kBackward);
    ASSERT_TRUE(renames.has_value());
    ASSERT_EQUALS(renames->at("somePath"), "otherField");

    // Backwards: As this is a _projection_, and "otherField" has not explicitly been preserved, it
    // no longer exists after this stage.
    renames = semantic_analysis::renamedPaths(
        {"otherField"}, *project, semantic_analysis::Direction::kBackward);
    ASSERT_FALSE(renames.has_value());
}

TEST_F(ProjectStageTest, ProjectionCorrectlyReportsRenamesBackwards) {
    auto project =
        DocumentSourceProject::create(fromjson("{'renamedB' : '$b'}"), getExpCtx(), "$project"_sd);
    auto renames = semantic_analysis::renamedPaths(
        {"renamedB"}, *project, semantic_analysis::Direction::kBackward);
    auto single_rename = renames->extract("renamedB");
    ASSERT_FALSE(single_rename.empty());
    ASSERT_EQUALS(single_rename.mapped(), "b");
    ASSERT_TRUE(renames->empty());
}

/**
 * Creates BSON for a DocumentSourceProject that represents projecting a new computed field nested
 * 'depth' levels deep.
 */
BSONObj makeProjectForNestedDocument(size_t depth) {
    ASSERT_GTE(depth, 2U);
    StringBuilder builder;
    builder << "a";
    for (size_t i = 0; i < depth - 1; ++i) {
        builder << ".a";
    }
    return BSON(builder.str() << BSON("$literal" << 1));
}

TEST_F(ProjectStageTest, CanAddNestedDocumentExactlyAtDepthLimit) {
    auto project = DocumentSourceProject::create(
        makeProjectForNestedDocument(BSONDepth::getMaxAllowableDepth()),
        getExpCtx(),
        "$project"_sd);
    auto projectStage = exec::agg::buildStage(project);
    auto mock = exec::agg::MockStage::createForTest(Document{{"_id", 1}}, getExpCtx());
    projectStage->setSource(mock.get());

    auto next = projectStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
}

TEST_F(ProjectStageTest, CannotAddNestedDocumentExceedingDepthLimit) {
    ASSERT_THROWS_CODE(DocumentSourceProject::create(
                           makeProjectForNestedDocument(BSONDepth::getMaxAllowableDepth() + 1),
                           getExpCtx(),
                           "$project"_sd),
                       AssertionException,
                       ErrorCodes::Overflow);
}

/**
 * A default redaction strategy that generates easy to check results for testing purposes.
 */
std::string transformIdentifiersForTest(StringData s) {
    return str::stream() << "HASH<" << s << ">";
}

TEST_F(ProjectStageTest, ShapifyAndRedact) {
    auto inclusionProject = DocumentSourceProject::create(
        fromjson("{a: true, x: '$b', y: {$and: ['$c','$d']}, z: {$meta: 'textScore'}}"),
        getExpCtx(),
        "$project"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$project": {
                "HASH<_id>": true,
                "HASH<a>": true,
                "HASH<x>": "$HASH<b>",
                "HASH<y>": {
                    "$and": [ "$HASH<c>", "$HASH<d>" ]
                    },
                "HASH<z>": { "$meta": "textScore" }
            }
        })",
        redact(*inclusionProject));

    auto exclusionProject = DocumentSourceProject::create(
        fromjson("{a: false, 'b.c': false}"), getExpCtx(), "$project"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$project": {
                "HASH<a>": false,
                "HASH<b>": {
                    "HASH<c>": false },
                "HASH<_id>": true }
        })",
        redact(*exclusionProject));
}

TEST_F(UnsetTest, AcceptsValidUnsetSpecWithArray) {
    auto spec = BSON("$unset" << BSON_ARRAY("a" << "b"
                                                << "c.d"));
    auto specElement = spec.firstElement();
    auto stage = DocumentSourceProject::createFromBson(specElement, getExpCtx());
    ASSERT(stage);
}

TEST_F(UnsetTest, AcceptsValidUnsetSpecWithSingleString) {
    auto spec = BSON("$unset" << "a");
    auto specElement = spec.firstElement();
    auto stage = DocumentSourceProject::createFromBson(specElement, getExpCtx());
    ASSERT(stage);
}

TEST_F(UnsetTest, RejectsUnsetSpecWhichIsNotAnArrayOrString) {
    auto spec = BSON("$unset" << 1);
    auto specElement = spec.firstElement();
    ASSERT_THROWS_CODE(
        DocumentSourceProject::createFromBson(specElement, getExpCtx()), AssertionException, 31002);
}

TEST_F(UnsetTest, RejectsUnsetSpecWithEmptyArray) {
    auto spec = BSON("$unset" << BSONArray());
    auto specElement = spec.firstElement();
    ASSERT_THROWS_CODE(
        DocumentSourceProject::createFromBson(specElement, getExpCtx()), AssertionException, 31119);
}

TEST_F(UnsetTest, RejectsUnsetSpecWithArrayContainingAnyNonStringValue) {
    auto spec = BSON("$unset" << BSON_ARRAY("a" << 2 << "b"));
    auto specElement = spec.firstElement();
    ASSERT_THROWS_CODE(
        DocumentSourceProject::createFromBson(specElement, getExpCtx()), AssertionException, 31120);
}

TEST_F(UnsetTest, UnsetSingleField) {
    auto updateDoc = BSON("$unset" << BSON_ARRAY("a"));
    auto unsetSource = DocumentSourceProject::createFromBson(updateDoc.firstElement(), getExpCtx());
    auto unsetStage = exec::agg::buildStage(unsetSource);
    auto mockStage = exec::agg::MockStage::createForTest({"{a: 10, b: 20}"}, getExpCtx());
    unsetStage->setSource(mockStage.get());
    auto next = unsetStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT(next.getDocument().getField("a").missing());
    ASSERT_EQUALS(20, next.getDocument().getField("b").getInt());

    ASSERT(unsetStage->getNext().isEOF());
}

TEST_F(UnsetTest, UnsetMultipleFields) {
    auto updateDoc = BSON("$unset" << BSON_ARRAY("a" << "b.c"
                                                     << "d.e"));
    auto unsetSource = DocumentSourceProject::createFromBson(updateDoc.firstElement(), getExpCtx());
    auto unsetStage = exec::agg::buildStage(unsetSource);
    auto source = exec::agg::MockStage::createForTest({"{a: 10, b: {c: 20}, d: [{e: 30, f: 40}]}"},
                                                      getExpCtx());
    unsetStage->setSource(source.get());
    auto next = unsetStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_BSONOBJ_EQ(next.getDocument().toBson(),
                      BSON("b" << BSONObj() << "d" << BSON_ARRAY(BSON("f" << 40))));

    ASSERT(unsetStage->getNext().isEOF());
}

TEST_F(UnsetTest, UnsetShouldBeAbleToProcessMultipleDocuments) {
    auto updateDoc = BSON("$unset" << BSON_ARRAY("a"));
    auto unsetSource = DocumentSourceProject::createFromBson(updateDoc.firstElement(), getExpCtx());
    auto unsetStage = exec::agg::buildStage(unsetSource);
    auto mockStage =
        exec::agg::MockStage::createForTest({"{a: 1, b: 2}", "{a: 3, b: 4}"}, getExpCtx());
    unsetStage->setSource(mockStage.get());
    auto next = unsetStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT(next.getDocument().getField("a").missing());
    ASSERT_EQUALS(2, next.getDocument().getField("b").getInt());

    next = unsetStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT(next.getDocument().getField("a").missing());
    ASSERT_EQUALS(4, next.getDocument().getField("b").getInt());

    ASSERT(unsetStage->getNext().isEOF());
}

TEST_F(UnsetTest, UnsetSerializesToProject) {
    auto updateDoc = BSON("$unset" << BSON_ARRAY("b.c"));
    auto unsetStage = DocumentSourceProject::createFromBson(updateDoc.firstElement(), getExpCtx());

    vector<Value> serializedArray;
    unsetStage->serializeToArray(serializedArray);
    auto serializedUnsetStage = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedUnsetStage, fromjson("{$project: {b: {c: false}, _id: true}}"));
    auto projectStage =
        DocumentSourceProject::createFromBson(serializedUnsetStage.firstElement(), getExpCtx());
    projectStage->serializeToArray(serializedArray);
    auto serializedProjectStage = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedUnsetStage, serializedProjectStage);
}

TEST_F(UnsetTest, UnsetShouldNotAddDependencies) {
    auto updateDoc = BSON("$unset" << BSON_ARRAY("a" << "b.c"));
    auto unsetStage = DocumentSourceProject::createFromBson(updateDoc.firstElement(), getExpCtx());

    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, unsetStage->getDependencies(&dependencies));

    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(UnsetTest, UnsetReportsExcludedPathsAsModifiedPaths) {
    auto updateDoc = BSON("$unset" << BSON_ARRAY("a" << "b.c.d"
                                                     << "e.f.g"));
    auto unsetStage = DocumentSourceProject::createFromBson(updateDoc.firstElement(), getExpCtx());

    auto modifiedPaths = unsetStage->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQUALS(3U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("a"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("b.c.d"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("e.f.g"));
}
}  // namespace
}  // namespace mongo

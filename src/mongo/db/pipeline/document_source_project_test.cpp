/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using boost::intrusive_ptr;
using std::vector;

//
// DocumentSourceProject delegates much of its responsibilities to the ParsedAggregationProjection.
// Most of the functional tests are testing ParsedAggregationProjection directly. These are meant as
// simpler integration tests.
//

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using ProjectStageTest = AggregationContextFixture;

TEST_F(ProjectStageTest, InclusionProjectionShouldRemoveUnspecifiedFields) {
    auto project =
        DocumentSourceProject::create(BSON("a" << true << "c" << BSON("d" << true)), getExpCtx());
    auto source = DocumentSourceMock::create("{_id: 0, a: 1, b: 1, c: {d: 1}}");
    project->setSource(source.get());
    // The first result exists and is as expected.
    auto next = project->getNext();
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
        BSON("a" << BSON("$and" << BSON_ARRAY(BSON("$const" << true)))), getExpCtx());
    project->optimize();
    // The $and should have been replaced with its only argument.
    vector<Value> serializedArray;
    project->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(serializedArray[0].getDocument().toBson(),
                      fromjson("{$project: {_id: true, a: {$const: true}}}"));
}

TEST_F(ProjectStageTest, ShouldErrorOnNonObjectSpec) {
    BSONObj spec = BSON("$project"
                        << "foo");
    BSONElement specElement = spec.firstElement();
    ASSERT_THROWS(DocumentSourceProject::createFromBson(specElement, getExpCtx()), UserException);
}

/**
 * Basic sanity check that two documents can be projected correctly with a simple inclusion
 * projection.
 */
TEST_F(ProjectStageTest, InclusionShouldBeAbleToProcessMultipleDocuments) {
    auto project = DocumentSourceProject::create(BSON("a" << true), getExpCtx());
    auto source = DocumentSourceMock::create({"{a: 1, b: 2}", "{a: 3, b: 4}"});
    project->setSource(source.get());
    auto next = project->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(1, next.getDocument().getField("a").getInt());
    ASSERT(next.getDocument().getField("b").missing());

    next = project->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQUALS(3, next.getDocument().getField("a").getInt());
    ASSERT(next.getDocument().getField("b").missing());

    ASSERT(project->getNext().isEOF());
    ASSERT(project->getNext().isEOF());
    ASSERT(project->getNext().isEOF());
}

/**
 * Basic sanity check that two documents can be projected correctly with a simple inclusion
 * projection.
 */
TEST_F(ProjectStageTest, ExclusionShouldBeAbleToProcessMultipleDocuments) {
    auto project = DocumentSourceProject::create(BSON("a" << false), getExpCtx());
    auto source = DocumentSourceMock::create({"{a: 1, b: 2}", "{a: 3, b: 4}"});
    project->setSource(source.get());
    auto next = project->getNext();
    ASSERT(next.isAdvanced());
    ASSERT(next.getDocument().getField("a").missing());
    ASSERT_EQUALS(2, next.getDocument().getField("b").getInt());

    next = project->getNext();
    ASSERT(next.isAdvanced());
    ASSERT(next.getDocument().getField("a").missing());
    ASSERT_EQUALS(4, next.getDocument().getField("b").getInt());

    ASSERT(project->getNext().isEOF());
    ASSERT(project->getNext().isEOF());
    ASSERT(project->getNext().isEOF());
}

TEST_F(ProjectStageTest, ShouldPropagatePauses) {
    auto project = DocumentSourceProject::create(BSON("a" << false), getExpCtx());
    auto source = DocumentSourceMock::create({Document(),
                                              DocumentSource::GetNextResult::makePauseExecution(),
                                              Document(),
                                              DocumentSource::GetNextResult::makePauseExecution(),
                                              Document(),
                                              DocumentSource::GetNextResult::makePauseExecution()});
    project->setSource(source.get());

    ASSERT_TRUE(project->getNext().isAdvanced());
    ASSERT_TRUE(project->getNext().isPaused());
    ASSERT_TRUE(project->getNext().isAdvanced());
    ASSERT_TRUE(project->getNext().isPaused());
    ASSERT_TRUE(project->getNext().isAdvanced());
    ASSERT_TRUE(project->getNext().isPaused());

    ASSERT(project->getNext().isEOF());
    ASSERT(project->getNext().isEOF());
    ASSERT(project->getNext().isEOF());
}

TEST_F(ProjectStageTest, InclusionShouldAddDependenciesOfIncludedAndComputedFields) {
    auto project = DocumentSourceProject::create(
        fromjson("{a: true, x: '$b', y: {$and: ['$c','$d']}, z: {$meta: 'textScore'}}"),
        getExpCtx());
    DepsTracker dependencies(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_EQUALS(DocumentSource::EXHAUSTIVE_FIELDS, project->getDependencies(&dependencies));
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
    ASSERT_EQUALS(true, dependencies.getNeedTextScore());
}

TEST_F(ProjectStageTest, ExclusionShouldNotAddDependencies) {
    auto project = DocumentSourceProject::create(fromjson("{a: false, 'b.c': false}"), getExpCtx());

    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, project->getDependencies(&dependencies));

    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(ProjectStageTest, InclusionProjectionReportsIncludedPathsFromGetModifiedPaths) {
    auto project = DocumentSourceProject::create(
        fromjson("{a: true, 'b.c': {d: true}, e: {f: {g: true}}, h: {i: {$literal: true}}}"),
        getExpCtx());

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
        getExpCtx());

    auto modifiedPaths = project->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    ASSERT_EQUALS(2U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("b.c.d"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("e.f.g"));
}

TEST_F(ProjectStageTest, ExclusionProjectionReportsExcludedPathsAsModifiedPaths) {
    auto project = DocumentSourceProject::create(
        fromjson("{a: false, 'b.c': {d: false}, e: {f: {g: false}}}"), getExpCtx());

    auto modifiedPaths = project->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQUALS(3U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("a"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("b.c.d"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("e.f.g"));
}

TEST_F(ProjectStageTest, ExclusionProjectionReportsExcludedPathsWithIdExclusion) {
    auto project = DocumentSourceProject::create(
        fromjson("{_id: false, 'b.c': {d: false}, e: {f: {g: false}}}"), getExpCtx());

    auto modifiedPaths = project->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQUALS(3U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("_id"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("b.c.d"));
    ASSERT_EQUALS(1U, modifiedPaths.paths.count("e.f.g"));
}

}  // namespace
}  // namespace mongo

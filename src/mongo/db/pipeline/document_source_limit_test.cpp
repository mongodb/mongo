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

#include "mongo/db/pipeline/document_source_limit.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/limit_stage.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <iterator>
#include <list>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceLimitTest = AggregationContextFixture;

boost::intrusive_ptr<mongo::exec::agg::LimitStage> createForTests(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, long long limit) {
    return make_intrusive<mongo::exec::agg::LimitStage>("$limit", expCtx, limit);
}

TEST_F(DocumentSourceLimitTest, ShouldDisposeSourceWhenLimitIsReached) {
    auto stage = exec::agg::MockStage::createForTest({"{a: 1}", "{a: 2}"}, getExpCtx());
    auto limitStage = createForTests(getExpCtx(), 1);
    limitStage->setSource(stage.get());
    // The limitStage's result is as expected.
    auto next = limitStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_VALUE_EQ(Value(1), next.getDocument().getField("a"));
    // The limitStage is exhausted.
    ASSERT(limitStage->getNext().isEOF());
    // The stage has been disposed
    ASSERT_TRUE(stage->isDisposed);
}

TEST_F(DocumentSourceLimitTest, ShouldNotBeAbleToLimitToZeroDocuments) {
    auto source = DocumentSourceMock::createForTest({"{a: 1}", "{a: 2}"}, getExpCtx());
    ASSERT_THROWS_CODE(DocumentSourceLimit::create(getExpCtx(), 0), AssertionException, 15958);
}

TEST_F(DocumentSourceLimitTest, ShouldRejectUserLimitOfZero) {
    ASSERT_THROWS_CODE(
        DocumentSourceLimit::createFromBson(BSON("$limit" << 0).firstElement(), getExpCtx()),
        AssertionException,
        15958);

    // A $limit with size 1 should be okay.
    auto shouldNotThrow =
        DocumentSourceLimit::createFromBson(BSON("$limit" << 1).firstElement(), getExpCtx());
    ASSERT(dynamic_cast<DocumentSourceLimit*>(shouldNotThrow.get()));
}

TEST_F(DocumentSourceLimitTest, TwoLimitStagesShouldCombineIntoOne) {
    DocumentSourceContainer container;
    auto firstLimit = DocumentSourceLimit::create(getExpCtx(), 10);
    auto secondLimit = DocumentSourceLimit::create(getExpCtx(), 5);

    container.push_back(firstLimit);
    container.push_back(secondLimit);

    firstLimit->optimizeAt(container.begin(), &container);
    ASSERT_EQUALS(5, firstLimit->getLimit());
    ASSERT_EQUALS(1U, container.size());
}

TEST_F(DocumentSourceLimitTest, DoesNotPushProjectBeforeSelf) {
    DocumentSourceContainer container;
    auto limit = DocumentSourceLimit::create(getExpCtx(), 10);
    auto project =
        DocumentSourceProject::create(BSON("fullDocument" << true), getExpCtx(), "$project"_sd);

    container.push_back(limit);
    container.push_back(project);

    limit->optimizeAt(container.begin(), &container);

    ASSERT_EQUALS(2U, container.size());
    ASSERT(dynamic_cast<DocumentSourceLimit*>(container.begin()->get()));
    ASSERT(dynamic_cast<DocumentSourceSingleDocumentTransformation*>(
        std::next(container.begin())->get()));
}

TEST_F(DocumentSourceLimitTest, DisposeShouldCascadeAllTheWayToSource) {
    auto stage = exec::agg::MockStage::createForTest({"{a: 1}", "{a: 1}"}, getExpCtx());

    // Create a DocumentSourceMatch.
    BSONObj spec = BSON("$match" << BSON("a" << 1));
    BSONElement specElement = spec.firstElement();
    auto match = DocumentSourceMatch::createFromBson(specElement, getExpCtx());
    auto matchStage = exec::agg::buildStage(match);
    matchStage->setSource(stage.get());

    auto limitStage = createForTests(getExpCtx(), 1);
    limitStage->setSource(matchStage.get());
    // The limitStage is not exhausted.
    auto next = limitStage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_VALUE_EQ(Value(1), next.getDocument().getField("a"));
    // The limitStage is exhausted.
    ASSERT(limitStage->getNext().isEOF());
    ASSERT_TRUE(stage->isDisposed);
}

TEST_F(DocumentSourceLimitTest, ShouldNotIntroduceAnyDependencies) {
    auto limit = DocumentSourceLimit::create(getExpCtx(), 1);
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, limit->getDependencies(&dependencies));
    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceLimitTest, ShouldPropagatePauses) {
    auto limitStage = createForTests(getExpCtx(), 2);
    auto mock =
        exec::agg::MockStage::createForTest({DocumentSource::GetNextResult::makePauseExecution(),
                                             Document(),
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document(),
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document()},
                                            getExpCtx());
    limitStage->setSource(mock.get());

    ASSERT_TRUE(limitStage->getNext().isPaused());
    ASSERT_TRUE(limitStage->getNext().isAdvanced());
    ASSERT_TRUE(limitStage->getNext().isPaused());
    ASSERT_TRUE(limitStage->getNext().isAdvanced());

    // We've reached the limit.
    ASSERT_TRUE(mock->isDisposed);
    ASSERT_TRUE(limitStage->getNext().isEOF());
    ASSERT_TRUE(limitStage->getNext().isEOF());
    ASSERT_TRUE(limitStage->getNext().isEOF());
}

TEST_F(DocumentSourceLimitTest, RedactsCorrectly) {
    auto limit = DocumentSourceLimit::create(getExpCtx(), 2);
    ASSERT_VALUE_EQ_AUTO(  // NOLINT
        "{ $limit: \"?number\" }",
        redact(*limit));
}

}  // namespace
}  // namespace mongo

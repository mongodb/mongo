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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceLimitTest = AggregationContextFixture;

TEST_F(DocumentSourceLimitTest, ShouldDisposeSourceWhenLimitIsReached) {
    auto source = DocumentSourceMock::createForTest({"{a: 1}", "{a: 2}"});
    auto limit = DocumentSourceLimit::create(getExpCtx(), 1);
    limit->setSource(source.get());
    // The limit's result is as expected.
    auto next = limit->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_VALUE_EQ(Value(1), next.getDocument().getField("a"));
    // The limit is exhausted.
    ASSERT(limit->getNext().isEOF());
    // The source has been disposed
    ASSERT_TRUE(source->isDisposed);
}

TEST_F(DocumentSourceLimitTest, ShouldNotBeAbleToLimitToZeroDocuments) {
    auto source = DocumentSourceMock::createForTest({"{a: 1}", "{a: 2}"});
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
    Pipeline::SourceContainer container;
    auto firstLimit = DocumentSourceLimit::create(getExpCtx(), 10);
    auto secondLimit = DocumentSourceLimit::create(getExpCtx(), 5);

    container.push_back(firstLimit);
    container.push_back(secondLimit);

    firstLimit->optimizeAt(container.begin(), &container);
    ASSERT_EQUALS(5, firstLimit->getLimit());
    ASSERT_EQUALS(1U, container.size());
}

TEST_F(DocumentSourceLimitTest, DisposeShouldCascadeAllTheWayToSource) {
    auto source = DocumentSourceMock::createForTest({"{a: 1}", "{a: 1}"});

    // Create a DocumentSourceMatch.
    BSONObj spec = BSON("$match" << BSON("a" << 1));
    BSONElement specElement = spec.firstElement();
    auto match = DocumentSourceMatch::createFromBson(specElement, getExpCtx());
    match->setSource(source.get());

    auto limit = DocumentSourceLimit::create(getExpCtx(), 1);
    limit->setSource(match.get());
    // The limit is not exhauted.
    auto next = limit->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_VALUE_EQ(Value(1), next.getDocument().getField("a"));
    // The limit is exhausted.
    ASSERT(limit->getNext().isEOF());
    ASSERT_TRUE(source->isDisposed);
}

TEST_F(DocumentSourceLimitTest, ShouldNotIntroduceAnyDependencies) {
    auto limit = DocumentSourceLimit::create(getExpCtx(), 1);
    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, limit->getDependencies(&dependencies));
    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));
}

TEST_F(DocumentSourceLimitTest, ShouldPropagatePauses) {
    auto limit = DocumentSourceLimit::create(getExpCtx(), 2);
    auto mock =
        DocumentSourceMock::createForTest({DocumentSource::GetNextResult::makePauseExecution(),
                                           Document(),
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document(),
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document()});
    limit->setSource(mock.get());

    ASSERT_TRUE(limit->getNext().isPaused());
    ASSERT_TRUE(limit->getNext().isAdvanced());
    ASSERT_TRUE(limit->getNext().isPaused());
    ASSERT_TRUE(limit->getNext().isAdvanced());

    // We've reached the limit.
    ASSERT_TRUE(mock->isDisposed);
    ASSERT_TRUE(limit->getNext().isEOF());
    ASSERT_TRUE(limit->getNext().isEOF());
    ASSERT_TRUE(limit->getNext().isEOF());
}

}  // namespace
}  // namespace mongo

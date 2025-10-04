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

#include "mongo/db/pipeline/document_source_skip.h"

#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/agg/skip_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <limits>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceSkipTest = AggregationContextFixture;

boost::intrusive_ptr<mongo::exec::agg::SkipStage> createForTests(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long nToSkip) {
    return make_intrusive<mongo::exec::agg::SkipStage>("$skip"_sd, pExpCtx, nToSkip);
}

TEST_F(DocumentSourceSkipTest, ShouldPropagatePauses) {
    auto skipStage = createForTests(getExpCtx(), 2);
    auto mock =
        exec::agg::MockStage::createForTest({Document(),
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document(),
                                             Document(),
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            getExpCtx());
    skipStage->setSource(mock.get());

    // Skip the first document.
    ASSERT_TRUE(skipStage->getNext().isPaused());

    // Skip one more, then advance.
    ASSERT_TRUE(skipStage->getNext().isAdvanced());

    ASSERT_TRUE(skipStage->getNext().isPaused());
    ASSERT_TRUE(skipStage->getNext().isPaused());

    ASSERT_TRUE(skipStage->getNext().isEOF());
    ASSERT_TRUE(skipStage->getNext().isEOF());
    ASSERT_TRUE(skipStage->getNext().isEOF());
}

TEST_F(DocumentSourceSkipTest, SkipsChainedTogetherShouldNotOverFlowWhenOptimizing) {
    // $skip should not optimize if combining the two values of skips would overflow a long long.
    auto skipShort = DocumentSourceSkip::create(getExpCtx(), 1);
    auto skipLong = DocumentSourceSkip::create(getExpCtx(), std::numeric_limits<long long>::max());
    DocumentSourceContainer overflowContainer;
    overflowContainer.push_back(skipShort);
    overflowContainer.push_back(skipLong);
    skipShort->doOptimizeAt(overflowContainer.begin(), &overflowContainer);
    ASSERT_EQUALS(overflowContainer.size(), 2U);
    ASSERT_EQUALS(skipShort->getSkip(), 1U);
    ASSERT_EQUALS(skipLong->getSkip(), std::numeric_limits<long long>::max());

    // $skip should not optimize if both skips are max values for long long.
    auto firstMaxSkip =
        DocumentSourceSkip::create(getExpCtx(), std::numeric_limits<long long>::max());
    auto secondMaxSkip =
        DocumentSourceSkip::create(getExpCtx(), std::numeric_limits<long long>::max());
    DocumentSourceContainer doubleMaxContainer;
    doubleMaxContainer.push_back(firstMaxSkip);
    doubleMaxContainer.push_back(secondMaxSkip);
    firstMaxSkip->doOptimizeAt(doubleMaxContainer.begin(), &doubleMaxContainer);
    ASSERT_EQUALS(doubleMaxContainer.size(), 2U);
    ASSERT_EQUALS(firstMaxSkip->getSkip(), std::numeric_limits<long long>::max());
    ASSERT_EQUALS(secondMaxSkip->getSkip(), std::numeric_limits<long long>::max());

    // $skip should optimize if the two skips will not overflow a long long when combined.
    auto skipFirst = DocumentSourceSkip::create(getExpCtx(), 1);
    auto skipSecond = DocumentSourceSkip::create(getExpCtx(), 1);
    DocumentSourceContainer containerOptimized;
    containerOptimized.push_back(skipFirst);
    containerOptimized.push_back(skipSecond);
    skipFirst->doOptimizeAt(containerOptimized.begin(), &containerOptimized);
    ASSERT_EQUALS(containerOptimized.size(), 1U);
    ASSERT_EQUALS(skipFirst->getSkip(), 2);
}

TEST_F(DocumentSourceSkipTest, Redaction) {
    auto stage = DocumentSourceSkip::create(getExpCtx(), 1337);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$skip":"?number"})",
        redact(*stage));
}
}  // namespace
}  // namespace mongo

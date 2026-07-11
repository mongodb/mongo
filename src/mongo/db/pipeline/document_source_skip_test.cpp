// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
using namespace std::literals::string_view_literals;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceSkipTest = AggregationContextFixture;

boost::intrusive_ptr<mongo::exec::agg::SkipStage> createForTests(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long nToSkip) {
    return make_intrusive<mongo::exec::agg::SkipStage>("$skip"sv, pExpCtx, nToSkip);
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
    exec::agg::MockStage::setSource_forTest(skipStage, mock.get());

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
    skipShort->optimizeAt(overflowContainer.begin(), &overflowContainer);
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
    firstMaxSkip->optimizeAt(doubleMaxContainer.begin(), &doubleMaxContainer);
    ASSERT_EQUALS(doubleMaxContainer.size(), 2U);
    ASSERT_EQUALS(firstMaxSkip->getSkip(), std::numeric_limits<long long>::max());
    ASSERT_EQUALS(secondMaxSkip->getSkip(), std::numeric_limits<long long>::max());

    // $skip should optimize if the two skips will not overflow a long long when combined.
    auto skipFirst = DocumentSourceSkip::create(getExpCtx(), 1);
    auto skipSecond = DocumentSourceSkip::create(getExpCtx(), 1);
    DocumentSourceContainer containerOptimized;
    containerOptimized.push_back(skipFirst);
    containerOptimized.push_back(skipSecond);
    skipFirst->optimizeAt(containerOptimized.begin(), &containerOptimized);
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

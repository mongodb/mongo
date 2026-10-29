// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/tee_buffer.h"

#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <deque>


namespace mongo {
namespace {

using TeeBufferTest = AggregationContextFixture;

TEST_F(TeeBufferTest, ShouldRequireAtLeastOneConsumer) {
    ASSERT_THROWS_CODE(
        TeeBuffer::create(getExpCtx()->getOperationContext(), 0), AssertionException, 40309);
}

TEST_F(TeeBufferTest, ShouldRequirePositiveBatchSize) {
    ASSERT_THROWS_CODE(
        TeeBuffer::create(getExpCtx()->getOperationContext(), 1, 0), AssertionException, 40310);
    ASSERT_THROWS_CODE(
        TeeBuffer::create(getExpCtx()->getOperationContext(), 1, -2), AssertionException, 40310);
}

TEST_F(TeeBufferTest, ShouldBeExhaustedIfInputIsExhausted) {
    auto mock = exec::agg::MockStage::createForTest({}, getExpCtx());
    auto teeBuffer = TeeBuffer::create(getExpCtx()->getOperationContext(), 1);
    teeBuffer->setSource(mock.get());

    ASSERT(teeBuffer->getNext(0).isEOF());
    ASSERT(teeBuffer->getNext(0).isEOF());
    ASSERT(teeBuffer->getNext(0).isEOF());
}

TEST_F(TeeBufferTest, ShouldProvideAllResultsWithoutPauseIfTheyFitInOneBatch) {
    std::deque<DocumentSource::GetNextResult> inputs{Document{{"a", 1}}, Document{{"a", 2}}};
    auto mock = exec::agg::MockStage::createForTest(inputs, getExpCtx());
    auto teeBuffer = TeeBuffer::create(getExpCtx()->getOperationContext(), 1);
    teeBuffer->setSource(mock.get());

    auto next = teeBuffer->getNext(0);
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), inputs.front().getDocument());

    next = teeBuffer->getNext(0);
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), inputs.back().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
}

TEST_F(TeeBufferTest, ShouldProvideAllResultsWithoutPauseIfOnlyOneConsumer) {
    std::deque<DocumentSource::GetNextResult> inputs{Document{{"a", 1}}, Document{{"a", 2}}};
    auto mock = exec::agg::MockStage::createForTest(inputs, getExpCtx());

    const size_t bufferBytes = 1;  // Both docs won't fit in a single batch.
    auto teeBuffer = TeeBuffer::create(getExpCtx()->getOperationContext(), 1, bufferBytes);
    teeBuffer->setSource(mock.get());

    auto next = teeBuffer->getNext(0);
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), inputs.front().getDocument());

    next = teeBuffer->getNext(0);
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), inputs.back().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
}

TEST_F(TeeBufferTest, ShouldTellConsumerToPauseIfItFinishesBatchBeforeOtherConsumers) {
    std::deque<DocumentSource::GetNextResult> inputs{Document{{"a", 1}}, Document{{"a", 2}}};
    auto mock = exec::agg::MockStage::createForTest(inputs, getExpCtx());

    const size_t nConsumers = 2;
    const size_t bufferBytes = 1;  // Both docs won't fit in a single batch.
    auto teeBuffer = TeeBuffer::create(getExpCtx()->getOperationContext(), nConsumers, bufferBytes);
    teeBuffer->setSource(mock.get());

    auto next0 = teeBuffer->getNext(0);
    ASSERT_TRUE(next0.isAdvanced());
    ASSERT_DOCUMENT_EQ(next0.getDocument(), inputs.front().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isPaused());  // Consumer #1 hasn't seen the first doc yet.
    ASSERT_TRUE(teeBuffer->getNext(0).isPaused());

    auto next1 = teeBuffer->getNext(1);
    ASSERT_TRUE(next1.isAdvanced());
    ASSERT_DOCUMENT_EQ(next1.getDocument(), inputs.front().getDocument());

    // Both consumers should be able to advance now. We'll advance consumer #1.
    next1 = teeBuffer->getNext(1);
    ASSERT_TRUE(next1.isAdvanced());
    ASSERT_DOCUMENT_EQ(next1.getDocument(), inputs.back().getDocument());

    // Consumer #1 should be blocked now. The next input is EOF, but the TeeBuffer didn't get that
    // far, since the first doc filled up the buffer.
    ASSERT_TRUE(teeBuffer->getNext(1).isPaused());
    ASSERT_TRUE(teeBuffer->getNext(1).isPaused());

    // Now exhaust consumer #0.
    next0 = teeBuffer->getNext(0);
    ASSERT_TRUE(next0.isAdvanced());
    ASSERT_DOCUMENT_EQ(next0.getDocument(), inputs.back().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());

    // Consumer #1 will now realize it's exhausted.
    ASSERT_TRUE(teeBuffer->getNext(1).isEOF());
    ASSERT_TRUE(teeBuffer->getNext(1).isEOF());
}

TEST_F(TeeBufferTest, ShouldAllowOtherConsumersToAdvanceOnceTrailingConsumerIsDisposed) {
    std::deque<DocumentSource::GetNextResult> inputs{Document{{"a", 1}}, Document{{"a", 2}}};
    auto mock = exec::agg::MockStage::createForTest(inputs, getExpCtx());

    const size_t nConsumers = 2;
    const size_t bufferBytes = 1;  // Both docs won't fit in a single batch.
    auto teeBuffer = TeeBuffer::create(getExpCtx()->getOperationContext(), nConsumers, bufferBytes);
    teeBuffer->setSource(mock.get());

    auto next0 = teeBuffer->getNext(0);
    ASSERT_TRUE(next0.isAdvanced());
    ASSERT_DOCUMENT_EQ(next0.getDocument(), inputs.front().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isPaused());  // Consumer #1 hasn't seen the first doc yet.
    ASSERT_TRUE(teeBuffer->getNext(0).isPaused());

    // Kill consumer #1.
    teeBuffer->dispose(1);

    // Consumer #0 should be able to advance now.
    next0 = teeBuffer->getNext(0);
    ASSERT_TRUE(next0.isAdvanced());
    ASSERT_DOCUMENT_EQ(next0.getDocument(), inputs.back().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
}
}  // namespace
}  // namespace mongo

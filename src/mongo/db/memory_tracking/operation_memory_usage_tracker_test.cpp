/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"

#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using GetNextResult = mongo::exec::agg::GetNextResult;

/**
 * This is a subclass of DocumentSourceMock that will track the memory of each document it produces,
 * and reset the in-use memory bytes to zero when EOF is reached.
 *
 * It is templatized to be able to track with either MemoryUsageTracker or SimpleMemoryUsageTracker.
 */
template <class Tracker>
class MockStageTracking : public mongo::exec::agg::MockStage {
public:
    static boost::intrusive_ptr<MockStageTracking<Tracker>> createForTest(
        std::deque<GetNextResult> results, const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    MockStageTracking(std::deque<GetNextResult> results,
                      const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      Tracker tracker)
        : mongo::exec::agg::MockStage{"mockedStage"_sd, expCtx, std::move(results)},
          _tracker{std::move(tracker)} {}

    GetNextResult doGetNext() override {
        GetNextResult result = MockStage::doGetNext();
        if (result.isAdvanced()) {
            _tracker.add(result.getDocument().getApproximateSize());
        } else if (result.isEOF()) {
            _tracker.add(-_tracker.inUseTrackedMemoryBytes());
        }

        return result;
    }

    Tracker _tracker;
};

/**
 * Specialization for SimpleMemoryUsageTracker.
 */
template <>
boost::intrusive_ptr<MockStageTracking<SimpleMemoryUsageTracker>>
MockStageTracking<SimpleMemoryUsageTracker>::createForTest(
    std::deque<GetNextResult> results, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new MockStageTracking(
        std::move(results),
        expCtx,
        OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForStage(*expCtx));
}

/**
 * Specialization for MemoryUsageTracker.
 */
template <>
boost::intrusive_ptr<MockStageTracking<MemoryUsageTracker>>
MockStageTracking<MemoryUsageTracker>::createForTest(
    std::deque<GetNextResult> results, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new MockStageTracking(
        std::move(results),
        expCtx,
        OperationMemoryUsageTracker::createMemoryUsageTrackerForStage(*expCtx));
}

class OperationMemoryUsageTrackerTest : public AggregationContextFixture {
protected:
    std::pair<int64_t, int64_t> getCurOpMemoryStats() {
        OperationContext* opCtx = getExpCtx()->getOperationContext();
        CurOp* curOp = CurOp::get(opCtx);
        return {curOp->getInUseTrackedMemoryBytes(), curOp->getPeakTrackedMemoryBytes()};
    }

    /**
     * Given a function createMock() that produces a mock document source, create the source and
     * verify it tracks memory as expected, by checking the stats in the operation's CurOp instance.
     */
    template <typename F>
    void runTest(F createMock) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                                   true);
        int64_t inUseTrackedMemBytes, peakTrackedMemBytes;

        std::tie(inUseTrackedMemBytes, peakTrackedMemBytes) = getCurOpMemoryStats();
        ASSERT_EQ(0, inUseTrackedMemBytes);
        ASSERT_EQ(0, peakTrackedMemBytes);

        std::deque<DocumentSource::GetNextResult> docs{
            Document{{"_id", 0}, {"a", 100}, {"b", "hello"_sd}},
            Document{{"_id", 1}, {"a", 200}, {"b", "howareya"_sd}},
            Document{{"_id", 2}, {"a", 300}, {"b", "goodbye"_sd}},
        };
        auto mock = createMock(docs, getExpCtx());

        // Process the first document, and the memory usage should now be non-zero.
        ASSERT_TRUE(mock->getNext().isAdvanced());
        std::tie(inUseTrackedMemBytes, peakTrackedMemBytes) = getCurOpMemoryStats();
        ASSERT_GT(inUseTrackedMemBytes, 0);
        ASSERT_GT(peakTrackedMemBytes, 0);

        // At the second document, the memory usage should have increased.
        ASSERT_TRUE(mock->getNext().isAdvanced());
        int64_t prevTrackedMemBytes, prevPeakTrackedMemBytes;
        prevTrackedMemBytes = inUseTrackedMemBytes;
        prevPeakTrackedMemBytes = peakTrackedMemBytes;
        std::tie(inUseTrackedMemBytes, peakTrackedMemBytes) = getCurOpMemoryStats();
        ASSERT_GT(inUseTrackedMemBytes, prevTrackedMemBytes);
        ASSERT_GT(peakTrackedMemBytes, prevPeakTrackedMemBytes);

        // Third document, the memory usage should increase again
        ASSERT_TRUE(mock->getNext().isAdvanced());
        prevTrackedMemBytes = inUseTrackedMemBytes;
        prevPeakTrackedMemBytes = peakTrackedMemBytes;
        std::tie(inUseTrackedMemBytes, peakTrackedMemBytes) = getCurOpMemoryStats();
        ASSERT_GT(inUseTrackedMemBytes, prevTrackedMemBytes);
        ASSERT_GT(peakTrackedMemBytes, prevPeakTrackedMemBytes);

        // When we reach the end of the documents, current memory goes to zero, while the max used
        // remains the same.
        ASSERT_TRUE(mock->getNext().isEOF());
        prevTrackedMemBytes = inUseTrackedMemBytes;
        prevPeakTrackedMemBytes = peakTrackedMemBytes;
        std::tie(inUseTrackedMemBytes, peakTrackedMemBytes) = getCurOpMemoryStats();
        ASSERT_EQ(0, inUseTrackedMemBytes);
        ASSERT_EQ(prevPeakTrackedMemBytes, peakTrackedMemBytes);
    }
};

/**
 * Show that memory tracking works for a mock node that uses SimpleMemoryUsageTracker.
 */
TEST_F(OperationMemoryUsageTrackerTest, SimpleStageMemoryUsageAggregatedInOperationMemory) {
    runTest(MockStageTracking<SimpleMemoryUsageTracker>::createForTest);
}

/**
 * Show that memory tracking works for a mock node that uses MemoryUsageTracker.
 */
TEST_F(OperationMemoryUsageTrackerTest, StageMemoryUsageAggregatedInOperationMemory) {
    runTest(MockStageTracking<MemoryUsageTracker>::createForTest);
}

/**
 * Show that we don't aggregate operation memory stats in CurOp if the feature flag is off. In this
 * case the metrics will stay at zero and won't be reported.
 */
TEST_F(OperationMemoryUsageTrackerTest, CurOpStatsAreNotUpdatedIfFeatureFlagOff) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagQueryMemoryTracking",
                                                               false);
    auto mock = MockStageTracking<SimpleMemoryUsageTracker>::createForTest(
        {Document{{"_id", 0}, {"a", 100}, {"b", "hello"_sd}}}, getExpCtx());
    ASSERT_TRUE(mock->getNext().isAdvanced());

    int64_t inUseTrackedMemBytes, peakTrackedMemBytes;
    std::tie(inUseTrackedMemBytes, peakTrackedMemBytes) = getCurOpMemoryStats();
    ASSERT_EQ(inUseTrackedMemBytes, 0);
    ASSERT_EQ(peakTrackedMemBytes, 0);

    ASSERT_TRUE(mock->getNext().isEOF());
}

}  // namespace
}  // namespace mongo

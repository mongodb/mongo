// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"

#include "mongo/db/client.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/query/query_knobs/query_knob_configuration_test_util.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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
        : mongo::exec::agg::MockStage{"mockedStage"sv, expCtx, std::move(results)},
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
        unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking",
                                                             true);
        int64_t inUseTrackedMemBytes, peakTrackedMemBytes;

        std::tie(inUseTrackedMemBytes, peakTrackedMemBytes) = getCurOpMemoryStats();
        ASSERT_EQ(0, inUseTrackedMemBytes);
        ASSERT_EQ(0, peakTrackedMemBytes);

        std::deque<DocumentSource::GetNextResult> docs{
            Document{{"_id", 0}, {"a", 100}, {"b", "hello"sv}},
            Document{{"_id", 1}, {"a", 200}, {"b", "howareya"sv}},
            Document{{"_id", 2}, {"a", 300}, {"b", "goodbye"sv}},
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
    unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking", false);
    auto mock = MockStageTracking<SimpleMemoryUsageTracker>::createForTest(
        {Document{{"_id", 0}, {"a", 100}, {"b", "hello"sv}}}, getExpCtx());
    ASSERT_TRUE(mock->getNext().isAdvanced());

    int64_t inUseTrackedMemBytes, peakTrackedMemBytes;
    std::tie(inUseTrackedMemBytes, peakTrackedMemBytes) = getCurOpMemoryStats();
    ASSERT_EQ(inUseTrackedMemBytes, 0);
    ASSERT_EQ(peakTrackedMemBytes, 0);

    ASSERT_TRUE(mock->getNext().isEOF());
}

/**
 * Contexts that opt out of operation-wide memory tracking get standalone stage trackers: per-stage
 * limits still apply, but usage neither counts toward the per-operation limit nor reaches CurOp.
 */
TEST_F(OperationMemoryUsageTrackerTest, StageTrackersAreStandaloneWhenOperationTrackingExcluded) {
    unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard perOpLimit("internalQueryMaxMemoryUsageBytesPerOperation", 4);

    auto expCtx = getExpCtx();
    expCtx->setExcludeOperationMemoryTracking(true);

    auto assertStandalone = [&](auto tracker) {
        tracker.add(100);  // Exceeds the per-operation limit; a chained tracker would fail.
        ASSERT_TRUE(tracker.withinMemoryLimit(expCtx->getOperationContext()));

        int64_t inUseTrackedMemBytes, peakTrackedMemBytes;
        std::tie(inUseTrackedMemBytes, peakTrackedMemBytes) = getCurOpMemoryStats();
        ASSERT_EQ(inUseTrackedMemBytes, 0);
        ASSERT_EQ(peakTrackedMemBytes, 0);
        tracker.add(-100);
    };

    assertStandalone(OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForStage(*expCtx));
    assertStandalone(
        OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForStage(*expCtx));
    assertStandalone(OperationMemoryUsageTracker::createMemoryUsageTrackerForStage(*expCtx));
    assertStandalone(OperationMemoryUsageTracker::createChunkedMemoryUsageTrackerForStage(*expCtx));
}

/**
 * A knob-backed limit built via loadMemoryLimit() must resolve its value from the query knob (its
 * backing server parameter) against the operation, not from any snapshot taken at construction.
 */
TEST_F(OperationMemoryUsageTrackerTest, KnobBackedLimitResolvesFromKnob) {
    OperationContext* opCtx = getExpCtx()->getOperationContext();
    QueryKnobGuardForTest knobGuard{opCtx, "internalDocumentSourceGroupMaxMemoryBytes", 100LL};

    SimpleMemoryUsageTracker tracker{
        loadMemoryLimit(StageMemoryLimit::DocumentSourceGroupMaxMemoryBytes)};
    ASSERT_EQ(tracker.maxAllowedMemoryUsageBytes(opCtx), 100);
}

/**
 * withinMemoryLimit()/assertWithinMemoryLimit() on a knob-backed tracker must enforce the knob's
 * value, not a hard-coded default.
 */
TEST_F(OperationMemoryUsageTrackerTest, WithinMemoryLimitEnforcesKnobBackedLimit) {
    OperationContext* opCtx = getExpCtx()->getOperationContext();
    QueryKnobGuardForTest knobGuard{opCtx, "internalDocumentSourceGroupMaxMemoryBytes", 100LL};

    SimpleMemoryUsageTracker tracker{
        loadMemoryLimit(StageMemoryLimit::DocumentSourceGroupMaxMemoryBytes)};

    tracker.add(50);
    ASSERT_TRUE(tracker.withinMemoryLimit(opCtx));

    tracker.add(51);
    ASSERT_FALSE(tracker.withinMemoryLimit(opCtx));
}

/**
 * The value is genuinely read from the knob: a different guarded value yields a different limit.
 */
TEST_F(OperationMemoryUsageTrackerTest, KnobBackedLimitTracksGuardedValue) {
    OperationContext* opCtx = getExpCtx()->getOperationContext();
    QueryKnobGuardForTest knobGuard{opCtx, "internalDocumentSourceGroupMaxMemoryBytes", 4242LL};

    SimpleMemoryUsageTracker tracker{
        loadMemoryLimit(StageMemoryLimit::DocumentSourceGroupMaxMemoryBytes)};
    ASSERT_EQ(tracker.maxAllowedMemoryUsageBytes(opCtx), 4242);
}

/**
 * With a null OperationContext a knob-backed limit returns the knob's global value without
 * latching it: a later read that does have an operation resolves against that operation's
 * QueryKnobConfiguration.
 */
TEST_F(OperationMemoryUsageTrackerTest, NullOpCtxReadsGlobalValueWithoutLatching) {
    OperationContext* opCtx = getExpCtx()->getOperationContext();
    MemoryUsageLimit limit = loadMemoryLimit(StageMemoryLimit::DocumentSourceGroupMaxMemoryBytes);
    {
        QueryKnobGuardForTest knobGuard{opCtx, "internalDocumentSourceGroupMaxMemoryBytes", 100LL};
        ASSERT_EQ(limit.get(nullptr), 100);
    }
    QueryKnobGuardForTest knobGuard{opCtx, "internalDocumentSourceGroupMaxMemoryBytes", 4242LL};
    ASSERT_EQ(limit.get(opCtx), 4242);
}

/**
 * The first get() with an operation latches the resolved value: a second get() under a different
 * configuration still returns the first-resolved value.
 */
TEST_F(OperationMemoryUsageTrackerTest, KnobBackedLimitLatchesFirstResolvedValue) {
    OperationContext* opCtx = getExpCtx()->getOperationContext();
    MemoryUsageLimit limit = loadMemoryLimit(StageMemoryLimit::DocumentSourceGroupMaxMemoryBytes);
    {
        QueryKnobGuardForTest knobGuard{opCtx, "internalDocumentSourceGroupMaxMemoryBytes", 100LL};
        ASSERT_EQ(limit.get(opCtx), 100);
    }
    QueryKnobGuardForTest knobGuard{opCtx, "internalDocumentSourceGroupMaxMemoryBytes", 4242LL};
    ASSERT_EQ(limit.get(opCtx), 100);
}

}  // namespace
}  // namespace mongo

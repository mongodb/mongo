// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_handle_topology_change_v2_metrics.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2_test_helpers.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace test {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

using V2Stage = exec::agg::ChangeStreamHandleTopologyChangeV2Stage;
using State = V2Stage::State;

// Owns the mock dependencies of a v2 stage built for metrics testing.
// 'buildParametersForTest()' stores non-owning pointers to the mocks, so they must outlive the
// stage; tests must therefore keep this struct alive for as long as they use 'stage'.
struct V2StageWithMocksForMetricsTest {
    std::shared_ptr<ChangeStreamReaderBuilderMock> changeStreamReaderBuilder;
    std::unique_ptr<DataToShardsAllocationQueryServiceMock> dataToShardsAllocationQueryService;
    boost::intrusive_ptr<V2Stage> docSource;
};

// Builds a standalone v2 stage with mock dependencies, sufficient for driving the state machine
// via 'setState_forTest()' and 'dispose()'. The returned struct owns the mocks so that the
// stage's non-owning 'Parameters' pointers stay valid.
V2StageWithMocksForMetricsTest makeV2StageForMetricsTest(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    V2StageWithMocksForMetricsTest result;
    result.changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    result.dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();
    auto params =
        buildParametersForTest(expCtx,
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               result.changeStreamReaderBuilder.get(),
                               result.dataToShardsAllocationQueryService.get());
    result.docSource = make_intrusive<V2Stage>(expCtx, params);
    return result;
}

// Each state entry increments exactly its own topologyState counter. Uses
// 'recordStateEntry_forTest()' to exercise every state directly, bypassing transition validation.
class V2StageStateEntryMetricsTest : public ChangeStreamStageTestNoSetup {};

TEST_F(V2StageStateEntryMetricsTest, StateEntryUpdatesMatchingCounter) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kIgnoreRemovedShards));
    // Keep the mock owners alive alongside the stage: Parameters holds non-owning pointers.
    const auto v2StageWithMocks = makeV2StageForMetricsTest(getExpCtx());
    const auto& docSource = v2StageWithMocks.docSource;

    auto waitingBefore =
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingTopologyStateWaiting);
    auto fetchingInitializationBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTopologyStateFetchingInitialization);
    auto fetchingGettingChangeEventBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTopologyStateFetchingGettingChangeEvent);
    auto fetchingStartingChangeStreamSegmentBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTopologyStateFetchingStartingChangeStreamSegment);
    auto fetchingNormalGettingChangeEventBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTopologyStateFetchingNormalGettingChangeEvent);
    auto fetchingDegradedGettingChangeEventBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTopologyStateFetchingDegradedGettingChangeEvent);
    auto downgradingBefore =
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingTopologyStateDowngrading);
    auto finalBefore =
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingTopologyStateFinal);

    docSource->recordStateEntry_forTest(State::kWaiting);
    docSource->recordStateEntry_forTest(State::kFetchingInitialization);
    docSource->recordStateEntry_forTest(State::kFetchingGettingChangeEvent);
    docSource->recordStateEntry_forTest(State::kFetchingStartingChangeStreamSegment);
    docSource->recordStateEntry_forTest(State::kFetchingNormalGettingChangeEvent);
    docSource->recordStateEntry_forTest(State::kFetchingDegradedGettingChangeEvent);
    docSource->recordStateEntry_forTest(State::kDowngrading);
    docSource->recordStateEntry_forTest(State::kFinal);

    ASSERT_EQ(
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingTopologyStateWaiting),
        waitingBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingTopologyStateFetchingInitialization),
              fetchingInitializationBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingTopologyStateFetchingGettingChangeEvent),
              fetchingGettingChangeEventBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::
                      kChangeStreamShardTargetingTopologyStateFetchingStartingChangeStreamSegment),
              fetchingStartingChangeStreamSegmentBefore + 1);
    ASSERT_EQ(
        capturer.readInt64Counter(
            MetricNames::kChangeStreamShardTargetingTopologyStateFetchingNormalGettingChangeEvent),
        fetchingNormalGettingChangeEventBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::
                      kChangeStreamShardTargetingTopologyStateFetchingDegradedGettingChangeEvent),
              fetchingDegradedGettingChangeEventBefore + 1);
    ASSERT_EQ(
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingTopologyStateDowngrading),
        downgradingBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingTopologyStateFinal),
              finalBefore + 1);
}

// Drives the real stage's state machine: a validated transition into degraded mode increments
// both the degraded-entry counter and the degraded gauge; a transition out decrements the gauge.
class V2StageDegradedMetricsTest : public ChangeStreamStageTestNoSetup {};

TEST_F(V2StageDegradedMetricsTest, DegradedEntryAndExitThroughSetState) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kIgnoreRemovedShards));
    // Keep the mock owners alive alongside the stage: Parameters holds non-owning pointers.
    const auto v2StageWithMocks = makeV2StageForMetricsTest(getExpCtx());
    const auto& docSource = v2StageWithMocks.docSource;

    auto degradedBefore =
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingDegraded);
    auto degradedEntriesBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTopologyStateFetchingDegradedGettingChangeEvent);
    auto normalEntriesBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTopologyStateFetchingNormalGettingChangeEvent);

    // Walk the stage into the segment-starting state without recording (unvalidated), then enter
    // degraded mode through a validated transition.
    docSource->setState_forTest(State::kWaiting, false);
    docSource->setState_forTest(State::kFetchingInitialization, false);
    docSource->setState_forTest(State::kFetchingStartingChangeStreamSegment, false);
    docSource->setState_forTest(State::kFetchingDegradedGettingChangeEvent,
                                true /* validateStateTransition */);

    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingDegraded),
              degradedBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::
                      kChangeStreamShardTargetingTopologyStateFetchingDegradedGettingChangeEvent),
              degradedEntriesBefore + 1);

    // Leaving degraded mode decrements the gauge again and counts a normal-mode entry.
    docSource->setState_forTest(State::kFetchingNormalGettingChangeEvent,
                                true /* validateStateTransition */);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingDegraded),
              degradedBefore);
    ASSERT_EQ(
        capturer.readInt64Counter(
            MetricNames::kChangeStreamShardTargetingTopologyStateFetchingNormalGettingChangeEvent),
        normalEntriesBefore + 1);
}

// A stage torn down while in degraded mode never leaves it via a state transition: 'dispose()'
// must decrement the gauge so the count does not leak.
TEST_F(V2StageDegradedMetricsTest, DisposeWhileDegradedDecrementsGauge) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kIgnoreRemovedShards));
    // Keep the mock owners alive alongside the stage: Parameters holds non-owning pointers.
    const auto v2StageWithMocks = makeV2StageForMetricsTest(getExpCtx());
    const auto& docSource = v2StageWithMocks.docSource;

    auto degradedBefore =
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingDegraded);

    docSource->setState_forTest(State::kWaiting, false);
    docSource->setState_forTest(State::kFetchingInitialization, false);
    docSource->setState_forTest(State::kFetchingStartingChangeStreamSegment, false);
    docSource->setState_forTest(State::kFetchingDegradedGettingChangeEvent,
                                true /* validateStateTransition */);
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingDegraded),
              degradedBefore + 1);

    // Dispose while still degraded; a state transition out never fires.
    docSource->dispose();
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingDegraded),
              degradedBefore);

    // A second dispose must not decrement again.
    docSource->dispose();
    ASSERT_EQ(capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingDegraded),
              degradedBefore);
}

}  // namespace
}  // namespace test
}  // namespace mongo

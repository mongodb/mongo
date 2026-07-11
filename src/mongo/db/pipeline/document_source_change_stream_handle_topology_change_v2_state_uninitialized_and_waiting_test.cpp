// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2_test_helpers.h"
#include "mongo/db/query/find_common.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using namespace test;

class DSV2StateUninitializedAndWaitingTest : public ChangeStreamStageTestNoSetup {};

// State tests.
// ------------

// Tests state machine for input state kUninitialized, for a cluster time for which there is no
// data-to-shards allocation information present. The change stream is expected to fail in this
// case.
TEST_F(DSV2StateUninitializedAndWaitingTest, StateUninitializedAllocationNotAvailable) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(
        std::make_pair(Timestamp(23, 0), AllocationToShardsStatus::kNotAvailable));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kUninitialized,
                                false /* validateStateTransition */);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    ASSERT_FALSE(getCursorManagerMock(params)->isInitialized());

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);

    // Cursor manager should not have been initialized.
    ASSERT_FALSE(getCursorManagerMock(params)->isInitialized());

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());

    // Last request time should have been updated due to the query-to-shards-allocation request.
    ASSERT_EQ(now.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());
}

// Tests state machine for input state kUninitialized, for a cluster time for which there is
// data-to-shards allocation information present. The state machine is supposed to go into state
// kFetchingInitialization.
TEST_F(DSV2StateUninitializedAndWaitingTest, StateUninitializedAllocationOk) {
    const Timestamp ts = Timestamp(23, 0);
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    // Timestamp of ResumeToken is in the past. We simulate that no data-to-shards allocation is
    // available anymore.
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kUninitialized,
                                false /* validateStateTransition */);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);
    ASSERT_FALSE(getCursorManagerMock(params)->isInitialized());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingInitialization, docSource->getState_forTest());

    // Cursor manager should have been initialized.
    ASSERT_TRUE(getCursorManagerMock(params)->isInitialized());
    ASSERT_BSONOBJ_EQ(ResumeToken::makeHighWaterMarkToken(ts, 1 /* version */).toBSON(),
                      getCursorManagerMock(params)->getResumeToken().toBSON());

    // Last request time should have been updated due to the query-to-shards-allocation request.
    ASSERT_EQ(now.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());
}

// Tests state machine for input state kUninitialized, for a cluster time which is in the future.
// The state machine is supposed to go into state kWaiting.
TEST_F(DSV2StateUninitializedAndWaitingTest, StateUninitializedAllocationFutureClusterTime) {
    const Timestamp ts = Timestamp(42, 23);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    // Timestamp of ResumeToken is in the past. We simulate that no data-to-shards allocation is
    // available anymore.
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kFutureClusterTime));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kUninitialized,
                                false /* validateStateTransition */);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    ASSERT_FALSE(getCursorManagerMock(params)->isInitialized());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());

    // Cursor manager should have been initialized.
    ASSERT_TRUE(getCursorManagerMock(params)->isInitialized());
    ASSERT_BSONOBJ_EQ(ResumeToken::makeHighWaterMarkToken(ts, 1 /* version */).toBSON(),
                      getCursorManagerMock(params)->getResumeToken().toBSON());

    // Last request time should have been updated due to the query-to-shards-allocation request.
    ASSERT_EQ(now.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());
}

// Tests state machine for input state kWaiting, when the data-to-shards allocation query service
// returns that no placement information is available.
TEST_F(DSV2StateUninitializedAndWaitingTest, StateWaitingNoPlacementInfoAvailable) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(ts,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        V2StageTestHelpers::kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(
        getExpCtx(), 10, changeStreamReaderBuilder.get(), dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kNotAvailable));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    // Set deadline to a few milliseconds in the future.
    Date_t deadline = now + Milliseconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());

    // Calling 'getNext()' again must return the same pre-recorded error:
    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when the data-to-shards allocation query service
// returns that the cluster time is still in the future.
TEST_F(DSV2StateUninitializedAndWaitingTest, StateWaitingFutureClusterTime) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(ts,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        V2StageTestHelpers::kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kFutureClusterTime));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    // Mark this as an awaitData getMore so the poll branch's kFutureClusterTime arm loops
    // instead of EOFing (that EOF is reserved for the initial-aggregate path).
    awaitDataState(getOpCtx()).shouldWaitForInserts = true;
    awaitDataState(getOpCtx()).waitForInsertsDeadline = now + Hours(1);

    // Set deadline to a few milliseconds in the future.
    Date_t deadline = now + Milliseconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // State should remain in kWaiting, as no progress has been made.
    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
}

// Tests that consecutive kFutureClusterTime poll results keep the state machine in kWaiting and
// never short-circuit awaitData. EOF must only fire when the OperationContext deadline expires
// (i.e., the deadlineWaiter throws ExceededTimeLimit).
TEST_F(DSV2StateUninitializedAndWaitingTest,
       StateWaitingFutureClusterTimeRepeatsPollsUntilDeadline) {
    const Timestamp ts = Timestamp(42, 0);
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    constexpr int kPollPeriodSecs = 5;
    constexpr int kNumPolls = 3;

    auto params = buildParametersForTest(getExpCtx(),
                                         kPollPeriodSecs /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Buffer kNumPolls consecutive kFutureClusterTime responses, one per loop iteration.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    for (int i = 0; i < kNumPolls; ++i) {
        mockResponses.emplace_back(ts, AllocationToShardsStatus::kFutureClusterTime);
    }
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    // Mark this as an awaitData getMore so the poll branch's kFutureClusterTime arm loops
    // instead of EOFing on the first poll (that EOF is reserved for the initial-aggregate path).
    awaitDataState(getOpCtx()).shouldWaitForInserts = true;
    awaitDataState(getOpCtx()).waitForInsertsDeadline = now + Hours(1);

    // Generous OperationContext deadline, far past kNumPolls * kPollPeriodSecs so it cannot
    // be the reason for any EOF in the loop below.
    getOpCtx()->setDeadlineByDate(now + Seconds(kPollPeriodSecs * (kNumPolls + 10)),
                                  ErrorCodes::ExceededTimeLimit);

    // Drive kNumPolls poll iterations. For each, pretend we last polled long enough ago that
    // _handleStateWaiting bypasses deadlineWaiter and goes straight to the poll branch.
    // With the fix, every poll returning kFutureClusterTime must return boost::none (loop)
    // and keep the state in kWaiting — never EOF.
    for (int i = 0; i < kNumPolls; ++i) {
        docSource->setLastAllocationToShardsRequestTime_forTest(now - Seconds(kPollPeriodSecs));

        auto result = docSource->runGetNextStateMachine_forTest();
        ASSERT_FALSE(result.has_value())
            << "kFutureClusterTime must not emit EOF (iteration " << i << ")";
        ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest())
            << "state must remain kWaiting (iteration " << i << ")";
    }

    // Now simulate the deadline finally firing, making the waiter return ExceededTimeLimit,
    // mirroring what happens when the opCtx deadline is reached while waiting for the next poll.
    docSource->setLastAllocationToShardsRequestTime_forTest(now);
    getDeadlineWaiterMock(params)->setStatus(
        Status(ErrorCodes::ExceededTimeLimit, "operation context deadline exceeded"));

    auto finalResult = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(finalResult.has_value());
    ASSERT_TRUE(finalResult->isEOF())
        << "EOF must come from the deadlineWaiter ExceededTimeLimit catch, not from "
           "kFutureClusterTime";

    // The catch block returns EOF without transitioning state; the cursor is still alive
    // and a subsequent getMore would re-enter kWaiting.
    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
}

// Tests that when this stage is in waiting state, it honours the awaitData deadline stored in
// 'awaitDataState(opCtx).waitForInsertsDeadline' by mongos. When that deadline is reached, the
// stage must surface a clean EOF.
TEST_F(DSV2StateUninitializedAndWaitingTest, StateWaitingHonoursAwaitDataDeadline) {
    const Timestamp ts = Timestamp(42, 0);
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    constexpr int kPollPeriodSecs = 10;
    auto params = buildParametersForTest(getExpCtx(),
                                         kPollPeriodSecs /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    // Pretend we just polled, so the wait branch is taken (not the poll branch).
    docSource->setLastAllocationToShardsRequestTime_forTest(now);

    // No opCtx deadline - mongos does not set one for tailable+awaitData getMores. The awaitData
    // deadline is stashed in awaitDataState instead. Set it well before nextPollTime so it is the
    // active cap.
    awaitDataState(getOpCtx()).shouldWaitForInserts = true;
    awaitDataState(getOpCtx()).waitForInsertsDeadline = now + Seconds(1);

    // Simulate "the wait completed normally at waitForInsertsDeadline": when the stage looks at
    // 'now' after waitUntil returns, it must see now >= waitForInsertsDeadline and EOF cleanly.
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now + Seconds(1));

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF())
        << "awaitData deadline expiry must surface a clean EOF (empty batch + PBRT)";

    // The cursor stays alive; only the batch ended. State must remain kWaiting so a subsequent
    // getMore picks up here and re-enters the same flow.
    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());

    // The waiter should have been clamped to the awaitData deadline (= now + 1s in the original
    // 'now'), not to nextPollTime (= now + 10s).
    ASSERT_EQ((now + Seconds(1)).toMillisSinceEpoch(),
              getDeadlineWaiterMock(params)->getLastUsedDeadline().toMillisSinceEpoch());
}

// Tests the full "initial aggregate against a future clusterTime" flow through this stage:
// turn 1 enters the wait branch and calls waitUntil with nextPollTime (no awaitData clamp);
// turn 2 (after the wait has elapsed) takes the poll branch, sees kFutureClusterTime, and
// emits a clean EOF so the aggregate returns the cursor with empty firstBatch.
TEST_F(DSV2StateUninitializedAndWaitingTest,
       InitialAggregateWithFutureClusterTimeEofsAfterOnePollCycle) {
    const Timestamp ts = Timestamp(42, 0);
    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    constexpr int kPollPeriodSecs = 10;
    auto params = buildParametersForTest(getExpCtx(),
                                         kPollPeriodSecs /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Turn 2 will poll and see kFutureClusterTime.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.emplace_back(ts, AllocationToShardsStatus::kFutureClusterTime);
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    // Generous opCtx deadline so it cannot cause EOF.
    getOpCtx()->setDeadlineByDate(now + Seconds(kPollPeriodSecs * 5),
                                  ErrorCodes::ExceededTimeLimit);

    // No awaitData state -> this is the initial aggregate path.
    ASSERT_FALSE(awaitDataState(getOpCtx()).shouldWaitForInserts);

    // Turn 1: wait branch (lastPoll == now, so secondsSinceLastPoll < kPollPeriodSecs). Without
    // an awaitData deadline, waitUntilDate must equal nextPollTime; the state machine loops
    // (returns boost::none), no EOF.
    docSource->setLastAllocationToShardsRequestTime_forTest(now);
    {
        auto result = docSource->runGetNextStateMachine_forTest();
        ASSERT_FALSE(result.has_value()) << "turn 1 must loop, not EOF";
        ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
        ASSERT_EQ((now + Seconds(kPollPeriodSecs)).toMillisSinceEpoch(),
                  getDeadlineWaiterMock(params)->getLastUsedDeadline().toMillisSinceEpoch())
            << "waiter must be called with nextPollTime when there is no awaitData deadline";
    }

    // Turn 2: pretend the wait has elapsed by backdating lastPoll, so we take the poll branch.
    // With kFutureClusterTime and no awaitData state, the stage must EOF instead of looping.
    docSource->setLastAllocationToShardsRequestTime_forTest(now - Seconds(kPollPeriodSecs + 1));
    {
        auto result = docSource->runGetNextStateMachine_forTest();
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->isEOF())
            << "initial aggregate with kFutureClusterTime must EOF after one poll cycle";
        // State stays kWaiting so the follow-up getMore picks up here with its awaitData
        // deadline and takes the loop-with-clamped-wait path.
        ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
    }
}

// Tests state machine for input state kWaiting, when the data-to-shards allocation query service
// returns that the allocation is available.
TEST_F(DSV2StateUninitializedAndWaitingTest, StateWaitingTransitioningToFetching) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(ts,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        V2StageTestHelpers::kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* allocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    // Last request time should have no value initially.
    ASSERT_EQ(Date_t().toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    // Set deadline to a few milliseconds in the future.
    Date_t deadline = now + Milliseconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // State should have transitioned to kFetchingInitialization.
    ASSERT_EQ(V2Stage::State::kFetchingInitialization, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when the deadline for the next data-to-shards
// allocation query is earlier than the deadline on the OperationContext.
TEST_F(DSV2StateUninitializedAndWaitingTest,
       StateWaitingBehaviorPollBeforeOperationContextDeadline) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    Date_t lastAllocationToShardsRequestTime = now - Seconds(2);
    docSource->setLastAllocationToShardsRequestTime_forTest(lastAllocationToShardsRequestTime);
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // Set deadline to further away than the next poll date/time.
    Date_t deadline = now + Seconds(params->minAllocationToShardsPollPeriodSecs);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // The waiter should have been called with a deadline that is equal to now plus the poll period.
    ASSERT_EQ(
        (lastAllocationToShardsRequestTime + Seconds(params->minAllocationToShardsPollPeriodSecs))
            .toMillisSinceEpoch(),
        getDeadlineWaiterMock(params)->getLastUsedDeadline().toMillisSinceEpoch());

    // Last request time shouldn't have been modified.
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when the deadline for the next data-to-shards
// allocation query is later than the deadline on the OperationContext.
TEST_F(DSV2StateUninitializedAndWaitingTest,
       StateWaitingBehaviorPollAfterOperationContextDeadline) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    Date_t lastAllocationToShardsRequestTime = now - Seconds(2);
    docSource->setLastAllocationToShardsRequestTime_forTest(lastAllocationToShardsRequestTime);
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // Set deadline to earlier than the next poll date/time.
    Date_t deadline = now + Seconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // The waiter should have been called with a deadline that is equal to the next poll time.
    ASSERT_EQ(
        (lastAllocationToShardsRequestTime + Seconds(params->minAllocationToShardsPollPeriodSecs))
            .toMillisSinceEpoch(),
        getDeadlineWaiterMock(params)->getLastUsedDeadline().toMillisSinceEpoch());

    // Last request time shouldn't have been modified.
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when we wait on the OperationContext and the wait
// function returns a timeout error status.
TEST_F(DSV2StateUninitializedAndWaitingTest, StateWaitingBehaviorWhenWaitReturnsTimeoutError) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    Date_t lastAllocationToShardsRequestTime = now;
    docSource->setLastAllocationToShardsRequestTime_forTest(lastAllocationToShardsRequestTime);
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // Set arbitrary deadline.
    Date_t deadline = now + Seconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    // Make waiting fail with a non-OK status.
    getDeadlineWaiterMock(params)->setStatus(
        Status(ErrorCodes::ExceededTimeLimit, "timelimit exceeded!"));

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());

    // The waiter should have been called with a deadline that is equal to the next poll time.
    ASSERT_EQ((now + Seconds(params->minAllocationToShardsPollPeriodSecs)).toMillisSinceEpoch(),
              getDeadlineWaiterMock(params)->getLastUsedDeadline().toMillisSinceEpoch());

    // Last request time shouldn't have been modified.
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // State should not have changed.
    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
}

// Tests state machine for input state kWaiting, when we wait on the OperationContext and the wait
// function returns a non-timeout error status.
TEST_F(DSV2StateUninitializedAndWaitingTest, StateWaitingBehaviorWhenWaitReturnsNonTimeoutError) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params = buildParametersForTest(getExpCtx(),
                                         10 /* minAllocationToShardsPollPeriodSecs */,
                                         changeStreamReaderBuilder.get(),
                                         dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    docSource->setState_forTest(V2Stage::State::kWaiting, false /* validateStateTransition */);

    Date_t now = Date_t::now();
    getPreciseClockSource(getOpCtx()->getServiceContext())->reset(now);

    Date_t lastAllocationToShardsRequestTime = now;
    docSource->setLastAllocationToShardsRequestTime_forTest(lastAllocationToShardsRequestTime);
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // Set arbitrary deadline.
    Date_t deadline = now + Seconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    // Make waiting fail with a non-OK status.
    getDeadlineWaiterMock(params)->setStatus(Status(ErrorCodes::ShutdownInProgress, "shutdown!"));

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::ShutdownInProgress);

    // The waiter should have been called with a deadline that is equal to the next poll time.
    ASSERT_EQ((now + Seconds(params->minAllocationToShardsPollPeriodSecs)).toMillisSinceEpoch(),
              getDeadlineWaiterMock(params)->getLastUsedDeadline().toMillisSinceEpoch());

    // Last request time shouldn't have been modified.
    ASSERT_EQ(lastAllocationToShardsRequestTime.toMillisSinceEpoch(),
              docSource->getLastAllocationToShardsRequestTime_forTest().toMillisSinceEpoch());

    // State should have transitioned to kFinal, as waiting threw an exception.
    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

}  // namespace
}  // namespace mongo

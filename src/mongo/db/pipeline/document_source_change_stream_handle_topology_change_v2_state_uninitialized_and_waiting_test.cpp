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

#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2_test_helpers.h"
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

    // Set deadline to a few milliseconds in the future.
    Date_t deadline = now + Milliseconds(5);
    getOpCtx()->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());

    // State should remain in kWaiting, as no progress has been made.
    ASSERT_EQ(V2Stage::State::kWaiting, docSource->getState_forTest());
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


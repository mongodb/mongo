// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2_test_helpers.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using namespace test;

class DSV2StateFetchingInitializationAndStartingTest : public ChangeStreamStageTestNoSetup {};
using DSV2StateFetchingInitializationAndStartingTestDeathTest =
    DSV2StateFetchingInitializationAndStartingTest;

// Tests state machine for input state kFetchingInitialization, when the shard targeter returns
// kContinue. Expects the state to transition to kFetchingGettingChangeEvent.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingInitializationStrictModeContinue) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
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

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingInitialization,
                                false /* validateStateTransition */);

    // Assuming not to have segment start or end timestamps before calling the state machine.
    ASSERT_FALSE(docSource->getSegmentStartTimestamp_forTest().has_value());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // Assuming not to have segment start or end timestamps after the call because we are in strict
    // mode.
    ASSERT_FALSE(docSource->getSegmentStartTimestamp_forTest().has_value());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingInitialization, when the shard targeter returns
// kSwitchToV1. Expects the state to transition to kDowngrading.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingInitializationStrictModeSwitchToV1) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
                                        ShardTargeterDecision::kSwitchToV1,
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

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingInitialization,
                                false /* validateStateTransition */);

    // Assuming not to have a segment start timestamp before calling the state machine.
    ASSERT_FALSE(docSource->getSegmentStartTimestamp_forTest().has_value());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());

    // Assuming not to have segment start or end timestamps after the call because we are in strict
    // mode.
    ASSERT_FALSE(docSource->getSegmentStartTimestamp_forTest().has_value());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_EQ(V2Stage::State::kDowngrading, docSource->getState_forTest());

    // When state is kDowngrading, the expected result upon next state machine invocation is an
    // exception.
    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingInitialization for non-control events.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingInitializationStrictModeGettingChangeEventNonControlEvents) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();


    // Test that the stage returns all inputs as they are.
    const BSONObj doc1 = BSON("operationType" << "test1" << "foo" << "bar");
    const BSONObj doc2 = BSON("operationType" << "test2" << "test" << "value");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc1),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc2),
        DocumentSource::GetNextResult::makeEOF(),
    };

    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(docSource, source.get());

    docSource->setState_forTest(V2Stage::State::kFetchingGettingChangeEvent,
                                false /* validateStateTransition */);

    // Check return value 1 (pause).
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());

    // Check return value 2 (doc1).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(doc1, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());

    // Check return value 3 (pause).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());

    // Check return value 4 (doc2).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(doc2, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());

    // Check return value 5 (eof).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingInitialization for a control event.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingInitializationStrictModeGettingChangeEventWithControlEvent) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    const BSONObj event = BSON("operationType" << "test" << "foo" << "bar"
                                               << Document::metaFieldChangeStreamControlEvent << 1);

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Document{event},
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

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makeAdvancedControlDocument(
            Document::fromBsonWithMetaData(event))};

    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(docSource, source.get());

    docSource->setState_forTest(V2Stage::State::kFetchingGettingChangeEvent,
                                false /* validateStateTransition */);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingInitialization in ignoreRemovedShards mode. This is
// supposed to set the start time of the change stream segment and transition to state
// kFetchingStartingChangeStreamSegment.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingInitializationIgnoreRemovedShards) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

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

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingInitialization,
                                false /* validateStateTransition */);

    // Assuming not to have segment start or end timestamps before calling the state machine.
    ASSERT_FALSE(docSource->getSegmentStartTimestamp_forTest().has_value());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // After calling the state machine, the segment start timestamp must be set to the resume
    // token's timestamp.
    const auto& segmentStartTimestamp = docSource->getSegmentStartTimestamp_forTest();
    ASSERT_TRUE(segmentStartTimestamp.has_value());
    ASSERT_EQ(ts, *segmentStartTimestamp);

    // Still assuming no end timestamp.
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, without the segment
// start timestamp being set.
DEATH_TEST_REGEX_F(DSV2StateFetchingInitializationAndStartingTestDeathTest,
                   StateFetchingStartingChangeStreamSegmentWithoutStartTimestamp,
                   "Tripwire assertion.*10657518") {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

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

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);

    // Intentionally do not set the segment start timestamp before entering the state to trigger the
    // following tassert.
    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657518);

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, when the shard targeter
// returns kSwitchToV1.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingStartingChangeStreamSegmentSwitchToV1) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
                                        ShardTargeterDecision::kSwitchToV1,
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

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);

    docSource->setSegmentStartTimestamp_forTest(ts);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());

    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_EQ(V2Stage::State::kDowngrading, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, when the shard targeter
// returns kContinue and no end timestamp for the segment.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingStartingChangeStreamSegmentContinueWithoutEndTimestamp) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
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

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);

    docSource->setSegmentStartTimestamp_forTest(ts);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, when the shard targeter
// returns kContinue and an end timestamp for the segment.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingStartingChangeStreamSegmentContinueAndEndTimestamp) {
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(42, 1);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
                                        ShardTargeterDecision::kContinue,
                                        segmentEndTimestamp,
                                        V2StageTestHelpers::kEmptyShardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);

    docSource->setSegmentStartTimestamp_forTest(ts);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    ASSERT_TRUE(docSource->getSegmentEndTimestamp_forTest().has_value());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());

    // Undo mode must have been turned on when entering the degraded fetching state.
    ASSERT_TRUE(*getCursorManagerMock(params)->getUndoNextMode());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, when we try to open a
// new cursor on a shard and this fails with 'ShardNotFound' exceptions.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingStartingChangeStreamSegmentOpenCursorFailsWithShardNotFound) {
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(42, 1);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    ChangeStreamShardTargeterMock::Response shardTargeterResponse(
        Timestamp(23, 0),
        ShardTargeterDecision::kContinue,
        segmentEndTimestamp,
        [](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDoc,
           ChangeStreamReaderContext& readerContext) {
            readerContext.openCursorsOnDataShards(std::get<Timestamp>(tsOrDoc),
                                                  stdx::unordered_set<ShardId>{{"shardA"}});
        });
    // Add the same response twice, as we will make the cursor manager throw an exception upon the
    // first attempt, and the shard targeter will be asked again.
    shardTargeterResponses.push_back(shardTargeterResponse);
    shardTargeterResponses.push_back(shardTargeterResponse);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);

    docSource->setSegmentStartTimestamp_forTest(ts);

    // Makes cursor manager throw a 'ShardNotFound' exception when trying to open a cursor.
    getCursorManagerMock(params)->setThrowShardNotFoundExceptions(1);
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // State should not have changed!
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    result = docSource->runGetNextStateMachine_forTest();
    // boost::none is returned because of the state transition.
    ASSERT_FALSE(result.has_value());

    ASSERT_TRUE(docSource->getSegmentEndTimestamp_forTest().has_value());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());

    // Undo mode must have been turned on when entering the degraded fetching state.
    ASSERT_TRUE(*getCursorManagerMock(params)->getUndoNextMode());
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment, when we try to open a
// new cursor on a shard and this fails with 'ShardNotFound' exceptions repeatedly until the max
// number of consecutive failures is reached.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingStartingChangeStreamSegmentOpenCursorFailsWithShardNotFoundRepeatedly) {
    const Timestamp ts = Timestamp(23, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;

    // Insert as many responses as we need to nudge the stage into triggering the tassert for too
    // many consecutive "ShardNotFound" errors.
    for (int i = 0; i <= V2Stage::kMaxShardNotFoundFailuresInARow; ++i) {
        shardTargeterResponses.emplace_back(
            ts,
            ShardTargeterDecision::kContinue,
            boost::optional<Timestamp>{},
            [](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDoc,
               ChangeStreamReaderContext& readerContext) {
                readerContext.openCursorsOnDataShards(std::get<Timestamp>(tsOrDoc),
                                                      stdx::unordered_set<ShardId>{{"shardA"}});
            });
    }

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);
    docSource->setSegmentStartTimestamp_forTest(ts);

    getCursorManagerMock(params)->setThrowShardNotFoundExceptions(
        V2Stage::kMaxShardNotFoundFailuresInARow);
    for (int i = 0; i < V2Stage::kMaxShardNotFoundFailuresInARow - 1; ++i) {
        auto result = docSource->runGetNextStateMachine_forTest();
        ASSERT_FALSE(result.has_value());

        ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment,
                  docSource->getState_forTest());
        ASSERT_EQ(boost::optional<Timestamp>(), docSource->getSegmentEndTimestamp_forTest());
    }

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);
}

// Tests state machine for input state kFetching for a control event, when we try to open a new
// cursor on the config server and this fails with 'RetryChangeStream' exception.
TEST_F(DSV2StateFetchingInitializationAndStartingTest,
       StateFetchingStartingChangeStreamSegmentOpenConfigServerCursorFailsWitRetryChangeStream) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            Timestamp openTs = std::get<Timestamp>(tsOrDocument);
            context.openCursorOnConfigServer(openTs);
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Timestamp(23, 0),
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    std::deque<DocumentSource::GetNextResult> inputDocs = {};
    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(docSource, source.get());
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);
    docSource->setSegmentStartTimestamp_forTest(Timestamp(23, 0));

    // Enable 'RetryChangeStream' exception, so opening the cursor on the config server will throw.
    // This makes the stage fail.
    getCursorManagerMock(params)->setThrowRetryChangeStreamExceptions(1);

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);

    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(docSource->getNext(), AssertionException, ErrorCodes::RetryChangeStream);
}

}  // namespace
}  // namespace mongo


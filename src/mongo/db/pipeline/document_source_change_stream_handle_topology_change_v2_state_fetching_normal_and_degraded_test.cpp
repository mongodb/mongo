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

class DSV2StateFetchingNormalAndDegradedTest : public ChangeStreamStageTestNoSetup {};
using DSV2StateFetchingNormalAndDegradedTestDeathTest = DSV2StateFetchingNormalAndDegradedTest;

// Tests state machine for input state kFetchingNormalGettingChangeEvent for non-control events.
TEST_F(DSV2StateFetchingNormalAndDegradedTest,
       StateFetchingNormalGettingChangeEventNonControlEvents) {
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

    docSource->setState_forTest(V2Stage::State::kFetchingNormalGettingChangeEvent,
                                false /* validateStateTransition */);

    // Check return value 1 (pause).
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    // Check return value 2 (doc1).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(doc1, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    // Check return value 3 (pause).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    // Check return value 4 (doc2).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(doc2, result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    // Check return value 5 (eof).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetching for a control event.
TEST_F(DSV2StateFetchingNormalAndDegradedTest, StateFetchingNormalGettingChangeEventControlEvent) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event = BSON("operationType" << "test" << "foo" << "bar" << "_id"
                                               << buildHighWaterMarkToken(Timestamp(23, 1))
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

    docSource->setState_forTest(V2Stage::State::kFetchingNormalGettingChangeEvent,
                                false /* validateStateTransition */);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetching for a control event, when we try to open a new
// cursor on a shard and this fails with 'ShardNotFound' exceptions. The state then transitions into
// degraded mode.
TEST_F(
    DSV2StateFetchingNormalAndDegradedTest,
    StateFetchingNormalGettingChangeEventControlEventOpenCursorFailsWithShardNotFoundTransitionToDegradedFetching) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event =
        BSON("operationType" << "test" << "foo" << "bar" << "_id"
                             << buildHighWaterMarkToken(Timestamp(23, 1)) << "$sortKey"
                             << BSON_ARRAY(buildHighWaterMarkToken(Timestamp(23, 1)))
                             << Document::metaFieldChangeStreamControlEvent << 1);

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        Document{event},
        ShardTargeterDecision::kContinue,
        boost::optional<Timestamp>{},
        [](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDoc,
           ChangeStreamReaderContext& readerContext) {
            readerContext.openCursorsOnDataShards(
                V2Stage::extractTimestampFromDocument(std::get<Document>(tsOrDoc)),
                stdx::unordered_set<ShardId>{{"shardA"}});
        });
    shardTargeterResponses.emplace_back(
        Timestamp(23, 2), ShardTargeterDecision::kContinue, boost::none);

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
    docSource->setState_forTest(V2Stage::State::kFetchingNormalGettingChangeEvent,
                                false /* validateStateTransition */);

    // Before executing the stage, fake that there is already another data-shard cursor open.
    getCursorManagerMock(params)->openCursorsOnDataShards(
        getExpCtx(), getOpCtx(), Timestamp(23, 0), {ShardId{"shardB"}});

    // Enable 'ShardNotFound' exceptions, so opening the next cursor will throw. This makes the
    // stage go into degraded fetching mode.
    getCursorManagerMock(params)->setThrowShardNotFoundExceptions(1);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 2), *docSource->getSegmentEndTimestamp_forTest());

    // Undo mode must have been turned on when entering the degraded fetching state.
    ASSERT_TRUE(*getCursorManagerMock(params)->getUndoNextMode());

    // Calling the state machine will start a new segment.
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 3));

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 2), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>(), docSource->getSegmentEndTimestamp_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    const stdx::unordered_set<ShardId> expectedShardCursors = {ShardId("shardB")};
    ASSERT_FALSE(params->cursorManager->isCursorOnConfigServerOpen());
    ASSERT_EQ(expectedShardCursors, params->cursorManager->getCurrentlyTargetedDataShards());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 2), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>(), docSource->getSegmentEndTimestamp_forTest());
}

// Tests state machine for input state kFetching for a control event, when we try to open a new
// cursor on a shard and this fails with 'ShardNotFound' exceptions. When there are no other data
// shard cursors open, the state transitions to starting a new segment.
TEST_F(
    DSV2StateFetchingNormalAndDegradedTest,
    StateFetchingNormalGettingChangeEventControlEventOpenCursorFailsWithShardNotFoundStartNewSegment) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event =
        BSON("operationType" << "test" << "foo" << "bar" << "_id"
                             << buildHighWaterMarkToken(Timestamp(23, 1)) << "$sortKey"
                             << BSON_ARRAY(buildHighWaterMarkToken(Timestamp(23, 1)))
                             << Document::metaFieldChangeStreamControlEvent << 1);

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        Document{event},
        ShardTargeterDecision::kContinue,
        boost::optional<Timestamp>{},
        [](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDoc,
           ChangeStreamReaderContext& readerContext) {
            readerContext.closeCursorOnConfigServer();
            readerContext.openCursorsOnDataShards(
                V2Stage::extractTimestampFromDocument(std::get<Document>(tsOrDoc)),
                stdx::unordered_set<ShardId>{{"shardA"}});
        });

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

    // Before executing the stage, fake that there is already a cursor open for the config server.
    getCursorManagerMock(params)->openCursorOnConfigServer(
        getExpCtx(), getOpCtx(), Timestamp(23, 0));

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(docSource, source.get());
    docSource->setState_forTest(V2Stage::State::kFetchingNormalGettingChangeEvent,
                                false /* validateStateTransition */);

    // Enable 'ShardNotFound' exceptions, so opening the next cursor will throw. This makes the
    // stage start a new segment.
    getCursorManagerMock(params)->setThrowShardNotFoundExceptions(1);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // We must have transitioned to starting a new segment.
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>(), docSource->getSegmentEndTimestamp_forTest());

    // The cursor on the config server should have been closed, the new data shard cursor should not
    // have been opened.
    ASSERT_FALSE(params->cursorManager->isCursorOnConfigServerOpen());
    ASSERT_TRUE(params->cursorManager->getCurrentlyTargetedDataShards().empty());
}

// Tests state machine for input state kFetchingNormalGettingChangeEvent and the shard targeter
// returning 'kSwitchToV1'.
TEST_F(DSV2StateFetchingNormalAndDegradedTest,
       StateFetchingNormalGettingChangeEventShardTargeterReturnsDowngrading) {
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(23, 99);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event =
        BSON("operationType" << "test1" << "foo" << "bar" << "clusterTime" << Timestamp(23, 2));

    MutableDocument docBuilder(Document::fromBsonWithMetaData(event));
    docBuilder.metadata().setChangeStreamControlEvent();
    Document doc = docBuilder.freeze();

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(doc,
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

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makeAdvancedControlDocument(std::move(doc))};

    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(docSource, source.get());

    docSource->setState_forTest(V2Stage::State::kFetchingNormalGettingChangeEvent,
                                false /* validateStateTransition */);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kDowngrading, docSource->getState_forTest());

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);
    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(docSource->getNext(), AssertionException, ErrorCodes::RetryChangeStream);
}

// Tests state machine for input state kFetchingDegradedGettingChangeEvent for non-control events.
TEST_F(DSV2StateFetchingNormalAndDegradedTest,
       StateFetchingDegradedGettingChangeEventNonControlEvents) {
    // The change stream segments in this test are [ts(23, 0), ts(42, 1)) and [ts(42, 1), inf).
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(42, 1);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(segmentEndTimestamp,
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

    // Test that the stage returns all inputs as they are.
    const BSONObj doc1 =
        BSON("operationType" << "test1" << "foo" << "bar" << "_id"
                             << buildHighWaterMarkToken(Timestamp(23, 1)) << "$sortKey"
                             << BSON_ARRAY(buildHighWaterMarkToken(Timestamp(23, 1))));
    const BSONObj doc2 =
        BSON("operationType" << "test2" << "test" << "value" << "_id"
                             << buildHighWaterMarkToken(Timestamp(42, 1)) << "$sortKey"
                             << BSON_ARRAY(buildHighWaterMarkToken(Timestamp(42, 1))));
    const BSONObj doc3 =
        BSON("operationType" << "test3" << "test" << "value" << "_id"
                             << buildHighWaterMarkToken(Timestamp(43, 1)) << "$sortKey"
                             << BSON_ARRAY(buildHighWaterMarkToken(Timestamp(43, 1))));

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc1),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc2),
        Document::fromBsonWithMetaData(doc3),
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

    docSource->setState_forTest(V2Stage::State::kFetchingDegradedGettingChangeEvent,
                                false /* validateStateTransition */);
    docSource->setSegmentStartTimestamp_forTest(ts);
    docSource->setSegmentEndTimestamp_forTest(segmentEndTimestamp);

    // Check return value 1 (pause).
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 1));
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    // Check return value 2 (doc1).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(Document::fromBsonWithMetaData(doc1).toBson(),
                      result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    // Check return value 3 (pause).
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 2));
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    // Check return value 4 (doc2). This also transitions the state.
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(42, 1));
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isAdvanced());
    ASSERT_BSONOBJ_EQ(Document::fromBsonWithMetaData(doc2).toBson(),
                      result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());

    // Segment start timestamp should change here.
    ASSERT_EQ(Timestamp(42, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    // 'undoNextReady()' should have been called on the 'CursorManager' for this transition.
    ASSERT_TRUE(getCursorManagerMock(params)->undoGetNextCalled());
    ASSERT_EQ(Timestamp(42, 1), *getCursorManagerMock(params)->getRestoredHighWaterMark());

    // Undo mode must have been turned off when exiting the degraded fetching state.
    ASSERT_FALSE(*getCursorManagerMock(params)->getUndoNextMode());

    // Check return value 5 (doc3).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_BSONOBJ_EQ(Document::fromBsonWithMetaData(doc3).toBson(),
                      result->getDocument().toBson());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(42, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    // Check return value 6 (eof).
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(43, 2));
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(42, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());
}

// Tests state machine for input state kFetchingDegradedGettingChangeEvent for Pause and EOF events.
TEST_F(DSV2StateFetchingNormalAndDegradedTest,
       StateFetchingDegradedGettingChangeEventPauseAndEOFEvents) {
    // The change stream segments in this test are [ts(23, 0), ts(23, 1)) and [ts(23, 1), inf).
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(23, 1);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(segmentEndTimestamp,
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
        DocumentSource::GetNextResult::makePauseExecution(),
        DocumentSource::GetNextResult::makeEOF(),
        DocumentSource::GetNextResult::makePauseExecution(),
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

    // Must enable undo mode when entering the degraded fetching mode.
    getCursorManagerMock(params)->enableUndoNextMode();
    docSource->setState_forTest(V2Stage::State::kFetchingDegradedGettingChangeEvent,
                                false /* validateStateTransition */);
    docSource->setSegmentStartTimestamp_forTest(ts);
    docSource->setSegmentEndTimestamp_forTest(segmentEndTimestamp);

    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 0));

    // Check return value 1 (pause).
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isPaused());
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());

    // Undo mode must have been turned on while still in the degraded fetching state.
    ASSERT_TRUE(*getCursorManagerMock(params)->getUndoNextMode());

    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, docSource->getSegmentEndTimestamp_forTest());

    // 'undoNextReady()' should not have been called on the 'CursorManager' for pause events.
    ASSERT_FALSE(getCursorManagerMock(params)->undoGetNextCalled());

    // Check return value 2 (EOF).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());

    // Undo mode must have been turned on while still in the degraded fetching state.
    ASSERT_TRUE(*getCursorManagerMock(params)->getUndoNextMode());

    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, docSource->getSegmentEndTimestamp_forTest());

    // 'undoNextReady()' should not have been called on the 'CursorManager' for EOF events.
    ASSERT_FALSE(getCursorManagerMock(params)->undoGetNextCalled());

    // Move high water mark forward.
    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 1));

    // Consume pause event (it is not undone).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());

    // Segment start timestamp should change here.
    ASSERT_EQ(Timestamp(23, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>(), docSource->getSegmentEndTimestamp_forTest());

    // 'undoNextReady()' should not have been called.
    ASSERT_FALSE(getCursorManagerMock(params)->undoGetNextCalled());
    ASSERT_EQ(Timestamp(23, 1), getCursorManagerMock(params)->getRestoredHighWaterMark());

    // Undo mode must have been turned off when exiting the degraded fetching state.
    ASSERT_FALSE(*getCursorManagerMock(params)->getUndoNextMode());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    // Check return value 4 (EOF).
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 1), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    // No more results.
    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
}

// Tests state machine for input state kFetchingDegradedGettingChangeEvent for control events.
TEST_F(DSV2StateFetchingNormalAndDegradedTest,
       StateFetchingDegradedGettingChangeEventControlEvent) {
    // The change stream segments in this test are [ts(23, 0), ts(23, 99)) and [ts(23, 99), inf).
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(23, 99);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event1 =
        BSON("operationType" << "test1" << "foo" << "bar" << "_id"
                             << buildHighWaterMarkToken(Timestamp(23, 2)) << "$sortKey"
                             << BSON_ARRAY(buildHighWaterMarkToken(Timestamp(23, 2))));
    const BSONObj event2 =
        BSON("operationType" << "test2" << "foo" << "bar" << "_id"
                             << buildHighWaterMarkToken(Timestamp(24, 0)) << "$sortKey"
                             << BSON_ARRAY(buildHighWaterMarkToken(Timestamp(24, 0))));

    MutableDocument docBuilder(Document::fromBsonWithMetaData(event1));
    docBuilder.metadata().setChangeStreamControlEvent();
    Document doc1 = docBuilder.freeze();

    docBuilder.reset(Document::fromBsonWithMetaData(event2));
    docBuilder.metadata().setChangeStreamControlEvent();
    Document doc2 = docBuilder.freeze();

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(doc1,
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        V2StageTestHelpers::kEmptyShardTargeterCallback);
    shardTargeterResponses.emplace_back(Timestamp(23, 99),
                                        ShardTargeterDecision::kContinue,
                                        boost::optional<Timestamp>{},
                                        V2StageTestHelpers::kEmptyShardTargeterCallback);
    shardTargeterResponses.emplace_back(doc2,
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
        DocumentSource::GetNextResult::makeAdvancedControlDocument(std::move(doc1)),
        DocumentSource::GetNextResult::makeAdvancedControlDocument(std::move(doc2)),
        DocumentSource::GetNextResult::makeEOF()};

    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());
    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(docSource, source.get());

    docSource->setState_forTest(V2Stage::State::kFetchingDegradedGettingChangeEvent,
                                false /* validateStateTransition */);
    docSource->setSegmentStartTimestamp_forTest(ts);
    docSource->setSegmentEndTimestamp_forTest(segmentEndTimestamp);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(ts, *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(segmentEndTimestamp, *docSource->getSegmentEndTimestamp_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingStartingChangeStreamSegment, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 99), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    // 'undoNextReady()' should have been called on the 'CursorManager' for this transition.
    ASSERT_TRUE(getCursorManagerMock(params)->undoGetNextCalled());
    ASSERT_EQ(Timestamp(23, 99), *getCursorManagerMock(params)->getRestoredHighWaterMark());

    // Undo mode must have been turned off when exiting the degraded fetching state.
    ASSERT_FALSE(*getCursorManagerMock(params)->getUndoNextMode());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_EQ(Timestamp(23, 99), *docSource->getSegmentStartTimestamp_forTest());
    ASSERT_EQ(boost::optional<Timestamp>{}, docSource->getSegmentEndTimestamp_forTest());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_FALSE(result.has_value());

    result = docSource->runGetNextStateMachine_forTest();
    ASSERT_EQ(V2Stage::State::kFetchingNormalGettingChangeEvent, docSource->getState_forTest());
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
}

// Tests state machine for input state kFetchingDegradedGettingChangeEvent and the shard targeter
// returning 'kSwitchToV1', which it shouldn't.
DEATH_TEST_REGEX_F(DSV2StateFetchingNormalAndDegradedTestDeathTest,
                   StateFetchingDegradedGettingChangeEventShardTargeterReturnsDowngrading,
                   "Tripwire assertion.*10922904") {
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(23, 99);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    const BSONObj event =
        BSON("operationType" << "test1" << "foo" << "bar" << "_id"
                             << buildHighWaterMarkToken(Timestamp(23, 2)) << "$sortKey"
                             << BSON_ARRAY(buildHighWaterMarkToken(Timestamp(23, 2))));

    MutableDocument docBuilder(Document::fromBsonWithMetaData(event));
    docBuilder.metadata().setChangeStreamControlEvent();
    Document doc = docBuilder.freeze();

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(doc,
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

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makeAdvancedControlDocument(std::move(doc))};

    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(docSource, source.get());

    docSource->setState_forTest(V2Stage::State::kFetchingDegradedGettingChangeEvent,
                                false /* validateStateTransition */);
    docSource->setSegmentStartTimestamp_forTest(ts);
    docSource->setSegmentEndTimestamp_forTest(segmentEndTimestamp);

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10922904);
}

// Tests state machine for input state kDowngrading. The change stream is expected to fail with an
// error in this case.
TEST_F(DSV2StateFetchingNormalAndDegradedTest, StateDowngrading) {
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

    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);
    docSource->setState_forTest(V2Stage::State::kDowngrading, false /* validateStateTransition */);

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);
    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(docSource->getNext(), AssertionException, ErrorCodes::RetryChangeStream);
}

}  // namespace
}  // namespace mongo


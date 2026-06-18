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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/pipeline/change_stream_pipeline_helpers.h"
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
using namespace std::literals::string_view_literals;

using namespace test;

class DSV2StageCursorManagementAndErrorHandlingTest : public ChangeStreamStageTestNoSetup {};
using DSV2StageCursorManagementAndErrorHandlingTestDeathTest =
    DSV2StageCursorManagementAndErrorHandlingTest;

const stdx::unordered_set<ShardId> kNoShards = {};

// Tests opening cursor on the config server.
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest, OpenCursorOnConfigServer) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            Timestamp openTs = std::get<Timestamp>(tsOrDocument);
            ASSERT_EQ(ts, openTs);
            context.openCursorOnConfigServer(ts);
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(handleTopologyChangeStage, source.get());

    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());

    auto next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // Cursor must have been opened on the config server and no data shards.
    ASSERT_TRUE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());
}

// Tests closing cursor on the config server when reentering the
// "kFetchingStartingChangeStreamSegment" state.
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest,
       CloseCursorOnConfigServerWhenReenteringFetchingStartingChangeStreamSegment) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
           ChangeStreamReaderContext& context) {
            context.closeCursorOnConfigServer();
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);
    handleTopologyChangeStage->setState_forTest(
        V2Stage::State::kFetchingStartingChangeStreamSegment, false /* validateStateTransition */);
    exec::agg::MockStage::setSource_forTest(handleTopologyChangeStage, source.get());
    handleTopologyChangeStage->setSegmentStartTimestamp_forTest(ts);

    getCursorManagerMock(params)->openCursorOnConfigServer(getExpCtx(), getOpCtx(), ts);
    ASSERT_TRUE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());

    auto next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // Cursor must have been closed on the config server.
    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());
}

// Tests opening cursors on data shards.
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest, OpenCursorsOnDataShards) {
    const Timestamp ts = Timestamp(42, 0);
    const stdx::unordered_set<ShardId> shardSet = {ShardId("abc"), ShardId("xyz")};

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            Timestamp openTs = std::get<Timestamp>(tsOrDocument);
            ASSERT_EQ(ts, openTs);
            context.openCursorsOnDataShards(openTs, shardSet);
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(handleTopologyChangeStage, source.get());

    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());

    auto next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // Cursor must have been opened on the data shards, but not the config server.
    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(shardSet, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());
}

// Tests closing cursors on data shards when reentering the "kFetchingStartingChangeStreamSegment"
// state.
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest,
       CloseCursorsOnDataShardsWhenReenteringStartingChangeStreamSegment) {
    const Timestamp ts = Timestamp(42, 0);
    const stdx::unordered_set<ShardId> shardSet = {ShardId("abc"), ShardId("xyz")};

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            context.closeCursorsOnDataShards(shardSet);
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);
    handleTopologyChangeStage->setState_forTest(
        V2Stage::State::kFetchingStartingChangeStreamSegment, false /* validateStateTransition */);
    exec::agg::MockStage::setSource_forTest(handleTopologyChangeStage, source.get());
    handleTopologyChangeStage->setSegmentStartTimestamp_forTest(ts);

    getCursorManagerMock(params)->openCursorsOnDataShards(getExpCtx(), getOpCtx(), ts, shardSet);

    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(shardSet, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());

    auto next = handleTopologyChangeStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // Cursor must have been closed on the data shards, and remain closed on the config server.
    ASSERT_FALSE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_EQ(kNoShards, getCursorManagerMock(params)->getCurrentlyTargetedDataShards());
}

// Tests that a 'ShardNotFound' error is converted to a 'ShardRemovedError' in strict mode.
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest,
       ShardNotFoundErrorIsConvertedToShardRemovedErrorInStrictMode) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            error_details::throwExceptionForStatus(
                Status(ErrorCodes::ShardNotFound, "did not find shard!"));
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(handleTopologyChangeStage, source.get());

    // 'ShardNotFound' error must be translated into 'ShardRemovedError'.
    ASSERT_THROWS_CODE(
        handleTopologyChangeStage->getNext(), AssertionException, ErrorCodes::ShardRemovedError);

    ASSERT_EQ(V2Stage::State::kFinal, handleTopologyChangeStage->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(
        handleTopologyChangeStage->getNext(), AssertionException, ErrorCodes::ShardRemovedError);
}

// Tests that a 'ShardNotFound' error is converted to a 'RetryChangeStream' error in
// ignoreRemovedShards mode.
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest,
       ShardNotFoundErrorIsConvertedToRetryChangeStreamInIgnoreRemovedShardsMode) {
    const Timestamp ts = Timestamp(42, 0);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            error_details::throwExceptionForStatus(
                Status(ErrorCodes::ShardNotFound, "did not find shard!"));
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(handleTopologyChangeStage, source.get());

    // 'ShardNotFound' error must be translated into 'RetryChangeStream' error.
    ASSERT_THROWS_CODE(
        handleTopologyChangeStage->getNext(), AssertionException, ErrorCodes::RetryChangeStream);

    ASSERT_EQ(V2Stage::State::kFinal, handleTopologyChangeStage->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(
        handleTopologyChangeStage->getNext(), AssertionException, ErrorCodes::RetryChangeStream);
}

// Tests that any non-'ShardNotFound' error is rethrown as is in strict mode.
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest,
       ErrorOtherThanShardNotFoundErrorIsRethrownInStrictMode) {
    const Timestamp ts = Timestamp(42, 0);
    const auto errorCode = ErrorCodes::ChangeStreamFatalError;

    getExpCtx()->setChangeStreamSpec(buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            error_details::throwExceptionForStatus(Status(errorCode, "some error occurred"));
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(handleTopologyChangeStage, source.get());

    // The expect error code should be returned as-is.
    ASSERT_THROWS_CODE(handleTopologyChangeStage->getNext(), AssertionException, errorCode);

    ASSERT_EQ(V2Stage::State::kFinal, handleTopologyChangeStage->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(handleTopologyChangeStage->getNext(), AssertionException, errorCode);
}

// Tests that any non-'ShardNotFound' error is rethrown as is in ignoreRemovedShards mode.
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest,
       ErrorOtherThanShardNotFoundErrorIsRethrownInIgnoreRemovedShardsMode) {
    const Timestamp ts = Timestamp(42, 0);
    const auto errorCode = ErrorCodes::ChangeStreamHistoryLost;

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    ChangeStreamShardTargeterMock::ReaderContextCallback shardTargeterCallback =
        [=](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDocument,
            ChangeStreamReaderContext& context) {
            error_details::throwExceptionForStatus(Status(errorCode, "some error occurred"));
        };

    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts, ShardTargeterDecision::kContinue, boost::optional<Timestamp>{}, shardTargeterCallback);

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) {
            auto shardTargeter = std::make_unique<ChangeStreamShardTargeterMock>();
            shardTargeter->bufferResponses(shardTargeterResponses);
            return shardTargeter;
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    const BSONObj doc = BSON("operationType" << "test1" << "foo" << "bar");

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        Document::fromBsonWithMetaData(doc),
    };
    auto source = MockWithUndoStage::createForTest(inputDocs, getExpCtx());

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get(),
                               source);

    // Prepare DataToShardsAllocationQueryServiceMock.
    std::vector<DataToShardsAllocationQueryServiceMock::Response> mockResponses;
    mockResponses.push_back(std::make_pair(ts, AllocationToShardsStatus::kOk));
    getDataToShardsAllocationQueryServiceMock(params)->bufferResponses(mockResponses);

    auto handleTopologyChangeStage = make_intrusive<V2Stage>(getExpCtx(), params);
    exec::agg::MockStage::setSource_forTest(handleTopologyChangeStage, source.get());

    // The expect error code should be returned as-is.
    ASSERT_THROWS_CODE(handleTopologyChangeStage->getNext(), AssertionException, errorCode);

    ASSERT_EQ(V2Stage::State::kFinal, handleTopologyChangeStage->getState_forTest());

    // Calling 'getNext()' again should return the same error.
    ASSERT_THROWS_CODE(handleTopologyChangeStage->getNext(), AssertionException, errorCode);
}

// Tests pipeline building for config server cursor.
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest, BuildPipelineForConfigServerV2) {
    const Timestamp ts = Timestamp(123, 45);

    DocumentSourceChangeStreamSpec spec = buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict);
    spec.setVersion(ChangeStreamReaderVersionEnum::kV2);
    getExpCtx()->setChangeStreamSpec(spec);
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [=](OperationContext* opCtx, const ChangeStream& changeStream) { return nullptr; },
        [](OperationContext*, const ChangeStream&) { return BSONObj(); },
        [](OperationContext*, const ChangeStream&) { return std::set<std::string>{}; },
        [](OperationContext*, const ChangeStream&) {
            return BSON("$or" << BSON_ARRAY(BSON("o2.eventType1" << BSON("$exists" << true))
                                            << BSON("o2.eventType2" << BSON("$exists" << true))));
        },
        [](OperationContext*, const ChangeStream&) {
            return std::set<std::string>{"eventType1", "eventType2"};
        });

    std::vector<BSONObj> pipeline = change_stream::pipeline_helpers::buildPipelineForConfigServerV2(
        getExpCtx(),
        getOpCtx(),
        ts,
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        changeStream,
        changeStreamReaderBuilder.get());

    ASSERT_EQ(5, pipeline.size());

    // { $_internalChangeStreamOplogMatch: { filter: { $and: [ { ts: { $gte: Timestamp(123, 45) } },
    // { fromMigrate: { $not: { $eq: true } } }, { $or: [ { o2.eventType1: { $exists: true } }, {
    // o2.eventType2: { $exists: true } }, { $and: [ { $or: [ { o.applyOps: { $elemMatch: { $or: [ {
    // o2.eventType1: { $exists: true } }, { o2.eventType2: { $exists: true } } ] } } }, {
    // prevOpTime: { $not: { $eq: { ts: Timestamp(0, 0), t: -1 } } } } ] }, { op: { $eq: "c" } }, {
    // o.partialTxn: { $not: { $eq: true } } }, { o.prepare: { $not: { $eq: true } } }, {
    // o.applyOps: { $type: [ 4 ] } } ] }, { $and: [ { o.commitTransaction: { $eq: 1 } }, { op: {
    // $eq: "c" } } ] } ] } ] } } }
    auto elem = pipeline[0].firstElement();
    ASSERT_EQ("$_internalChangeStreamOplogMatch"sv, elem.fieldNameStringData());
    ASSERT_EQ(BSONType::object, elem.type());
    ASSERT_BSONOBJ_EQ(
        BSON(
            "filter" << BSON(
                "$and" << BSON_ARRAY(
                    BSON("ts" << BSON("$gte" << ts))
                    << BSON("fromMigrate" << BSON("$not" << BSON("$eq" << true)))
                    << BSON(
                           "$or" << BSON_ARRAY(
                               BSON("o2.eventType1" << BSON("$exists" << true))
                               << BSON("o2.eventType2" << BSON("$exists" << true))
                               << BSON("$and" << BSON_ARRAY(
                                           BSON("$or" << BSON_ARRAY(
                                                    BSON("o.applyOps" << BSON(
                                                             "$elemMatch" << BSON(
                                                                 "$or" << BSON_ARRAY(
                                                                     BSON("o2.eventType1" << BSON(
                                                                              "$exists" << true))
                                                                     << BSON("o2.eventType2"
                                                                             << BSON("$exists"
                                                                                     << true))))))
                                                    << BSON("prevOpTime"
                                                            << BSON("$not" << BSON(
                                                                        "$eq" << BSON(
                                                                            "ts" << Timestamp(0, 0)
                                                                                 << "t" << -1))))))
                                           << BSON("op" << BSON("$eq" << "c"))
                                           << BSON("o.partialTxn"
                                                   << BSON("$not" << BSON("$eq" << true)))
                                           << BSON("o.prepare"
                                                   << BSON("$not" << BSON("$eq" << true)))
                                           << BSON("o.applyOps" << BSON("$type" << BSON_ARRAY(4)))))
                               << BSON("$and"
                                       << BSON_ARRAY(BSON("o.commitTransaction" << BSON("$eq" << 1))
                                                     << BSON("op" << BSON("$eq" << "c"))))))))),
        pipeline[0].getField("$_internalChangeStreamOplogMatch").Obj());

    // { $_internalChangeStreamUnwindTransaction: { filter: { $and: [ { fromMigrate: { $not: { $eq:
    // true } } }, { $or: [ { o2.eventType1: { $exists: true } }, { o2.eventType2: { $exists: true }
    // } ] } ] } } }
    elem = pipeline[1].firstElement();
    ASSERT_EQ("$_internalChangeStreamUnwindTransaction"sv, elem.fieldNameStringData());
    ASSERT_EQ(BSONType::object, elem.type());
    ASSERT_BSONOBJ_EQ(
        BSON("filter" << BSON(
                 "$and" << BSON_ARRAY(
                     BSON("fromMigrate" << BSON("$not" << BSON("$eq" << true)))
                     << BSON("$or"
                             << BSON_ARRAY(BSON("o2.eventType1" << BSON("$exists" << true))
                                           << BSON("o2.eventType2" << BSON("$exists" << true))))))),
        pipeline[1].getField("$_internalChangeStreamUnwindTransaction").Obj());

    // { $_internalChangeStreamTransform: { startAtOperationTime: Timestamp(123, 45), fullDocument:
    // "default", fullDocumentBeforeChange: "off", allowToRunOnConfigDB: true, ignoreRemovedShards:
    // false, supportedEvents: [ "eventType1", "eventType2" ] } }
    elem = pipeline[2].firstElement();
    ASSERT_EQ("$_internalChangeStreamTransform"sv, elem.fieldNameStringData());
    ASSERT_EQ(BSONType::object, elem.type());
    ASSERT_BSONOBJ_EQ(BSON("startAtOperationTime"
                           << ts << "fullDocument" << "default" << "fullDocumentBeforeChange"
                           << "off" << "allowToRunOnConfigDB" << true << "supportedEvents"
                           << BSON_ARRAY("eventType1" << "eventType2")),
                      pipeline[2].getField("$_internalChangeStreamTransform").Obj());

    // { $_internalChangeStreamCheckResumability: {}}
    elem = pipeline[3].firstElement();
    ASSERT_EQ("$_internalChangeStreamCheckResumability"sv, elem.fieldNameStringData());
    ASSERT_EQ(BSONType::object, elem.type());

    // { $_internalChangeStreamInjectControlEvents: { actions: { eventType1:
    // "transformToControlEvent", eventType2: "transformToControlEvent" } } }
    elem = pipeline[4].firstElement();
    ASSERT_EQ("$_internalChangeStreamInjectControlEvents"sv, elem.fieldNameStringData());
    ASSERT_EQ(BSONType::object, elem.type());
    ASSERT_BSONOBJ_EQ(
        BSON("actions" << BSON("eventType1" << "transformToControlEvent" << "eventType2"
                                            << "transformToControlEvent")),
        pipeline[4].getField("$_internalChangeStreamInjectControlEvents").Obj());
}

// Tests pipeline building for config server cursor.
DEATH_TEST_REGEX_F(DSV2StageCursorManagementAndErrorHandlingTestDeathTest,
                   BuildPipelineForConfigServerWithV1ExpressionContext,
                   "Tripwire assertion.*10657555") {
    const Timestamp ts = Timestamp(123, 45);

    DocumentSourceChangeStreamSpec spec = buildChangeStreamSpec(ts, ChangeStreamReadMode::kStrict);
    spec.setVersion(ChangeStreamReaderVersionEnum::kV1);
    getExpCtx()->setChangeStreamSpec(spec);
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(getExpCtx());

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>();

    ASSERT_THROWS_CODE(change_stream::pipeline_helpers::buildPipelineForConfigServerV2(
                           getExpCtx(),
                           getOpCtx(),
                           ts,
                           NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
                           changeStream,
                           changeStreamReaderBuilder.get()),
                       AssertionException,
                       10657555);
}

// Tests that a tassert is thrown when trying to transition to kWaiting from any state other than
// kUninitialized (enforces that kWaiting is only reachable from the start state).
DEATH_TEST_REGEX_F(DSV2StageCursorManagementAndErrorHandlingTestDeathTest,
                   CheckStateTransitionToWaitingFromInvalidState,
                   "Tripwire assertion.*10657529") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), nullptr);

    for (auto state : {V2Stage::State::kFetchingInitialization,
                       V2Stage::State::kFetchingGettingChangeEvent,
                       V2Stage::State::kFetchingStartingChangeStreamSegment,
                       V2Stage::State::kFetchingNormalGettingChangeEvent,
                       V2Stage::State::kFetchingDegradedGettingChangeEvent,
                       V2Stage::State::kDowngrading}) {
        docSource->setState_forTest(state, false /* validateStateTransition */);

        ASSERT_THROWS_CODE(docSource->setState_forTest(V2Stage::State::kWaiting,
                                                       true /* validateStateTransition */),
                           AssertionException,
                           10657529);
    }
}

// Tests that a tassert is thrown when trying to transition to kFetchingInitialization from any
// state other than kUninitialized or kWaiting (initialization is a one-time phase).
DEATH_TEST_REGEX_F(DSV2StageCursorManagementAndErrorHandlingTestDeathTest,
                   CheckStateTransitionToFetchingInitializationFromInvalidState,
                   "Tripwire assertion.*10657530") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), nullptr);

    for (auto state : {V2Stage::State::kFetchingGettingChangeEvent,
                       V2Stage::State::kFetchingStartingChangeStreamSegment,
                       V2Stage::State::kFetchingNormalGettingChangeEvent,
                       V2Stage::State::kFetchingDegradedGettingChangeEvent,
                       V2Stage::State::kDowngrading}) {
        docSource->setState_forTest(state, false /* validateStateTransition */);

        ASSERT_THROWS_CODE(docSource->setState_forTest(V2Stage::State::kFetchingInitialization,
                                                       true /* validateStateTransition */),
                           AssertionException,
                           10657530);
    }
}

// Tests state machine for input state kFetchingGettingChangeEvent for a control event where the
// shard targeter returns kSwitchToV1. Expects the state to transition to kDowngrading, then to
// kFinal.
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest,
       StateFetchingGettingChangeEventControlEventSwitchToV1) {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kStrict));

    const BSONObj event = BSON("operationType" << "test" << "foo" << "bar"
                                               << Document::metaFieldChangeStreamControlEvent << 1);

    // Prepare ShardTargeterMock responses.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(Document{event},
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

    // The shard targeter returns kSwitchToV1 for the control event, so the state should
    // transition to kDowngrading and an EOF should be returned.
    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->isEOF());
    ASSERT_EQ(V2Stage::State::kDowngrading, docSource->getState_forTest());

    // Calling the state machine again must throw RetryChangeStream and transition to kFinal.
    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(),
                       AssertionException,
                       ErrorCodes::RetryChangeStream);
    ASSERT_EQ(V2Stage::State::kFinal, docSource->getState_forTest());

    // Calling 'getNext()' again must return the same error.
    ASSERT_THROWS_CODE(docSource->getNext(), AssertionException, ErrorCodes::RetryChangeStream);
}

// Tests state machine for input state kFetchingDegradedGettingChangeEvent without the segment end
// timestamp being set. This should tassert because the segment end is required to know when to
// exit degraded mode.
DEATH_TEST_REGEX_F(DSV2StageCursorManagementAndErrorHandlingTestDeathTest,
                   StateFetchingDegradedGettingChangeEventWithoutSegmentEndTimestamp,
                   "Tripwire assertion.*10657521") {
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
    docSource->setState_forTest(V2Stage::State::kFetchingDegradedGettingChangeEvent,
                                false /* validateStateTransition */);
    docSource->setSegmentStartTimestamp_forTest(ts);

    // Intentionally do not set the segment end timestamp before entering the state to trigger the
    // following tassert.
    ASSERT_FALSE(docSource->getSegmentEndTimestamp_forTest().has_value());

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657521);
}

// Tests state machine for input state kFetchingDegradedGettingChangeEvent when the shard targeter
// returns kContinue but its callback made cursor management calls, which must not happen in
// degraded mode.
DEATH_TEST_REGEX_F(DSV2StageCursorManagementAndErrorHandlingTestDeathTest,
                   StateFetchingDegradedGettingChangeEventContinueWithBufferedCursorRequests,
                   "Tripwire assertion.*10657556") {
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

    // The shard targeter returns kContinue but its callback illegally opens a data shard cursor
    // inside a degraded-mode context, which should trigger the tassert.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        doc,
        ShardTargeterDecision::kContinue,
        boost::optional<Timestamp>{},
        [](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDoc,
           ChangeStreamReaderContext& readerContext) {
            readerContext.openCursorsOnDataShards(Timestamp(23, 2),
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

    getCursorManagerMock(params)->setTimestampForCurrentHighWaterMark(Timestamp(23, 1));

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657556);
}

// Tests state machine for input state kFetchingStartingChangeStreamSegment when the shard targeter
// returns kContinue with a segment end timestamp (entering degraded mode), but the callback opened
// a config server cursor. The implementation must tassert because no config server cursor may be
// open when entering degraded mode.
DEATH_TEST_REGEX_F(DSV2StageCursorManagementAndErrorHandlingTestDeathTest,
                   StateFetchingStartingChangeStreamSegmentToDegradedWithConfigServerCursorOpen,
                   "Tripwire assertion.*12013806") {
    const Timestamp ts = Timestamp(23, 0);
    const Timestamp segmentEndTimestamp = Timestamp(42, 1);

    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(ts, ChangeStreamReadMode::kIgnoreRemovedShards));

    // The shard targeter returns kContinue with an end timestamp (degraded mode), but its callback
    // opens a cursor on the config server, violating the contract.
    std::vector<ChangeStreamShardTargeterMock::Response> shardTargeterResponses;
    shardTargeterResponses.emplace_back(
        ts,
        ShardTargeterDecision::kContinue,
        segmentEndTimestamp,
        [ts](ChangeStreamShardTargeterMock::TimestampOrDocument tsOrDoc,
             ChangeStreamReaderContext& readerContext) {
            readerContext.openCursorOnConfigServer(ts);
        });

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

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 12013806);
}

// Tests that a tassert is thrown when the state machine enters kFetchingGettingChangeEvent in
// ignoreRemovedShards mode, since that state is exclusively valid in strict mode.
DEATH_TEST_REGEX_F(DSV2StageCursorManagementAndErrorHandlingTestDeathTest,
                   StateFetchingGettingChangeEventInIgnoreRemovedShardsMode,
                   "Tripwire assertion.*10657515") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(23, 0), ChangeStreamReadMode::kIgnoreRemovedShards));

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
    docSource->setState_forTest(V2Stage::State::kFetchingGettingChangeEvent,
                                false /* validateStateTransition */);

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657515);
}

// Tests that a tassert is thrown when the state machine enters kFetchingStartingChangeStreamSegment
// in strict mode, since that state is exclusively valid in ignoreRemovedShards mode.
DEATH_TEST_REGEX_F(DSV2StageCursorManagementAndErrorHandlingTestDeathTest,
                   StateFetchingStartingChangeStreamSegmentInStrictMode,
                   "Tripwire assertion.*10657515") {
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
    docSource->setState_forTest(V2Stage::State::kFetchingStartingChangeStreamSegment,
                                false /* validateStateTransition */);
    docSource->setSegmentStartTimestamp_forTest(Timestamp(23, 0));

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657515);
}

// Tests that a tassert is thrown when the state machine enters kFetchingNormalGettingChangeEvent in
// strict mode, since that state is exclusively valid in ignoreRemovedShards mode.
DEATH_TEST_REGEX_F(DSV2StageCursorManagementAndErrorHandlingTestDeathTest,
                   StateFetchingNormalGettingChangeEventInStrictMode,
                   "Tripwire assertion.*10657515") {
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
    docSource->setState_forTest(V2Stage::State::kFetchingNormalGettingChangeEvent,
                                false /* validateStateTransition */);

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657515);
}

// Tests that a tassert is thrown when the state machine enters
// kFetchingDegradedGettingChangeEvent in strict mode, since that state is exclusively valid in
// ignoreRemovedShards mode.
DEATH_TEST_REGEX_F(DSV2StageCursorManagementAndErrorHandlingTestDeathTest,
                   StateFetchingDegradedGettingChangeEventInStrictMode,
                   "Tripwire assertion.*10657515") {
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
    docSource->setState_forTest(V2Stage::State::kFetchingDegradedGettingChangeEvent,
                                false /* validateStateTransition */);
    docSource->setSegmentStartTimestamp_forTest(Timestamp(23, 0));
    docSource->setSegmentEndTimestamp_forTest(Timestamp(42, 0));

    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657515);
}

// Tests state machine for input state kFetchingNormalGettingChangeEvent for a control event, when
// opening a new data shard cursor fails with 'ShardNotFound' and only the config server cursor is
// open (no data shard cursors). Unlike the case with no open cursors at all, the stage should
// transition to degraded mode rather than starting a new segment, because there is still a cursor
// active (the config server one).
TEST_F(DSV2StageCursorManagementAndErrorHandlingTest,
       StateFetchingNormalGettingChangeEventShardNotFoundWithOnlyConfigServerCursorOpen) {
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

    // Open only a config server cursor (no data shard cursors).
    getCursorManagerMock(params)->openCursorOnConfigServer(
        getExpCtx(), getOpCtx(), Timestamp(23, 0));
    ASSERT_TRUE(getCursorManagerMock(params)->cursorOpenedOnConfigServer());
    ASSERT_TRUE(getCursorManagerMock(params)->getCurrentlyTargetedDataShards().empty());

    // Enable 'ShardNotFound' so opening the data shard cursor will fail.
    getCursorManagerMock(params)->setThrowShardNotFoundExceptions(1);

    auto result = docSource->runGetNextStateMachine_forTest();
    ASSERT_FALSE(result.has_value());

    // The config server cursor being open makes this go to degraded mode (not start a new segment).
    ASSERT_EQ(V2Stage::State::kFetchingDegradedGettingChangeEvent, docSource->getState_forTest());

    // Segment end is set to the event's timestamp + 1.
    ASSERT_TRUE(docSource->getSegmentEndTimestamp_forTest().has_value());
    ASSERT_EQ(Timestamp(23, 2), *docSource->getSegmentEndTimestamp_forTest());

    // Undo mode must have been enabled when entering the degraded fetching state.
    ASSERT_TRUE(*getCursorManagerMock(params)->getUndoNextMode());
}

}  // namespace
}  // namespace mongo


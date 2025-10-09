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

#include "mongo/s/change_streams/collection_change_stream_shard_targeter_impl.h"

#include "mongo/db/pipeline/change_stream_reader_context_mock.h"
#include "mongo/db/pipeline/historical_placement_fetcher_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler_mock.h"
#include "mongo/s/change_streams/collection_change_stream_db_absent_state_event_handler.h"
#include "mongo/s/change_streams/collection_change_stream_db_present_state_event_handler.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <vector>

namespace mongo {
namespace {

NamespaceString makeTestNss() {
    return NamespaceString::createNamespaceString_forTest("testDb.testColl");
}

class CollectionChangeStreamShardTargeterImplFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
        _fetcher = std::make_unique<HistoricalPlacementFetcherMock>();
        _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(ChangeStream(
            ChangeStreamReadMode::kStrict, ChangeStreamType::kCollection, makeTestNss()));
        _targeter = std::make_unique<CollectionChangeStreamShardTargeterImpl>(std::move(_fetcher));
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }
    CollectionChangeStreamShardTargeterImpl& targeter() {
        return *_targeter;
    }
    ChangeStreamReaderContextMock& readerCtx() {
        return *_readerCtx;
    }
    HistoricalPlacementFetcherMock& fetcher() {
        return dynamic_cast<HistoricalPlacementFetcherMock&>(
            targeter().getHistoricalPlacementFetcher());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ChangeStreamReaderContextMock> _readerCtx;
    std::unique_ptr<CollectionChangeStreamShardTargeterImpl> _targeter;
    std::unique_ptr<HistoricalPlacementFetcherMock> _fetcher;
};

TEST_F(CollectionChangeStreamShardTargeterImplFixture,
       Given_PlacementNotAvailable_When_Initialize_Then_ReturnsSwitchToV1AndDoesNotOpenCursors) {
    Timestamp clusterTime(10, 1);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::NotAvailable)}};
    fetcher().bufferResponses(responses);

    auto result = targeter().initialize(opCtx(), clusterTime, readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kSwitchToV1);
    ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    ASSERT_TRUE(readerCtx().openCursorOnConfigServerCalls.empty());
}

DEATH_TEST_REGEX_F(CollectionChangeStreamShardTargeterImplFixture,
                   Given_FuturePlacement_When_Initialize_Then_Throws,
                   "Tripwire assertion.*10720100") {
    Timestamp clusterTime(20, 1);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)}};
    fetcher().bufferResponses(responses);

    targeter().initialize(opCtx(), clusterTime, readerCtx());
}

TEST_F(
    CollectionChangeStreamShardTargeterImplFixture,
    Given_NoShardsInPlacement_When_Initialize_Then_ConfigsvrCursorOpenedAndEventHandlerSetToDbAbsent) {
    Timestamp clusterTime(11, 2);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = targeter().initialize(opCtx(), clusterTime, readerCtx());

    ASSERT_EQ(result, ShardTargeterDecision::kContinue);
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls[0].atClusterTime, clusterTime);
    ASSERT(dynamic_cast<ChangeStreamShardTargeterDbAbsentStateEventHandler*>(
        targeter().getEventHandler()));
}

TEST_F(
    CollectionChangeStreamShardTargeterImplFixture,
    Given_ActiveShardsInPlacement_When_Initialize_Then_OpensDataShardCursorsAndEventHandlerSetToDbPresent) {
    Timestamp clusterTime(13, 5);
    std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardB")};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = targeter().initialize(opCtx(), clusterTime, readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
    ASSERT(dynamic_cast<ChangeStreamShardTargeterDbPresentStateEventHandler*>(
        targeter().getEventHandler()));
}

DEATH_TEST_REGEX_F(CollectionChangeStreamShardTargeterImplFixture,
                   Given_NoEventHandlerSet_When_HandleEvent_Then_Throws,
                   "Tripwire assertion.*10720101") {
    Document event(BSON("operationType" << MoveChunkControlEvent::opType));
    targeter().handleEvent(opCtx(), event, readerCtx());
}

TEST_F(CollectionChangeStreamShardTargeterImplFixture,
       Given_HandlerSet_When_HandleEvent_Then_ParsesEventAndDelegatesToHandler) {
    targeter().setEventHandler(std::make_unique<ChangeStreamShardTargeterEventHandlerMock>(
        ShardTargeterDecision::kContinue, ShardTargeterDecision::kContinue));
    auto& eventHandlerMock =
        dynamic_cast<ChangeStreamShardTargeterEventHandlerMock&>(*targeter().getEventHandler());

    Document moveChunkEvent = Document(
        BSON("operationType" << MoveChunkControlEvent::opType << "clusterTime" << Timestamp()
                             << "operationDescription"
                             << BSON("donor" << "shardA" << "recipient" << "shardB"
                                             << "allCollectionChunksMigratedFromDonor" << true)));
    auto controlEvent = parseControlEvent(moveChunkEvent);
    targeter().handleEvent(opCtx(), moveChunkEvent, readerCtx());
    ASSERT_EQ(eventHandlerMock.calls.size(), 1);
    ASSERT_EQ(eventHandlerMock.normalCallCount, 1);
    ASSERT_EQ(eventHandlerMock.degradedCallCount, 0);

    {
        ChangeStreamShardTargeterEventHandlerMock::Call expectedCall{false /* degradedMode */,
                                                                     controlEvent};
        ASSERT_EQ(eventHandlerMock.calls[0], expectedCall);
    }

    // Now test degraded mode delegation
    readerCtx().setDegradedMode(true);
    targeter().handleEvent(opCtx(), moveChunkEvent, readerCtx());
    ASSERT_EQ(eventHandlerMock.calls.size(), 2);
    ASSERT_EQ(eventHandlerMock.normalCallCount, 1);
    ASSERT_EQ(eventHandlerMock.degradedCallCount, 1);

    {
        ChangeStreamShardTargeterEventHandlerMock::Call expectedCall{true /* degradedMode */,
                                                                     controlEvent};
        ASSERT_EQ(eventHandlerMock.calls[1], expectedCall);
    }
}

DEATH_TEST_REGEX_F(CollectionChangeStreamShardTargeterImplFixture,
                   When_StartChangeStreamSegmentIsCalled_Then_Throws,
                   "Tripwire assertion.*10783902") {
    targeter().startChangeStreamSegment(opCtx(), Timestamp(99, 0), readerCtx());
}

}  // namespace
}  // namespace mongo

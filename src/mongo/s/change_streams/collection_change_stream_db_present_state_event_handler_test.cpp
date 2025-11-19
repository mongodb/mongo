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

#include "mongo/s/change_streams/collection_change_stream_db_present_state_event_handler.h"

#include "mongo/db/pipeline/change_stream_reader_context_mock.h"
#include "mongo/db/pipeline/historical_placement_fetcher_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler_mock.h"
#include "mongo/s/change_streams/collection_change_stream_db_absent_state_event_handler.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/optional.hpp>

namespace mongo {
namespace {

NamespaceString makeTestNss() {
    return NamespaceString::createNamespaceString_forTest("testDb.testColl");
}

class CollectionDbPresentStateEventHandlerFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        _opCtx = makeOperationContext();
        _ctx = std::make_unique<ChangeStreamShardTargeterStateEventHandlingContextMock>(
            std::make_unique<HistoricalPlacementFetcherMock>());
        _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(ChangeStream(
            ChangeStreamReadMode::kStrict, ChangeStreamType::kCollection, makeTestNss()));
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    CollectionChangeStreamShardTargeterDbPresentStateEventHandler& handler() {
        return _handler;
    }

    ChangeStreamShardTargeterStateEventHandlingContextMock& ctx() {
        return *_ctx;
    }

    ChangeStreamReaderContextMock& readerCtx() {
        return *_readerCtx;
    }

    HistoricalPlacementFetcherMock& fetcher() {
        return dynamic_cast<HistoricalPlacementFetcherMock&>(_ctx->getHistoricalPlacementFetcher());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    CollectionChangeStreamShardTargeterDbPresentStateEventHandler _handler;
    std::unique_ptr<ChangeStreamShardTargeterStateEventHandlingContextMock> _ctx;
    std::unique_ptr<ChangeStreamReaderContextMock> _readerCtx;
};

using CollectionDbPresentStateEventHandlerFixtureDeathTest =
    CollectionDbPresentStateEventHandlerFixture;
DEATH_TEST_REGEX_F(CollectionDbPresentStateEventHandlerFixtureDeathTest,
                   Given_DatabaseCreatedControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*IllegalOperation") {
    handler().handleEvent(opCtx(), DatabaseCreatedControlEvent{}, ctx(), readerCtx());
}

TEST_F(
    CollectionDbPresentStateEventHandlerFixture,
    Given_MoveChunkControlEventToShardNotYetOpened_When_HandleEventIsCalled_Then_OpensCursorOnToShard) {
    Timestamp clusterTime(101, 3);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, false /* allCollectionChunksMigratedFromDonor */};

    readerCtx().currentlyTargetedShards = {shardA};

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet,
              stdx::unordered_set<ShardId>{shardB});
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
    ASSERT_TRUE(readerCtx().closeCursorsOnDataShardsCalls.empty());
}

TEST_F(
    CollectionDbPresentStateEventHandlerFixture,
    Given_MoveChunkControlEventAllChunksMigrated_When_HandleEventIsCalled_Then_ClosesCursorOnFromShard) {
    Timestamp clusterTime(102, 3);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, true /* allCollectionChunksMigratedFromDonor */};

    readerCtx().currentlyTargetedShards = {shardA, shardB};

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    // Confirm close occurred on "shardA".
    ASSERT_FALSE(readerCtx().closeCursorsOnDataShardsCalls.empty());
    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls[0].shardSet,
              stdx::unordered_set<ShardId>{shardA});
}

DEATH_TEST_REGEX_F(
    CollectionDbPresentStateEventHandlerFixtureDeathTest,
    Given_MoveChunkControlEventAllChunksMigratedAndCursorNotOpened_When_HandleEventIsCalled_Then_Throws,
    "Tripwire assertion.*10917003") {
    Timestamp clusterTime(102, 3);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, true /* allCollectionChunksMigratedFromDonor */};
    readerCtx().currentlyTargetedShards = {shardB};

    handler().handleEvent(opCtx(), event, ctx(), readerCtx());
}

TEST_F(
    CollectionDbPresentStateEventHandlerFixture,
    Given_MovePrimaryControlEventWithPlacementNotAvailable_When_HandleEventIsCalled_Then_ReturnSwitchToV1) {
    Timestamp clusterTime(20, 1);
    MovePrimaryControlEvent event{clusterTime};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::NotAvailable)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kSwitchToV1);
    ASSERT_TRUE(readerCtx().closeCursorsOnDataShardsCalls.empty());
    ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    ASSERT_TRUE(ctx().setHandlerCalls.empty());
}

DEATH_TEST_REGEX_F(
    CollectionDbPresentStateEventHandlerFixtureDeathTest,
    Given_MovePrimaryControlEventWithPlacementInFuture_When_HandleEventIsCalled_Then_Throws,
    "Tripwire assertion.*10917001") {
    Timestamp clusterTime(101, 0);
    MovePrimaryControlEvent event;
    event.clusterTime = clusterTime;

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)}};
    fetcher().bufferResponses(responses);

    handler().handleEvent(opCtx(), event, ctx(), readerCtx());
}

TEST_F(CollectionDbPresentStateEventHandlerFixture,
       Given_MovePrimaryControlEventWithShards_When_HandleEventIsCalled_Then_CursorsAreUpdated) {
    Timestamp clusterTime(60, 10);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MovePrimaryControlEvent event{clusterTime, shardA, shardB};

    readerCtx().currentlyTargetedShards = {shardA};

    std::vector<ShardId> shards = {shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    // Expect 'shardB' to be opened and 'shardA' to be closed.
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet,
              stdx::unordered_set<ShardId>{shardB});

    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls[0].shardSet,
              stdx::unordered_set<ShardId>{shardA});
}

TEST_F(
    CollectionDbPresentStateEventHandlerFixture,
    Given_NamespacePlacementChangedControlEventWithShards_When_HandleEventIsCalled_Then_CursorsAreUpdated) {
    Timestamp clusterTime(60, 10);
    NamespacePlacementChangedControlEvent event{clusterTime, makeTestNss()};

    ShardId shardA("shardA");
    ShardId shardB("shardB");
    ShardId shardC("shardC");
    ShardId shardD("shardD");
    readerCtx().currentlyTargetedShards = {shardA, shardD};

    std::vector<ShardId> shards = {shardA, shardB, shardC};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    // Expect 'shardB' to be opened and 'shardA' to be closed.
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet,
              (stdx::unordered_set<ShardId>{shardB, shardC}));

    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls[0].shardSet,
              (stdx::unordered_set<ShardId>{shardD}));
}

TEST_F(
    CollectionDbPresentStateEventHandlerFixture,
    Given_NamespacePlacementChangedControlEventWithEmptyPlacement_When_HandleEventIsCalled_Then_ConfigsvrCursorOpenedAndHandlerIsSet) {
    Timestamp clusterTime(250, 3);
    NamespacePlacementChangedControlEvent event{clusterTime};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    // Ensure cursor on configsvr is opened.
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);

    // Ensure that the event handler changed to DbAbsent for collection change streams.
    ASSERT_EQ(ctx().setHandlerCalls.size(), 1);
    ASSERT_TRUE(dynamic_cast<CollectionChangeStreamShardTargeterDbAbsentStateEventHandler*>(
        ctx().lastSetEventHandler()));
}

DEATH_TEST_REGEX_F(CollectionDbPresentStateEventHandlerFixtureDeathTest,
                   When_HandleEventInDegradedModeIsCalled_Then_AlwaysThrows,
                   "Tripwire assertion.*10917000") {
    handler().handleEventInDegradedMode(opCtx(), MovePrimaryControlEvent{}, ctx(), readerCtx());
}

}  // namespace
}  // namespace mongo

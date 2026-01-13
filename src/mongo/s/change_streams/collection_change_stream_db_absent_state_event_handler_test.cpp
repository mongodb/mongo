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

#include "mongo/s/change_streams/collection_change_stream_db_absent_state_event_handler.h"

#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/change_stream_reader_context_mock.h"
#include "mongo/db/pipeline/historical_placement_fetcher_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler_mock.h"
#include "mongo/s/change_streams/collection_change_stream_db_present_state_event_handler.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
namespace {
NamespaceString makeTestNss() {
    return NamespaceString::createNamespaceString_forTest("testDb.testColl");
}

template <ChangeStreamReadMode ReadMode>
class CollectionDbAbsentStateEventHandlerFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        _opCtx = makeOperationContext();
        _ctx = std::make_unique<ChangeStreamShardTargeterStateEventHandlingContextMock>(
            std::make_unique<HistoricalPlacementFetcherMock>());
        if constexpr (ReadMode == ChangeStreamReadMode::kStrict) {
            _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(ChangeStream(
                ChangeStreamReadMode::kStrict, ChangeStreamType::kCollection, makeTestNss()));
        } else {
            _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(
                ChangeStream(ChangeStreamReadMode::kIgnoreRemovedShards,
                             ChangeStreamType::kCollection,
                             makeTestNss()));
        }
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    CollectionChangeStreamShardTargeterDbAbsentStateEventHandler& handler() {
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
    CollectionChangeStreamShardTargeterDbAbsentStateEventHandler _handler;
    std::unique_ptr<ChangeStreamShardTargeterStateEventHandlingContextMock> _ctx;
    std::unique_ptr<ChangeStreamReaderContextMock> _readerCtx;
};

using CollectionDbAbsentStateEventHandlerStrictModeFixture =
    CollectionDbAbsentStateEventHandlerFixture<ChangeStreamReadMode::kStrict>;
using CollectionDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixture =
    CollectionDbAbsentStateEventHandlerFixture<ChangeStreamReadMode::kIgnoreRemovedShards>;

using CollectionDbAbsentStateEventHandlerStrictModeFixtureDeathTest =
    CollectionDbAbsentStateEventHandlerStrictModeFixture;
using CollectionDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest =
    CollectionDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixture;

// Tests for strict mode.
// ----------------------

DEATH_TEST_REGEX_F(CollectionDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
                   Given_MovePrimaryControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*IllegalOperation") {
    handler().handleEvent(opCtx(), MovePrimaryControlEvent{}, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(CollectionDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
                   Given_MoveChunkControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*IllegalOperation") {
    handler().handleEvent(opCtx(), MoveChunkControlEvent{}, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(CollectionDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
                   Given_NamespacePlacementChangedControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*IllegalOperation") {
    handler().handleEvent(opCtx(), NamespacePlacementChangedControlEvent{}, ctx(), readerCtx());
}

TEST_F(
    CollectionDbAbsentStateEventHandlerStrictModeFixture,
    Given_DatabaseCreatedControlEventWithPlacementNotAvailable_When_HandleEventIsCalled_Then_ReturnSwitchToV1) {
    Timestamp clusterTime(10, 2);
    DatabaseCreatedControlEvent event;
    event.clusterTime = clusterTime;

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::NotAvailable)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());

    ASSERT_EQ(result, ShardTargeterDecision::kSwitchToV1);
    ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 0);
    ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    ASSERT_TRUE(ctx().setHandlerCalls.empty());
}

DEATH_TEST_REGEX_F(
    CollectionDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
    Given_DatabaseCreatedControlEventWithPlacementInFuture_When_HandleEventIsCalled_Then_Throws,
    "Tripwire assertion.*10917001") {
    Timestamp clusterTime(100, 0);
    DatabaseCreatedControlEvent event;
    event.clusterTime = clusterTime;

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)}};
    fetcher().bufferResponses(responses);

    handler().handleEvent(opCtx(), event, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(
    CollectionDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
    Given_DatabaseCreatedControlEventWithEmptyPlacement_When_HandleEventIsCalled_Then_Throws,
    "Tripwire assertion.*10915201") {
    Timestamp clusterTime(50, 1);
    DatabaseCreatedControlEvent event;
    event.clusterTime = clusterTime;

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    handler().handleEvent(opCtx(), event, ctx(), readerCtx());
}

TEST_F(
    CollectionDbAbsentStateEventHandlerStrictModeFixture,
    Given_DatabaseCreatedControlEventWithShards_When_HandleEventIsCalled_Then_ConfigsvrCursorClosedDataShardCursorsOpenedAndHandlerIsSet) {
    Timestamp clusterTime(77, 0);
    DatabaseCreatedControlEvent event;
    event.clusterTime = clusterTime;
    std::vector<ShardId> shards = {ShardId("shardA"), ShardId("shardB")};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);

    ASSERT_EQ(ctx().setHandlerCalls.size(), 1);
    ASSERT_TRUE(dynamic_cast<CollectionChangeStreamShardTargeterDbPresentStateEventHandler*>(
        ctx().lastSetEventHandler()));
}

// Tests for ignoreRemovedShards mode.
// -----------------------------------

DEATH_TEST_REGEX_F(CollectionDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_MovePrimaryControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*IllegalOperation") {
    handler().handleEvent(opCtx(), MovePrimaryControlEvent{}, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(CollectionDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_MoveChunkControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*IllegalOperation") {
    handler().handleEvent(opCtx(), MoveChunkControlEvent{}, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(CollectionDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_NamespacePlacementChangedControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*IllegalOperation") {
    handler().handleEvent(opCtx(), NamespacePlacementChangedControlEvent{}, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(CollectionDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_NonDegradedMode_When_HandleEventInDegradedModeIsCalled_Then_Throws,
                   "Tripwire assertion.*10922908") {
    Timestamp clusterTime(101, 0);
    MovePrimaryControlEvent event;
    event.clusterTime = clusterTime;

    readerCtx().setDegradedMode(false);
    handler().handleEventInDegradedMode(opCtx(), event, ctx(), readerCtx());
}

TEST_F(
    CollectionDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixture,
    Given_DatabaseCreatedControlEventWithShards_When_HandleEventIsCalled_Then_ConfigsvrCursorClosedDataShardCursorsOpenedAndHandlerIsSet) {
    Timestamp clusterTime(77, 0);
    DatabaseCreatedControlEvent event;
    event.clusterTime = clusterTime;
    std::vector<ShardId> shards = {ShardId("shardA"), ShardId("shardB")};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    readerCtx().setDegradedMode(true);
    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);

    ASSERT_EQ(ctx().setHandlerCalls.size(), 1);
    ASSERT_TRUE(dynamic_cast<CollectionChangeStreamShardTargeterDbPresentStateEventHandler*>(
        ctx().lastSetEventHandler()));
}

DEATH_TEST_REGEX_F(
    CollectionDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
    When_HandleEventInDegradedModeIsCalledForDatabaseCreated_Then_DoesNotModifyCursors,
    "Tripwire assertion.*10922908") {
    readerCtx().setDegradedMode(true);
    handler().handleEventInDegradedMode(opCtx(), DatabaseCreatedControlEvent{}, ctx(), readerCtx());
}

}  // namespace
}  // namespace mongo

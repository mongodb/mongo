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

#include "mongo/s/change_streams/database_change_stream_shard_targeter_impl.h"

#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/change_stream_reader_context_mock.h"
#include "mongo/db/pipeline/historical_placement_fetcher_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/change_streams/change_stream_db_absent_state_event_handler.h"
#include "mongo/s/change_streams/change_stream_db_present_state_event_handler.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler_mock.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

namespace mongo {
namespace {

NamespaceString makeTestNss() {
    return NamespaceString::createNamespaceString_forTest("testDb");
}

template <ChangeStreamReadMode ReadMode>
class DatabaseChangeStreamShardTargeterImplFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
        _fetcher = std::make_unique<HistoricalPlacementFetcherMock>();
        if constexpr (ReadMode == ChangeStreamReadMode::kStrict) {
            _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(ChangeStream(
                ChangeStreamReadMode::kStrict, ChangeStreamType::kDatabase, makeTestNss()));
        } else {
            _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(
                ChangeStream(ChangeStreamReadMode::kIgnoreRemovedShards,
                             ChangeStreamType::kDatabase,
                             makeTestNss()));
        }
        _targeter = std::make_unique<DatabaseChangeStreamShardTargeterImpl>(std::move(_fetcher));
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    const DatabaseChangeStreamShardTargeterImpl& targeter() const {
        return *_targeter;
    }

    DatabaseChangeStreamShardTargeterImpl& targeter() {
        return *_targeter;
    }

    ChangeStreamReaderContextMock& readerCtx() {
        return *_readerCtx;
    }

    HistoricalPlacementFetcherMock& fetcher() {
        return dynamic_cast<HistoricalPlacementFetcherMock&>(
            targeter().getHistoricalPlacementFetcher());
    }

    bool isInDbAbsentState() const {
        return targeter().getEventHandler_forTest().getDbPresenceState() ==
            ChangeStreamShardTargeterStateEventHandler::DbPresenceState::kDbAbsent;
    }

    bool isInDbPresentState() const {
        return targeter().getEventHandler_forTest().getDbPresenceState() ==
            ChangeStreamShardTargeterStateEventHandler::DbPresenceState::kDbPresent;
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ChangeStreamReaderContextMock> _readerCtx;
    std::unique_ptr<DatabaseChangeStreamShardTargeterImpl> _targeter;
    std::unique_ptr<HistoricalPlacementFetcherMock> _fetcher;
};

using DatabaseChangeStreamShardTargeterImplStrictModeFixture =
    DatabaseChangeStreamShardTargeterImplFixture<ChangeStreamReadMode::kStrict>;
using DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture =
    DatabaseChangeStreamShardTargeterImplFixture<ChangeStreamReadMode::kIgnoreRemovedShards>;

using DatabaseChangeStreamShardTargeterImplStrictModeFixtureDeathTest =
    DatabaseChangeStreamShardTargeterImplStrictModeFixture;
using DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest =
    DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture;

// Tests for strict mode.
// ----------------------

DEATH_TEST_REGEX_F(DatabaseChangeStreamShardTargeterImplStrictModeFixtureDeathTest,
                   Given_StrictMode_When_StartChangeStreamSegment_Then_Throws,
                   "Tripwire assertion.*10922911") {
    Timestamp clusterTime(10, 1);
    targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
}

TEST_F(DatabaseChangeStreamShardTargeterImplStrictModeFixture,
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

DEATH_TEST_REGEX_F(DatabaseChangeStreamShardTargeterImplStrictModeFixtureDeathTest,
                   Given_FuturePlacement_When_Initialize_Then_Throws,
                   "Tripwire assertion.*10720100") {
    Timestamp clusterTime(20, 1);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)}};
    fetcher().bufferResponses(responses);

    targeter().initialize(opCtx(), clusterTime, readerCtx());
}

TEST_F(
    DatabaseChangeStreamShardTargeterImplStrictModeFixture,
    Given_NoShardsInPlacement_When_Initialize_Then_ConfigsvrCursorOpenedAndEventHandlerSetToDbAbsent) {
    Timestamp clusterTime(11, 2);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = targeter().initialize(opCtx(), clusterTime, readerCtx());

    ASSERT_EQ(result, ShardTargeterDecision::kContinue);
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls[0].atClusterTime, clusterTime);
    ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    ASSERT_TRUE(isInDbAbsentState());
}

TEST_F(
    DatabaseChangeStreamShardTargeterImplStrictModeFixture,
    Given_ActiveShardsInPlacement_When_Initialize_Then_OpensDataShardCursorsAndEventHandlerSetToDbPresent) {
    Timestamp clusterTime(13, 5);
    std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardB")};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = targeter().initialize(opCtx(), clusterTime, readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    ASSERT_TRUE(readerCtx().openCursorOnConfigServerCalls.empty());
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
    ASSERT_TRUE(isInDbPresentState());
}

DEATH_TEST_REGEX_F(DatabaseChangeStreamShardTargeterImplStrictModeFixtureDeathTest,
                   Given_NoEventHandlerSet_When_HandleEvent_Then_Throws,
                   "Tripwire assertion.*10720101") {
    Document event(BSON("operationType" << MoveChunkControlEvent::opType));
    targeter().handleEvent(opCtx(), event, readerCtx());
}

TEST_F(DatabaseChangeStreamShardTargeterImplStrictModeFixture,
       Given_HandlerSet_When_HandleEvent_Then_ParsesEventAndDelegatesToHandler) {
    targeter().setEventHandler(std::make_unique<ChangeStreamShardTargeterEventHandlerMock>(
        ShardTargeterDecision::kContinue, ShardTargeterDecision::kContinue));
    auto& eventHandlerMock = dynamic_cast<ChangeStreamShardTargeterEventHandlerMock&>(
        targeter().getEventHandler_forTest());

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

// Tests for ignoreRemovedShards mode.
// -----------------------------------

DEATH_TEST_REGEX_F(DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_IgnoreRemovedShardsMode_When_Initialize_Then_Throws,
                   "Tripwire assertion.*10922910") {
    Timestamp clusterTime(10, 1);
    targeter().initialize(opCtx(), clusterTime, readerCtx());
}

DEATH_TEST_REGEX_F(DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_FuturePlacement_When_StartChangeStreamSegment_Then_Throws,
                   "Tripwire assertion.*10917001") {
    Timestamp clusterTime(20, 1);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)}};
    fetcher().bufferResponses(responses);

    targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
}

DEATH_TEST_REGEX_F(DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_OpenCursorAtBeforeAtClusterTime_When_StartChangeStreamSegment_Then_Throws,
                   "Tripwire assertion.*10922901") {
    std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardB")};

    Timestamp clusterTime(20, 1);
    HistoricalPlacement placement;
    placement.setStatus(HistoricalPlacementStatus::OK);
    placement.setShards(shards);
    placement.setOpenCursorAt(Timestamp(19, 23));
    std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
    fetcher().bufferResponses(responses);

    targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
}

DEATH_TEST_REGEX_F(
    DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
    Given_NextPlacementChangedAtBeforeOpenCursorAt_When_StartChangeStreamSegment_Then_Throws,
    "Tripwire assertion.*10922902") {
    std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardB")};

    Timestamp clusterTime(20, 1);
    HistoricalPlacement placement;
    placement.setStatus(HistoricalPlacementStatus::OK);
    placement.setShards(shards);
    placement.setOpenCursorAt(Timestamp(21, 0));
    placement.setNextPlacementChangedAt(Timestamp(20, 99));
    std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
    fetcher().bufferResponses(responses);

    targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
}

DEATH_TEST_REGEX_F(
    DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
    Given_NextPlacementChangedAtWithoutShards_When_StartChangeStreamSegment_Then_Throws,
    "Tripwire assertion.*10922913") {
    Timestamp clusterTime(20, 1);
    HistoricalPlacement placement;
    placement.setStatus(HistoricalPlacementStatus::OK);
    placement.setOpenCursorAt(Timestamp(21, 0));
    placement.setNextPlacementChangedAt(Timestamp(23, 99));
    std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
    fetcher().bufferResponses(responses);

    targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
}

TEST_F(
    DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
    Given_PlacementNotAvailable_When_StartChangeStreamSegment_Then_ReturnsSwitchToV1AndDoesNotOpenCursors) {
    Timestamp clusterTime(10, 1);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::NotAvailable)}};
    fetcher().bufferResponses(responses);

    auto result = targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
    ASSERT_EQ(result.first, ShardTargeterDecision::kSwitchToV1);
    ASSERT_EQ(result.second, boost::none);
    ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    ASSERT_TRUE(readerCtx().openCursorOnConfigServerCalls.empty());
}

TEST_F(DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
       Given_PlacementWithoutShards_When_StartChangeStreamSegment_Then_OpensCursorOnConfigSvr) {
    Timestamp clusterTime(20, 1);

    HistoricalPlacement placement;
    placement.setStatus(HistoricalPlacementStatus::OK);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
    fetcher().bufferResponses(responses);

    auto result = targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
    ASSERT_EQ(result.first, ShardTargeterDecision::kContinue);
    ASSERT_EQ(result.second, boost::none);
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls[0].atClusterTime, clusterTime);
    ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    ASSERT_TRUE(isInDbAbsentState());
}

TEST_F(DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
       Given_PlacementWithShards_When_StartChangeStreamSegment_Then_OpensCursorOnDataShards) {
    std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardB")};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    Timestamp clusterTime(20, 1);

    HistoricalPlacement placement;
    placement.setStatus(HistoricalPlacementStatus::OK);
    placement.setShards(shards);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
    fetcher().bufferResponses(responses);

    auto result = targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
    ASSERT_EQ(result.first, ShardTargeterDecision::kContinue);
    ASSERT_EQ(result.second, boost::none);
    ASSERT_TRUE(readerCtx().openCursorOnConfigServerCalls.empty());
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
    ASSERT_TRUE(isInDbPresentState());
}

TEST_F(
    DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
    Given_PlacementWithShardsAndUnboundedSegmentInFuture_When_StartChangeStreamSegment_Then_OpensCursorOnDataShardsAtSegmentStart) {
    std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardC")};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    Timestamp clusterTime(20, 1);
    Timestamp segmentStart = Timestamp(23, 42);

    HistoricalPlacement placement;
    placement.setStatus(HistoricalPlacementStatus::OK);
    placement.setShards(shards);
    placement.setOpenCursorAt(segmentStart);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
    fetcher().bufferResponses(responses);

    auto result = targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
    ASSERT_EQ(result.first, ShardTargeterDecision::kContinue);
    ASSERT_EQ(result.second, boost::none);
    ASSERT_TRUE(readerCtx().openCursorOnConfigServerCalls.empty());
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, segmentStart);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
    ASSERT_TRUE(isInDbPresentState());
}

TEST_F(
    DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
    Given_PlacementWithShardsAndBoundedSegment_When_StartChangeStreamSegment_Then_OpensCursorOnDataShardsAtSegmentStart) {
    std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardC")};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    Timestamp clusterTime(20, 1);
    Timestamp segmentStart = clusterTime;
    Timestamp segmentEnd = Timestamp(21, 99);

    HistoricalPlacement placement;
    placement.setStatus(HistoricalPlacementStatus::OK);
    placement.setShards(shards);
    placement.setOpenCursorAt(segmentStart);
    placement.setNextPlacementChangedAt(segmentEnd);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
    fetcher().bufferResponses(responses);

    auto result = targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
    ASSERT_EQ(result.first, ShardTargeterDecision::kContinue);
    ASSERT_EQ(result.second, segmentEnd);
    ASSERT_TRUE(readerCtx().openCursorOnConfigServerCalls.empty());
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, segmentStart);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
    ASSERT_TRUE(isInDbPresentState());
}

TEST_F(DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
       Given_CursorIsOpenOnConfigShard_When_HandleEvent_Then_OpensCursorOnDataShards) {
    // First invocation of 'startChangeStreamSegment()' opens a cursor on the config server.
    {
        Timestamp clusterTime(20, 1);

        HistoricalPlacement placement;
        placement.setStatus(HistoricalPlacementStatus::OK);
        std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
        fetcher().bufferResponses(responses);

        auto result = targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
        ASSERT_EQ(result.first, ShardTargeterDecision::kContinue);
        ASSERT_EQ(result.second, boost::none);
        ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
        ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls[0].atClusterTime, clusterTime);
        ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
        ASSERT_TRUE(isInDbAbsentState());

        readerCtx().reset();
    }

    // Simulate receiving a 'DatabaseCreated' event. This should close the cursor on the config
    // server and open the cursors on the data shards.
    {
        std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardB")};
        stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
        Timestamp clusterTime = Timestamp(21, 10);
        HistoricalPlacement placement;
        placement.setStatus(HistoricalPlacementStatus::OK);
        placement.setShards(shards);
        std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
        fetcher().bufferResponses(responses);

        DatabaseName dbName = DatabaseName::kConfig;
        Document event = Document(BSON(
            "operationType" << DatabaseCreatedControlEvent::opType << "clusterTime" << clusterTime
                            << "fullDocument" << BSON("_id" << dbName.toString_forTest())));
        auto result = targeter().handleEvent(opCtx(), event, readerCtx());
        ASSERT_EQ(result, ShardTargeterDecision::kContinue);
        ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 1);
        ASSERT_TRUE(readerCtx().openCursorOnConfigServerCalls.empty());
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
        ASSERT_TRUE(isInDbPresentState());
    }
}

TEST_F(
    DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
    Given_MultipleChangeStreamSegments_When_StartChangeStreamSegment_Then_OpensCursorOnDataShardsAtSegmentStart) {
    // First invocation of 'startChangeStreamSegment()' opens cursors on three shards.
    {
        std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardB"), ShardId("shardC")};
        stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

        Timestamp clusterTime(20, 1);
        Timestamp segmentStart = Timestamp(23, 42);
        Timestamp segmentEnd = Timestamp(23, 99);

        HistoricalPlacement placement;
        placement.setStatus(HistoricalPlacementStatus::OK);
        placement.setShards(shards);
        placement.setOpenCursorAt(segmentStart);
        placement.setNextPlacementChangedAt(segmentEnd);
        std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
        fetcher().bufferResponses(responses);

        auto result = targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
        ASSERT_EQ(result.first, ShardTargeterDecision::kContinue);
        ASSERT_EQ(result.second, segmentEnd);
        ASSERT_TRUE(readerCtx().openCursorOnConfigServerCalls.empty());
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, segmentStart);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
        ASSERT_TRUE(isInDbPresentState());

        readerCtx().reset();
        readerCtx().setTargetedShards(shardSet);
    }

    // Second invocation of 'startChangeStreamSegment()' opens cursors on different shards.
    {
        std::vector<ShardId> shards = {ShardId("shardC"), ShardId("shardD"), ShardId("shardE")};
        stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
        std::vector<ShardId> shardsToOpen{ShardId("shardD"), ShardId("shardE")};
        stdx::unordered_set<ShardId> shardsToOpenSet(shardsToOpen.begin(), shardsToOpen.end());
        std::vector<ShardId> shardsToClose{ShardId("shardA"), ShardId("shardB")};
        stdx::unordered_set<ShardId> shardsToCloseSet(shardsToClose.begin(), shardsToClose.end());

        Timestamp clusterTime(23, 100);

        HistoricalPlacement placement;
        placement.setStatus(HistoricalPlacementStatus::OK);
        placement.setShards(shards);
        placement.setOpenCursorAt(clusterTime);
        std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
        fetcher().bufferResponses(responses);

        auto result = targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
        ASSERT_EQ(result.first, ShardTargeterDecision::kContinue);
        ASSERT_EQ(result.second, boost::none);
        ASSERT_TRUE(readerCtx().openCursorOnConfigServerCalls.empty());
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardsToOpenSet);
        ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls.size(), 1);
        ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls[0].shardSet, shardsToCloseSet);
        ASSERT_TRUE(isInDbPresentState());
    }
}

DEATH_TEST_REGEX_F(DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_NoEventHandlerSet_When_HandleEvent_Then_Throws,
                   "Tripwire assertion.*10720101") {
    Document event(BSON("operationType" << MoveChunkControlEvent::opType));
    targeter().handleEvent(opCtx(), event, readerCtx());
}

TEST_F(DatabaseChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
       Given_HandlerSet_When_HandleEvent_Then_ParsesEventAndDelegatesToHandler) {
    targeter().setEventHandler(std::make_unique<ChangeStreamShardTargeterEventHandlerMock>(
        ShardTargeterDecision::kContinue, ShardTargeterDecision::kContinue));
    auto& eventHandlerMock = dynamic_cast<ChangeStreamShardTargeterEventHandlerMock&>(
        targeter().getEventHandler_forTest());

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

}  // namespace
}  // namespace mongo

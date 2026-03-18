/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/s/change_streams/all_databases_change_stream_shard_targeter_impl.h"

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

template <ChangeStreamReadMode ReadMode>
class AllDatabasesChangeStreamShardTargeterImplFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
        _fetcher = std::make_unique<HistoricalPlacementFetcherMock>();
        if constexpr (ReadMode == ChangeStreamReadMode::kStrict) {
            _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(
                ChangeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kAllDatabases, {}));
        } else {
            _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(ChangeStream(
                ChangeStreamReadMode::kIgnoreRemovedShards, ChangeStreamType::kAllDatabases, {}));
        }
        _targeter =
            std::make_unique<AllDatabasesChangeStreamShardTargeterImpl>(std::move(_fetcher));
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    const AllDatabasesChangeStreamShardTargeterImpl& targeter() const {
        return *_targeter;
    }

    AllDatabasesChangeStreamShardTargeterImpl& targeter() {
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
    std::unique_ptr<AllDatabasesChangeStreamShardTargeterImpl> _targeter;
    std::unique_ptr<HistoricalPlacementFetcherMock> _fetcher;
};

using AllDatabasesChangeStreamShardTargeterImplStrictModeFixture =
    AllDatabasesChangeStreamShardTargeterImplFixture<ChangeStreamReadMode::kStrict>;
using AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture =
    AllDatabasesChangeStreamShardTargeterImplFixture<ChangeStreamReadMode::kIgnoreRemovedShards>;

using AllDatabasesChangeStreamShardTargeterImplStrictModeFixtureDeathTest =
    AllDatabasesChangeStreamShardTargeterImplStrictModeFixture;
using AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest =
    AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture;

// Tests for strict mode.
// ----------------------


TEST_F(AllDatabasesChangeStreamShardTargeterImplStrictModeFixture,
       Given_ActiveShardsInPlacement_When_Initialize_Then_OpensDataShardCursors) {
    Timestamp clusterTime(13, 5);
    std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardB")};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = targeter().initialize(opCtx(), clusterTime, readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
}

DEATH_TEST_REGEX_F(AllDatabasesChangeStreamShardTargeterImplStrictModeFixtureDeathTest,
                   Given_StrictMode_When_StartChangeStreamSegment_Then_Throws,
                   "Tripwire assertion.*11138107") {
    Timestamp clusterTime(10, 1);
    targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
}

TEST_F(AllDatabasesChangeStreamShardTargeterImplStrictModeFixture,
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

DEATH_TEST_REGEX_F(AllDatabasesChangeStreamShardTargeterImplStrictModeFixtureDeathTest,
                   Given_FuturePlacement_When_Initialize_Then_Throws,
                   "Tripwire assertion.*11138102") {
    Timestamp clusterTime(20, 1);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)}};
    fetcher().bufferResponses(responses);

    targeter().initialize(opCtx(), clusterTime, readerCtx());
}

TEST_F(AllDatabasesChangeStreamShardTargeterImplStrictModeFixture,
       Given_Empty_Shard_Set_When_Initialize_Then_OpenConfigServer) {
    Timestamp clusterTime(13, 5);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    targeter().initialize(opCtx(), clusterTime, readerCtx());

    ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
}

DEATH_TEST_REGEX_F(AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_IgnoreRemovedShardsMode_When_Initialize_Then_Throws,
                   "Tripwire assertion.*11138101") {
    Timestamp clusterTime(10, 1);
    targeter().initialize(opCtx(), clusterTime, readerCtx());
}


TEST_F(AllDatabasesChangeStreamShardTargeterImplStrictModeFixture,
       HandleDatabaseCreatedEvent_NoNewShards) {

    Timestamp clusterTime(20, 1);

    // Initial set of shards before DatabaseCreated event.
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    std::vector<ShardId> shards{shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    readerCtx().currentlyTargetedShards = {shardA, shardB};

    // Simulate receiving a 'DatabaseCreated' event. No new shards are introduced, so no new
    // cursors should be open.
    std::vector<ShardId> newShards{ShardId("shardA")};
    stdx::unordered_set<ShardId> newShardSet(newShards.begin(), newShards.end());
    HistoricalPlacement placement;
    placement.setStatus(HistoricalPlacementStatus::OK);
    placement.setShards(newShards);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
    fetcher().bufferResponses(responses);
    targeter().initialize(opCtx(), clusterTime, readerCtx());

    responses.push_back({clusterTime, placement});
    fetcher().bufferResponses(responses);

    DatabaseName dbName = DatabaseName::kConfig;
    Document event = Document(BSON("operationType" << DatabaseCreatedControlEvent::opType
                                                   << "clusterTime" << clusterTime << "fullDocument"
                                                   << BSON("_id" << dbName.toString_forTest())));
    auto result = targeter().handleEvent(opCtx(), event, readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);
    ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 0);
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, newShardSet);
}

TEST_F(AllDatabasesChangeStreamShardTargeterImplStrictModeFixture,
       HandleDatabaseCreatedEvent_WithNewShards) {

    Timestamp clusterTime(20, 1);

    // Initial set of shards before DatabaseCreated event.
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    std::vector<ShardId> shards{shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    readerCtx().currentlyTargetedShards = {shardA, shardB};

    // Simulate receiving a 'DatabaseCreated' event. As new shards are introduced, new cursors
    // should be open.
    std::vector<ShardId> newShards{ShardId("shardA"), ShardId("shardC")};
    stdx::unordered_set<ShardId> newShardSet(newShards.begin(), newShards.end());
    HistoricalPlacement placement;
    placement.setStatus(HistoricalPlacementStatus::OK);
    placement.setShards(newShards);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
    fetcher().bufferResponses(responses);
    targeter().initialize(opCtx(), clusterTime, readerCtx());

    responses.push_back({clusterTime, placement});
    fetcher().bufferResponses(responses);

    DatabaseName dbName = DatabaseName::kConfig;
    Document event = Document(BSON("operationType" << DatabaseCreatedControlEvent::opType
                                                   << "clusterTime" << clusterTime << "fullDocument"
                                                   << BSON("_id" << dbName.toString_forTest())));
    auto result = targeter().handleEvent(opCtx(), event, readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);
    ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 0);
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 2);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, newShardSet);
}

// // Tests for ignoreRemovedShards mode.
// // -----------------------------------


DEATH_TEST_REGEX_F(AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_FuturePlacement_When_Initialize_Then_Throws,
                   "Tripwire assertion.*10917001") {
    Timestamp clusterTime(20, 1);
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)}};
    fetcher().bufferResponses(responses);

    targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
}

DEATH_TEST_REGEX_F(AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_OpenCursorAtBeforeAtClusterTime_When_StartChangeStreamSegment_Then_Throws,
                   "Tripwire assertion.*11138109") {
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
    AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
    Given_NextPlacementChangedAtBeforeOpenCursorAt_When_StartChangeStreamSegment_Then_Throws,
    "Tripwire assertion.*11138110") {
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
    AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixtureDeathTest,
    Given_NextPlacementChangedAtWithoutShards_When_StartChangeStreamSegment_Then_Throws,
    "Tripwire assertion.*11138111") {
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
    AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
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

TEST_F(AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
       Given_PlacementWithoutShards_When_StartChangeStreamSegment_Then_OpensCursorOnConfigSvr) {
    Timestamp clusterTime(20, 1);
    readerCtx().setDegradedMode(true);

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
}

TEST_F(AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
       Given_PlacementWithShards_When_StartChangeStreamSegment_Then_OpensCursorOnDataShards) {
    readerCtx().setDegradedMode(true);

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
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
}

TEST_F(
    AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
    Given_PlacementWithShardsAndUnboundedSegmentInFuture_When_StartChangeStreamSegment_Then_OpensCursorOnDataShardsAtSegmentStart) {
    readerCtx().setDegradedMode(true);

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
    ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, segmentStart);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
}

TEST_F(
    AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
    Given_PlacementWithShardsAndBoundedSegment_When_StartChangeStreamSegment_Then_OpensCursorOnDataShardsAtSegmentStart) {
    readerCtx().setDegradedMode(true);

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
    ASSERT(readerCtx().openCursorOnConfigServerCalls.empty());
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, segmentStart);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
}

TEST_F(AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
       Given_CursorIsOpenOnConfigShard_When_HandleEvent_Then_DoesNotOpenCursorOnNewDataShards) {
    // First invocation of 'startChangeStreamSegment()' opens a cursor on the config server.
    {
        Timestamp clusterTime(20, 1);

        // Initial set of shards before DatabaseCreated event.
        std::vector<ShardId> shards{ShardId("shardA"), ShardId("shardB")};
        stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

        HistoricalPlacement placement;
        placement.setStatus(HistoricalPlacementStatus::OK);

        std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
        fetcher().bufferResponses(responses);

        auto result = targeter().startChangeStreamSegment(opCtx(), clusterTime, readerCtx());
        ASSERT_EQ(result.first, ShardTargeterDecision::kContinue);
        ASSERT_EQ(result.second, boost::none);
        ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
        ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls[0].atClusterTime, clusterTime);
        ASSERT(readerCtx().openCursorsOnDataShardsCalls.empty());
    }

    // Simulate receiving a 'DatabaseCreated' event. This should open the cursors on the new
    // data shards.
    {
        std::vector<ShardId> newShards{ShardId("shardA"), ShardId("shardC")};
        stdx::unordered_set<ShardId> newShardSet(newShards.begin(), newShards.end());
        Timestamp clusterTime = Timestamp(21, 10);
        HistoricalPlacement placement;
        placement.setStatus(HistoricalPlacementStatus::OK);
        placement.setShards(newShards);
        std::vector<HistoricalPlacementFetcherMock::Response> responses{{clusterTime, placement}};
        fetcher().bufferResponses(responses);

        DatabaseName dbName = DatabaseName::kConfig;
        Document event = Document(BSON(
            "operationType" << DatabaseCreatedControlEvent::opType << "clusterTime" << clusterTime
                            << "fullDocument" << BSON("_id" << dbName.toString_forTest())));
        auto result = targeter().handleEvent(opCtx(), event, readerCtx());
        ASSERT_EQ(result, ShardTargeterDecision::kContinue);
        ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 0);
        ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    }
}

TEST_F(
    AllDatabasesChangeStreamShardTargeterImplIgnoreRemovedShardsModeFixture,
    Given_MultipleChangeStreamSegments_When_StartChangeStreamSegment_Then_OpensCursorOnDataShardsAtSegmentStart) {
    readerCtx().setDegradedMode(true);

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
        ASSERT(readerCtx().openCursorOnConfigServerCalls.empty());
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, segmentStart);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);

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
        ASSERT_EQ(readerCtx().openCursorOnConfigServerCalls.size(), 1);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime);
        ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardsToOpenSet);
        ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls.size(), 1);
        ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls[0].shardSet, shardsToCloseSet);
    }
}

}  // namespace
}  // namespace mongo

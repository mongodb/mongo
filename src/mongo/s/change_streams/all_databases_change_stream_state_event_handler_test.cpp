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

#include "mongo/s/change_streams/all_databases_change_stream_state_event_handler.h"

#include "mongo/db/pipeline/change_stream_reader_context_mock.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service_mock.h"
#include "mongo/db/pipeline/historical_placement_fetcher_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler_mock.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional.hpp>

namespace mongo {
namespace {

NamespaceString makeTestNss() {
    return NamespaceString::createNamespaceString_forTest("testAllDbs");
}

template <ChangeStreamReadMode ReadMode>
class AllDatabasesEventHandlerFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        _opCtx = makeOperationContext();
        _ctx = std::make_unique<ChangeStreamShardTargeterStateEventHandlingContextMock>(
            std::make_unique<HistoricalPlacementFetcherMock>());
        if constexpr (ReadMode == ChangeStreamReadMode::kStrict) {
            _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(
                ChangeStream(ChangeStreamReadMode::kStrict, ChangeStreamType::kAllDatabases, {}));
        } else {
            _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(ChangeStream(
                ChangeStreamReadMode::kIgnoreRemovedShards, ChangeStreamType::kAllDatabases, {}));
        }
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    AllDatabasesShardTargeterStateEventHandler& handler() {
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

    void assertNoCursorOperations() {
        ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 0);
        ASSERT_TRUE(readerCtx().closeCursorsOnDataShardsCalls.empty());
        ASSERT_TRUE(readerCtx().openCursorOnConfigServerCalls.empty());
        ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    AllDatabasesShardTargeterStateEventHandler _handler;
    std::unique_ptr<ChangeStreamShardTargeterStateEventHandlingContextMock> _ctx;
    std::unique_ptr<ChangeStreamReaderContextMock> _readerCtx;
};

using AllDatabasesEventHandlerStrictModeFixture =
    AllDatabasesEventHandlerFixture<ChangeStreamReadMode::kStrict>;
using AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture =
    AllDatabasesEventHandlerFixture<ChangeStreamReadMode::kIgnoreRemovedShards>;

using AllDatabasesEventHandlerStrictModeFixtureDeathTest =
    AllDatabasesEventHandlerStrictModeFixture;
using AllDatabasesEventHandlerIgnoreRemovedShardsModeFixtureDeathTest =
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture;

// Tests for strict mode.
// ----------------------

TEST_F(
    AllDatabasesEventHandlerStrictModeFixture,
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
}

DEATH_TEST_REGEX_F(
    AllDatabasesEventHandlerStrictModeFixtureDeathTest,
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
    AllDatabasesEventHandlerStrictModeFixtureDeathTest,
    Given_DatabaseCreatedControlEventWithEmptyPlacement_When_HandleEventIsCalled_Then_Throws,
    "Tripwire assertion.*11138118") {
    Timestamp clusterTime(50, 1);
    DatabaseCreatedControlEvent event;
    event.clusterTime = clusterTime;

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    handler().handleEvent(opCtx(), event, ctx(), readerCtx());
}

TEST_F(
    AllDatabasesEventHandlerStrictModeFixture,
    Given_DatabaseCreatedControlEventWithShards_When_HandleEventIsCalled_Then_ConfigsvrCursorOpenDataShardCursorsOpened) {
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

    ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 0);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet, shardSet);
}

TEST_F(
    AllDatabasesEventHandlerStrictModeFixture,
    Given_MoveChunkControlEventWithPlacementNotAvailable_When_HandleEventIsCalled_Then_ReturnSwitchToV1) {
    Timestamp clusterTime(20, 1);
    ShardId shardA("shardA");
    ShardId shardB("shardB");

    for (bool allCollectionChunksMigratedFromDonor : {true, false}) {
        MoveChunkControlEvent event{
            clusterTime, shardA, shardB, allCollectionChunksMigratedFromDonor};

        std::vector<HistoricalPlacementFetcherMock::Response> responses{
            {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::NotAvailable)}};
        fetcher().bufferResponses(responses);

        auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
        ASSERT_EQ(result, ShardTargeterDecision::kSwitchToV1);
        assertNoCursorOperations();
    }
}

DEATH_TEST_REGEX_F(
    AllDatabasesEventHandlerStrictModeFixtureDeathTest,
    Given_MoveChunkControlEventWithNotAllChunksMigratedWithPlacementInFuture_When_HandleEventIsCalled_Then_Throws,
    "Tripwire assertion.*10917001") {
    Timestamp clusterTime(20, 1);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, false /* allCollectionChunksMigratedFromDonor */};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)}};
    fetcher().bufferResponses(responses);

    handler().handleEvent(opCtx(), event, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(
    AllDatabasesEventHandlerStrictModeFixtureDeathTest,
    Given_MoveChunkControlEventWithAllChunksMigratedWithPlacementInFuture_When_HandleEventIsCalled_Then_Throws,
    "Tripwire assertion.*10917001") {
    Timestamp clusterTime(20, 1);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, true /* allCollectionChunksMigratedFromDonor */};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)}};
    fetcher().bufferResponses(responses);

    handler().handleEvent(opCtx(), event, ctx(), readerCtx());
}

TEST_F(
    AllDatabasesEventHandlerStrictModeFixture,
    Given_MoveChunkControlEventWithShardsAndPlacementOnBothShards_When_HandleEventIsCalled_Then_CursorsAreUpdated) {
    Timestamp clusterTime(101, 3);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, false /* allCollectionChunksMigratedFromDonor */};

    readerCtx().currentlyTargetedShards = {shardA};

    std::vector<ShardId> shards = {shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet,
              stdx::unordered_set<ShardId>{shardB});
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
    ASSERT_TRUE(readerCtx().closeCursorsOnDataShardsCalls.empty());
}

TEST_F(
    AllDatabasesEventHandlerStrictModeFixture,
    Given_MoveChunkControlEventWithShardsAndPlacementOnTheNewShard_When_HandleEventIsCalled_Then_CursorsAreUpdated) {
    Timestamp clusterTime(101, 3);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, true /* allCollectionChunksMigratedFromDonor */};

    readerCtx().currentlyTargetedShards = {shardA};

    std::vector<ShardId> shards = {shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet,
              stdx::unordered_set<ShardId>{shardB});
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls[0].shardSet,
              stdx::unordered_set<ShardId>{shardA});
}

TEST_F(
    AllDatabasesEventHandlerStrictModeFixture,
    Given_MoveChunkControlEventAndPlacementDidNotChange_When_HandleEventIsCalled_Then_CursorsRemainSame) {
    Timestamp clusterTime(101, 3);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, true /* allCollectionChunksMigratedFromDonor */};

    readerCtx().currentlyTargetedShards = {shardA, shardB};

    // Simulate that both shards are still part of the placement, as the database can have multiple
    // collections.
    std::vector<ShardId> shards = {shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);
    assertNoCursorOperations();
}

TEST_F(
    AllDatabasesEventHandlerStrictModeFixture,
    Given_MovePrimaryControlEventWithPlacementNotAvailable_When_HandleEventIsCalled_Then_ReturnSwitchToV1) {
    Timestamp clusterTime(20, 1);
    MovePrimaryControlEvent event{clusterTime};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::NotAvailable)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kSwitchToV1);
    assertNoCursorOperations();
}

DEATH_TEST_REGEX_F(
    AllDatabasesEventHandlerStrictModeFixtureDeathTest,
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

TEST_F(AllDatabasesEventHandlerStrictModeFixture,
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
    AllDatabasesEventHandlerStrictModeFixture,
    Given_NamespacePlacementChangedControlEventWithPlacementNotAvailable_When_HandleEventIsCalled_Then_ReturnSwitchToV1) {
    Timestamp clusterTime(20, 1);
    NamespacePlacementChangedControlEvent event{clusterTime};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::NotAvailable)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kSwitchToV1);
    assertNoCursorOperations();
}

DEATH_TEST_REGEX_F(
    AllDatabasesEventHandlerStrictModeFixtureDeathTest,
    Given_NamespacePlacementChangedControlEventWithPlacementInFuture_When_HandleEventIsCalled_Then_Throws,
    "Tripwire assertion.*10917001") {
    Timestamp clusterTime(101, 0);
    NamespacePlacementChangedControlEvent event{clusterTime};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::FutureClusterTime)}};
    fetcher().bufferResponses(responses);

    handler().handleEvent(opCtx(), event, ctx(), readerCtx());
}

TEST_F(
    AllDatabasesEventHandlerStrictModeFixture,
    Given_NamespacePlacementChangedControlEventWithEmptyPlacement_When_HandleEventIsCalled_Then_ConfigsvrCursorOpened) {
    Timestamp clusterTime(250, 3);
    NamespacePlacementChangedControlEvent event{clusterTime};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);
}


// Tests for ignoreRemovedShards mode.
// -----------------------------------

TEST_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
    Given_MoveChunkControlEventToShardNotYetOpened_When_HandleEventIsCalled_Then_OpensCursorOnToShard) {
    Timestamp clusterTime(101, 3);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, false /* allCollectionChunksMigratedFromDonor */};

    readerCtx().currentlyTargetedShards = {shardA};

    std::vector<ShardId> shards = {shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet,
              stdx::unordered_set<ShardId>{shardB});
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls[0].shardSet,
              stdx::unordered_set<ShardId>{shardA});
}

TEST_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
    Given_MoveChunkControlEventAllChunksMigrated_When_HandleEventIsCalled_Then_ClosesCursorOnFromShard) {
    Timestamp clusterTime(102, 3);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, true /* allCollectionChunksMigratedFromDonor */};

    readerCtx().currentlyTargetedShards = {shardA, shardB};

    std::vector<ShardId> shards = {shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    // Confirm close occurred on "shardA".
    ASSERT_FALSE(readerCtx().closeCursorsOnDataShardsCalls.empty());
    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls[0].shardSet,
              stdx::unordered_set<ShardId>{shardA});
}

TEST_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
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
}

DEATH_TEST_REGEX_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
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

TEST_F(AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
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
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
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

    // Expect 'shardB' and 'shardC' to be opened and 'shardD' to be closed.
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].atClusterTime, clusterTime + 1);
    ASSERT_EQ(readerCtx().openCursorsOnDataShardsCalls[0].shardSet,
              (stdx::unordered_set<ShardId>{shardB, shardC}));

    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls.size(), 1);
    ASSERT_EQ(readerCtx().closeCursorsOnDataShardsCalls[0].shardSet,
              (stdx::unordered_set<ShardId>{shardD}));
}

TEST_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
    Given_NamespacePlacementChangedControlEventWithEmptyPlacement_When_HandleEventIsCalled_Then_NoCursorOpOccurs) {
    Timestamp clusterTime(250, 3);
    NamespacePlacementChangedControlEvent event{clusterTime};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);
    assertNoCursorOperations();
}


TEST_F(AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
       HandleEventInDegradedMode_WhenDatabaseCreated_AndAllCursorsAlreadyOpen) {
    readerCtx().setDegradedMode(true);

    ShardId shardA("shardA");
    ShardId shardB("shardB");
    readerCtx().currentlyTargetedShards = {shardA, shardB};

    Timestamp clusterTime(77, 0);
    DatabaseCreatedControlEvent event;
    event.clusterTime = clusterTime;
    std::vector<ShardId> shards = {shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEventInDegradedMode(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 0);
    ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    ASSERT_TRUE(readerCtx().closeCursorsOnDataShardsCalls.empty());
}

TEST_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
    Given_MoveChunkControlEventWithNewToShard_When_HandleEventIsCalledInDegradedMode_Then_DoesNotUpdateCursors) {
    readerCtx().setDegradedMode(true);

    Timestamp clusterTime(20, 1);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    ShardId shardC("shardC");

    std::vector<ShardId> shards = {shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    for (bool allCollectionChunksMigratedFromDonor : {true, false}) {
        MoveChunkControlEvent event{
            clusterTime, shardA, shardC, allCollectionChunksMigratedFromDonor};

        auto result = handler().handleEventInDegradedMode(opCtx(), event, ctx(), readerCtx());
        ASSERT_EQ(result, ShardTargeterDecision::kContinue);
        assertNoCursorOperations();
    }
}

TEST_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
    When_HandleEventInDegradedModeIsCalledForDatabaseCreatedWithNewShard_Then_DoesNotUpdateCursors) {
    readerCtx().setDegradedMode(true);

    ShardId shardA("shardA");
    ShardId shardB("shardB");
    Timestamp clusterTime(77, 0);
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, false /* allCollectionChunksMigratedFromDonor */};

    std::vector<ShardId> shards = {shardA, shardB};

    readerCtx().setDegradedMode(true);
    readerCtx().currentlyTargetedShards = {shardA};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEventInDegradedMode(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    assertNoCursorOperations();
}

TEST_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
    When_HandleEventInDegradedModeIsCalledForDatabaseCreatedWithNewShard_Then_DoesNotModifyCursors) {

    readerCtx().setDegradedMode(true);
    Timestamp clusterTime(77, 0);
    DatabaseCreatedControlEvent event;
    event.clusterTime = clusterTime;
    std::vector<ShardId> shards = {ShardId("shardA"), ShardId("shardB")};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEventInDegradedMode(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    assertNoCursorOperations();
}

TEST_F(AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
       When_HandleEventInDegradedModeIsCalledForMovePrimaryWithNewShard_Then_DoesNotModifyCursors) {
    readerCtx().setDegradedMode(true);
    Timestamp clusterTime(77, 0);
    MovePrimaryControlEvent event({clusterTime});

    ShardId shardA("shardA");
    ShardId shardB("shardB");

    readerCtx().currentlyTargetedShards = {shardA};
    std::vector<ShardId> shards = {shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEventInDegradedMode(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    assertNoCursorOperations();
}

TEST_F(AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
       When_HandleEventInDegradedModeIsCalledForMovePrimary_Then_DoesNotModifyCursors) {
    readerCtx().setDegradedMode(true);
    Timestamp clusterTime(101, 3);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    std::vector<ShardId> shards = {shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    readerCtx().currentlyTargetedShards = {shardA, shardB};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEventInDegradedMode(
        opCtx(), MovePrimaryControlEvent{clusterTime}, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    assertNoCursorOperations();
}

TEST_F(AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
       When_HandleEventInDegradedModeIsCalledForMoveChunk_Then_DoesNotModifyCursors) {
    Timestamp clusterTime(101, 3);
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    MoveChunkControlEvent event{
        clusterTime, shardA, shardB, false /* allCollectionChunksMigratedFromDonor */};

    std::vector<ShardId> shards = {shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    readerCtx().setDegradedMode(true);
    readerCtx().currentlyTargetedShards = {shardA, shardB};

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    auto result = handler().handleEventInDegradedMode(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    assertNoCursorOperations();
}

TEST_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
    When_HandleEventInDegradedModeIsCalledForNamespacePlacementChangedWithNonEmptyNamespace_Then_DoesNotModifyCursors) {
    Timestamp clusterTime(60, 10);
    NamespacePlacementChangedControlEvent event{clusterTime, makeTestNss()};
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    std::vector<ShardId> shards = {shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    readerCtx().setDegradedMode(true);
    readerCtx().currentlyTargetedShards = {shardA, shardB};

    auto result = handler().handleEventInDegradedMode(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    assertNoCursorOperations();
}

TEST_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
    When_HandleEventInDegradedModeIsCalledForNamespacePlacementChangedWithEmptyNamespace_Then_QueriesShardAllocationAndReturnsContinue) {
    Timestamp clusterTime(60, 10);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock(clusterTime,
                                                                  AllocationToShardsStatus::kOk);

    NamespacePlacementChangedControlEvent event{clusterTime,
                                                NamespaceString::createNamespaceString_forTest("")};
    ShardId shardA("shardA");
    ShardId shardB("shardB");
    std::vector<ShardId> shards = {shardA, shardB};
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement(shards, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    readerCtx().setDegradedMode(true);
    readerCtx().currentlyTargetedShards = {shardA, shardB};

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);

    assertNoCursorOperations();
}

TEST_F(
    AllDatabasesEventHandlerIgnoreRemovedShardsModeFixture,
    When_HandleEventInDegradedModeIsCalledForNamespacePlacementChangedWithEmptyNamespaceAndNoAllocationAvailable_Then_ReturnsSwitchToV1) {
    Timestamp clusterTime(60, 10);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock(
        clusterTime, AllocationToShardsStatus::kNotAvailable);

    std::vector<HistoricalPlacementFetcherMock::Response> responses{
        {clusterTime, HistoricalPlacement({}, HistoricalPlacementStatus::OK)}};
    fetcher().bufferResponses(responses);

    NamespacePlacementChangedControlEvent event{clusterTime,
                                                NamespaceString::createNamespaceString_forTest("")};

    readerCtx().setDegradedMode(true);
    auto result = handler().handleEventInDegradedMode(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kSwitchToV1);

    assertNoCursorOperations();
}

}  // namespace
}  // namespace mongo

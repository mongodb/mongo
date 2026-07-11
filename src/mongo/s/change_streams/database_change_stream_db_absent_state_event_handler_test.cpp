// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/database_change_stream_db_absent_state_event_handler.h"

#include "mongo/db/pipeline/change_stream_reader_context_mock.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service_mock.h"
#include "mongo/db/pipeline/historical_placement_fetcher_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler_mock.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/s/change_streams/database_change_stream_db_present_state_event_handler.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/optional.hpp>

namespace mongo {
namespace {
NamespaceString makeTestNss() {
    return NamespaceString::createNamespaceString_forTest("testDb");
}

template <ChangeStreamReadMode ReadMode>
class DatabaseDbAbsentStateEventHandlerFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        _opCtx = makeOperationContext();
        _ctx = std::make_unique<ChangeStreamShardTargeterStateEventHandlingContextMock>(
            std::make_unique<HistoricalPlacementFetcherMock>());
        if constexpr (ReadMode == ChangeStreamReadMode::kStrict) {
            _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(ChangeStream(
                ChangeStreamReadMode::kStrict, ChangeStreamType::kDatabase, makeTestNss()));
        } else {
            _readerCtx = std::make_unique<ChangeStreamReaderContextMock>(
                ChangeStream(ChangeStreamReadMode::kIgnoreRemovedShards,
                             ChangeStreamType::kDatabase,
                             makeTestNss()));
        }
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    DatabaseChangeStreamShardTargeterDbAbsentStateEventHandler& handler() {
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
    DatabaseChangeStreamShardTargeterDbAbsentStateEventHandler _handler;
    std::unique_ptr<ChangeStreamShardTargeterStateEventHandlingContextMock> _ctx;
    std::unique_ptr<ChangeStreamReaderContextMock> _readerCtx;
};

using DatabaseDbAbsentStateEventHandlerStrictModeFixture =
    DatabaseDbAbsentStateEventHandlerFixture<ChangeStreamReadMode::kStrict>;
using DatabaseDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixture =
    DatabaseDbAbsentStateEventHandlerFixture<ChangeStreamReadMode::kIgnoreRemovedShards>;

using DatabaseDbAbsentStateEventHandlerStrictModeFixtureDeathTest =
    DatabaseDbAbsentStateEventHandlerStrictModeFixture;
using DatabaseDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest =
    DatabaseDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixture;

// Tests for strict mode.
// ----------------------

DEATH_TEST_REGEX_F(DatabaseDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
                   Given_MovePrimaryControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*11600502") {
    handler().handleEvent(opCtx(), MovePrimaryControlEvent{}, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(DatabaseDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
                   Given_MoveChunkControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*11600502") {
    handler().handleEvent(opCtx(), MoveChunkControlEvent{}, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(
    DatabaseDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
    Given_NamespacePlacementChangedWithNonEmptyNamespace_When_HandleEventIsCalled_Then_Throws,
    "Tripwire assertion.*12321702") {
    NamespacePlacementChangedControlEvent event{
        Timestamp(10, 0), NamespaceString::createNamespaceString_forTest("testDb.testColl")};
    handler().handleEvent(opCtx(), event, ctx(), readerCtx());
}

TEST_F(
    DatabaseDbAbsentStateEventHandlerStrictModeFixture,
    Given_NamespacePlacementChangedWithEmptyNamespaceAndNotAvailable_When_HandleEventIsCalled_Then_ReturnsSwitchToV1) {
    Timestamp clusterTime(10, 0);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock(
        clusterTime, AllocationToShardsStatus::kNotAvailable);

    NamespacePlacementChangedControlEvent event{clusterTime,
                                                NamespaceString::createNamespaceString_forTest("")};

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kSwitchToV1);
    ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 0);
    ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    ASSERT_TRUE(ctx().setHandlerCalls.empty());
}

TEST_F(
    DatabaseDbAbsentStateEventHandlerStrictModeFixture,
    Given_NamespacePlacementChangedWithEmptyNamespaceAndAvailable_When_HandleEventIsCalled_Then_ReturnsContinue) {
    Timestamp clusterTime(10, 0);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock(clusterTime,
                                                                  AllocationToShardsStatus::kOk);

    NamespacePlacementChangedControlEvent event{clusterTime,
                                                NamespaceString::createNamespaceString_forTest("")};

    auto result = handler().handleEvent(opCtx(), event, ctx(), readerCtx());
    ASSERT_EQ(result, ShardTargeterDecision::kContinue);
    ASSERT_EQ(readerCtx().closeCursorOnConfigServerCount, 0);
    ASSERT_TRUE(readerCtx().openCursorsOnDataShardsCalls.empty());
    ASSERT_TRUE(ctx().setHandlerCalls.empty());
}

TEST_F(
    DatabaseDbAbsentStateEventHandlerStrictModeFixture,
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
    DatabaseDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
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
    DatabaseDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
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
    DatabaseDbAbsentStateEventHandlerStrictModeFixture,
    Given_DatabaseCreatedControlEventWithShards_When_HandleEventIsCalled_Then_ConfigsvrCursorClosedDataShardCursorsOpenedAndHandlerIsSet) {
    Timestamp clusterTime(77, 0);
    DatabaseCreatedControlEvent event;
    event.clusterTime = clusterTime;
    std::vector<ShardRef> shards = {ShardRef("shardA"), ShardRef("shardB")};
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
    ASSERT_TRUE(dynamic_cast<DatabaseChangeStreamShardTargeterDbPresentStateEventHandler*>(
        ctx().lastSetEventHandler()));
}

DEATH_TEST_REGEX_F(DatabaseDbAbsentStateEventHandlerStrictModeFixtureDeathTest,
                   When_HandleEventInDegradedModeIsCalled_Then_AlwaysThrows,
                   "Tripwire assertion.*10922908") {
    handler().handleEventInDegradedMode(opCtx(), DatabaseCreatedControlEvent{}, ctx(), readerCtx());
}

// Tests for ignoreRemovedShards mode.
// -----------------------------------

DEATH_TEST_REGEX_F(DatabaseDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_MovePrimaryControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*11600502") {
    handler().handleEvent(opCtx(), MovePrimaryControlEvent{}, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(DatabaseDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_MoveChunkControlEvent_When_HandleEventIsCalled_Then_Throws,
                   "Tripwire assertion.*11600502") {
    handler().handleEvent(opCtx(), MoveChunkControlEvent{}, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(
    DatabaseDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
    Given_NamespacePlacementChangedWithNonEmptyNamespace_When_HandleEventIsCalled_Then_Throws,
    "Tripwire assertion.*12321702") {
    NamespacePlacementChangedControlEvent event{
        Timestamp(10, 0), NamespaceString::createNamespaceString_forTest("testDb.testColl")};
    handler().handleEvent(opCtx(), event, ctx(), readerCtx());
}

DEATH_TEST_REGEX_F(DatabaseDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
                   Given_NonDegradedMode_When_HandleEventInDegradedModeIsCalled_Then_Throws,
                   "Tripwire assertion.*10922908") {
    Timestamp clusterTime(101, 0);
    MovePrimaryControlEvent event;
    event.clusterTime = clusterTime;

    readerCtx().setDegradedMode(false);
    handler().handleEventInDegradedMode(opCtx(), event, ctx(), readerCtx());
}

TEST_F(
    DatabaseDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixture,
    Given_DatabaseCreatedControlEventWithShards_When_HandleEventIsCalled_Then_ConfigsvrCursorClosedDataShardCursorsOpenedAndHandlerIsSet) {
    Timestamp clusterTime(77, 0);
    DatabaseCreatedControlEvent event;
    event.clusterTime = clusterTime;
    std::vector<ShardRef> shards = {ShardRef("shardA"), ShardRef("shardB")};
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
    ASSERT_TRUE(dynamic_cast<DatabaseChangeStreamShardTargeterDbPresentStateEventHandler*>(
        ctx().lastSetEventHandler()));
}

DEATH_TEST_REGEX_F(
    DatabaseDbAbsentStateEventHandlerIgnoreRemovedShardsModeFixtureDeathTest,
    When_HandleEventInDegradedModeIsCalledForDatabaseCreated_Then_DoesNotModifyCursors,
    "Tripwire assertion.*10922908") {
    readerCtx().setDegradedMode(true);
    handler().handleEventInDegradedMode(opCtx(), DatabaseCreatedControlEvent{}, ctx(), readerCtx());
}

}  // namespace
}  // namespace mongo

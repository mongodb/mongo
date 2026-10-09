// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/change_stream_shard_targeter_metrics.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

using Presence = ChangeStreamShardTargeterStateEventHandler::DbPresenceState;

MoveChunkControlEvent makeMoveChunkEvent() {
    return MoveChunkControlEvent{
        .clusterTime = Timestamp(10, 1),
        .fromShard = ShardId("shard0"),
        .toShard = ShardId("shard1"),
        .allCollectionChunksMigratedFromDonor = false,
    };
}

MovePrimaryControlEvent makeMovePrimaryEvent() {
    return MovePrimaryControlEvent{
        .clusterTime = Timestamp(10, 2),
        .fromShard = ShardId("shard0"),
        .toShard = ShardId("shard1"),
    };
}

NamespacePlacementChangedControlEvent makeNamespacePlacementChangedEvent() {
    return NamespacePlacementChangedControlEvent{
        .clusterTime = Timestamp(10, 3),
        .nss = NamespaceString::createNamespaceString_forTest("testDb.testColl"),
    };
}

DatabaseCreatedControlEvent makeDatabaseCreatedEvent() {
    return DatabaseCreatedControlEvent{
        .clusterTime = Timestamp(10, 4),
        .createdDatabaseName = DatabaseName::kConfig,
    };
}

// Each (scope, db-presence) combination increments exactly its own targeterScope counter.
TEST(ChangeStreamShardTargeterMetricsTest, ScopeTransitionUpdatesMatchingCounter) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto collectionDbPresentBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTargeterScopeCollectionDbPresent);
    auto collectionDbAbsentBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTargeterScopeCollectionDbAbsent);
    auto databaseDbPresentBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTargeterScopeDatabaseDbPresent);
    auto databaseDbAbsentBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTargeterScopeDatabaseDbAbsent);
    auto allDatabasesBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTargeterScopeAllDatabases);

    auto collectionScopeMetrics = getCollectionShardTargeterScopeMetricsRecorder();
    auto databaseScopeMetrics = getDatabaseShardTargeterScopeMetricsRecorder();
    auto allDatabasesScopeMetrics = getAllDatabasesShardTargeterMetricsRecorder();

    collectionScopeMetrics.recordTransition(Presence::kDbPresent);
    collectionScopeMetrics.recordTransition(Presence::kDbAbsent);
    databaseScopeMetrics.recordTransition(Presence::kDbPresent);
    databaseScopeMetrics.recordTransition(Presence::kDbAbsent);
    // The all-databases targeter does not use the db-present/db-absent split: every handler
    // installation counts, regardless of the presence the handler reports.
    allDatabasesScopeMetrics.recordTransition();
    allDatabasesScopeMetrics.recordTransition();

    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingTargeterScopeCollectionDbPresent),
              collectionDbPresentBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingTargeterScopeCollectionDbAbsent),
              collectionDbAbsentBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingTargeterScopeDatabaseDbPresent),
              databaseDbPresentBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingTargeterScopeDatabaseDbAbsent),
              databaseDbAbsentBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingTargeterScopeAllDatabases),
              allDatabasesBefore + 2);
}

// A collection/database scope recorder ignores an unknown presence (only reachable from a test
// mock in production).
TEST(ChangeStreamShardTargeterMetricsTest, ScopeTransitionIgnoresUnknownPresence) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto collectionDbPresentBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTargeterScopeCollectionDbPresent);
    auto collectionDbAbsentBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingTargeterScopeCollectionDbAbsent);

    getCollectionShardTargeterScopeMetricsRecorder().recordTransition(Presence::kUnknown);

    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingTargeterScopeCollectionDbPresent),
              collectionDbPresentBefore);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingTargeterScopeCollectionDbAbsent),
              collectionDbAbsentBefore);
}

// Each ControlEvent alternative increments exactly its own controlEvents counter.
TEST(ChangeStreamShardTargeterMetricsTest, ControlEventUpdatesMatchingCounter) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto moveChunkBefore =
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingControlEventMoveChunk);
    auto movePrimaryBefore =
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingControlEventMovePrimary);
    auto namespacePlacementChangedBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingControlEventNamespacePlacementChanged);
    auto databaseCreatedBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingControlEventDatabaseCreated);

    auto controlEventMetrics = getShardTargeterControlEventMetricsRecorder();
    controlEventMetrics.recordControlEvent(makeMoveChunkEvent());
    controlEventMetrics.recordControlEvent(makeMovePrimaryEvent());
    controlEventMetrics.recordControlEvent(makeNamespacePlacementChangedEvent());
    controlEventMetrics.recordControlEvent(makeDatabaseCreatedEvent());

    ASSERT_EQ(
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingControlEventMoveChunk),
        moveChunkBefore + 1);
    ASSERT_EQ(
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingControlEventMovePrimary),
        movePrimaryBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingControlEventNamespacePlacementChanged),
              namespacePlacementChangedBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingControlEventDatabaseCreated),
              databaseCreatedBefore + 1);
}

}  // namespace
}  // namespace mongo

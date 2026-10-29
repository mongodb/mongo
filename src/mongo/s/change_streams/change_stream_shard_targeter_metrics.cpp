// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/change_stream_shard_targeter_metrics.h"

#include "mongo/db/change_stream_metrics_util.h"
#include "mongo/util/assert_util.h"

#include <type_traits>
#include <variant>

namespace mongo {

using Presence = ChangeStreamShardTargeterStateEventHandler::DbPresenceState;

namespace {

using namespace change_stream;

/**
 * References to the OTEL instruments for the v2 change stream shard targeter's event-handler
 * state: one counter per (scope, db-presence) combination plus one per control-event type. The
 * instruments are process-global singletons owned by the OTEL MetricsService; this struct only
 * borrows references to them.
 */
struct ChangeStreamShardTargeterMetrics {
    otel::metrics::Counter<int64_t>& collectionDbPresent;
    otel::metrics::Counter<int64_t>& collectionDbAbsent;
    otel::metrics::Counter<int64_t>& databaseDbPresent;
    otel::metrics::Counter<int64_t>& databaseDbAbsent;
    otel::metrics::Counter<int64_t>& allDatabases;
    otel::metrics::Counter<int64_t>& moveChunk;
    otel::metrics::Counter<int64_t>& movePrimary;
    otel::metrics::Counter<int64_t>& namespacePlacementChanged;
    otel::metrics::Counter<int64_t>& databaseCreated;
};

const ChangeStreamShardTargeterMetrics kChangeStreamShardTargeterMetrics{
    .collectionDbPresent = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingTargeterScopeCollectionDbPresent,
        "changeStreams.shardTargeting.targeterScope.collection.dbPresent",
        "Number of times the v2 change stream collection-scoped shard targeter installed its "
        "db-present event handler."),
    .collectionDbAbsent = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingTargeterScopeCollectionDbAbsent,
        "changeStreams.shardTargeting.targeterScope.collection.dbAbsent",
        "Number of times the v2 change stream collection-scoped shard targeter installed its "
        "db-absent event handler."),
    .databaseDbPresent = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingTargeterScopeDatabaseDbPresent,
        "changeStreams.shardTargeting.targeterScope.database.dbPresent",
        "Number of times the v2 change stream database-scoped shard targeter installed its "
        "db-present event handler."),
    .databaseDbAbsent = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingTargeterScopeDatabaseDbAbsent,
        "changeStreams.shardTargeting.targeterScope.database.dbAbsent",
        "Number of times the v2 change stream database-scoped shard targeter installed its "
        "db-absent event handler."),
    .allDatabases = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingTargeterScopeAllDatabases,
        "changeStreams.shardTargeting.targeterScope.allDatabases",
        "Number of times the v2 change stream all-databases shard targeter installed its event "
        "handler; it does not use the db-present/db-absent split."),
    .moveChunk = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingControlEventMoveChunk,
        "changeStreams.shardTargeting.controlEvents.moveChunk",
        "Number of moveChunk control events observed by v2 change stream shard targeters."),
    .movePrimary = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingControlEventMovePrimary,
        "changeStreams.shardTargeting.controlEvents.movePrimary",
        "Number of movePrimary control events observed by v2 change stream shard targeters."),
    .namespacePlacementChanged = createShardTargetingCounter(
        otel::metrics::MetricNames::
            kChangeStreamShardTargetingControlEventNamespacePlacementChanged,
        "changeStreams.shardTargeting.controlEvents.namespacePlacementChanged",
        "Number of namespacePlacementChanged control events observed by v2 change stream shard "
        "targeters."),
    .databaseCreated = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingControlEventDatabaseCreated,
        "changeStreams.shardTargeting.controlEvents.databaseCreated",
        "Number of databaseCreated control events observed by v2 change stream shard targeters."),
};

}  // namespace

void ShardTargeterScopeMetricsRecorder::recordTransition(
    ChangeStreamShardTargeterStateEventHandler::DbPresenceState newPresence) {
    switch (newPresence) {
        case Presence::kDbPresent:
            dbPresent.add(1);
            return;
        case Presence::kDbAbsent:
            dbAbsent.add(1);
            return;
        case Presence::kUnknown:
            return;
    }
}

void ShardTargeterControlEventMetricsRecorder::recordControlEvent(const ControlEvent& event) {
    std::visit(
        [&]<typename T>(const T&) {
            if constexpr (std::is_same_v<T, MoveChunkControlEvent>) {
                moveChunk.add(1);
            } else if constexpr (std::is_same_v<T, MovePrimaryControlEvent>) {
                movePrimary.add(1);
            } else if constexpr (std::is_same_v<T, NamespacePlacementChangedControlEvent>) {
                namespacePlacementChanged.add(1);
            } else {
                static_assert(std::is_same_v<T, DatabaseCreatedControlEvent>);
                databaseCreated.add(1);
            }
        },
        event);
}

ShardTargeterScopeMetricsRecorder getCollectionShardTargeterScopeMetricsRecorder() {
    return ShardTargeterScopeMetricsRecorder{
        .dbPresent = kChangeStreamShardTargeterMetrics.collectionDbPresent,
        .dbAbsent = kChangeStreamShardTargeterMetrics.collectionDbAbsent,
    };
}

ShardTargeterScopeMetricsRecorder getDatabaseShardTargeterScopeMetricsRecorder() {
    return ShardTargeterScopeMetricsRecorder{
        .dbPresent = kChangeStreamShardTargeterMetrics.databaseDbPresent,
        .dbAbsent = kChangeStreamShardTargeterMetrics.databaseDbAbsent,
    };
}

AllDatabasesShardTargeterMetricsRecorder getAllDatabasesShardTargeterMetricsRecorder() {
    return AllDatabasesShardTargeterMetricsRecorder{
        .installed = kChangeStreamShardTargeterMetrics.allDatabases,
    };
}

ShardTargeterControlEventMetricsRecorder getShardTargeterControlEventMetricsRecorder() {
    return ShardTargeterControlEventMetricsRecorder{
        .moveChunk = kChangeStreamShardTargeterMetrics.moveChunk,
        .movePrimary = kChangeStreamShardTargeterMetrics.movePrimary,
        .namespacePlacementChanged = kChangeStreamShardTargeterMetrics.namespacePlacementChanged,
        .databaseCreated = kChangeStreamShardTargeterMetrics.databaseCreated,
    };
}

}  // namespace mongo

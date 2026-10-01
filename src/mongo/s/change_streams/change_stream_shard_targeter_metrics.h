// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Counts event-handler installations of a v2 change stream collection- or database-scoped shard
 * targeter into 'changeStreams.shardTargeting.targeterScope.<scope>.<dbPresent|dbAbsent>'. Held by
 * the targeter and invoked whenever it (re-)initializes its event handler.
 */
struct ShardTargeterScopeMetricsRecorder {
    otel::metrics::Counter<int64_t>& dbPresent;
    otel::metrics::Counter<int64_t>& dbAbsent;

    /**
     * The production handlers of the collection- and database-scoped targeters always report
     * db-present or db-absent; 'kUnknown' can only come from a test mock and is not recorded.
     */
    void recordTransition(ChangeStreamShardTargeterStateEventHandler::DbPresenceState newPresence);
};

/**
 * Counts event-handler installations of the v2 change stream all-databases shard targeter into
 * 'changeStreams.shardTargeting.targeterScope.allDatabases'. This targeter does not use the
 * db-present/db-absent split, so every installation counts.
 */
struct AllDatabasesShardTargeterMetricsRecorder {
    otel::metrics::Counter<int64_t>& installed;

    void recordTransition() {
        installed.add(1);
    }
};

/**
 * Counts a control event observed by a v2 change stream shard targeter into
 * 'changeStreams.shardTargeting.controlEvents.<event>'. Shared by all targeter scopes.
 */
struct ShardTargeterControlEventMetricsRecorder {
    otel::metrics::Counter<int64_t>& moveChunk;
    otel::metrics::Counter<int64_t>& movePrimary;
    otel::metrics::Counter<int64_t>& namespacePlacementChanged;
    otel::metrics::Counter<int64_t>& databaseCreated;

    void recordControlEvent(const ControlEvent& event);
};

/**
 * Factories returning recorders bound to the process-global OTEL instruments. Called once, at
 * shard-targeter construction time, so that targeters hold their metrics recorder as a member
 * instead of reaching for a global on every event.
 */
[[MONGO_MOD_PUBLIC]] ShardTargeterScopeMetricsRecorder
getCollectionShardTargeterScopeMetricsRecorder();
[[MONGO_MOD_PUBLIC]] ShardTargeterScopeMetricsRecorder
getDatabaseShardTargeterScopeMetricsRecorder();
[[MONGO_MOD_PUBLIC]] AllDatabasesShardTargeterMetricsRecorder
getAllDatabasesShardTargeterMetricsRecorder();
[[MONGO_MOD_PUBLIC]] ShardTargeterControlEventMetricsRecorder
getShardTargeterControlEventMetricsRecorder();

}  // namespace mongo

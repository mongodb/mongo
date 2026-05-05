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
#pragma once

#include "mongo/base/string_data.h"
#include "mongo/util/modules.h"

MONGO_MOD_PUBLIC;

namespace mongo {
// Forward declarations needed for MetricName to declare Passkey friends.
namespace disagg {
class MetricNameMaker;
}
namespace otel::metrics {
class MetricNameMaker;

/** Helper to implement the passkey idiom. */
template <typename T>
class MONGO_MOD_PUBLIC Passkey {
private:
    friend T;
    constexpr Passkey() = default;
};

/** Wrapper class around a string to ensure `MetricName`s are only constructed in certain places. */
class MONGO_MOD_PUBLIC MetricName {
public:
    /**
     * Note that this can only be constructed by code allowed to access the passkey. N&O must have
     * ownership of the files defining and instantiating the Passkey types. Additional Passkey types
     * are meant to facilitate cases where the metric names should not be visible outside some
     * module, in order to prevent leaking information related to that module.
     */
    constexpr MetricName(StringData name, Passkey<MetricNameMaker>) : _name(name) {}
    constexpr MetricName(StringData name, Passkey<disagg::MetricNameMaker>) : _name(name) {}

    constexpr StringData getName() const {
        return _name;
    }

    constexpr bool operator==(const MetricName& other) const {
        return getName() == other.getName();
    }

private:
    StringData _name;
};

/** Helper to create MetricName instances. */
class MONGO_MOD_FILE_PRIVATE MetricNameMaker {
public:
    static constexpr MetricName make(StringData name) {
        return MetricName(name, Passkey<MetricNameMaker>{});
    }
};

/**
 * Central registry of OpenTelemetry metric names used in the server. When adding a new metric to
 * the server, please add an entry to MetricNames grouped under your team name.
 *
 * This ensures that the N&O team has full ownership over new OTel metrics in the server for
 * centralized collaboration with downstream OTel consumers. OTel metrics are stored in time-series
 * DBs by the SRE team, and a sudden increase in metrics will result in operational costs ballooning
 * for the SRE team, which is why N&O owns this registry.
 */
class MetricNames {
public:
    // Networking & Observability Team Metrics
    static constexpr MetricName kPrometheusFileExporterWrites =
        MetricNameMaker::make("metrics.prometheus_file_exporter.writes");
    static constexpr MetricName kPrometheusFileExporterWritesFailed =
        MetricNameMaker::make("metrics.prometheus_file_exporter.failed_writes");
    static constexpr MetricName kPrometheusFileExporterWritesSkipped =
        MetricNameMaker::make("metrics.prometheus_file_exporter.skipped_writes");
    static constexpr MetricName kPrometheusFileExporterWriteDuration =
        MetricNameMaker::make("metrics.prometheus_file_exporter.write_duration");
    static constexpr MetricName kPrometheusFileExporterWriteSize =
        MetricNameMaker::make("metrics.prometheus_file_exporter.write_size");
    static constexpr MetricName kConnectionsProcessed =
        MetricNameMaker::make("network.connections_processed");
    static constexpr MetricName kIngressTLSHandshakeLatency =
        MetricNameMaker::make("network.ingress_tls_handshake_latency");
    static constexpr MetricName kOpenConnections =
        MetricNameMaker::make("network.open_ingress_connections");

    // Query Execution Team Metrics
    static constexpr MetricName kChangeStreamCursorsTotalOpened =
        MetricNameMaker::make("change_streams.cursor.total_opened");
    static constexpr MetricName kChangeStreamCursorsLifespan =
        MetricNameMaker::make("change_streams.cursor.lifespan");
    static constexpr MetricName kChangeStreamCursorsOpenTotal =
        MetricNameMaker::make("change_streams.cursor.open.total");
    static constexpr MetricName kChangeStreamCursorsOpenPinned =
        MetricNameMaker::make("change_streams.cursor.open.pinned");

    // Storage Execution Team Metrics
    static constexpr MetricName kIndexBuildsActive = MetricNameMaker::make("index_builds.active");
    static constexpr MetricName kIndexBuildsStarted = MetricNameMaker::make("index_builds.started");
    static constexpr MetricName kIndexBuildsSucceeded =
        MetricNameMaker::make("index_builds.succeeded");
    static constexpr MetricName kIndexBuildsFailed = MetricNameMaker::make("index_builds.failed");
    static constexpr MetricName kIndexBuildSideWritesInserted =
        MetricNameMaker::make("index_builds.side_writes.inserted");
    static constexpr MetricName kIndexBuildSideWritesDeleted =
        MetricNameMaker::make("index_builds.side_writes.deleted");
    static constexpr MetricName kIndexBuildSideWritesDrained =
        MetricNameMaker::make("index_builds.side_writes.drained");
    static constexpr MetricName kIndexBuildSideWritesDrainDuration =
        MetricNameMaker::make("index_builds.side_writes.drain_duration");
    static constexpr MetricName kIndexBuildSideWritesDrainBytes =
        MetricNameMaker::make("index_builds.side_writes.drain_bytes");
    static constexpr MetricName kIndexBuildSideWritesDrainYields =
        MetricNameMaker::make("index_builds.side_writes.drain_yields");
    static constexpr MetricName kReplicatedFastCountIsRunning =
        MetricNameMaker::make("replicated_fast_count.is_running");
    static constexpr MetricName kReplicatedFastCountFlushSuccessCount =
        MetricNameMaker::make("replicated_fast_count.flush.success_count");
    static constexpr MetricName kReplicatedFastCountFlushFailureCount =
        MetricNameMaker::make("replicated_fast_count.flush.failure_count");
    static constexpr MetricName kReplicatedFastCountFlushTimeMsMin =
        MetricNameMaker::make("replicated_fast_count.flush_time.min");
    static constexpr MetricName kReplicatedFastCountFlushTimeMsMax =
        MetricNameMaker::make("replicated_fast_count.flush_time.max");
    static constexpr MetricName kReplicatedFastCountFlushTimeMsTotal =
        MetricNameMaker::make("replicated_fast_count.flush_time.total");
    static constexpr MetricName kReplicatedFastCountFlushedDocsMin =
        MetricNameMaker::make("replicated_fast_count.flushed_docs.min");
    static constexpr MetricName kReplicatedFastCountFlushedDocsMax =
        MetricNameMaker::make("replicated_fast_count.flushed_docs.max");
    static constexpr MetricName kReplicatedFastCountFlushedDocsTotal =
        MetricNameMaker::make("replicated_fast_count.flushed_docs.total");
    static constexpr MetricName kReplicatedFastCountInsertCount =
        MetricNameMaker::make("replicated_fast_count.insert_count");
    static constexpr MetricName kReplicatedFastCountUpdateCount =
        MetricNameMaker::make("replicated_fast_count.update_count");
    static constexpr MetricName kReplicatedFastCountWriteTimeMsTotal =
        MetricNameMaker::make("replicated_fast_count.write_time.total");
    static constexpr MetricName kReplicatedFastCountCheckpointOplogEntriesProcessed =
        MetricNameMaker::make("replicated_fast_count.checkpoint.oplog_entries_processed");
    static constexpr MetricName kReplicatedFastCountCheckpointOplogEntriesSkipped =
        MetricNameMaker::make("replicated_fast_count.checkpoint.oplog_entries_skipped");
    static constexpr MetricName kReplicatedFastCountCheckpointSizeCountEntriesProcessed =
        MetricNameMaker::make("replicated_fast_count.checkpoint.size_count_entries_processed");
    static constexpr MetricName kReplicatedFastCountOplogLagSecs =
        MetricNameMaker::make("replicated_fast_count.oplog_lag_secs");

    static constexpr MetricName kIndexBuildKeysInsertedFromScan =
        MetricNameMaker::make("index_builds.keys_inserted_from_scan");
    static constexpr MetricName kIndexBuildDocsScanned =
        MetricNameMaker::make("index_builds.docs_scanned");
    static constexpr MetricName kIndexBuildKeysGeneratedFromScan =
        MetricNameMaker::make("index_builds.keys_generated_from_scan");

    // Replication Team Metrics
    static constexpr MetricName kOplogApplyBytes = MetricNameMaker::make("oplog.apply.bytes");

    // Query Integration Team Metrics

    // Op Counters
    static constexpr MetricName kInsertOpCount = MetricNameMaker::make("opcounters.inserts");
    static constexpr MetricName kQueryOpCount = MetricNameMaker::make("opcounters.queries");
    static constexpr MetricName kUpdateOpCount = MetricNameMaker::make("opcounters.updates");
    static constexpr MetricName kDeleteOpCount = MetricNameMaker::make("opcounters.deletes");
    static constexpr MetricName kGetMoreOpCount = MetricNameMaker::make("opcounters.get_mores");
    static constexpr MetricName kCommandOpCount = MetricNameMaker::make("opcounters.commands");
    // New in SERVER-123987 - Counts every top-level 'aggregate' command.
    static constexpr MetricName kAggregateOpCount = MetricNameMaker::make("opcounters.aggregates");

    // Test-only
    static constexpr MetricName kTest1 = MetricNameMaker::make("test_only.metric1");
    static constexpr MetricName kTest2 = MetricNameMaker::make("test_only.metric2");
    static constexpr MetricName kTest3 = MetricNameMaker::make("test_only.metric3");
    static constexpr MetricName kTest4 = MetricNameMaker::make("test_only.metric4");
    static constexpr MetricName kTest5 = MetricNameMaker::make("test_only.metric5");
    static constexpr MetricName kTest6 = MetricNameMaker::make("test_only.metric6");
    static constexpr MetricName kTestShardMergeNone =
        MetricNameMaker::make("test_only.shard_merge_none");
    static constexpr MetricName kTestShardMergeShard =
        MetricNameMaker::make("test_only.shard_merge_shard");
    static constexpr MetricName kTestShardMergeRouter =
        MetricNameMaker::make("test_only.shard_merge_router");
    static constexpr MetricName kTestRouterMergeNone =
        MetricNameMaker::make("test_only.router_merge_none");
    static constexpr MetricName kTestRouterMergeShard =
        MetricNameMaker::make("test_only.router_merge_shard");
    static constexpr MetricName kTestRouterMergeRouter =
        MetricNameMaker::make("test_only.router_merge_router");
    // camelCase is not allowed.
    static constexpr MetricName kTestInvalid = MetricNameMaker::make("test_only.Metric");
};

}  // namespace otel::metrics
}  // namespace mongo

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
// Forward declaration needed for MetricName to declare the friend.
namespace disagg {
class MetricName;
}
namespace otel::metrics {

/**
 * Wrapper class around a string to ensure `MetricName`s are only constructed in the class
 * definition of `MetricNames`.
 *
 * Note that this class is "open" only to enable defining module-specific metric names - see comment
 * in the "private" section.
 */
class MONGO_MOD_OPEN MetricName {
public:
    virtual ~MetricName() = default;

    constexpr StringData getName() const {
        return _name;
    };

    bool operator==(const MetricName& other) const {
        return getName() == other.getName();
    }

private:
    MONGO_MOD_PUBLIC explicit(false) constexpr MetricName(StringData name) : _name(name){};
    friend class MetricNames;
    /**
     * Module-specific metric names classes. N&O must have ownership of the files defining and
     * instantiating these classes. These classes are only allowed to use the private constructor.
     * This is only meant to facilitate cases where the metric names should not be visible outside
     * the module, in order to prevent leaking information related to that module.
     */
    friend class mongo::disagg::MetricName;

    StringData _name;
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
    static constexpr MetricName kPrometheusFileExporterWrites = {
        "metrics.prometheus_file_exporter.writes"};
    static constexpr MetricName kPrometheusFileExporterWritesFailed = {
        "metrics.prometheus_file_exporter.failed_writes"};
    static constexpr MetricName kPrometheusFileExporterWritesSkipped = {
        "metrics.prometheus_file_exporter.skipped_writes"};
    static constexpr MetricName kConnectionsProcessed = {"network.connections_processed"};
    static constexpr MetricName kIngressTLSHandshakeLatency = {
        "network.ingress_tls_handshake_latency"};
    static constexpr MetricName kOpenConnections = {"network.open_ingress_connections"};

    // Query Execution Team Metrics
    static constexpr MetricName kChangeStreamCursorsTotalOpened = {
        "change_streams.cursor.total_opened"};
    static constexpr MetricName kChangeStreamCursorsLifespan = {"change_streams.cursor.lifespan"};
    static constexpr MetricName kChangeStreamCursorsOpenTotal = {
        "change_streams.cursor.open.total"};
    static constexpr MetricName kChangeStreamCursorsOpenPinned = {
        "change_streams.cursor.open.pinned"};

    // Storage Execution Team Metrics
    static constexpr MetricName kIndexBuildsActive = {"index_builds.active"};
    static constexpr MetricName kIndexBuildsStarted = {"index_builds.started"};
    static constexpr MetricName kIndexBuildsSucceeded = {"index_builds.succeeded"};
    static constexpr MetricName kIndexBuildsFailed = {"index_builds.failed"};
    static constexpr MetricName kIndexBuildSideWritesInserted = {
        "index_builds.side_writes.inserted"};
    static constexpr MetricName kIndexBuildSideWritesDeleted = {"index_builds.side_writes.deleted"};
    static constexpr MetricName kIndexBuildSideWritesDrained = {"index_builds.side_writes.drained"};
    static constexpr MetricName kIndexBuildSideWritesDrainDuration = {
        "index_builds.side_writes.drain_duration"};
    static constexpr MetricName kIndexBuildSideWritesDrainBytes = {
        "index_builds.side_writes.drain_bytes"};
    static constexpr MetricName kIndexBuildSideWritesDrainYields = {
        "index_builds.side_writes.drain_yields"};
    static constexpr MetricName kReplicatedFastCountIsRunning = {
        "replicated_fast_count.is_running"};
    static constexpr MetricName kReplicatedFastCountFlushSuccessCount = {
        "replicated_fast_count.flush.success_count"};
    static constexpr MetricName kReplicatedFastCountFlushFailureCount = {
        "replicated_fast_count.flush.failure_count"};
    static constexpr MetricName kReplicatedFastCountFlushTimeMsMin = {
        "replicated_fast_count.flush_time.min"};
    static constexpr MetricName kReplicatedFastCountFlushTimeMsMax = {
        "replicated_fast_count.flush_time.max"};
    static constexpr MetricName kReplicatedFastCountFlushTimeMsTotal = {
        "replicated_fast_count.flush_time.total"};
    static constexpr MetricName kReplicatedFastCountFlushedDocsMin = {
        "replicated_fast_count.flushed_docs.min"};
    static constexpr MetricName kReplicatedFastCountFlushedDocsMax = {
        "replicated_fast_count.flushed_docs.max"};
    static constexpr MetricName kReplicatedFastCountFlushedDocsTotal = {
        "replicated_fast_count.flushed_docs.total"};
    static constexpr MetricName kReplicatedFastCountInsertCount = {
        "replicated_fast_count.insert_count"};
    static constexpr MetricName kReplicatedFastCountUpdateCount = {
        "replicated_fast_count.update_count"};
    static constexpr MetricName kReplicatedFastCountWriteTimeMsTotal = {
        "replicated_fast_count.write_time.total"};
    static constexpr MetricName kReplicatedFastCountCheckpointOplogEntriesProcessed = {
        "replicated_fast_count.checkpoint.oplog_entries_processed"};
    static constexpr MetricName kReplicatedFastCountCheckpointOplogEntriesSkipped = {
        "replicated_fast_count.checkpoint.oplog_entries_skipped"};
    static constexpr MetricName kReplicatedFastCountCheckpointSizeCountEntriesProcessed = {
        "replicated_fast_count.checkpoint.size_count_entries_processed"};

    static constexpr MetricName kIndexBuildKeysInsertedFromScan = {
        "index_builds.keys_inserted_from_scan"};
    static constexpr MetricName kIndexBuildDocsScanned = {"index_builds.docs_scanned"};
    static constexpr MetricName kIndexBuildKeysGeneratedFromScan = {
        "index_builds.keys_generated_from_scan"};

    // Replication Team Metrics
    static constexpr MetricName kOplogApplyBytes = {"oplog.apply.bytes"};

    // Query Integration Team Metrics

    // Op Counters
    static constexpr MetricName kInsertOpCount = {"opcounters.inserts"};
    static constexpr MetricName kQueryOpCount = {"opcounters.queries"};
    static constexpr MetricName kUpdateOpCount = {"opcounters.updates"};
    static constexpr MetricName kDeleteOpCount = {"opcounters.deletes"};
    static constexpr MetricName kGetMoreOpCount = {"opcounters.get_mores"};
    static constexpr MetricName kCommandOpCount = {"opcounters.commands"};
    // New in SERVER-123987 - Counts every top-level 'aggregate' command.
    static constexpr MetricName kAggregateOpCount = {"opcounters.aggregates"};

    // Test-only
    static constexpr MetricName kTest1 = {"test_only.metric1"};
    static constexpr MetricName kTest2 = {"test_only.metric2"};
    static constexpr MetricName kTest3 = {"test_only.metric3"};
    static constexpr MetricName kTest4 = {"test_only.metric4"};
    static constexpr MetricName kTest5 = {"test_only.metric5"};
    static constexpr MetricName kTest6 = {"test_only.metric6"};
    static constexpr MetricName kTestShardMergeNone = {"test_only.shard_merge_none"};
    static constexpr MetricName kTestShardMergeShard = {"test_only.shard_merge_shard"};
    static constexpr MetricName kTestShardMergeRouter = {"test_only.shard_merge_router"};
    static constexpr MetricName kTestRouterMergeNone = {"test_only.router_merge_none"};
    static constexpr MetricName kTestRouterMergeShard = {"test_only.router_merge_shard"};
    static constexpr MetricName kTestRouterMergeRouter = {"test_only.router_merge_router"};
    // camelCase is not allowed.
    static constexpr MetricName kTestInvalid = {"test_only.Metric"};
};

}  // namespace otel::metrics
}  // namespace mongo

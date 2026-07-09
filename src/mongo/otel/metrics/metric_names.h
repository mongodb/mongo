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

#include "mongo/util/modules.h"

#include <string_view>

MONGO_MOD_PUBLIC;

namespace mongo {
// Forward declarations needed for DynamicMetricNameMaker to declare Passkey friends
class DiskMetrics;
class SystemMountMetrics;

// Forward declarations needed for MetricName to declare Passkey friends.
namespace disagg {
class MetricNameMaker;
}
namespace otel::metrics {
class MetricNameMaker;
class DynamicMetricNameMaker;
class DynamicMetricNameTestPasskeyMaker;

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
    constexpr MetricName(std::string_view name, Passkey<MetricNameMaker>) : _name(name) {}
    constexpr MetricName(std::string_view name, Passkey<disagg::MetricNameMaker>) : _name(name) {}
    constexpr std::string_view getName() const {
        return _name;
    }

    constexpr bool operator==(const MetricName& other) const {
        return getName() == other.getName();
    }

private:
    std::string_view _name;
};

/** Helper to create MetricName instances. */
class MONGO_MOD_FILE_PRIVATE MetricNameMaker{public : static constexpr MetricName make(
    std::string_view name){return MetricName(name, Passkey<MetricNameMaker>{});
}  // namespace otel::metrics
};  // namespace mongo

/**
 * Helper to create MetricName instances with runtime-constructed names (e.g. names that embed
 * device names or mount paths discovered at startup). Requires N&O review since dynamic names
 * cannot be audited at compile time.
 */
class MONGO_MOD_PUBLIC DynamicMetricNameMaker {
public:
    /**
     * Classes that need to create dynamic metric names should be added as a
     * friend to the Passkey.
     */
    class Passkey {
        friend ::mongo::DiskMetrics;
        friend ::mongo::SystemMountMetrics;
        // This allows us to create dynamic metric names in tests
        friend ::mongo::otel::metrics::DynamicMetricNameTestPasskeyMaker;
        constexpr Passkey() = default;
    };

    static MetricName make(std::string_view name, Passkey passkey) {
        return MetricNameMaker::make(name);
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
    static constexpr MetricName kNetworkIngressBytesIn =
        MetricNameMaker::make("serverStatus.network.bytesIn");
    static constexpr MetricName kNetworkIngressBytesOut =
        MetricNameMaker::make("serverStatus.network.bytesOut");
    static constexpr MetricName kNetworkIngressNumRequests =
        MetricNameMaker::make("serverStatus.network.numRequests");
    static constexpr MetricName kNetworkEgressBytesIn =
        MetricNameMaker::make("serverStatus.network.egress.bytesIn");
    static constexpr MetricName kNetworkEgressBytesOut =
        MetricNameMaker::make("serverStatus.network.egress.bytesOut");
    static constexpr MetricName kNetworkEgressNumRequests =
        MetricNameMaker::make("serverStatus.network.egress.numRequests");
    static constexpr MetricName kNetworkNumSlowDNSOperations =
        MetricNameMaker::make("serverStatus.network.numSlowDNSOperations");
    static constexpr MetricName kNetworkNumSlowSSLOperations =
        MetricNameMaker::make("serverStatus.network.numSlowSSLOperations");
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
    static constexpr MetricName kIngressTLSHandshakeLatency =
        MetricNameMaker::make("network.ingress_tls_handshake_latency");
    static constexpr MetricName kConnectionsCurrent =
        MetricNameMaker::make("serverStatus.connections.current");
    static constexpr MetricName kConnectionsAvailable =
        MetricNameMaker::make("serverStatus.connections.available");
    static constexpr MetricName kConnectionsTotalCreated =
        MetricNameMaker::make("serverStatus.connections.totalCreated");
    static constexpr MetricName kConnectionsRejected =
        MetricNameMaker::make("serverStatus.connections.rejected");
    static constexpr MetricName kConnectionsActive =
        MetricNameMaker::make("serverStatus.connections.active");
    static constexpr MetricName kMongoDBBuildInfo = MetricNameMaker::make("mongodb.build.info");

    // Ingress Request Rate Limiter (Admission Control) Metrics. These mirror the fields that
    // RateLimiter::appendStats() historically reported under
    static constexpr MetricName kIngressRequestRateLimiterAttemptedAdmissions =
        MetricNameMaker::make("serverStatus.network.ingressRequestRateLimiter.attemptedAdmissions");
    static constexpr MetricName kIngressRequestRateLimiterSuccessfulAdmissions =
        MetricNameMaker::make(
            "serverStatus.network.ingressRequestRateLimiter.successfulAdmissions");
    static constexpr MetricName kIngressRequestRateLimiterRejectedAdmissions =
        MetricNameMaker::make("serverStatus.network.ingressRequestRateLimiter.rejectedAdmissions");
    static constexpr MetricName kIngressRequestRateLimiterExemptedAdmissions =
        MetricNameMaker::make("serverStatus.network.ingressRequestRateLimiter.exemptedAdmissions");
    static constexpr MetricName kIngressRequestRateLimiterAddedToQueue =
        MetricNameMaker::make("serverStatus.network.ingressRequestRateLimiter.addedToQueue");
    static constexpr MetricName kIngressRequestRateLimiterRemovedFromQueue =
        MetricNameMaker::make("serverStatus.network.ingressRequestRateLimiter.removedFromQueue");
    static constexpr MetricName kIngressRequestRateLimiterInterruptedInQueue =
        MetricNameMaker::make("serverStatus.network.ingressRequestRateLimiter.interruptedInQueue");
    static constexpr MetricName kIngressRequestRateLimiterTokensAcquired =
        MetricNameMaker::make("serverStatus.network.ingressRequestRateLimiter.tokensAcquired");
    static constexpr MetricName kIngressRequestRateLimiterCurrentQueueDepth =
        MetricNameMaker::make("serverStatus.network.ingressRequestRateLimiter.currentQueueDepth");
    static constexpr MetricName kIngressRequestRateLimiterTotalAvailableTokens =
        MetricNameMaker::make(
            "serverStatus.network.ingressRequestRateLimiter.totalAvailableTokens");
    static constexpr MetricName kIngressRequestRateLimiterAverageTimeQueuedMicros =
        MetricNameMaker::make(
            "serverStatus.network.ingressRequestRateLimiter.averageTimeQueuedMicros");
    static constexpr MetricName kIngressRequestRateLimiterTimeQueuedMicros =
        MetricNameMaker::make("serverStatus.network.ingressRequestRateLimiter.timeQueuedMicros");

    // Query Execution Team Metrics
    static constexpr MetricName kChangeStreamCursorsTotalOpened =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.totalOpened");
    static constexpr MetricName kChangeStreamCursorsLifespan =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.lifespan");
    static constexpr MetricName kChangeStreamCursorsOpenTotal =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.open.total");
    static constexpr MetricName kChangeStreamCursorsOpenPinned =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.open.pinned");
    static constexpr MetricName kChangeStreamCursorsOpenOptimeMin =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.open.optime.min");
    static constexpr MetricName kChangeStreamCursorsOpenOptimeMax =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.open.optime.max");
    static constexpr MetricName kChangeStreamBatchExecMicrosSum =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.batch.execMicros.sum");
    static constexpr MetricName kChangeStreamBatchShardLatencyMicrosSum =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.batch.shardLatencyMicros.sum");
    static constexpr MetricName kChangeStreamBatchConfigLatencyMicrosSum =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.batch.configLatencyMicros.sum");
    // Per-(consumer x engine) single-document lookup metrics for change stream updateLookup.
    static constexpr MetricName kChangeStreamUpdateLookupExpressFound =
        MetricNameMaker::make("changeStreams.updateLookup.express.found");
    static constexpr MetricName kChangeStreamUpdateLookupExpressNotFound =
        MetricNameMaker::make("changeStreams.updateLookup.express.notFound");
    static constexpr MetricName kChangeStreamUpdateLookupExpressNotHandled =
        MetricNameMaker::make("changeStreams.updateLookup.express.notHandled");
    static constexpr MetricName kChangeStreamUpdateLookupExpressLatency =
        MetricNameMaker::make("changeStreams.updateLookup.express.latencyMicros");
    static constexpr MetricName kChangeStreamUpdateLookupAggregationFound =
        MetricNameMaker::make("changeStreams.updateLookup.aggregation.found");
    static constexpr MetricName kChangeStreamUpdateLookupAggregationNotFound =
        MetricNameMaker::make("changeStreams.updateLookup.aggregation.notFound");
    static constexpr MetricName kChangeStreamUpdateLookupAggregationNotHandled =
        MetricNameMaker::make("changeStreams.updateLookup.aggregation.notHandled");
    static constexpr MetricName kChangeStreamUpdateLookupAggregationLatency =
        MetricNameMaker::make("changeStreams.updateLookup.aggregation.latencyMicros");
    static constexpr MetricName kChangeStreamErrorNonRetriableHistoryLost = MetricNameMaker::make(
        "serverStatus.metrics.changeStreams.error.nonRetriable.changeStreamHistoryLost");
    static constexpr MetricName kChangeStreamErrorNonRetriableFatalError = MetricNameMaker::make(
        "serverStatus.metrics.changeStreams.error.nonRetriable.changeStreamFatalError");
    static constexpr MetricName kChangeStreamErrorNonRetriableBsonObjectTooLarge =
        MetricNameMaker::make(
            "serverStatus.metrics.changeStreams.error.nonRetriable.bsonObjectTooLarge");
    static constexpr MetricName kChangeStreamErrorNonRetriableOther =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.error.nonRetriable.other");
    static constexpr MetricName kChangeStreamErrorRetriableInterruptedDueToReplStateChange =
        MetricNameMaker::make(
            "serverStatus.metrics.changeStreams.error.retriable.interruptedDueToReplStateChange");
    static constexpr MetricName kChangeStreamErrorRetriableOther =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.error.retriable.other");
    static constexpr MetricName kChangeStreamOptionShowExpandedEvents =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.showExpandedEvents");
    static constexpr MetricName kChangeStreamOptionShowMigrationEvents =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.showMigrationEvents");
    static constexpr MetricName kChangeStreamOptionShowSystemEvents =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.showSystemEvents");
    static constexpr MetricName kChangeStreamOptionShowRawUpdateDescription =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.showRawUpdateDescription");
    static constexpr MetricName kChangeStreamOptionIgnoreRemovedShards =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.ignoreRemovedShards");
    static constexpr MetricName kChangeStreamOptionMatchCollectionUUIDForUpdateLookup =
        MetricNameMaker::make(
            "serverStatus.metrics.changeStreams.option.matchCollectionUUIDForUpdateLookup");
    static constexpr MetricName kChangeStreamOptionStartAfter =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.startAfter");
    static constexpr MetricName kChangeStreamOptionResumeAfter =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.resumeAfter");
    static constexpr MetricName kChangeStreamOptionStartAtOperationTime =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.startAtOperationTime");
    static constexpr MetricName kChangeStreamOptionFullDocumentRequired =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.fullDocument.required");
    static constexpr MetricName kChangeStreamOptionFullDocumentUpdateLookup = MetricNameMaker::make(
        "serverStatus.metrics.changeStreams.option.fullDocument.updateLookup");
    static constexpr MetricName kChangeStreamOptionFullDocumentWhenAvailable =
        MetricNameMaker::make(
            "serverStatus.metrics.changeStreams.option.fullDocument.whenAvailable");
    static constexpr MetricName kChangeStreamOptionFullDocumentBeforeChangeRequired =
        MetricNameMaker::make(
            "serverStatus.metrics.changeStreams.option.fullDocumentBeforeChange.required");
    static constexpr MetricName kChangeStreamOptionFullDocumentBeforeChangeWhenAvailable =
        MetricNameMaker::make(
            "serverStatus.metrics.changeStreams.option.fullDocumentBeforeChange.whenAvailable");
    static constexpr MetricName kChangeStreamScopeCluster =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.scope.cluster");
    static constexpr MetricName kChangeStreamScopeDb =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.scope.db");
    static constexpr MetricName kChangeStreamScopeCollection =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.scope.collection");
    static constexpr MetricName kChangeStreamOptionCursorBatchSize =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.cursor.batchSize");
    static constexpr MetricName kChangeStreamOptionCursorMaxTimeMS =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.option.cursor.maxTimeMS");
    static constexpr MetricName kChangeStreamCursorDocsReturned =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.docsReturned");
    static constexpr MetricName kChangeStreamCursorBytesReturned =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.bytesReturned");
    static constexpr MetricName kChangeStreamCursorBatchesReturned =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.batchesReturned");
    static constexpr MetricName kChangeStreamCursorDocsExamined =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.docsExamined");
    static constexpr MetricName kChangeStreamCursorBytesRead =
        MetricNameMaker::make("serverStatus.metrics.changeStreams.cursor.bytesRead");

    // Storage Execution Team Metrics
    static constexpr MetricName kIndexBuildsActive = MetricNameMaker::make("index_builds.active");
    static constexpr MetricName kIndexBuildsStarted = MetricNameMaker::make("index_builds.started");
    static constexpr MetricName kIndexBuildsSucceeded =
        MetricNameMaker::make("index_builds.succeeded");
    static constexpr MetricName kIndexBuildsFailed = MetricNameMaker::make("index_builds.failed");
    static constexpr MetricName kIndexBuildsToBeResumed =
        MetricNameMaker::make("index_builds.to_be_resumed");
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
    static constexpr MetricName kIndexBuildResumeSucceeded =
        MetricNameMaker::make("index_builds.resume.succeeded");
    static constexpr MetricName kIndexBuildResumeFailed =
        MetricNameMaker::make("index_builds.resume.failed");

    static constexpr MetricName kTtlPasses =
        MetricNameMaker::make("serverStatus.metrics.ttl.passes");
    static constexpr MetricName kTtlSubPasses =
        MetricNameMaker::make("serverStatus.metrics.ttl.subPasses");
    static constexpr MetricName kTtlDuration =
        MetricNameMaker::make("serverStatus.metrics.ttl.durationMicros");
    static constexpr MetricName kTtlDeletedDocuments =
        MetricNameMaker::make("serverStatus.metrics.ttl.deletedDocuments");
    static constexpr MetricName kTtlDeletedKeys =
        MetricNameMaker::make("serverStatus.metrics.ttl.deletedKeys");
    static constexpr MetricName kTtlExaminedDocuments =
        MetricNameMaker::make("serverStatus.metrics.ttl.examinedDocuments");
    static constexpr MetricName kTtlExaminedKeys =
        MetricNameMaker::make("serverStatus.metrics.ttl.examinedKeys");
    static constexpr MetricName kTtlInvalidTtlIndexSkips =
        MetricNameMaker::make("serverStatus.metrics.ttl.invalidTTLIndexSkips");
    static constexpr MetricName kTtlTimeQueuedForTickets =
        MetricNameMaker::make("serverStatus.metrics.ttl.timeQueuedForTicketsMicros");
    static constexpr MetricName kTtlTimeProcessingWithTickets =
        MetricNameMaker::make("serverStatus.metrics.ttl.timeProcessingWithTicketsMicros");
    static constexpr MetricName kTtlTicketAdmissions =
        MetricNameMaker::make("serverStatus.metrics.ttl.ticketAdmissions");
    static constexpr MetricName kTtlLowPriorityTicketAdmissions =
        MetricNameMaker::make("serverStatus.metrics.ttl.lowPriorityTicketAdmissions");
    static constexpr MetricName kTtlQueuedForTickets =
        MetricNameMaker::make("serverStatus.metrics.ttl.queuedForTickets");
    static constexpr MetricName kIndexCount =
        MetricNameMaker::make("serverStatus.indexStats.count");
    static constexpr MetricName kIndexStatsMultikeyNewPathsOrdinaryInTransaction =
        MetricNameMaker::make("serverStatus.indexStats.multikey.newPaths.ordinary.inTransaction");
    static constexpr MetricName kIndexStatsMultikeyNewPathsOrdinaryOutsideTransaction =
        MetricNameMaker::make(
            "serverStatus.indexStats.multikey.newPaths.ordinary.outsideTransaction");
    static constexpr MetricName kIndexStatsMultikeyNewPathsWildcardInTransaction =
        MetricNameMaker::make("serverStatus.indexStats.multikey.newPaths.wildcard.inTransaction");
    static constexpr MetricName kIndexStatsMultikeyNewPathsWildcardOutsideTransaction =
        MetricNameMaker::make(
            "serverStatus.indexStats.multikey.newPaths.wildcard.outsideTransaction");
    static constexpr MetricName kIndexStatsMultikeySideTransactions =
        MetricNameMaker::make("serverStatus.indexStats.multikey.sideTransactions");

    static constexpr MetricName kIndexBuildsTotal =
        MetricNameMaker::make("serverStatus.indexBuilds.total");
    static constexpr MetricName kIndexBuildPhasesCommit =
        MetricNameMaker::make("serverStatus.indexBuilds.phases.commit");

    static constexpr MetricName kIndexBulkBuilderNumSorted =
        MetricNameMaker::make("serverStatus.indexBulkBuilder.numSorted");
    static constexpr MetricName kIndexBulkBuilderBytesSorted =
        MetricNameMaker::make("serverStatus.indexBulkBuilder.bytesSorted");
    static constexpr MetricName kIndexBulkBuilderBytesSpilled =
        MetricNameMaker::make("serverStatus.indexBulkBuilder.bytesSpilled");
    static constexpr MetricName kIndexBulkBuilderBytesSpilledUncompressed =
        MetricNameMaker::make("serverStatus.indexBulkBuilder.bytesSpilledUncompressed");
    static constexpr MetricName kIndexBulkBuilderMemUsage =
        MetricNameMaker::make("serverStatus.indexBulkBuilder.memUsage");
    static constexpr MetricName kIndexBulkBuilderSpilledRanges =
        MetricNameMaker::make("serverStatus.indexBulkBuilder.spilledRanges");

    // Replication Team Metrics
    static constexpr MetricName kOplogApplyBytes = MetricNameMaker::make("oplog.apply.bytes");
    static constexpr MetricName kOplogApplyBufferCount =
        MetricNameMaker::make("serverStatus.metrics.repl.buffer.apply.count");
    static constexpr MetricName kOplogApplyBufferSize =
        MetricNameMaker::make("serverStatus.metrics.repl.buffer.apply.sizeBytes");
    static constexpr MetricName kOplogApplyBufferMaxSize =
        MetricNameMaker::make("serverStatus.metrics.repl.buffer.apply.maxSizeBytes");
    static constexpr MetricName kOplogApplyBufferMaxCount =
        MetricNameMaker::make("serverStatus.metrics.repl.buffer.apply.maxCount");
    static constexpr MetricName kOplogWriteBufferCount =
        MetricNameMaker::make("serverStatus.metrics.repl.buffer.write.count");
    static constexpr MetricName kOplogWriteBufferSize =
        MetricNameMaker::make("serverStatus.metrics.repl.buffer.write.sizeBytes");
    static constexpr MetricName kOplogWriteBufferMaxSize =
        MetricNameMaker::make("serverStatus.metrics.repl.buffer.write.maxSizeBytes");
    static constexpr MetricName kApplyBatchesNum =
        MetricNameMaker::make("serverStatus.metrics.repl.apply.batches.num");
    static constexpr MetricName kApplyBatchesTotalMillis =
        MetricNameMaker::make("serverStatus.metrics.repl.apply.batches.totalMillis");
    static constexpr MetricName kInitialSyncFailedAttempts =
        MetricNameMaker::make("serverStatus.metrics.repl.initialSync.failedAttempts");
    static constexpr MetricName kInitialSyncFailures =
        MetricNameMaker::make("serverStatus.metrics.repl.initialSync.failures");
    static constexpr MetricName kInitialSyncCompleted =
        MetricNameMaker::make("serverStatus.metrics.repl.initialSync.completed");
    static constexpr MetricName kReplNetworkBytes =
        MetricNameMaker::make("serverStatus.metrics.repl.network.bytes");
    static constexpr MetricName kReplNetworkBytesSent =
        MetricNameMaker::make("serverStatus.metrics.repl.network.bytesSent");
    static constexpr MetricName kGetLastErrorWtimeNum =
        MetricNameMaker::make("serverStatus.metrics.getLastError.wtime.num");
    static constexpr MetricName kGetLastErrorWtimeTotalMillis =
        MetricNameMaker::make("serverStatus.metrics.getLastError.wtime.totalMillis");
    static constexpr MetricName kGetLastErrorWtimeouts =
        MetricNameMaker::make("serverStatus.metrics.getLastError.wtimeouts");
    static constexpr MetricName kGetLastErrorDefaultWtimeouts =
        MetricNameMaker::make("serverStatus.metrics.getLastError.default.wtimeouts");
    static constexpr MetricName kGetLastErrorDefaultUnsatisfiable =
        MetricNameMaker::make("serverStatus.metrics.getLastError.default.unsatisfiable");

    // Query Integration Team Metrics

    // System Health
    static constexpr MetricName kCpuTime = MetricNameMaker::make("mongodb.system.cpu.time");
    static constexpr MetricName kCpuUtilization =
        MetricNameMaker::make("mongodb.system.cpu.utilization");
    static constexpr MetricName kThreadActive =
        MetricNameMaker::make("mongodb.system.thread.active");
    static constexpr MetricName kThreadQueued =
        MetricNameMaker::make("mongodb.system.thread.queued");
    static constexpr MetricName kFdOpen = MetricNameMaker::make("mongodb.system.fd.open");
    static constexpr MetricName kSystemHealthCollectErrors =
        MetricNameMaker::make("mongodb.systemHealth.collectErrors");

    static constexpr MetricName kProcessCpuTime = MetricNameMaker::make("mongodb.process.cpu.time");
    static constexpr MetricName kProcessCpuUtilization =
        MetricNameMaker::make("mongodb.process.cpu.utilization");
    static constexpr MetricName kProcessContextSwitches =
        MetricNameMaker::make("mongodb.process.context.switch");
    static constexpr MetricName kProcessThreadCount =
        MetricNameMaker::make("mongodb.process.threads.count");
    static constexpr MetricName kProcessPagingFaults =
        MetricNameMaker::make("mongodb.process.paging.faults");
    static constexpr MetricName kProcessHealthCollectErrors =
        MetricNameMaker::make("mongodb.process.collectErrors");

    // Global Lock
    static constexpr MetricName kGlobalLockTotalTime =
        MetricNameMaker::make("serverStatus.globalLock.totalTime");
    static constexpr MetricName kGlobalLockCurrentQueueTotal =
        MetricNameMaker::make("serverStatus.globalLock.currentQueue.total");
    static constexpr MetricName kGlobalLockCurrentQueueReaders =
        MetricNameMaker::make("serverStatus.globalLock.currentQueue.readers");
    static constexpr MetricName kGlobalLockCurrentQueueWriters =
        MetricNameMaker::make("serverStatus.globalLock.currentQueue.writers");
    static constexpr MetricName kGlobalLockActiveClientsTotal =
        MetricNameMaker::make("serverStatus.globalLock.activeClients.total");
    static constexpr MetricName kGlobalLockActiveClientsReaders =
        MetricNameMaker::make("serverStatus.globalLock.activeClients.readers");
    static constexpr MetricName kGlobalLockActiveClientsWriters =
        MetricNameMaker::make("serverStatus.globalLock.activeClients.writers");

    static constexpr MetricName kOperationLatency =
        MetricNameMaker::make("serverStatus.opLatencies.latency");

    // Op Counters
    static constexpr MetricName kInsertOpCount =
        MetricNameMaker::make("serverStatus.opcounters.inserts");
    static constexpr MetricName kQueryOpCount =
        MetricNameMaker::make("serverStatus.opcounters.queries");
    static constexpr MetricName kUpdateOpCount =
        MetricNameMaker::make("serverStatus.opcounters.updates");
    static constexpr MetricName kDeleteOpCount =
        MetricNameMaker::make("serverStatus.opcounters.deletes");
    static constexpr MetricName kGetMoreOpCount =
        MetricNameMaker::make("serverStatus.opcounters.getMores");
    static constexpr MetricName kCommandOpCount =
        MetricNameMaker::make("serverStatus.opcounters.commands");
    // New in SERVER-123987 - Counts every top-level 'aggregate' command.
    static constexpr MetricName kAggregateOpCount =
        MetricNameMaker::make("serverStatus.opcounters.aggregates");

    // Asserts - Counts every assertion failure broken down by `kind` attribute (one
    // of: "regular", "msg", "user", "tripwire"). Mirrors the per-type counters under
    // `serverStatus.asserts.*`; `warning` is omitted because nothing increments it, and
    // `rollovers` is omitted because the OTel counter is int64 and never wraps.
    // TODO (follow-up SERVER ticket): add a `command` attribute so failures can be sliced by
    // command name (the "(c)" approach from the design discussion).
    static constexpr MetricName kAsserts = MetricNameMaker::make("serverStatus.asserts");

    // Query Performance Counters
    static constexpr MetricName kQueryExecutorScanned =
        MetricNameMaker::make("serverStatus.metrics.queryExecutor.scanned");
    static constexpr MetricName kQueryExecutorScannedObjects =
        MetricNameMaker::make("serverStatus.metrics.queryExecutor.scannedObjects");
    static constexpr MetricName kDocumentReturned =
        MetricNameMaker::make("serverStatus.metrics.document.returned");

    // Plan cache counters — classic engine
    static constexpr MetricName kPlanCacheClassicHits =
        MetricNameMaker::make("serverStatus.metrics.query.planCache.classic.hits");
    static constexpr MetricName kPlanCacheClassicMisses =
        MetricNameMaker::make("serverStatus.metrics.query.planCache.classic.misses");
    static constexpr MetricName kPlanCacheClassicSkipped =
        MetricNameMaker::make("serverStatus.metrics.query.planCache.classic.skipped");
    static constexpr MetricName kPlanCacheClassicReplanned =
        MetricNameMaker::make("serverStatus.metrics.query.planCache.classic.replanned");
    static constexpr MetricName kPlanCacheClassicReplannedPlanIsCachedPlan = MetricNameMaker::make(
        "serverStatus.metrics.query.planCache.classic.replanned_plan_is_cached_plan");
    static constexpr MetricName kPlanCacheClassicCachedPlansEvicted =
        MetricNameMaker::make("serverStatus.metrics.query.planCache.classic.cached_plans_evicted");
    static constexpr MetricName kPlanCacheClassicInactiveCachedPlansReplaced =
        MetricNameMaker::make(
            "serverStatus.metrics.query.planCache.classic.inactive_cached_plans_replaced");

    // Plan cache counters — SBE engine
    static constexpr MetricName kPlanCacheSbeHits =
        MetricNameMaker::make("serverStatus.metrics.query.planCache.sbe.hits");
    static constexpr MetricName kPlanCacheSbeMisses =
        MetricNameMaker::make("serverStatus.metrics.query.planCache.sbe.misses");
    static constexpr MetricName kPlanCacheSbeSkipped =
        MetricNameMaker::make("serverStatus.metrics.query.planCache.sbe.skipped");
    static constexpr MetricName kPlanCacheSbeReplanned =
        MetricNameMaker::make("serverStatus.metrics.query.planCache.sbe.replanned");
    static constexpr MetricName kPlanCacheSbeReplannedPlanIsCachedPlan = MetricNameMaker::make(
        "serverStatus.metrics.query.planCache.sbe.replanned_plan_is_cached_plan");
    static constexpr MetricName kPlanCacheSbeCachedPlansEvicted =
        MetricNameMaker::make("serverStatus.metrics.query.planCache.sbe.cached_plans_evicted");
    static constexpr MetricName kPlanCacheSbeInactiveCachedPlansReplaced = MetricNameMaker::make(
        "serverStatus.metrics.query.planCache.sbe.inactive_cached_plans_replaced");

    // Query framework engine-mix counters
    static constexpr MetricName kQueryFrameworkFindSbe =
        MetricNameMaker::make("serverStatus.metrics.query.queryFramework.find.sbe");
    static constexpr MetricName kQueryFrameworkFindClassic =
        MetricNameMaker::make("serverStatus.metrics.query.queryFramework.find.classic");
    static constexpr MetricName kQueryFrameworkAggregateSbeOnly =
        MetricNameMaker::make("serverStatus.metrics.query.queryFramework.aggregate.sbeOnly");
    static constexpr MetricName kQueryFrameworkAggregateClassicOnly =
        MetricNameMaker::make("serverStatus.metrics.query.queryFramework.aggregate.classicOnly");
    static constexpr MetricName kQueryFrameworkAggregateSbeHybrid =
        MetricNameMaker::make("serverStatus.metrics.query.queryFramework.aggregate.sbeHybrid");
    static constexpr MetricName kQueryFrameworkAggregateClassicHybrid =
        MetricNameMaker::make("serverStatus.metrics.query.queryFramework.aggregate.classicHybrid");

    // Fast-path planning counters
    static constexpr MetricName kFastPathIdHack =
        MetricNameMaker::make("serverStatus.metrics.query.planning.fastPath.idHack");
    static constexpr MetricName kFastPathExpress =
        MetricNameMaker::make("serverStatus.metrics.query.planning.fastPath.express");

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

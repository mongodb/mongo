// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/util/modules.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
// Forward declarations needed for DynamicMetricNameMaker to declare Passkey friends
class DiskMetrics;
class ObservableMutexMetrics;
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
class [[MONGO_MOD_PUBLIC]] Passkey {
private:
    friend T;
    constexpr Passkey() = default;
};

/** Wrapper class around a string to ensure `MetricName`s are only constructed in certain places. */
class [[MONGO_MOD_PUBLIC]] MetricName {
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
class [[MONGO_MOD_FILE_PRIVATE]] MetricNameMaker {
public:
    static constexpr MetricName make(std::string_view name) {
        return MetricName(name, Passkey<MetricNameMaker>{});
    }  // namespace otel::metrics
};  // namespace mongo

/**
 * Helper to create MetricName instances with runtime-constructed names (e.g. names that embed
 * device names or mount paths discovered at startup). Requires N&O review since dynamic names
 * cannot be audited at compile time.
 */
class [[MONGO_MOD_PUBLIC]] DynamicMetricNameMaker {
public:
    /**
     * Classes that need to create dynamic metric names should be added as a
     * friend to the Passkey.
     */
    class Passkey {
        friend ::mongo::DiskMetrics;
        friend ::mongo::ObservableMutexMetrics;
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
        MetricNameMaker::make("mongodb.serverStatus.network.bytesIn");
    static constexpr MetricName kNetworkIngressBytesOut =
        MetricNameMaker::make("mongodb.serverStatus.network.bytesOut");
    static constexpr MetricName kNetworkIngressNumRequests =
        MetricNameMaker::make("mongodb.serverStatus.network.numRequests");
    static constexpr MetricName kNetworkEgressBytesIn =
        MetricNameMaker::make("mongodb.serverStatus.network.egress.bytesIn");
    static constexpr MetricName kNetworkEgressBytesOut =
        MetricNameMaker::make("mongodb.serverStatus.network.egress.bytesOut");
    static constexpr MetricName kNetworkEgressNumRequests =
        MetricNameMaker::make("mongodb.serverStatus.network.egress.numRequests");
    static constexpr MetricName kNetworkNumSlowDNSOperations =
        MetricNameMaker::make("mongodb.serverStatus.network.numSlowDNSOperations");
    static constexpr MetricName kNetworkNumSlowSSLOperations =
        MetricNameMaker::make("mongodb.serverStatus.network.numSlowSSLOperations");
    static constexpr MetricName kPrometheusFileExporterWrites =
        MetricNameMaker::make("mongodb.metrics.prometheus_file_exporter.writes");
    static constexpr MetricName kPrometheusFileExporterWritesFailed =
        MetricNameMaker::make("mongodb.metrics.prometheus_file_exporter.failed_writes");
    static constexpr MetricName kPrometheusFileExporterWritesSkipped =
        MetricNameMaker::make("mongodb.metrics.prometheus_file_exporter.skipped_writes");
    static constexpr MetricName kPrometheusFileExporterWriteDuration =
        MetricNameMaker::make("mongodb.metrics.prometheus_file_exporter.write_duration");
    static constexpr MetricName kPrometheusFileExporterWriteSize =
        MetricNameMaker::make("mongodb.metrics.prometheus_file_exporter.write_size");
    static constexpr MetricName kIngressTLSHandshakeLatency =
        MetricNameMaker::make("mongodb.network.ingress_tls_handshake_latency");
    static constexpr MetricName kConnectionsCurrent =
        MetricNameMaker::make("mongodb.serverStatus.connections.current");
    static constexpr MetricName kConnectionsAvailable =
        MetricNameMaker::make("mongodb.serverStatus.connections.available");
    static constexpr MetricName kConnectionsTotalCreated =
        MetricNameMaker::make("mongodb.serverStatus.connections.totalCreated");
    static constexpr MetricName kConnectionsRejected =
        MetricNameMaker::make("mongodb.serverStatus.connections.rejected");
    static constexpr MetricName kConnectionsActive =
        MetricNameMaker::make("mongodb.serverStatus.connections.active");
    static constexpr MetricName kConnectionsBackpressureVersionsCurrent =
        MetricNameMaker::make("mongodb.serverStatus.connections.backpressureVersions.current");
    static constexpr MetricName kConnectionsBackpressureVersionsTotal =
        MetricNameMaker::make("mongodb.serverStatus.connections.backpressureVersions.total");
    static constexpr MetricName kMongoDBBuildInfo = MetricNameMaker::make("mongodb.build.info");

    // WiredTiger metrics
    static constexpr MetricName kEvictionCallsToGetAPageFoundQueueEmpty = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.cache.eviction_calls_to_get_a_page_found_queue_empty");
    static constexpr MetricName kEvictionEmptyScore =
        MetricNameMaker::make("mongodb.serverStatus.wiredTiger.cache.eviction_empty_score");
    static constexpr MetricName kEvictPageAttemptsByEvictionWorkerThreads = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.cache.evict_page_attempts_by_eviction_worker_threads");
    static constexpr MetricName kEvictPageFailuresByEvictionWorkerThreads = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.cache.evict_page_failures_by_eviction_worker_threads");
    static constexpr MetricName kEvictionWorkerThreadActive = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.cache.eviction_worker_thread_active");
    static constexpr MetricName kEvictionWorkerThreadStableNumber = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.cache.eviction_worker_thread_stable_number");
    static constexpr MetricName kPageEvictAttemptsByApplicationThreads = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.cache.page_evict_attempts_by_application_threads");
    static constexpr MetricName kPageEvictFailuresByApplicationThreads = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.cache.page_evict_failures_by_application_threads");
    static constexpr MetricName kConnectionDataHandlesCurrentlyActive = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.data-handle.connection_data_handles_currently_active");
    static constexpr MetricName kTransactionCheckpointMostRecentTime = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.transaction.transaction_checkpoint_most_recent_time_"
        "msecs");
    static constexpr MetricName kConcurrentTransactionsReadAvailable = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.concurrentTransactions.read.available");
    static constexpr MetricName kConcurrentTransactionsWriteAvailable = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.concurrentTransactions.write.available");
    static constexpr MetricName kBytesReadIntoCache =
        MetricNameMaker::make("mongodb.serverStatus.wiredTiger.cache.bytes_read_into_cache");
    static constexpr MetricName kBytesWrittenFromCache =
        MetricNameMaker::make("mongodb.serverStatus.wiredTiger.cache.bytes_written_from_cache");
    static constexpr MetricName kBytesCurrentlyInTheCache =
        MetricNameMaker::make("mongodb.serverStatus.wiredTiger.cache.bytes_currently_in_the_cache");
    static constexpr MetricName kTrackedDirtyBytesInTheCache = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.cache.tracked_dirty_bytes_in_the_cache");
    static constexpr MetricName kMaximumBytesConfigured =
        MetricNameMaker::make("mongodb.serverStatus.wiredTiger.cache.maximum_bytes_configured");
    static constexpr MetricName kPagesReadIntoCache =
        MetricNameMaker::make("mongodb.serverStatus.wiredTiger.cache.pages_read_into_cache");
    static constexpr MetricName kPagesRequestedFromTheCache = MetricNameMaker::make(
        "mongodb.serverStatus.wiredTiger.cache.pages_requested_from_the_cache");
    // Keeping these scoped to serverStatus for consistency with every other metric avoids
    // confusion.
    static constexpr MetricName kWiredTigerCollectErrors =
        MetricNameMaker::make("mongodb.serverStatus.wiredTiger.collectErrors");
    static constexpr MetricName kWiredTigerEngineNotReadyErrors =
        MetricNameMaker::make("mongodb.serverStatus.wiredTiger.engineNotReadyErrors");

    // Ingress Request Rate Limiter (Admission Control) Metrics. These mirror the fields that
    // RateLimiter::appendStats() historically reported under
    static constexpr MetricName kIngressRequestRateLimiterAttemptedAdmissions =
        MetricNameMaker::make(
            "mongodb.serverStatus.network.ingressRequestRateLimiter.attemptedAdmissions");
    static constexpr MetricName kIngressRequestRateLimiterSuccessfulAdmissions =
        MetricNameMaker::make(
            "mongodb.serverStatus.network.ingressRequestRateLimiter.successfulAdmissions");
    static constexpr MetricName kIngressRequestRateLimiterRejectedAdmissions =
        MetricNameMaker::make(
            "mongodb.serverStatus.network.ingressRequestRateLimiter.rejectedAdmissions");
    static constexpr MetricName kIngressRequestRateLimiterExemptedAdmissions =
        MetricNameMaker::make(
            "mongodb.serverStatus.network.ingressRequestRateLimiter.exemptedAdmissions");
    static constexpr MetricName kIngressRequestRateLimiterAddedToQueue = MetricNameMaker::make(
        "mongodb.serverStatus.network.ingressRequestRateLimiter.addedToQueue");
    static constexpr MetricName kIngressRequestRateLimiterRemovedFromQueue = MetricNameMaker::make(
        "mongodb.serverStatus.network.ingressRequestRateLimiter.removedFromQueue");
    static constexpr MetricName kIngressRequestRateLimiterInterruptedInQueue =
        MetricNameMaker::make(
            "mongodb.serverStatus.network.ingressRequestRateLimiter.interruptedInQueue");
    static constexpr MetricName kIngressRequestRateLimiterTokensAcquired = MetricNameMaker::make(
        "mongodb.serverStatus.network.ingressRequestRateLimiter.tokensAcquired");
    static constexpr MetricName kIngressRequestRateLimiterCurrentQueueDepth = MetricNameMaker::make(
        "mongodb.serverStatus.network.ingressRequestRateLimiter.currentQueueDepth");
    static constexpr MetricName kIngressRequestRateLimiterTotalAvailableTokens =
        MetricNameMaker::make(
            "mongodb.serverStatus.network.ingressRequestRateLimiter.totalAvailableTokens");
    static constexpr MetricName kIngressRequestRateLimiterAverageTimeQueuedMicros =
        MetricNameMaker::make(
            "mongodb.serverStatus.network.ingressRequestRateLimiter.averageTimeQueuedMicros");
    static constexpr MetricName kIngressRequestRateLimiterTimeQueuedMicros = MetricNameMaker::make(
        "mongodb.serverStatus.network.ingressRequestRateLimiter.timeQueuedMicros");

    // OpenTelemetry Tracing Sampler Metrics
    static constexpr MetricName kOtelTracingSamplerInternalSpanRateLimiterSuccessfulAdmissions =
        MetricNameMaker::make(
            "serverStatus.otelTracingSampler.internalSpans.rateLimiter.successfulAdmissions");
    static constexpr MetricName kOtelTracingSamplerInternalSpanRateLimiterRejectedAdmissions =
        MetricNameMaker::make(
            "serverStatus.otelTracingSampler.internalSpans.rateLimiter.rejectedAdmissions");
    static constexpr MetricName kOtelTracingSamplerExternalSpanRateLimiterSuccessfulAdmissions =
        MetricNameMaker::make(
            "serverStatus.otelTracingSampler.externalSpan.rateLimiter.successfulAdmissions");
    static constexpr MetricName kOtelTracingSamplerExternalSpanRateLimiterRejectedAdmissions =
        MetricNameMaker::make(
            "serverStatus.otelTracingSampler.externalSpan.rateLimiter.rejectedAdmissions");

    // Query Execution Team Metrics
    static constexpr MetricName kChangeStreamCursorsTotalOpened =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.totalOpened");
    static constexpr MetricName kChangeStreamCursorsLifespan =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.lifespan");
    static constexpr MetricName kChangeStreamCursorsOpenTotal =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.open.total");
    static constexpr MetricName kChangeStreamCursorsOpenPinned =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.open.pinned");
    static constexpr MetricName kChangeStreamCursorsOpenOptimeMin =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.open.optime.min");
    static constexpr MetricName kChangeStreamCursorsOpenOptimeMax =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.open.optime.max");
    static constexpr MetricName kChangeStreamBatchExecMicrosSum =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.batch.execMicros.sum");
    static constexpr MetricName kChangeStreamBatchShardLatencyMicrosSum = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.batch.shardLatencyMicros.sum");
    static constexpr MetricName kChangeStreamBatchConfigLatencyMicrosSum = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.batch.configLatencyMicros.sum");
    // Per-(consumer x engine) single-document lookup metrics for change stream updateLookup.
    static constexpr MetricName kChangeStreamUpdateLookupExpressFound = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.updateLookup.express.found");
    static constexpr MetricName kChangeStreamUpdateLookupExpressNotFound = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.updateLookup.express.notFound");
    static constexpr MetricName kChangeStreamUpdateLookupExpressNotHandled = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.updateLookup.express.notHandled");
    static constexpr MetricName kChangeStreamUpdateLookupExpressLatency = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.updateLookup.express.latencyMicros");
    static constexpr MetricName kChangeStreamUpdateLookupAggregationFound = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.updateLookup.aggregation.found");
    static constexpr MetricName kChangeStreamUpdateLookupAggregationNotFound =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.changeStreams.updateLookup.aggregation.notFound");
    static constexpr MetricName kChangeStreamUpdateLookupAggregationNotHandled =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.changeStreams.updateLookup.aggregation.notHandled");
    static constexpr MetricName kChangeStreamUpdateLookupAggregationLatency = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.updateLookup.aggregation.latencyMicros");
    static constexpr MetricName kChangeStreamUpdateLookupSbeFound =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.updateLookup.sbe.found");
    static constexpr MetricName kChangeStreamUpdateLookupSbeNotFound = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.updateLookup.sbe.notFound");
    static constexpr MetricName kChangeStreamUpdateLookupSbeNotHandled = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.updateLookup.sbe.notHandled");
    static constexpr MetricName kChangeStreamUpdateLookupSbeLatency = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.updateLookup.sbe.latencyMicros");
    static constexpr MetricName kChangeStreamErrorNonRetriableHistoryLost = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.error.nonRetriable.changeStreamHistoryLost");
    static constexpr MetricName kChangeStreamErrorNonRetriableFatalError = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.error.nonRetriable.changeStreamFatalError");
    static constexpr MetricName kChangeStreamErrorNonRetriableBsonObjectTooLarge =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.changeStreams.error.nonRetriable.bsonObjectTooLarge");
    static constexpr MetricName kChangeStreamErrorNonRetriableOther = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.error.nonRetriable.other");
    static constexpr MetricName kChangeStreamErrorRetriableInterruptedDueToReplStateChange =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.changeStreams.error.retriable."
            "interruptedDueToReplStateChange");
    static constexpr MetricName kChangeStreamErrorRetriableOther =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.error.retriable.other");
    static constexpr MetricName kChangeStreamOptionShowExpandedEvents = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.option.showExpandedEvents");
    static constexpr MetricName kChangeStreamOptionShowMigrationEvents = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.option.showMigrationEvents");
    static constexpr MetricName kChangeStreamOptionShowSystemEvents =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.option.showSystemEvents");
    static constexpr MetricName kChangeStreamOptionShowRawUpdateDescription = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.option.showRawUpdateDescription");
    static constexpr MetricName kChangeStreamOptionIgnoreRemovedShards = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.option.ignoreRemovedShards");
    static constexpr MetricName kChangeStreamOptionMatchCollectionUUIDForUpdateLookup =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.changeStreams.option.matchCollectionUUIDForUpdateLookup");
    static constexpr MetricName kChangeStreamOptionStartAfter =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.option.startAfter");
    static constexpr MetricName kChangeStreamOptionResumeAfter =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.option.resumeAfter");
    static constexpr MetricName kChangeStreamOptionStartAtOperationTime = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.option.startAtOperationTime");
    static constexpr MetricName kChangeStreamOptionFullDocumentRequired = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.option.fullDocument.required");
    static constexpr MetricName kChangeStreamOptionFullDocumentUpdateLookup = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.changeStreams.option.fullDocument.updateLookup");
    static constexpr MetricName kChangeStreamOptionFullDocumentWhenAvailable =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.changeStreams.option.fullDocument.whenAvailable");
    static constexpr MetricName kChangeStreamOptionFullDocumentBeforeChangeRequired =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.changeStreams.option.fullDocumentBeforeChange.required");
    static constexpr MetricName kChangeStreamOptionFullDocumentBeforeChangeWhenAvailable =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.changeStreams.option.fullDocumentBeforeChange."
            "whenAvailable");
    static constexpr MetricName kChangeStreamScopeCluster =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.scope.cluster");
    static constexpr MetricName kChangeStreamScopeDb =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.scope.db");
    static constexpr MetricName kChangeStreamScopeCollection =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.scope.collection");
    static constexpr MetricName kChangeStreamOptionCursorBatchSize =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.option.cursor.batchSize");
    static constexpr MetricName kChangeStreamOptionCursorMaxTimeMS =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.option.cursor.maxTimeMS");
    static constexpr MetricName kChangeStreamCursorDocsReturned =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.docsReturned");
    static constexpr MetricName kChangeStreamCursorBytesReturned =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.bytesReturned");
    static constexpr MetricName kChangeStreamCursorBatchesReturned =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.batchesReturned");
    static constexpr MetricName kChangeStreamCursorDocsExamined =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.docsExamined");
    static constexpr MetricName kChangeStreamCursorBytesRead =
        MetricNameMaker::make("mongodb.serverStatus.metrics.changeStreams.cursor.bytesRead");

    // Storage Execution Team Metrics
    static constexpr MetricName kIndexBuildsActive =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.active");
    static constexpr MetricName kIndexBuildsStarted =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.started");
    static constexpr MetricName kIndexBuildsSucceeded =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.succeeded");
    static constexpr MetricName kIndexBuildsFailed =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.failed");
    static constexpr MetricName kIndexBuildsToBeResumed =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.to_be_resumed");
    static constexpr MetricName kIndexBuildSideWritesInserted =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.sideWrites.inserted");
    static constexpr MetricName kIndexBuildSideWritesDeleted =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.sideWrites.deleted");
    static constexpr MetricName kIndexBuildSideWritesDrained =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.sideWrites.drained");
    static constexpr MetricName kIndexBuildSideWritesDrainDuration =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.sideWrites.drainDuration");
    static constexpr MetricName kIndexBuildSideWritesDrainBytes =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.sideWrites.drainBytes");
    static constexpr MetricName kIndexBuildSideWritesDrainYields =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.sideWrites.drainYields");
    static constexpr MetricName kReplicatedFastCountIsRunning =
        MetricNameMaker::make("mongodb.serverStatus.metrics.replicatedFastCount.isRunning");
    static constexpr MetricName kReplicatedFastCountFlushSuccessCount = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.replicatedFastCount.flush.successCount");
    static constexpr MetricName kReplicatedFastCountFlushFailureCount = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.replicatedFastCount.flush.failureCount");
    static constexpr MetricName kReplicatedFastCountFlushTimeMsMin =
        MetricNameMaker::make("mongodb.serverStatus.metrics.replicatedFastCount.flushTime.min");
    static constexpr MetricName kReplicatedFastCountFlushTimeMsMax =
        MetricNameMaker::make("mongodb.serverStatus.metrics.replicatedFastCount.flushTime.max");
    static constexpr MetricName kReplicatedFastCountFlushTimeMsTotal =
        MetricNameMaker::make("mongodb.serverStatus.metrics.replicatedFastCount.flushTime.total");
    static constexpr MetricName kReplicatedFastCountFlushedDocsMin =
        MetricNameMaker::make("mongodb.serverStatus.metrics.replicatedFastCount.flushed_docs.min");
    static constexpr MetricName kReplicatedFastCountFlushedDocsMax =
        MetricNameMaker::make("mongodb.serverStatus.metrics.replicatedFastCount.flushed_docs.max");
    static constexpr MetricName kReplicatedFastCountFlushedDocsTotal = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.replicatedFastCount.flushed_docs.total");
    static constexpr MetricName kReplicatedFastCountInsertCount =
        MetricNameMaker::make("mongodb.serverStatus.metrics.replicatedFastCount.insertCount");
    static constexpr MetricName kReplicatedFastCountUpdateCount =
        MetricNameMaker::make("mongodb.serverStatus.metrics.replicatedFastCount.updateCount");
    static constexpr MetricName kReplicatedFastCountCheckpointOplogEntriesProcessed =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.replicatedFastCount.checkpoint.oplogEntriesProcessed");
    static constexpr MetricName kReplicatedFastCountCheckpointOplogEntriesSkipped =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.replicatedFastCount.checkpoint.oplogEntriesSkipped");
    static constexpr MetricName kReplicatedFastCountCheckpointSizeCountEntriesProcessed =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.replicatedFastCount.checkpoint."
            "sizeCountEntriesProcessed");
    static constexpr MetricName kReplicatedFastCountOplogLagSecs =
        MetricNameMaker::make("mongodb.serverStatus.metrics.replicatedFastCount.oplogLagSecs");

    static constexpr MetricName kIndexBuildKeysInsertedFromScan =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.keysInsertedFromScan");
    static constexpr MetricName kIndexBuildDocsScanned =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.docsScanned");
    static constexpr MetricName kIndexBuildKeysGeneratedFromScan =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.keysGeneratedFromScan");
    static constexpr MetricName kIndexBuildResumeSucceeded =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.resume.succeeded");
    static constexpr MetricName kIndexBuildResumeFailed =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.resume.failed");

    static constexpr MetricName kTtlPasses =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.passes");
    static constexpr MetricName kTtlSubPasses =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.subPasses");
    static constexpr MetricName kTtlDuration =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.durationMicros");
    static constexpr MetricName kTtlDeletedDocuments =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.deletedDocuments");
    static constexpr MetricName kTtlDeletedKeys =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.deletedKeys");
    static constexpr MetricName kTtlExaminedDocuments =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.examinedDocuments");
    static constexpr MetricName kTtlExaminedKeys =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.examinedKeys");
    static constexpr MetricName kTtlInvalidTtlIndexSkips =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.invalidTTLIndexSkips");
    static constexpr MetricName kTtlTimeQueuedForTickets =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.timeQueuedForTicketsMicros");
    static constexpr MetricName kTtlTimeProcessingWithTickets =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.timeProcessingWithTicketsMicros");
    static constexpr MetricName kTtlTicketAdmissions =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.ticketAdmissions");
    static constexpr MetricName kTtlLowPriorityTicketAdmissions =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.lowPriorityTicketAdmissions");
    static constexpr MetricName kTtlQueuedForTickets =
        MetricNameMaker::make("mongodb.serverStatus.metrics.ttl.queuedForTickets");
    static constexpr MetricName kIndexCount =
        MetricNameMaker::make("mongodb.serverStatus.indexStats.count");
    static constexpr MetricName kIndexStatsMultikeyNewPathsOrdinaryInTransaction =
        MetricNameMaker::make(
            "mongodb.serverStatus.indexStats.multikey.newPaths.ordinary.inTransaction");
    static constexpr MetricName kIndexStatsMultikeyNewPathsOrdinaryOutsideTransaction =
        MetricNameMaker::make(
            "mongodb.serverStatus.indexStats.multikey.newPaths.ordinary.outsideTransaction");
    static constexpr MetricName kIndexStatsMultikeyNewPathsWildcardInTransaction =
        MetricNameMaker::make(
            "mongodb.serverStatus.indexStats.multikey.newPaths.wildcard.inTransaction");
    static constexpr MetricName kIndexStatsMultikeyNewPathsWildcardOutsideTransaction =
        MetricNameMaker::make(
            "mongodb.serverStatus.indexStats.multikey.newPaths.wildcard.outsideTransaction");
    static constexpr MetricName kIndexStatsMultikeySideTransactions =
        MetricNameMaker::make("mongodb.serverStatus.indexStats.multikey.sideTransactions");

    static constexpr MetricName kIndexBuildsTotal =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.total");
    static constexpr MetricName kIndexBuildPhasesCommit =
        MetricNameMaker::make("mongodb.serverStatus.indexBuilds.phases.commit");

    static constexpr MetricName kIndexBulkBuilderNumSorted =
        MetricNameMaker::make("mongodb.serverStatus.indexBulkBuilder.numSorted");
    static constexpr MetricName kIndexBulkBuilderBytesSorted =
        MetricNameMaker::make("mongodb.serverStatus.indexBulkBuilder.bytesSorted");
    static constexpr MetricName kIndexBulkBuilderBytesSpilled =
        MetricNameMaker::make("mongodb.serverStatus.indexBulkBuilder.bytesSpilled");
    static constexpr MetricName kIndexBulkBuilderBytesSpilledUncompressed =
        MetricNameMaker::make("mongodb.serverStatus.indexBulkBuilder.bytesSpilledUncompressed");
    static constexpr MetricName kIndexBulkBuilderMemUsage =
        MetricNameMaker::make("mongodb.serverStatus.indexBulkBuilder.memUsage");
    static constexpr MetricName kIndexBulkBuilderSpilledRanges =
        MetricNameMaker::make("mongodb.serverStatus.indexBulkBuilder.spilledRanges");

    // Replication Team Metrics
    static constexpr MetricName kOplogApplyBytes =
        MetricNameMaker::make("mongodb.oplog.apply.bytes");
    static constexpr MetricName kOplogApplyBufferCount =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.buffer.apply.count");
    static constexpr MetricName kOplogApplyBufferSize =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.buffer.apply.sizeBytes");
    static constexpr MetricName kOplogApplyBufferMaxSize =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.buffer.apply.maxSizeBytes");
    static constexpr MetricName kOplogApplyBufferMaxCount =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.buffer.apply.maxCount");
    static constexpr MetricName kOplogWriteBufferCount =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.buffer.write.count");
    static constexpr MetricName kOplogWriteBufferSize =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.buffer.write.sizeBytes");
    static constexpr MetricName kOplogWriteBufferMaxSize =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.buffer.write.maxSizeBytes");
    static constexpr MetricName kApplyBatchesNum =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.apply.batches.num");
    static constexpr MetricName kApplyBatchesTotalMillis =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.apply.batches.totalMillis");
    static constexpr MetricName kInitialSyncFailedAttempts =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.initialSync.failedAttempts");
    static constexpr MetricName kInitialSyncFailures =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.initialSync.failures");
    static constexpr MetricName kInitialSyncCompleted =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.initialSync.completed");
    static constexpr MetricName kReplNetworkBytes =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.network.bytes");
    static constexpr MetricName kReplNetworkBytesSent =
        MetricNameMaker::make("mongodb.serverStatus.metrics.repl.network.bytesSent");
    static constexpr MetricName kGetLastErrorWtimeNum =
        MetricNameMaker::make("mongodb.serverStatus.metrics.getLastError.wtime.num");
    static constexpr MetricName kGetLastErrorWtimeTotalMillis =
        MetricNameMaker::make("mongodb.serverStatus.metrics.getLastError.wtime.totalMillis");
    static constexpr MetricName kGetLastErrorWtimeouts =
        MetricNameMaker::make("mongodb.serverStatus.metrics.getLastError.wtimeouts");
    static constexpr MetricName kGetLastErrorDefaultWtimeouts =
        MetricNameMaker::make("mongodb.serverStatus.metrics.getLastError.default.wtimeouts");
    static constexpr MetricName kGetLastErrorDefaultUnsatisfiable =
        MetricNameMaker::make("mongodb.serverStatus.metrics.getLastError.default.unsatisfiable");

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
        MetricNameMaker::make("mongodb.serverStatus.globalLock.totalTime");
    static constexpr MetricName kGlobalLockCurrentQueueTotal =
        MetricNameMaker::make("mongodb.serverStatus.globalLock.currentQueue.total");
    static constexpr MetricName kGlobalLockCurrentQueueReaders =
        MetricNameMaker::make("mongodb.serverStatus.globalLock.currentQueue.readers");
    static constexpr MetricName kGlobalLockCurrentQueueWriters =
        MetricNameMaker::make("mongodb.serverStatus.globalLock.currentQueue.writers");
    static constexpr MetricName kGlobalLockActiveClientsTotal =
        MetricNameMaker::make("mongodb.serverStatus.globalLock.activeClients.total");
    static constexpr MetricName kGlobalLockActiveClientsReaders =
        MetricNameMaker::make("mongodb.serverStatus.globalLock.activeClients.readers");
    static constexpr MetricName kGlobalLockActiveClientsWriters =
        MetricNameMaker::make("mongodb.serverStatus.globalLock.activeClients.writers");

    static constexpr MetricName kOperationLatency =
        MetricNameMaker::make("mongodb.serverStatus.opLatencies.latency");

    // Op Counters
    static constexpr MetricName kInsertOpCount =
        MetricNameMaker::make("mongodb.serverStatus.opcounters.insert");
    static constexpr MetricName kQueryOpCount =
        MetricNameMaker::make("mongodb.serverStatus.opcounters.query");
    static constexpr MetricName kUpdateOpCount =
        MetricNameMaker::make("mongodb.serverStatus.opcounters.update");
    static constexpr MetricName kDeleteOpCount =
        MetricNameMaker::make("mongodb.serverStatus.opcounters.delete");
    static constexpr MetricName kGetMoreOpCount =
        MetricNameMaker::make("mongodb.serverStatus.opcounters.getmore");
    static constexpr MetricName kCommandOpCount =
        MetricNameMaker::make("mongodb.serverStatus.opcounters.command");
    // New in SERVER-123987 - Counts every top-level 'aggregate' command.
    static constexpr MetricName kAggregateOpCount =
        MetricNameMaker::make("mongodb.serverStatus.opcounters.aggregate");

    // Asserts - Counts every assertion failure broken down by `kind` attribute (one
    // of: "regular", "msg", "user", "tripwire"). Mirrors the per-type counters under
    // `serverStatus.asserts.*`; `warning` is omitted because nothing increments it, and
    // `rollovers` is omitted because the OTel counter is int64 and never wraps.
    // TODO (follow-up SERVER ticket): add a `command` attribute so failures can be sliced by
    // command name (the "(c)" approach from the design discussion).
    static constexpr MetricName kAsserts = MetricNameMaker::make("mongodb.serverStatus.asserts");

    // Query Performance Counters
    static constexpr MetricName kQueryExecutorScanned =
        MetricNameMaker::make("mongodb.serverStatus.metrics.queryExecutor.scanned");
    static constexpr MetricName kQueryExecutorScannedObjects =
        MetricNameMaker::make("mongodb.serverStatus.metrics.queryExecutor.scannedObjects");
    static constexpr MetricName kDocumentReturned =
        MetricNameMaker::make("mongodb.serverStatus.metrics.document.returned");

    // Memory tracking — system-wide high-water mark of the largest amount of memory (in bytes) ever
    // tracked for a single operation since process startup. Mirrors the serverStatus value at
    // metrics.query.peakMemoryUsageOperation.
    static constexpr MetricName kQueryPeakMemoryUsageOperation =
        MetricNameMaker::make("serverStatus.metrics.query.peakMemoryUsageOperation");

    // Memory tracking — current configured value of the
    // internalQueryMaxMemoryUsageBytesPerOperation server parameter, i.e. the operation-wide cap
    // (in bytes) on memory-tracked query stages. Mirrors the serverStatus value at
    // metrics.query.configuredMaxMemoryUsageBytesPerOperation.
    static constexpr MetricName kQueryConfiguredMaxMemoryUsageBytesPerOperation =
        MetricNameMaker::make(
            "serverStatus.metrics.query.configuredMaxMemoryUsageBytesPerOperation");

    // Memory tracking — number of times a query operation was failed (via an ExceededMemoryLimit
    // error) because it exceeded a memory-tracking limit. Mirrors the serverStatus value at
    // metrics.query.operationsFailedDueToMemoryLimit.
    static constexpr MetricName kQueryOperationsFailedDueToMemoryLimit =
        MetricNameMaker::make("serverStatus.metrics.query.operationsFailedDueToMemoryLimit");

    // Plan cache counters — classic engine
    static constexpr MetricName kPlanCacheClassicHits =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.planCache.classic.hits");
    static constexpr MetricName kPlanCacheClassicMisses =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.planCache.classic.misses");
    static constexpr MetricName kPlanCacheClassicSkipped =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.planCache.classic.skipped");
    static constexpr MetricName kPlanCacheClassicReplanned =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.planCache.classic.replanned");
    static constexpr MetricName kPlanCacheClassicReplannedPlanIsCachedPlan = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.query.planCache.classic.replanned_plan_is_cached_plan");
    static constexpr MetricName kPlanCacheClassicCachedPlansEvicted = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.query.planCache.classic.cached_plans_evicted");
    static constexpr MetricName kPlanCacheClassicInactiveCachedPlansReplaced =
        MetricNameMaker::make(
            "mongodb.serverStatus.metrics.query.planCache.classic.inactive_cached_plans_replaced");

    // Plan cache counters — SBE engine
    static constexpr MetricName kPlanCacheSbeHits =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.planCache.sbe.hits");
    static constexpr MetricName kPlanCacheSbeMisses =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.planCache.sbe.misses");
    static constexpr MetricName kPlanCacheSbeSkipped =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.planCache.sbe.skipped");
    static constexpr MetricName kPlanCacheSbeReplanned =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.planCache.sbe.replanned");
    static constexpr MetricName kPlanCacheSbeReplannedPlanIsCachedPlan = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.query.planCache.sbe.replanned_plan_is_cached_plan");
    static constexpr MetricName kPlanCacheSbeCachedPlansEvicted = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.query.planCache.sbe.cached_plans_evicted");
    static constexpr MetricName kPlanCacheSbeInactiveCachedPlansReplaced = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.query.planCache.sbe.inactive_cached_plans_replaced");

    // Query framework engine-mix counters
    static constexpr MetricName kQueryFrameworkFindSbe =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.queryFramework.find.sbe");
    static constexpr MetricName kQueryFrameworkFindClassic =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.queryFramework.find.classic");
    static constexpr MetricName kQueryFrameworkAggregateSbeOnly = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.query.queryFramework.aggregate.sbeOnly");
    static constexpr MetricName kQueryFrameworkAggregateClassicOnly = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.query.queryFramework.aggregate.classicOnly");
    static constexpr MetricName kQueryFrameworkAggregateSbeHybrid = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.query.queryFramework.aggregate.sbeHybrid");
    static constexpr MetricName kQueryFrameworkAggregateClassicHybrid = MetricNameMaker::make(
        "mongodb.serverStatus.metrics.query.queryFramework.aggregate.classicHybrid");

    // Fast-path planning counters
    static constexpr MetricName kFastPathIdHack =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.planning.fastPath.idHack");
    static constexpr MetricName kFastPathExpress =
        MetricNameMaker::make("mongodb.serverStatus.metrics.query.planning.fastPath.express");

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

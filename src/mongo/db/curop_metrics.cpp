// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/curop_metrics.h"

#include "mongo/db/admission/ticketing/ticketholder_parameters_gen.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/counters_sort.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/platform/atomic.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {

// Per-opCtx decoration tracking non-ticketed execution intervals for aggregation pipelines.
static const OperationContext::Decoration<AggNonTicketedIntervalTracker> aggNonTicketedIntervalDec =
    OperationContext::declareDecoration<AggNonTicketedIntervalTracker>();

AggNonTicketedIntervalTracker& getAggNonTicketedIntervalTracker(OperationContext* opCtx) {
    return aggNonTicketedIntervalDec(opCtx);
}

int64_t aggNonTicketedIntervalThresholdMillis() {
    return gDelinquentAcquisitionIntervalMillis.load();
}

void closeAggNonTicketedIntervalIfOpen(AggNonTicketedIntervalTracker& tracker,
                                       OperationContext* opCtx) {
    if (!tracker.hasIntervalStart)
        return;
    auto& ts = opCtx->tickSource();
    tracker.closeInterval(
        ts.ticksTo<Milliseconds>(ts.getTicks() - tracker.intervalStartTick).count(),
        aggNonTicketedIntervalThresholdMillis());
}

namespace {

/** Build a `Counter64` metric with the given `name` and `role`. */
Counter64* makeCounter(std::string name, ClusterRole role) {
    return &*MetricBuilder<Counter64>(std::move(name)).setRole(role);
}

/** If `in` is nonzero, increment `stat` by it. */
template <typename T>
void incrCounter(Counter64* stat, const T& in) {
    if (in)
        stat->incrementRelaxed(in);
}

/** If `in` is an atomic, load it and increment by that value. */
template <typename T>
void incrCounter(Counter64* stat, const Atomic<T>& in) {
    incrCounter(stat, in.load());
}

/** If `in` is an engaged optional, increment by its dereferenced value. */
template <typename T>
void incrCounter(Counter64* stat, const boost::optional<T>& in) {
    if (in)
        incrCounter(stat, *in);
}

/**
 * If `in` is an engaged optional with a positive value, increment the OTel counter. OTel Counter is
 * monotonic; negative `*in` would violate the contract. In practice these inputs are always
 * non-negative.
 */
template <typename T>
void incrOtelCounter(otel::metrics::Counter<int64_t>* counter, const boost::optional<T>& in) {
    if (in && *in > 0)
        counter->add(static_cast<int64_t>(*in));
}

/** Counters that are in both shard and router. */
struct InBoth {
    explicit InBoth(ClusterRole role)
        : killedDueToClientDisconnect{makeCounter("operation.killedDueToClientDisconnect", role)},
          killedDueToMaxTimeMSExpired{makeCounter("operation.killedDueToMaxTimeMSExpired", role)},
          killedDueToDefaultMaxTimeMSExpired{
              makeCounter("operation.killedDueToDefaultMaxTimeMSExpired", role)},
          killedDueToInterruptedDueToOverload{
              makeCounter("operation.killedDueToInterruptedDueToOverload", role)} {}

    void record(OperationContext* opCtx) {
        auto* curOp = CurOp::get(opCtx);
        auto& debug = curOp->debug();
        auto killStatus = opCtx->getKillStatus();
        if (killStatus == ErrorCodes::ClientDisconnect) {
            killedDueToClientDisconnect->increment();
        }
        if (killStatus == ErrorCodes::MaxTimeMSExpired ||
            debug.errInfo == ErrorCodes::MaxTimeMSExpired) {
            if (opCtx->usesDefaultMaxTimeMS()) {
                killedDueToDefaultMaxTimeMSExpired->increment();
            } else {
                killedDueToMaxTimeMSExpired->increment();
            }
        }
        if (killStatus == ErrorCodes::InterruptedDueToOverload ||
            debug.errInfo == ErrorCodes::InterruptedDueToOverload) {
            killedDueToInterruptedDueToOverload->increment();
        }
    }

    Counter64* killedDueToClientDisconnect;
    Counter64* killedDueToMaxTimeMSExpired;
    Counter64* killedDueToDefaultMaxTimeMSExpired;
    Counter64* killedDueToInterruptedDueToOverload;
};

/** Counters that are in shard service. */
struct InShard : InBoth {
    static constexpr auto role = ClusterRole::ShardServer;

    InShard() : InBoth{role} {}

    void recordWriteConflicts(OperationContext* opCtx) {
        auto* curOp = CurOp::get(opCtx);
        const auto& sm = curOp->getOperationStorageMetrics();
        incrCounter(writeConflicts, sm.writeConflicts);
    }

    void record(OperationContext* opCtx) {
        InBoth::record(opCtx);
        auto* curOp = CurOp::get(opCtx);
        auto& debug = curOp->debug();
        auto& am = debug.getAdditiveMetrics();
        incrCounter(deleted, am.ndeleted);
        incrCounter(inserted, am.ninserted);
        incrCounter(returned, am.nreturned);
        incrCounter(updated, am.nMatched);
        incrCounter(scanned, am.keysExamined);
        incrCounter(scannedObjects, am.docsExamined);
        incrCounter(scanAndOrder, am.hasSortStage);

        auto& otelCounters = _otelCounters();
        incrOtelCounter(otelCounters.scanned, am.keysExamined);
        incrOtelCounter(otelCounters.scannedObjects, am.docsExamined);
        incrOtelCounter(otelCounters.returned, am.nreturned);

        // Increment oplog metrics if the current request is a change stream or replication request.
        if (debug.isChangeStreamQuery || debug.isReplOplogGetMore) {
            incrCounter(oplogReturned, am.nreturned);
            incrCounter(oplogScannedObjects, am.docsExamined);
        }
        // Write Conflicts is recorded at Operation level, not individual CurOp stash,
        // so we only increment the counters if we are the top level.
        if (curOp->isTop()) {
            recordWriteConflicts(opCtx);
        }

        _updateExternalStats(opCtx);
        _flushAggNonTicketedStats(opCtx);
    }

private:
    /** Close any in-progress non-ticketed interval and flush to global counters. */
    void _flushAggNonTicketedStats(OperationContext* opCtx) {
        auto& tracker = aggNonTicketedIntervalDec(opCtx);
        closeAggNonTicketedIntervalIfOpen(tracker, opCtx);
        if (tracker.hadLongInterval) {
            aggNonTicketedIntervals->incrementRelaxed(tracker.longIntervalCount);
            aggNonTicketedTotalMillis->incrementRelaxed(tracker.longIntervalTotalMs);
            aggNonTicketedMaxMillis->setToMax(tracker.longIntervalMaxMs);
            aggNonTicketedQueries->increment();
            tracker.hadLongInterval = false;  // prevent double-counting if flushed again
        }
    }

    /** A few nonmember variables also need to be updated. */
    static void _updateExternalStats(const OperationContext* opCtx) {
        auto* curOp = CurOp::get(opCtx);
        auto& debug = curOp->debug();
        lookupPushdownCounters.incrementLookupCountersPerQuery(debug.lookupNestedLoopJoin,
                                                               debug.lookupIndexedLoopJoin,
                                                               debug.lookupHashLookup,
                                                               debug.lookupDynamicIndexedLoopJoin);
        lookupUnwindPushdownCounters.incrementLookupUnwindCountersPerQuery(
            debug.luIndexedLoopJoin,
            debug.luNestedLoopJoin,
            debug.luHashLookup,
            debug.luDynamicIndexedLoopJoin,
            debug.luLocalCollscan,
            debug.luLocalIxscanFetch,
            debug.luLocalComplex);
        sortCounters.incrementSortCountersPerQuery(debug.sortTotalDataSizeBytes, debug.keysSorted);
        queryFrameworkCounters.incrementQueryEngineCounters(curOp);
        nonLeadingPushdownCounters.incrementCounters(
            debug.nlpMatch, debug.nlpProject, debug.nlpAddFields, debug.nlpReplaceRoot);
        pathArraynessCounters.incrementPerQuery(debug.pathArraynessLeadingFilter,
                                                debug.pathArraynessSimplified);
    }

public:
    Counter64* deleted{makeCounter("document.deleted", role)};
    Counter64* inserted{makeCounter("document.inserted", role)};
    Counter64* returned{makeCounter("document.returned", role)};
    Counter64* updated{makeCounter("document.updated", role)};
    Counter64* scanned{makeCounter("queryExecutor.scanned", role)};
    Counter64* scannedObjects{makeCounter("queryExecutor.scannedObjects", role)};
    Counter64* scanAndOrder{makeCounter("operation.scanAndOrder", role)};
    Counter64* writeConflicts{makeCounter("operation.writeConflicts", role)};
    Counter64* oplogReturned{makeCounter("oplogStats.document.returned", role)};
    Counter64* oplogScannedObjects{makeCounter("oplogStats.queryExecutor.scannedObjects", role)};
    // Aggregation pipeline non-ticketed execution interval metrics.
    // These track periods when an aggregate command releases its execution ticket to perform
    // in-memory work (e.g. $sort, $group) after the $cursor stage finishes reading. Intervals
    // longer than delinquentAcquisitionIntervalMillis are counted.
    //   nonTicketed.intervals  - total long intervals across all aggregations
    //   nonTicketed.totalMillis - cumulative ms in those intervals
    //   nonTicketed.maxMillis  - longest single interval ever seen
    //   nonTicketed.queries    - distinct aggregate commands with at least one long interval
    Counter64* aggNonTicketedIntervals{
        makeCounter("query.aggregation.nonTicketed.intervals", role)};
    Counter64* aggNonTicketedTotalMillis{
        makeCounter("query.aggregation.nonTicketed.totalMillis", role)};
    Atomic64Metric* aggNonTicketedMaxMillis{
        &*MetricBuilder<Atomic64Metric>("query.aggregation.nonTicketed.maxMillis").setRole(role)};
    Counter64* aggNonTicketedQueries{makeCounter("query.aggregation.nonTicketed.queries", role)};

private:
    struct OtelCounters {
        otel::metrics::Counter<int64_t>* scanned;
        otel::metrics::Counter<int64_t>* scannedObjects;
        otel::metrics::Counter<int64_t>* returned;
    };

    static OtelCounters& _otelCounters() {
        static OtelCounters counters = [] {
            auto& service = otel::metrics::MetricsService::instance();
            return OtelCounters{
                &service.createInt64Counter(otel::metrics::MetricNames::kQueryExecutorScanned,
                                            "Total index keys examined during query execution",
                                            otel::metrics::MetricUnit::kEvents),
                &service.createInt64Counter(
                    otel::metrics::MetricNames::kQueryExecutorScannedObjects,
                    "Total documents examined during query execution",
                    otel::metrics::MetricUnit::kEvents),
                &service.createInt64Counter(otel::metrics::MetricNames::kDocumentReturned,
                                            "Total documents returned to clients",
                                            otel::metrics::MetricUnit::kEvents)};
        }();
        return counters;
    }
};

/** Counters that are in the router service (currently none). */
struct InRouter : InBoth {
    InRouter() : InBoth{ClusterRole::RouterServer} {}
};

static InShard shardStats{};
static InRouter routerStats{};

}  // namespace

void recordCurOpMetrics(OperationContext* opCtx) {
    auto role = opCtx->getService()->role();
    if (role.hasExclusively(ClusterRole::ShardServer)) {
        shardStats.record(opCtx);
    } else if (role.hasExclusively(ClusterRole::RouterServer)) {
        routerStats.record(opCtx);
    } else {
        MONGO_UNREACHABLE;
    }
}

void recordCurOpMetricsOplogApplication(OperationContext* opCtx) {
    auto role = opCtx->getService()->role();
    if (role.hasExclusively(ClusterRole::ShardServer))
        shardStats.recordWriteConflicts(opCtx);
}

}  // namespace mongo

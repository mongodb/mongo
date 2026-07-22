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

#include "mongo/otel/metrics/instrumentation/wiredtiger_metrics.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/ticketing_system.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/metrics_updown_counter.h"
#include "mongo/util/periodic_runner.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {

using admission::execution_control::TicketingSystem;
using otel::metrics::Counter;
using otel::metrics::Gauge;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

struct WiredTigerOtelMetricsState {
    std::unique_ptr<WiredTigerMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getWiredTigerOtelMetricsState =
    ServiceContext::declareDecoration<WiredTigerOtelMetricsState>();

}  // namespace

class WiredTigerMetrics::Impl {
public:
    Impl()
        : _evictionCallsToGetPageFoundQueueEmpty(MetricsService::instance().createInt64Counter(
              MetricNames::kEvictionCallsToGetAPageFoundQueueEmpty,
              "Eviction server calls to get a page that found the queue empty",
              MetricUnit::kCount)),
          _evictPageAttemptsByWorkerThreads(MetricsService::instance().createInt64Counter(
              MetricNames::kEvictPageAttemptsByEvictionWorkerThreads,
              "Page evict attempts by eviction worker threads",
              MetricUnit::kCount)),
          _evictPageFailuresByWorkerThreads(MetricsService::instance().createInt64Counter(
              MetricNames::kEvictPageFailuresByEvictionWorkerThreads,
              "Page evict failures by eviction worker threads",
              MetricUnit::kCount)),
          _pageEvictAttemptsByAppThreads(MetricsService::instance().createInt64Counter(
              MetricNames::kPageEvictAttemptsByApplicationThreads,
              "Page evict attempts by application threads",
              MetricUnit::kCount)),
          _pageEvictFailuresByAppThreads(MetricsService::instance().createInt64Counter(
              MetricNames::kPageEvictFailuresByApplicationThreads,
              "Page evict failures by application threads",
              MetricUnit::kCount)),
          _bytesReadIntoCache(MetricsService::instance().createInt64Counter(
              MetricNames::kBytesReadIntoCache, "Bytes read into the cache", MetricUnit::kBytes)),
          _bytesWrittenFromCache(
              MetricsService::instance().createInt64Counter(MetricNames::kBytesWrittenFromCache,
                                                            "Bytes written from the cache",
                                                            MetricUnit::kBytes)),
          _pagesReadIntoCache(MetricsService::instance().createInt64Counter(
              MetricNames::kPagesReadIntoCache, "Pages read into the cache", MetricUnit::kCount)),
          _pagesRequestedFromCache(MetricsService::instance().createInt64Counter(
              MetricNames::kPagesRequestedFromTheCache,
              "Pages requested from the cache",
              MetricUnit::kCount)),
          _evictionEmptyScore(MetricsService::instance().createInt64Gauge(
              MetricNames::kEvictionEmptyScore, "Eviction empty score", MetricUnit::kCount)),
          _evictionWorkerThreadActive(MetricsService::instance().createInt64Gauge(
              MetricNames::kEvictionWorkerThreadActive,
              "Number of active eviction worker threads",
              MetricUnit::kCount)),
          _evictionWorkerThreadStableNumber(MetricsService::instance().createInt64Gauge(
              MetricNames::kEvictionWorkerThreadStableNumber,
              "Stable number of eviction worker threads",
              MetricUnit::kCount)),
          _bytesCurrentlyInCache(
              MetricsService::instance().createInt64Gauge(MetricNames::kBytesCurrentlyInTheCache,
                                                          "Bytes currently in the cache",
                                                          MetricUnit::kBytes)),
          _trackedDirtyBytesInCache(
              MetricsService::instance().createInt64Gauge(MetricNames::kTrackedDirtyBytesInTheCache,
                                                          "Tracked dirty bytes in the cache",
                                                          MetricUnit::kBytes)),
          _maximumBytesConfigured(
              MetricsService::instance().createInt64Gauge(MetricNames::kMaximumBytesConfigured,
                                                          "Maximum bytes configured for the cache",
                                                          MetricUnit::kBytes)),
          _connectionDataHandlesCurrentlyActive(MetricsService::instance().createInt64Gauge(
              MetricNames::kConnectionDataHandlesCurrentlyActive,
              "Connection data handles currently active",
              MetricUnit::kCount)),
          _transactionCheckpointMostRecentTimeMsecs(MetricsService::instance().createInt64Gauge(
              MetricNames::kTransactionCheckpointMostRecentTime,
              "Most recent transaction checkpoint time",
              MetricUnit::kMilliseconds)),
          _concurrentTransactionsReadAvailable(MetricsService::instance().createInt64Gauge(
              MetricNames::kConcurrentTransactionsReadAvailable,
              "Amount of concurrent read transactions available",
              MetricUnit::kCount)),
          _concurrentTransactionsWriteAvailable(MetricsService::instance().createInt64Gauge(
              MetricNames::kConcurrentTransactionsWriteAvailable,
              "Amount of concurrent write transactions available",
              MetricUnit::kCount)),
          _collectErrors(MetricsService::instance().createInt64Counter(
              MetricNames::kWiredTigerCollectErrors,
              "Number of times WiredTiger stats reading failed during collection",
              MetricUnit::kCount)),
          _engineNotReadyErrors(MetricsService::instance().createInt64Counter(
              MetricNames::kWiredTigerEngineNotReadyErrors,
              "Number of times the WiredTiger storage engine was not ready in time for collection",
              MetricUnit::kCount)),
          _ticketingSystemCollectErrors(MetricsService::instance().createInt64Counter(
              MetricNames::kTicketingSystemCollectErrors,
              "Number of times reading the ticketing system's stats failed during collection",
              MetricUnit::kCount)) {}

    void updateWiredTiger(const WiredTigerStatsSnapshot& snap) {
        // Add the difference since the previous snapshot to counter metrics.
        // Counters should never decrease, so negative deltas (e.g., resets) are ignored.
        auto addDelta = [](Counter<int64_t>& counter, int64_t& prev, int64_t current) {
            int64_t delta = std::max<int64_t>(current - std::exchange(prev, current), 0);
            if (delta > 0)
                counter.add(delta);
        };

        addDelta(_evictionCallsToGetPageFoundQueueEmpty,
                 _prev.evictionCallsToGetPageFoundQueueEmpty,
                 snap.evictionCallsToGetPageFoundQueueEmpty);
        addDelta(_evictPageAttemptsByWorkerThreads,
                 _prev.evictPageAttemptsByWorkerThreads,
                 snap.evictPageAttemptsByWorkerThreads);
        addDelta(_evictPageFailuresByWorkerThreads,
                 _prev.evictPageFailuresByWorkerThreads,
                 snap.evictPageFailuresByWorkerThreads);
        addDelta(_pageEvictAttemptsByAppThreads,
                 _prev.pageEvictAttemptsByAppThreads,
                 snap.pageEvictAttemptsByAppThreads);
        addDelta(_pageEvictFailuresByAppThreads,
                 _prev.pageEvictFailuresByAppThreads,
                 snap.pageEvictFailuresByAppThreads);
        addDelta(_bytesReadIntoCache, _prev.bytesReadIntoCache, snap.bytesReadIntoCache);
        addDelta(_bytesWrittenFromCache, _prev.bytesWrittenFromCache, snap.bytesWrittenFromCache);
        addDelta(_pagesReadIntoCache, _prev.pagesReadIntoCache, snap.pagesReadIntoCache);
        addDelta(
            _pagesRequestedFromCache, _prev.pagesRequestedFromCache, snap.pagesRequestedFromCache);

        // Gauges report the current value, no need to add a delta.
        _evictionEmptyScore.set(snap.evictionEmptyScore);
        _evictionWorkerThreadActive.set(snap.evictionWorkerThreadActive);
        _evictionWorkerThreadStableNumber.set(snap.evictionWorkerThreadStableNumber);
        _bytesCurrentlyInCache.set(snap.bytesCurrentlyInCache);
        _trackedDirtyBytesInCache.set(snap.trackedDirtyBytesInCache);
        _maximumBytesConfigured.set(snap.maximumBytesConfigured);
        _connectionDataHandlesCurrentlyActive.set(snap.connectionDataHandlesCurrentlyActive);
        _transactionCheckpointMostRecentTimeMsecs.set(
            snap.transactionCheckpointMostRecentTimeMsecs);
    }

    void updateTicketingSystem(const TicketingSystemStatsSnapshot& snap) {
        _concurrentTransactionsReadAvailable.set(snap.readAvailable);
        _concurrentTransactionsWriteAvailable.set(snap.writeAvailable);
    }

    void recordWTCollectError() {
        _collectErrors.add(1);
    }

    void recordWTEngineNotReadyError() {
        _engineNotReadyErrors.add(1);
    }

    void recordTSCollectError() {
        _ticketingSystemCollectErrors.add(1);
    }

private:
    Counter<int64_t>& _evictionCallsToGetPageFoundQueueEmpty;
    Counter<int64_t>& _evictPageAttemptsByWorkerThreads;
    Counter<int64_t>& _evictPageFailuresByWorkerThreads;
    Counter<int64_t>& _pageEvictAttemptsByAppThreads;
    Counter<int64_t>& _pageEvictFailuresByAppThreads;
    Counter<int64_t>& _bytesReadIntoCache;
    Counter<int64_t>& _bytesWrittenFromCache;
    Counter<int64_t>& _pagesReadIntoCache;
    Counter<int64_t>& _pagesRequestedFromCache;

    Gauge<int64_t>& _evictionEmptyScore;
    Gauge<int64_t>& _evictionWorkerThreadActive;
    Gauge<int64_t>& _evictionWorkerThreadStableNumber;
    Gauge<int64_t>& _bytesCurrentlyInCache;
    Gauge<int64_t>& _trackedDirtyBytesInCache;
    Gauge<int64_t>& _maximumBytesConfigured;
    Gauge<int64_t>& _connectionDataHandlesCurrentlyActive;
    Gauge<int64_t>& _transactionCheckpointMostRecentTimeMsecs;
    Gauge<int64_t>& _concurrentTransactionsReadAvailable;
    Gauge<int64_t>& _concurrentTransactionsWriteAvailable;

    // Error-handling metrics
    Counter<int64_t>& _collectErrors;
    Counter<int64_t>& _engineNotReadyErrors;
    Counter<int64_t>& _ticketingSystemCollectErrors;

    // Previous snapshot, used to compute deltas for the monotonic counters.
    WiredTigerStatsSnapshot _prev;
};

WiredTigerMetrics::WiredTigerMetrics() : _impl(std::make_unique<Impl>()) {}
WiredTigerMetrics::~WiredTigerMetrics() = default;

void WiredTigerMetrics::updateWiredTiger(const WiredTigerStatsSnapshot& snap) {
    _impl->updateWiredTiger(snap);
}

void WiredTigerMetrics::updateTicketingSystem(const TicketingSystemStatsSnapshot& snap) {
    _impl->updateTicketingSystem(snap);
}

void WiredTigerMetrics::recordWTCollectError() {
    _impl->recordWTCollectError();
}

void WiredTigerMetrics::recordWTEngineNotReadyError() {
    _impl->recordWTEngineNotReadyError();
}

void WiredTigerMetrics::recordTSCollectError() {
    _impl->recordTSCollectError();
}

WiredTigerStatsSnapshot parseWiredTigerStats(const BSONObj& stats) {
    WiredTigerStatsSnapshot snap;

    const BSONObj cache = stats.getObjectField("cache");
    snap.evictionCallsToGetPageFoundQueueEmpty =
        cache["eviction calls to get a page found queue empty"].safeNumberLong();
    snap.evictPageAttemptsByWorkerThreads =
        cache["evict page attempts by eviction worker threads"].safeNumberLong();
    snap.evictPageFailuresByWorkerThreads =
        cache["evict page failures by eviction worker threads"].safeNumberLong();
    snap.pageEvictAttemptsByAppThreads =
        cache["page evict attempts by application threads"].safeNumberLong();
    snap.pageEvictFailuresByAppThreads =
        cache["page evict failures by application threads"].safeNumberLong();
    snap.bytesReadIntoCache = cache["bytes read into cache"].safeNumberLong();
    snap.bytesWrittenFromCache = cache["bytes written from cache"].safeNumberLong();
    snap.pagesReadIntoCache = cache["pages read into cache"].safeNumberLong();
    snap.pagesRequestedFromCache = cache["pages requested from the cache"].safeNumberLong();
    snap.evictionEmptyScore = cache["eviction empty score"].safeNumberLong();
    snap.evictionWorkerThreadActive = cache["eviction worker thread active"].safeNumberLong();
    snap.evictionWorkerThreadStableNumber =
        cache["eviction worker thread stable number"].safeNumberLong();
    snap.bytesCurrentlyInCache = cache["bytes currently in the cache"].safeNumberLong();
    snap.trackedDirtyBytesInCache = cache["tracked dirty bytes in the cache"].safeNumberLong();
    snap.maximumBytesConfigured = cache["maximum bytes configured"].safeNumberLong();

    const BSONObj checkpoint = stats.getObjectField("checkpoint");
    snap.transactionCheckpointMostRecentTimeMsecs =
        checkpoint["most recent time (msecs)"].safeNumberLong();

    const BSONObj dataHandle = stats.getObjectField("data-handle");
    snap.connectionDataHandlesCurrentlyActive =
        dataHandle["connection data handles currently active"].safeNumberLong();

    return snap;
}

void runWiredTigerCollectionCycle(WiredTigerMetrics& metrics, ServiceContext* svc) {
    // Make sure the storage engine cannot change while we're reading it.
    auto rlk = svc->getStorageChangeMutex().readLock();
    auto* se = svc->getStorageEngine();
    if (!se) {
        metrics.recordWTEngineNotReadyError();
        return;
    }

    auto* engine = se->getEngine();
    if (!engine) {
        metrics.recordWTEngineNotReadyError();
        return;
    }

    auto stats = engine->collectStorageStats();
    if (!stats) {
        metrics.recordWTCollectError();
        return;
    }

    auto snap = parseWiredTigerStats(*stats);
    metrics.updateWiredTiger(snap);
}

TicketingSystemStatsSnapshot parseTicketingSystemStats(const BSONObj& stats) {
    TicketingSystemStatsSnapshot snap;

    auto readStats = stats.getObjectField("read");
    snap.readAvailable = readStats["available"].safeNumberInt();
    auto writeStats = stats.getObjectField("write");
    snap.writeAvailable = writeStats["available"].safeNumberInt();

    return snap;
}

void runTicketingSystemCollectionCycle(WiredTigerMetrics& metrics, ServiceContext* svc) {
    auto* ts = TicketingSystem::get(svc);
    if (!ts) {
        metrics.recordTSCollectError();
        return;
    }

    BSONObjBuilder statBuilder;
    ts->appendStats(statBuilder);

    auto stats = statBuilder.obj();
    auto snap = parseTicketingSystemStats(stats);
    metrics.updateTicketingSystem(snap);
}

void runCollectionCycle(WiredTigerMetrics& metrics, ServiceContext* svc) {
    runWiredTigerCollectionCycle(metrics, svc);
    runTicketingSystemCollectionCycle(metrics, svc);
}

void installWiredTigerOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getWiredTigerOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<WiredTigerMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "WiredTigerOtelMetrics",
        [&state, svcCtx](Client*) { runCollectionCycle(*state.metrics, svcCtx); },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}

}  // namespace mongo

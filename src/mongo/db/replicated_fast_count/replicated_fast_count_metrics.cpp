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

#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"

#include "mongo/db/repl/optime_observer.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/server_status_options.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ServerStatusOptions;

// Boolean flag indicating whether or not the fast count background thread is currently running.
// Since OTel gauges do not natively support booleans, we use an int64_t gauge instead.
auto& isRunningGauge = MetricsService::instance().createInt64Gauge(
    MetricNames::kReplicatedFastCountIsRunning,
    "1 if the replicated fast count background thread is running, 0 otherwise",
    MetricUnit::kEvents,
    {.serverStatusOptions = ServerStatusOptions{.dottedPath = "replicatedFastCount.isRunning",
                                                .role = ClusterRole::None}});

// Flushes persist fast count information to the oplog and occur during checkpointing,
// shutdown, etc. The total number of flush attempts = flushSuccessCounter + flushFailureCounter.
auto& flushSuccessCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kReplicatedFastCountFlushSuccessCount,
    "Total number of successful replicated fast count flushes",
    MetricUnit::kEvents,
    {.serverStatusOptions = ServerStatusOptions{
         .dottedPath = "replicatedFastCount.flush.successCount", .role = ClusterRole::None}});

auto& flushFailureCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kReplicatedFastCountFlushFailureCount,
    "Total number of failed replicated fast count flushes",
    MetricUnit::kEvents,
    {.serverStatusOptions = ServerStatusOptions{
         .dottedPath = "replicatedFastCount.flush.failureCount", .role = ClusterRole::None}});

auto& flushTimeMsTotalCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kReplicatedFastCountFlushTimeMsTotal,
    "Total flush duration in milliseconds across all replicated fast count flushes",
    MetricUnit::kMilliseconds,
    {.serverStatusOptions = ServerStatusOptions{.dottedPath = "replicatedFastCount.flushTime.total",
                                                .role = ClusterRole::None}});

// The total number of documents written during flushes.
auto& flushedDocsTotalCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kReplicatedFastCountFlushedDocsTotal,
    "Total number of documents written across all replicated fast count flushes",
    MetricUnit::kEvents,
    {.serverStatusOptions = ServerStatusOptions{
         .dottedPath = "replicatedFastCount.flushedDocs.total", .role = ClusterRole::None}});

// The number of inserts/updates to the replicated fast count collection.
auto& insertCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kReplicatedFastCountInsertCount,
    "Number of inserts into a new record for storing size and count data in the replicated "
    "fast count collection",
    MetricUnit::kOperations,
    {.serverStatusOptions = ServerStatusOptions{.dottedPath = "replicatedFastCount.insertCount",
                                                .role = ClusterRole::None}});

auto& updateCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kReplicatedFastCountUpdateCount,
    "Number of updates to an existing record storing size and count data in the replicated "
    "fast count collection",
    MetricUnit::kOperations,
    {.serverStatusOptions = ServerStatusOptions{.dottedPath = "replicatedFastCount.updateCount",
                                                .role = ClusterRole::None}});

// The total time spent writing to the replicated fast count collection during flushing. This is
// useful for determining the proportion of flush time spent writing (writeTimeMsTotalCounter /
// flushTimeMsTotalCounter).
auto& writeTimeMsTotalCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kReplicatedFastCountWriteTimeMsTotal,
    "Total time in milliseconds spent writing metadata to the replicated fast count collection",
    MetricUnit::kMilliseconds,
    {.serverStatusOptions = ServerStatusOptions{.dottedPath = "replicatedFastCount.writeTime.total",
                                                .role = ClusterRole::None}});

// Gauge for the number of seconds between the most recently applied oplog entry and the most
// recently persisted fastcount checkpoint. A large value indicates the fastcount checkpoint is
// falling behind replication.
auto& oplogLagSecsGauge = MetricsService::instance().createInt64Gauge(
    MetricNames::kReplicatedFastCountOplogLagSecs,
    "Seconds between the last applied oplog entry and the last persisted fastcount checkpoint",
    MetricUnit::kSeconds,
    {.serverStatusOptions = ServerStatusOptions{.dottedPath = "replicatedFastCount.oplogLagSecs",
                                                .role = ClusterRole::None}});

// Hold the seconds field of the last observed applied and persisted checkpoint Timestamps. Zero
// means "not yet observed"; the gauge stays at 0 until both are populated at least once.
Atomic<uint32_t> lastAppliedSecs{0};
Atomic<uint32_t> checkpointSecs{0};

void refreshOplogLagGauge() {
    const auto applied = lastAppliedSecs.loadRelaxed();
    const auto checkpoint = checkpointSecs.loadRelaxed();
    if (applied == 0 || checkpoint == 0) {
        return;
    }
    oplogLagSecsGauge.set(static_cast<int64_t>(applied) - static_cast<int64_t>(checkpoint));
}

}  // namespace

void ReplicatedFastCountMetrics::setIsRunning(bool running) {
    isRunningGauge.set(running ? 1 : 0);
}

void ReplicatedFastCountMetrics::recordFlush(Date_t startTime, size_t batchSize) {
    const int64_t elapsedMs = (Date_t::now() - startTime).count();

    flushSuccessCounter.add(1);
    flushTimeMsTotalCounter.add(elapsedMs);
    flushedDocsTotalCounter.add(static_cast<int64_t>(batchSize));
}

void ReplicatedFastCountMetrics::incrementFlushFailureCount() {
    flushFailureCounter.add(1);
}

void ReplicatedFastCountMetrics::incrementInsertCount() {
    insertCounter.add(1);
}

void ReplicatedFastCountMetrics::incrementUpdateCount() {
    updateCounter.add(1);
}

void ReplicatedFastCountMetrics::addWriteTimeMsTotal(int64_t ms) {
    writeTimeMsTotalCounter.add(ms);
}

void recordAppliedOpTime(const Timestamp& ts) {
    tassert(12397401, "applied opTime fed into oplog_lag_secs must be non-null", !ts.isNull());
    lastAppliedSecs.storeRelaxed(ts.getSecs());
    refreshOplogLagGauge();
}

void recordCheckpointAdvanced(const Timestamp& ts) {
    tassert(
        12397402, "checkpoint timestamp fed into oplog_lag_secs must be non-null", !ts.isNull());
    checkpointSecs.storeRelaxed(ts.getSecs());
    refreshOplogLagGauge();
}

namespace {
class FastCountAppliedOpTimeObserver : public repl::OpTimeObserver {
public:
    void onOpTime(const Timestamp& ts) override {
        recordAppliedOpTime(ts);
    }
};
}  // namespace

void registerAppliedOpTimeObserver(ServiceContext* svcCtx) {
    repl::ReplicationCoordinator::get(svcCtx)->addAppliedOpTimeObserver(
        std::make_unique<FastCountAppliedOpTimeObserver>());
}

void resetOplogLagState_ForTest() {
    lastAppliedSecs.storeRelaxed(0);
    checkpointSecs.storeRelaxed(0);
    oplogLagSecsGauge.set(0);
}

namespace {

using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ServerStatusOptions;

auto& checkpointOplogEntriesProcessedCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kReplicatedFastCountCheckpointOplogEntriesProcessed,
    "Total oplog entries forwarded to delta processing during checkpoint scans",
    MetricUnit::kEvents,
    {.serverStatusOptions = ServerStatusOptions{
         .dottedPath = "replicatedFastCount.checkpoint.oplogEntriesProcessed"}});

auto& checkpointOplogEntriesSkippedCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kReplicatedFastCountCheckpointOplogEntriesSkipped,
    "Total oplog entries discarded as fastcount-internal writes during checkpoint scans",
    MetricUnit::kEvents,
    {.serverStatusOptions =
         ServerStatusOptions{.dottedPath = "replicatedFastCount.checkpoint.oplogEntriesSkipped"}});

auto& checkpointSizeCountEntriesProcessedCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kReplicatedFastCountCheckpointSizeCountEntriesProcessed,
    "Total individual size/count deltas accumulated during checkpoint scans",
    MetricUnit::kEvents,
    {.serverStatusOptions = ServerStatusOptions{
         .dottedPath = "replicatedFastCount.checkpoint.sizeCountEntriesProcessed"}});

}  // namespace

void recordCheckpointOplogEntryProcessed() {
    checkpointOplogEntriesProcessedCounter.add(1);
}

void recordCheckpointOplogEntrySkipped() {
    checkpointOplogEntriesSkippedCounter.add(1);
}

void recordCheckpointSizeCountEntryProcessed(int count) {
    checkpointSizeCountEntriesProcessedCounter.add(count);
}

}  // namespace mongo

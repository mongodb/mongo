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

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_service.h"

#include <algorithm>

namespace mongo {

ReplicatedFastCountMetrics::ReplicatedFastCountMetrics()
    : _isRunningGauge(otel::metrics::MetricsService::instance().createInt64Gauge(
          otel::metrics::MetricNames::kReplicatedFastCountIsRunning,
          "1 if the replicated fast count background thread is running, 0 otherwise",
          otel::metrics::MetricUnit::kEvents,
          {.inServerStatus = true})),
      _flushSuccessCounter(otel::metrics::MetricsService::instance().createInt64Counter(
          otel::metrics::MetricNames::kReplicatedFastCountFlushSuccessCount,
          "Total number of successful replicated fast count flushes",
          otel::metrics::MetricUnit::kEvents,
          {.inServerStatus = true})),
      _flushFailureCounter(otel::metrics::MetricsService::instance().createInt64Counter(
          otel::metrics::MetricNames::kReplicatedFastCountFlushFailureCount,
          "Total number of failed replicated fast count flushes",
          otel::metrics::MetricUnit::kEvents,
          {.inServerStatus = true})),
      _flushTimeMsMinGauge(otel::metrics::MetricsService::instance().createInt64Gauge(
          otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsMin,
          "Minimum flush duration in milliseconds across all replicated fast count flushes",
          otel::metrics::MetricUnit::kMilliseconds,
          {.inServerStatus = true})),
      _flushTimeMsMaxGauge(otel::metrics::MetricsService::instance().createInt64Gauge(
          otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsMax,
          "Maximum flush duration in milliseconds across all replicated fast count flushes",
          otel::metrics::MetricUnit::kMilliseconds,
          {.inServerStatus = true})),
      _flushTimeMsTotalCounter(otel::metrics::MetricsService::instance().createInt64Counter(
          otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsTotal,
          "Total flush duration in milliseconds across all replicated fast count flushes",
          otel::metrics::MetricUnit::kMilliseconds,
          {.inServerStatus = true})),
      _flushedDocsMinGauge(otel::metrics::MetricsService::instance().createInt64Gauge(
          otel::metrics::MetricNames::kReplicatedFastCountFlushedDocsMin,
          "Minimum number of documents written in a single replicated fast count flush",
          otel::metrics::MetricUnit::kEvents,
          {.inServerStatus = true})),
      _flushedDocsMaxGauge(otel::metrics::MetricsService::instance().createInt64Gauge(
          otel::metrics::MetricNames::kReplicatedFastCountFlushedDocsMax,
          "Maximum number of documents written in a single replicated fast count flush",
          otel::metrics::MetricUnit::kEvents,
          {.inServerStatus = true})),
      _flushedDocsTotalCounter(otel::metrics::MetricsService::instance().createInt64Counter(
          otel::metrics::MetricNames::kReplicatedFastCountFlushedDocsTotal,
          "Total number of documents written across all replicated fast count flushes",
          otel::metrics::MetricUnit::kEvents,
          {.inServerStatus = true})),
      _emptyUpdateCounter(otel::metrics::MetricsService::instance().createInt64Counter(
          otel::metrics::MetricNames::kReplicatedFastCountEmptyUpdateCount,
          "Number of times an empty diff was found when writing an update to the replicated fast "
          "count collection",
          otel::metrics::MetricUnit::kEvents,
          {.inServerStatus = true})),
      _insertCounter(otel::metrics::MetricsService::instance().createInt64Counter(
          otel::metrics::MetricNames::kReplicatedFastCountInsertCount,
          "Number of inserts into a new record for storing size and count data in the replicated "
          "fast count collection",
          otel::metrics::MetricUnit::kOperations,
          {.inServerStatus = true})),
      _updateCounter(otel::metrics::MetricsService::instance().createInt64Counter(
          otel::metrics::MetricNames::kReplicatedFastCountUpdateCount,
          "Number of updates to an existing record storing size and count data in the replicated "
          "fast count collection",
          otel::metrics::MetricUnit::kOperations,
          {.inServerStatus = true})),
      _writeTimeMsTotalCounter(otel::metrics::MetricsService::instance().createInt64Counter(
          otel::metrics::MetricNames::kReplicatedFastCountWriteTimeMsTotal,
          "Total time in milliseconds spent writing metadata to the replicated fast count "
          "collection",
          otel::metrics::MetricUnit::kMilliseconds,
          {.inServerStatus = true})) {}

void ReplicatedFastCountMetrics::setIsRunning(bool running) {
    _isRunningGauge.set(running ? 1 : 0);
}

void ReplicatedFastCountMetrics::recordFlush(Date_t startTime, size_t batchSize) {
    const int64_t elapsedMs = (Date_t::now() - startTime).count();

    _flushSuccessCounter.add(1);
    _flushTimeMsTotalCounter.add(elapsedMs);
    _flushTimeMsMaxGauge.set(std::max(_flushTimeMsMaxGauge.value(), elapsedMs));
    _flushTimeMsMinPlaceholder.storeRelaxed(
        std::min(_flushTimeMsMinPlaceholder.loadRelaxed(), elapsedMs));
    _flushTimeMsMinGauge.set(_flushTimeMsMinPlaceholder.loadRelaxed());

    const int64_t docs = static_cast<int64_t>(batchSize);
    _flushedDocsTotalCounter.add(docs);
    _flushedDocsMaxGauge.set(std::max(_flushedDocsMaxGauge.value(), docs));
    _flushedDocsMinPlaceholder.storeRelaxed(
        std::min(_flushedDocsMinPlaceholder.loadRelaxed(), docs));
    _flushedDocsMinGauge.set(_flushedDocsMinPlaceholder.loadRelaxed());
}

void ReplicatedFastCountMetrics::incrementFlushFailureCount() {
    _flushFailureCounter.add(1);
}

void ReplicatedFastCountMetrics::incrementEmptyUpdateCount() {
    _emptyUpdateCounter.add(1);
}

void ReplicatedFastCountMetrics::incrementInsertCount() {
    _insertCounter.add(1);
}

void ReplicatedFastCountMetrics::incrementUpdateCount() {
    _updateCounter.add(1);
}

void ReplicatedFastCountMetrics::addWriteTimeMsTotal(int64_t ms) {
    _writeTimeMsTotalCounter.add(ms);
}

}  // namespace mongo

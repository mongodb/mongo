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

#pragma once

#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/time_support.h"

#include <limits>

namespace mongo {

/**
 * Wrapper for the ReplicatedFastCountManager OpenTelemetry metric instruments. All metrics are
 * reported directly via OTel and serverStatus.
 */
class ReplicatedFastCountMetrics {
public:
    ReplicatedFastCountMetrics();

    void setIsRunning(bool running);

    /**
     * Records metrics for a successful flush. Updates flush timing and flushed-document
     * counters and gauges.
     */
    void recordFlush(Date_t startTime, size_t batchSize);

    void incrementFlushFailureCount();

    void incrementEmptyUpdateCount();

    void incrementInsertCount();

    void incrementUpdateCount();

    void addWriteTimeMsTotal(int64_t ms);

private:
    // Boolean flag indicating whether or not the fast count background thread is currently running.
    // Since OTel gauges do not natively support booleans, we use an int64_t gauge instead.
    otel::metrics::Gauge<int64_t>& _isRunningGauge;

    // Flushes persist fast count information to the oplog and occur during checkpointing,
    // shutdown, etc. The total number of flush attempts = _flushSuccessCounter +
    // _flushFailureCounter.
    otel::metrics::Counter<int64_t>& _flushSuccessCounter;
    otel::metrics::Counter<int64_t>& _flushFailureCounter;
    otel::metrics::Gauge<int64_t>& _flushTimeMsMinGauge;
    otel::metrics::Gauge<int64_t>& _flushTimeMsMaxGauge;
    otel::metrics::Counter<int64_t>& _flushTimeMsTotalCounter;
    // Aggregate metrics for the min/max number of documents inserted or updated during one
    // flush.
    otel::metrics::Gauge<int64_t>& _flushedDocsMinGauge;
    otel::metrics::Gauge<int64_t>& _flushedDocsMaxGauge;
    // The total number of documents written during flushes.
    otel::metrics::Counter<int64_t>& _flushedDocsTotalCounter;

    // The number of times an empty diff is found when writing an update to the replicated fast
    // count collection.
    otel::metrics::Counter<int64_t>& _emptyUpdateCounter;

    // The number of inserts/updates to the replicated fast count collection.
    otel::metrics::Counter<int64_t>& _insertCounter;
    otel::metrics::Counter<int64_t>& _updateCounter;

    // The total time spent writing to the replicated fast count collection during flushing. This is
    // useful for determining the proportion of flush time spent writing (_writeTimeMsTotalCounter /
    // flushTimeMsTotalCounter).
    otel::metrics::Counter<int64_t>& _writeTimeMsTotalCounter;

    // Placeholder atomics for tracking the running minimum across flushes. Since flushTimeMinGauge
    // and flushedDocsMinGauge are initialized to 0, we compare the first flush duration to these
    // placeholders to ensure that the aforementioned gauges do not forever remain set to 0.
    Atomic<int64_t> _flushTimeMsMinPlaceholder{std::numeric_limits<int64_t>::max()};
    Atomic<int64_t> _flushedDocsMinPlaceholder{std::numeric_limits<int64_t>::max()};
};

}  // namespace mongo

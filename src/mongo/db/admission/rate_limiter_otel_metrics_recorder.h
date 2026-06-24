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

#include "mongo/base/string_data.h"
#include "mongo/db/admission/rate_limiter_metrics_events.h"
#include "mongo/db/admission/rate_limiter_metrics_recorder.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"
#include "mongo/util/moving_average.h"
#include "mongo/util/periodic_runner.h"

#include <cstdint>
#include <functional>

#include <boost/optional.hpp>

namespace mongo::admission {

/**
 * An OTEL integrated metrics recorder for the rate limiter. It mirrors the rate limiter's metric
 * events into OTel instruments and reads the values back to satisfy the RateLimiterMetricsRecorder
 * accessors. The counter-backed accessors read directly from the OTel instruments, while the
 * moving average (gauge-only, no read-back) is tracked in process to keep it in sync.
 */
class RateLimiterOtelMetricsRecorder final : public RateLimiterMetricsRecorder {
public:
    /**
     * Identifies the OTel instrument names (from the central MetricNames registry) and the
     * serverStatus dotted-path prefix used by a particular rate limiter. Every rate limiter that
     * records metrics must use its own distinct instrument names (the MetricsService registry
     * rejects re-registering a name with a different definition), so each caller supplies the spec
     * appropriate for its limiter.
     */
    struct MetricsSpec {
        otel::metrics::MetricName attemptedAdmissions;
        otel::metrics::MetricName successfulAdmissions;
        otel::metrics::MetricName rejectedAdmissions;
        otel::metrics::MetricName exemptedAdmissions;
        otel::metrics::MetricName addedToQueue;
        otel::metrics::MetricName removedFromQueue;
        otel::metrics::MetricName interruptedInQueue;
        otel::metrics::MetricName tokensAcquired;
        otel::metrics::MetricName currentQueueDepth;
        otel::metrics::MetricName totalAvailableTokens;
        otel::metrics::MetricName averageTimeQueuedMicros;
        otel::metrics::MetricName timeQueuedMicros;
    };

    /** Creates the OTel instruments described by `spec`. */
    explicit RateLimiterOtelMetricsRecorder(MetricsSpec spec);

    void record(const RateLimiterMetricsRecorderEvent& event) noexcept override;

    int64_t addedToQueue() const override;
    int64_t removedFromQueue() const override;
    int64_t interruptedInQueue() const override;
    int64_t rejectedAdmissions() const override;
    int64_t successfulAdmissions() const override;
    int64_t exemptedAdmissions() const override;
    int64_t attemptedAdmissions() const override;
    boost::optional<double> averageTimeQueuedMicros() const override;
    double tokensAcquired() const override;
    double tokensAvailable() const override;
    int64_t currentQueueDepth() const override;

    /**
     * Starts a periodic job on `runner` that samples the available-token count via
     * `sampleAvailableTokens` and mirrors it into this recorder's OTel gauges. Returns the job
     * anchor; the caller owns it and must keep it alive for sampling to continue.
     */
    PeriodicRunner::JobAnchor installOtelMetrics(PeriodicRunner* runner,
                                                 Milliseconds period,
                                                 StringData name,
                                                 std::function<double()> sampleAvailableTokens);

private:
    otel::metrics::Counter<int64_t>* _attemptedCounter{nullptr};
    otel::metrics::Counter<int64_t>* _successfulCounter{nullptr};
    otel::metrics::Counter<int64_t>* _rejectedCounter{nullptr};
    otel::metrics::Counter<int64_t>* _addedToQueueCounter{nullptr};
    otel::metrics::Counter<int64_t>* _removedFromQueueCounter{nullptr};
    otel::metrics::Counter<int64_t>* _interruptedInQueueCounter{nullptr};
    otel::metrics::Counter<int64_t>* _exemptedAdmissionsCounter{nullptr};
    otel::metrics::Counter<double>* _tokensAcquiredCounter{nullptr};
    otel::metrics::Gauge<int64_t>* _queueDepthGauge{nullptr};
    otel::metrics::Gauge<double>* _tokensAvailableGauge{nullptr};
    otel::metrics::Gauge<double>* _averageTimeQueuedMicrosGauge{nullptr};
    otel::metrics::Histogram<int64_t>* _timeQueuedMicrosHistogram{nullptr};

    Atomic<int64_t> _queueDepthCounter{0};
    Atomic<double> _tokensAvailable{0};
    MovingAverage _averageTimeQueuedMicros{0.2};
};

}  // namespace mongo::admission

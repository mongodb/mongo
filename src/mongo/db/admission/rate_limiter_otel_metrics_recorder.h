// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

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
#include <string_view>

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
                                                 std::string_view name,
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

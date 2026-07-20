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
#include "mongo/util/modules.h"
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
 *
 * Only the metrics whose instrument names are supplied in the MetricsSpec are instantiated. For any
 * metric left unset, recording the corresponding event is a no-op and its accessor returns a
 * zero-valued default (or boost::none for averageTimeQueuedMicros). This lets lightweight callers
 * mirror only the admission outcomes they care about without paying for the full instrument set.
 */
class [[MONGO_MOD_PUBLIC]] RateLimiterOtelMetricsRecorder final
    : public RateLimiterMetricsRecorder {
public:
    /**
     * Identifies the OTel instrument names (from the central MetricNames registry) and the
     * serverStatus dotted-path prefix used by a particular rate limiter. Each recorder must use its
     * own distinct instrument names. Having multiple recorders share the same MetricNames will
     * cause each rate limiter to influence the same OTel instruments.
     *
     * Each field is optional: an unset field means the corresponding instrument is not created, so
     * recording that metric is a no-op and its accessor returns a default value.
     */
    struct MetricsSpec {
        boost::optional<otel::metrics::MetricName> attemptedAdmissions;
        boost::optional<otel::metrics::MetricName> successfulAdmissions;
        boost::optional<otel::metrics::MetricName> rejectedAdmissions;
        boost::optional<otel::metrics::MetricName> exemptedAdmissions;
        boost::optional<otel::metrics::MetricName> addedToQueue;
        boost::optional<otel::metrics::MetricName> removedFromQueue;
        boost::optional<otel::metrics::MetricName> interruptedInQueue;
        boost::optional<otel::metrics::MetricName> tokensAcquired;
        boost::optional<otel::metrics::MetricName> currentQueueDepth;
        boost::optional<otel::metrics::MetricName> totalAvailableTokens;
        boost::optional<otel::metrics::MetricName> averageTimeQueuedMicros;
        boost::optional<otel::metrics::MetricName> timeQueuedMicros;
    };

    /** Creates the OTel instruments described by the set fields of `spec`. */
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
    otel::metrics::Counter<int64_t>* _attemptedCounter{
        otel::metrics::NoopCounter<int64_t>::instance()};
    otel::metrics::Counter<int64_t>* _successfulCounter{
        otel::metrics::NoopCounter<int64_t>::instance()};
    otel::metrics::Counter<int64_t>* _rejectedCounter{
        otel::metrics::NoopCounter<int64_t>::instance()};
    otel::metrics::Counter<int64_t>* _addedToQueueCounter{
        otel::metrics::NoopCounter<int64_t>::instance()};
    otel::metrics::Counter<int64_t>* _removedFromQueueCounter{
        otel::metrics::NoopCounter<int64_t>::instance()};
    otel::metrics::Counter<int64_t>* _interruptedInQueueCounter{
        otel::metrics::NoopCounter<int64_t>::instance()};
    otel::metrics::Counter<int64_t>* _exemptedAdmissionsCounter{
        otel::metrics::NoopCounter<int64_t>::instance()};
    otel::metrics::Counter<double>* _tokensAcquiredCounter{
        otel::metrics::NoopCounter<double>::instance()};
    otel::metrics::Gauge<int64_t>* _queueDepthGauge{otel::metrics::NoopGauge<int64_t>::instance()};
    otel::metrics::Gauge<double>* _tokensAvailableGauge{
        otel::metrics::NoopGauge<double>::instance()};
    otel::metrics::Gauge<double>* _averageTimeQueuedMicrosGauge{
        otel::metrics::NoopGauge<double>::instance()};
    otel::metrics::Histogram<int64_t>* _timeQueuedMicrosHistogram{
        otel::metrics::NoopHistogram<int64_t>::instance()};

    Atomic<int64_t> _queueDepthCounter{0};
    Atomic<double> _tokensAvailable{0};
    MovingAverage _averageTimeQueuedMicros{0.2};
};

}  // namespace mongo::admission

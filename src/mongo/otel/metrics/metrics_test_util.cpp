// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/metrics_test_util.h"

#include "mongo/otel/metrics/metrics_service.h"

namespace mongo::otel::metrics {

#ifdef MONGO_CONFIG_OTEL
using test_util_detail::getMetricData;
#endif  // MONGO_CONFIG_OTEL

OtelMetricsCapturer::OtelMetricsCapturer() : OtelMetricsCapturer(MetricsService::instance()) {}

OtelMetricsCapturer::OtelMetricsCapturer(MetricsService& metricsService)
    : _metricsService(metricsService) {
#ifdef MONGO_CONFIG_OTEL
    invariant(isNoopMeterProvider(opentelemetry::metrics::Provider::GetMeterProvider().get()));

    auto metrics =
        std::make_shared<opentelemetry::exporter::memory::SimpleAggregateInMemoryMetricData>();
    _metrics = metrics.get();

    auto exporter =
        opentelemetry::exporter::memory::InMemoryMetricExporterFactory::Create(std::move(metrics));

    auto reader = std::make_shared<test_util_detail::OnDemandMetricReader>(std::move(exporter));
    _reader = reader.get();

    std::shared_ptr<opentelemetry::sdk::metrics::MeterProvider> provider =
        opentelemetry::sdk::metrics::MeterProviderFactory::Create();
    provider->AddMetricReader(std::move(reader));

    // Initialize metrics that were created before the MeterProvider was set.
    metricsService.initialize(*provider);

    opentelemetry::metrics::Provider::SetMeterProvider(std::move(provider));
#endif  // MONGO_CONFIG_OTEL
}

#ifdef MONGO_CONFIG_OTEL
OtelMetricsCapturer::~OtelMetricsCapturer() {
    auto provider = std::make_shared<opentelemetry::metrics::NoopMeterProvider>();
    // Re-initialize the metrics service so that any metrics relying on the old meter provider can
    // continue to function correctly.
    _metricsService.initialize(*provider);
    opentelemetry::metrics::Provider::SetMeterProvider(std::move(provider));
}
#endif  // MONGO_CONFIG_OTEL

int64_t OtelMetricsCapturer::readInt64Counter(MetricName name) {
    return readInt64Counter(name, /*attributes=*/{});
}

double OtelMetricsCapturer::readDoubleCounter(MetricName name) {
    return readDoubleCounter(name, /*attributes=*/{});
}

int64_t OtelMetricsCapturer::readInt64Gauge(MetricName name) {
    return readInt64Gauge(name, /*attributes=*/{});
}

double OtelMetricsCapturer::readDoubleGauge(MetricName name) {
    return readDoubleGauge(name, /*attributes=*/{});
}

HistogramData<int64_t> OtelMetricsCapturer::readInt64Histogram(MetricName name) {
#ifdef MONGO_CONFIG_OTEL
    return getMetricData<opentelemetry::sdk::metrics::HistogramPointData>(
        *_metrics, *_reader, _metricsService, name, /*attributes=*/{});
#else
    invariant(false, kUsingOtelOnWindows);
    return {};
#endif  // MONGO_CONFIG_OTEL
}

HistogramData<double> OtelMetricsCapturer::readDoubleHistogram(MetricName name) {
#ifdef MONGO_CONFIG_OTEL
    return getMetricData<opentelemetry::sdk::metrics::HistogramPointData>(
        *_metrics, *_reader, _metricsService, name, /*attributes=*/{});
#else
    invariant(false, kUsingOtelOnWindows);
    return {};
#endif  // MONGO_CONFIG_OTEL
}

bool OtelMetricsCapturer::canReadMetrics() {
#ifdef MONGO_CONFIG_OTEL
    return true;
#else
    return false;
#endif
}

}  // namespace mongo::otel::metrics

/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

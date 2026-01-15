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

#pragma once

#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/modules.h"

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/exporters/memory/in_memory_data.h>
#include <opentelemetry/exporters/memory/in_memory_metric_data.h>
#include <opentelemetry/exporters/memory/in_memory_metric_exporter_factory.h>
#include <opentelemetry/metrics/noop.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/data/metric_data.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/metrics/metric_reader.h>
#endif  // MONGO_CONFIG_OTEL

namespace mongo::otel::metrics {
#ifdef MONGO_CONFIG_OTEL
namespace test_util_detail {
/**
 * The MetricReader is the OTel component that connects the exporter with each registered
 * Instrument. This implementation allows callers to trigger collection on-demand so that unit tests
 * can collect metrics in a predictable fashion.
 */
class OnDemandMetricReader : public opentelemetry::sdk::metrics::MetricReader {
public:
    OnDemandMetricReader(std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> exporter)
        : _exporter{std::move(exporter)} {}

    opentelemetry::sdk::metrics::AggregationTemporality GetAggregationTemporality(
        opentelemetry::sdk::metrics::InstrumentType instrument_type) const noexcept override {
        return _exporter->GetAggregationTemporality(instrument_type);
    }

    void triggerMetricExport() {
        this->Collect([this](opentelemetry::sdk::metrics::ResourceMetrics& metric_data) {
            this->_exporter->Export(metric_data);
            return true;
        });
    }

private:
    bool OnForceFlush(std::chrono::microseconds) noexcept override {
        return true;
    }

    bool OnShutDown(std::chrono::microseconds) noexcept override {
        return true;
    }

    std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> _exporter;
};
}  // namespace test_util_detail

inline bool isNoopMeter(opentelemetry::metrics::Meter* provider) {
    return !!dynamic_cast<opentelemetry::metrics::NoopMeter*>(provider);
}

inline bool isNoopMeterProvider(opentelemetry::metrics::MeterProvider* provider) {
    return !!dynamic_cast<opentelemetry::metrics::NoopMeterProvider*>(provider);
}
#endif  // MONGO_CONFIG_OTEL

/**
 * Class used for reading the data of an OpenTelemtry histogram.
 */
template <typename T>
class HistogramData {
public:
#ifdef MONGO_CONFIG_OTEL
    HistogramData(opentelemetry::sdk::metrics::HistogramPointData data);
#endif  // MONGO_CONFIG_OTEL

    // See the documentation for MetricsService::createInt64Histogram in metrics_service.h for an
    // explanation of this boundaries member variable.
    std::vector<double> boundaries;
    T sum;
    T min;
    T max;
    std::vector<uint64_t> counts;
    uint64_t count;

#ifdef MONGO_CONFIG_OTEL
private:
    /**
     * Gets the underlying value of the provided ValueType, either int64_t or double.
     */
    T getValue(opentelemetry::sdk::metrics::ValueType valueType) {
        massert(ErrorCodes::TypeMismatch,
                "The internal type of the histogram and the requested type do not match",
                std::holds_alternative<T>(valueType));
        return std::get<T>(valueType);
    }
#endif  // MONGO_CONFIG_OTEL
};
#ifdef MONGO_CONFIG_OTEL
template <typename T>
HistogramData<T>::HistogramData(opentelemetry::sdk::metrics::HistogramPointData data)
    : boundaries(data.boundaries_), counts(data.counts_), count(data.count_) {
    sum = getValue(data.sum_);
    min = getValue(data.min_);
    max = getValue(data.max_);
};
#endif  // MONGO_CONFIG_OTEL

/**
 * Sets up a MetricProvider with an in-memory exporter so tests can create and inspect metrics. This
 * must be constructed before creating any metrics in order to capture them.
 *
 * NOTE: Not all platforms support exporting OpenTelemetry metrics (e.g., Windows), and on those
 * platforms it is not possible to read the metrics in tests via readInt64Counter(),
 * readDoubleCounter(), etc. You can use canReadMetrics() in tests to check whether the platform
 * supports OpenTelemetry metrics.
 *
 * WARNING: On platforms that do not support OpenTelemetry metrics, the OtelMetricsCapturer does not
 * call MetricsService::initialize(), so metric values recorded before initialization are not reset
 * when creating the OtelMetricsCapturer. Do not record metrics before creating the
 * OtelMetricsCapturer, otherwise the values will vary depending on the platform. On
 * non-OpenTelemetry platforms, it is only possible to read metrics via serverStatus.
 */
class MONGO_MOD_PUBLIC OtelMetricsCapturer {
public:
    /**
     * Default constructor which uses the static MetricsService object.
     */
    OtelMetricsCapturer();

    /**
     * Constructor that allows specifying the MetricsService object to initialize.
     *
     * This constructor should be used directly in limited circumstances. Specifically, this
     * constructor allows unit testing the MetricsService, but tests in server code should use the
     * static instance.
     */
    MONGO_MOD_PRIVATE OtelMetricsCapturer(MetricsService& metricsService);
#if MONGO_CONFIG_OTEL
    ~OtelMetricsCapturer() {
        opentelemetry::metrics::Provider::SetMeterProvider(
            opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider>(
                new opentelemetry::metrics::NoopMeterProvider()));
    }
#endif  // MONGO_CONFIG_OTEL
    /**
     * Gets the value of an int64_t counter and throws an exception if it is not found.
     */
    int64_t readInt64Counter(MetricName name);

    /**
     * Gets the value of a double counter and throws an exception if it is not found.
     */
    double readDoubleCounter(MetricName name);

    /**
     * Gets the value of an int64_t gauge and throws an exception if it is not found.
     */
    int64_t readInt64Gauge(MetricName name);

    /**
     * Gets the value of a double gauge and throws an exception if it is not found.
     */
    double readDoubleGauge(MetricName name);

    /**
     * Gets the data of an int64_t histogram and throws an exception if it is not found.
     */
    HistogramData<int64_t> readInt64Histogram(MetricName name);

    /**
     * Gets the data of a double histogram and throws an exception if it is not found.
     */
    HistogramData<double> readDoubleHistogram(MetricName name);

    /**
     * Returns whether it is safe to use the above "read" methods in the current environment. This
     * is required for tests that should also run on Windows because metrics can be recorded but
     * cannot be exported or read on Windows, and attempting to read them will crash.
     */
    static bool canReadMetrics();

private:
#if MONGO_CONFIG_OTEL
    // Gets a specific opentelemetry Instrument type based on the instrument's name. The MetricType
    // must be a PointType as defined here in the opentelemetry library:
    // https://github.com/open-telemetry/opentelemetry-cpp/blob/f0a1da286f3b130df1eb3db79ffc1ae427c9532b/sdk/include/opentelemetry/sdk/metrics/data/metric_data.h#L21
    template <typename DataType>
    DataType getMetricData(MetricName name) {
        _metrics->Clear();
        _reader->triggerMetricExport();

        const opentelemetry::exporter::memory::SimpleAggregateInMemoryMetricData::AttributeToPoint&
            attributeToPoint =
                _metrics->Get(std::string(toStdStringViewForInterop(MetricsService::kMeterName)),
                              std::string(toStdStringViewForInterop(name.getName())));
        auto it = attributeToPoint.find({});
        massert(ErrorCodes::KeyNotFound,
                fmt::format("No metric with name {} exists", name.getName()),
                it != attributeToPoint.end());

        massert(ErrorCodes::TypeMismatch,
                fmt::format("Metric {} does not have matching metric type", name.getName()),
                std::holds_alternative<DataType>(it->second));
        return std::get<DataType>(it->second);
    }

    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagOtelMetrics", true};

    // Stash the reader so that callers can trigger on-demand metric collection.
    test_util_detail::OnDemandMetricReader* _reader;
    // This is the in-memory data structure that holds the collected metrics. The exporter writes to
    // this DS, and the get() function will read from it.
    opentelemetry::exporter::memory::SimpleAggregateInMemoryMetricData* _metrics;
#endif  // MONGO_CONFIG_OTEL
};

}  // namespace mongo::otel::metrics

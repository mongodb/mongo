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

#include <opentelemetry/exporters/memory/in_memory_data.h>
#include <opentelemetry/exporters/memory/in_memory_metric_data.h>
#include <opentelemetry/exporters/memory/in_memory_metric_exporter_factory.h>
#include <opentelemetry/metrics/noop.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/metrics/metric_reader.h>

namespace mongo::otel::metrics {

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

/**
 * Class used for reading the data of an OpenTelemtry histogram.
 */
template <typename T>
class HistogramData {
public:
    HistogramData(opentelemetry::sdk::metrics::HistogramPointData data);

    /**
     * These values denote the upper and lower bounds for the histogram buckets.
     *
     * Bucket upper-bounds are inclusive (except when the upper-bound is +inf), and bucket
     * lower-bounds are exclusive. The implicit first boundary is -inf and the implicit last
     * boundary is +inf. Given a list of n boundaries, there are n + 1 buckets. For example,
     *
     * boundaries = {2, 4}
     * buckets = (-inf, 2], (2, 4], (4, +inf)
     *
     * If, for example, the value 2 is recorded, the corresponding `counts` vector is as follows:
     * {1, 0, 0}.
     *
     * See https://opentelemetry.io/docs/specs/otel/metrics/data-model/#histogram for more
     * information.
     */
    std::vector<double> boundaries;
    T sum;
    T min;
    T max;
    std::vector<uint64_t> counts;
    uint64_t count;

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
};

template <typename T>
HistogramData<T>::HistogramData(opentelemetry::sdk::metrics::HistogramPointData data)
    : boundaries(data.boundaries_), counts(data.counts_), count(data.count_) {
    sum = getValue(data.sum_);
    min = getValue(data.min_);
    max = getValue(data.max_);
};

/**
 * Sets up a MetricProvider with an in-memory exporter so tests can create and inspect metrics.
 * This must be constructed before creating any metrics in order to capture them.
 */
class MONGO_MOD_PUBLIC OtelMetricsCapturer {
public:
    OtelMetricsCapturer();

    ~OtelMetricsCapturer() {
        opentelemetry::metrics::Provider::SetMeterProvider(
            opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider>(
                new opentelemetry::metrics::NoopMeterProvider()));
    }

    /**
     * Gets the value of an int64_t counter and throws an exception if it is not found.
     */
    int64_t readInt64Counter(MetricName name);

    /**
     * Gets the data of an int64_t histogram and throws an exception if it is not found.
     */
    HistogramData<int64_t> readInt64Histogram(MetricName name);

    /**
     * Gets the data of an double histogram and throws an exception if it is not found.
     */
    HistogramData<double> readDoubleHistogram(MetricName name);

private:
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagOtelMetrics", true};

    // Stash the reader so that callers can trigger on-demand metric collection.
    test_util_detail::OnDemandMetricReader* _reader;
    // This is the in-memory data structure that holds the collected metrics. The exporter writes to
    // this DS, and the get() function will read from it.
    opentelemetry::exporter::memory::SimpleAggregateInMemoryMetricData* _metrics;
};

}  // namespace mongo::otel::metrics

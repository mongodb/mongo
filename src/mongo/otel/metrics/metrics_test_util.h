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

    // Gets the value of an Int64 counter, and throws an exception if it is not found.
    int64_t readInt64Counter(StringData name);

private:
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagOtelMetrics", true};

    // Stash the reader so that callers can trigger on-demand metric collection.
    test_util_detail::OnDemandMetricReader* _reader;
    // This is the in-memory data structure that holds the collected metrics. The exporter writes to
    // this DS, and the get() function will read from it.
    opentelemetry::exporter::memory::SimpleAggregateInMemoryMetricData* _metrics;
};

}  // namespace mongo::otel::metrics

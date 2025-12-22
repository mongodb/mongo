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
using opentelemetry::exporter::memory::SimpleAggregateInMemoryMetricData;
using opentelemetry::sdk::metrics::SumPointData;

OtelMetricsCapturer::OtelMetricsCapturer() {
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
    opentelemetry::metrics::Provider::SetMeterProvider(std::move(provider));
}

int64_t OtelMetricsCapturer::readInt64Counter(MetricName name) {
    _metrics->Clear();
    _reader->triggerMetricExport();

    const SimpleAggregateInMemoryMetricData::AttributeToPoint& attributeToPoint =
        _metrics->Get(std::string(toStdStringViewForInterop(MetricsService::kMeterName)),
                      std::string(toStdStringViewForInterop(name.getName())));
    auto it = attributeToPoint.find({});
    massert(ErrorCodes::KeyNotFound,
            fmt::format("No metric with name {} exists", name.getName()),
            it != attributeToPoint.end());

    massert(ErrorCodes::TypeMismatch,
            fmt::format("Metric {} does not have counter values", name.getName()),
            std::holds_alternative<SumPointData>(it->second));

    const SumPointData& sumPointData = std::get<SumPointData>(it->second);
    massert(ErrorCodes::TypeMismatch,
            fmt::format("Metric {} has non-int64_t value", name.getName()),
            std::holds_alternative<int64_t>(sumPointData.value_));

    return std::get<int64_t>(sumPointData.value_);
}
}  // namespace mongo::otel::metrics

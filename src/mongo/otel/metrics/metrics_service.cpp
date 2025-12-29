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


#include "mongo/otel/metrics/metrics_service.h"

#include <fmt/format.h>

namespace mongo::otel::metrics {

namespace {
const auto& getMetricsService = ServiceContext::declareDecoration<MetricsService>();
}  // namespace

#ifdef MONGO_CONFIG_OTEL
namespace {

// Static callback function for observable counters.
void observableCounterCallback(opentelemetry::metrics::ObserverResult observer_result,
                               void* state) {
    invariant(state != nullptr);
    auto* const counter = static_cast<Counter<int64_t>*>(state);
    int64_t value = counter->value();

    auto observer = std::get_if<std::shared_ptr<opentelemetry::metrics::ObserverResultT<int64_t>>>(
        &observer_result);
    invariant(observer != nullptr && *observer != nullptr);
    (*observer)->Observe(value);
}
}  // namespace

template <typename T>
T* MetricsService::getDuplicateMetric(WithLock,
                                      const std::string& name,
                                      MetricIdentifier identifier) {
    if (auto it = _metrics.find(name); it != _metrics.end()) {
        massert(ErrorCodes::ObjectAlreadyExists,
                fmt::format("Tried to create a metric with the name: {} but different definition "
                            "already exists.",
                            name),
                it->second.identifier == identifier);
        massert(ErrorCodes::ObjectAlreadyExists,
                "Tried to create a new metric, but a metric with a different type parameter "
                "already exists.",
                std::holds_alternative<std::unique_ptr<T>>(it->second.metric));

        return std::get<std::unique_ptr<T>>(it->second.metric).get();
    }
    return nullptr;
}

Counter<int64_t>* MetricsService::createInt64Counter(MetricName name,
                                                     std::string description,
                                                     MetricUnit unit) {
    std::string nameStr{name.getName()};
    MetricIdentifier identifier{.description = description, .unit = unit};
    stdx::lock_guard lock(_mutex);
    auto duplicate = getDuplicateMetric<Counter<int64_t>>(lock, nameStr, identifier);
    if (duplicate) {
        return duplicate;
    }

    // Make the raw counter.
    auto counter = std::make_unique<CounterImpl<int64_t>>(
        std::string(nameStr), description, std::string(toString(unit)));
    Counter<int64_t>* const counter_ptr = counter.get();
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(counter)};

    // Observe the raw counter.
    std::shared_ptr<opentelemetry::metrics::ObservableInstrument> observableCounter =
        opentelemetry::metrics::Provider::GetMeterProvider()
            ->GetMeter(std::string{kMeterName})
            ->CreateInt64ObservableCounter(toStdStringViewForInterop(nameStr),
                                           description,
                                           toStdStringViewForInterop(toString(unit)));
    tassert(ErrorCodes::InternalError,
            fmt::format("Could not create observable counter for metric: {}", nameStr),
            observableCounter != nullptr);
    observableCounter->AddCallback(observableCounterCallback, counter_ptr);
    _observableInstruments.push_back(std::move(observableCounter));

    return counter_ptr;
}

Histogram<double>* MetricsService::createDoubleHistogram(MetricName name,
                                                         std::string description,
                                                         MetricUnit unit) {
    std::string nameStr(name.getName());
    MetricIdentifier identifier{.description = description, .unit = unit};
    stdx::lock_guard lock(_mutex);
    auto duplicate = getDuplicateMetric<Histogram<double>>(lock, nameStr, identifier);
    if (duplicate) {
        return duplicate;
    }

    auto histogram =
        std::make_unique<HistogramImpl<double>>(opentelemetry::metrics::Provider::GetMeterProvider()
                                                    ->GetMeter(std::string{kMeterName})
                                                    .get(),
                                                nameStr,
                                                description,
                                                std::string(toString(unit)));
    Histogram<double>* const histogram_ptr = histogram.get();
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(histogram)};

    return histogram_ptr;
}

Histogram<int64_t>* MetricsService::createInt64Histogram(MetricName name,
                                                         std::string description,
                                                         MetricUnit unit) {
    std::string nameStr(name.getName());
    MetricIdentifier identifier{.description = description, .unit = unit};
    stdx::lock_guard lock(_mutex);
    auto duplicate = getDuplicateMetric<Histogram<int64_t>>(lock, nameStr, identifier);
    if (duplicate) {
        return duplicate;
    }

    auto histogram = std::make_unique<HistogramImpl<int64_t>>(
        opentelemetry::metrics::Provider::GetMeterProvider()
            ->GetMeter(std::string{kMeterName})
            .get(),
        nameStr,
        description,
        std::string(toString(unit)));
    Histogram<int64_t>* const histogram_ptr = histogram.get();
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(histogram)};

    return histogram_ptr;
}

BSONObj MetricsService::serializeMetrics() const {
    BSONObjBuilder builder;
    stdx::lock_guard lock(_mutex);
    for (const auto& [name, identifierAndMetric] : _metrics) {
        std::visit(
            [&](const auto& metric) {
                builder.append("otelMetrics",
                               metric->serializeToBson(fmt::format(
                                   "{}_{}", name, toString(identifierAndMetric.identifier.unit))));
            },
            identifierAndMetric.metric);
    }
    return builder.obj();
}
#else
Counter<int64_t>* MetricsService::createInt64Counter(MetricName name,
                                                     std::string description,
                                                     MetricUnit unit) {
    stdx::lock_guard lock(_mutex);
    _metrics.push_back(std::make_unique<CounterImpl<int64_t>>(
        std::string(name.getName()), std::move(description), std::string(toString(unit))));
    auto ptr = dynamic_cast<Counter<int64_t>*>(_metrics.back().get());
    invariant(ptr);
    return ptr;
}

Histogram<int64_t>* MetricsService::createInt64Histogram(MetricName name,
                                                         std::string description,
                                                         MetricUnit unit) {
    stdx::lock_guard lock(_mutex);
    _metrics.push_back(std::make_unique<NoopHistogramImpl<int64_t>>());
    auto ptr = dynamic_cast<Histogram<int64_t>*>(_metrics.back().get());
    invariant(ptr);
    return ptr;
}

Histogram<double>* MetricsService::createDoubleHistogram(MetricName name,
                                                         std::string description,
                                                         MetricUnit unit) {
    stdx::lock_guard lock(_mutex);
    _metrics.push_back(std::make_unique<NoopHistogramImpl<double>>());
    auto ptr = dynamic_cast<Histogram<double>*>(_metrics.back().get());
    invariant(ptr);
    return ptr;
}
#endif

MetricsService& MetricsService::get(ServiceContext* serviceContext) {
    return getMetricsService(serviceContext);
}
}  // namespace mongo::otel::metrics

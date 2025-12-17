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

Counter<int64_t>* MetricsService::createInt64Counter(std::string name,
                                                     std::string description,
                                                     MetricUnit unit) {
    MetricIdentifier identifier{
        .name = name, .description = description, .unit = unit, .type = MetricType::kCounter};
    stdx::lock_guard lock(_mutex);
    if (auto it = _metrics.find(name); it != _metrics.end()) {
        massert(ErrorCodes::ObjectAlreadyExists,
                fmt::format("Tried to create a metric with the name: {} but different definition "
                            "already exists.",
                            name),
                it->second.identifier == identifier);
        return it->second.metric.get();
    }

    // Make the raw counter.
    auto counter =
        std::make_unique<CounterImpl<int64_t>>(name, description, std::string(toString(unit)));
    Counter<int64_t>* const counter_ptr = counter.get();
    _metrics[name] = {.identifier = std::move(identifier), .metric = std::move(counter)};

    // Observe the raw counter.
    std::shared_ptr<opentelemetry::metrics::ObservableInstrument> observableCounter =
        opentelemetry::metrics::Provider::GetMeterProvider()
            ->GetMeter(std::string{kMeterName})
            ->CreateInt64ObservableCounter(
                name, description, toStdStringViewForInterop(toString(unit)));
    tassert(ErrorCodes::InternalError,
            fmt::format("Could not create observable counter for metric: {}", name),
            observableCounter != nullptr);
    observableCounter->AddCallback(observableCounterCallback, counter_ptr);
    _observableInstruments.push_back(std::move(observableCounter));

    return counter_ptr;
}
#else
Counter<int64_t>* MetricsService::createInt64Counter(std::string name,
                                                     std::string description,
                                                     MetricUnit unit) {
    stdx::lock_guard lock(_mutex);
    _counters.push_back(std::make_unique<CounterImpl<int64_t>>(
        std::move(name), std::move(description), std::string(toString(unit))));
    return _counters.back().get();
}
#endif

MetricsService& MetricsService::get(ServiceContext* serviceContext) {
    return getMetricsService(serviceContext);
}

}  // namespace mongo::otel::metrics


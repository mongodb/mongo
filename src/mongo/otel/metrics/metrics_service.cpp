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

#include "mongo/util/assert_util.h"

#include <fmt/format.h>
#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/view/instrument_selector.h>
#include <opentelemetry/sdk/metrics/view/meter_selector.h>
#include <opentelemetry/sdk/metrics/view/view.h>
#endif

namespace mongo::otel::metrics {

namespace {
const auto& getMetricsService = ServiceContext::declareDecoration<MetricsService>();
}  // namespace

#ifdef MONGO_CONFIG_OTEL
namespace {

// Static callback function for observable counters.
template <typename T>
void observableCounterCallback(opentelemetry::metrics::ObserverResult observer_result,
                               void* state) {
    invariant(state != nullptr);
    auto* const counter = static_cast<Counter<T>*>(state);
    T value = counter->value();

    auto observer =
        std::get_if<std::shared_ptr<opentelemetry::metrics::ObserverResultT<T>>>(&observer_result);
    invariant(observer != nullptr && *observer != nullptr);
    (*observer)->Observe(value);
}

// Static callback function for observable gauges.
template <typename T>
void observableGaugeCallback(opentelemetry::metrics::ObserverResult observer_result, void* state) {
    invariant(state != nullptr);
    auto* const gauge = static_cast<Gauge<T>*>(state);
    T value = gauge->value();

    auto observer =
        std::get_if<std::shared_ptr<opentelemetry::metrics::ObserverResultT<T>>>(&observer_result);
    invariant(observer != nullptr && *observer != nullptr);
    (*observer)->Observe(value);
}

// Creates a view with the provided aggregation type and aggregation configuration.
void createView(WithLock,
                const std::string& metricName,
                const std::string& description,
                const std::string& unit,
                opentelemetry::sdk::metrics::AggregationType aggregationType,
                std::unique_ptr<opentelemetry::sdk::metrics::AggregationConfig> aggregationConfig) {
    auto instrumentSelector = std::make_unique<opentelemetry::sdk::metrics::InstrumentSelector>(
        opentelemetry::sdk::metrics::InstrumentType::kHistogram, metricName, unit);
    auto meterSelector = std::make_unique<opentelemetry::sdk::metrics::MeterSelector>(
        std::string(MetricsService::kMeterName), "", "");
    auto view = std::make_unique<opentelemetry::sdk::metrics::View>(
        metricName, description, aggregationType, std::move(aggregationConfig));
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    // WARNING: This AddView function call is not thread safe, hence the WithLock parameter.
    std::static_pointer_cast<opentelemetry::sdk::metrics::MeterProvider>(provider)->AddView(
        std::move(instrumentSelector), std::move(meterSelector), std::move(view));
}

// Creates a view for a histogram with explicit bucket boundaries.
void createHistogramView(WithLock lock,
                         const std::string& metricName,
                         const std::string& description,
                         const std::string& unit,
                         std::vector<double> explicitBucketBoundaries) {
    auto aggregationConfig =
        std::make_unique<opentelemetry::sdk::metrics::HistogramAggregationConfig>();
    aggregationConfig->boundaries_ = std::move(explicitBucketBoundaries);
    createView(lock,
               metricName,
               description,
               unit,
               opentelemetry::sdk::metrics::AggregationType::kHistogram,
               std::move(aggregationConfig));
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

template <typename T>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument> makeObservableInstrument(
    std::string name, std::string description, MetricUnit unit);

template <>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<Counter<int64_t>>(std::string name,
                                           std::string description,
                                           MetricUnit unit) {
    return opentelemetry::metrics::Provider::GetMeterProvider()
        ->GetMeter(std::string{MetricsService::kMeterName})
        ->CreateInt64ObservableCounter(toStdStringViewForInterop(name),
                                       description,
                                       toStdStringViewForInterop(toString(unit)));
}

template <>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<Counter<double>>(std::string name,
                                          std::string description,
                                          MetricUnit unit) {
    return opentelemetry::metrics::Provider::GetMeterProvider()
        ->GetMeter(std::string{MetricsService::kMeterName})
        ->CreateDoubleObservableCounter(toStdStringViewForInterop(name),
                                        description,
                                        toStdStringViewForInterop(toString(unit)));
}

template <>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<Gauge<int64_t>>(std::string name,
                                         std::string description,
                                         MetricUnit unit) {
    return opentelemetry::metrics::Provider::GetMeterProvider()
        ->GetMeter(std::string{MetricsService::kMeterName})
        ->CreateInt64ObservableGauge(toStdStringViewForInterop(name),
                                     description,
                                     toStdStringViewForInterop(toString(unit)));
}

template <>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<Gauge<double>>(std::string name,
                                        std::string description,
                                        MetricUnit unit) {
    return opentelemetry::metrics::Provider::GetMeterProvider()
        ->GetMeter(std::string{MetricsService::kMeterName})
        ->CreateDoubleObservableGauge(toStdStringViewForInterop(name),
                                      description,
                                      toStdStringViewForInterop(toString(unit)));
}

template <typename T>
Counter<T>* MetricsService::createCounter(MetricName name,
                                          std::string description,
                                          MetricUnit unit) {
    const std::string nameStr(name.getName());
    MetricIdentifier identifier{.description = description, .unit = unit};
    stdx::lock_guard lock(_mutex);
    auto duplicate = getDuplicateMetric<Counter<T>>(lock, nameStr, identifier);
    if (duplicate) {
        return duplicate;
    }

    // Make the raw counter.
    auto counter = std::make_unique<CounterImpl<T>>(
        std::string(nameStr), description, std::string(toString(unit)));
    Counter<T>* const counter_ptr = counter.get();
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(counter)};

    // Observe the raw counter.
    std::shared_ptr<opentelemetry::metrics::ObservableInstrument> observableCounter =
        makeObservableInstrument<Counter<T>>(nameStr, description, unit);

    tassert(ErrorCodes::InternalError,
            fmt::format("Could not create observable counter for metric: {}", nameStr),
            observableCounter != nullptr);
    observableCounter->AddCallback(observableCounterCallback<T>, counter_ptr);
    _observableInstruments.push_back(std::move(observableCounter));

    return counter_ptr;
}

Counter<int64_t>* MetricsService::createInt64Counter(MetricName name,
                                                     std::string description,
                                                     MetricUnit unit) {
    return createCounter<int64_t>(name, description, unit);
}

Counter<double>* MetricsService::createDoubleCounter(MetricName name,
                                                     std::string description,
                                                     MetricUnit unit) {
    return createCounter<double>(name, description, unit);
}

template <typename T>
Gauge<T>* MetricsService::createGauge(MetricName name, std::string description, MetricUnit unit) {
    std::string nameStr(name.getName());
    MetricIdentifier identifier{.description = description, .unit = unit};
    stdx::lock_guard lock(_mutex);
    auto duplicate = getDuplicateMetric<Gauge<T>>(lock, nameStr, identifier);
    if (duplicate) {
        return duplicate;
    }

    // Make the raw gauge.
    auto gauge = std::make_unique<GaugeImpl<T>>();
    Gauge<T>* const gauge_ptr = gauge.get();
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(gauge)};

    // Observe the raw gauge.
    std::shared_ptr<opentelemetry::metrics::ObservableInstrument> observableGauge =
        makeObservableInstrument<Gauge<T>>(nameStr, description, unit);
    tassert(ErrorCodes::InternalError,
            fmt::format("Could not create observable gauge for metric: {}", nameStr),
            observableGauge != nullptr);
    observableGauge->AddCallback(observableGaugeCallback<T>, gauge_ptr);
    _observableInstruments.push_back(std::move(observableGauge));

    return gauge_ptr;
}

Gauge<int64_t>* MetricsService::createInt64Gauge(MetricName name,
                                                 std::string description,
                                                 MetricUnit unit) {
    return createGauge<int64_t>(name, description, unit);
}

Gauge<double>* MetricsService::createDoubleGauge(MetricName name,
                                                 std::string description,
                                                 MetricUnit unit) {
    return createGauge<double>(name, description, unit);
}

Histogram<double>* MetricsService::createDoubleHistogram(
    MetricName name,
    std::string description,
    MetricUnit unit,
    boost::optional<std::vector<double>> explicitBucketBoundaries) {
    std::string nameStr(name.getName());
    MetricIdentifier identifier{.description = description, .unit = unit};
    stdx::lock_guard lock(_mutex);
    auto duplicate = getDuplicateMetric<Histogram<double>>(lock, nameStr, identifier);
    if (duplicate) {
        return duplicate;
    }

    const std::string unitStr(toString(unit));
    if (explicitBucketBoundaries.has_value()) {
        createHistogramView(
            lock, nameStr, description, unitStr, std::move(explicitBucketBoundaries.value()));
    }

    auto histogram =
        std::make_unique<HistogramImpl<double>>(opentelemetry::metrics::Provider::GetMeterProvider()
                                                    ->GetMeter(std::string{kMeterName})
                                                    .get(),
                                                nameStr,
                                                description,
                                                unitStr);
    Histogram<double>* const histogram_ptr = histogram.get();
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(histogram)};

    return histogram_ptr;
}

Histogram<int64_t>* MetricsService::createInt64Histogram(
    MetricName name,
    std::string description,
    MetricUnit unit,
    boost::optional<std::vector<double>> explicitBucketBoundaries) {
    const std::string nameStr(name.getName());
    MetricIdentifier identifier{.description = description, .unit = unit};
    stdx::lock_guard lock(_mutex);
    auto duplicate = getDuplicateMetric<Histogram<int64_t>>(lock, nameStr, identifier);
    if (duplicate) {
        return duplicate;
    }

    const std::string unitStr(toString(unit));
    if (explicitBucketBoundaries.has_value()) {
        createHistogramView(
            lock, nameStr, description, unitStr, std::move(explicitBucketBoundaries.value()));
    }

    auto histogram = std::make_unique<HistogramImpl<int64_t>>(
        opentelemetry::metrics::Provider::GetMeterProvider()
            ->GetMeter(std::string{kMeterName})
            .get(),
        nameStr,
        description,
        unitStr);
    Histogram<int64_t>* const histogram_ptr = histogram.get();
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(histogram)};

    return histogram_ptr;
}

BSONObj MetricsService::serializeMetrics() const {
    BSONObjBuilder builder;
    BSONObjBuilder otelMetrics(builder.subobjStart("otelMetrics"));
    stdx::lock_guard lock(_mutex);
    for (const auto& [name, identifierAndMetric] : _metrics) {
        std::visit(
            [&](const auto& metric) {
                const auto key =
                    fmt::format("{}_{}", name, toString(identifierAndMetric.identifier.unit));
                const auto obj = metric->serializeToBson(key);
                massert(ErrorCodes::KeyNotFound,
                        fmt::format("Provided key {} not found in serialized BSONObj", key),
                        !obj.getField(key).eoo());
                otelMetrics.append(obj.getField(key));
            },
            identifierAndMetric.metric);
    }
    otelMetrics.doneFast();
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

Counter<double>* MetricsService::createDoubleCounter(MetricName name,
                                                     std::string description,
                                                     MetricUnit unit) {
    stdx::lock_guard lock(_mutex);
    _metrics.push_back(std::make_unique<CounterImpl<double>>(
        std::string(name.getName()), std::move(description), std::string(toString(unit))));
    auto ptr = dynamic_cast<Counter<double>*>(_metrics.back().get());
    invariant(ptr);
    return ptr;
}

Gauge<int64_t>* MetricsService::createInt64Gauge(MetricName name,
                                                 std::string description,
                                                 MetricUnit unit) {
    stdx::lock_guard lock(_mutex);
    _metrics.push_back(std::make_unique<GaugeImpl<int64_t>>());
    auto ptr = dynamic_cast<Gauge<int64_t>*>(_metrics.back().get());
    invariant(ptr);
    return ptr;
}

Gauge<double>* MetricsService::createDoubleGauge(MetricName name,
                                                 std::string description,
                                                 MetricUnit unit) {
    stdx::lock_guard lock(_mutex);
    _metrics.push_back(std::make_unique<GaugeImpl<double>>());
    auto ptr = dynamic_cast<Gauge<double>*>(_metrics.back().get());
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

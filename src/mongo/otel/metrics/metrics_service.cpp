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

#include "mongo/otel/metrics/metrics_initialization.h"
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
                opentelemetry::metrics::MeterProvider* provider,
                const std::string& metricName,
                const std::string& description,
                const std::string& unit,
                opentelemetry::sdk::metrics::AggregationType aggregationType,
                std::unique_ptr<opentelemetry::sdk::metrics::AggregationConfig> aggregationConfig) {
    auto* sdkProvider = dynamic_cast<opentelemetry::sdk::metrics::MeterProvider*>(provider);
    if (sdkProvider == nullptr) {
        // MeterProvider is no-op; initialize must be called first.
        return;
    }
    auto instrumentSelector = std::make_unique<opentelemetry::sdk::metrics::InstrumentSelector>(
        opentelemetry::sdk::metrics::InstrumentType::kHistogram, metricName, unit);
    auto meterSelector = std::make_unique<opentelemetry::sdk::metrics::MeterSelector>(
        std::string(MetricsService::kMeterName), "", "");
    auto view = std::make_unique<opentelemetry::sdk::metrics::View>(
        metricName, description, aggregationType, std::move(aggregationConfig));
    // WARNING: This AddView function call is not thread safe, hence the WithLock parameter.
    sdkProvider->AddView(std::move(instrumentSelector), std::move(meterSelector), std::move(view));
}

// Creates a view for a histogram with explicit bucket boundaries and registers the view with the
// provided MeterProvider.
void createAndRegisterHistogramView(WithLock lock,
                                    opentelemetry::metrics::MeterProvider* provider,
                                    const std::string& metricName,
                                    const std::string& description,
                                    const std::string& unit,
                                    std::vector<double> explicitBucketBoundaries) {
    auto aggregationConfig =
        std::make_unique<opentelemetry::sdk::metrics::HistogramAggregationConfig>();
    aggregationConfig->boundaries_ = std::move(explicitBucketBoundaries);
    createView(lock,
               provider,
               metricName,
               description,
               unit,
               opentelemetry::sdk::metrics::AggregationType::kHistogram,
               std::move(aggregationConfig));
}

template <typename T>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument> makeObservableInstrument(
    opentelemetry::metrics::MeterProvider& provider,
    std::string name,
    std::string description,
    MetricUnit unit);

template <>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<Counter<int64_t>>(opentelemetry::metrics::MeterProvider& provider,
                                           std::string name,
                                           std::string description,
                                           MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateInt64ObservableCounter(toStdStringViewForInterop(name),
                                       description,
                                       toStdStringViewForInterop(toString(unit)));
}

template <>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<Counter<double>>(opentelemetry::metrics::MeterProvider& provider,
                                          std::string name,
                                          std::string description,
                                          MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateDoubleObservableCounter(toStdStringViewForInterop(name),
                                        description,
                                        toStdStringViewForInterop(toString(unit)));
}

template <>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<Gauge<int64_t>>(opentelemetry::metrics::MeterProvider& provider,
                                         std::string name,
                                         std::string description,
                                         MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateInt64ObservableGauge(toStdStringViewForInterop(name),
                                     description,
                                     toStdStringViewForInterop(toString(unit)));
}

template <>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<Gauge<double>>(opentelemetry::metrics::MeterProvider& provider,
                                        std::string name,
                                        std::string description,
                                        MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateDoubleObservableGauge(toStdStringViewForInterop(name),
                                      description,
                                      toStdStringViewForInterop(toString(unit)));
}

template <typename T>
std::unique_ptr<T> makeHistogram(WithLock lock,
                                 opentelemetry::metrics::MeterProvider& provider,
                                 std::string name,
                                 std::string description,
                                 std::string unit,
                                 boost::optional<std::vector<double>> explicitBucketBoundaries) {
    if (explicitBucketBoundaries.has_value()) {
        createAndRegisterHistogramView(
            lock, &provider, name, description, unit, explicitBucketBoundaries.value());
    }
    return std::make_unique<T>(*provider.GetMeter(std::string{MetricsService::kMeterName}),
                               name,
                               description,
                               unit,
                               explicitBucketBoundaries);
}
}  // namespace

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<Counter<int64_t>>& counter) {
    counter->reset();
    auto observable =
        makeObservableInstrument<Counter<int64_t>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCounterCallback<int64_t>, counter.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<Counter<double>>& counter) {
    counter->reset();
    auto observable =
        makeObservableInstrument<Counter<double>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCounterCallback<double>, counter.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<Gauge<int64_t>>& gauge) {
    gauge->reset();
    auto observable =
        makeObservableInstrument<Gauge<int64_t>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableGaugeCallback<int64_t>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<Gauge<double>>& gauge) {
    gauge->reset();
    auto observable =
        makeObservableInstrument<Gauge<double>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableGaugeCallback<double>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<Histogram<double>>& histogram) {
    auto* histogramImpl = dynamic_cast<HistogramImpl<double>*>(histogram.get());
    invariant(histogramImpl);
    if (histogramImpl->explicitBucketBoundaries.has_value()) {
        createAndRegisterHistogramView(lock,
                                       &provider,
                                       name,
                                       id.description,
                                       std::string(toString(id.unit)),
                                       histogramImpl->explicitBucketBoundaries.value());
    }
    histogram->reset(provider.GetMeter(std::string(MetricsService::kMeterName)).get());
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<Histogram<int64_t>>& histogram) {
    auto* histogramImpl = dynamic_cast<HistogramImpl<int64_t>*>(histogram.get());
    invariant(histogramImpl);
    if (histogramImpl->explicitBucketBoundaries.has_value()) {
        createAndRegisterHistogramView(lock,
                                       &provider,
                                       name,
                                       id.description,
                                       std::string(toString(id.unit)),
                                       histogramImpl->explicitBucketBoundaries.value());
    }
    histogram->reset(provider.GetMeter(std::string(MetricsService::kMeterName)).get());
}

void MetricsService::initialize(opentelemetry::metrics::MeterProvider& provider) {
    std::lock_guard lock(_mutex);
    std::vector<std::shared_ptr<opentelemetry::metrics::ObservableInstrument>>
        newObservableInstruments;

    for (auto& [name, identifierAndMetric] : _metrics) {
        std::visit(OwnedMetricVisitor{.lock = lock,
                                      .provider = provider,
                                      .name = name,
                                      .id = identifierAndMetric.identifier,
                                      .newObservableInstruments = newObservableInstruments},
                   identifierAndMetric.metric);
    }

    // Re-assign the observable instruments vector stored in MetricsService, implicitly invoking the
    // destructors of any observable instrument instance(s) created before initialization.
    _observableInstruments = std::move(newObservableInstruments);
}
#endif  // MONGO_CONFIG_OTEL

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
    auto counter = std::make_unique<CounterImpl<T>>();
    Counter<T>* const counter_ptr = counter.get();
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(counter)};

#ifdef MONGO_CONFIG_OTEL
    // Observe the raw counter.
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    std::shared_ptr<opentelemetry::metrics::ObservableInstrument> observableCounter =
        makeObservableInstrument<Counter<T>>(*provider, nameStr, description, unit);
    tassert(ErrorCodes::InternalError,
            fmt::format("Could not create observable counter for metric: {}", nameStr),
            observableCounter != nullptr);
    observableCounter->AddCallback(observableCounterCallback<T>, counter_ptr);
    _observableInstruments.push_back(std::move(observableCounter));
#endif  // MONGO_CONFIG_OTEL

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

#ifdef MONGO_CONFIG_OTEL
    // Observe the raw gauge.
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    std::shared_ptr<opentelemetry::metrics::ObservableInstrument> observableGauge =
        makeObservableInstrument<Gauge<T>>(*provider, nameStr, description, unit);
    tassert(ErrorCodes::InternalError,
            fmt::format("Could not create observable gauge for metric: {}", nameStr),
            observableGauge != nullptr);
    observableGauge->AddCallback(observableGaugeCallback<T>, gauge_ptr);
    _observableInstruments.push_back(std::move(observableGauge));
#endif  // MONGO_CONFIG_OTEL

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

#ifdef MONGO_CONFIG_OTEL
    auto histogram =
        makeHistogram<HistogramImpl<double>>(lock,
                                             *opentelemetry::metrics::Provider::GetMeterProvider(),
                                             nameStr,
                                             description,
                                             std::string(toString(unit)),
                                             std::move(explicitBucketBoundaries));
#else
    auto histogram = std::make_unique<HistogramImpl<double>>();
#endif  // MONGO_CONFIG_OTEL
    auto* histogram_ptr = histogram.get();
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
#ifdef MONGO_CONFIG_OTEL
    auto histogram =
        makeHistogram<HistogramImpl<int64_t>>(lock,
                                              *opentelemetry::metrics::Provider::GetMeterProvider(),
                                              nameStr,
                                              description,
                                              std::string(toString(unit)),
                                              std::move(explicitBucketBoundaries));
#else
    auto histogram = std::make_unique<HistogramImpl<int64_t>>();
#endif  // MONGO_CONFIG_OTEL
    auto* histogram_ptr = histogram.get();
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(histogram)};

    return histogram_ptr;
}

void MetricsService::appendMetricsForServerStatus(BSONObjBuilder& bsonBuilder) const {
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
                bsonBuilder.append(obj.getField(key));
            },
            identifierAndMetric.metric);
    }
}
}  // namespace mongo::otel::metrics

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

#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/otel/metrics/metrics_initialization.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/otel/metrics/otel_metric_name_validation.h"
#include "mongo/otel/metrics/otel_metric_server_status_adapter.h"
#include "mongo/otel/metrics/server_status_metric_name_validation.h"
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

// Static callback function for metric types that are observable.
template <template <typename> class MetricT, typename ValueT>
void observableCallback(opentelemetry::metrics::ObserverResult observer_result, void* state) {
    invariant(state != nullptr);
    auto* const metric = static_cast<MetricT<ValueT>*>(state);
    auto observer = std::get_if<std::shared_ptr<opentelemetry::metrics::ObserverResultT<ValueT>>>(
        &observer_result);
    invariant(observer != nullptr && *observer != nullptr);
    for (const auto& entry : metric->values()) {
        (*observer)->Observe(entry.value, entry.attributes);
    }
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
makeObservableInstrument<UpDownCounter<int64_t>>(opentelemetry::metrics::MeterProvider& provider,
                                                 std::string name,
                                                 std::string description,
                                                 MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateInt64ObservableUpDownCounter(toStdStringViewForInterop(name),
                                             description,
                                             toStdStringViewForInterop(toString(unit)));
}

template <>
std::shared_ptr<opentelemetry::metrics::ObservableInstrument>
makeObservableInstrument<UpDownCounter<double>>(opentelemetry::metrics::MeterProvider& provider,
                                                std::string name,
                                                std::string description,
                                                MetricUnit unit) {
    return provider.GetMeter(std::string{MetricsService::kMeterName})
        ->CreateDoubleObservableUpDownCounter(toStdStringViewForInterop(name),
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
    observable->AddCallback(observableCallback<Counter, int64_t>, counter.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<Counter<double>>& counter) {
    counter->reset();
    auto observable =
        makeObservableInstrument<Counter<double>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<Counter, double>, counter.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<UpDownCounter<int64_t>>& upDownCounter) {
    upDownCounter->reset();
    auto observable =
        makeObservableInstrument<UpDownCounter<int64_t>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<UpDownCounter, int64_t>, upDownCounter.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<UpDownCounter<double>>& upDownCounter) {
    upDownCounter->reset();
    auto observable =
        makeObservableInstrument<UpDownCounter<double>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<UpDownCounter, double>, upDownCounter.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<Gauge<int64_t>>& gauge) {
    gauge->reset();
    auto observable =
        makeObservableInstrument<Gauge<int64_t>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<Gauge, int64_t>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<Gauge<double>>& gauge) {
    gauge->reset();
    auto observable =
        makeObservableInstrument<Gauge<double>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<Gauge, double>, gauge.get());
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

void MetricsService::_registerServerStatusTree(
    WithLock, Metric* metricPtr, const boost::optional<ServerStatusOptions>& serverStatusOptions) {
    if (!serverStatusOptions.has_value()) {
        return;
    }
    globalMetricTreeSet()[serverStatusOptions->role].add(
        serverStatusOptions->dottedPath,
        std::make_unique<OtelMetricServerStatusAdapter>(metricPtr));
}

template <typename InstrumentT, typename Options>
InstrumentT& MetricsService::_createMetric(MetricName name,
                                           const Options& options,
                                           MetricIdentifier identifier,
                                           MakeInstrumentRef<InstrumentT> makeInstrument,
                                           AddObservableRef<InstrumentT> addObservable) {
    // Validate otel and serverStatus metric names.
    uassertStatusOK(validateOtelMetricName(name.getName()));
    if (options.serverStatusOptions) {
        uassertStatusOK(validateServerStatusMetricPath(options.serverStatusOptions->dottedPath));
    }

    const std::string nameStr(name.getName());

    stdx::lock_guard lock(_mutex);

    // Check for duplicate.
    if (auto* duplicate = getDuplicateMetric<InstrumentT>(lock, nameStr, identifier)) {
        return *duplicate;
    }

    // Register.
    auto impl = makeInstrument(lock, nameStr);
    InstrumentT* const ptr = impl.get();

    auto owned = OwnedMetric{std::unique_ptr<InstrumentT>(std::move(impl))};
    _metrics[nameStr] = {.identifier = std::move(identifier), .metric = std::move(owned)};
    _registerServerStatusTree(lock, static_cast<Metric*>(ptr), options.serverStatusOptions);
#ifdef MONGO_CONFIG_OTEL
    addObservable(lock, nameStr, ptr);
#endif  // MONGO_CONFIG_OTEL
    return *ptr;
}

template <typename T>
Counter<T>& MetricsService::createCounter(MetricName name,
                                          std::string description,
                                          MetricUnit unit,
                                          const CounterOptions& options) {
    MetricIdentifier identifier{.description = description,
                                .unit = unit,
                                .serverStatusOptions = options.serverStatusOptions,
                                .histogramBucketBoundaries = boost::none};
    return _createMetric<Counter<T>, CounterOptions>(
        name,
        options,
        std::move(identifier),
        /* makeInstrument= */
        [](WithLock, const std::string&) { return std::make_unique<CounterImpl<T>>(); },
#ifdef MONGO_CONFIG_OTEL
        /* addObservable= */
        [this, description, unit](
            WithLock lock, const std::string& nameStr, Counter<T>* counter_ptr) {
            (void)lock;
            auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
            std::shared_ptr<opentelemetry::metrics::ObservableInstrument> observableCounter =
                makeObservableInstrument<Counter<T>>(*provider, nameStr, description, unit);
            tassert(ErrorCodes::InternalError,
                    fmt::format("Could not create observable counter for metric: {}", nameStr),
                    observableCounter != nullptr);
            observableCounter->AddCallback(observableCallback<Counter, T>, counter_ptr);
            _observableInstruments.push_back(std::move(observableCounter));
        }
#else
        [](WithLock, const std::string&, Counter<T>*) {}
#endif
    );
}

Counter<int64_t>& MetricsService::createInt64Counter(MetricName name,
                                                     std::string description,
                                                     MetricUnit unit,
                                                     const CounterOptions& options) {
    return createCounter<int64_t>(name, description, unit, options);
}

Counter<double>& MetricsService::createDoubleCounter(MetricName name,
                                                     std::string description,
                                                     MetricUnit unit,
                                                     const CounterOptions& options) {
    return createCounter<double>(name, description, unit, options);
}

template <typename T>
UpDownCounter<T>& MetricsService::createUpDownCounter(MetricName name,
                                                      std::string description,
                                                      MetricUnit unit,
                                                      const UpDownCounterOptions& options) {
    MetricIdentifier identifier{.description = description,
                                .unit = unit,
                                .serverStatusOptions = options.serverStatusOptions,
                                .histogramBucketBoundaries = boost::none};
    return _createMetric<UpDownCounter<T>, UpDownCounterOptions>(
        name,
        options,
        std::move(identifier),
        /* makeInstrument= */
        [](WithLock, const std::string&) { return std::make_unique<UpDownCounterImpl<T>>(); },
#ifdef MONGO_CONFIG_OTEL
        /* addObservable= */
        [this, description, unit](
            WithLock lock, const std::string& nameStr, UpDownCounter<T>* upDownCounter_ptr) {
            (void)lock;
            auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
            std::shared_ptr<opentelemetry::metrics::ObservableInstrument> observableUpDownCounter =
                makeObservableInstrument<UpDownCounter<T>>(*provider, nameStr, description, unit);
            tassert(
                ErrorCodes::InternalError,
                fmt::format("Could not create observable up-down counter for metric: {}", nameStr),
                observableUpDownCounter != nullptr);
            observableUpDownCounter->AddCallback(observableCallback<UpDownCounter, T>,
                                                 upDownCounter_ptr);
            _observableInstruments.push_back(std::move(observableUpDownCounter));
        }
#else
        [](WithLock, const std::string&, UpDownCounter<T>*) {}
#endif
    );
}

UpDownCounter<int64_t>& MetricsService::createInt64UpDownCounter(
    MetricName name,
    std::string description,
    MetricUnit unit,
    const UpDownCounterOptions& options) {
    return createUpDownCounter<int64_t>(name, description, unit, options);
}

UpDownCounter<double>& MetricsService::createDoubleUpDownCounter(
    MetricName name,
    std::string description,
    MetricUnit unit,
    const UpDownCounterOptions& options) {
    return createUpDownCounter<double>(name, description, unit, options);
}

template <typename T>
Gauge<T>& MetricsService::createGauge(MetricName name,
                                      std::string description,
                                      MetricUnit unit,
                                      const GaugeOptions& options) {
    MetricIdentifier identifier{.description = description,
                                .unit = unit,
                                .serverStatusOptions = options.serverStatusOptions,
                                .histogramBucketBoundaries = boost::none};
    return _createMetric<Gauge<T>, GaugeOptions>(
        name,
        options,
        std::move(identifier),
        /* makeInstrument= */
        [](WithLock, const std::string&) { return std::make_unique<GaugeImpl<T>>(); },
#ifdef MONGO_CONFIG_OTEL
        /* addObservable= */
        [this, description, unit](WithLock lock, const std::string& nameStr, Gauge<T>* gauge_ptr) {
            (void)lock;
            auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
            std::shared_ptr<opentelemetry::metrics::ObservableInstrument> observableGauge =
                makeObservableInstrument<Gauge<T>>(*provider, nameStr, description, unit);
            tassert(ErrorCodes::InternalError,
                    fmt::format("Could not create observable gauge for metric: {}", nameStr),
                    observableGauge != nullptr);
            observableGauge->AddCallback(observableCallback<Gauge, T>, gauge_ptr);
            _observableInstruments.push_back(std::move(observableGauge));
        }
#else
        [](WithLock, const std::string&, Gauge<T>*) {}
#endif
    );
}

Gauge<int64_t>& MetricsService::createInt64Gauge(MetricName name,
                                                 std::string description,
                                                 MetricUnit unit,
                                                 const GaugeOptions& options) {
    return createGauge<int64_t>(name, description, unit, options);
}

Gauge<double>& MetricsService::createDoubleGauge(MetricName name,
                                                 std::string description,
                                                 MetricUnit unit,
                                                 const GaugeOptions& options) {
    return createGauge<double>(name, description, unit, options);
}

template <typename T>
Histogram<T>& MetricsService::createHistogram(MetricName name,
                                              std::string description,
                                              MetricUnit unit,
                                              const HistogramOptions& options) {
    const std::string unitStr = static_cast<std::string>(toString(unit));
    MetricIdentifier identifier{.description = description,
                                .unit = unit,
                                .serverStatusOptions = options.serverStatusOptions,
                                .histogramBucketBoundaries = options.explicitBucketBoundaries};
    return _createMetric<Histogram<T>, HistogramOptions>(
        name,
        options,
        std::move(identifier),
        /* makeInstrument= */
        [&](WithLock lock, const std::string& nameStr) -> std::unique_ptr<Histogram<T>> {
#ifdef MONGO_CONFIG_OTEL
            return makeHistogram<HistogramImpl<T>>(
                lock,
                *opentelemetry::metrics::Provider::GetMeterProvider(),
                nameStr,
                description,
                unitStr,
                options.explicitBucketBoundaries);
#else
            return std::make_unique<HistogramImpl<T>>();
#endif  // MONGO_CONFIG_OTEL
        },
        /* addObservable= */ [](WithLock, const std::string&, Histogram<T>*) {});
}

Histogram<double>& MetricsService::createDoubleHistogram(MetricName name,
                                                         std::string description,
                                                         MetricUnit unit,
                                                         const HistogramOptions& options) {
    return createHistogram<double>(name, description, unit, options);
}

Histogram<int64_t>& MetricsService::createInt64Histogram(MetricName name,
                                                         std::string description,
                                                         MetricUnit unit,
                                                         const HistogramOptions& options) {
    return createHistogram<int64_t>(name, description, unit, options);
}

void MetricsService::clearForTests() {
    stdx::lock_guard lock(_mutex);
#ifdef MONGO_CONFIG_OTEL
    _observableInstruments.clear();
#endif
    for (auto& [name, identAndMetric] : _metrics) {
        auto& opts = identAndMetric.identifier.serverStatusOptions;
        if (opts.has_value()) {
            globalMetricTreeSet()[opts->role].removeForTests(opts->dottedPath);
        }
    }
    _metrics.clear();
}
}  // namespace mongo::otel::metrics

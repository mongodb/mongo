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
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/otel/metrics/otel_metric_server_status_adapter.h"
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
using metrics_service_detail::makeObservableInstrument;
using metrics_service_detail::observableCallback;

namespace {

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

}  // namespace

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<ObservableCounter<int64_t>>& counter) {
    counter->reset();
    auto observable = makeObservableInstrument<ObservableCounter<int64_t>>(
        provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<ObservableCounter, int64_t>, counter.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<ObservableCounter<double>>& counter) {
    counter->reset();
    auto observable = makeObservableInstrument<ObservableCounter<double>>(
        provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<ObservableCounter, double>, counter.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<ObservableUpDownCounter<int64_t>>& upDownCounter) {
    upDownCounter->reset();
    auto observable = makeObservableInstrument<ObservableUpDownCounter<int64_t>>(
        provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<ObservableUpDownCounter, int64_t>,
                            upDownCounter.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<ObservableUpDownCounter<double>>& upDownCounter) {
    upDownCounter->reset();
    auto observable = makeObservableInstrument<ObservableUpDownCounter<double>>(
        provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<ObservableUpDownCounter, double>,
                            upDownCounter.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<ObservableGauge<int64_t>>& gauge) {
    gauge->reset();
    auto observable =
        makeObservableInstrument<ObservableGauge<int64_t>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<ObservableGauge, int64_t>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<ObservableGauge<double>>& gauge) {
    gauge->reset();
    auto observable =
        makeObservableInstrument<ObservableGauge<double>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<ObservableGauge, double>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<MinGauge<int64_t>>& gauge) {
    gauge->reset();
    auto observable =
        makeObservableInstrument<ObservableGauge<int64_t>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<MinGauge, int64_t>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<MinGauge<double>>& gauge) {
    gauge->reset();
    auto observable =
        makeObservableInstrument<ObservableGauge<double>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<MinGauge, double>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<MaxGauge<int64_t>>& gauge) {
    gauge->reset();
    auto observable =
        makeObservableInstrument<ObservableGauge<int64_t>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<MaxGauge, int64_t>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(std::unique_ptr<MaxGauge<double>>& gauge) {
    gauge->reset();
    auto observable =
        makeObservableInstrument<ObservableGauge<double>>(provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<MaxGauge, double>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<HistogramBase<double>>& histogram) {
    if (histogram->explicitBucketBoundaries.has_value()) {
        createAndRegisterHistogramView(lock,
                                       &provider,
                                       name,
                                       id.description,
                                       std::string(toString(id.unit)),
                                       histogram->explicitBucketBoundaries.value());
    }
    histogram->reset(provider.GetMeter(std::string(MetricsService::kMeterName)).get());
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<HistogramBase<int64_t>>& histogram) {
    if (histogram->explicitBucketBoundaries.has_value()) {
        createAndRegisterHistogramView(lock,
                                       &provider,
                                       name,
                                       id.description,
                                       std::string(toString(id.unit)),
                                       histogram->explicitBucketBoundaries.value());
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

void MetricsService::_registerHistogramView(
    WithLock lock,
    const std::string& name,
    const std::string& description,
    const std::string& unit,
    const boost::optional<std::vector<double>>& explicitBucketBoundaries) {
    if (explicitBucketBoundaries.has_value()) {
        createAndRegisterHistogramView(lock,
                                       opentelemetry::metrics::Provider::GetMeterProvider().get(),
                                       name,
                                       description,
                                       unit,
                                       explicitBucketBoundaries.value());
    }
}
#endif  // MONGO_CONFIG_OTEL

void MetricsService::_registerServerStatusTree(
    WithLock, Metric* metricPtr, const boost::optional<ServerStatusOptions>& serverStatusOptions) {
    if (!serverStatusOptions.has_value()) {
        return;
    }
    globalMetricTreeSet()[serverStatusOptions->role].add(
        serverStatusOptions->dottedPath,
        std::make_unique<OtelMetricServerStatusAdapter>(metricPtr));
}

UpDownCounter<int64_t>& MetricsService::createInt64UpDownCounter(
    MetricName name,
    std::string description,
    MetricUnit unit,
    const UpDownCounterOptions& options) {
    return _createScalarMetric<ObservableUpDownCounter, int64_t>(
        name, std::move(description), unit, options);
}

UpDownCounter<double>& MetricsService::createDoubleUpDownCounter(
    MetricName name,
    std::string description,
    MetricUnit unit,
    const UpDownCounterOptions& options) {
    return _createScalarMetric<ObservableUpDownCounter, double>(
        name, std::move(description), unit, options);
}

template <template <typename> class GaugeTpl, typename T>
GaugeTpl<T>& MetricsService::createGaugeBase(MetricName name,
                                             std::string description,
                                             MetricUnit unit,
                                             const GaugeOptions& options,
                                             T initialValue) {
    MetricIdentifier identifier{.description = description,
                                .unit = unit,
                                .serverStatusOptions = options.serverStatusOptions,
                                .histogramBucketBoundaries = boost::none};
    return _createMetric<GaugeTpl<T>, GaugeTpl<T>, GaugeOptions>(
        name,
        options,
        std::move(identifier),
        /* makeInstrument= */
        [initialValue](WithLock, const std::string&) -> std::unique_ptr<GaugeTpl<T>> {
            return std::make_unique<GaugeImpl<T>>(initialValue);
        },
#ifdef MONGO_CONFIG_OTEL
        /* addObservable= */
        [this, description, unit](
            WithLock lock, const std::string& nameStr, GaugeTpl<T>* gauge_ptr) {
            (void)lock;
            auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
            auto observableGauge =
                metrics_service_detail::makeObservableInstrument<ObservableGauge<T>>(
                    *provider, nameStr, description, unit);
            tassert(ErrorCodes::InternalError,
                    fmt::format("Could not create observable gauge for metric: {}", nameStr),
                    observableGauge != nullptr);
            observableGauge->AddCallback(metrics_service_detail::observableCallback<GaugeTpl, T>,
                                         gauge_ptr);
            _observableInstruments.push_back(std::move(observableGauge));
        }
#else
        [](WithLock, const std::string&, GaugeTpl<T>*) {}
#endif
    );
}

Gauge<int64_t>& MetricsService::createInt64Gauge(MetricName name,
                                                 std::string description,
                                                 MetricUnit unit,
                                                 const GaugeOptions& options) {
    return _createScalarMetric<ObservableGauge, int64_t>(
        name, std::move(description), unit, options);
}

Gauge<double>& MetricsService::createDoubleGauge(MetricName name,
                                                 std::string description,
                                                 MetricUnit unit,
                                                 const GaugeOptions& options) {
    return _createScalarMetric<ObservableGauge, double>(
        name, std::move(description), unit, options);
}

template <typename T>
MinGauge<T>& MetricsService::createMinGauge(MetricName name,
                                            std::string description,
                                            MetricUnit unit,
                                            const GaugeOptions& options) {
    return createGaugeBase<MinGauge, T>(
        name, description, unit, options, std::numeric_limits<T>::max());
}

MinGauge<int64_t>& MetricsService::createInt64MinGauge(MetricName name,
                                                       std::string description,
                                                       MetricUnit unit,
                                                       const GaugeOptions& options) {
    return createMinGauge<int64_t>(name, description, unit, options);
}

MinGauge<double>& MetricsService::createDoubleMinGauge(MetricName name,
                                                       std::string description,
                                                       MetricUnit unit,
                                                       const GaugeOptions& options) {
    return createMinGauge<double>(name, description, unit, options);
}

template <typename T>
MaxGauge<T>& MetricsService::createMaxGauge(MetricName name,
                                            std::string description,
                                            MetricUnit unit,
                                            const GaugeOptions& options) {
    return createGaugeBase<MaxGauge, T>(
        name, description, unit, options, std::numeric_limits<T>::lowest());
}

MaxGauge<int64_t>& MetricsService::createInt64MaxGauge(MetricName name,
                                                       std::string description,
                                                       MetricUnit unit,
                                                       const GaugeOptions& options) {
    return createMaxGauge<int64_t>(name, description, unit, options);
}

MaxGauge<double>& MetricsService::createDoubleMaxGauge(MetricName name,
                                                       std::string description,
                                                       MetricUnit unit,
                                                       const GaugeOptions& options) {
    return createMaxGauge<double>(name, description, unit, options);
}

std::vector<std::string> MetricsService::getAttributeNamesForTests(MetricName name) const {
    std::lock_guard lock(_mutex);
    const std::string nameStr(name.getName());
    auto it = _metrics.find(nameStr);
    massert(ErrorCodes::KeyNotFound,
            fmt::format("No metric with name {} exists", nameStr),
            it != _metrics.end());
    std::vector<std::string> names;
    for (const ComparableAttributeDefinition& def : it->second.identifier.attributeDefinitions) {
        names.push_back(def.name);
    }
    return names;
}

void MetricsService::clearForTests() {
    std::lock_guard lock(_mutex);
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

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<ObservableMinGauge<int64_t>>& gauge) {
    gauge->reset();
    auto observable = makeObservableInstrument<ObservableMinGauge<int64_t>>(
        provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<ObservableMinGauge, int64_t>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<ObservableMinGauge<double>>& gauge) {
    gauge->reset();
    auto observable = makeObservableInstrument<ObservableMinGauge<double>>(
        provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<ObservableMinGauge, double>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<ObservableMaxGauge<int64_t>>& gauge) {
    gauge->reset();
    auto observable = makeObservableInstrument<ObservableMaxGauge<int64_t>>(
        provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<ObservableMaxGauge, int64_t>, gauge.get());
    newObservableInstruments.push_back(observable);
}

void MetricsService::OwnedMetricVisitor::operator()(
    std::unique_ptr<ObservableMaxGauge<double>>& gauge) {
    gauge->reset();
    auto observable = makeObservableInstrument<ObservableMaxGauge<double>>(
        provider, name, id.description, id.unit);
    observable->AddCallback(observableCallback<ObservableMaxGauge, double>, gauge.get());
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

MetricsService::MetricsService(MetricTreeSet& metricTreeSet) : _metricTreeSet(metricTreeSet) {}

void MetricsService::_registerServerStatusTree(
    WithLock, Metric* metricPtr, const boost::optional<ServerStatusOptions>& serverStatusOptions) {
    if (!serverStatusOptions.has_value()) {
        return;
    }
    _metricTreeSet[serverStatusOptions->role].add(
        serverStatusOptions->dottedPath,
        std::make_unique<OtelMetricServerStatusAdapter>(metricPtr));
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

}  // namespace mongo::otel::metrics

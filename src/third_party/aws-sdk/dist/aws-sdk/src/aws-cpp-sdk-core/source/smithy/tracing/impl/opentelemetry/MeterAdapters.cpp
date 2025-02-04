/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <smithy/tracing/impl/opentelemetry/MeterAdapters.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <opentelemetry/metrics/sync_instruments.h>
#include <opentelemetry/metrics/async_instruments.h>
#include <opentelemetry/metrics/observer_result.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/nostd/variant.h>

#include <utility>

using namespace smithy::components::tracing;

static const char *ALLOC_TAG = "OTEL_METER_ADAPTER";

Aws::UniquePtr<MonotonicCounter> OtelMeterAdapter::CreateCounter(Aws::String name,
    Aws::String units,
    Aws::String description) const
{
    auto counter = otelMeter->CreateUInt64Counter(name, description, units);
    return Aws::MakeUnique<OtelCounterAdapter>(ALLOC_TAG, std::move(counter));
}

Aws::UniquePtr<GaugeHandle> OtelMeterAdapter::CreateGauge(Aws::String name,
    std::function<void(Aws::UniquePtr<AsyncMeasurement>)> callback,
    Aws::String units,
    Aws::String description) const
{
    auto gauge = otelMeter->CreateInt64ObservableGauge(name, description, units);
    GaugeHandleState gaugeHandleState{callback};
    auto callbackFunc = [](opentelemetry::metrics::ObserverResult result, void *state) -> void {
        if (state == nullptr) {
            AWS_LOG_ERROR(ALLOC_TAG, "refusing to process null observer result")
            return;
        }
        Aws::UniquePtr<AsyncMeasurement> measurement = Aws::MakeUnique<OtelObserverAdapter>(ALLOC_TAG, result);
        auto handleState = reinterpret_cast<GaugeHandleState *>(state);
        handleState->callback(std::move(measurement));
    };
    gauge->AddCallback(callbackFunc, &gaugeHandleState);
    return Aws::MakeUnique<OtelGaugeAdapter>(ALLOC_TAG, std::move(gauge), callbackFunc);
}

Aws::UniquePtr<Histogram> OtelMeterAdapter::CreateHistogram(Aws::String name,
    Aws::String units,
    Aws::String description) const
{
    auto histogram = otelMeter->CreateDoubleHistogram(name, description, units);
    return Aws::MakeUnique<OtelHistogramAdapter>(ALLOC_TAG, std::move(histogram));
}

Aws::UniquePtr<UpDownCounter> OtelMeterAdapter::CreateUpDownCounter(Aws::String name,
    Aws::String units,
    Aws::String description) const
{
    auto counter = otelMeter->CreateInt64UpDownCounter(name, description, units);
    return Aws::MakeUnique<OtelUpDownCounterAdapter>(ALLOC_TAG, std::move(counter));
}

OtelCounterAdapter::OtelCounterAdapter(
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>> counter) :
    otelCounter(std::move(counter)) {}

void OtelCounterAdapter::add(long value, Aws::Map<Aws::String, Aws::String> attributes) {
    otelCounter->Add(value, attributes);
}

OtelUpDownCounterAdapter::OtelUpDownCounterAdapter(
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<int64_t>> counter) :
    otelUpDownCounter(std::move(counter)) {}

void OtelUpDownCounterAdapter::add(long value,
    Aws::Map<Aws::String, Aws::String> attributes) {
    otelUpDownCounter->Add(value, attributes);
}

OtelHistogramAdapter::OtelHistogramAdapter(
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>> otelHistogram)
    : otelHistogram(std::move(otelHistogram)) {}

void OtelHistogramAdapter::record(double value, Aws::Map<Aws::String, Aws::String> attributes) {
    otelHistogram->Record(value, attributes, opentelemetry::context::RuntimeContext::GetCurrent());
}

OtelGaugeAdapter::OtelGaugeAdapter(
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> otelGauge,
    opentelemetry::metrics::ObservableCallbackPtr callback)
    : otelGauge(std::move(otelGauge)), otelCallback(callback) {}

void OtelGaugeAdapter::Stop() {
    otelGauge->RemoveCallback(otelCallback, nullptr);
}

OtelObserverAdapter::OtelObserverAdapter(const opentelemetry::metrics::ObserverResult &result) :
    otelResult(result) {}

void OtelObserverAdapter::Record(double value, const Aws::Map<Aws::String, Aws::String> &attributes) {
    auto result = opentelemetry::nostd::get<opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>>(
        otelResult);
    result->Observe(value, attributes);
}


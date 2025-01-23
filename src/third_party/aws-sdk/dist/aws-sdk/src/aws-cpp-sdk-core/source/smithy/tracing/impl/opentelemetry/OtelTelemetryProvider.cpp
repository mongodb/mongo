/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <smithy/tracing/impl/opentelemetry/OtelTelemetryProvider.h>
#include <smithy/tracing/impl/opentelemetry/OtelTracerProvider.h>
#include <smithy/tracing/impl/opentelemetry/OtelMeterProvider.h>
#include <opentelemetry/exporters/ostream/span_exporter.h>
#include <opentelemetry/exporters/ostream/metric_exporter.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_context.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/metrics/provider.h>

#include <memory>

using namespace smithy::components::tracing;

static const char *ALLOC_TAG = "OTEL_TELEMETRY_PROVIDER";

Aws::UniquePtr<TelemetryProvider> OtelTelemetryProvider::CreateOtelProvider(
    std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> spanExporter,
    std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> pushMetricExporter)
{
    auto tracerProvider = Aws::MakeUnique<OtelTracerProvider>(ALLOC_TAG);
    auto meterProvider = Aws::MakeUnique<OtelMeterProvider>(ALLOC_TAG);
    return Aws::MakeUnique<TelemetryProvider>(ALLOC_TAG, std::move(tracerProvider),
        std::move(meterProvider),
        [&]() -> void {
            //Init Tracing
            auto traceProcessor = opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(std::move(spanExporter));
            std::shared_ptr<opentelemetry::trace::TracerProvider> otelTracerProvider = opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(traceProcessor));
            opentelemetry::trace::Provider::SetTracerProvider(otelTracerProvider);
            //Init Metrics
            opentelemetry::sdk::metrics::PeriodicExportingMetricReaderOptions options;
            options.export_interval_millis = std::chrono::milliseconds(1000);
            options.export_timeout_millis = std::chrono::milliseconds(500);
            std::unique_ptr<opentelemetry::sdk::metrics::MetricReader> reader{new opentelemetry::sdk::metrics::PeriodicExportingMetricReader(std::move(pushMetricExporter), options)};
            auto otelMeterProvider = std::shared_ptr<opentelemetry::metrics::MeterProvider>(new opentelemetry::sdk::metrics::MeterProvider());
            auto p = std::static_pointer_cast<opentelemetry::sdk::metrics::MeterProvider>(otelMeterProvider);
            p->AddMetricReader(std::move(reader));
            opentelemetry::metrics::Provider::SetMeterProvider(otelMeterProvider);
        },
        []() -> void {
            // Clean up tracing
            std::shared_ptr<opentelemetry::trace::TracerProvider> emptyTracer;
            opentelemetry::trace::Provider::SetTracerProvider(emptyTracer);
            // Clean up metrics
            std::shared_ptr<opentelemetry::metrics::MeterProvider> emptyMeter;
            opentelemetry::metrics::Provider::SetMeterProvider(emptyMeter);
        }
    );
}

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/tracer_provider_service_factory.h"

#include "mongo/logv2/log.h"
#include "mongo/otel/traces/trace_settings.h"
#include "mongo/otel/traces/trace_settings_gen.h"
#include "mongo/platform/process_id.h"

#include <chrono>
#include <string_view>

#include <fmt/format.h>
#include <opentelemetry/exporters/otlp/otlp_file_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_file_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/processor.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo::otel::traces {
namespace {

/** Populates BatchSpanProcessorOptions from the current tracing server parameters. */
opentelemetry::sdk::trace::BatchSpanProcessorOptions makeBatchSpanProcessorOptions() {
    opentelemetry::sdk::trace::BatchSpanProcessorOptions opts;
    opts.max_queue_size = static_cast<size_t>(gOpenTelemetryTracingMaxQueueSize);
    opts.max_export_batch_size = static_cast<size_t>(gOpenTelemetryTracingMaxBatchSize);
    opts.schedule_delay_millis =
        std::chrono::milliseconds(gOpenTelemetryTracingBatchExportIntervalMillis);
    return opts;
}

/**
 * Builds the OTel Resource with service identity plus any user-supplied attributes. Attributes
 * "service.name" and "service.instance.id" are set with default values if no user configuration is
 * provided.
 */
opentelemetry::sdk::resource::Resource makeResource(std::string_view name, std::string_view pid) {
    opentelemetry::sdk::resource::ResourceAttributes attrs;
    attrs["service.name"] = std::string(name);
    attrs["service.instance.id"] = std::string(pid);
    for (auto& [key, value] : getTracingResourceAttributes()) {
        attrs[key] = value;
    }
    return opentelemetry::sdk::resource::Resource::Create(attrs);
}

}  // namespace

StatusWith<std::unique_ptr<TracerProviderService>> createHttpTracerProviderService(
    std::string name, std::string endpoint) {
    LOGV2(9859702,
          "Initializing OpenTelemetry tracing using HTTP exporter",
          "name"_attr = name,
          "endpoint"_attr = endpoint);

    const auto pid = ProcessId::getCurrent().toString();

    opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
    opts.url = std::move(endpoint);
    opts.compression = gOpenTelemetryTracingCompression;
    for (auto& [headerName, headerValues] : getTracingHttpExportHeaders()) {
        for (const std::string& value : headerValues) {
            opts.http_headers.emplace(headerName, value);
        }
    }

    auto exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(opts);
    auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(
        std::move(exporter), makeBatchSpanProcessorOptions());

    auto tracerProvider = opentelemetry::sdk::trace::TracerProviderFactory::Create(
        std::move(processor), makeResource(name, pid));

    return std::make_unique<TracerProviderService>(std::move(tracerProvider));
}

StatusWith<std::unique_ptr<TracerProviderService>> createFileTracerProviderService(
    std::string name, std::string directory) {
    LOGV2(9859701,
          "Initializing OpenTelemetry tracing using file exporter",
          "name"_attr = name,
          "directory"_attr = directory);

    const auto pid = ProcessId::getCurrent().toString();

    opentelemetry::exporter::otlp::OtlpFileExporterOptions opts;
    opentelemetry::exporter::otlp::OtlpFileClientFileSystemOptions sysOpts;
    sysOpts.file_pattern = fmt::format("{}/{}-{}-%Y%m%d-trace.jsonl", directory, name, pid);
    opts.backend_options = std::move(sysOpts);

    auto exporter = opentelemetry::exporter::otlp::OtlpFileExporterFactory::Create(opts);
    auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(
        std::move(exporter), makeBatchSpanProcessorOptions());

    auto tracerProvider = opentelemetry::sdk::trace::TracerProviderFactory::Create(
        std::move(processor), makeResource(name, pid));

    return std::make_unique<TracerProviderService>(std::move(tracerProvider));
}

std::unique_ptr<TracerProviderService> createNoOpTracerProviderService() {
    LOGV2(9859700, "Not initializing OpenTelemetry");
    return std::make_unique<TracerProviderService>(nullptr);
}

}  // namespace mongo::otel::traces

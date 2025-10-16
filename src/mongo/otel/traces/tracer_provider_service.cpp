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

#include "mongo/otel/traces/tracer_provider_service.h"

#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/traces/trace_settings_gen.h"

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
const auto getTracerProviderService =
    ServiceContext::declareDecoration<std::unique_ptr<TracerProviderService>>();
}  // namespace

TracerProviderService* TracerProviderService::get(ServiceContext* serviceContext) {
    return getTracerProviderService(serviceContext).get();
}

TracerProviderService* TracerProviderService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void TracerProviderService::set(ServiceContext* serviceContext,
                                std::unique_ptr<TracerProviderService> service) {
    getTracerProviderService(serviceContext) = std::move(service);
}

Status TracerProviderService::initializeHttp(std::string name, std::string endpoint) {
    LOGV2(9859702,
          "Initializing OpenTelemetry tracing using HTTP exporter",
          "name"_attr = name,
          "endpoint"_attr = endpoint);

    auto pid = ProcessId::getCurrent().toString();

    opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
    opts.url = endpoint;

    const opentelemetry::sdk::trace::BatchSpanProcessorOptions batchSpanProcessorOptions;
    auto exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(opts);
    auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(
        std::move(exporter), std::move(batchSpanProcessorOptions));

    auto resourceAttributes = opentelemetry::sdk::resource::ResourceAttributes{
        {"service.name", name}, {"service.instance.id", pid}};
    auto resource = opentelemetry::sdk::resource::Resource::Create(resourceAttributes);

    _tracerProvider =
        opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor), resource);
    _enabled = true;

    return Status::OK();
}

Status TracerProviderService::initializeFile(std::string name, std::string directory) {
    LOGV2(9859701,
          "Initializing OpenTelemetry tracing using file exporter",
          "name"_attr = name,
          "directory"_attr = directory);

    auto pid = ProcessId::getCurrent().toString();

    opentelemetry::exporter::otlp::OtlpFileExporterOptions opts;
    opentelemetry::exporter::otlp::OtlpFileClientFileSystemOptions sysOpts;
    sysOpts.file_pattern = fmt::format("{}/{}-{}-%Y%m%d-trace.jsonl", directory, name, pid);
    opts.backend_options = sysOpts;

    const opentelemetry::sdk::trace::BatchSpanProcessorOptions batchSpanProcessorOptions;
    auto exporter = opentelemetry::exporter::otlp::OtlpFileExporterFactory::Create(opts);
    auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(
        std::move(exporter), std::move(batchSpanProcessorOptions));

    auto resourceAttributes = opentelemetry::sdk::resource::ResourceAttributes{
        {"service.name", name}, {"service.instance.id", pid}};
    auto resource = opentelemetry::sdk::resource::Resource::Create(resourceAttributes);

    _tracerProvider =
        opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor), resource);
    _enabled = true;

    return Status::OK();
}

void TracerProviderService::initializeNoOp() {
    LOGV2(9859700, "Not initializing OpenTelemetry");
    _tracerProvider = nullptr;
    _enabled = false;
}

void TracerProviderService::shutdown() {
    if (_enabled && _tracerProvider) {
        auto tracer = _tracerProvider->GetTracer("mongodb");
        tracer->Close(stdx::chrono::seconds{1});
        _tracerProvider = nullptr;
        _enabled = false;
    }
}

}  // namespace mongo::otel::traces

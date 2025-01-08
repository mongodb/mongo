/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/tracing/trace_initialization.h"

#ifdef MONGO_CONFIG_OTEL

#include <opentelemetry/exporters/otlp/otlp_file_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_file_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/processor.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>

#include "mongo/logv2/log.h"
#include "mongo/tracing/trace_settings_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace tracing {

namespace {

namespace otlp = opentelemetry::exporter::otlp;
namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;

Status initializeHttp(std::string name, std::string endpoint) {
    LOGV2(9859702,
          "Initializing OpenTelemetry tracing using HTTP exporter",
          "name"_attr = name,
          "endpoint"_attr = endpoint);

    auto pid = ProcessId::getCurrent().toString();

    opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
    opts.url = endpoint;

    auto exporter = otlp::OtlpHttpExporterFactory::Create(opts);
    auto processor = trace_sdk::SimpleSpanProcessorFactory::Create(std::move(exporter));

    auto resourceAttributes = opentelemetry::sdk::resource::ResourceAttributes{
        {"service.name", name}, {"service.instance.id", pid}};
    auto resource = opentelemetry::sdk::resource::Resource::Create(resourceAttributes);

    std::shared_ptr<trace_api::TracerProvider> provider =
        trace_sdk::TracerProviderFactory::Create(std::move(processor), resource);
    trace_api::Provider::SetTracerProvider(std::move(provider));

    return Status::OK();
}

Status initializeFile(std::string name, std::string directory) {
    LOGV2(9859701,
          "Initializing OpenTelemetry tracing using file exporter",
          "name"_attr = name,
          "directory"_attr = directory);

    auto pid = ProcessId::getCurrent().toString();

    otlp::OtlpFileExporterOptions opts;
    otlp::OtlpFileClientFileSystemOptions sysOpts;
    sysOpts.file_pattern = fmt::format("{}/{}-{}-%Y%m%d-trace.jsonl", directory, name, pid);
    opts.backend_options = sysOpts;

    auto exporter = otlp::OtlpFileExporterFactory::Create(opts);
    auto processor = trace_sdk::SimpleSpanProcessorFactory::Create(std::move(exporter));

    auto resourceAttributes = opentelemetry::sdk::resource::ResourceAttributes{
        {"service.name", name}, {"service.instance.id", pid}};
    auto resource = opentelemetry::sdk::resource::Resource::Create(resourceAttributes);

    std::shared_ptr<trace_api::TracerProvider> provider =
        trace_sdk::TracerProviderFactory::Create(std::move(processor), resource);
    trace_api::Provider::SetTracerProvider(std::move(provider));

    return Status::OK();
}

}  // namespace

Status initialize(std::string name) {
    uassert(
        ErrorCodes::InvalidOptions,
        "gOpenTelemetryHttpEndpoint and gOpenTelemetryTraceDirectory cannot be set simultaneously",
        gOpenTelemetryHttpEndpoint.empty() || gOpenTelemetryTraceDirectory.empty());

    if (!gOpenTelemetryHttpEndpoint.empty()) {
        return initializeHttp(name, gOpenTelemetryHttpEndpoint);
    } else if (!gOpenTelemetryTraceDirectory.empty()) {
        return initializeFile(name, gOpenTelemetryTraceDirectory);
    } else {
        LOGV2(9859700, "Not initializing OpenTelemetry");
        return Status::OK();
    }
}

void shutdown() {
    if (!gOpenTelemetryHttpEndpoint.empty() || !gOpenTelemetryTraceDirectory.empty()) {
        auto tracer = opentelemetry::trace::Provider::GetTracerProvider()->GetTracer("mongodb");
        tracer->Close(std::chrono::seconds{1});

        trace_api::Provider::SetTracerProvider({});
    }
}

}  // namespace tracing
}  // namespace mongo

#endif

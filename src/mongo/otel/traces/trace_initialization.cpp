// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/trace_initialization.h"

#include "mongo/logv2/log.h"
#include "mongo/otel/traces/trace_settings_gen.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/otel/traces/tracer_provider_service_factory.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace otel {
namespace traces {

namespace {

/** Throws InvalidOptions if any combination of server parameters is logically inconsistent. */
void validateOptions() {
    uassert(
        ErrorCodes::InvalidOptions,
        "opentelemetryHttpEndpoint and opentelemetryTraceDirectory cannot be set simultaneously",
        gOpenTelemetryHttpEndpoint.empty() || gOpenTelemetryTraceDirectory.empty());

    uassert(ErrorCodes::InvalidOptions,
            "openTelemetryTracingCompression must be `none` or `gzip`",
            gOpenTelemetryTracingCompression == "none" ||
                gOpenTelemetryTracingCompression == "gzip");

    uassert(
        ErrorCodes::InvalidOptions,
        "openTelemetryTracingCompression must be `none` unless opentelemetryHttpEndpoint is set",
        !gOpenTelemetryHttpEndpoint.empty() || gOpenTelemetryTracingCompression == "none");

    uassert(ErrorCodes::InvalidOptions,
            "openTelemetryTracingMaxBatchSize must be <= openTelemetryTracingMaxQueueSize",
            gOpenTelemetryTracingMaxBatchSize <= gOpenTelemetryTracingMaxQueueSize);
}

}  // namespace

Status initialize(std::string name) {
    validateOptions();

    if (!gOpenTelemetryHttpEndpoint.empty()) {
        auto swTracerProviderService =
            createHttpTracerProviderService(name, gOpenTelemetryHttpEndpoint);
        if (!swTracerProviderService.isOK()) {
            return swTracerProviderService.getStatus();
        }
        setGlobalTracerProviderService(std::move(swTracerProviderService.getValue()));
    } else if (!gOpenTelemetryTraceDirectory.empty()) {
        auto swTracerProviderService =
            createFileTracerProviderService(name, gOpenTelemetryTraceDirectory);
        if (!swTracerProviderService.isOK()) {
            return swTracerProviderService.getStatus();
        }
        setGlobalTracerProviderService(std::move(swTracerProviderService.getValue()));
    } else {
        setGlobalTracerProviderService(createNoOpTracerProviderService());
    }

    return Status::OK();
}

void shutdown() {
    auto tracerProviderService = getGlobalTracerProviderService();
    if (tracerProviderService) {
        tracerProviderService->shutdown();
    }
}

}  // namespace traces
}  // namespace otel
}  // namespace mongo

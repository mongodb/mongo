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

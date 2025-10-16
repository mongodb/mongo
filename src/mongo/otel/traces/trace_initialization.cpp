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
#include "mongo/stdx/chrono.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace otel {
namespace traces {

namespace {

Status initializeHttp(ServiceContext* serviceContext, std::string name, std::string endpoint) {
    auto tracerProviderService = TracerProviderService::get(serviceContext);
    if (!tracerProviderService) {
        return Status(ErrorCodes::InternalError, "TracerProviderService not initialized");
    }
    return tracerProviderService->initializeHttp(name, endpoint);
}

Status initializeFile(ServiceContext* serviceContext, std::string name, std::string directory) {
    auto tracerProviderService = TracerProviderService::get(serviceContext);
    if (!tracerProviderService) {
        return Status(ErrorCodes::InternalError, "TracerProviderService not initialized");
    }
    return tracerProviderService->initializeFile(name, directory);
}

void initializeNoOp(ServiceContext* serviceContext) {
    auto tracerProviderService = TracerProviderService::get(serviceContext);
    if (tracerProviderService) {
        tracerProviderService->initializeNoOp();
    }
}

void shutdownTracerProvider(ServiceContext* serviceContext) {
    auto tracerProviderService = TracerProviderService::get(serviceContext);
    if (tracerProviderService) {
        tracerProviderService->shutdown();
    }
}

}  // namespace

Status initialize(ServiceContext* serviceContext, std::string name) {
    uassert(
        ErrorCodes::InvalidOptions,
        "gOpenTelemetryHttpEndpoint and gOpenTelemetryTraceDirectory cannot be set simultaneously",
        gOpenTelemetryHttpEndpoint.empty() || gOpenTelemetryTraceDirectory.empty());

    if (!serviceContext) {
        return Status(ErrorCodes::InternalError, "No global ServiceContext available");
    }

    // Initialize the TracerProviderService if it doesn't exist
    if (!TracerProviderService::get(serviceContext)) {
        TracerProviderService::set(serviceContext, TracerProviderService::create());
    }

    if (!gOpenTelemetryHttpEndpoint.empty()) {
        return initializeHttp(serviceContext, name, gOpenTelemetryHttpEndpoint);
    } else if (!gOpenTelemetryTraceDirectory.empty()) {
        return initializeFile(serviceContext, name, gOpenTelemetryTraceDirectory);
    } else {
        initializeNoOp(serviceContext);
        return Status::OK();
    }
}

void shutdown(ServiceContext* serviceContext) {
    if (serviceContext) {
        shutdownTracerProvider(serviceContext);
    }
}


}  // namespace traces
}  // namespace otel
}  // namespace mongo

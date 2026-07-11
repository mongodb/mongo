// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/tracer_provider_service.h"

#include "mongo/util/static_immortal.h"

#include <chrono>
#include <utility>

namespace mongo::otel::traces {
namespace {

std::unique_ptr<TracerProviderService>& globalTracerProviderService() {
    static StaticImmortal<std::unique_ptr<TracerProviderService>> instance;
    return *instance;
}

}  // namespace

TracerProviderService* getGlobalTracerProviderService() {
    return globalTracerProviderService().get();
}

void setGlobalTracerProviderService(std::unique_ptr<TracerProviderService> service) {
    globalTracerProviderService() = std::move(service);
}

std::unique_ptr<TracerProviderService> swapGlobalTracerProviderServiceForTest(
    std::unique_ptr<TracerProviderService> newService) {
    return std::exchange(globalTracerProviderService(), std::move(newService));
}

void TracerProviderService::shutdown() {
    if (_enabled && _tracerProvider) {
        auto tracer = _tracerProvider->GetTracer("mongodb");
        tracer->Close(std::chrono::seconds{1});
        _tracerProvider = nullptr;
        _enabled = false;
    }
}

}  // namespace mongo::otel::traces

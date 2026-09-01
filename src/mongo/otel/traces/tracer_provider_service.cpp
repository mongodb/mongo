// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/tracer_provider_service.h"

#include "mongo/platform/atomic.h"
#include "mongo/util/static_immortal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/noop.h>

namespace mongo::otel::traces {
namespace {

using ::opentelemetry::trace::TracerProvider;

std::unique_ptr<TracerProviderService>& globalTracerProviderService() {
    static StaticImmortal<std::unique_ptr<TracerProviderService>> instance;
    return *instance;
}

}  // namespace

TracerProviderService::TracerProviderService(
    std::unique_ptr<opentelemetry::trace::TracerProvider> tracerProvider)
    : _tracerProvider(tracerProvider
                          ? std::make_shared<std::unique_ptr<opentelemetry::trace::TracerProvider>>(
                                std::move(tracerProvider))
                          : nullptr) {}

void TracerProviderService::setTracerProvider_ForTest(
    std::unique_ptr<opentelemetry::trace::TracerProvider> tracerProvider) {
    _tracerProvider.update(
        tracerProvider ? std::make_shared<std::unique_ptr<opentelemetry::trace::TracerProvider>>(
                             std::move(tracerProvider))
                       : nullptr);
}

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

TracerProvider* TracerProviderService::getTracerProvider() const {
    // The thread-local is shared across all instances, so it is keyed to this instance's unique
    // id: a snapshot taken from a different service (e.g. a prior global service swapped out in
    // a test) is discarded rather than mistaken for current.
    thread_local uint64_t cachedId = 0;
    thread_local VersionedValue<std::unique_ptr<TracerProvider>>::Snapshot snapshot;
    if (cachedId != _id) {
        cachedId = _id;
        snapshot = {};
    }
    _tracerProvider.refreshSnapshot(snapshot);
    return snapshot ? (*snapshot).get() : nullptr;
}

uint64_t TracerProviderService::_nextId() {
    static Atomic<uint64_t> counter{0};
    return counter.fetchAndAddRelaxed(1);
}

void TracerProviderService::shutdown() {
    if (auto snapshot = _tracerProvider.makeSnapshot()) {
        if (auto* sdkProvider =
                dynamic_cast<opentelemetry::sdk::trace::TracerProvider*>(snapshot->get())) {
            sdkProvider->Shutdown(std::chrono::seconds(1));
        } else {
            // The factory always builds an SDK TracerProvider; the API base type exposes no
            // Shutdown(). If we don't shut it down, server shutdown will be unsafe.
            invariant(dynamic_cast<opentelemetry::trace::NoopTracerProvider*>(snapshot->get()));
        }
        _tracerProvider.update(nullptr);
    }
}

}  // namespace mongo::otel::traces

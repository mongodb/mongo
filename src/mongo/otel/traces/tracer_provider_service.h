// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/versioned_value.h"

#include <cstdint>
#include <memory>

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/trace/provider.h>
#endif

namespace mongo::otel::traces {

/**
 * Service class that manages the OpenTelemetry TracerProvider. This allows the tracer provider to
 * be isolated from other code that may interact with the OpenTelemetry library.
 *
 * Instances are constructed with an already-created tracer provider (or none, if tracing is
 * disabled). The logic for building a tracer provider (e.g. from an HTTP or file OTLP exporter)
 * and its dependencies (OTLP exporters, trace settings, etc.) lives in a separate library; see
 * tracer_provider_service_factory.h.
 *
 * This class is thread-safe, with some caveats (see getTracerProvider() documentation).
 */
class TracerProviderService {
public:
    /** A null tracerProvider is valid and represents tracing being disabled. */
    explicit TracerProviderService(
        std::unique_ptr<opentelemetry::trace::TracerProvider> tracerProvider);

    /**
     * Shutdown the tracer provider.
     */
    void shutdown();

    /**
     * Returns the current tracer provider, or nullptr when tracing is disabled. The returned
     * pointer is valid until the next call to getTracerProvider() from the same thread. As such,
     * be very careful about passing the returned pointer to a different thread.
     */
    opentelemetry::trace::TracerProvider* getTracerProvider() const;

    void setTracerProvider_ForTest(
        std::unique_ptr<opentelemetry::trace::TracerProvider> tracerProvider);

private:
    /** Returns a process-unique id, never reused, used to invalidate thread-local snapshots. */
    static uint64_t _nextId();

    // Id for the purposes of invalidating thread-local snapshots if the global service is changed.
    const uint64_t _id = _nextId();
    // The provider handle is stored in a VersionedValue so that operation threads (which read it on
    // every span start) never race with provider change (e.g. during shutdown). Readers access it
    // through a snapshot that keeps the provider alive for the duration of their use, so it can
    // never be destroyed out from under an in-flight operation.
    VersionedValue<std::unique_ptr<opentelemetry::trace::TracerProvider>> _tracerProvider;
};

/** May return nullptr if tracing is not initialized or supported in the current environment. */
TracerProviderService* getGlobalTracerProviderService();

void setGlobalTracerProviderService(std::unique_ptr<TracerProviderService> service);

/**
 * Replaces the current global TracerProviderService with a new one and returns the old one.
 * Intended for use in tests.
 */
std::unique_ptr<TracerProviderService> swapGlobalTracerProviderServiceForTest(
    std::unique_ptr<TracerProviderService> newService);


}  // namespace mongo::otel::traces

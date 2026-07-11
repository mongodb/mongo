// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"  // IWYU pragma: keep

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
 */
class TracerProviderService {
public:
    TracerProviderService(std::shared_ptr<opentelemetry::trace::TracerProvider> tracerProvider,
                          bool enabled)
        : _tracerProvider(std::move(tracerProvider)), _enabled(enabled) {}

    /**
     * Shutdown the tracer provider.
     */
    void shutdown();

    /**
     * Get the tracer provider.
     */
    std::shared_ptr<opentelemetry::trace::TracerProvider> getTracerProvider() const {
        return _tracerProvider;
    }

    /**
     * Check if tracing is enabled.
     */
    bool isEnabled() const {
        return _enabled;
    }

    void setTracerProvider_ForTest(
        std::shared_ptr<opentelemetry::trace::TracerProvider> tracerProvider) {
        _tracerProvider = tracerProvider;
        _enabled = (tracerProvider != nullptr);
    }

private:
    std::shared_ptr<opentelemetry::trace::TracerProvider> _tracerProvider;
    bool _enabled = false;
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

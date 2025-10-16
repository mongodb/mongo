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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/service_context.h"

#include <memory>
#include <string>

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/trace/provider.h>
#endif

namespace mongo::otel::traces {

/**
 * Service class that manages the OpenTelemetry TracerProvider using ServiceContext decorations.
 * This allows the tracer provider to be isolated from other code that may interact with the
 * OpenTelemetry library.
 */
class TracerProviderService {
public:
    /**
     * Get the TracerProviderService instance from ServiceContext decoration.
     */
    static TracerProviderService* get(ServiceContext* serviceContext);
    static TracerProviderService* get(OperationContext* opCtx);

    /**
     * Set the TracerProviderService instance in ServiceContext decoration.
     */
    static void set(ServiceContext* serviceContext, std::unique_ptr<TracerProviderService> service);

    /**
     * Initialize the tracer provider with HTTP exporter.
     */
    Status initializeHttp(std::string name, std::string endpoint);

    /**
     * Initialize the tracer provider with file exporter.
     */
    Status initializeFile(std::string name, std::string directory);

    /**
     * Initialize with no-op tracer provider (when tracing is disabled).
     */
    void initializeNoOp();

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

    /**
     * Factory method for creating TracerProviderService instances.
     * This is needed for testing and should be used instead of direct construction.
     */
    static std::unique_ptr<TracerProviderService> create() {
        return std::unique_ptr<TracerProviderService>(new TracerProviderService());
    }

    void setTracerProvider_ForTest(
        std::shared_ptr<opentelemetry::trace::TracerProvider> tracerProvider) {
        _tracerProvider = tracerProvider;
        _enabled = (tracerProvider != nullptr);
    }

private:
    TracerProviderService() = default;

    std::shared_ptr<opentelemetry::trace::TracerProvider> _tracerProvider;
    bool _enabled = false;
};

}  // namespace mongo::otel::traces

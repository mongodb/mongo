// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/otel/traces/tracer_provider_service.h"

#include <memory>
#include <string>

namespace mongo::otel::traces {

/**
 * Creates a TracerProviderService whose tracer provider exports spans over HTTP, configured via
 * the relevant server parameters.
 */
StatusWith<std::unique_ptr<TracerProviderService>> createHttpTracerProviderService(
    std::string name, std::string endpoint);

/**
 * Creates a TracerProviderService whose tracer provider exports spans to files, configured via the
 * relevant server parameters.
 */
StatusWith<std::unique_ptr<TracerProviderService>> createFileTracerProviderService(
    std::string name, std::string directory);

/**
 * Creates a TracerProviderService with a no-op tracer provider. This should be used when tracing is
 * disabled, or for tests when an arbitrary service is needed.
 */
std::unique_ptr<TracerProviderService> createNoOpTracerProviderService();

}  // namespace mongo::otel::traces

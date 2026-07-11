// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/utils/bson_to_http_headers.h"
#include "mongo/stdx/unordered_map.h"

#include <string>
#include <vector>

namespace mongo::otel::traces {

/**
 * Returns the HTTP export headers parsed from openTelemetryTracingHttpExportHeaders.
 * Each key maps to a list of values to support duplicate header names.
 */
[[nodiscard]] const HttpHeaderMap& getTracingHttpExportHeaders();

/**
 * Returns the OTel resource attributes applied to all exported spans.
 */
[[nodiscard]] const stdx::unordered_map<std::string, std::string>& getTracingResourceAttributes();

}  // namespace mongo::otel::traces

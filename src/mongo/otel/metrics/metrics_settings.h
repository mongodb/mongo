// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/utils/bson_to_http_headers.h"

namespace mongo::otel::metrics {
/**
 * Returns the HTTP export headers parsed from openTelemetryMetricsHttpExportHeaders.
 * Each key maps to a list of values to support duplicate header names.
 */
[[nodiscard]] const HttpHeaderMap& getMetricsHttpExportHeaders();
}  // namespace mongo::otel::metrics

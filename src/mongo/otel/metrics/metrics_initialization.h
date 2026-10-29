// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace mongo::otel::metrics {
/**
 * Initializes OpenTelemetry metrics using either the HTTP or file exporter. If this needs to be
 * called more than once, `shutdown` must be called before it can be called again. This is not
 * thread-safe.
 */
[[MONGO_MOD_PUBLIC]] Status initialize();

/**
 * Shuts down the OpenTelemetry metric export process by setting the global MeterProvider to a
 * NoopMeterProvider. This is not thread-safe.
 */
[[MONGO_MOD_PUBLIC]] void shutdown();

namespace metrics_initialization_detail {
/**
 * The file name pattern and per-file size cap used to configure the OTLP file metric exporter.
 */
struct OtelMetricsFileExporterConfig {
    std::string filePattern;
    std::size_t fileSize;
};

/**
 * Builds the file name pattern and per-file size cap for the OTLP file metric exporter. Exposed for
 * testing.
 *
 * The pattern embeds a rotation index (%N) so the exporter's built-in size-based rotation produces
 * distinct files instead of appending to a single file that grows without bound, and `fileSize` is
 * kept below the mongo shell's cat() 16MB limit so the resulting files stay readable by shell-based
 * tooling (see jstests otel_file_export_helpers.js).
 */
OtelMetricsFileExporterConfig makeMetricsFileExporterConfig(std::string_view directory,
                                                            std::string_view pid);
}  // namespace metrics_initialization_detail
}  // namespace mongo::otel::metrics

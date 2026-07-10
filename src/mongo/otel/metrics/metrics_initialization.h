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

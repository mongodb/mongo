// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/util/functional.h"
#include "mongo/util/system_clock_source.h"

#include <memory>
#include <string>

#include <opentelemetry/sdk/metrics/push_metric_exporter.h>

namespace mongo::otel::metrics {

struct PrometheusFileExporterOptions {
    /**
     * The maximum number of export failures allowed before we consider the situation unrecoverable
     * and terminate the process. This could be triggered by things like the output file becoming
     * unwritable or writes suddenly becoming extremely slow.
     */
    int maxConsecutiveFailures = 10;

    /**
     * The clock to wait on for timeouts. Exposed for testing.
     */
    ClockSource* clockSource = SystemClockSource::get();

    /**
     * Used for testing delays and slowness in the file writer thread. This code will be executed
     * when the failpoint is active.
     */
    unique_function<void()> testOnlyFailpointCallback;
};

/**
 * Exports files to the provided `filepath`. Note that the exporter will sometimes create a file
 * with the same name as `filepath` but ".tmp" extension to facilitate the process. These files are
 * cleaned up by the exporter.
 */
StatusWith<std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>>
createPrometheusFileExporter(const std::string& filepath,
                             PrometheusFileExporterOptions options = {});
}  // namespace mongo::otel::metrics

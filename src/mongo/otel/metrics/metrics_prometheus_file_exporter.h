/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

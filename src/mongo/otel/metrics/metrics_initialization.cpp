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


#include "mongo/otel/metrics/metrics_initialization.h"

#ifdef MONGO_CONFIG_OTEL

#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metrics_settings_gen.h"
#include "mongo/stdx/chrono.h"

#include <opentelemetry/exporters/otlp/otlp_file_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_file_metric_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo::otel::metrics {

namespace {

namespace otlp = opentelemetry::exporter::otlp;
namespace metrics_api = opentelemetry::metrics;
namespace metrics_sdk = opentelemetry::sdk::metrics;

Status initializeHttp(const std::string& name, const std::string& endpoint) {
    LOGV2(10500901,
          "Initializing OpenTelemetry metrics using HTTP exporter",
          "name"_attr = name,
          "endpoint"_attr = endpoint);

    opentelemetry::exporter::otlp::OtlpHttpMetricExporterOptions hmeOpts;
    hmeOpts.url = endpoint;

    auto exporter = otlp::OtlpHttpMetricExporterFactory::Create(hmeOpts);

    // Initialize and set the global MeterProvider
    metrics_sdk::PeriodicExportingMetricReaderOptions pemOpts;
    // TODO SERVER-105803 add a configurable knob for these
    pemOpts.export_interval_millis = stdx::chrono::milliseconds(1000);
    pemOpts.export_timeout_millis = stdx::chrono::milliseconds(500);

    auto reader =
        metrics_sdk::PeriodicExportingMetricReaderFactory::Create(std::move(exporter), pemOpts);

    std::shared_ptr<metrics_sdk::MeterProvider> provider =
        metrics_sdk::MeterProviderFactory::Create();

    provider->AddMetricReader(std::move(reader));
    metrics_api::Provider::SetMeterProvider(std::move(provider));

    return Status::OK();
}

Status initializeFile(const std::string& name, const std::string& directory) {
    LOGV2(10500902,
          "Initializing OpenTelemetry metrics using file exporter",
          "name"_attr = name,
          "directory"_attr = directory);

    auto pid = ProcessId::getCurrent().toString();

    opentelemetry::exporter::otlp::OtlpFileMetricExporterOptions fmeOpts;
    otlp::OtlpFileClientFileSystemOptions sysOpts;
    sysOpts.file_pattern = fmt::format("{}/{}-{}-%Y%m%d-metrics.jsonl", directory, name, pid);
    fmeOpts.backend_options = sysOpts;

    auto exporter = otlp::OtlpFileMetricExporterFactory::Create(fmeOpts);

    // Initialize and set the global MeterProvider
    metrics_sdk::PeriodicExportingMetricReaderOptions pemOpts;
    // TODO SERVER-105803 add a configurable knob for these
    pemOpts.export_interval_millis = stdx::chrono::milliseconds(1000);
    pemOpts.export_timeout_millis = stdx::chrono::milliseconds(500);

    auto reader =
        metrics_sdk::PeriodicExportingMetricReaderFactory::Create(std::move(exporter), pemOpts);

    std::shared_ptr<metrics_sdk::MeterProvider> provider =
        metrics_sdk::MeterProviderFactory::Create();

    provider->AddMetricReader(std::move(reader));
    metrics_api::Provider::SetMeterProvider(std::move(provider));

    return Status::OK();
}

}  // namespace

Status initialize(const std::string& name) {
    try {
        uassert(
            ErrorCodes::InvalidOptions,
            "gOpenTelemetryMetricsHttpEndpoint and gOpenTelemetryMetricsDirectory cannot be set "
            "simultaneously",
            gOpenTelemetryMetricsHttpEndpoint.empty() || gOpenTelemetryMetricsDirectory.empty());

        if (!gOpenTelemetryMetricsHttpEndpoint.empty()) {
            return initializeHttp(name, gOpenTelemetryMetricsHttpEndpoint);
        } else if (!gOpenTelemetryMetricsDirectory.empty()) {
            return initializeFile(name, gOpenTelemetryMetricsDirectory);
        }
        LOGV2(10500903, "Not initializing OpenTelemetry metrics");
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

void shutdown() {
    LOGV2(10500904, "Shutting down OpenTelemetry metrics");
    if (!gOpenTelemetryMetricsHttpEndpoint.empty() || !gOpenTelemetryMetricsDirectory.empty()) {
        metrics_api::Provider::SetMeterProvider({});
    }
}

}  // namespace mongo::otel::metrics
#else
namespace mongo::otel::metrics {
// Provide empty definitions.
/**
 * Initializes OpenTelemetry metrics using either the HTTP or file exporter.
 */
Status initialize(const std::string&) {
    return Status::OK();
}

/**
 * Shuts down the OpenTelemetry metric export process by setting the global MeterProvider to a
 * NoopMeterProvider.
 */
void shutdown() {}
}  // namespace mongo::otel::metrics
#endif

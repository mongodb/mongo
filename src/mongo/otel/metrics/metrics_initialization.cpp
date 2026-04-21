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

#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metrics_prometheus_file_exporter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/metrics_settings_gen.h"

#include <chrono>

#include <absl/algorithm/container.h>
#include <google/protobuf/message.h>
#ifdef MONGO_CONFIG_GRPC
#include <grpcpp/ext/otel_plugin.h>
#endif
#include <opentelemetry/exporters/otlp/otlp_file_client.h>
#include <opentelemetry/exporters/otlp/otlp_file_client_options.h>
#include <opentelemetry/exporters/otlp/otlp_file_client_runtime_options.h>
#include <opentelemetry/exporters/otlp/otlp_file_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_file_metric_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/proto/resource/v1/resource.pb.h>
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

/**
 * Returns the current singleton metricReader.
 */
std::shared_ptr<metrics_sdk::MetricReader>& metricReader() {
    static std::shared_ptr<metrics_sdk::MetricReader> reader;
    return reader;
}

/**
 * Returns whether metrics has been initialized.
 */
bool isInitialized() {
    return metricReader() != nullptr;
}

Status initializeHttp(const std::string& endpoint, const std::string& compression) {
    LOGV2(10500901,
          "Initializing OpenTelemetry metrics using HTTP exporter",
          "endpoint"_attr = endpoint);

    otlp::OtlpHttpMetricExporterOptions hmeOpts;
    hmeOpts.url = endpoint;
    hmeOpts.compression = compression;

    auto exporter = otlp::OtlpHttpMetricExporterFactory::Create(hmeOpts);

    // Initialize and set the global MeterProvider
    metrics_sdk::PeriodicExportingMetricReaderOptions pemOpts;
    pemOpts.export_interval_millis = std::chrono::milliseconds(gOpenTelemetryExportIntervalMillis);
    pemOpts.export_timeout_millis = std::chrono::milliseconds(gOpenTelemetryExportTimeoutMillis);

    auto reader =
        metrics_sdk::PeriodicExportingMetricReaderFactory::Create(std::move(exporter), pemOpts);

    std::shared_ptr<metrics_sdk::MeterProvider> provider =
        metrics_sdk::MeterProviderFactory::Create();

    metricReader() = std::move(reader);
    provider->AddMetricReader(metricReader());
    metrics_api::Provider::SetMeterProvider(std::move(provider));

    return Status::OK();
}

// Uses the otlp file client to write out an empty file to do some one-time initialization.
void prepOtlpFileClient(const otlp::OtlpFileMetricExporterOptions& fmeOpts) {
    otlp::OtlpFileClientOptions options;
    // This will create the same file as the actual output, which will empty from the export here
    // and be overwritten when we actually output metrics.
    options.backend_options = fmeOpts.backend_options;
    otlp::OtlpFileClientRuntimeOptions runtimeOptions;
    otlp::OtlpFileClient client(std::move(options), std::move(runtimeOptions));
    opentelemetry::proto::resource::v1::Resource emptyMessage;
    invariant(client.Export(emptyMessage, 1) == opentelemetry::sdk::common::ExportResult::kSuccess);
}

Status initializeFile(const std::string& directory) {
    LOGV2(10500902,
          "Initializing OpenTelemetry metrics using file exporter",
          "directory"_attr = directory);

    auto pid = ProcessId::getCurrent().toString();

    otlp::OtlpFileMetricExporterOptions fmeOpts;
    otlp::OtlpFileClientFileSystemOptions sysOpts;
    sysOpts.file_pattern = fmt::format("{}/mongodb-{}-%Y%m%d-metrics.jsonl", directory, pid);
    fmeOpts.backend_options = sysOpts;

    auto exporter = otlp::OtlpFileMetricExporterFactory::Create(fmeOpts);

    // Initialize and set the global MeterProvider
    metrics_sdk::PeriodicExportingMetricReaderOptions pemOpts;
    pemOpts.export_interval_millis = std::chrono::milliseconds(gOpenTelemetryExportIntervalMillis);
    pemOpts.export_timeout_millis = std::chrono::milliseconds(gOpenTelemetryExportTimeoutMillis);
    // We do a empty file write immediately here because there's some one-time static initialization
    // that needs to be done in the file exporter, which is currently not done in a thread-safe way,
    // so if there are multiple file exporters (e.g. for metrics and for traces) it can lead to a
    // data race.
    prepOtlpFileClient(fmeOpts);

    auto reader =
        metrics_sdk::PeriodicExportingMetricReaderFactory::Create(std::move(exporter), pemOpts);

    std::shared_ptr<metrics_sdk::MeterProvider> provider =
        metrics_sdk::MeterProviderFactory::Create();

    metricReader() = std::move(reader);
    provider->AddMetricReader(metricReader());
    metrics_api::Provider::SetMeterProvider(std::move(provider));

    return Status::OK();
}

Status initializePrometheusFileExporter(const std::string& path, const int maxConsecutiveFailures) {
    LOGV2(11730000,
          "Initializing OpenTelemetry metrics using Prometheus file exporter",
          "path"_attr = path,
          "maxConsecutiveFailures"_attr = maxConsecutiveFailures);

    StatusWith<std::unique_ptr<metrics_sdk::PushMetricExporter>> prometheusFileExporter =
        createPrometheusFileExporter(path, {.maxConsecutiveFailures = maxConsecutiveFailures});
    if (!prometheusFileExporter.isOK()) {
        return prometheusFileExporter.getStatus();
    }

    metrics_sdk::PeriodicExportingMetricReaderOptions pemOpts;
    pemOpts.export_interval_millis = std::chrono::milliseconds(gOpenTelemetryExportIntervalMillis);
    pemOpts.export_timeout_millis = std::chrono::milliseconds(gOpenTelemetryExportTimeoutMillis);
    auto reader = metrics_sdk::PeriodicExportingMetricReaderFactory::Create(
        std::move(prometheusFileExporter.getValue()), pemOpts);

    // Initialize and set the global MeterProvider
    std::shared_ptr<metrics_sdk::MeterProvider> provider =
        metrics_sdk::MeterProviderFactory::Create();

    metricReader() = std::move(reader);
    provider->AddMetricReader(metricReader());
    metrics_api::Provider::SetMeterProvider(std::move(provider));

    return Status::OK();
}

void validateOptions() {
    uassert(ErrorCodes::InvalidOptions,
            "featureFlagOtelMetrics must be enabled in order to export OpenTelemetry metrics",
            gFeatureFlagOtelMetrics.isEnabled() ||
                (gOpenTelemetryMetricsHttpEndpoint.empty() &&
                 gOpenTelemetryMetricsDirectory.empty() &&
                 gOpenTelemetryPrometheusMetricsPath.empty() &&
                 gOpenTelemetryPrometheusMetricsDirectory.empty()));

    uassert(ErrorCodes::InvalidOptions,
            "At most one of openTelemetryMetricsHttpEndpoint, openTelemetryMetricsDirectory, and "
            "(openTelemetryPrometheusMetricsPath or openTelemetryPrometheusMetricsDirectory) may "
            "be set",
            absl::c_count(std::vector<bool>{!gOpenTelemetryMetricsHttpEndpoint.empty(),
                                            !gOpenTelemetryMetricsDirectory.empty(),
                                            !gOpenTelemetryPrometheusMetricsPath.empty() ||
                                                !gOpenTelemetryPrometheusMetricsDirectory.empty()},
                          true) <= 1);

    uassert(ErrorCodes::InvalidOptions,
            "openTelemetryMetricsCompression must be either `none` or `gzip`",
            gOpenTelemetryMetricsCompression == "none" ||
                gOpenTelemetryMetricsCompression == "gzip");

    uassert(ErrorCodes::InvalidOptions,
            "openTelemetryMetricsCompression must be `none` unless "
            "openTelemetryMetricsHttpEndpoint is set",
            !gOpenTelemetryMetricsHttpEndpoint.empty() ||
                gOpenTelemetryMetricsCompression == "none");

    uassert(ErrorCodes::InvalidOptions,
            "openTelemetryExportTimeoutMillis must be less than openTelemetryExportIntervalMillis",
            gOpenTelemetryExportTimeoutMillis < gOpenTelemetryExportIntervalMillis);
}
}  // namespace

Status initialize() {
    try {
        uassert(ErrorCodes::IllegalOperation,
                "Metrics initialization attempted after a previous initialization without "
                "calling shutdown.",
                !isInitialized());

        validateOptions();

        const bool httpEndpointParameterSet = !gOpenTelemetryMetricsHttpEndpoint.empty();
        const bool directoryParameterSet = !gOpenTelemetryMetricsDirectory.empty();
        const bool prometheusExporterParamaterSet = !gOpenTelemetryPrometheusMetricsPath.empty() ||
            !gOpenTelemetryPrometheusMetricsDirectory.empty();

        if (!httpEndpointParameterSet && !directoryParameterSet &&
            !prometheusExporterParamaterSet) {
            LOGV2(10500903, "Not initializing OpenTelemetry metrics");
            return Status::OK();
        }

        auto status = [&]() {
            if (httpEndpointParameterSet) {
                return initializeHttp(gOpenTelemetryMetricsHttpEndpoint,
                                      gOpenTelemetryMetricsCompression);
            } else if (prometheusExporterParamaterSet) {
                if (!gOpenTelemetryPrometheusMetricsDirectory.empty() &&
                    !gOpenTelemetryPrometheusMetricsPath.empty()) {
                    LOGV2(12291200,
                          "Both openTelemetryPrometheusMetricsPath and "
                          "openTelemetryPrometheusMetricsDirectory are set, so "
                          "openTelemetryPrometheusMetricsPath takes precedence.");
                }
                const std::string prometheusPath = !gOpenTelemetryPrometheusMetricsPath.empty()
                    ? gOpenTelemetryPrometheusMetricsPath
                    : fmt::format("{}/mongodb-prometheus-metrics.txt",
                                  gOpenTelemetryPrometheusMetricsDirectory);
                return initializePrometheusFileExporter(
                    prometheusPath, gOpenTelemetryPrometheusFileExportMaxConsecutiveFailures);
            }
            return initializeFile(gOpenTelemetryMetricsDirectory);
        }();

        if (!status.isOK()) {
            return status;
        }

        auto provider = metrics_api::Provider::GetMeterProvider();
        invariant(provider);
        MetricsService::instance().initialize(*provider);

#ifdef MONGO_CONFIG_GRPC
        // Register gRPC OpenTelemetry plugin globally
        auto grpcStatus =
            grpc::OpenTelemetryPluginBuilder().SetMeterProvider(provider).BuildAndRegisterGlobal();
        if (!grpcStatus.ok()) {
            LOGV2_WARNING(12022600,
                          "Failed to register gRPC OTel plugin",
                          "status"_attr = grpcStatus.ToString());
        }
#endif

        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

void shutdown() {
    LOGV2(10500904, "Shutting down OpenTelemetry metrics");
    if (isInitialized()) {
        // Explicitly shutdown the metricReader since the Otel implementation may hold on to the
        // shared_ptr even after the current provider is replaced.
        invariant(metricReader() != nullptr);
        metricReader()->Shutdown();
        metricReader() = nullptr;
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
Status initialize() {
    return Status::OK();
}

/**
 * Shuts down the OpenTelemetry metric export process by setting the global MeterProvider to a
 * NoopMeterProvider.
 */
void shutdown() {}
}  // namespace mongo::otel::metrics
#endif

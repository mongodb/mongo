// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/exporters/otlp/otlp_file_metric_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_file_metric_exporter_runtime_options.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

/**
 * Factory class for OtlpFileExporter.
 */
class OPENTELEMETRY_EXPORT OtlpFileMetricExporterFactory
{
public:
  /**
   * Create an OtlpFileExporter using all default options.
   */
  static std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> Create();

  /**
   * Create an OtlpFileExporter using the given options.
   */
  static std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> Create(
      const OtlpFileMetricExporterOptions &options);

  /**
   * Create an OtlpFileExporter using the given options.
   */
  static std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> Create(
      const OtlpFileMetricExporterOptions &options,
      const OtlpFileMetricExporterRuntimeOptions &runtime_options);
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

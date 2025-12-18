// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_runtime_options.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>
OtlpHttpMetricExporterFactory::Create()
{
  OtlpHttpMetricExporterOptions options;
  return Create(options);
}

std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>
OtlpHttpMetricExporterFactory::Create(const OtlpHttpMetricExporterOptions &options)
{
  OtlpHttpMetricExporterRuntimeOptions runtime_options;
  return Create(options, runtime_options);
}

std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>
OtlpHttpMetricExporterFactory::Create(const OtlpHttpMetricExporterOptions &options,
                                      const OtlpHttpMetricExporterRuntimeOptions &runtime_options)
{
  std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> exporter(
      new OtlpHttpMetricExporter(options, runtime_options));
  return exporter;
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

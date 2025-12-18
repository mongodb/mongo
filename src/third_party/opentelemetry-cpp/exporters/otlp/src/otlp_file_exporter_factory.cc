// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/otlp/otlp_file_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_file_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_file_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_file_exporter_runtime_options.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> OtlpFileExporterFactory::Create()
{
  OtlpFileExporterOptions options;
  return Create(options);
}

std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> OtlpFileExporterFactory::Create(
    const OtlpFileExporterOptions &options)
{
  OtlpFileExporterRuntimeOptions runtime_options;
  return Create(options, runtime_options);
}

std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> OtlpFileExporterFactory::Create(
    const OtlpFileExporterOptions &options,
    const OtlpFileExporterRuntimeOptions &runtime_options)
{
  std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> exporter(
      new OtlpFileExporter(options, runtime_options));
  return exporter;
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

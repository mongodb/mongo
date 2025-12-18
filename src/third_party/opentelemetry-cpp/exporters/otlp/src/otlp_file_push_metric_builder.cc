// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <utility>

#include "opentelemetry/exporters/otlp/otlp_builder_utils.h"
#include "opentelemetry/exporters/otlp/otlp_file_metric_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_file_metric_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_file_push_metric_builder.h"
#include "opentelemetry/sdk/configuration/otlp_file_push_metric_exporter_builder.h"
#include "opentelemetry/sdk/configuration/otlp_file_push_metric_exporter_configuration.h"
#include "opentelemetry/sdk/configuration/registry.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

void OtlpFilePushMetricBuilder::Register(opentelemetry::sdk::configuration::Registry *registry)
{
  auto builder = std::make_unique<OtlpFilePushMetricBuilder>();
  registry->SetOtlpFilePushMetricExporterBuilder(std::move(builder));
}

std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> OtlpFilePushMetricBuilder::Build(
    const opentelemetry::sdk::configuration::OtlpFilePushMetricExporterConfiguration *model) const
{
  OtlpFileMetricExporterOptions options;

  // FIXME: unclear how to map model->output_stream to a OtlpFileClientBackendOptions

  options.aggregation_temporality =
      OtlpBuilderUtils::ConvertTemporalityPreference(model->temporality_preference);

  return OtlpFileMetricExporterFactory::Create(options);
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

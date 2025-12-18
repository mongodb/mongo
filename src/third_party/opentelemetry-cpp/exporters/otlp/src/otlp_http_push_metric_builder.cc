// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "opentelemetry/exporters/otlp/otlp_builder_utils.h"
#include "opentelemetry/exporters/otlp/otlp_http.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_push_metric_builder.h"
#include "opentelemetry/sdk/configuration/http_tls_configuration.h"
#include "opentelemetry/sdk/configuration/otlp_http_push_metric_exporter_builder.h"
#include "opentelemetry/sdk/configuration/otlp_http_push_metric_exporter_configuration.h"
#include "opentelemetry/sdk/configuration/registry.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

void OtlpHttpPushMetricBuilder::Register(opentelemetry::sdk::configuration::Registry *registry)
{
  auto builder = std::make_unique<OtlpHttpPushMetricBuilder>();
  registry->SetOtlpHttpPushMetricExporterBuilder(std::move(builder));
}

std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> OtlpHttpPushMetricBuilder::Build(
    const opentelemetry::sdk::configuration::OtlpHttpPushMetricExporterConfiguration *model) const
{
  OtlpHttpMetricExporterOptions options(nullptr);

  const auto *tls = model->tls.get();

  options.url                = model->endpoint;
  options.content_type       = OtlpBuilderUtils::ConvertOtlpHttpEncoding(model->encoding);
  options.json_bytes_mapping = JsonBytesMappingKind::kHexId;
  options.use_json_name      = false;
  options.console_debug      = false;
  options.timeout            = std::chrono::duration_cast<std::chrono::system_clock::duration>(
      std::chrono::seconds{model->timeout});
  options.http_headers =
      OtlpBuilderUtils::ConvertHeadersConfigurationModel(model->headers.get(), model->headers_list);
  options.aggregation_temporality =
      OtlpBuilderUtils::ConvertTemporalityPreference(model->temporality_preference);
  options.ssl_insecure_skip_verify = false;

  if (tls != nullptr)
  {
    options.ssl_ca_cert_path     = tls->certificate_file;
    options.ssl_client_key_path  = tls->client_key_file;
    options.ssl_client_cert_path = tls->client_certificate_file;
  }

  options.compression = model->compression;

  return OtlpHttpMetricExporterFactory::Create(options);
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "opentelemetry/exporters/otlp/otlp_builder_utils.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_span_builder.h"
#include "opentelemetry/sdk/configuration/grpc_tls_configuration.h"
#include "opentelemetry/sdk/configuration/otlp_grpc_span_exporter_builder.h"
#include "opentelemetry/sdk/configuration/otlp_grpc_span_exporter_configuration.h"
#include "opentelemetry/sdk/configuration/registry.h"
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

void OtlpGrpcSpanBuilder::Register(opentelemetry::sdk::configuration::Registry *registry)
{
  auto builder = std::make_unique<OtlpGrpcSpanBuilder>();
  registry->SetOtlpGrpcSpanBuilder(std::move(builder));
}

std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> OtlpGrpcSpanBuilder::Build(
    const opentelemetry::sdk::configuration::OtlpGrpcSpanExporterConfiguration *model) const
{
  OtlpGrpcExporterOptions options(nullptr);

  const auto *tls = model->tls.get();

  options.endpoint = model->endpoint;

  options.use_ssl_credentials = OtlpBuilderUtils::GrpcUseSsl(options.endpoint, tls);

  if (tls != nullptr)
  {
    options.ssl_credentials_cacert_path = tls->certificate_file;
#ifdef ENABLE_OTLP_GRPC_SSL_MTLS_PREVIEW
    options.ssl_client_key_path  = tls->client_key_file;
    options.ssl_client_cert_path = tls->client_certificate_file;
#endif
  }

  options.timeout = std::chrono::duration_cast<std::chrono::system_clock::duration>(
      std::chrono::seconds{model->timeout});
  options.metadata =
      OtlpBuilderUtils::ConvertHeadersConfigurationModel(model->headers.get(), model->headers_list);
  options.compression = model->compression;

  return OtlpGrpcExporterFactory::Create(options);
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h"
#include "opentelemetry/version.h"

#include <string>

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

OtlpGrpcExporterOptions::OtlpGrpcExporterOptions()
{
  endpoint                         = GetOtlpDefaultGrpcTracesEndpoint();
  use_ssl_credentials              = !GetOtlpDefaultGrpcTracesIsInsecure(); /* negation intended. */
  ssl_credentials_cacert_path      = GetOtlpDefaultTracesSslCertificatePath();
  ssl_credentials_cacert_as_string = GetOtlpDefaultTracesSslCertificateString();

#ifdef ENABLE_OTLP_GRPC_SSL_MTLS_PREVIEW
  ssl_client_key_path    = GetOtlpDefaultTracesSslClientKeyPath();
  ssl_client_key_string  = GetOtlpDefaultTracesSslClientKeyString();
  ssl_client_cert_path   = GetOtlpDefaultTracesSslClientCertificatePath();
  ssl_client_cert_string = GetOtlpDefaultTracesSslClientCertificateString();
#endif

  timeout    = GetOtlpDefaultTracesTimeout();
  metadata   = GetOtlpDefaultTracesHeaders();
  user_agent = GetOtlpDefaultUserAgent();

  max_threads = 0;

  compression = GetOtlpDefaultTracesCompression();
#ifdef ENABLE_ASYNC_EXPORT
  max_concurrent_requests = 64;
#endif
}

OtlpGrpcExporterOptions::~OtlpGrpcExporterOptions() {}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

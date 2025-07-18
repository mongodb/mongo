// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <string>

#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/exporters/otlp/otlp_http.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

OtlpHttpExporterOptions::OtlpHttpExporterOptions()
    : json_bytes_mapping(JsonBytesMappingKind::kHexId),
      use_json_name(false),
      console_debug(false),
      ssl_insecure_skip_verify(false)
{
  url          = GetOtlpDefaultHttpTracesEndpoint();
  content_type = GetOtlpHttpProtocolFromString(GetOtlpDefaultHttpTracesProtocol());

  timeout      = GetOtlpDefaultTracesTimeout();
  http_headers = GetOtlpDefaultTracesHeaders();

#ifdef ENABLE_ASYNC_EXPORT
  max_concurrent_requests     = 64;
  max_requests_per_connection = 8;
#endif /* ENABLE_ASYNC_EXPORT */

  ssl_ca_cert_path       = GetOtlpDefaultTracesSslCertificatePath();
  ssl_ca_cert_string     = GetOtlpDefaultTracesSslCertificateString();
  ssl_client_key_path    = GetOtlpDefaultTracesSslClientKeyPath();
  ssl_client_key_string  = GetOtlpDefaultTracesSslClientKeyString();
  ssl_client_cert_path   = GetOtlpDefaultTracesSslClientCertificatePath();
  ssl_client_cert_string = GetOtlpDefaultTracesSslClientCertificateString();

  ssl_min_tls      = GetOtlpDefaultTracesSslTlsMinVersion();
  ssl_max_tls      = GetOtlpDefaultTracesSslTlsMaxVersion();
  ssl_cipher       = GetOtlpDefaultTracesSslTlsCipher();
  ssl_cipher_suite = GetOtlpDefaultTracesSslTlsCipherSuite();

  compression = GetOtlpDefaultTracesCompression();
}

OtlpHttpExporterOptions::~OtlpHttpExporterOptions() {}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

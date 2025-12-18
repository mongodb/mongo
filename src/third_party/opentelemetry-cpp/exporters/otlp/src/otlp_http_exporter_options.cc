// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/exporters/otlp/otlp_http.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

OtlpHttpExporterOptions::OtlpHttpExporterOptions()
    : url(GetOtlpDefaultHttpTracesEndpoint()),
      content_type(GetOtlpHttpProtocolFromString(GetOtlpDefaultHttpTracesProtocol())),
      json_bytes_mapping(JsonBytesMappingKind::kHexId),
      use_json_name(false),
      console_debug(false),
      timeout(GetOtlpDefaultTracesTimeout()),
      http_headers(GetOtlpDefaultTracesHeaders()),
#ifdef ENABLE_ASYNC_EXPORT
      max_concurrent_requests{64},
      max_requests_per_connection{8},
#endif
      ssl_insecure_skip_verify(false),
      ssl_ca_cert_path(GetOtlpDefaultTracesSslCertificatePath()),
      ssl_ca_cert_string(GetOtlpDefaultTracesSslCertificateString()),
      ssl_client_key_path(GetOtlpDefaultTracesSslClientKeyPath()),
      ssl_client_key_string(GetOtlpDefaultTracesSslClientKeyString()),
      ssl_client_cert_path(GetOtlpDefaultTracesSslClientCertificatePath()),
      ssl_client_cert_string(GetOtlpDefaultTracesSslClientCertificateString()),
      ssl_min_tls(GetOtlpDefaultTracesSslTlsMinVersion()),
      ssl_max_tls(GetOtlpDefaultTracesSslTlsMaxVersion()),
      ssl_cipher(GetOtlpDefaultTracesSslTlsCipher()),
      ssl_cipher_suite(GetOtlpDefaultTracesSslTlsCipherSuite()),
      compression(GetOtlpDefaultTracesCompression()),
      retry_policy_max_attempts(GetOtlpDefaultTracesRetryMaxAttempts()),
      retry_policy_initial_backoff(GetOtlpDefaultTracesRetryInitialBackoff()),
      retry_policy_max_backoff(GetOtlpDefaultTracesRetryMaxBackoff()),
      retry_policy_backoff_multiplier(GetOtlpDefaultTracesRetryBackoffMultiplier())
{}

OtlpHttpExporterOptions::OtlpHttpExporterOptions(void *)
    : url(),
      content_type(HttpRequestContentType::kBinary),
      json_bytes_mapping(JsonBytesMappingKind::kHexId),
      use_json_name(false),
      console_debug(false),
#ifdef ENABLE_ASYNC_EXPORT
      max_concurrent_requests{64},
      max_requests_per_connection{8},
#endif
      ssl_insecure_skip_verify(false)
{}

OtlpHttpExporterOptions::~OtlpHttpExporterOptions() {}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

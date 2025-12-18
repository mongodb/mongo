// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/exporters/otlp/otlp_http.h"
#include "opentelemetry/exporters/otlp/otlp_preferred_temporality.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

OtlpHttpMetricExporterOptions::OtlpHttpMetricExporterOptions()
    : url(GetOtlpDefaultMetricsEndpoint()),
      content_type(GetOtlpHttpProtocolFromString(GetOtlpDefaultHttpMetricsProtocol())),
      json_bytes_mapping(JsonBytesMappingKind::kHexId),
      use_json_name(false),
      console_debug(false),
      timeout(GetOtlpDefaultMetricsTimeout()),
      http_headers(GetOtlpDefaultMetricsHeaders()),
      aggregation_temporality(PreferredAggregationTemporality::kCumulative),
#ifdef ENABLE_ASYNC_EXPORT
      max_concurrent_requests{64},
      max_requests_per_connection{8},
#endif
      ssl_insecure_skip_verify(false),
      ssl_ca_cert_path(GetOtlpDefaultMetricsSslCertificatePath()),
      ssl_ca_cert_string(GetOtlpDefaultMetricsSslCertificateString()),
      ssl_client_key_path(GetOtlpDefaultMetricsSslClientKeyPath()),
      ssl_client_key_string(GetOtlpDefaultMetricsSslClientKeyString()),
      ssl_client_cert_path(GetOtlpDefaultMetricsSslClientCertificatePath()),
      ssl_client_cert_string(GetOtlpDefaultMetricsSslClientCertificateString()),
      ssl_min_tls(GetOtlpDefaultMetricsSslTlsMinVersion()),
      ssl_max_tls(GetOtlpDefaultMetricsSslTlsMaxVersion()),
      ssl_cipher(GetOtlpDefaultMetricsSslTlsCipher()),
      ssl_cipher_suite(GetOtlpDefaultMetricsSslTlsCipherSuite()),
      compression(GetOtlpDefaultMetricsCompression()),
      retry_policy_max_attempts(GetOtlpDefaultMetricsRetryMaxAttempts()),
      retry_policy_initial_backoff(GetOtlpDefaultMetricsRetryInitialBackoff()),
      retry_policy_max_backoff(GetOtlpDefaultMetricsRetryMaxBackoff()),
      retry_policy_backoff_multiplier(GetOtlpDefaultMetricsRetryBackoffMultiplier())
{}

OtlpHttpMetricExporterOptions::OtlpHttpMetricExporterOptions(void *)
    : url(),
      content_type(exporter::otlp::HttpRequestContentType::kBinary),
      json_bytes_mapping(JsonBytesMappingKind::kHexId),
      use_json_name(false),
      console_debug(false),
      aggregation_temporality(PreferredAggregationTemporality::kCumulative),
#ifdef ENABLE_ASYNC_EXPORT
      max_concurrent_requests{64},
      max_requests_per_connection{8},
#endif
      ssl_insecure_skip_verify(false)
{}

OtlpHttpMetricExporterOptions::~OtlpHttpMetricExporterOptions() {}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

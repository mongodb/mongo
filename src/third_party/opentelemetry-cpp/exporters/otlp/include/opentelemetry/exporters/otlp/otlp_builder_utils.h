// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

#include "opentelemetry/exporters/otlp/otlp_environment.h"  // For OtlpHeaders
#include "opentelemetry/exporters/otlp/otlp_http.h"
#include "opentelemetry/exporters/otlp/otlp_preferred_temporality.h"
#include "opentelemetry/sdk/configuration/grpc_tls_configuration.h"
#include "opentelemetry/sdk/configuration/headers_configuration.h"
#include "opentelemetry/sdk/configuration/otlp_http_encoding.h"
#include "opentelemetry/sdk/configuration/temporality_preference.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

class OtlpBuilderUtils
{
public:
  static HttpRequestContentType ConvertOtlpHttpEncoding(
      opentelemetry::sdk::configuration::OtlpHttpEncoding model);

  static OtlpHeaders ConvertHeadersConfigurationModel(
      const opentelemetry::sdk::configuration::HeadersConfiguration *model,
      const std::string &headers_list);

  static PreferredAggregationTemporality ConvertTemporalityPreference(
      opentelemetry::sdk::configuration::TemporalityPreference model);

  static bool GrpcUseSsl(const std::string &endpoint,
                         const opentelemetry::sdk::configuration::GrpcTlsConfiguration *tls);
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

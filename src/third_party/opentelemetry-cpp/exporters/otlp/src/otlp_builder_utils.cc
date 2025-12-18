// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <string>
#include <utility>

#include "opentelemetry/common/kv_properties.h"
#include "opentelemetry/exporters/otlp/otlp_builder_utils.h"
#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/exporters/otlp/otlp_http.h"
#include "opentelemetry/exporters/otlp/otlp_preferred_temporality.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
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

HttpRequestContentType OtlpBuilderUtils::ConvertOtlpHttpEncoding(
    opentelemetry::sdk::configuration::OtlpHttpEncoding model)
{
  auto result = exporter::otlp::HttpRequestContentType::kBinary;

  switch (model)
  {
    case opentelemetry::sdk::configuration::OtlpHttpEncoding::protobuf:
      result = exporter::otlp::HttpRequestContentType::kBinary;
      break;
    case opentelemetry::sdk::configuration::OtlpHttpEncoding::json:
      result = exporter::otlp::HttpRequestContentType::kJson;
      break;
  }

  return result;
}

OtlpHeaders OtlpBuilderUtils::ConvertHeadersConfigurationModel(
    const opentelemetry::sdk::configuration::HeadersConfiguration *model,
    const std::string &headers_list)
{
  OtlpHeaders headers;

  // First, scan headers_list, which has low priority.
  if (headers_list.size() > 0)
  {
    opentelemetry::common::KeyValueStringTokenizer tokenizer{headers_list};

    opentelemetry::nostd::string_view header_key;
    opentelemetry::nostd::string_view header_value;
    bool header_valid = true;

    while (tokenizer.next(header_valid, header_key, header_value))
    {
      if (header_valid)
      {
        std::string key(header_key);
        std::string value(header_value);

        if (headers.find(key) == headers.end())
        {
          headers.emplace(std::make_pair(std::move(key), std::move(value)));
        }
        else
        {
          OTEL_INTERNAL_LOG_WARN("Found duplicate key in headers_list");
        }
      }
      else
      {
        OTEL_INTERNAL_LOG_WARN("Found invalid key/value pair in headers_list");
      }
    }
  }

  if (model != nullptr)
  {
    // Second, scan headers, which has high priority.
    for (const auto &kv : model->kv_map)
    {
      const auto &search = headers.find(kv.first);
      if (search != headers.end())
      {
        headers.erase(search);
      }

      headers.emplace(std::make_pair(kv.first, kv.second));
    }
  }

  return headers;
}

PreferredAggregationTemporality OtlpBuilderUtils::ConvertTemporalityPreference(
    opentelemetry::sdk::configuration::TemporalityPreference model)
{
  auto result = exporter::otlp::PreferredAggregationTemporality::kCumulative;

  switch (model)
  {
    case opentelemetry::sdk::configuration::TemporalityPreference::cumulative:
      result = exporter::otlp::PreferredAggregationTemporality::kCumulative;
      break;
    case opentelemetry::sdk::configuration::TemporalityPreference::delta:
      result = exporter::otlp::PreferredAggregationTemporality::kDelta;
      break;
    case opentelemetry::sdk::configuration::TemporalityPreference::low_memory:
      result = exporter::otlp::PreferredAggregationTemporality::kLowMemory;
      break;
  }

  return result;
}

bool OtlpBuilderUtils::GrpcUseSsl(
    const std::string &endpoint,
    const opentelemetry::sdk::configuration::GrpcTlsConfiguration *tls)
{
  if (endpoint.substr(0, 6) == "https:")
  {
    return true;
  }

  if (endpoint.substr(0, 5) == "http:")
  {
    return false;
  }

  if (tls != nullptr)
  {
    return !tls->insecure;
  }

  return true;
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/otlp/otlp_http.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

HttpRequestContentType GetOtlpHttpProtocolFromString(nostd::string_view name) noexcept
{
  if (name == "http/json")
  {
    return HttpRequestContentType::kJson;
  }
  else
  {
    return HttpRequestContentType::kBinary;
  }
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

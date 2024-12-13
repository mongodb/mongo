// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/resource/resource_detector.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/common/env_variables.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/resource/resource.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/resource/semantic_conventions.h"
#include "third_party/opentelemetry-cpp/api/include/opentelemetry/version.h"

#include <stddef.h>
#include <sstream>
#include <string>
#include <unordered_map>

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace resource
{

const char *OTEL_RESOURCE_ATTRIBUTES = "OTEL_RESOURCE_ATTRIBUTES";
const char *OTEL_SERVICE_NAME        = "OTEL_SERVICE_NAME";

Resource OTELResourceDetector::Detect() noexcept
{
  std::string attributes_str, service_name;

  bool attributes_exists = opentelemetry::sdk::common::GetStringEnvironmentVariable(
      OTEL_RESOURCE_ATTRIBUTES, attributes_str);
  bool service_name_exists =
      opentelemetry::sdk::common::GetStringEnvironmentVariable(OTEL_SERVICE_NAME, service_name);

  if (!attributes_exists && !service_name_exists)
  {
    return Resource();
  }

  ResourceAttributes attributes;

  if (attributes_exists)
  {
    std::istringstream iss(attributes_str);
    std::string token;
    while (std::getline(iss, token, ','))
    {
      size_t pos = token.find('=');
      if (pos != std::string::npos)
      {
        std::string key   = token.substr(0, pos);
        std::string value = token.substr(pos + 1);
        attributes[key]   = value;
      }
    }
  }

  if (service_name_exists)
  {
    attributes[SemanticConventions::kServiceName] = service_name;
  }

  return Resource(attributes);
}

}  // namespace resource
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

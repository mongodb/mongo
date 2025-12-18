// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/resource/resource_detector.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/env_variables.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/semconv/service_attributes.h"
#include "opentelemetry/version.h"

#include <stddef.h>
#include <sstream>
#include <string>
#include <unordered_map>

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace resource
{

constexpr const char *kOtelResourceAttributes = "OTEL_RESOURCE_ATTRIBUTES";
constexpr const char *kOtelServiceName        = "OTEL_SERVICE_NAME";

Resource ResourceDetector::Create(const ResourceAttributes &attributes,
                                  const std::string &schema_url)
{
  return Resource(attributes, schema_url);
}

Resource OTELResourceDetector::Detect() noexcept
{
  std::string attributes_str, service_name;

  bool attributes_exists = opentelemetry::sdk::common::GetStringEnvironmentVariable(
      kOtelResourceAttributes, attributes_str);
  bool service_name_exists =
      opentelemetry::sdk::common::GetStringEnvironmentVariable(kOtelServiceName, service_name);

  if (!attributes_exists && !service_name_exists)
  {
    return ResourceDetector::Create({});
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
    attributes[semconv::service::kServiceName] = service_name;
  }

  return ResourceDetector::Create(attributes);
}

}  // namespace resource
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/trace/samplers/always_on_factory.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/trace/samplers/always_on.h"
#include "third_party/opentelemetry-cpp/api/include/opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

std::unique_ptr<Sampler> AlwaysOnSamplerFactory::Create()
{
  std::unique_ptr<Sampler> sampler(new AlwaysOnSampler());
  return sampler;
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

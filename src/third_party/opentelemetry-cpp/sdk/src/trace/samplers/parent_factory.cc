// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/trace/sampler.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/trace/samplers/parent.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/trace/samplers/parent_factory.h"
#include "third_party/opentelemetry-cpp/api/include/opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

std::unique_ptr<Sampler> ParentBasedSamplerFactory::Create(
    const std::shared_ptr<Sampler> &delegate_sampler)
{
  std::unique_ptr<Sampler> sampler(new ParentBasedSampler(delegate_sampler));
  return sampler;
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

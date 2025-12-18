// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/sdk/trace/samplers/always_off.h"
#include "opentelemetry/sdk/trace/samplers/always_on.h"
#include "opentelemetry/sdk/trace/samplers/parent.h"
#include "opentelemetry/sdk/trace/samplers/parent_factory.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

std::unique_ptr<Sampler> ParentBasedSamplerFactory::Create(
    const std::shared_ptr<Sampler> &root_sampler)
{
  std::unique_ptr<Sampler> sampler = ParentBasedSamplerFactory::Create(
      root_sampler, std::make_shared<AlwaysOnSampler>(), std::make_shared<AlwaysOffSampler>(),
      std::make_shared<AlwaysOnSampler>(), std::make_shared<AlwaysOffSampler>());
  return sampler;
}

std::unique_ptr<Sampler> ParentBasedSamplerFactory::Create(
    const std::shared_ptr<Sampler> &root_sampler,
    const std::shared_ptr<Sampler> &remote_parent_sampled_sampler,
    const std::shared_ptr<Sampler> &remote_parent_nonsampled_sampler,
    const std::shared_ptr<Sampler> &local_parent_sampled_sampler,
    const std::shared_ptr<Sampler> &local_parent_nonsampled_sampler)
{
  std::unique_ptr<Sampler> sampler(new ParentBasedSampler(
      root_sampler, remote_parent_sampled_sampler, remote_parent_nonsampled_sampler,
      local_parent_sampled_sampler, local_parent_nonsampled_sampler));
  return sampler;
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

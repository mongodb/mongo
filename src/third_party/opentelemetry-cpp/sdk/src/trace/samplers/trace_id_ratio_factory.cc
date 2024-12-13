// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/trace/samplers/trace_id_ratio_factory.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/trace/samplers/trace_id_ratio.h"
#include "third_party/opentelemetry-cpp/api/include/opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

std::unique_ptr<Sampler> TraceIdRatioBasedSamplerFactory::Create(double ratio)
{
  std::unique_ptr<Sampler> sampler(new TraceIdRatioBasedSampler(ratio));
  return sampler;
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

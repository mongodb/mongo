// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

/**
 * Factory class for TraceIdRatioBasedSampler.
 */
class TraceIdRatioBasedSamplerFactory
{
public:
  /**
   * Create a TraceIdRatioBasedSampler.
   */
  static std::unique_ptr<Sampler> Create(double ratio);
};

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

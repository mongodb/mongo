// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <vector>

#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/trace/id_generator.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/sdk/trace/tracer_context.h"
#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/trace/tracer_provider.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

/**
 * Factory class for TracerProvider.
 * See @ref TracerProvider.
 */
class OPENTELEMETRY_EXPORT TracerProviderFactory
{
public:
  /* Serie of builders with a single processor. */

  static std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> Create(
      std::unique_ptr<SpanProcessor> processor);

  static std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> Create(
      std::unique_ptr<SpanProcessor> processor,
      const opentelemetry::sdk::resource::Resource &resource);

  static std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> Create(
      std::unique_ptr<SpanProcessor> processor,
      const opentelemetry::sdk::resource::Resource &resource,
      std::unique_ptr<Sampler> sampler);

  static std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> Create(
      std::unique_ptr<SpanProcessor> processor,
      const opentelemetry::sdk::resource::Resource &resource,
      std::unique_ptr<Sampler> sampler,
      std::unique_ptr<IdGenerator> id_generator);

  /* Serie of builders with a vector of processor. */

  static std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> Create(
      std::vector<std::unique_ptr<SpanProcessor>> &&processors);

  static std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> Create(
      std::vector<std::unique_ptr<SpanProcessor>> &&processors,
      const opentelemetry::sdk::resource::Resource &resource);

  static std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> Create(
      std::vector<std::unique_ptr<SpanProcessor>> &&processors,
      const opentelemetry::sdk::resource::Resource &resource,
      std::unique_ptr<Sampler> sampler);

  static std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> Create(
      std::vector<std::unique_ptr<SpanProcessor>> &&processors,
      const opentelemetry::sdk::resource::Resource &resource,
      std::unique_ptr<Sampler> sampler,
      std::unique_ptr<IdGenerator> id_generator);

  /* Create with a tracer context. */

  static std::unique_ptr<opentelemetry::sdk::trace::TracerProvider> Create(
      std::unique_ptr<TracerContext> context);
};

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <memory>
#include <vector>

#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/trace/id_generator.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/random_id_generator.h"
#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/sdk/trace/samplers/always_on.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

/**
 * A class which stores the TracerProvider context.
 *
 * This class meets the following design criteria:
 * - A shared reference between TracerProvider and Tracers instantiated.
 * - A thread-safe class that allows updating/altering processor/exporter pipelines
 *   and sampling config.
 * - The owner/destroyer of Processors/Exporters.  These will remain active until
 *   this class is destroyed.  I.e. Sampling, Exporting, flushing, Custom Iterator etc. are all ok
 * if this object is alive, and they will work together. If this object is destroyed, then no shared
 * references to Processor, Exporter, Recordable, Custom Iterator etc. should exist, and all
 *   associated pipelines will have been flushed.
 */
class TracerContext
{
public:
  explicit TracerContext(
      std::vector<std::unique_ptr<SpanProcessor>> &&processor,
      const opentelemetry::sdk::resource::Resource &resource =
          opentelemetry::sdk::resource::Resource::Create({}),
      std::unique_ptr<Sampler> sampler = std::unique_ptr<AlwaysOnSampler>(new AlwaysOnSampler),
      std::unique_ptr<IdGenerator> id_generator =
          std::unique_ptr<IdGenerator>(new RandomIdGenerator())) noexcept;

  virtual ~TracerContext() = default;

  /**
   * Attaches a span processor to list of configured processors to this tracer context.
   * Processor once attached can't be removed.
   * @param processor The new span processor for this tracer. This must not be
   * a nullptr. Ownership is given to the `TracerContext`.
   *
   * Note: This method is not thread safe.
   */
  void AddProcessor(std::unique_ptr<SpanProcessor> processor) noexcept;

  /**
   * Obtain the sampler associated with this tracer.
   * @return The sampler for this tracer.
   */
  Sampler &GetSampler() const noexcept;

  /**
   * Obtain the configured (composite) processor.
   *
   * Note: When more than one processor is active, this will
   * return an "aggregate" processor
   */
  SpanProcessor &GetProcessor() const noexcept;

  /**
   * Obtain the resource associated with this tracer context.
   * @return The resource for this tracer context.
   */
  const opentelemetry::sdk::resource::Resource &GetResource() const noexcept;

  /**
   * Obtain the Id Generator associated with this tracer context.
   * @return The ID Generator for this tracer context.
   */
  opentelemetry::sdk::trace::IdGenerator &GetIdGenerator() const noexcept;

  /**
   * Force all active SpanProcessors to flush any buffered spans
   * within the given timeout.
   */
  bool ForceFlush(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

  /**
   * Shutdown the span processor associated with this tracer provider.
   */
  bool Shutdown(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

private:
  //  order of declaration is important here - resource object should be destroyed after processor.
  opentelemetry::sdk::resource::Resource resource_;
  std::unique_ptr<Sampler> sampler_;
  std::unique_ptr<IdGenerator> id_generator_;
  std::unique_ptr<SpanProcessor> processor_;
};

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

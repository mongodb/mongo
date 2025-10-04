// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/trace/id_generator.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/sdk/trace/tracer.h"
#include "opentelemetry/sdk/trace/tracer_context.h"
#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/tracer.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{
namespace resource  = opentelemetry::sdk::resource;
namespace trace_api = opentelemetry::trace;

TracerProvider::TracerProvider(std::unique_ptr<TracerContext> context) noexcept
    : context_(std::move(context))
{
  OTEL_INTERNAL_LOG_DEBUG("[TracerProvider] TracerProvider created.");
}

TracerProvider::TracerProvider(std::unique_ptr<SpanProcessor> processor,
                               const resource::Resource &resource,
                               std::unique_ptr<Sampler> sampler,
                               std::unique_ptr<IdGenerator> id_generator) noexcept
{
  std::vector<std::unique_ptr<SpanProcessor>> processors;
  processors.push_back(std::move(processor));
  context_ = std::make_shared<TracerContext>(std::move(processors), resource, std::move(sampler),
                                             std::move(id_generator));
}

TracerProvider::TracerProvider(std::vector<std::unique_ptr<SpanProcessor>> &&processors,
                               const resource::Resource &resource,
                               std::unique_ptr<Sampler> sampler,
                               std::unique_ptr<IdGenerator> id_generator) noexcept
{
  context_ = std::make_shared<TracerContext>(std::move(processors), resource, std::move(sampler),
                                             std::move(id_generator));
}

TracerProvider::~TracerProvider()
{
  // Tracer hold the shared pointer to the context. So we can not use destructor of TracerContext to
  // Shutdown and flush all pending recordables when we have more than one tracers.These recordables
  // may use the raw pointer of instrumentation_scope_ in Tracer
  if (context_)
  {
    context_->Shutdown();
  }
}

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
nostd::shared_ptr<trace_api::Tracer> TracerProvider::GetTracer(
    nostd::string_view name,
    nostd::string_view version,
    nostd::string_view schema_url,
    const opentelemetry::common::KeyValueIterable *attributes) noexcept
#else
nostd::shared_ptr<trace_api::Tracer> TracerProvider::GetTracer(
    nostd::string_view name,
    nostd::string_view version,
    nostd::string_view schema_url) noexcept
#endif
{
#if OPENTELEMETRY_ABI_VERSION_NO < 2
  const opentelemetry::common::KeyValueIterable *attributes = nullptr;
#endif

  if (name.data() == nullptr)
  {
    OTEL_INTERNAL_LOG_ERROR("[TracerProvider::GetTracer] Library name is null.");
    name = "";
  }
  else if (name == "")
  {
    OTEL_INTERNAL_LOG_ERROR("[TracerProvider::GetTracer] Library name is empty.");
  }

  const std::lock_guard<std::mutex> guard(lock_);

  for (auto &tracer : tracers_)
  {
    auto &tracer_scope = tracer->GetInstrumentationScope();
    if (tracer_scope.equal(name, version, schema_url))
    {
      return nostd::shared_ptr<trace_api::Tracer>{tracer};
    }
  }

  instrumentationscope::InstrumentationScopeAttributes attrs_map(attributes);
  auto scope =
      instrumentationscope::InstrumentationScope::Create(name, version, schema_url, attrs_map);

  auto tracer = std::shared_ptr<Tracer>(new Tracer(context_, std::move(scope)));
  tracers_.push_back(tracer);
  return nostd::shared_ptr<trace_api::Tracer>{tracer};
}

void TracerProvider::AddProcessor(std::unique_ptr<SpanProcessor> processor) noexcept
{
  context_->AddProcessor(std::move(processor));
}

const resource::Resource &TracerProvider::GetResource() const noexcept
{
  return context_->GetResource();
}

bool TracerProvider::Shutdown() noexcept
{
  return context_->Shutdown();
}

bool TracerProvider::ForceFlush(std::chrono::microseconds timeout) noexcept
{
  return context_->ForceFlush(timeout);
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

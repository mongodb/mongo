// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/sdk/trace/samplers/parent.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/trace/trace_id.h"
#include "opentelemetry/version.h"

namespace trace_api = opentelemetry::trace;

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{
ParentBasedSampler::ParentBasedSampler(
    const std::shared_ptr<Sampler> &root_sampler,
    const std::shared_ptr<Sampler> &remote_parent_sampled_sampler,
    const std::shared_ptr<Sampler> &remote_parent_nonsampled_sampler,
    const std::shared_ptr<Sampler> &local_parent_sampled_sampler,
    const std::shared_ptr<Sampler> &local_parent_nonsampled_sampler) noexcept
    : root_sampler_(root_sampler),
      remote_parent_sampled_sampler_(remote_parent_sampled_sampler),
      remote_parent_nonsampled_sampler_(remote_parent_nonsampled_sampler),
      local_parent_sampled_sampler_(local_parent_sampled_sampler),
      local_parent_nonsampled_sampler_(local_parent_nonsampled_sampler),
      description_("ParentBased{" + std::string{root_sampler->GetDescription()} + "}")
{}

SamplingResult ParentBasedSampler::ShouldSample(
    const trace_api::SpanContext &parent_context,
    trace_api::TraceId trace_id,
    nostd::string_view name,
    trace_api::SpanKind span_kind,
    const opentelemetry::common::KeyValueIterable &attributes,
    const trace_api::SpanContextKeyValueIterable &links) noexcept
{
  if (!parent_context.IsValid())
  {
    // If no parent (root span) exists returns the result of the root_sampler
    return root_sampler_->ShouldSample(parent_context, trace_id, name, span_kind, attributes,
                                       links);
  }

  // If parent exists:
  if (parent_context.IsSampled())
  {
    if (parent_context.IsRemote())
    {
      return remote_parent_sampled_sampler_->ShouldSample(parent_context, trace_id, name, span_kind,
                                                          attributes, links);
    }
    return local_parent_sampled_sampler_->ShouldSample(parent_context, trace_id, name, span_kind,
                                                       attributes, links);
  }

  // Parent is not sampled
  if (parent_context.IsRemote())
  {
    return remote_parent_nonsampled_sampler_->ShouldSample(parent_context, trace_id, name,
                                                           span_kind, attributes, links);
  }
  return local_parent_nonsampled_sampler_->ShouldSample(parent_context, trace_id, name, span_kind,
                                                        attributes, links);
}

nostd::string_view ParentBasedSampler::GetDescription() const noexcept
{
  return description_;
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

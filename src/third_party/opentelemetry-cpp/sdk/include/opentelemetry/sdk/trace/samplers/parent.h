// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/trace/trace_id.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

/**
 * The ParentBased sampler is a composite sampler. ParentBased(delegateSampler) either respects
 * the parent span's sampling decision or delegates to delegateSampler for root spans.
 */
class ParentBasedSampler : public Sampler
{
public:
  explicit ParentBasedSampler(const std::shared_ptr<Sampler> &delegate_sampler) noexcept;
  /** The decision either respects the parent span's sampling decision or delegates to
   * delegateSampler for root spans
   * @return Returns DROP always
   */
  SamplingResult ShouldSample(
      const opentelemetry::trace::SpanContext &parent_context,
      opentelemetry::trace::TraceId trace_id,
      nostd::string_view name,
      opentelemetry::trace::SpanKind span_kind,
      const opentelemetry::common::KeyValueIterable &attributes,
      const opentelemetry::trace::SpanContextKeyValueIterable &links) noexcept override;

  /**
   * @return Description MUST be ParentBased{delegate_sampler_.getDescription()}
   */
  nostd::string_view GetDescription() const noexcept override;

private:
  const std::shared_ptr<Sampler> delegate_sampler_;
  const std::string description_;
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

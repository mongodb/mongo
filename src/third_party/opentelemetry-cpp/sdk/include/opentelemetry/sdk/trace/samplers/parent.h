// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/sdk/trace/samplers/always_off.h"
#include "opentelemetry/sdk/trace/samplers/always_on.h"
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
 * The ParentBased sampler is a composite sampler that delegates sampling decisions based on the
 * parent span's context.
 *
 * The decision is delegated to one of five configurable samplers:
 * - No parent exists (root span): delegates to `root sampler`.
 * - A remote parent exists and was sampled: delegates to `remote_parent_sampled_sampler` (default
 * to AlwaysOnSampler).
 * - A remote parent exists and was not sampled: delegates to `remote_parent_nonsampled_sampler`
 * (default to AlwaysOffSampler).
 * - A local parent exists and was sampled: delegates to `local_parent_sampled_sampler` (default to
 * AlwaysOnSampler).
 * - A local parent exists and was not sampled: delegates to `local_parent_nonsampled_sampler`
 * (default to AlwaysOffSampler).
 */
class ParentBasedSampler : public Sampler
{
public:
  explicit ParentBasedSampler(const std::shared_ptr<Sampler> &root_sampler,
                              const std::shared_ptr<Sampler> &remote_parent_sampled_sampler =
                                  std::make_shared<AlwaysOnSampler>(),
                              const std::shared_ptr<Sampler> &remote_parent_nonsampled_sampler =
                                  std::make_shared<AlwaysOffSampler>(),
                              const std::shared_ptr<Sampler> &local_parent_sampled_sampler =
                                  std::make_shared<AlwaysOnSampler>(),
                              const std::shared_ptr<Sampler> &local_parent_nonsampled_sampler =
                                  std::make_shared<AlwaysOffSampler>()) noexcept;

  /** Implements the decision logic by checking the parent context and delegating to the appropriate
   * configured sampler
   * @return The SamplingResult from the delegated sampler
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
  const std::shared_ptr<Sampler> root_sampler_;
  const std::shared_ptr<Sampler> remote_parent_sampled_sampler_;
  const std::shared_ptr<Sampler> remote_parent_nonsampled_sampler_;
  const std::shared_ptr<Sampler> local_parent_sampled_sampler_;
  const std::shared_ptr<Sampler> local_parent_nonsampled_sampler_;
  const std::string description_;
};

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

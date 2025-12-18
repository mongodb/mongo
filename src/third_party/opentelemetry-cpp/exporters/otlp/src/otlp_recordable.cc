// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include <chrono>
#include <string>

#include "opentelemetry/common/attribute_value.h"
#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/exporters/otlp/otlp_populate_attribute_utils.h"
#include "opentelemetry/exporters/otlp/otlp_recordable.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/trace/trace_flags.h"
#include "opentelemetry/trace/trace_id.h"
#include "opentelemetry/trace/trace_state.h"
#include "opentelemetry/version.h"

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h" // IWYU pragma: keep
#include "opentelemetry/proto/common/v1/common.pb.h"
#include "opentelemetry/proto/resource/v1/resource.pb.h"
#include "opentelemetry/proto/trace/v1/trace.pb.h"
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h" // IWYU pragma: keep
// clang-format on

namespace nostd = opentelemetry::nostd;

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

void OtlpRecordable::SetIdentity(const opentelemetry::trace::SpanContext &span_context,
                                 opentelemetry::trace::SpanId parent_span_id) noexcept
{
  span_.set_trace_id(reinterpret_cast<const char *>(span_context.trace_id().Id().data()),
                     trace::TraceId::kSize);
  span_.set_span_id(reinterpret_cast<const char *>(span_context.span_id().Id().data()),
                    trace::SpanId::kSize);
  if (parent_span_id.IsValid())
  {
    span_.set_parent_span_id(reinterpret_cast<const char *>(parent_span_id.Id().data()),
                             trace::SpanId::kSize);
  }
  span_.set_trace_state(span_context.trace_state()->ToHeader());
}

proto::resource::v1::Resource OtlpRecordable::ProtoResource() const noexcept
{
  proto::resource::v1::Resource proto;
  if (resource_)
  {
    OtlpPopulateAttributeUtils::PopulateAttribute(&proto, *resource_);
  }

  return proto;
}

const opentelemetry::sdk::resource::Resource *OtlpRecordable::GetResource() const noexcept
{
  return resource_;
}

const std::string OtlpRecordable::GetResourceSchemaURL() const noexcept
{
  std::string schema_url;
  if (resource_)
  {
    schema_url = resource_->GetSchemaURL();
  }

  return schema_url;
}

const opentelemetry::sdk::instrumentationscope::InstrumentationScope *
OtlpRecordable::GetInstrumentationScope() const noexcept
{
  return instrumentation_scope_;
}

const std::string OtlpRecordable::GetInstrumentationLibrarySchemaURL() const noexcept
{
  std::string schema_url;
  if (instrumentation_scope_)
  {
    schema_url = instrumentation_scope_->GetSchemaURL();
  }

  return schema_url;
}

proto::common::v1::InstrumentationScope OtlpRecordable::GetProtoInstrumentationScope()
    const noexcept
{
  proto::common::v1::InstrumentationScope instrumentation_scope;
  if (instrumentation_scope_)
  {
    instrumentation_scope.set_name(instrumentation_scope_->GetName());
    instrumentation_scope.set_version(instrumentation_scope_->GetVersion());
    OtlpPopulateAttributeUtils::PopulateAttribute(&instrumentation_scope, *instrumentation_scope_);
  }
  return instrumentation_scope;
}

void OtlpRecordable::SetResource(const sdk::resource::Resource &resource) noexcept
{
  resource_ = &resource;
}

void OtlpRecordable::SetAttribute(nostd::string_view key,
                                  const common::AttributeValue &value) noexcept
{
  auto *attribute = span_.add_attributes();
  OtlpPopulateAttributeUtils::PopulateAttribute(attribute, key, value, false);
}

void OtlpRecordable::AddEvent(nostd::string_view name,
                              common::SystemTimestamp timestamp,
                              const common::KeyValueIterable &attributes) noexcept
{
  auto *event = span_.add_events();
  event->set_name(name.data(), name.size());
  event->set_time_unix_nano(timestamp.time_since_epoch().count());

  attributes.ForEachKeyValue([&](nostd::string_view key, common::AttributeValue value) noexcept {
    OtlpPopulateAttributeUtils::PopulateAttribute(event->add_attributes(), key, value, false);
    return true;
  });
}

void OtlpRecordable::AddLink(const trace::SpanContext &span_context,
                             const common::KeyValueIterable &attributes) noexcept
{
  auto *link = span_.add_links();
  link->set_trace_id(reinterpret_cast<const char *>(span_context.trace_id().Id().data()),
                     trace::TraceId::kSize);
  link->set_span_id(reinterpret_cast<const char *>(span_context.span_id().Id().data()),
                    trace::SpanId::kSize);
  link->set_trace_state(span_context.trace_state()->ToHeader());
  attributes.ForEachKeyValue([&](nostd::string_view key, common::AttributeValue value) noexcept {
    OtlpPopulateAttributeUtils::PopulateAttribute(link->add_attributes(), key, value, false);
    return true;
  });
}

void OtlpRecordable::SetStatus(trace::StatusCode code, nostd::string_view description) noexcept
{
  span_.mutable_status()->set_code(proto::trace::v1::Status_StatusCode(code));
  if (code == trace::StatusCode::kError)
  {
    span_.mutable_status()->set_message(description.data(), description.size());
  }
}

void OtlpRecordable::SetName(nostd::string_view name) noexcept
{
  span_.set_name(name.data(), name.size());
}

void OtlpRecordable::SetTraceFlags(opentelemetry::trace::TraceFlags flags) noexcept
{
  uint32_t all_flags = flags.flags() & opentelemetry::proto::trace::v1::SPAN_FLAGS_TRACE_FLAGS_MASK;

  span_.set_flags(all_flags);
}

void OtlpRecordable::SetSpanKind(trace::SpanKind span_kind) noexcept
{
  proto::trace::v1::Span_SpanKind proto_span_kind =
      proto::trace::v1::Span_SpanKind::Span_SpanKind_SPAN_KIND_UNSPECIFIED;

  switch (span_kind)
  {

    case trace::SpanKind::kInternal:
      proto_span_kind = proto::trace::v1::Span_SpanKind::Span_SpanKind_SPAN_KIND_INTERNAL;
      break;

    case trace::SpanKind::kServer:
      proto_span_kind = proto::trace::v1::Span_SpanKind::Span_SpanKind_SPAN_KIND_SERVER;
      break;

    case trace::SpanKind::kClient:
      proto_span_kind = proto::trace::v1::Span_SpanKind::Span_SpanKind_SPAN_KIND_CLIENT;
      break;

    case trace::SpanKind::kProducer:
      proto_span_kind = proto::trace::v1::Span_SpanKind::Span_SpanKind_SPAN_KIND_PRODUCER;
      break;

    case trace::SpanKind::kConsumer:
      proto_span_kind = proto::trace::v1::Span_SpanKind::Span_SpanKind_SPAN_KIND_CONSUMER;
      break;

    default:
      // shouldn't reach here.
      proto_span_kind = proto::trace::v1::Span_SpanKind::Span_SpanKind_SPAN_KIND_UNSPECIFIED;
  }

  span_.set_kind(proto_span_kind);
}

void OtlpRecordable::SetStartTime(common::SystemTimestamp start_time) noexcept
{
  span_.set_start_time_unix_nano(start_time.time_since_epoch().count());
}

void OtlpRecordable::SetDuration(std::chrono::nanoseconds duration) noexcept
{
  const uint64_t unix_end_time = span_.start_time_unix_nano() + duration.count();
  span_.set_end_time_unix_nano(unix_end_time);
}

void OtlpRecordable::SetInstrumentationScope(
    const opentelemetry::sdk::instrumentationscope::InstrumentationScope
        &instrumentation_scope) noexcept
{
  instrumentation_scope_ = &instrumentation_scope;
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"
#include "mongo/stdx/unordered_map.h"

#include <deque>
#include <string_view>

#include <opentelemetry/sdk/trace/exporter.h>
#include <opentelemetry/sdk/trace/recordable.h>

namespace mongo {
namespace otel {
namespace traces {

class MockRecordable : public opentelemetry::sdk::trace::Recordable {
public:
    MockRecordable() : context(opentelemetry::trace::SpanContext::GetInvalid()) {}

    void SetIdentity(const opentelemetry::trace::SpanContext& spanContext,
                     opentelemetry::trace::SpanId parentSpanId) noexcept override {
        context = spanContext;
        parentId = parentSpanId;
    }

    void SetStatus(opentelemetry::trace::StatusCode spanStatus,
                   std::string_view description) noexcept override {
        status = spanStatus;
    }

    void SetName(std::string_view spanName) noexcept override {
        name = spanName;
    }

    void SetAttribute(std::string_view key,
                      const opentelemetry::common::AttributeValue& value) noexcept override;

    void AddEvent(std::string_view name,
                  opentelemetry::common::SystemTimestamp timestamp,
                  const opentelemetry::common::KeyValueIterable& attributes) noexcept override {}
    void AddLink(const opentelemetry::trace::SpanContext& span_context,
                 const opentelemetry::common::KeyValueIterable& attributes) noexcept override {}
    void SetSpanKind(opentelemetry::trace::SpanKind span_kind) noexcept override {}
    void SetResource(const opentelemetry::sdk::resource::Resource& resource) noexcept override {}
    void SetStartTime(opentelemetry::common::SystemTimestamp start_time) noexcept override {}
    void SetDuration(std::chrono::nanoseconds duration) noexcept override {}
    void SetInstrumentationScope(
        const opentelemetry::sdk::instrumentationscope::InstrumentationScope&
            instrumentation_scope) noexcept override {}

    MockRecordable(const MockRecordable&) = delete;
    MockRecordable& operator=(const MockRecordable&) = delete;

    opentelemetry::trace::SpanContext context;
    opentelemetry::trace::SpanId parentId;
    opentelemetry::trace::StatusCode status = opentelemetry::trace::StatusCode::kUnset;
    std::string name;
    stdx::unordered_map<std::string, opentelemetry::common::AttributeValue> attributes;

private:
    // Owns copies of string attribute values so that string_view entries in `attributes`
    // remain valid for the lifetime of this recordable. std::deque is used because push_back
    // does not invalidate references to existing elements, unlike std::vector.
    std::deque<std::string> _ownedStrings;
};

class MockExporter : public opentelemetry::sdk::trace::SpanExporter {
public:
    bool Shutdown(
        std::chrono::microseconds timeout = std::chrono::microseconds::max()) noexcept override {
        return true;
    }

    bool ForceFlush(
        std::chrono::microseconds timeout = std::chrono::microseconds::max()) noexcept override {
        return true;
    }

    std::unique_ptr<opentelemetry::sdk::trace::Recordable> MakeRecordable() noexcept override;

    opentelemetry::sdk::common::ExportResult Export(
        const opentelemetry::nostd::span<std::unique_ptr<opentelemetry::sdk::trace::Recordable>>&
            spans) noexcept override;

    const std::vector<std::unique_ptr<MockRecordable>>& getSpans() const {
        return _exportedSpans;
    }

private:
    std::vector<std::unique_ptr<MockRecordable>> _exportedSpans;
};

}  // namespace traces
}  // namespace otel
}  // namespace mongo

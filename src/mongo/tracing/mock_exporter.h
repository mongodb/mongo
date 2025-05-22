/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/config.h"

#include <opentelemetry/sdk/trace/exporter.h>
#include <opentelemetry/sdk/trace/recordable.h>

#include "mongo/stdx/unordered_map.h"

namespace mongo {
namespace tracing {

class MockRecordable : public opentelemetry::sdk::trace::Recordable {
public:
    MockRecordable() : context(opentelemetry::trace::SpanContext::GetInvalid()) {}

    void SetIdentity(const opentelemetry::trace::SpanContext& spanContext,
                     opentelemetry::trace::SpanId parentSpanId) noexcept override {
        context = spanContext;
        parentId = parentSpanId;
    }

    void SetStatus(opentelemetry::trace::StatusCode spanStatus,
                   opentelemetry::nostd::string_view description) noexcept override {
        status = spanStatus;
    }

    void SetName(opentelemetry::nostd::string_view spanName) noexcept override {
        name = spanName;
    }

    void SetAttribute(opentelemetry::nostd::string_view key,
                      const opentelemetry::common::AttributeValue& value) noexcept override;

    void AddEvent(opentelemetry::nostd::string_view name,
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

    opentelemetry::trace::SpanContext context;
    opentelemetry::trace::SpanId parentId;
    opentelemetry::trace::StatusCode status = opentelemetry::trace::StatusCode::kUnset;
    std::string name;
    stdx::unordered_map<std::string, opentelemetry::common::AttributeValue> attributes;
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

}  // namespace tracing
}  // namespace mongo

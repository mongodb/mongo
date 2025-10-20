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

#include "mongo/base/string_data.h"
#include "mongo/otel/telemetry_context.h"

#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/trace/context.h>

namespace mongo {
namespace otel {
namespace traces {

using OtelContext = opentelemetry::context::Context;
using ScopedSpan = opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>;
using OtelStringView = opentelemetry::nostd::string_view;
using TextMapPropagator = opentelemetry::context::propagation::TextMapPropagator;
using TextMapCarrier = opentelemetry::context::propagation::TextMapCarrier;

constexpr OtelStringView keepSpanKey = "keepSpan";
constexpr OtelStringView trueValue = "true";
constexpr OtelStringView falseValue = "false";

/**
 * SpanTelemetryContextImpl is an implementation of TelemetryContext that wraps OpenTelemetry's
 * Context to allow for propagation of span state across OpenTelemetry functionality.
 */
class SpanTelemetryContextImpl : public TelemetryContext {
public:
    explicit SpanTelemetryContextImpl(OtelContext ctx) : _ctx(std::move(ctx)) {}
    SpanTelemetryContextImpl() : _ctx() {}

    /**
     * Sets whether spans created with this context should be kept (exported) or not.
     */
    void keepSpan(bool keepSpan);

    /**
     * Returns whether spans created with this context should be kept (exported) or not.
     */
    bool shouldKeepSpan() const;

    /**
     * Sets the provided span as the current span on this context.
     */
    void setSpan(ScopedSpan span) {
        _ctx = opentelemetry::trace::SetSpan(_ctx, span);
    }

    /**
     * Returns the current span on this context.
     */
    ScopedSpan getSpan() {
        return opentelemetry::trace::GetSpan(_ctx);
    }

    StringData type() const override {
        return "SpanTelemetryContextImpl";
    }

    void propagate(TextMapPropagator& propagator, TextMapCarrier& carrier) const;

private:
    OtelContext _ctx;
};

}  // namespace traces
}  // namespace otel
}  // namespace mongo

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

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/operation_context.h"
#include "mongo/otel/telemetry_context.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {
namespace otel {
namespace [[MONGO_MOD_PUBLIC]] traces {

#ifdef MONGO_CONFIG_OTEL

/**
 * Set an attribute with `key` and `value` on the provided `span`.
 */
#define TRACING_SPAN_ATTR(span, key, value) \
    do {                                    \
        span.setAttribute(key, value);      \
    } while (0)

/**
 * Span class is an RAII utility to create and save OpenTelemetry spans. Whether or not the span
 * is used within an exported OpenTelemetry trace is determined by a combination of whether its
 * parent is in a trace and the sampling decision made by the `TracingSampler`.
 *
 * Spans should be created by calling the Span::start free function defined below to ensure new
 * Spans have a valid parent-child relationships with other Spans in the Trace.
 */
class Span {
    class SpanImpl;

public:
    /**
     * Starts a new Span with the provided `name` and using the provided `telemetryCtx` to allow for
     * propagation of parent-child relationships. If a `telemetryCtx` is not provided but will be
     * needed going forward, `telemetryCtx` will be populated with a newly created one.
     */
    static Span start(std::shared_ptr<TelemetryContext>& telemetryCtx, SpanName name);

    /**
     * Wrapper around the other start function. It will also fetch and store the current
     * TelemetryContext from the provided OperationContext. This function is preferred when
     * OperationContext is available so that the calling code does not have to manage its own
     * TelemetryContext.
     */
    static Span start(OperationContext* opCtx, SpanName name);

    static std::shared_ptr<TelemetryContext> createTelemetryContext();

    ~Span();
    Span& operator=(Span&&) noexcept;
    Span(Span&&) noexcept;

    /**
     * Caller should use `TRACING_SPAN_ATTR` instead of calling `setAttribute` directly.
     *
     * Adds an integer attribute with `key` and `value` to this Span. This attribute MUST NOT
     * contain PII.
     */
    void setAttribute(std::string_view key, int value);

    /**
     * Caller should use `TRACING_SPAN_ATTR` instead of calling `setAttribute` directly.
     *
     * Adds a string attribute with `key` and `value` to this Span. This attribute MUST NOT
     * contain PII.
     */
    void setAttribute(std::string_view key, std::string_view value);

    /**
     * Set the status associated with this Span. If the status's code is non-zero the OpenTelemetry
     * span associated will have a status of `StatusCode::kError`.
     */
    void setStatus(const Status& status);

private:
    /** Construction of Spans should be done through Span::start(context, name). */
    Span();
    Span(std::unique_ptr<SpanImpl> impl);

    /** The actual span implementation. Null if this Span will not be part of an exported trace. */
    std::unique_ptr<SpanImpl> _impl;
};

#else

#define TRACING_SPAN_ATTR(span, key, value) \
    do {                                    \
    } while (0)

/**
 * Provide empty definitions when OpenTelemetry is disabled at compile-time to ensure there is no
 * overhead from unused tracepoints.
 */
class Span {
public:
    static Span start(OperationContext* opCtx, SpanName) {
        return Span{};
    }
    static Span start(std::shared_ptr<TelemetryContext>& telemetryCtx, SpanName) {
        return Span{};
    }

    static std::shared_ptr<TelemetryContext> createTelemetryContext() {
        return std::make_shared<TelemetryContext>();
    }

    ~Span() {}

    void setAttribute(std::string_view, int) {}
    void setAttribute(std::string_view, std::string_view) {}
    void setStatus(const Status&) {}
};

#endif

}  // namespace traces
}  // namespace otel
}  // namespace mongo

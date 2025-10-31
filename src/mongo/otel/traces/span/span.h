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
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {
namespace otel {
namespace MONGO_MOD_PUBLIC traces {

#ifdef MONGO_CONFIG_OTEL

/**
 * Set an attribute with `key` and `value` on the provided `span`.
 */
#define TRACING_SPAN_ATTR(span, key, value) \
    do {                                    \
        span.setAttribute(key, value);      \
    } while (0)

/**
 * Span class is an RAII utility to create and save OpenTelemetry spans. A Span that has a
 * non-null _impl field is considered as valid and will generate a new OpenTelemetry's span upon
 * destruction. A Span that has an empty _impl field is a no-op Span and will not generate an
 * OpenTelemetry Span.
 *
 * Spans should be created by calling the Span::start free function defined below to ensure new
 * Spans have a valid parent-child relationships with other Spans in the Trace.
 */
class Span {
    class SpanImpl;

public:
    /**
     * Starts a new Span with the provided `name` and using the provided `telemetryCtx` to allow for
     * propagation of parent-child relationships. The `keepSpan` parameter indicates whether this
     * Span will be ingested by our Trace backend or dropped. It should only be set if you have
     * confirmed it is valid to send the Span to the Trace backend.
     */
    static Span start(std::shared_ptr<TelemetryContext>& telemetryCtx,
                      const std::string& name,
                      bool keepSpan = false);

    /**
     * Wrapper around the other start function. It will also fetch and store the current
     * TelemetryContext from the provided OperationContext. This function is preferred when
     * OperationContext is available so that the calling code does not have to manage its own
     * TelemetryContext.
     */
    static Span start(OperationContext* opCtx, const std::string& name, bool keepSpan = false);

    /**
     * Similar to `start`, but only starts and returns a Span if there is an existing Span in the
     * provided `opCtx`'s TelemetryContext. If there is no existing Span, a no-op Span is returned.
     */
    static Span startIfExistingTraceParent(OperationContext* opCtx,
                                           const std::string& name,
                                           bool keepSpan = false);

    static std::shared_ptr<TelemetryContext> createTelemetryContext();

    ~Span();
    Span& operator=(Span&&);
    Span(Span&&);

    /**
     * Caller should use `TRACING_SPAN_ATTR` instead of calling `setAttribute` directly.
     *
     * Adds an integer attribute with `key` and `value` to this Span. This attribute MUST NOT
     * contain PII.
     */
    void setAttribute(StringData key, int value);

    /**
     * Set the status associated with this Span. If the status's code is non-zero the OpenTelemetry
     * span associated will have a status of `StatusCode::kError`.
     */
    void setStatus(const Status& status);

private:
    std::unique_ptr<SpanImpl> _impl;
    /**
     * Construction of Spans should be done through Span::start(traceable, name).
     */
    Span();
    Span(std::unique_ptr<SpanImpl> impl);
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
    static Span start(OperationContext* opCtx, const std::string&, bool keepSpan = false) {
        return Span{};
    }
    static Span start(std::shared_ptr<TelemetryContext> telemetryCtx,
                      const std::string& name,
                      bool keepSpan = false) {
        return Span{};
    }

    static Span startIfExistingTraceParent(OperationContext* opCtx,
                                           const std::string& name,
                                           bool keepSpan = false) {
        return Span{};
    }

    static std::shared_ptr<TelemetryContext> createTelemetryContext() {
        return std::make_shared<TelemetryContext>();
    }

    ~Span() {}

    void setAttribute(StringData, int) {}
    void setError(const Status&) {}
};

#endif

}  // namespace MONGO_MOD_PUBLIC traces
}  // namespace otel
}  // namespace mongo

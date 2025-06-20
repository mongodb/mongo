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
#include "mongo/otel/traces/traceable.h"

#include <memory>

namespace mongo {
namespace otel {
namespace traces {

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

    /**
     * Construction of Spans should be done through Span::start(traceable, name).
     */
    Span();
    Span(std::unique_ptr<SpanImpl> impl);

public:
    /**
     * When tracing is enabled and a valid Traceable is provided, Span::start creates a valid Span.
     * This Span becomes a child of the Traceable's active span (if it exists) and Span::start
     * replaces the Traceable's context with the new Span's context.
     *
     * If Traceable is null or tracing is disabled, this returns an empty no-op span.
     */
    static Span start(Traceable* traceable, const std::string& name);

    ~Span();
    Span& operator=(Span&&);

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
    static Span start(Traceable* traceable, const std::string&) {
        return Span{};
    }

    void setAttribute(StringData, int) {}
    void setError(const Status&) {}
};

#endif

}  // namespace traces
}  // namespace otel
}  // namespace mongo

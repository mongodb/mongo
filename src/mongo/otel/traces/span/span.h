// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

/** The kind of span. */
enum class SpanKind {
    /** A span started and ended internally in the current process. */
    kInternal,
    /** A span started as the result of an incoming RPC for which a response will be sent. */
    kServer,
    /** A span started as part of sending an outgoing RPC for which a response will be received. */
    kClient,
    /**
     * A span initiating some work that may be completed after this span ends. This could be a
     * fire-and-forget RPC, or something starting internal background work.
     */
    kProducer,
    /** A span for work initiated by a span of kind `kProducer`. */
    kConsumer,
};

struct SpanOptions {
    SpanKind kind = SpanKind::kInternal;
};

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
    static Span start(std::shared_ptr<TelemetryContext>& telemetryCtx,
                      SpanName name,
                      SpanOptions options = {});

    /**
     * Wrapper around the other start function. It will also fetch and store the current
     * TelemetryContext from the provided OperationContext. This function is preferred when
     * OperationContext is available so that the calling code does not have to manage its own
     * TelemetryContext.
     */
    static Span start(OperationContext* opCtx, SpanName name, SpanOptions options = {});

    /**
     * Starts a new Span from an ingress source, which may be sampled differently than an
     * internally-started span (e.g. a separate rate limit). Uses the presence of `telemetryCtx` to
     * determine if the ingress span is part of an external trace. Defaults to SERVER span kind. If
     * a context is created during this, `telemetryCtx` is updated in place.
     */
    static Span startIngressSpan(std::shared_ptr<TelemetryContext>& telemetryCtx,
                                 SpanName name,
                                 SpanOptions options = {.kind = SpanKind::kServer});

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

    // Internal start functions that allow bypassing the sampling mechanism to create the span
    // unconditonally. These are used by the public start functions to control whether the span
    // should be sampled or not.
    static Span _start(std::shared_ptr<TelemetryContext>& telemetryCtx,
                       SpanName name,
                       bool bypassSampling,
                       SpanKind kind);
    static Span _start(OperationContext* opCtx, SpanName name, bool bypassSampling, SpanKind kind);

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
    static Span start(OperationContext* opCtx, SpanName, SpanOptions = {}) {
        return Span{};
    }
    static Span start(std::shared_ptr<TelemetryContext>& telemetryCtx, SpanName, SpanOptions = {}) {
        return Span{};
    }
    static Span startIngressSpan(std::shared_ptr<TelemetryContext>&,
                                 SpanName,
                                 SpanOptions = {.kind = SpanKind::kServer}) {
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

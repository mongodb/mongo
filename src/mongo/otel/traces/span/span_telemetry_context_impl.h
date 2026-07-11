// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/telemetry_context.h"
#include "mongo/platform/random.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional.hpp>
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/trace/context.h>

namespace mongo {
namespace otel {
namespace traces {

using OtelContext = opentelemetry::context::Context;
using ScopedSpan = opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>;
using TextMapPropagator = opentelemetry::context::propagation::TextMapPropagator;
using TextMapCarrier = opentelemetry::context::propagation::TextMapCarrier;

/**
 * SpanTelemetryContextImpl is an implementation of TelemetryContext that wraps OpenTelemetry's
 * Context to allow for propagation of span state across OpenTelemetry functionality.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] SpanTelemetryContextImpl : public TelemetryContext {
public:
    explicit SpanTelemetryContextImpl(OtelContext ctx, PseudoRandom* prng = nullptr);
    SpanTelemetryContextImpl();

    /**
     * Returns this telemetry context's sampling roll: a value in [0, 1) used to make sampling
     * decisions. The value is drawn lazily from the constructor-supplied PRNG on the first call and
     * then memoized, so every span created on this context observes the same value.
     *
     * Drawing a single value per telemetry context (rather than rolling independently per span) is
     * what keeps the overall trace rate constant when sampling-eligible spans overlap within one
     * operation -- e.g. when a request traverses both the sharded and DSC paths.
     */
    double getSamplingValue();

    bool hasActiveTrace() const override {
        return getSpan()->GetContext().IsValid();
    }

    /**
     * Sets the provided span as the current span on this context.
     */
    void setSpan(ScopedSpan span) {
        _ctx = opentelemetry::trace::SetSpan(_ctx, span);
    }

    /**
     * Returns the current span on this context.
     */
    ScopedSpan getSpan() const {
        return opentelemetry::trace::GetSpan(_ctx);
    }

    std::string_view type() const override {
        return "SpanTelemetryContextImpl";
    }

    void propagate(TextMapPropagator& propagator, TextMapCarrier& carrier) const;

    std::shared_ptr<TelemetryContext> clone() const override {
        return std::make_shared<SpanTelemetryContextImpl>(*this);
    }

private:
    OtelContext _ctx;
    PseudoRandom* _prng{nullptr};

    // The sampling roll for this telemetry context: a value in [0, 1) drawn lazily on first use
    // and reused for the lifetime of the context. See getSamplingValue().
    boost::optional<double> _samplingRoll;
};

}  // namespace traces
}  // namespace otel
}  // namespace mongo

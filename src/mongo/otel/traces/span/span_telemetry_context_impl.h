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
class MONGO_MOD_NEEDS_REPLACEMENT SpanTelemetryContextImpl : public TelemetryContext {
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

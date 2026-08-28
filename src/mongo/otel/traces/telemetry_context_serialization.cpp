// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/telemetry_context_serialization.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/otel/traces/bson_text_map_carrier.h"
#include "mongo/otel/traces/span/span_telemetry_context_impl.h"
#include "mongo/otel/traces/traceparent.h"
#include "mongo/rpc/telemetry_context_section_gen.h"

#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>

namespace mongo {
namespace otel {
namespace traces {

using opentelemetry::context::propagation::TextMapPropagator;
using opentelemetry::trace::propagation::HttpTraceContext;
using OtelContext = opentelemetry::context::Context;

namespace {
// TODO: TextMapPropagator's interface (defined by the C++ Otel library) does not appear to mark
// Inject/Extract as const, despite it being required according to my reading of the spec:
// "Getter and Setter MUST be stateless and allowed to be saved as constants, in order to
// effectively avoid runtime allocations."
// https://opentelemetry.io/docs/specs/otel/context/api-propagators/#textmap-propagator
// Unless this is fixed, create the propagator on demand.
HttpTraceContext getPropagator() {
    return HttpTraceContext{};
}

std::shared_ptr<TelemetryContext> fromBSON(const BSONObj& bson, TextMapPropagator& propagator) {
    auto carrier = BSONTextMapCarrier{bson};
    auto context = OtelContext{};
    context = propagator.Extract(carrier, context);
    return std::make_shared<traces::SpanTelemetryContextImpl>(std::move(context));
}

BSONObj toBSON(const TelemetryContext& context, TextMapPropagator& propagator) {
    try {
        auto carrier = BSONTextMapCarrier{};
        const auto* typedContext = dynamic_cast<const traces::SpanTelemetryContextImpl*>(&context);
        tassert(10012000, "Bad cast", typedContext);
        typedContext->propagate(propagator, carrier);
        return carrier.toBSON();
    } catch (const AssertionException&) {
        return {};
    }
}

std::shared_ptr<TelemetryContext> fromSection(const TelemetryContextSection& section,
                                              TextMapPropagator& propagator) {
    auto mapCarrier = BSONTextMapCarrier{section};
    auto context = OtelContext{};
    context = propagator.Extract(mapCarrier, context);
    return std::make_shared<SpanTelemetryContextImpl>(std::move(context));
}

boost::optional<mongo::TelemetryContextSection> toSection(const TelemetryContext& context,
                                                          TextMapPropagator& propagator) {
    try {
        auto mapCarrier = BSONTextMapCarrier{};
        const auto* typedContext = dynamic_cast<const SpanTelemetryContextImpl*>(&context);
        tassert(10012001, "Bad cast", typedContext);
        typedContext->propagate(propagator, mapCarrier);
        auto traceParent = mapCarrier.Get(BSONTextMapCarrier::kTraceParentKey);
        if (traceParent.empty() || traceParent == kMissingKeyReturnValue) {
            return boost::none;
        }
        return mongo::TelemetryContextSection{OtelContextSection{std::string{traceParent}}};
    } catch (const AssertionException&) {
        return boost::none;
    }
}

}  // namespace

std::shared_ptr<TelemetryContext> TelemetryContextSerializer::fromBSON(const BSONObj& bson) {
    auto propagator = getPropagator();
    return traces::fromBSON(bson, propagator);
}

BSONObj TelemetryContextSerializer::toBSON(const std::shared_ptr<TelemetryContext>& context) {
    auto propagator = getPropagator();
    return traces::toBSON(*context, propagator);
}

std::shared_ptr<TelemetryContext> TelemetryContextSerializer::fromSection(
    const boost::optional<mongo::TelemetryContextSection>& section) {
    if (!section || section->getOtel().getTraceparent().empty() ||
        !validateW3CTraceparent(section->getOtel().getTraceparent()).isOK()) {
        return nullptr;
    }
    auto propagator = getPropagator();
    return traces::fromSection(*section, propagator);
}

boost::optional<mongo::TelemetryContextSection> TelemetryContextSerializer::toSection(
    const TelemetryContext* context) {
    if (!context) {
        return boost::none;
    }
    auto propagator = getPropagator();
    return traces::toSection(*context, propagator);
}

}  // namespace traces
}  // namespace otel
}  // namespace mongo

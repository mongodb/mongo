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

#include "mongo/otel/traces/telemetry_context_serialization.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/bson_text_map_carrier.h"
#include "mongo/otel/traces/span/span_telemetry_context_impl.h"
#include "mongo/otel/traces/traceparent.h"
#include "mongo/rpc/telemetry_context_section_gen.h"

#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

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

BSONObj TelemetryContextSerializer::appendTelemetryContext(OperationContext* opCtx, BSONObj bson) {
    invariant(opCtx);
    auto& telemetryCtxHolder = TelemetryContextHolder::getDecoration(opCtx);
    if (!telemetryCtxHolder.getTelemetryContext()) {
        return bson;
    }

    BSONObjBuilder bob;
    for (const auto& field : bson) {
        if (field.fieldName() == GenericArguments::kTraceCtxFieldName) {
            continue;
        }
        bob.append(field);
    }
    bob.append(GenericArguments::kTraceCtxFieldName,
               TelemetryContextSerializer::toBSON(telemetryCtxHolder.getTelemetryContext()));

    return bob.obj();
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
    const std::shared_ptr<TelemetryContext>& context) {
    if (!context) {
        return boost::none;
    }
    auto propagator = getPropagator();
    return traces::toSection(*context, propagator);
}

boost::optional<TelemetryContextSection> toWireType(const TelemetryContext* ctx) {
    if (!ctx) {
        return boost::none;
    }
    // Reuse the existing BSON serialization rather than duplicating the propagator/carrier logic,
    // then pull the traceparent field off the result.
    // TODO(SERVER-130639): Separate the serialization from the propagator logic.
    auto propagator = getPropagator();
    auto bson = traces::toBSON(*ctx, propagator);
    auto traceparent = bson.getStringField(OtelContextSection::kTraceparentFieldName);
    if (traceparent.empty()) {
        return boost::none;
    }
    OtelContextSection otelCtx;
    otelCtx.setTraceparent(std::string{traceparent});
    TelemetryContextSection wireTc;
    wireTc.setOtel(std::move(otelCtx));
    return wireTc;
}

}  // namespace traces
}  // namespace otel
}  // namespace mongo

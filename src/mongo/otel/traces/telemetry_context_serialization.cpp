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

#include <opentelemetry/baggage/propagation/baggage_propagator.h>
#include <opentelemetry/context/propagation/composite_propagator.h>
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace otel {
namespace traces {

using opentelemetry::baggage::propagation::BaggagePropagator;
using opentelemetry::context::propagation::CompositePropagator;
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
auto getPropagator() {
    std::vector<std::unique_ptr<TextMapPropagator>> propagators;
    propagators.emplace_back(std::make_unique<BaggagePropagator>());
    propagators.emplace_back(std::make_unique<HttpTraceContext>());
    return CompositePropagator{std::move(propagators)};
}
}  // namespace

std::shared_ptr<TelemetryContext> TelemetryContextSerializer::fromBSON(const BSONObj& bson) {
    auto propagator = getPropagator();
    return detail::fromBSON(bson, propagator);
}

BSONObj TelemetryContextSerializer::toBSON(const std::shared_ptr<TelemetryContext>& context) {
    auto propagator = getPropagator();
    return detail::toBSON(*context, propagator);
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

namespace detail {
std::shared_ptr<TelemetryContext> fromBSON(const BSONObj& bson, TextMapPropagator& propagator) {
    BSONTextMapCarrier carrier{bson};
    OtelContext context;
    context = propagator.Extract(carrier, context);
    return std::make_shared<traces::SpanTelemetryContextImpl>(std::move(context));
}

BSONObj toBSON(const TelemetryContext& context, TextMapPropagator& propagator) {
    try {
        BSONTextMapCarrier carrier;
        const auto* typedContext = dynamic_cast<const traces::SpanTelemetryContextImpl*>(&context);
        tassert(10012000, "Bad cast", typedContext);
        typedContext->propagate(propagator, carrier);
        return carrier.toBSON();
    } catch (const AssertionException&) {
        return {};
    }
}
}  // namespace detail

}  // namespace traces
}  // namespace otel
}  // namespace mongo

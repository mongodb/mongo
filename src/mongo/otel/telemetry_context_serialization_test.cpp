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

#include "mongo/otel/telemetry_context_serialization.h"

#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/bson_text_map_carrier.h"
#include "mongo/otel/traces/otel_test_fixture.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_telemetry_context_impl.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <opentelemetry/baggage/propagation/baggage_propagator.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>

namespace mongo {
namespace otel {
namespace {

using DefaultSpan = opentelemetry::trace::DefaultSpan;
using opentelemetry::baggage::propagation::BaggagePropagator;
using opentelemetry::trace::propagation::HttpTraceContext;

class TelemetryContextSerializationTest : public traces::OtelTestFixture {
protected:
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagTracing", true};
};

BSONObj serializeTraceContextOnly(const std::shared_ptr<TelemetryContext>& context) {
    HttpTraceContext propagator;
    return detail::toBSON(*context, propagator);
}

BSONObj serializeBaggageOnly(const std::shared_ptr<TelemetryContext>& context) {
    BaggagePropagator propagator;
    return detail::toBSON(*context, propagator);
}

std::shared_ptr<TelemetryContext> performRoundTrip(
    const std::shared_ptr<TelemetryContext>& context) {
    auto bson = TelemetryContextSerializer::toBSON(context);
    return TelemetryContextSerializer::fromBSON(bson);
}

bool getKeepSpan(const std::shared_ptr<TelemetryContext>& context) {
    const auto* typed = dynamic_cast<const traces::SpanTelemetryContextImpl*>(context.get());
    return typed->shouldKeepSpan();
}

TEST_F(TelemetryContextSerializationTest, SerializeTraceContext) {
    auto context = traces::Span::createTelemetryContext();
    auto span = traces::Span::start(context, "TestSpan");
    BSONObj fullBson = TelemetryContextSerializer::toBSON(context);
    BSONObj traceContextBson = serializeTraceContextOnly(context);
    ASSERT(!traceContextBson.isEmpty());
    for (const auto& field : traceContextBson) {
        const auto& name = field.fieldName();
        ASSERT(fullBson.hasField(name));
        ASSERT_EQ(fullBson.getStringField(name), traceContextBson.getStringField(name));
    }
}

TEST_F(TelemetryContextSerializationTest, SerializeBaggage) {
    auto context = traces::Span::createTelemetryContext();
    auto span = traces::Span::start(context, "TestSpan");
    BSONObj fullBson = TelemetryContextSerializer::toBSON(context);
    BSONObj baggageBson = serializeBaggageOnly(context);
    ASSERT(!baggageBson.isEmpty());
    for (const auto& field : baggageBson) {
        const auto& name = field.fieldName();
        ASSERT(fullBson.hasField(name));
        ASSERT_EQ(fullBson.getStringField(name), baggageBson.getStringField(name));
    }
}

TEST_F(TelemetryContextSerializationTest, RoundTrip) {
    auto context = traces::Span::createTelemetryContext();
    auto span = traces::Span::start(context, "TestSpan");
    BSONObj bson = TelemetryContextSerializer::toBSON(context);
    auto rehydratedContext = TelemetryContextSerializer::fromBSON(bson);
    ASSERT_BSONOBJ_EQ_UNORDERED(bson, TelemetryContextSerializer::toBSON(rehydratedContext));
}

TEST_F(TelemetryContextSerializationTest, KeepSpan) {
    auto context = traces::Span::createTelemetryContext();
    ASSERT_FALSE(getKeepSpan(context));
    auto span = traces::Span::start(context, "TestSpan", true);
    ASSERT_TRUE(getKeepSpan(context));
    context = performRoundTrip(context);
    ASSERT_TRUE(getKeepSpan(context));
}

TEST_F(TelemetryContextSerializationTest, AppendTelemetryContextReturnsIfNoTelemetryContext) {
    BSONObj originalBson = BSON("key" << "value");
    auto opCtx = makeOperationContext();
    BSONObj resultBson =
        TelemetryContextSerializer::appendTelemetryContext(opCtx.get(), originalBson);
    ASSERT_BSONOBJ_EQ(originalBson, resultBson);
}

TEST_F(TelemetryContextSerializationTest, AppendTelemetryContextAddsTelemetryContextIfExists) {
    BSONObj originalBson = BSON("key" << "value");
    auto opCtx = makeOperationContext();
    auto& telemetryContextHolder = TelemetryContextHolder::get(opCtx.get());
    auto telemetryContext = traces::Span::createTelemetryContext();
    telemetryContextHolder.set(telemetryContext);
    BSONObj resultBson =
        TelemetryContextSerializer::appendTelemetryContext(opCtx.get(), originalBson);
    ASSERT_BSONOBJ_NE(originalBson, resultBson);
    ASSERT_TRUE(resultBson.hasField(GenericArguments::kTraceCtxFieldName));
}

TEST_F(TelemetryContextSerializationTest,
       AppendTelemetryContextAddsTelemetryContextIfExistsAndReplacesBSONFieldIfExists) {
    BSONObj originalBson =
        BSON("key" << "value" << GenericArguments::kTraceCtxFieldName << "old_value");
    auto opCtx = makeOperationContext();
    auto& telemetryContextHolder = TelemetryContextHolder::get(opCtx.get());
    auto telemetryContext = traces::Span::createTelemetryContext();
    telemetryContextHolder.set(telemetryContext);
    BSONObj resultBson =
        TelemetryContextSerializer::appendTelemetryContext(opCtx.get(), originalBson);
    ASSERT_BSONOBJ_NE(originalBson, resultBson);
    ASSERT_TRUE(resultBson.hasField(GenericArguments::kTraceCtxFieldName));
}

}  // namespace
}  // namespace otel
}  // namespace mongo

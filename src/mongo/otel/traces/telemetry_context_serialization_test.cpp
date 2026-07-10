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

#include "mongo/idl/generic_argument_gen.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/bson_text_map_carrier.h"
#include "mongo/otel/traces/otel_test_fixture.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/span/span_telemetry_context_impl.h"
#include "mongo/rpc/telemetry_context_section_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace otel {
namespace traces {
namespace {

class TelemetryContextSerializationTest : public traces::OtelTestFixture {
protected:
    unittest::ServerParameterGuard _featureFlagTracingController{"featureFlagTracing", true};
    unittest::ServerParameterGuard _featureFlagSamplingController{"featureFlagOtelTraceSampling",
                                                                  true};
    ScopedSamplerOverride _samplerGuard =
        setTraceSamplingFnForTest([](std::string_view, double) { return true; });
};

TEST_F(TelemetryContextSerializationTest, SerializeTraceContext) {
    auto context = traces::Span::createTelemetryContext();
    auto span = traces::Span::start(context, traces::span_names::kTest1);
    BSONObj bson = TelemetryContextSerializer::toBSON(context);
    EXPECT_TRUE(bson.hasField(BSONTextMapCarrier::kTraceParentKey));
}

TEST_F(TelemetryContextSerializationTest, RoundTrip) {
    auto context = traces::Span::createTelemetryContext();
    auto span = traces::Span::start(context, traces::span_names::kTest1);
    BSONObj bson = TelemetryContextSerializer::toBSON(context);
    auto rehydratedContext = TelemetryContextSerializer::fromBSON(bson);
    ASSERT_BSONOBJ_EQ_UNORDERED(bson, TelemetryContextSerializer::toBSON(rehydratedContext));
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
    auto& telemetryContextHolder = TelemetryContextHolder::getDecoration(opCtx.get());
    telemetryContextHolder.setTelemetryContext(traces::Span::createTelemetryContext());
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
    auto& telemetryContextHolder = TelemetryContextHolder::getDecoration(opCtx.get());
    telemetryContextHolder.setTelemetryContext(traces::Span::createTelemetryContext());
    BSONObj resultBson =
        TelemetryContextSerializer::appendTelemetryContext(opCtx.get(), originalBson);
    ASSERT_BSONOBJ_NE(originalBson, resultBson);
    ASSERT_TRUE(resultBson.hasField(GenericArguments::kTraceCtxFieldName));
}

TEST_F(TelemetryContextSerializationTest, FromSectionReturnsNulloptIfNoSection) {
    auto section = boost::optional<TelemetryContextSection>{};
    auto context = TelemetryContextSerializer::fromSection(section);
    ASSERT(!context);
}

TEST_F(TelemetryContextSerializationTest, ToSectionReturnsNulloptIfNoContext) {
    auto context = std::shared_ptr<TelemetryContext>{};
    auto section = TelemetryContextSerializer::toSection(context);
    ASSERT(!section);
}

TEST_F(TelemetryContextSerializationTest, FromSectionAndToSectionRoundTrip) {
    auto context = traces::Span::createTelemetryContext();
    auto span = traces::Span::start(context, traces::span_names::kTest1);
    auto section = TelemetryContextSerializer::toSection(context);
    ASSERT(section);
    auto rehydratedContext = TelemetryContextSerializer::fromSection(section);
    ASSERT(rehydratedContext);
    auto rehydratedSection = TelemetryContextSerializer::toSection(rehydratedContext);
    ASSERT(rehydratedSection);
    EXPECT_THAT(section->getOtel().getTraceparent(), testing::Not(testing::Eq("")));
    EXPECT_EQ(section->getOtel().getTraceparent(), rehydratedSection->getOtel().getTraceparent());
}

TEST_F(TelemetryContextSerializationTest, FromSectionReturnsNulloptIfNoTraceparent) {
    auto section = TelemetryContextSection{OtelContextSection{""}};
    auto context = TelemetryContextSerializer::fromSection(section);
    ASSERT(!context);
}

TEST_F(TelemetryContextSerializationTest, ToSectionReturnsNulloptIfNoTraceparent) {
    auto context = traces::Span::createTelemetryContext();
    auto section = TelemetryContextSerializer::toSection(context);
    ASSERT(!section);
    auto rehydratedContext = TelemetryContextSerializer::fromSection(section);
    ASSERT(!rehydratedContext);
}

TEST_F(TelemetryContextSerializationTest, FromSectionReturnsNulloptIfBadTraceparent) {
    auto section = TelemetryContextSection{OtelContextSection{"not-a-valid-traceparent"}};
    auto context = TelemetryContextSerializer::fromSection(section);
    ASSERT(!context);
}

TEST_F(TelemetryContextSerializationTest, FromBSONMarksContextAsRemote) {
    auto context = TelemetryContextSerializer::fromBSON(
        BSON("traceparent" << "00-11111111111111111111111111111111-2222222222222222-01"));
    ASSERT(context);
    auto spanContext =
        static_cast<SpanTelemetryContextImpl*>(context.get())->getSpan()->GetContext();
    EXPECT_TRUE(spanContext.IsValid());
    EXPECT_TRUE(spanContext.IsRemote());
}

TEST_F(TelemetryContextSerializationTest, LocallyCreatedContextIsNotRemote) {
    auto context = traces::Span::createTelemetryContext();
    auto span = traces::Span::start(context, traces::span_names::kTest1);
    auto spanContext =
        static_cast<SpanTelemetryContextImpl*>(context.get())->getSpan()->GetContext();
    EXPECT_TRUE(spanContext.IsValid());
    EXPECT_FALSE(spanContext.IsRemote());
}

TEST_F(TelemetryContextSerializationTest, ToWireTypeNullInputReturnsNull) {
    EXPECT_FALSE(toWireType(nullptr).has_value());
}

TEST_F(TelemetryContextSerializationTest, ToWireTypeNoActiveSpanReturnsNull) {
    auto context = traces::Span::createTelemetryContext();
    EXPECT_FALSE(toWireType(context.get()).has_value());
}

TEST_F(TelemetryContextSerializationTest, ToWireTypeActiveSpanReturnsWireType) {
    auto context = traces::Span::createTelemetryContext();
    auto span = traces::Span::start(context, traces::span_names::kTest1);
    auto wireType = toWireType(context.get());
    ASSERT_TRUE(wireType.has_value());
    EXPECT_FALSE(wireType->getOtel().getTraceparent().empty());
}

TEST_F(TelemetryContextSerializationTest, ToWireTypeTraceparentMatchesBSONSerialization) {
    auto context = traces::Span::createTelemetryContext();
    auto span = traces::Span::start(context, traces::span_names::kTest1);

    auto wireType = toWireType(context.get());
    ASSERT_TRUE(wireType.has_value());

    BSONObj bson = TelemetryContextSerializer::toBSON(context);
    auto traceparentFromBson = bson.getStringField(BSONTextMapCarrier::kTraceParentKey);
    ASSERT_EQ(wireType->getOtel().getTraceparent(), traceparentFromBson);
}

}  // namespace
}  // namespace traces
}  // namespace otel
}  // namespace mongo

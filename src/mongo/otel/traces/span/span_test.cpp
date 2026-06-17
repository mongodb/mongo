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

#include "mongo/otel/traces/span/span.h"

#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/otel_test_fixture.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/unittest/server_parameter_guard.h"

namespace mongo {
namespace otel {
namespace traces {
namespace {

class SpanTest : public OtelTestFixture {
protected:
    // Enable OTel sampling for all span tests; individual tests may override.
    unittest::ServerParameterGuard _samplingFlagController{"featureFlagOtelTraceSampling", true};
};

TEST_F(SpanTest, NoOpCtxStartSpan) {
    {
        auto span = Span::start(nullptr, span_names::kTest1);
        TRACING_SPAN_ATTR(span, "test", 1);
        ASSERT_TRUE(isEmpty());
    }
    ASSERT_TRUE(isEmpty());
}

TEST_F(SpanTest, NoTracerStartSpan) {
    clearProvider();
    {
        auto span = Span::start(nullptr, span_names::kTest2);
        TRACING_SPAN_ATTR(span, "test", 1);
    }
    // Note : this test checks that no crash happens if there's no trace provider. We can't call
    // `isEmpty` as this uses the trace provider to retrieve spans.
}

TEST_F(SpanTest, ExporterSingleSpan) {
    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
        ASSERT_TRUE(isEmpty());
    }

    ASSERT_FALSE(isEmpty());
    auto span = getSpan(0, span_names::kTest1);
    ASSERT_EQ(span->parentId, opentelemetry::trace::SpanId());
}

TEST_F(SpanTest, ParentSpan) {
    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
        auto secondSpan = Span::start(opCtx.get(), span_names::kTest2);
        ASSERT_TRUE(isEmpty());
    }
    ASSERT_FALSE(isEmpty());

    {
        auto span = Span::start(opCtx.get(), span_names::kTest3);
    }

    auto firstRecord = getSpan(1, span_names::kTest1);
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = getSpan(0, span_names::kTest2);
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = getSpan(2, span_names::kTest3);
    ASSERT_EQ(thirdRecord->parentId, opentelemetry::trace::SpanId());
}

TEST_F(SpanTest, SpanDepthThree) {
    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
        auto secondSpan = Span::start(opCtx.get(), span_names::kTest2);
        auto thirdSpan = Span::start(opCtx.get(), span_names::kTest3);

        ASSERT_TRUE(isEmpty());
    }

    auto firstRecord = getSpan(2, span_names::kTest1);
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = getSpan(1, span_names::kTest2);
    ASSERT_NE(secondRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = getSpan(0, span_names::kTest3);
    ASSERT_EQ(thirdRecord->parentId, secondRecord->context.span_id());
}

TEST_F(SpanTest, ParallelSpan) {
    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);

        {
            auto secondSpan = Span::start(opCtx.get(), span_names::kTest2);
        }
        {
            auto thirdSpan = Span::start(opCtx.get(), span_names::kTest3);
        }
    }

    auto firstRecord = getSpan(2, span_names::kTest1);
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = getSpan(1, span_names::kTest3);
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = getSpan(0, span_names::kTest2);
    ASSERT_EQ(thirdRecord->parentId, firstRecord->context.span_id());
}

TEST_F(SpanTest, AsyncSpan) {
    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
        auto future = Future<void>::makeReady().then(
            [telemetryCtx = TelemetryContextHolder::getDecoration(opCtx.get())
                                .getTelemetryContext()]() mutable {
                auto span = Span::start(telemetryCtx, span_names::kTest2);
            });
        future.get();
        auto thirdSpan = Span::start(opCtx.get(), span_names::kTest3);
    }

    auto firstRecord = getSpan(2, span_names::kTest1);
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = getSpan(1, span_names::kTest3);
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = getSpan(0, span_names::kTest2);
    ASSERT_EQ(thirdRecord->parentId, firstRecord->context.span_id());
}

TEST_F(SpanTest, SetIntAttribute) {
    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
        TRACING_SPAN_ATTR(span, "value1", 15);
        TRACING_SPAN_ATTR(span, "value2", 32);
    }

    auto firstRecord = getSpan(0, span_names::kTest1);
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(firstRecord->attributes.size(), 2);
    ASSERT_EQ(firstRecord->status, opentelemetry::trace::StatusCode::kOk);

    auto value1 = firstRecord->attributes.find("value1");
    ASSERT_NE(value1, firstRecord->attributes.end());
    ASSERT_TRUE(absl::holds_alternative<int32_t>(value1->second));
    ASSERT_EQ(static_cast<int>(absl::get<int32_t>(value1->second)), 15);

    auto value2 = firstRecord->attributes.find("value2");
    ASSERT_NE(value2, firstRecord->attributes.end());
    ASSERT_TRUE(absl::holds_alternative<int32_t>(value2->second));
    ASSERT_EQ(static_cast<int>(absl::get<int32_t>(value2->second)), 32);
}

TEST_F(SpanTest, ErrorCode) {
    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
        span.setStatus(Status{ErrorCodes::InternalError, "failed"});
    }

    auto firstRecord = getSpan(0, span_names::kTest1);
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(firstRecord->attributes.size(), 2);
    ASSERT_EQ(firstRecord->status, opentelemetry::trace::StatusCode::kError);

    auto value1 = firstRecord->attributes.find("errorCode");
    ASSERT_NE(value1, firstRecord->attributes.end());
    ASSERT_TRUE(absl::holds_alternative<int32_t>(value1->second));
    ASSERT_EQ(static_cast<int>(absl::get<int32_t>(value1->second)), ErrorCodes::InternalError);

    auto value2 = firstRecord->attributes.find("errorCodeString");
    ASSERT_NE(value2, firstRecord->attributes.end());
    ASSERT_TRUE(absl::holds_alternative<std::basic_string_view<char>>(value2->second));
}

TEST_F(SpanTest, SpanDuringException) {
    auto opCtx = makeOperationContext();
    try {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
        throw std::runtime_error{"testing"};
    } catch (const std::exception&) {
    }

    auto firstRecord = getSpan(0, span_names::kTest1);
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(firstRecord->attributes.size(), 0);
    ASSERT_EQ(firstRecord->status, opentelemetry::trace::StatusCode::kError);
}

TEST_F(SpanTest, CreateTelemetryContext) {
    auto telemetryCtx = Span::createTelemetryContext();
    ASSERT_EQ(telemetryCtx->type(), "SpanTelemetryContextImpl");
}

TEST_F(SpanTest, StartWithTelemetryContextDoesNotCrash) {
    // Using the base TelemetryContext class instead of SpanTelemetryContextImpl should not crash.
    auto telemetryCtx = std::make_shared<TelemetryContext>();
    {
        auto span = Span::start(telemetryCtx, span_names::kTest1);
        TRACING_SPAN_ATTR(span, "test", 1);
    }
    ASSERT_TRUE(isEmpty());
}

TEST_F(SpanTest, StartIfExistingTraceParentNoTraceParent) {
    auto opCtx = makeOperationContext();
    {
        auto span = Span::startIfExistingTraceParent(opCtx.get(), span_names::kTest1);
        TRACING_SPAN_ATTR(span, "test", 1);
        ASSERT_TRUE(isEmpty());
    }
    ASSERT_TRUE(isEmpty());
}

TEST_F(SpanTest, StartIfExistingTraceParentIfTraceParent) {
    auto opCtx = makeOperationContext();
    auto& telemetryCtxHolder = TelemetryContextHolder::getDecoration(opCtx.get());
    telemetryCtxHolder.setTelemetryContext(Span::createTelemetryContext());
    {
        auto span = Span::startIfExistingTraceParent(opCtx.get(), span_names::kTest1);
        TRACING_SPAN_ATTR(span, "test", 1);
        ASSERT_TRUE(isEmpty());
    }
    ASSERT_FALSE(isEmpty());
}

TEST_F(SpanTest, SamplingFlagDisabledDropsRootSpan) {
    auto guard = setTraceSamplingFnForTest([](StringData) { return true; });
    unittest::ServerParameterGuard flagController("featureFlagOtelTraceSampling", false);

    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
    }
    EXPECT_TRUE(isEmpty());
}

TEST_F(SpanTest, TracingFlagDisabledDropsRootSpan) {
    auto guard = setTraceSamplingFnForTest([](StringData) { return true; });
    unittest::ServerParameterGuard flagController("featureFlagTracing", false);

    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
    }
    EXPECT_TRUE(isEmpty());
}

TEST_F(SpanTest, SamplingFlagEnabledSamplerReturnsTrueExportsSpan) {
    auto guard = setTraceSamplingFnForTest([](StringData) { return true; });

    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
    }
    EXPECT_FALSE(isEmpty());
}

TEST_F(SpanTest, SamplingFlagEnabledSamplerReturnsFalseDropsSpan) {
    auto guard = setTraceSamplingFnForTest([](StringData) { return false; });

    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
    }
    EXPECT_TRUE(isEmpty());
}

TEST_F(SpanTest, SamplingDroppedRootMeansChildHasNoParentAndIsAlsoDropped) {
    auto guard = setTraceSamplingFnForTest([](StringData) { return false; });

    auto opCtx = makeOperationContext();
    {
        // Root is dropped: Span{} is returned and no OTel context is set.
        // The subsequent "child" therefore has no real OTel parent and is also dropped.
        auto rootSpan = Span::start(opCtx.get(), span_names::kTest1);
        auto childSpan = Span::start(opCtx.get(), span_names::kTest2);
    }
    EXPECT_TRUE(isEmpty());
}

TEST_F(SpanTest, SamplingFlagEnabledChildOfRealParentAlwaysExported) {
    auto opCtx = makeOperationContext();
    {
        // Sampler approves only name1; name2 is rejected. The child span must still
        // be created because it has a real OTel parent context and bypasses the sampler.
        auto guard = setTraceSamplingFnForTest(
            [&](StringData name) { return name == span_names::kTest1.getName(); });
        auto rootSpan = Span::start(opCtx.get(), span_names::kTest1);
        auto childSpan = Span::start(opCtx.get(), span_names::kTest2);
    }
    // Both root and child should be exported.
    ASSERT_FALSE(isEmpty());
    auto rootRecord = getSpan(1, span_names::kTest1);
    EXPECT_EQ(rootRecord->parentId, opentelemetry::trace::SpanId());
    auto childRecord = getSpan(0, span_names::kTest2);
    EXPECT_NE(childRecord->parentId, opentelemetry::trace::SpanId());
}

}  // namespace
}  // namespace traces
}  // namespace otel
}  // namespace mongo

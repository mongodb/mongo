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

// Tag types selecting which Span::start overload to exercise.
struct SpanNameApi {};
struct StringApi {};

template <typename T>
struct SpanNameHelper;

template <>
struct SpanNameHelper<SpanNameApi> {
    static SpanName name1() {
        return SpanNames::kTest1;
    }
    static SpanName name2() {
        return SpanNames::kTest2;
    }
    static SpanName name3() {
        return SpanNames::kTest3;
    }
    static std::string toString(SpanName n) {
        return std::string(n.getName());
    }
};

template <>
struct SpanNameHelper<StringApi> {
    static std::string name1() {
        return std::string(SpanNames::kTest1.getName());
    }
    static std::string name2() {
        return std::string(SpanNames::kTest2.getName());
    }
    static std::string name3() {
        return std::string(SpanNames::kTest3.getName());
    }
    static std::string toString(const std::string& n) {
        return n;
    }
};

template <typename T>
class SpanTest : public OtelTestFixture {
protected:
    // Enable OTel sampling for all span tests; individual tests may override.
    unittest::ServerParameterGuard _samplingFlagController{"featureFlagOtelTraceSampling", true};

    auto name1() {
        return SpanNameHelper<T>::name1();
    }
    auto name2() {
        return SpanNameHelper<T>::name2();
    }
    auto name3() {
        return SpanNameHelper<T>::name3();
    }
    std::string toString(auto n) {
        return SpanNameHelper<T>::toString(n);
    }
};

using SpanTestTypes = ::testing::Types<SpanNameApi, StringApi>;
TYPED_TEST_SUITE(SpanTest, SpanTestTypes);

TYPED_TEST(SpanTest, NoOpCtxStartSpan) {
    {
        auto span = Span::start(nullptr, this->name1());
        TRACING_SPAN_ATTR(span, "test", 1);
        ASSERT_TRUE(this->isEmpty());
    }
    ASSERT_TRUE(this->isEmpty());
}

TYPED_TEST(SpanTest, NoTracerStartSpan) {
    this->clearProvider();
    {
        auto span = Span::start(nullptr, this->name2());
        TRACING_SPAN_ATTR(span, "test", 1);
    }
    // Note : this test checks that no crash happens if there's no trace provider. We can't call
    // `isEmpty` as this uses the trace provider to retrieve spans.
}

TYPED_TEST(SpanTest, ExporterSingleSpan) {
    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());
        ASSERT_TRUE(this->isEmpty());
    }

    ASSERT_FALSE(this->isEmpty());
    auto span = this->getSpan(0, this->toString(this->name1()));
    ASSERT_EQ(span->parentId, opentelemetry::trace::SpanId());
}

TYPED_TEST(SpanTest, ParentSpan) {
    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());
        auto secondSpan = Span::start(opCtx.get(), this->name2());
        ASSERT_TRUE(this->isEmpty());
    }
    ASSERT_FALSE(this->isEmpty());

    {
        auto span = Span::start(opCtx.get(), this->name3());
    }

    auto firstRecord = this->getSpan(1, this->toString(this->name1()));
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = this->getSpan(0, this->toString(this->name2()));
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = this->getSpan(2, this->toString(this->name3()));
    ASSERT_EQ(thirdRecord->parentId, opentelemetry::trace::SpanId());
}

TYPED_TEST(SpanTest, SpanDepthThree) {
    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());
        auto secondSpan = Span::start(opCtx.get(), this->name2());
        auto thirdSpan = Span::start(opCtx.get(), this->name3());

        ASSERT_TRUE(this->isEmpty());
    }

    auto firstRecord = this->getSpan(2, this->toString(this->name1()));
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = this->getSpan(1, this->toString(this->name2()));
    ASSERT_NE(secondRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = this->getSpan(0, this->toString(this->name3()));
    ASSERT_EQ(thirdRecord->parentId, secondRecord->context.span_id());
}

TYPED_TEST(SpanTest, ParallelSpan) {
    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());

        {
            auto secondSpan = Span::start(opCtx.get(), this->name2());
        }
        {
            auto thirdSpan = Span::start(opCtx.get(), this->name3());
        }
    }

    auto firstRecord = this->getSpan(2, this->toString(this->name1()));
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = this->getSpan(1, this->toString(this->name3()));
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = this->getSpan(0, this->toString(this->name2()));
    ASSERT_EQ(thirdRecord->parentId, firstRecord->context.span_id());
}

TYPED_TEST(SpanTest, AsyncSpan) {
    auto opCtx = this->makeOperationContext();
    auto n2 = this->name2();
    {
        auto span = Span::start(opCtx.get(), this->name1());
        auto future = Future<void>::makeReady().then(
            [telemetryCtx =
                 TelemetryContextHolder::getDecoration(opCtx.get()).getTelemetryContext(),
             n2]() mutable { auto span = Span::start(telemetryCtx, n2); });
        future.get();
        auto thirdSpan = Span::start(opCtx.get(), this->name3());
    }

    auto firstRecord = this->getSpan(2, this->toString(this->name1()));
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());

    auto secondRecord = this->getSpan(1, this->toString(this->name3()));
    ASSERT_EQ(secondRecord->parentId, firstRecord->context.span_id());

    auto thirdRecord = this->getSpan(0, this->toString(n2));
    ASSERT_EQ(thirdRecord->parentId, firstRecord->context.span_id());
}

TYPED_TEST(SpanTest, TestShouldDrop) {
    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());

        auto secondSpan = Span::start(opCtx.get(), this->name2(), true);
        auto thirdSpan = Span::start(opCtx.get(), this->name3());

        ASSERT_TRUE(this->isEmpty());
    }

    auto firstRecord = this->getSpan(2, this->toString(this->name1()));
    ASSERT_EQ(firstRecord->attributes.size(), this->getBaseAttributesSize());
    {
        auto dropSpan = firstRecord->attributes.find("DROP_SPAN");
        ASSERT_NE(dropSpan, firstRecord->attributes.end());
        ASSERT_TRUE(absl::holds_alternative<bool>(dropSpan->second));
        ASSERT_TRUE(static_cast<int>(absl::get<bool>(dropSpan->second)));
    }

    auto secondRecord = this->getSpan(1, this->toString(this->name2()));
    ASSERT_EQ(secondRecord->attributes.size(), this->getBaseAttributesSize());
    {
        auto dropSpan = secondRecord->attributes.find("DROP_SPAN");
        ASSERT_NE(dropSpan, secondRecord->attributes.end());
        ASSERT_TRUE(absl::holds_alternative<bool>(dropSpan->second));
        ASSERT_FALSE(static_cast<int>(absl::get<bool>(dropSpan->second)));
    }

    auto thirdRecord = this->getSpan(0, this->toString(this->name3()));
    ASSERT_EQ(thirdRecord->attributes.size(), this->getBaseAttributesSize());
    {
        auto dropSpan = thirdRecord->attributes.find("DROP_SPAN");
        ASSERT_NE(dropSpan, thirdRecord->attributes.end());
        ASSERT_TRUE(absl::holds_alternative<bool>(dropSpan->second));
        ASSERT_FALSE(static_cast<int>(absl::get<bool>(dropSpan->second)));
    }
}

TYPED_TEST(SpanTest, SetIntAttribute) {
    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());
        TRACING_SPAN_ATTR(span, "value1", 15);
        TRACING_SPAN_ATTR(span, "value2", 32);
    }

    auto firstRecord = this->getSpan(0, this->toString(this->name1()));
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(firstRecord->attributes.size(), this->getBaseAttributesSize() + 2);
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

TYPED_TEST(SpanTest, ErrorCode) {
    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());
        span.setStatus(Status{ErrorCodes::InternalError, "failed"});
    }

    auto firstRecord = this->getSpan(0, this->toString(this->name1()));
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(firstRecord->attributes.size(), this->getBaseAttributesSize() + 2);
    ASSERT_EQ(firstRecord->status, opentelemetry::trace::StatusCode::kError);

    auto value1 = firstRecord->attributes.find("errorCode");
    ASSERT_NE(value1, firstRecord->attributes.end());
    ASSERT_TRUE(absl::holds_alternative<int32_t>(value1->second));
    ASSERT_EQ(static_cast<int>(absl::get<int32_t>(value1->second)), ErrorCodes::InternalError);

    auto value2 = firstRecord->attributes.find("errorCodeString");
    ASSERT_NE(value2, firstRecord->attributes.end());
    ASSERT_TRUE(absl::holds_alternative<std::basic_string_view<char>>(value2->second));
}

TYPED_TEST(SpanTest, SpanDuringException) {
    auto opCtx = this->makeOperationContext();
    try {
        auto span = Span::start(opCtx.get(), this->name1());
        throw std::runtime_error{"testing"};
    } catch (const std::exception&) {
    }

    auto firstRecord = this->getSpan(0, this->toString(this->name1()));
    ASSERT_EQ(firstRecord->parentId, opentelemetry::trace::SpanId());
    ASSERT_EQ(firstRecord->attributes.size(), this->getBaseAttributesSize());
    ASSERT_EQ(firstRecord->status, opentelemetry::trace::StatusCode::kError);
}

TYPED_TEST(SpanTest, CreateTelemetryContext) {
    auto telemetryCtx = Span::createTelemetryContext();
    ASSERT_EQ(telemetryCtx->type(), "SpanTelemetryContextImpl");
}

TYPED_TEST(SpanTest, StartWithTelemetryContextDoesNotCrash) {
    // Using the base TelemetryContext class instead of SpanTelemetryContextImpl should not crash.
    auto telemetryCtx = std::make_shared<TelemetryContext>();
    {
        auto span = Span::start(telemetryCtx, this->name1());
        TRACING_SPAN_ATTR(span, "test", 1);
    }
    ASSERT_TRUE(this->isEmpty());
}

TYPED_TEST(SpanTest, StartIfExistingTraceParentNoTraceParent) {
    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::startIfExistingTraceParent(opCtx.get(), this->name1());
        TRACING_SPAN_ATTR(span, "test", 1);
        ASSERT_TRUE(this->isEmpty());
    }
    ASSERT_TRUE(this->isEmpty());
}

TYPED_TEST(SpanTest, StartIfExistingTraceParentIfTraceParent) {
    auto opCtx = this->makeOperationContext();
    auto& telemetryCtxHolder = TelemetryContextHolder::getDecoration(opCtx.get());
    telemetryCtxHolder.setTelemetryContext(Span::createTelemetryContext());
    {
        auto span = Span::startIfExistingTraceParent(opCtx.get(), this->name1());
        TRACING_SPAN_ATTR(span, "test", 1);
        ASSERT_TRUE(this->isEmpty());
    }
    ASSERT_FALSE(this->isEmpty());
}

TYPED_TEST(SpanTest, SamplingFlagDisabledDropsRootSpan) {
    auto guard = setTraceSamplingFnForTest([](StringData) { return true; });
    unittest::ServerParameterGuard flagController("featureFlagOtelTraceSampling", false);

    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());
    }
    EXPECT_TRUE(this->isEmpty());
}

TYPED_TEST(SpanTest, TracingFlagDisabledDropsRootSpan) {
    auto guard = setTraceSamplingFnForTest([](StringData) { return true; });
    unittest::ServerParameterGuard flagController("featureFlagTracing", false);

    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());
    }
    EXPECT_TRUE(this->isEmpty());
}

TYPED_TEST(SpanTest, SamplingFlagEnabledSamplerReturnsTrueExportsSpan) {
    auto guard = setTraceSamplingFnForTest([](StringData) { return true; });

    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());
    }
    EXPECT_FALSE(this->isEmpty());
}

TYPED_TEST(SpanTest, SamplingFlagEnabledSamplerReturnsFalseDropsSpan) {
    auto guard = setTraceSamplingFnForTest([](StringData) { return false; });

    auto opCtx = this->makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), this->name1());
    }
    EXPECT_TRUE(this->isEmpty());
}

TYPED_TEST(SpanTest, SamplingDroppedRootMeansChildHasNoParentAndIsAlsoDropped) {
    auto guard = setTraceSamplingFnForTest([](StringData) { return false; });

    auto opCtx = this->makeOperationContext();
    {
        // Root is dropped: Span{} is returned and no OTel context is set.
        // The subsequent "child" therefore has no real OTel parent and is also dropped.
        auto rootSpan = Span::start(opCtx.get(), this->name1());
        auto childSpan = Span::start(opCtx.get(), this->name2());
    }
    EXPECT_TRUE(this->isEmpty());
}

TYPED_TEST(SpanTest, SamplingFlagEnabledChildOfRealParentAlwaysExported) {
    auto opCtx = this->makeOperationContext();
    {
        // Sampler approves only name1; name2 is rejected. The child span must still
        // be created because it has a real OTel parent context and bypasses the sampler.
        auto guard = setTraceSamplingFnForTest(
            [&](StringData name) { return name == this->toString(this->name1()); });
        auto rootSpan = Span::start(opCtx.get(), this->name1());
        auto childSpan = Span::start(opCtx.get(), this->name2());
    }
    // Both root and child should be exported.
    ASSERT_FALSE(this->isEmpty());
    auto rootRecord = this->getSpan(1, this->toString(this->name1()));
    EXPECT_EQ(rootRecord->parentId, opentelemetry::trace::SpanId());
    auto childRecord = this->getSpan(0, this->toString(this->name2()));
    EXPECT_NE(childRecord->parentId, opentelemetry::trace::SpanId());
}

}  // namespace
}  // namespace traces
}  // namespace otel
}  // namespace mongo

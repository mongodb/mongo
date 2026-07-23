// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/span/span.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/otel_test_fixture.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/telemetry_context_serialization.h"
#include "mongo/unittest/server_parameter_guard.h"

#include <string_view>

namespace mongo {
namespace otel {
namespace traces {
namespace {

class SpanTest : public OtelTestFixture {
protected:
    // Enable OTel sampling for all span tests; individual tests may override.
    unittest::ServerParameterGuard _samplingFlagController{"featureFlagOtelTraceSampling", true};
    // Approve all spans by default so tests not focused on sampling still export their spans.
    ScopedSamplerOverride _samplerGuard =
        setTraceSamplingFnForTest([](std::string_view, double) { return true; });
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

TEST_F(SpanTest, StartCreatesInternalSpanKind) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto span = Span::start(telemetryCtx, span_names::kTest1);
    }
    ASSERT_EQ(getSpan(0, span_names::kTest1)->kind, opentelemetry::trace::SpanKind::kInternal);
}

TEST_F(SpanTest, StartIngressSpanCreatesServerSpanKind) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto span = Span::startIngressSpan(telemetryCtx, span_names::kTest1);
    }
    ASSERT_EQ(getSpan(0, span_names::kTest1)->kind, opentelemetry::trace::SpanKind::kServer);
}

TEST_F(SpanTest, StartWithClientKindCreatesClientSpanKind) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto span =
            Span::start(telemetryCtx, span_names::kTest1, SpanOptions{.kind = SpanKind::kClient});
    }
    ASSERT_EQ(getSpan(0, span_names::kTest1)->kind, opentelemetry::trace::SpanKind::kClient);
}

TEST_F(SpanTest, StartWithTelemetryContextAndClientKindCreatesClientSpanKind) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto span =
            Span::start(telemetryCtx, span_names::kTest1, SpanOptions{.kind = SpanKind::kClient});
    }
    ASSERT_EQ(getSpan(0, span_names::kTest1)->kind, opentelemetry::trace::SpanKind::kClient);
}

TEST_F(SpanTest, StartWithProducerKindCreatesProducerSpanKind) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto span =
            Span::start(telemetryCtx, span_names::kTest1, SpanOptions{.kind = SpanKind::kProducer});
    }
    ASSERT_EQ(getSpan(0, span_names::kTest1)->kind, opentelemetry::trace::SpanKind::kProducer);
}

TEST_F(SpanTest, StartIngressSpanWithConsumerKindCreatesConsumerSpanKind) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto span = Span::startIngressSpan(
            telemetryCtx, span_names::kTest1, SpanOptions{.kind = SpanKind::kConsumer});
    }
    ASSERT_EQ(getSpan(0, span_names::kTest1)->kind, opentelemetry::trace::SpanKind::kConsumer);
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

TEST_F(SpanTest, SamplingFlagDisabledDropsRootSpan) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return true; });
    unittest::ServerParameterGuard flagController("featureFlagOtelTraceSampling", false);

    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
    }
    EXPECT_TRUE(isEmpty());
}

TEST_F(SpanTest, TracingFlagDisabledDropsRootSpan) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return true; });
    unittest::ServerParameterGuard flagController("featureFlagTracing", false);

    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
    }
    EXPECT_TRUE(isEmpty());
}

TEST_F(SpanTest, SamplingFlagEnabledSamplerReturnsTrueExportsSpan) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return true; });

    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
    }
    EXPECT_FALSE(isEmpty());
}

TEST_F(SpanTest, SamplingFlagEnabledSamplerReturnsFalseDropsSpan) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return false; });

    auto opCtx = makeOperationContext();
    {
        auto span = Span::start(opCtx.get(), span_names::kTest1);
    }
    EXPECT_TRUE(isEmpty());
}

TEST_F(SpanTest, SamplingDroppedRootMeansChildHasNoParentAndIsAlsoDropped) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return false; });

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
            [&](std::string_view name, double) { return name == span_names::kTest1.getName(); });
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

TEST_F(SpanTest, ClonedContextCreatesChildOfCurrentSpan) {
    auto opCtx = makeOperationContext();
    {
        auto rootSpan = Span::start(opCtx.get(), span_names::kTest1);
        {
            // Starting a span on the clone parents it under root. Export order mirrors destruction
            // order (reverse of construction), so clonedSpan is exported before rootSpan.
            auto clonedCtx =
                TelemetryContextHolder::getDecoration(opCtx.get()).cloneTelemetryContext();
            auto clonedSpan = Span::start(clonedCtx, span_names::kTest2);
        }
    }

    ASSERT_FALSE(isEmpty());
    auto rootSpanRecord = getSpan(1, span_names::kTest1);
    EXPECT_EQ(rootSpanRecord->parentId, opentelemetry::trace::SpanId());
    auto clonedSpanRecord = getSpan(0, span_names::kTest2);
    EXPECT_EQ(clonedSpanRecord->parentId, rootSpanRecord->context.span_id());
}

TEST_F(SpanTest, MultipleClonedContextsSpans) {
    auto opCtx = makeOperationContext();
    {
        auto rootSpan = Span::start(opCtx.get(), span_names::kTest1);
        auto clonedCtx1 =
            TelemetryContextHolder::getDecoration(opCtx.get()).cloneTelemetryContext();
        auto clonedSpan1 = Span::start(clonedCtx1, span_names::kTest2);
        auto clonedSpan1Child = Span::start(clonedCtx1, span_names::kTest3);
        auto clonedCtx2 =
            TelemetryContextHolder::getDecoration(opCtx.get()).cloneTelemetryContext();
        auto clonedSpan2 = Span::start(clonedCtx2, span_names::kTest4);
    }

    ASSERT_FALSE(isEmpty());
    auto rootSpanRecord = getSpan(3, span_names::kTest1);
    auto clonedSpanRecord1 = getSpan(2, span_names::kTest2);
    auto clonedSpanRecord1Child = getSpan(1, span_names::kTest3);
    auto clonedSpanRecord2 = getSpan(0, span_names::kTest4);

    EXPECT_EQ(clonedSpanRecord1->parentId, rootSpanRecord->context.span_id());
    EXPECT_EQ(clonedSpanRecord1Child->parentId, clonedSpanRecord1->context.span_id());
    EXPECT_EQ(clonedSpanRecord2->parentId, rootSpanRecord->context.span_id());
}

TEST_F(SpanTest, ClonedAndNonClonedSpans) {
    auto opCtx = makeOperationContext();
    {
        auto rootSpan = Span::start(opCtx.get(), span_names::kTest1);
        auto clonedCtx = TelemetryContextHolder::getDecoration(opCtx.get()).cloneTelemetryContext();
        auto childSpan = Span::start(opCtx.get(), span_names::kTest2);
        auto clonedSpan = Span::start(clonedCtx, span_names::kTest3);
    }

    ASSERT_FALSE(isEmpty());
    auto rootSpanRecord = getSpan(2, span_names::kTest1);
    auto childSpanRecord = getSpan(1, span_names::kTest2);
    auto clonedSpanRecord = getSpan(0, span_names::kTest3);

    EXPECT_EQ(childSpanRecord->parentId, rootSpanRecord->context.span_id());
    EXPECT_EQ(clonedSpanRecord->parentId, rootSpanRecord->context.span_id());
}

TEST_F(SpanTest, ClonedContextSpanOutlivesOriginalContextSpan) {
    auto opCtx = makeOperationContext();
    {
        std::optional<Span> rootSpan = Span::start(opCtx.get(), span_names::kTest1);
        auto clonedCtx = TelemetryContextHolder::getDecoration(opCtx.get()).cloneTelemetryContext();
        auto clonedSpan = Span::start(clonedCtx, span_names::kTest2);
        rootSpan.reset();
    }

    ASSERT_FALSE(isEmpty());
    auto rootSpanRecord = getSpan(0, span_names::kTest1);
    auto clonedSpanRecord = getSpan(1, span_names::kTest2);

    EXPECT_EQ(clonedSpanRecord->parentId, rootSpanRecord->context.span_id());
}

TEST_F(SpanTest, ClonedContextSpanOutlivesOriginalContext) {
    {
        std::optional<std::shared_ptr<TelemetryContext>> rootCtx = Span::createTelemetryContext();
        std::optional<Span> rootSpan = Span::start(*rootCtx, span_names::kTest1);
        auto clonedCtx = (*rootCtx)->clone();
        auto clonedSpan = Span::start(clonedCtx, span_names::kTest2);
        rootSpan.reset();
        rootCtx.reset();
    }

    ASSERT_FALSE(isEmpty());
    auto rootSpanRecord = getSpan(0, span_names::kTest1);
    auto clonedSpanRecord = getSpan(1, span_names::kTest2);

    EXPECT_EQ(clonedSpanRecord->parentId, rootSpanRecord->context.span_id());
}

using IngressSpanTest = SpanTest;

TEST_F(IngressSpanTest, ExternalTraceAcceptedBypassesSampling) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return false; },
                                           [] { return true; });
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto _ = Span::startIngressSpan(telemetryCtx, span_names::kTest1);
    }
    EXPECT_FALSE(isEmpty());
}

TEST_F(IngressSpanTest, ExternalTraceNotAcceptedIsSampled) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return false; },
                                           [] { return false; });
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto _ = Span::startIngressSpan(telemetryCtx, span_names::kTest1);
    }
    EXPECT_TRUE(isEmpty());
}

TEST_F(IngressSpanTest, NoExternalContextIgnoresAcceptance) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return false; },
                                           [] { return true; });
    std::shared_ptr<TelemetryContext> telemetryCtx;
    {
        auto _ = Span::startIngressSpan(telemetryCtx, span_names::kTest1);
    }
    EXPECT_TRUE(isEmpty());
}

TEST_F(IngressSpanTest, NoExternalContextSampledExports) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return true; },
                                           [] { return false; });
    std::shared_ptr<TelemetryContext> telemetryCtx;
    {
        auto _ = Span::startIngressSpan(telemetryCtx, span_names::kTest1);
    }
    EXPECT_FALSE(isEmpty());
    EXPECT_NE(telemetryCtx, nullptr);
}

TEST_F(IngressSpanTest, RemoteParentContinuesTrace) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return false; },
                                           [] { return true; });
    BSONObj traceCtxBson =
        BSON("traceparent" << "00-11111111111111111111111111111111-2222222222222222-01");
    auto telemetryCtx = TelemetryContextSerializer::fromBSON(traceCtxBson);
    {
        auto _ = Span::startIngressSpan(telemetryCtx, span_names::kTest1);
    }
    ASSERT_FALSE(isEmpty());
    auto record = getSpan(0, span_names::kTest1);
    EXPECT_NE(record->parentId, opentelemetry::trace::SpanId());
}

TEST_F(IngressSpanTest, RemoteParentNotAcceptedIsSampledAndDropped) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return false; },
                                           [] { return false; });
    BSONObj traceCtxBson =
        BSON("traceparent" << "00-11111111111111111111111111111111-2222222222222222-01");
    auto telemetryCtx = TelemetryContextSerializer::fromBSON(traceCtxBson);
    {
        auto _ = Span::startIngressSpan(telemetryCtx, span_names::kTest1);
    }
    EXPECT_TRUE(isEmpty());
}

TEST_F(IngressSpanTest, RemoteParentNotAcceptedButSampledContinues) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return true; },
                                           [] { return false; });
    BSONObj traceCtxBson =
        BSON("traceparent" << "00-11111111111111111111111111111111-2222222222222222-01");
    auto telemetryCtx = TelemetryContextSerializer::fromBSON(traceCtxBson);
    {
        auto _ = Span::startIngressSpan(telemetryCtx, span_names::kTest1);
    }
    ASSERT_FALSE(isEmpty());
    auto record = getSpan(0, span_names::kTest1);
    EXPECT_NE(record->parentId, opentelemetry::trace::SpanId());
}

TEST_F(IngressSpanTest, RemoteParentGrandchildrenInheritHeadDecision) {
    auto guard = setTraceSamplingFnForTest([](std::string_view, double) { return false; },
                                           [] { return true; });
    auto opCtx = makeOperationContext();
    BSONObj traceCtxBson =
        BSON("traceparent" << "00-11111111111111111111111111111111-2222222222222222-01");
    auto telemetryCtx = TelemetryContextSerializer::fromBSON(traceCtxBson);
    {
        auto ingress = Span::startIngressSpan(telemetryCtx, span_names::kTest1);
        TelemetryContextHolder::getDecoration(opCtx.get()).setTelemetryContext(telemetryCtx);
        auto child = Span::start(opCtx.get(), span_names::kTest2);
        auto grandchild = Span::start(opCtx.get(), span_names::kTest3);
    }

    // All three spans must be exported (innermost-first: kTest3 at 0, kTest2 at 1, kTest1 at 2).
    ASSERT_FALSE(isEmpty());
    ASSERT_EQ(_mockExporter->getSpans().size(), 3u);
    auto ingressRecord = getSpan(2, span_names::kTest1);
    auto childRecord = getSpan(1, span_names::kTest2);
    auto grandchildRecord = getSpan(0, span_names::kTest3);

    // The ingress span continues the remote trace (it has a remote parent).
    EXPECT_NE(ingressRecord->parentId, opentelemetry::trace::SpanId());

    // Each descendant nests directly under its immediate parent.
    EXPECT_EQ(childRecord->parentId, ingressRecord->context.span_id());
    EXPECT_EQ(grandchildRecord->parentId, childRecord->context.span_id());

    // Every span belongs to the same trace as the ingress span.
    EXPECT_EQ(childRecord->context.trace_id(), ingressRecord->context.trace_id());
    EXPECT_EQ(grandchildRecord->context.trace_id(), ingressRecord->context.trace_id());
}

TEST_F(IngressSpanTest, FeatureFlagsDisabledAfterHeadStillCreatesChildren) {
    auto opCtx = makeOperationContext();
    {
        auto root = Span::start(opCtx.get(), span_names::kTest1);

        // Disable both tracing feature flags now that the head span already exists.
        unittest::ServerParameterGuard samplingOff{"featureFlagOtelTraceSampling", false};
        unittest::ServerParameterGuard tracingOff{"featureFlagTracing", false};

        auto child = Span::start(opCtx.get(), span_names::kTest2);
        auto grandchild = Span::start(opCtx.get(), span_names::kTest3);
    }

    // All three spans must still be exported (innermost-first: kTest3 at 0, kTest2 at 1,
    // kTest1 at 2).
    ASSERT_FALSE(isEmpty());
    ASSERT_EQ(_mockExporter->getSpans().size(), 3u);
    auto rootRecord = getSpan(2, span_names::kTest1);
    auto childRecord = getSpan(1, span_names::kTest2);
    auto grandchildRecord = getSpan(0, span_names::kTest3);

    // The head is a root; descendants nest directly under their immediate parent.
    EXPECT_EQ(rootRecord->parentId, opentelemetry::trace::SpanId());
    EXPECT_EQ(childRecord->parentId, rootRecord->context.span_id());
    EXPECT_EQ(grandchildRecord->parentId, childRecord->context.span_id());
}

TEST_F(IngressSpanTest, InternalSamplingDisabledAfterHeadStillCreatesChildren) {
    auto opCtx = makeOperationContext();
    {
        auto root = Span::start(opCtx.get(), span_names::kTest1);

        // Reject every span from the internal sampler now that the head span already exists.
        auto samplerOff = setTraceSamplingFnForTest([](std::string_view, double) { return false; });

        auto child = Span::start(opCtx.get(), span_names::kTest2);
        auto grandchild = Span::start(opCtx.get(), span_names::kTest3);
    }

    // All three spans must still be exported (innermost-first: kTest3 at 0, kTest2 at 1,
    // kTest1 at 2).
    ASSERT_FALSE(isEmpty());
    ASSERT_EQ(_mockExporter->getSpans().size(), 3u);
    auto rootRecord = getSpan(2, span_names::kTest1);
    auto childRecord = getSpan(1, span_names::kTest2);
    auto grandchildRecord = getSpan(0, span_names::kTest3);

    // The head is a root; descendants nest directly under their immediate parent.
    EXPECT_EQ(rootRecord->parentId, opentelemetry::trace::SpanId());
    EXPECT_EQ(childRecord->parentId, rootRecord->context.span_id());
    EXPECT_EQ(grandchildRecord->parentId, childRecord->context.span_id());
}

}  // namespace
}  // namespace traces
}  // namespace otel
}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/traces_test_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/telemetry_context_serialization.h"
#include "mongo/unittest/unittest.h"

#ifdef MONGO_CONFIG_OTEL
#include "mongo/otel/traces/tracer_provider_service.h"
#endif

namespace mongo::otel::traces {
namespace {

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Ne;
using ::testing::Not;
using ::testing::Optional;
using ::testing::Pair;
using ::testing::Property;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

/** Creates a capturer and skips the test if Otel is not configured. */
class OtelTracesCapturerTest : public ::testing::Test {
public:
    void SetUp() override {
        if (!OtelTracesCapturer::canReadSpans())
            GTEST_SKIP() << "OTel not configured";
    }

protected:
    OtelTracesCapturer capturer;
};

TEST(CanReadSpansTest, CanReadSpans) {
#ifdef MONGO_CONFIG_OTEL
    ASSERT_TRUE(OtelTracesCapturer::canReadSpans());
#else
    ASSERT_FALSE(OtelTracesCapturer::canReadSpans());
#endif
}

TEST(OtelTracesCapturerLifecycleTest, ConstructorInstallsAndDestructorRestoresGlobal) {
#ifdef MONGO_CONFIG_OTEL
    EXPECT_EQ(getGlobalTracerProviderService(), nullptr);
    {
        OtelTracesCapturer capturer;
        EXPECT_NE(getGlobalTracerProviderService(), nullptr);
    }
    EXPECT_EQ(getGlobalTracerProviderService(), nullptr);
#endif
}

TEST_F(OtelTracesCapturerTest, SpanCreatedDuringScopeIsVisible) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans,
                ElementsAre(Property("name", &CapturedSpan::name, span_names::kTest1.getName())));
}

TEST(OtelTracesCapturerLifecycleTest, SpanCreatedBeforeScopeNotCaptured) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

    EXPECT_THAT(capturer.getSpans(span_names::kTest1), IsEmpty());
}

TEST_F(OtelTracesCapturerTest, GetSpansByStringView) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1.getName());
    EXPECT_THAT(spans, SizeIs(1));
}

TEST_F(OtelTracesCapturerTest, FreshCapturerStartsEmpty) {
    EXPECT_THAT(capturer.getSpans(span_names::kTest1), IsEmpty());
    EXPECT_THAT(capturer.getSpans(span_names::kTest2), IsEmpty());
}

TEST_F(OtelTracesCapturerTest, ClearSpansEmptiesTheList) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    EXPECT_THAT(capturer.getSpans(span_names::kTest1), Not(IsEmpty()));
    capturer.clearSpans();
    EXPECT_THAT(capturer.getSpans(span_names::kTest1), IsEmpty());
}

TEST_F(OtelTracesCapturerTest, ClearSpansAllowsNewSpansToBeVisible) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }
    capturer.clearSpans();

    {
        Span span2 = Span::start(telemetryCtx, span_names::kTest2);
    }

    EXPECT_THAT(capturer.getSpans(span_names::kTest1), IsEmpty());
    EXPECT_THAT(capturer.getSpans(span_names::kTest2), SizeIs(1));
}

TEST_F(OtelTracesCapturerTest, SpansNotCompletedAreNotCaptured) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span1 = Span::start(telemetryCtx, span_names::kTest1);
        {
            Span span2 = Span::start(telemetryCtx, span_names::kTest2);
            EXPECT_THAT(capturer.getSpans(span_names::kTest1), IsEmpty());
            EXPECT_THAT(capturer.getSpans(span_names::kTest2), IsEmpty());
        }
        EXPECT_THAT(capturer.getSpans(span_names::kTest1), IsEmpty());
        EXPECT_THAT(capturer.getSpans(span_names::kTest2), SizeIs(1));
    }
    EXPECT_THAT(capturer.getSpans(span_names::kTest1), SizeIs(1));
    EXPECT_THAT(capturer.getSpans(span_names::kTest2), SizeIs(1));
}

TEST_F(OtelTracesCapturerTest, ParentChildLinksWired) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto parent = Span::start(telemetryCtx, span_names::kTest1);
        auto child = Span::start(telemetryCtx, span_names::kTest2);
    }

    auto parents = capturer.getSpans(span_names::kTest1);
    auto children = capturer.getSpans(span_names::kTest2);
    EXPECT_THAT(parents, ElementsAre(Property("parent", &CapturedSpan::parent, Eq(std::nullopt))));
    EXPECT_THAT(
        children,
        ElementsAre(Property(
            "parent",
            &CapturedSpan::parent,
            Optional(Property("name", &CapturedSpan::name, span_names::kTest1.getName())))));
}

TEST_F(OtelTracesCapturerTest, ChildrenLinksWired) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto root = Span::start(telemetryCtx, span_names::kTest1);
        auto child = Span::start(telemetryCtx, span_names::kTest2);
    }

    auto roots = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(
        roots,
        ElementsAre(Property(
            "children",
            &CapturedSpan::children,
            ElementsAre(Property("name", &CapturedSpan::name, span_names::kTest2.getName())))));
}

TEST_F(OtelTracesCapturerTest, AttributesReadable) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
        TRACING_SPAN_ATTR(span, "key1", "value1");
        TRACING_SPAN_ATTR(span, "key2", "value2");
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans,
                ElementsAre(Property(
                    "attributes",
                    &CapturedSpan::attributes,
                    UnorderedElementsAre(Pair("key1", "value1"), Pair("key2", "value2")))));
}

using SpanNameMatcherTest = OtelTracesCapturerTest;
TEST_F(SpanNameMatcherTest, Matches) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans,
                // Can use string literal, string_view, or SpanName.
                ElementsAre(AllOf(HasSpanName(span_names::kTest1.getName()),
                                  HasSpanName("test_only.span1"),
                                  HasSpanName(span_names::kTest1),
                                  Not(HasSpanName("unknown")))));
}

using AttributesMatcherTest = OtelTracesCapturerTest;
TEST_F(AttributesMatcherTest, Matches) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
        TRACING_SPAN_ATTR(span, "db", "test");
        TRACING_SPAN_ATTR(span, "command", "foo");
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans,
                ElementsAre(AllOf(HasAttribute("db", "test"),
                                  HasAttribute("command", "foo"),
                                  Not(HasAttribute("unknown", "value")))));
}

TEST_F(AttributesMatcherTest, DistinguishesEmptyValueAndMissing) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
        TRACING_SPAN_ATTR(span, "db", "");
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans,
                ElementsAre(AllOf(HasAttribute("db", ""), Not(HasAttribute("missing", "")))));
}

TEST_F(AttributesMatcherTest, ValueSatisfiesInnerMatcher) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
        TRACING_SPAN_ATTR(span, "command", "findAndModify");
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans,
                ElementsAre(AllOf(HasAttribute("command", HasSubstr("Modify")),
                                  Not(HasAttribute("command", HasSubstr("aggregate"))),
                                  Not(HasAttribute("missing", HasSubstr("x"))))));
}

using ErrorMatcherTest = OtelTracesCapturerTest;

TEST_F(ErrorMatcherTest, MatchesSpanWithError) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
        span.setStatus(Status{ErrorCodes::InternalError, "failed"});
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans, ElementsAre(HasError()));
}

TEST_F(ErrorMatcherTest, DoesNotMatchSpanWithoutError) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans, ElementsAre(Not(HasError())));
}

TEST_F(ErrorMatcherTest, DoesNotMatchSpanWithOkStatus) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
        span.setStatus(Status::OK());
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans, ElementsAre(Not(HasError())));
}

using ParentMatcherTest = OtelTracesCapturerTest;
TEST_F(ParentMatcherTest, Matches) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span root = Span::start(telemetryCtx, span_names::kTest1);
        Span child = Span::start(telemetryCtx, span_names::kTest2);
    }

    std::vector<CapturedSpan> children = capturer.getSpans(span_names::kTest2);
    EXPECT_THAT(children, ElementsAre(Parent(HasSpanName(span_names::kTest1))));
}

using ChildrenMatcherTest = OtelTracesCapturerTest;
TEST_F(ChildrenMatcherTest, Matches) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span root = Span::start(telemetryCtx, span_names::kTest1);
        {
            Span child = Span::start(telemetryCtx, span_names::kTest2);
        }
        Span child2 = Span::start(telemetryCtx, span_names::kTest3);
    }

    auto roots = capturer.getSpans(span_names::kTest1);
    ASSERT_THAT(roots,
                ElementsAre(Children(UnorderedElementsAre(HasSpanName(span_names::kTest2),
                                                          HasSpanName(span_names::kTest3)))));
}

TEST_F(OtelTracesCapturerTest, SpanFromRemoteContextHasCorrectTraceAndParentSpanIds) {
    // Mirrors what happens when a span is continued from an externally initiated trace.
    BSONObj traceCtxBson =
        BSON("traceparent" << "00-11111111111111111111111111111111-2222222222222222-01");
    auto telemetryCtx = TelemetryContextSerializer::fromBSON(traceCtxBson);
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    auto spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(
        spans,
        ElementsAre(AllOf(
            Property("parent", &CapturedSpan::parent, Eq(std::nullopt)),
            Property("parentSpanIdHex", &CapturedSpan::parentSpanIdHex, "2222222222222222"),
            Property(
                "traceIdHex", &CapturedSpan::traceIdHex, "11111111111111111111111111111111"))));
}

TEST_F(OtelTracesCapturerTest, TraceIdHexNonEmptyForValidSpan) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    auto spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans,
                ElementsAre(Property("traceIdHex",
                                     &CapturedSpan::traceIdHex,
                                     // The trace id hex is present and valid.
                                     AllOf(Not(Eq("")), Not(Eq(std::string(32, '0')))))));
}

TEST_F(OtelTracesCapturerTest, ParentSpanIdHexEmptyForRootSpan) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    auto spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans,
                ElementsAre(Property("parentSpanIdHex", &CapturedSpan::parentSpanIdHex, "")));
}

TEST_F(OtelTracesCapturerTest, ParentSpanIdHexNonEmptyForChildSpan) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto parent = Span::start(telemetryCtx, span_names::kTest1);
        auto child = Span::start(telemetryCtx, span_names::kTest2);
    }

    auto parents = capturer.getSpans(span_names::kTest1);
    auto children = capturer.getSpans(span_names::kTest2);
    EXPECT_THAT(parents,
                ElementsAre(Property("parentSpanIdHex", &CapturedSpan::parentSpanIdHex, Eq(""))));
    EXPECT_THAT(children,
                ElementsAre(Property("parentSpanIdHex", &CapturedSpan::parentSpanIdHex, Ne(""))));
}

}  // namespace
}  // namespace mongo::otel::traces

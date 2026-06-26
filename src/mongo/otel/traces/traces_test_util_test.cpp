/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/traces/traces_test_util.h"

#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/unittest/unittest.h"

#ifdef MONGO_CONFIG_OTEL
#include "mongo/otel/traces/tracer_provider_service.h"
#endif

namespace mongo::otel::traces {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Optional;
using ::testing::Pair;
using ::testing::Property;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

TEST(OtelTracesCapturerTest, CanReadSpans) {
#ifdef MONGO_CONFIG_OTEL
    ASSERT_TRUE(OtelTracesCapturer::canReadSpans());
#else
    ASSERT_FALSE(OtelTracesCapturer::canReadSpans());
#endif
}

TEST(OtelTracesCapturerTest, ConstructorInstallsAndDestructorRestoresGlobal) {
#ifdef MONGO_CONFIG_OTEL
    EXPECT_EQ(getGlobalTracerProviderService(), nullptr);
    {
        OtelTracesCapturer capturer;
        EXPECT_NE(getGlobalTracerProviderService(), nullptr);
    }
    EXPECT_EQ(getGlobalTracerProviderService(), nullptr);
#endif
}

TEST(OtelTracesCapturerTest, SpanCreatedDuringScopeIsVisible) {
    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1);
    EXPECT_THAT(spans,
                ElementsAre(Property("name", &CapturedSpan::name, span_names::kTest1.getName())));
}

TEST(OtelTracesCapturerTest, SpanCreatedBeforeScopeNotCaptured) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

    EXPECT_THAT(capturer.getSpans(span_names::kTest1), IsEmpty());
}

TEST(OtelTracesCapturerTest, GetSpansByStringView) {
    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    std::vector<CapturedSpan> spans = capturer.getSpans(span_names::kTest1.getName());
    EXPECT_THAT(spans, SizeIs(1));
}

TEST(OtelTracesCapturerTest, FreshCapturerStartsEmpty) {
    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

    EXPECT_THAT(capturer.getSpans(span_names::kTest1), IsEmpty());
    EXPECT_THAT(capturer.getSpans(span_names::kTest2), IsEmpty());
}

TEST(OtelTracesCapturerTest, ClearSpansEmptiesTheList) {
    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

    auto telemetryCtx = Span::createTelemetryContext();
    {
        Span span = Span::start(telemetryCtx, span_names::kTest1);
    }

    EXPECT_THAT(capturer.getSpans(span_names::kTest1), Not(IsEmpty()));
    capturer.clearSpans();
    EXPECT_THAT(capturer.getSpans(span_names::kTest1), IsEmpty());
}

TEST(OtelTracesCapturerTest, ClearSpansAllowsNewSpansToBeVisible) {
    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

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

TEST(OtelTracesCapturerTest, SpansNotCompletedAreNotCaptured) {
    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

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

TEST(OtelTracesCapturerTest, ParentChildLinksWired) {
    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

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

TEST(OtelTracesCapturerTest, ChildrenLinksWired) {
    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

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

TEST(OtelTracesCapturerTest, AttributesReadable) {
    OtelTracesCapturer capturer;
    if (!OtelTracesCapturer::canReadSpans())
        GTEST_SKIP() << "OTel not configured";

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

}  // namespace
}  // namespace mongo::otel::traces

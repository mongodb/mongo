// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/tracing_support.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source_mock.h"

#include <cstdint>
#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


/*
 * We use `NOLINT` throughout this file since usages of `TracerProvider` aren't meant to be used in
 * production, but we still wanted to be able to commit unit tests.
 */

namespace mongo {
namespace {

using TickSourceMockMicros = TickSourceMock<Microseconds>;

std::unique_ptr<TickSource> makeTickSource() {
    return std::make_unique<TickSourceMockMicros>();
}

void advanceTime(std::shared_ptr<Tracer>& tracer, Microseconds duration) {
    TickSourceMockMicros* clk = dynamic_cast<TickSourceMockMicros*>(tracer->getTickSource());
    clk->advance(duration);
}

// Uses the mocked clock source to initialize the trace provider.
MONGO_INITIALIZER_GENERAL(InitializeTraceProviderForTest, (), ("InitializeTraceProvider"))
(InitializerContext*) {
    TracerProvider::initialize(makeTickSource());  // NOLINT
}

static constexpr auto kTracerName = "MyTracer";
}  // namespace

DEATH_TEST(TracingSupportTestDeathTest, CannotInitializeTwice, "invariant") {
    TracerProvider::initialize(makeTickSource());  // NOLINT
}

DEATH_TEST(TracingSupportTestDeathTest, SpansMustCloseInOrder, "invariant") {
    auto tracer = TracerProvider::get().getTracer(kTracerName);  // NOLINT
    auto outerSpan = tracer->startSpan("outer span");
    auto innerSpan = tracer->startSpan("inner span");
    outerSpan.reset();
}

TEST(TracingSupportTest, TraceIsInitiallyEmpty) {
    auto tracer = TracerProvider::get().getTracer(kTracerName);  // NOLINT
    ASSERT_FALSE(tracer->getLatestTrace());
}

TEST(TracingSupportTest, TraceIsEmptyWithActiveSpans) {
    auto tracer = TracerProvider::get().getTracer(kTracerName);  // NOLINT
    auto span = tracer->startSpan("some span");
    ASSERT_FALSE(tracer->getLatestTrace());
}

TEST(TracingSupportTest, BasicUsage) {
    const auto kSpanDuration = Seconds(5);
    auto tracer = TracerProvider::get().getTracer(kTracerName);  // NOLINT

    {
        auto rootSpan = tracer->startSpan("root");
        advanceTime(tracer, kSpanDuration);
        {
            auto childSpan = tracer->startSpan("child");
            advanceTime(tracer, kSpanDuration);
            {
                {
                    auto grandChildOne = tracer->startSpan("grand child #1");
                    advanceTime(tracer, kSpanDuration);
                }
                {
                    auto grandChildTwo = tracer->startSpan("grand child #2");
                    advanceTime(tracer, kSpanDuration);
                }
            }
        }
    }

    const auto trace = tracer->getLatestTrace();
    ASSERT_TRUE(trace);

    const auto kSpanDurationMicros = durationCount<Microseconds>(kSpanDuration);

    const auto expected = BSON(
        "tracer" << kTracerName << "root"
                 << BSON("startedMicros"
                         << 0 << "spans"
                         << BSON("child" << BSON(
                                     "startedMicros"
                                     << kSpanDurationMicros << "spans"
                                     << BSON("grand child #1"
                                             << BSON("startedMicros" << 2 * kSpanDurationMicros
                                                                     << "stoppedMicros"
                                                                     << 3 * kSpanDurationMicros)
                                             << "grand child #2"
                                             << BSON("startedMicros" << 3 * kSpanDurationMicros
                                                                     << "stoppedMicros"
                                                                     << 4 * kSpanDurationMicros))
                                     << "stoppedMicros" << 4 * kSpanDurationMicros))
                         << "stoppedMicros" << 4 * kSpanDurationMicros));
    ASSERT_BSONOBJ_EQ(expected, trace.value());
}

BSONObj beginEvent(std::string_view name, int64_t time) {
    return BSON("name" << name << "ph"
                       << "B"
                       << "ts" << time << "pid" << 1 << "tid" << 1);
}

BSONObj endEvent(int64_t time) {
    return BSON("ph" << "E"
                     << "ts" << time << "pid" << 1 << "tid" << 1);
}

TEST(TracingSupportTest, BasicEventUsage) {
    const auto kSpanDuration = Seconds(5);
    auto tracer = TracerProvider::get().getEventTracer(kTracerName);  // NOLINT

    {
        auto rootSpan = tracer->startSpan("root");
        advanceTime(tracer, kSpanDuration);
        {
            auto childSpan = tracer->startSpan("child");
            advanceTime(tracer, kSpanDuration);
            {
                {
                    auto grandChildOne = tracer->startSpan("grand child #1");
                    advanceTime(tracer, kSpanDuration);
                }
                {
                    auto grandChildTwo = tracer->startSpan("grand child #2");
                    advanceTime(tracer, kSpanDuration);
                }
            }
        }
    }

    const auto trace = tracer->getLatestTrace();
    ASSERT_TRUE(trace);

    const auto kSpanDurationMillis = durationCount<Milliseconds>(kSpanDuration);

    const auto expected =
        BSON("traceEvents" << BSON_ARRAY(beginEvent("root", 0)
                                         << beginEvent("child", 1 * kSpanDurationMillis) <<

                                         beginEvent("grand child #1", 2 * kSpanDurationMillis)
                                         << endEvent(3 * kSpanDurationMillis) <<

                                         beginEvent("grand child #2", 3 * kSpanDurationMillis)
                                         << endEvent(4 * kSpanDurationMillis) <<

                                         endEvent(4 * kSpanDurationMillis)
                                         << endEvent(4 * kSpanDurationMillis))
                           << "displayTimeUnit"
                           << "ms");
    ASSERT_BSONOBJ_EQ(expected, trace.value());
}

}  // namespace mongo

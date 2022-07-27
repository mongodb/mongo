/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <memory>

#include "mongo/base/init.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tracing_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


/*
 * We use `NOLINT` throughout this file since usages of `TracerProvider` aren't meant to be used in
 * production, but we still wanted to be able to commit unit tests.
 */

namespace mongo {
namespace {
std::unique_ptr<ClockSource> makeClockSource() {
    return std::make_unique<ClockSourceMock>();
}

void advanceTime(std::shared_ptr<Tracer>& tracer, Milliseconds duration) {
    ClockSourceMock* clk = dynamic_cast<ClockSourceMock*>(tracer->getClockSource());
    clk->advance(duration);
}

// Uses the mocked clock source to initialize the trace provider.
MONGO_INITIALIZER_GENERAL(InitializeTraceProviderForTest, (), ("InitializeTraceProvider"))
(InitializerContext*) {
    TracerProvider::initialize(makeClockSource());  // NOLINT
}

static constexpr auto kTracerName = "MyTracer";
}  // namespace

DEATH_TEST(TracingSupportTest, CannotInitializeTwice, "invariant") {
    TracerProvider::initialize(makeClockSource());  // NOLINT
}

DEATH_TEST(TracingSupportTest, SpansMustCloseInOrder, "invariant") {
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
    const auto startTime = tracer->getClockSource()->now();

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

    const auto expected = BSON(
        "tracer"
        << kTracerName << "root"
        << BSON("started"
                << startTime << "spans"
                << BSON("child" << BSON(
                            "started"
                            << startTime + kSpanDuration << "spans"
                            << BSON("grand child #1"
                                    << BSON("started" << startTime + 2 * kSpanDuration << "stopped"
                                                      << startTime + 3 * kSpanDuration)
                                    << "grand child #2"
                                    << BSON("started" << startTime + 3 * kSpanDuration << "stopped"
                                                      << startTime + 4 * kSpanDuration))
                            << "stopped" << startTime + 4 * kSpanDuration))
                << "stopped" << startTime + 4 * kSpanDuration));
    ASSERT_BSONOBJ_EQ(expected, trace.value());
}

}  // namespace mongo

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

#include "mongo/otel/traces/span/span_telemetry_context_impl.h"

#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <span>

#include <opentelemetry/trace/noop.h>
#include <opentelemetry/trace/span_context.h>

namespace mongo {
namespace otel {
namespace traces {
namespace {

class SpanTelemetryContextImplTest : public unittest::Test {
public:
    OtelContext getSpanContext() {
        return OtelContext();
    }

    ScopedSpan makeValidSpan() {
        constexpr uint8_t kTraceIdBytes[16] = {1};
        constexpr uint8_t kSpanIdBytes[8] = {1};
        auto ctx = std::unique_ptr<opentelemetry::trace::SpanContext>(
            new opentelemetry::trace::SpanContext(
                opentelemetry::trace::TraceId(std::span<const uint8_t, 16>(kTraceIdBytes)),
                opentelemetry::trace::SpanId(std::span<const uint8_t, 8>(kSpanIdBytes)),
                opentelemetry::trace::TraceFlags{},
                false));
        auto tracer = std::make_shared<opentelemetry::trace::NoopTracer>();
        return std::shared_ptr<opentelemetry::trace::Span>(
            new opentelemetry::trace::NoopSpan(tracer, std::move(ctx)));
    }
};

TEST_F(SpanTelemetryContextImplTest, SamplingRollIsInUnitInterval) {
    PseudoRandom prng(int64_t{1});
    SpanTelemetryContextImpl impl(getSpanContext(), &prng);
    double roll = impl.getSamplingValue();
    ASSERT_GTE(roll, 0.0);
    ASSERT_LT(roll, 1.0);
}

TEST_F(SpanTelemetryContextImplTest, SamplingRollIsMemoized) {
    PseudoRandom prng(int64_t{1});
    SpanTelemetryContextImpl impl(getSpanContext(), &prng);
    double first = impl.getSamplingValue();

    // A second call must return the value drawn on the first call. This is the "one roll per
    // telemetry context" invariant.
    double second = impl.getSamplingValue();
    ASSERT_EQ(first, second);
}

TEST_F(SpanTelemetryContextImplTest, HasActiveTraceReturnsFalseWhenNoSpanIsSet) {
    SpanTelemetryContextImpl impl(getSpanContext());
    ASSERT_FALSE(impl.hasActiveTrace());
}

TEST_F(SpanTelemetryContextImplTest, HasActiveTraceReturnsTrueWhenSpanIsSet) {
    SpanTelemetryContextImpl impl(getSpanContext());
    impl.setSpan(makeValidSpan());
    ASSERT_TRUE(impl.hasActiveTrace());
}

}  // namespace
}  // namespace traces
}  // namespace otel
}  // namespace mongo

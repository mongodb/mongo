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

#include "mongo/otel/traces/tracing_utils.h"
#include "mongo/unittest/unittest.h"

#include <opentelemetry/baggage/baggage_context.h>

namespace mongo {
namespace otel {
namespace traces {
namespace {

class SpanTelemetryContextImplTest : public unittest::Test {
public:
    OtelContext getSpanContext() {
        return OtelContext();
    }
};

TEST_F(SpanTelemetryContextImplTest, KeepSpanTrue) {
    auto otelCtx = getSpanContext();
    SpanTelemetryContextImpl impl(std::move(otelCtx));
    impl.keepSpan(true);
    ASSERT_TRUE(impl.shouldKeepSpan());
}

TEST_F(SpanTelemetryContextImplTest, KeepSpanFalseByDefault) {
    auto otelCtx = getSpanContext();
    SpanTelemetryContextImpl impl(std::move(otelCtx));
    ASSERT_FALSE(impl.shouldKeepSpan());
}

TEST_F(SpanTelemetryContextImplTest, KeepSpanTrueIfAlreadyTrue) {
    auto otelCtx = getSpanContext();
    auto baggage = opentelemetry::baggage::GetBaggage(otelCtx);
    baggage = baggage->Set(keepSpanKey, trueValue);
    otelCtx = OtelContext(opentelemetry::baggage::SetBaggage(otelCtx, baggage));
    SpanTelemetryContextImpl impl(std::move(otelCtx));
    ASSERT_TRUE(impl.shouldKeepSpan());
}

TEST_F(SpanTelemetryContextImplTest, KeepSpanValueIsFalseAfterSettingToTrue) {
    auto otelCtx = getSpanContext();
    SpanTelemetryContextImpl impl(std::move(otelCtx));
    ASSERT_FALSE(impl.shouldKeepSpan());
    impl.keepSpan(true);
    ASSERT_TRUE(impl.shouldKeepSpan());

    impl.keepSpan(false);
    ASSERT_FALSE(impl.shouldKeepSpan());
}

}  // namespace
}  // namespace traces
}  // namespace otel
}  // namespace mongo

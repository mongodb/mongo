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

#include <opentelemetry/baggage/baggage_context.h>
#include <opentelemetry/trace/span_context.h>

namespace mongo {
namespace otel {
namespace traces {

void SpanTelemetryContextImpl::keepSpan(bool keepSpan) {
    auto baggage = opentelemetry::baggage::GetBaggage(_ctx);

    // Baggage is not a set and we do not want duplicate keys, so we must remove the existing key if
    // it exists.
    baggage = baggage->Delete(keepSpanKey);

    auto value = keepSpan ? trueValue : falseValue;
    baggage = baggage->Set(keepSpanKey, value);
    _ctx = opentelemetry::baggage::SetBaggage(_ctx, baggage);
}

bool SpanTelemetryContextImpl::shouldKeepSpan() const {
    auto baggage = opentelemetry::baggage::GetBaggage(_ctx);
    std::string value;
    auto exists = baggage->GetValue(keepSpanKey, value);
    return exists && (value == trueValue);
}

}  // namespace traces
}  // namespace otel
}  // namespace mongo

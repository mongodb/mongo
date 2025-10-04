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

#include "mongo/otel/telemetry_context_serialization.h"

#include "mongo/otel/traces/span/span_telemetry_context_impl.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace otel {
namespace {

using DefaultSpan = opentelemetry::trace::DefaultSpan;

class TelemetryContextSerializationTest : public unittest::Test {};

TEST_F(TelemetryContextSerializationTest, NoOpSerializeToBSON) {
    std::shared_ptr<TelemetryContext> context = std::make_shared<TelemetryContext>();
    BSONObj bson = TelemetryContextSerializer::toBSON(context);
    ASSERT_BSONOBJ_EQ(bson, BSONObj());
}

TEST_F(TelemetryContextSerializationTest, NoOpSerializeFromBSON) {
    BSONObj bson = BSON("key" << "value");
    std::shared_ptr<TelemetryContext> context = TelemetryContextSerializer::fromBSON(bson);
    ASSERT_EQ(context->type(), "SpanTelemetryContextImpl");

    auto spanContext = std::dynamic_pointer_cast<traces::SpanTelemetryContextImpl>(context);
    ASSERT_EQ(spanContext->shouldKeepSpan(), false);

    auto defaultSpan = std::dynamic_pointer_cast<DefaultSpan>(spanContext->getSpan());

    ASSERT_NOT_EQUALS(defaultSpan, nullptr);
}

}  // namespace
}  // namespace otel
}  // namespace mongo

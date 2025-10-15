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

#include "mongo/otel/telemetry_context_metadata_hook.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/unittest/unittest.h"

#ifdef MONGO_CONFIG_OTEL
#include "mongo/otel/telemetry_context_serialization.h"
#include "mongo/otel/traces/otel_test_fixture.h"
#include "mongo/otel/traces/span/span.h"
#endif

namespace mongo {
namespace otel {
namespace {

class TelemetryContextMetadataHookTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _hook = std::make_unique<TelemetryContextMetadataHook>(getServiceContext());
    }

protected:
    std::unique_ptr<TelemetryContextMetadataHook> _hook;
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagTracing", true};
};

TEST_F(TelemetryContextMetadataHookTest, WriteRequestMetadataWithoutTelemetryContext) {
    auto opCtx = makeOperationContext();
    BSONObjBuilder bob;

    ASSERT_OK(_hook->writeRequestMetadata(opCtx.get(), &bob));

    auto obj = bob.obj();
    ASSERT_FALSE(obj.hasField("$traceCtx"));
}

TEST_F(TelemetryContextMetadataHookTest, ReadReplyMetadataEmpty) {
    auto opCtx = makeOperationContext();

    ASSERT_OK(_hook->readReplyMetadata(opCtx.get(), BSONObj()));
}

#ifdef MONGO_CONFIG_OTEL
class TelemetryContextMetadataHookWithSpanTest : public traces::OtelTestFixture {
public:
    void setUp() override {
        traces::OtelTestFixture::setUp();
        _hook = std::make_unique<TelemetryContextMetadataHook>(getServiceContext());
    }

protected:
    std::unique_ptr<TelemetryContextMetadataHook> _hook;
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagTracing", true};
};

TEST_F(TelemetryContextMetadataHookWithSpanTest, WriteRequestMetadataWithRealSpan) {
    auto opCtx = makeOperationContext();

    auto span = traces::Span::start(opCtx.get(), "testSpan");
    auto& holder = TelemetryContextHolder::get(opCtx.get());
    ASSERT_NE(holder.get(), nullptr);

    BSONObjBuilder bob;
    ASSERT_OK(_hook->writeRequestMetadata(opCtx.get(), &bob));

    auto obj = bob.obj();
    ASSERT_TRUE(obj.hasField("$traceCtx"));
    ASSERT_EQ(obj["$traceCtx"].type(), BSONType::object);

    // TODO SERVER-100120: Uncomment the assertion below
    // BSONObj traceCtx = obj["$traceCtx"].Obj();
    // ASSERT_TRUE(traceCtx.hasField("traceparent"));
}
#endif  // MONGO_CONFIG_OTEL

}  // namespace
}  // namespace otel
}  // namespace mongo

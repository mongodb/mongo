/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/otel/traces/trace_initialization.h"

#include "mongo/config.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/otel/traces/trace_settings_gen.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/unittest/unittest.h"

#include <opentelemetry/trace/noop.h>
#include <opentelemetry/trace/provider.h>

namespace mongo {
namespace {

class TraceInitializationTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        // Initialize TracerProviderService with no-op provider
        auto tracerProviderService = otel::traces::TracerProviderService::create();
        tracerProviderService->setTracerProvider_ForTest(
            std::make_shared<opentelemetry::trace::NoopTracerProvider>());
        otel::traces::TracerProviderService::set(getServiceContext(),
                                                 std::move(tracerProviderService));

        otel::traces::gOpenTelemetryHttpEndpoint.clear();
        otel::traces::gOpenTelemetryTraceDirectory.clear();
    }

    void tearDown() override {
        // Clear the TracerProviderService
        otel::traces::TracerProviderService::set(getServiceContext(), nullptr);
        ServiceContextTest::tearDown();
    }
};

bool isNoop(opentelemetry::trace::TracerProvider* provider) {
    return !!dynamic_cast<opentelemetry::trace::NoopTracerProvider*>(provider);
}

TEST_F(TraceInitializationTest, NoTraceProvider) {
    ASSERT_OK(otel::traces::initialize(getServiceContext(), "mongod"));

    auto tracerProviderService = otel::traces::TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    ASSERT_FALSE(tracerProviderService->isEnabled());
}

TEST_F(TraceInitializationTest, Shutdown) {
    otel::traces::shutdown(getServiceContext());

    auto tracerProviderService = otel::traces::TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    ASSERT_FALSE(tracerProviderService->isEnabled());

    otel::traces::gOpenTelemetryTraceDirectory = "/tmp/";
    otel::traces::shutdown(getServiceContext());

    // After calling shutdown, the service should still exist but be disabled
    tracerProviderService = otel::traces::TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    ASSERT_FALSE(tracerProviderService->isEnabled());
}

TEST_F(TraceInitializationTest, FileTraceProvider) {
    otel::traces::gOpenTelemetryTraceDirectory = "/tmp/";
    ASSERT_OK(otel::traces::initialize(getServiceContext(), "mongod"));

    auto tracerProviderService = otel::traces::TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    ASSERT_TRUE(tracerProviderService->isEnabled());
    ASSERT_FALSE(isNoop(tracerProviderService->getTracerProvider().get()));
}

TEST_F(TraceInitializationTest, HttpTraceProvider) {
    otel::traces::gOpenTelemetryHttpEndpoint = "http://localhost:4318/v1/traces";
    ASSERT_OK(otel::traces::initialize(getServiceContext(), "mongod"));

    auto tracerProviderService = otel::traces::TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    ASSERT_TRUE(tracerProviderService->isEnabled());
    ASSERT_FALSE(isNoop(tracerProviderService->getTracerProvider().get()));
}

TEST_F(TraceInitializationTest, HttpAndDirectory) {
    otel::traces::gOpenTelemetryHttpEndpoint = "http://localhost:4318/v1/traces";
    otel::traces::gOpenTelemetryTraceDirectory = "/tmp/";
    ASSERT_THROWS_CODE(otel::traces::initialize(getServiceContext(), "mongod"),
                       DBException,
                       ErrorCodes::InvalidOptions);

    auto tracerProviderService = otel::traces::TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    ASSERT_TRUE(tracerProviderService->isEnabled());
    ASSERT_TRUE(isNoop(tracerProviderService->getTracerProvider().get()));
}

}  // namespace
}  // namespace mongo

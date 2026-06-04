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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/unittest/unittest.h"

#include <gmock/gmock.h>
#include <opentelemetry/trace/noop.h>
#include <opentelemetry/trace/provider.h>

namespace mongo::otel::traces {
namespace {

constexpr auto kServiceName = "mongod";

class TraceInitializationTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        // Initialize TracerProviderService with no-op provider
        auto tracerProviderService = TracerProviderService::create();
        tracerProviderService->setTracerProvider_ForTest(
            std::make_shared<opentelemetry::trace::NoopTracerProvider>());
        TracerProviderService::set(getServiceContext(), std::move(tracerProviderService));
    }

    void tearDown() override {
        TracerProviderService::set(getServiceContext(), nullptr);
        ServiceContextTest::tearDown();
    }
};

bool isNoop(opentelemetry::trace::TracerProvider* provider) {
    return !!dynamic_cast<opentelemetry::trace::NoopTracerProvider*>(provider);
}

TEST_F(TraceInitializationTest, NoTraceProvider) {
    ASSERT_OK(otel::traces::initialize(getServiceContext(), kServiceName));

    auto tracerProviderService = TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    EXPECT_FALSE(tracerProviderService->isEnabled());
}

TEST_F(TraceInitializationTest, Shutdown) {
    shutdown(getServiceContext());

    auto tracerProviderService = TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    EXPECT_FALSE(tracerProviderService->isEnabled());

    RAIIServerParameterControllerForTest directoryParam{"opentelemetryTraceDirectory", "/tmp/"};
    shutdown(getServiceContext());

    tracerProviderService = TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    EXPECT_FALSE(tracerProviderService->isEnabled());
}

TEST_F(TraceInitializationTest, FileTraceProvider) {
    RAIIServerParameterControllerForTest directoryParam{"opentelemetryTraceDirectory", "/tmp/"};
    ASSERT_OK(initialize(getServiceContext(), kServiceName));

    auto tracerProviderService = TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    EXPECT_TRUE(tracerProviderService->isEnabled());
    EXPECT_FALSE(isNoop(tracerProviderService->getTracerProvider().get()));
}

TEST_F(TraceInitializationTest, HttpTraceProvider) {
    RAIIServerParameterControllerForTest endpointParam{"opentelemetryHttpEndpoint",
                                                       "http://localhost:4318/v1/traces"};
    ASSERT_OK(initialize(getServiceContext(), kServiceName));

    auto tracerProviderService = TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    EXPECT_TRUE(tracerProviderService->isEnabled());
    EXPECT_FALSE(isNoop(tracerProviderService->getTracerProvider().get()));
}

TEST_F(TraceInitializationTest, HttpAndDirectorySetSimultaneouslyFails) {
    RAIIServerParameterControllerForTest endpointParam{"opentelemetryHttpEndpoint",
                                                       "http://localhost:4318/v1/traces"};
    RAIIServerParameterControllerForTest directoryParam{"opentelemetryTraceDirectory", "/tmp/"};
    ASSERT_THROWS_CODE(
        initialize(getServiceContext(), kServiceName), DBException, ErrorCodes::InvalidOptions);

    auto tracerProviderService = TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    EXPECT_TRUE(tracerProviderService->isEnabled());
    EXPECT_TRUE(isNoop(tracerProviderService->getTracerProvider().get()));
}

TEST_F(TraceInitializationTest, InvalidCompressionFails) {
    RAIIServerParameterControllerForTest compressionParam{"openTelemetryTracingCompression",
                                                          "zstd"};
    ASSERT_THROWS_CODE(
        initialize(getServiceContext(), kServiceName), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(TraceInitializationTest, GzipCompressionWithoutHttpEndpointFails) {
    RAIIServerParameterControllerForTest compressionParam{"openTelemetryTracingCompression",
                                                          "gzip"};
    ASSERT_THROWS_CODE(
        initialize(getServiceContext(), kServiceName), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(TraceInitializationTest, GzipCompressionWithHttpEndpointSucceeds) {
    RAIIServerParameterControllerForTest compressionParam{"openTelemetryTracingCompression",
                                                          "gzip"};
    RAIIServerParameterControllerForTest endpointParam{"opentelemetryHttpEndpoint",
                                                       "http://localhost:4318/v1/traces"};
    ASSERT_OK(initialize(getServiceContext(), kServiceName));
}

TEST_F(TraceInitializationTest, MaxBatchSizeExceedsMaxQueueSizeFails) {
    RAIIServerParameterControllerForTest batchSizeParam{"openTelemetryTracingMaxBatchSize", 5000};
    RAIIServerParameterControllerForTest queueSizeParam{"openTelemetryTracingMaxQueueSize", 100};
    ASSERT_THROWS_CODE(
        initialize(getServiceContext(), kServiceName), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(TraceInitializationTest, MaxBatchSizeEqualToMaxQueueSizeSucceeds) {
    RAIIServerParameterControllerForTest batchSizeParam{"openTelemetryTracingMaxBatchSize", 512};
    RAIIServerParameterControllerForTest queueSizeParam{"openTelemetryTracingMaxQueueSize", 512};
    ASSERT_OK(initialize(getServiceContext(), kServiceName));
}

}  // namespace
}  // namespace mongo::otel::traces

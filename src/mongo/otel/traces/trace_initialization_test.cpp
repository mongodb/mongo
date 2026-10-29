// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/trace_initialization.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/traces/trace_settings.h"
#include "mongo/otel/traces/trace_settings_gen.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/otel/traces/tracer_provider_service_factory.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <gmock/gmock.h>
#include <opentelemetry/trace/noop.h>
#include <opentelemetry/trace/provider.h>

namespace mongo::otel::traces {
namespace {

constexpr auto kServiceName = "mongod";

class TraceInitializationTest : public unittest::Test {
public:
    void setUp() override {
        // Initialize TracerProviderService with no-op provider
        auto tracerProviderService = createNoOpTracerProviderService();
        tracerProviderService->setTracerProvider_ForTest(
            std::make_unique<opentelemetry::trace::NoopTracerProvider>());
        setGlobalTracerProviderService(std::move(tracerProviderService));
    }

    void tearDown() override {
        setGlobalTracerProviderService(nullptr);
    }
};

bool isNoop(opentelemetry::trace::TracerProvider* provider) {
    return !!dynamic_cast<opentelemetry::trace::NoopTracerProvider*>(provider);
}

TEST_F(TraceInitializationTest, NoTraceProvider) {
    ASSERT_OK(otel::traces::initialize(kServiceName));

    auto tracerProviderService = getGlobalTracerProviderService();
    ASSERT_TRUE(tracerProviderService);
    EXPECT_FALSE(tracerProviderService->getTracerProvider());
}

TEST_F(TraceInitializationTest, Shutdown) {
    shutdown();

    auto tracerProviderService = getGlobalTracerProviderService();
    ASSERT_TRUE(tracerProviderService);
    EXPECT_FALSE(tracerProviderService->getTracerProvider());

    unittest::ServerParameterGuard directoryParam{"opentelemetryTraceDirectory", "/tmp/"};
    shutdown();

    tracerProviderService = getGlobalTracerProviderService();
    ASSERT_TRUE(tracerProviderService);
    EXPECT_FALSE(tracerProviderService->getTracerProvider());
}

TEST_F(TraceInitializationTest, FileTraceProvider) {
    unittest::ServerParameterGuard directoryParam{"opentelemetryTraceDirectory", "/tmp/"};
    ASSERT_OK(initialize(kServiceName));

    auto tracerProviderService = getGlobalTracerProviderService();
    ASSERT_TRUE(tracerProviderService);
    auto* provider = tracerProviderService->getTracerProvider();
    ASSERT_NE(provider, nullptr);
    EXPECT_FALSE(isNoop(provider));
}

TEST_F(TraceInitializationTest, HttpTraceProvider) {
    unittest::ServerParameterGuard endpointParam{"opentelemetryHttpEndpoint",
                                                 "http://localhost:4318/v1/traces"};
    ASSERT_OK(initialize(kServiceName));

    auto tracerProviderService = getGlobalTracerProviderService();
    ASSERT_TRUE(tracerProviderService);
    auto* provider = tracerProviderService->getTracerProvider();
    ASSERT_NE(provider, nullptr);
    EXPECT_FALSE(isNoop(provider));
}

TEST_F(TraceInitializationTest, HttpAndDirectorySetSimultaneouslyFails) {
    unittest::ServerParameterGuard endpointParam{"opentelemetryHttpEndpoint",
                                                 "http://localhost:4318/v1/traces"};
    unittest::ServerParameterGuard directoryParam{"opentelemetryTraceDirectory", "/tmp/"};
    ASSERT_THROWS_CODE(initialize(kServiceName), DBException, ErrorCodes::InvalidOptions);

    auto tracerProviderService = getGlobalTracerProviderService();
    ASSERT_TRUE(tracerProviderService);
    auto* provider = tracerProviderService->getTracerProvider();
    ASSERT_NE(provider, nullptr);
    EXPECT_TRUE(isNoop(provider));
}

TEST_F(TraceInitializationTest, InvalidCompressionFails) {
    unittest::ServerParameterGuard compressionParam{"openTelemetryTracingCompression", "zstd"};
    ASSERT_THROWS_CODE(initialize(kServiceName), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(TraceInitializationTest, GzipCompressionWithoutHttpEndpointFails) {
    unittest::ServerParameterGuard compressionParam{"openTelemetryTracingCompression", "gzip"};
    ASSERT_THROWS_CODE(initialize(kServiceName), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(TraceInitializationTest, GzipCompressionWithHttpEndpointSucceeds) {
    unittest::ServerParameterGuard compressionParam{"openTelemetryTracingCompression", "gzip"};
    unittest::ServerParameterGuard endpointParam{"opentelemetryHttpEndpoint",
                                                 "http://localhost:4318/v1/traces"};
    ASSERT_OK(initialize(kServiceName));
}

TEST_F(TraceInitializationTest, MaxBatchSizeExceedsMaxQueueSizeFails) {
    unittest::ServerParameterGuard batchSizeParam{"openTelemetryTracingMaxBatchSize", 5000};
    unittest::ServerParameterGuard queueSizeParam{"openTelemetryTracingMaxQueueSize", 100};
    ASSERT_THROWS_CODE(initialize(kServiceName), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(TraceInitializationTest, MaxBatchSizeEqualToMaxQueueSizeSucceeds) {
    unittest::ServerParameterGuard batchSizeParam{"openTelemetryTracingMaxBatchSize", 512};
    unittest::ServerParameterGuard queueSizeParam{"openTelemetryTracingMaxQueueSize", 512};
    ASSERT_OK(initialize(kServiceName));
}

TEST_F(TraceInitializationTest, HeadersWithoutHttpExporterLogsWarning) {
    // Configure HTTP export headers while only the file exporter is enabled. The headers cannot be
    // sent without the HTTP exporter, so initialization must log a warning.
    unittest::ServerParameterGuard directoryParam{"opentelemetryTraceDirectory", "/tmp/"};

    OpenTelemetryTracingHttpExportHeaders headersParam{"openTelemetryTracingHttpExportHeaders",
                                                       ServerParameterType::kStartupOnly};
    auto setHeaders = [&](BSONObj doc) {
        auto storage = BSON("v" << doc);
        return headersParam.set(storage.firstElement(), /*tenantId=*/boost::none);
    };
    ASSERT_OK(setHeaders(BSON("Authorization" << "Bearer ignored-token")));
    ON_BLOCK_EXIT([&] { invariant(setHeaders(BSONObj{})); });

    unittest::LogCaptureGuard logs;
    ASSERT_OK(initialize(kServiceName));
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 12877900)), 1);
}

}  // namespace
}  // namespace mongo::otel::traces

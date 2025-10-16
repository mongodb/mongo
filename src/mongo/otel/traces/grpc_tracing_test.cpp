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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/unittest/unittest.h"

#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>

namespace mongo {
namespace otel {
namespace traces {
namespace {

class GRPCTracingTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        // Initialize TracerProviderService with a basic provider
        auto tracerProviderService = TracerProviderService::create();
        tracerProviderService->setTracerProvider_ForTest(
            opentelemetry::trace::Provider::GetTracerProvider());
        TracerProviderService::set(getServiceContext(), std::move(tracerProviderService));
    }

    void tearDown() override {
        TracerProviderService::set(getServiceContext(), nullptr);
        ServiceContextTest::tearDown();
    }
};

TEST_F(GRPCTracingTest, GetTracer) {
    auto tracerProviderService = TracerProviderService::get(getServiceContext());
    ASSERT_TRUE(tracerProviderService);
    auto tracer = tracerProviderService->getTracerProvider()->GetTracer("grpc");
    ASSERT(tracer);
    auto span = tracer->GetCurrentSpan();
    ASSERT_FALSE(span->IsRecording());
}

TEST_F(GRPCTracingTest, CreateExporterAndSpanProcesser) {
    opentelemetry::exporter::otlp::OtlpGrpcExporterOptions opts;
    opts.endpoint = "localhost:12345";
    auto exporter = opentelemetry::exporter::otlp::OtlpGrpcExporterFactory::Create(opts);
    ASSERT(exporter);

    opentelemetry::sdk::trace::BatchSpanProcessorOptions bspOpts{};
    auto processor =
        opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(std::move(exporter), bspOpts);
    ASSERT(processor);
}

}  // namespace
}  // namespace traces
}  // namespace otel
}  // namespace mongo

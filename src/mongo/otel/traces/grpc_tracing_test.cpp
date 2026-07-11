// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/otel/traces/tracer_provider_service_factory.h"
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

class GRPCTracingTest : public unittest::Test {
public:
    void setUp() override {
        // Initialize TracerProviderService with a basic provider
        auto tracerProviderService = createNoOpTracerProviderService();
        tracerProviderService->setTracerProvider_ForTest(
            opentelemetry::trace::Provider::GetTracerProvider());
        setGlobalTracerProviderService(std::move(tracerProviderService));
    }

    void tearDown() override {
        setGlobalTracerProviderService(nullptr);
    }
};

TEST_F(GRPCTracingTest, GetTracer) {
    auto tracerProviderService = getGlobalTracerProviderService();
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
